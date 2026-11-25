[CmdletBinding()]
param([Parameter(Mandatory)][string]$BackendExe, [Parameter(Mandatory)][string]$SignalConfig,
    [string[]]$Cases = @('refused-limit', 'refused-restart', 'fin-default', 'partial-fin-restart', 'rst-websocket', 'processing-error', 'idle-recover', 'idle-limit'),
    [ValidateRange(1, 100)][int]$Repeat = 1,
    [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$BackendExe = (Resolve-Path -LiteralPath $BackendExe).Path
$SignalConfig = (Resolve-Path -LiteralPath $SignalConfig).Path
$helper = Join-Path (Split-Path (Split-Path $SignalConfig)) 'websocket_radar_fixture.ps1'
. ([scriptblock]::Create((Get-Content -LiteralPath $helper -Raw -Encoding UTF8))) `
    -BackendExe $BackendExe -SignalConfig $SignalConfig -FunctionsOnly
$outputDirectory = if ($OutputDirectory) { $OutputDirectory }
    else { Join-Path (Split-Path $BackendExe) ('radar-reconnect-fixture/' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $outputDirectory) { throw 'OutputDirectory must be new; existing logs will not be overwritten' }
$null = New-Item -ItemType Directory -Path $outputDirectory
$outputDirectory = (Resolve-Path -LiteralPath $outputDirectory).Path
$failures = @()

function Assert-That($condition, [string]$message) { if (-not $condition) { throw $message } }
function Read-Text($path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Wait-Text($path, [string]$pattern, $process) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 6) {
        if ((Read-Text $path) -match $pattern) { return }
        if ($process.HasExited) { throw "Child exited before $pattern" }
        Start-Sleep -Milliseconds 10
    }
    throw "Timeout waiting for $pattern"
}
function Bound-Socket([int]$port) {
    $socket = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork,
        [Net.Sockets.SocketType]::Stream, [Net.Sockets.ProtocolType]::Tcp)
    try {
        # Own the port exclusively; do not allow overlapping Windows listeners.
        $socket.ExclusiveAddressUse = $true
        $socket.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, $port))
        return $socket
    } catch { $socket.Dispose(); throw }
}
function Accept-Peer($listener, $process) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    # Readiness + nonblocking accept also bounds a restarted listener's accept path.
    $listener.Blocking = $false
    while (-not $process.HasExited -and $timer.Elapsed.TotalSeconds -lt 6) {
        if ($listener.Poll(10000, [Net.Sockets.SelectMode]::SelectRead)) { return $listener.Accept() }
    }
    Get-NetTCPConnection -LocalPort $listener.LocalEndPoint.Port -ErrorAction SilentlyContinue |
        Select-Object LocalAddress,LocalPort,RemoteAddress,RemotePort,State,OwningProcess | ConvertTo-Json | Write-Host
    throw "Backend did not connect: endpoint=$($listener.LocalEndPoint) handle=$($listener.Handle) PID=$($process.Id)"
}
function Send-Bytes($peer, [byte[]]$bytes) {
    $peer.SendTimeout = 1000
    $offset = 0
    while ($offset -lt $bytes.Length) {
        $sent = $peer.Send($bytes, $offset, $bytes.Length - $offset, [Net.Sockets.SocketFlags]::None)
        Assert-That ($sent -gt 0) 'Send did not progress'
        $offset += $sent
    }
}
function Send-Command($client, [string]$command) {
    $bytes = [Text.Encoding]::UTF8.GetBytes('{"door_command":"' + $command + '"}')
    # Existing manual JSON schema; no hardware action or ACK is expected.
    $task = $client.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text,
        $true, [Threading.CancellationToken]::None)
    Assert-That ($task.Wait(2000)) 'WebSocket command send timeout'
}

function Start-Server([int]$port, [byte[]]$payload, [string]$ending, [string]$tag) {
    $binary = Join-Path (Split-Path $BackendExe) 'retina_reconnect_test.exe'
    $data = Join-Path $outputDirectory ($tag + '.bin')
    $gate = Join-Path $outputDirectory ($tag + '.gate')
    $out = Join-Path $outputDirectory ($tag + '-server-stdout.log')
    $err = Join-Path $outputDirectory ($tag + '-server-stderr.log')
    [IO.File]::WriteAllBytes($data, $payload)
    $args = @('--server', [string]$port, ('"' + $data + '"'), ('"' + $gate + '"'), $ending)
    $child = Start-Process -FilePath $binary -ArgumentList $args -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $child.Handle
    try {
        Wait-Text $out 'READY port=\d+' $child
        $serverPort = [int][regex]::Match((Read-Text $out), 'READY port=(\d+)').Groups[1].Value
        Write-Host "SERVER tag=$tag PID=$($child.Id) port=$serverPort"
        return [pscustomobject]@{ Process=$child; Port=$serverPort; Gate=$gate; Stdout=$out; Stderr=$err }
    } catch {
        Write-Host (Read-Text $err)
        if (-not $child.HasExited) { $child.Kill(); $null = $child.WaitForExit(1000) }
        $child.Dispose()
        throw
    }
}
function Finish-Server($server) {
    Assert-That ($server.Process.WaitForExit(3000)) 'Fixture server failed to exit'
    Write-Host (Read-Text $server.Stderr)
    Assert-That ($server.Process.ExitCode -eq 0) 'Fixture server failed (native socket error recorded above)'
}

$caseNumber = 0
foreach ($mode in (@($Cases) * $Repeat)) {
    ++$caseNumber
    $tag = if ($Repeat -eq 1) { $mode } else { '{0:D3}-{1}' -f $caseNumber, $mode }
    $listener = $null; $peer = $null; $process = $null; $ws = $null; $server1 = $null; $server2 = $null; $stage = 'fixture.bind'
    $stdout = Join-Path $outputDirectory ($tag + '-stdout.log')
    $stderr = Join-Path $outputDirectory ($tag + '-stderr.log')
    try {
        if ($mode -in @('partial-fin-restart', 'rst-websocket')) {
            [byte[]]$payload = (Make-Packet 41) + (Make-Packet 42)
            if ($mode -eq 'partial-fin-restart') {
                $partial = Make-Packet 777
                $payload += [byte[]]$partial[0..9]
            }
            $ending = if ($mode -eq 'rst-websocket') { 'rst' } else { 'fin' }
            $server1 = Start-Server 0 $payload $ending ($tag + '-first')
            $port = $server1.Port
        } else {
            $listener = Bound-Socket 0
            $port = $listener.LocalEndPoint.Port
            if ($mode -notlike 'refused-*') {
                $stage = 'fixture.listen'
                Write-Host "LISTEN mode=$mode pid=$PID family=IPv4 address=127.0.0.1 requested_port=0 bound_port=$port backlog=8"
                $listener.Listen(8)
            }
        }
        $arguments = @('127.0.0.1', [string]$port, '--signal-config', ('"' + $SignalConfig + '"'), '--radar-reconnect')
        if ($mode -eq 'partial-fin-restart') { $arguments += '--radar-reconnect-eof' }
        if ($mode -like 'idle-*') { $arguments += @('--radar-idle-timeout-ms', '600') }
        if ($mode -eq 'rst-websocket') {
            $reservation = Bound-Socket 0; $wsPort = $reservation.LocalEndPoint.Port; $reservation.Dispose()
            $arguments += @('--websocket', '--ws-port', [string]$wsPort, '--ws-periodic-snapshot')
        } else { $arguments += @('--ab-profile', '--ab-profile-warmup', '1') }
        $stage = 'backend.start'
        $process = Start-Process -FilePath $BackendExe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $process.Handle
        Write-Host "START mode=$mode PID=$($process.Id) port=$port time=$([DateTimeOffset]::Now.ToString('o')) tag=$tag"
        if ($mode -like 'idle-*') {
            $sessions = if ($mode -eq 'idle-limit') { 4 } else { 2 }
            for ($attempt = 1; $attempt -le $sessions; ++$attempt) {
                $stage = "fixture.idle.accept-$attempt"
                $peer = Accept-Peer $listener $process
                if ($mode -eq 'idle-recover' -and $attempt -eq 2) {
                    $stage = 'fixture.idle.send-recovered'
                    Send-Bytes $peer (Make-Packet 7)
                    $peer.Shutdown([Net.Sockets.SocketShutdown]::Send)
                } else {
                    $stage = "fixture.idle.wait-$attempt"
                    if ($mode -eq 'idle-limit' -and $attempt -eq 1) {
                        $partial = Make-Packet 41
                        Send-Bytes $peer ([byte[]]$partial[0..9])
                    }
                    Wait-Text $stderr "session=$attempt end stage=idle_timeout code=0 .*last_valid_frame_age_ms=never" $process
                    $peer.Dispose(); $peer = $null
                }
            }
        } elseif ($mode -eq 'refused-limit') {
            # Bound, non-listening port: cannot be occupied by an unrelated test server.
        } else {
            if ($mode -eq 'refused-restart') {
                Wait-Text $stderr '\[radar\] retry=1/3' $process
                $stage = 'fixture.listen-after-refusal'; $listener.Listen(8)
            }
            if ($server1) {
                $stage = 'fixture.first-connection'; Wait-Text $stderr 'Connected to 127.0.0.1:' $process
                if ($mode -eq 'rst-websocket') {
                    $stage = 'websocket.connect'
                    $ws = [Net.WebSockets.ClientWebSocket]::new()
                    $cancel = [Threading.CancellationTokenSource]::new(3000)
                    try { $null = $ws.ConnectAsync([Uri]"ws://127.0.0.1:$wsPort/radar", $cancel.Token).GetAwaiter().GetResult() }
                    finally { $cancel.Dispose() }
                }
                [IO.File]::WriteAllText($server1.Gate, 'go')
                $stage = 'fixture.first-server-exit'; Finish-Server $server1
                Wait-Text $stderr '\[radar\] retry=1/3' $process
                if ($ws) {
                    $stage = 'websocket.during-retry'; Send-Command $ws 'OPEN'
                    Wait-Text $stderr 'command=OPEN' $process
                }
                # A new process starts only after the old server has completely exited.
                $stage = 'fixture.restart'; $server2 = Start-Server $port (Make-Packet 7) 'fin' ($tag + '-second')
                if ($ws) {
                    Send-Command $ws 'HOLD'; Send-Command $ws 'CLOSE'
                    Wait-Text $stderr 'command=CLOSE' $process
                }
                [IO.File]::WriteAllText($server2.Gate, 'go')
                $stage = 'fixture.second-server-exit'; Finish-Server $server2
            } else {
                $stage = 'fixture.accept-first'; $peer = Accept-Peer $listener $process
                $stage = 'fixture.send-first'
                Send-Bytes $peer (Make-Packet 41)
                Send-Bytes $peer (Make-Packet 42)
                if ($mode -eq 'processing-error') { Send-Bytes $peer (Make-Packet ([uint32]::MaxValue)) }
                $peer.Shutdown([Net.Sockets.SocketShutdown]::Send)
            }
        }
        $stage = 'backend.exit'
        Assert-That ($process.WaitForExit(12000)) 'Backend did not finish within fixture bound'
        $errorText = Read-Text $stderr
        $text = (Read-Text $stdout) + "`n" + $errorText
        Assert-That ($text -notmatch 'winsock error 10014|code=10014\b|\[system:10014') 'Winsock 10014; not retried by fixture'
        $expectedExit = if ($mode -in @('refused-limit','partial-fin-restart','processing-error','idle-limit')) { 1 } else { 0 }
        Assert-That ($process.ExitCode -eq $expectedExit) "Unexpected exit $($process.ExitCode), expected $expectedExit"
        if ($mode -eq 'idle-recover') {
            Assert-That ($errorText -match 'session=1 end stage=idle_timeout code=0 .*first_byte_pending=1 last_byte_age_ms=never last_valid_frame_age_ms=never') 'First-byte timeout diagnostics missing'
            Assert-That ($errorText -match 'session=2 end stage=eof code=0 .*parsed=1 rejected=0 buffered=0 .*B_frames=1 .*first_timestamp_us=0 .*last_valid_frame_age_ms=\d+') 'Recovered session lost its first frame or reused prior state'
            Assert-That ([regex]::Matches($errorText, '\[radar\] retry=').Count -eq 1) 'Expected exactly one retry before clean EOF'
        } elseif ($mode -eq 'idle-limit') {
            Assert-That ($errorText -match 'session=1 end stage=idle_timeout code=0 .*parsed=0 rejected=0 buffered=10 .*first_byte_pending=0 last_byte_age_ms=\d+ last_valid_frame_age_ms=never') 'Idle after a partial packet did not record byte/parser state'
            Assert-That ([regex]::Matches($errorText, 'session=[234] end stage=idle_timeout code=0 .*parsed=0 rejected=0 buffered=0 .*first_byte_pending=1').Count -eq 3) 'Partial bytes leaked across retries'
            Assert-That ([regex]::Matches($errorText, '\[radar\] retry=').Count -eq 3 -and $errorText -match 'recovery exhausted retries=3') 'Idle used a different retry budget'
        } elseif ($mode -eq 'refused-limit') {
            Assert-That ([regex]::Matches($errorText, 'TCP connect failed.* operation=(connect|connect_SO_ERROR) family=2 address_length=16').Count -eq 4) 'Expected native connect diagnostics on the initial refusal and three retries'
            Assert-That ($errorText -match 'recovery exhausted retries=3') 'Retry limit was not reported'
        } elseif ($mode -eq 'processing-error') {
            Assert-That ($errorText -match 'frameCount exceeds B frame_id range' -and $errorText -notmatch '\[radar\] retry=') 'Processing exception retried'
        } elseif ($mode -eq 'fin-default') {
            Assert-That ($errorText -match 'end stage=eof' -and $errorText -notmatch '\[radar\] retry=') 'EOF retried without explicit EOF option'
        } elseif ($mode -eq 'rst-websocket') {
            Assert-That ($errorText -match 'session=1 end stage=receive code=10054') 'Expected actual RST/WSAECONNRESET'
            Assert-That ($text -match 'signal frame_id=7 timestamp_us=0 .*estimated_door_state=UNKNOWN') 'B history or timestamp leaked into new session'
            Assert-That ([regex]::Matches($errorText, '\[ws\] listening').Count -eq 1) 'WebSocket recreated'
            Assert-That ($errorText -match 'session=2 end stage=eof.*B_frames=1') 'B statistics not reset'
            Assert-That ($errorText.IndexOf('command=OPEN') -lt $errorText.IndexOf('command=HOLD') -and
                $errorText.IndexOf('command=HOLD') -lt $errorText.IndexOf('command=CLOSE')) 'Manual request order changed'
            $seals = [regex]::Matches($errorText, '\[ws-periodic\] session=(\d+) published=(\d+) mean_interval_us=\d+ sealed_enqueues=(\d+)')
            Assert-That ($seals.Count -eq 2 -and !($seals | Where-Object {$_.Groups[2].Value -ne $_.Groups[3].Value})) 'RST/EOF publisher seal was not preserved'
            Assert-That ($errorText -match 'stop.complete') 'WebSocket did not finish cleanup'
        }
        $profiles = @([regex]::Matches($text, '(?m)^AB_PROFILE_JSON (.+)\r?$') | ForEach-Object { ConvertFrom-Json $_.Groups[1].Value })
        if ($mode -eq 'partial-fin-restart') {
            Assert-That ($profiles.Count -eq 4) 'Expected four separate attempt reports'
            Assert-That ($profiles[0].parser_buffered_bytes -eq 10 -and $profiles[0].metrics.observed_frames -eq 2 -and
                $profiles[0].metrics.stored_samples -eq 1) 'Partial packet was not retained/reported'
            Assert-That ($profiles[1].parser_packets_parsed -eq 1 -and $profiles[1].parser_packets_rejected -eq 0 -and
                $profiles[1].parser_buffered_bytes -eq 0 -and $profiles[1].metrics.observed_frames -eq 1 -and
                $profiles[1].metrics.warmup_excluded -eq 1 -and $profiles[1].metrics.stored_samples -eq 0 -and
                $profiles[1].metrics.all_run_id_discontinuities -eq 0) 'Second session Parser/profile state leaked'
            Assert-That ($errorText -match 'session=2 end stage=eof.*B_frames=1') 'Second session B statistics leaked'
            Assert-That ($errorText -match 'session=1 end stage=eof.*last_door_state=CLOSED.*first_timestamp_us=0' -and
                $errorText -match 'session=2 end stage=eof.*last_door_state=UNKNOWN.*first_timestamp_us=0') 'B history or first timestamp was not reset'
        }
        if ($mode -eq 'refused-restart') {
            Assert-That ($profiles.Count -eq 2 -and $profiles[0].metrics.observed_frames -eq 0 -and
                $profiles[1].clean_eof -and $profiles[1].metrics.observed_frames -eq 2) 'Initial refusal/restart did not recover'
        }
        Write-Host "PASS mode=$mode exit=$($process.ExitCode) profiles=$($profiles.Count) tag=$tag logs=$outputDirectory"
    } catch {
        $failures += "$tag stage=$stage error=$_"
        Write-Error -ErrorAction Continue "FAIL mode=$mode stage=$stage tag=$tag error=$_ logs=$outputDirectory"
        $native = $_.Exception
        while ($native.InnerException) { $native = $native.InnerException }
        if ($native -is [Net.Sockets.SocketException]) { Write-Host "Native socket code=$($native.NativeErrorCode) stage=$stage" }
        foreach ($path in @($stdout,$stderr)) { Write-Host (Read-Text $path) }
    } finally {
        if ($ws) { $ws.Abort(); $ws.Dispose() }
        if ($peer) { $peer.Close(0) }
        if ($listener) { $listener.Close(0) }
        if ($process -and -not $process.HasExited) {
            Write-Warning "Terminating this fixture's failed/timed-out child PID=$($process.Id)"
            $process.Kill(); $null = $process.WaitForExit(1000)
        }
        if ($process) { $process.Dispose() }
        foreach ($server in @($server1,$server2)) {
            if ($server) {
                if (-not $server.Process.HasExited) {
                    Write-Warning "Terminating this fixture's server PID=$($server.Process.Id)"
                    $server.Process.Kill(); $null = $server.Process.WaitForExit(1000)
                }
                $server.Process.Dispose()
            }
        }
    }
}
if ($failures.Count) { throw "Radar reconnect fixture failures=$($failures.Count): $($failures -join '; ')" }
Write-Host "Radar reconnect fixture passed; logs=$outputDirectory"
