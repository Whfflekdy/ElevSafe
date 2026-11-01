[CmdletBinding()]
param([Parameter(Mandatory)][string]$BackendExe, [Parameter(Mandatory)][string]$SignalConfig)
$ErrorActionPreference = 'Stop'
$BackendExe = (Resolve-Path -LiteralPath $BackendExe).Path
$SignalConfig = (Resolve-Path -LiteralPath $SignalConfig).Path
$outputDirectory = Join-Path (Split-Path $BackendExe) ('cli-radar-fixture/' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $outputDirectory

function Make-Packet([uint32]$frameId) {
    # Same little-endian layout as retina_protocol_test.cpp::makePacket:
    # network header, frame header, float32 point fields, then int32 target IDs.
    $bytes = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($bytes)
    try {
        foreach ($word in @([uint32]0, [uint32]2882356001, [uint32]0, [uint32]0,
            [uint32]64, [uint32]0, [uint32]0, [uint32]0, [uint32]0)) { $writer.Write($word) }
        $writer.Write([uint64]578437695752307201)
        $writer.Write($frameId)
        $writer.Write([uint32]2)
        foreach ($value in @([single]0, [single]0, [single]1, [single]0.25, [single]10,
            [single]0.1, [single]0, [single]1, [single]-0.25, [single]11)) { $writer.Write($value) }
        $writer.Write([int32]7)
        $writer.Write([int32]-1)
        $writer.Flush()
        return ,$bytes.ToArray()
    } finally { $writer.Dispose(); $bytes.Dispose() }
}

foreach ($mode in @('radar', 'radar-websocket-failed')) {
    $listener = $null; $peer = $null; $process = $null; $stage = 'fixture.open'
    $stdout = Join-Path $outputDirectory ($mode + '-stdout.log')
    $stderr = Join-Path $outputDirectory ($mode + '-stderr.log')
    try {
        $listener = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork,
            [Net.Sockets.SocketType]::Stream, [Net.Sockets.ProtocolType]::Tcp)
        $stage = 'fixture.bind'
        $listener.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 0))
        $port = $listener.LocalEndPoint.Port
        $stage = 'fixture.listen'
        $listener.Listen(8)
        $stage = 'fixture.accept'
        $accept = $listener.BeginAccept($null, $null)
        $arguments = @('127.0.0.1', [string]$port, '--signal-config', ('"' + $SignalConfig + '"'))
        if ($mode -eq 'radar-websocket-failed') { $arguments += @('--websocket', '--ws-bind', 'invalid-ip') }
        $stage = 'backend.start'
        $process = Start-Process -FilePath $BackendExe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $process.Handle
        Write-Host "START mode=$mode PID=$($process.Id) port=$port time=$([DateTimeOffset]::Now.ToString('o'))"
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        $stage = 'fixture.accept'
        while (-not $accept.IsCompleted -and -not $process.HasExited -and $deadline.Elapsed.TotalSeconds -lt 6) {
            Start-Sleep -Milliseconds 10
        }
        if (-not $accept.IsCompleted) { throw 'Backend did not connect to the controlled TCP fixture' }
        $peer = $listener.EndAccept($accept)
        $peer.SendTimeout = 1000
        $stage = 'fixture.send'
        foreach ($frameId in @([uint32]41, [uint32]42)) {
            $packet = Make-Packet $frameId
            $offset = 0
            while ($offset -lt $packet.Length) {
                $sent = $peer.Send($packet, $offset, $packet.Length - $offset, [Net.Sockets.SocketFlags]::None)
                if ($sent -le 0) { throw 'TCP send did not progress' }
                $offset += $sent
            }
        }
        $peer.Shutdown([Net.Sockets.SocketShutdown]::Send)
        $stage = 'backend.exit'
        if (-not $process.WaitForExit(3000)) { throw 'Backend did not exit after fixture EOF' }
        $text = [IO.File]::ReadAllText($stdout) + "`n" + [IO.File]::ReadAllText($stderr)
        if ($text -match '\bcode=10014\b|\[system:10014\b|\(10014\)') { throw 'Winsock 10014; not retried' }
        if ($process.ExitCode -ne 0) { throw "Backend exit code $($process.ExitCode)" }
        foreach ($pattern in @('SignalPipeline ready:', 'Connected to 127.0.0.1:', 'Remote endpoint closed',
            'signal frame_id=41 timestamp_us=0 inputPoints=2 clusters=1',
            'signal frame_id=42 timestamp_us=\d+ inputPoints=2 clusters=1',
            'frameCount=41 timestampUs=0 pointCount=2', 'frameCount=42 timestampUs=\d+ pointCount=2')) {
            if ($text -notmatch $pattern) { throw "Missing evidence: $pattern" }
        }
        if ([regex]::Matches($text, '(?m)^signal frame_id=').Count -ne 2 -or
            [regex]::Matches($text, '(?m)^frameCount=').Count -ne 2) { throw 'Unexpected frame/B result count' }
        if ($mode -eq 'radar-websocket-failed') {
            $errorText = [IO.File]::ReadAllText($stderr)
            $ordered = @('SignalPipeline ready:', '[ws] stop.complete', '[ws] startup failed:',
                '[ws] disabled for this run; continuing radar processing', 'Connecting to 127.0.0.1:')
            $previous = -1
            foreach ($marker in $ordered) {
                $position = $errorText.IndexOf($marker, [StringComparison]::Ordinal)
                if ($position -le $previous) { throw "Missing/out-of-order cleanup/fallback: $marker" }
                $previous = $position
            }
            if ($text -match '\[ws\] listening|transport only') { throw 'Failed WebSocket was marked active' }
        } elseif ($text -match '\[ws\]') { throw 'Radar-only mode started WebSocket' }
        Write-Host "PASS mode=$mode frames=2 B_results=2 clusters=2 EOF=true exit=0"
    } catch {
        $failure = $_.Exception
        while ($failure.InnerException) { $failure = $failure.InnerException }
        Write-Error -ErrorAction Continue "FAIL mode=$mode stage=$stage message=$($failure.Message) logs=$outputDirectory"
        if ($failure -is [Net.Sockets.SocketException]) {
            Write-Error -ErrorAction Continue "Native socket code=$($failure.NativeErrorCode) API stage=$stage"
        }
        foreach ($path in @($stdout, $stderr)) { if (Test-Path $path) { Write-Host ([IO.File]::ReadAllText($path)) } }
        throw
    } finally {
        if ($peer) { $peer.Dispose() }
        if ($listener) { $listener.Dispose() }
        if ($process -and -not $process.HasExited) {
            # Only this fixture's bounded child; failure has already been recorded.
            Write-Warning "Terminating timed-out fixture child PID=$($process.Id)"
            $process.Kill()
            $null = $process.WaitForExit(1000)
        }
        if ($process) { $process.Dispose() }
    }
}
Write-Host "Radar startup isolation passed; logs=$outputDirectory"
