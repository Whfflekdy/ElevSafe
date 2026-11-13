[CmdletBinding()]
param(
    [ValidateSet('Server', 'Mock')][string]$Mode = 'Server',
    [ValidateRange(1, 10000)][int]$Count = 20,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 45,
    [ValidateRange(0, 60000)][int]$DelayMilliseconds = 200,
    [ValidateRange(1, 65535)][int]$Port = 8080,
    [ValidateRange(1, 86400)][int]$MockDurationSeconds = 1,
    [ValidateSet('', 'failed-start-cleanup', 'handshake-snapshots-commands',
        'queue-bounds-slow-client', 'oversized-pending-handshake')][string]$Case = '',
    [string]$OutputDirectory = '',
    [string]$RepositoryRoot = '',
    [string]$BuildDirectory = 'backend/build-websocket-integration'
)

$ErrorActionPreference = 'Stop'
$root = if ($RepositoryRoot) { (Resolve-Path -LiteralPath $RepositoryRoot).Path }
    else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory }
    else { Join-Path $root $BuildDirectory }
$name = if ($Mode -eq 'Server') { 'websocket_server_test.exe' } else { 'elevsafe_backend.exe' }
$exe = (Resolve-Path (Join-Path $build "Release/$name")).Path
if ($Mode -eq 'Mock' -and $Case) { throw '-Case is only valid in Server mode' }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $build ('diagnostics/' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '-' + $Mode)
}
if ((Test-Path -LiteralPath $OutputDirectory) -and
    @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -ne 0) {
    throw 'OutputDirectory must be new or empty; existing logs will not be overwritten'
}
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$journal = Join-Path $OutputDirectory 'runs.jsonl'
$csv = Join-Path $OutputDirectory 'results.csv'
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$results = [Collections.Generic.List[object]]::new()

function Write-Record($record) {
    [pscustomobject]$record | ConvertTo-Json -Compress | Add-Content -LiteralPath $journal -Encoding UTF8
    [pscustomobject]$record | Export-Csv -LiteralPath $csv -NoTypeInformation -Append -Encoding UTF8
}

function Read-Log($path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    try { return [string](Get-Content -LiteralPath $path -Raw -Encoding UTF8) }
    catch { return "Log read not yet available: $($_.Exception.Message)" }
}

function Update-Diagnostics($record) {
    $text = (Read-Log $record.Stderr) + "`n" + (Read-Log $record.Stdout)
    $markers = [regex]::Matches($text, '(?m)^(?:PROGRESS|PENDING|TIMEOUT|ERROR|TEST_BEGIN|TEST_END) subtest=(\S+) stage=(\S+).*')
    if ($markers.Count) {
        $last = $markers[$markers.Count - 1]
        $record.Subtest = $last.Groups[1].Value
        $record.LastStage = $last.Groups[2].Value
    }
    $failure = [regex]::Match($text, '(?m)^(?:TIMEOUT|ERROR) subtest=(\S+) stage=(\S+)')
    if ($failure.Success) {
        $record.FailureSubtest = $failure.Groups[1].Value
        $record.FailureStage = $failure.Groups[2].Value
    }
    $progress = [regex]::Matches($text, '(?m)^(?:PROGRESS|PENDING|TIMEOUT|ERROR|TEST_BEGIN|TEST_END|CLEANUP|\[ws\] ).*')
    if ($progress.Count) { $record.LastProgress = $progress[$progress.Count - 1].Value }
    $workerWait = [regex]::Match($record.LastProgress, '^\[ws\] (stop\.\S+)')
    if ($workerWait.Success) { $record.LastStage = $workerWait.Groups[1].Value }
    if ($Mode -eq 'Mock' -and $record.LastStage -eq 'process.start') {
        if ($text -match '(?m)^\[ws\] MOCK ONLY:') { $record.LastStage = 'mock.run' }
        elseif ($text -match '(?m)^\[ws\] listening ') { $record.LastStage = 'server.listening' }
        elseif ($text -match '(?m)^\[ws\] startup/runtime failed:') { $record.LastStage = 'server.start' }
    }
    $primary = [regex]::Match($text, '(?m)^ERROR .*? code=(\d+)\b')
    if (-not $primary.Success -and $Mode -eq 'Mock') {
        $primary = [regex]::Match($text, '(?m)^\[ws\] startup/runtime failed:.*?\[system:(\d+)')
    }
    if ($primary.Success) { $record.NativeCode = [int]$primary.Groups[1].Value }
    return $text
}

function Save-TimeoutState($process, $record) {
    $state = [ordered]@{ Time = [DateTimeOffset]::Now.ToString('o'); PID = $record.PID }
    try {
        $process.Refresh()
        $state.HasExited = $process.HasExited
        if (-not $process.HasExited) {
            $state.StartTime = $process.StartTime.ToString('o')
            $state.CpuMs = $process.TotalProcessorTime.TotalMilliseconds
            $state.HandleCount = $process.HandleCount
            $state.Threads = @($process.Threads | ForEach-Object {
                $thread = $_
                try {
                    [pscustomobject]@{
                        Id = $thread.Id
                        State = [string]$thread.ThreadState
                        WaitReason = $(if ($thread.ThreadState -eq [Diagnostics.ThreadState]::Wait) {
                            [string]$thread.WaitReason
                        } else { '' })
                    }
                } catch { [pscustomobject]@{ Error = $_.Exception.Message } }
            })
        }
    } catch { $state.ProcessQueryError = $_.Exception.Message }
    try {
        $state.Tcp = @(Get-NetTCPConnection -OwningProcess $record.PID -ErrorAction Stop |
            Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort, State, OwningProcess)
    } catch { $state.TcpQueryError = $_.Exception.Message }
    $record.StateFile = Join-Path $OutputDirectory ('{0:D4}-state.json' -f $record.Round)
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $record.StateFile -Encoding UTF8
}

Write-Host "Executable: $exe SHA256=$hash"
Write-Host "Diagnostics: $OutputDirectory"
for ($round = 1; $round -le $Count; $round++) {
    $arguments = @('--diagnostics')
    if ($Mode -eq 'Server') {
        if ($Case) { $arguments += @('--case', $Case) }
    } else {
        $arguments = @('--ws-mock', '--ws-port', [string]$Port, '--ws-duration', [string]$MockDurationSeconds)
    }
    $record = [ordered]@{
        Mode = $Mode; Case = $Case; Round = $round; PID = $null
        Start = [DateTimeOffset]::Now.ToString('o'); End = ''; Event = 'Start'; Status = 'Running'
        ExitCode = $null; NativeCode = $null; Subtest = ''; FailureSubtest = ''; FailureStage = ''
        LastStage = 'process.start'; LastProgress = ''
        Stdout = (Join-Path $OutputDirectory ('{0:D4}-stdout.log' -f $round))
        Stderr = (Join-Path $OutputDirectory ('{0:D4}-stderr.log' -f $round))
        StateFile = ''; Executable = $exe; Arguments = ($arguments -join ' '); SHA256 = $hash
    }
    try {
        $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $build -PassThru `
            -WindowStyle Hidden -RedirectStandardOutput $record.Stdout -RedirectStandardError $record.Stderr
    } catch {
        $record.Event = 'StartFailed'; $record.Status = 'Other'; $record.End = [DateTimeOffset]::Now.ToString('o')
        $record.LastProgress = $_.Exception.Message; Write-Record $record
        throw
    }
    $record.PID = $process.Id
    Write-Record $record
    Write-Host "START round=$round PID=$($record.PID) time=$($record.Start)"
    # Cache the handle before waiting: Windows PowerShell 5.1 Start-Process can
    # otherwise return an object whose ExitCode remains null after process exit.
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $record.Event = 'Timeout'; $record.Status = 'Timeout'; $record.End = [DateTimeOffset]::Now.ToString('o')
        Write-Record $record
        try {
            $null = Update-Diagnostics $record
            Save-TimeoutState $process $record
        } catch { $record.LastProgress += " Diagnostic collection error: $($_.Exception.Message)" }
        $record.Event = 'TimeoutDiagnostics'
        Write-Record $record
        throw "Timeout PID=$($record.PID); logs/state preserved in $OutputDirectory. No process was killed; no next run."
    }
    $record.ExitCode = $process.ExitCode
    $record.Event = 'Finished'; $record.End = [DateTimeOffset]::Now.ToString('o')
    # Allow redirected file writers a bounded interval to finish, never an unbounded WaitForExit().
    $text = ''
    for ($poll = 0; $poll -lt 20; $poll++) {
        $text = Update-Diagnostics $record
        if (($Mode -eq 'Server' -and $text -match 'websocket_server_test passed|--- end server log ---') -or
            ($Mode -eq 'Mock' -and $text -match '\[ws\] stopped|startup/runtime failed:')) { break }
        Start-Sleep -Milliseconds 50
    }
    $record.Status = if ($null -eq $record.ExitCode) { 'OutputIncomplete' }
    elseif ($record.ExitCode -eq 0) {
        if (($Mode -eq 'Server' -and $text -notmatch 'websocket_server_test passed') -or
            ($Mode -eq 'Mock' -and $text -notmatch '\[ws\] stopped')) { 'OutputIncomplete' } else { 'Success' }
    } elseif ($text -match '(?m)^TIMEOUT ') { 'Timeout' }
    elseif ($record.NativeCode -eq 10014) { 'Winsock10014' }
    elseif ($record.NativeCode -eq 10048) { 'Winsock10048' }
    else { 'Other' }
    Write-Record $record
    $results.Add([pscustomobject]$record)
    Write-Host "END round=$round PID=$($record.PID) status=$($record.Status) failureStage=$($record.FailureStage) lastStage=$($record.LastStage)"
    $process.Dispose()
    Start-Sleep -Milliseconds $DelayMilliseconds
}
$results | Group-Object Status | Select-Object Count, Name
Write-Host "Results: $csv"
