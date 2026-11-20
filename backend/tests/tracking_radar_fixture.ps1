[CmdletBinding()]
param([Parameter(Mandatory)][string]$BackendExe, [Parameter(Mandatory)][string]$SignalConfig, [switch]$EmulatorDoorSim)
$ErrorActionPreference = 'Stop'
$BackendExe = (Resolve-Path -LiteralPath $BackendExe).Path
$SignalConfig = (Resolve-Path -LiteralPath $SignalConfig).Path
$logs = Join-Path (Split-Path $BackendExe) ('tracking-fixture/' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $logs

function Packet([uint32]$frameId, [uint32]$count) {
    # Existing retina_protocol_test / websocket_radar_fixture wire layout, no new protocol.
    $memory = [IO.MemoryStream]::new(); $writer = [IO.BinaryWriter]::new($memory)
    try {
        foreach ($word in @([uint32]0,[uint32]2882356001,[uint32]0,[uint32]0,
            [uint32](16 + 24 * $count),[uint32]0,[uint32]0,[uint32]0,[uint32]0)) { $writer.Write($word) }
        $writer.Write([uint64]578437695752307201); $writer.Write($frameId); $writer.Write($count)
        for ($j=0; $j -lt $count; $j++) {
            foreach ($v in @([single]0.25,[single]0.8,[single]1,[single]0,[single]10)) { $writer.Write($v) }
        }
        for ($j=0; $j -lt $count; $j++) { $writer.Write([int32]255) }
        $writer.Flush(); return ,$memory.ToArray()
    } finally { $writer.Dispose(); $memory.Dispose() }
}
function Read-LiveLog([string]$path) {
    $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $reader=[IO.StreamReader]::new($stream)
        try { $reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}
function Read-TextResult($task, [byte[]]$bytes) {
    $result = $task.GetAwaiter().GetResult()
    if (-not $result.EndOfMessage -or $result.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text) {
        throw 'Unexpected fragmented/close message in bounded fixture'
    }
    return [Text.Encoding]::UTF8.GetString($bytes,0,$result.Count)
}
function Receive-Text($socket, $cancel) {
    $bytes = [byte[]]::new(65536)
    $task = $socket.ReceiveAsync([ArraySegment[byte]]::new($bytes), $cancel.Token)
    Read-TextResult $task $bytes
}
$listener=$null; $peer=$null; $child=$null; $first=$null; $second=$null
$stdout=Join-Path $logs 'stdout.log'; $stderr=Join-Path $logs 'stderr.log'; $stage='setup'
try {
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $stage='fixture.listen'; $listener.Start(8); $radarPort=$listener.LocalEndpoint.Port
    # Backend CLI requires a nonzero port. Reserve and release an ephemeral port, no retries on collision.
    $reservation=[Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork,[Net.Sockets.SocketType]::Stream,[Net.Sockets.ProtocolType]::Tcp)
    try { $reservation.Bind([Net.IPEndPoint]::new([Net.IPAddress]::Loopback,0)); $wsPort=$reservation.LocalEndPoint.Port }
    finally { $reservation.Dispose() }
    $accept=$listener.AcceptTcpClientAsync()
    $stage='backend.start'
    $arguments=@('127.0.0.1',"$radarPort",'--signal-config',('"'+$SignalConfig+'"'),'--websocket','--ws-port',"$wsPort")
    if ($EmulatorDoorSim) { $arguments+=@('--emulator-door-sim','--radar-reconnect','--radar-reconnect-eof') }
    $child=Start-Process -FilePath $BackendExe -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null=$child.Handle
    Write-Host "START PID=$($child.Id) radar=$radarPort ws=$wsPort UTC=$([DateTime]::UtcNow.ToString('o')) logs=$logs"
    $stage='radar.accept'
    if (-not $accept.Wait(6000)) { throw 'Backend did not connect' }
    $peer=$accept.GetAwaiter().GetResult(); $peer.SendTimeout=2000
    $stage='websocket.connect'
    $first=[Net.WebSockets.ClientWebSocket]::new(); $second=[Net.WebSockets.ClientWebSocket]::new()
    foreach ($ws in @($first,$second)) {
        $cancel=[Threading.CancellationTokenSource]::new(3000)
        try { $ws.ConnectAsync([Uri]"ws://127.0.0.1:$wsPort/radar",$cancel.Token).GetAwaiter().GetResult() }
        finally { $cancel.Dispose() }
    }

    function Read-SimSnapshot($pending = $null, [byte[]]$buffer = $null) {
        if ($null -ne $pending) {
            $text=Read-TextResult $pending $buffer
        } else {
            $cancel=[Threading.CancellationTokenSource]::new(6500)
            try { $text=Receive-Text $first $cancel }
            finally { $cancel.Dispose() }
        }
        $json=ConvertFrom-Json -InputObject $text
        if (Compare-Object @('door_command','door_state','passengers','timestamp','timestamp_us') @($json.PSObject.Properties.Name) -CaseSensitive) {
            throw 'Snapshot keys changed'
        }
        return $json
    }

    function Send-Door($socket, [string]$command) {
        $cancel=[Threading.CancellationTokenSource]::new(2000)
        try {
            $bytes=[Text.Encoding]::UTF8.GetBytes('{"door_command":"'+$command+'"}')
            $null=$socket.SendAsync([ArraySegment[byte]]::new($bytes),[Net.WebSockets.WebSocketMessageType]::Text,$true,$cancel.Token).GetAwaiter().GetResult()
        } finally { $cancel.Dispose() }
    }

    function Wait-SimLog([string]$pattern, [int]$count = 1) {
        $timer=[Diagnostics.Stopwatch]::StartNew()
        while ($timer.ElapsedMilliseconds -lt 2500) {
            if ($child.HasExited) { throw "Backend exited while waiting for $pattern" }
            if (([regex]::Matches((Read-LiveLog $stderr),$pattern)).Count -ge $count) { return }
            Start-Sleep -Milliseconds 20
        }
        throw "Missing log: $pattern (count=$count)"
    }

    function Expect-Door([string]$state, [string]$command, $pending = $null, [byte[]]$buffer = $null) {
        $snapshot=Read-SimSnapshot $pending $buffer
        $passengers=ConvertTo-Json -InputObject @($snapshot.passengers) -Depth 5 -Compress
        if ($snapshot.timestamp_us -ne $frozenTime -or $passengers -cne $frozenPassengers) {
            throw 'Door update replaced Radar time or passengers during the pause'
        }
        if ($snapshot.door_state -cne $state -or [string]$snapshot.door_command -cne $command) {
            throw "Wanted $state / '$command'; got $($snapshot.door_state) / '$($snapshot.door_command)'"
        }
    }

    function Restart-SimRadar($oldPeer, [int]$session, [switch]$CheckClosedWindow) {
        $accept=$listener.AcceptTcpClientAsync()
        $oldPeer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        Wait-SimLog "\[radar\] session=$session end"
        if ($CheckClosedWindow) {
            Send-Door $first 'HOLD'
            Wait-SimLog 'manual request ignored: no active command window' 2
        }
        if (-not $accept.Wait(6000)) { throw "Radar client did not reconnect after session $session" }
        $replacement=$accept.GetAwaiter().GetResult()
        $oldPeer.Dispose()
        return $replacement
    }

    function Expect-Reset($radarPeer) {
        $frame=Packet ([uint32]1) ([uint32]40)
        $radarPeer.GetStream().Write($frame,0,$frame.Length)
        $reset=Read-SimSnapshot
        if ($reset.timestamp_us -ne 0 -or $reset.door_state -ne 'CLOSED' -or $null -ne $reset.door_command -or $reset.passengers.Count -ne 0) {
            throw 'Radar/Tracking or virtual door state leaked across sessions'
        }
    }

    if ($EmulatorDoorSim) {
        $stage='emulator-door.pre-frame'
        Send-Door $first 'CLOSE'
        Wait-SimLog 'manual request ignored: no active command window'
        $batch=[IO.MemoryStream]::new()
        try {
            for ($i=0; $i -lt 12; ++$i) {
                $bytes=Packet ([uint32](100+$i)) ([uint32]40)
                $batch.Write($bytes,0,$bytes.Length)
            }
            $initial=$batch.ToArray()
            $peer.GetStream().Write($initial,0,$initial.Length)
        } finally { $batch.Dispose() }
        do {
            $last=Read-SimSnapshot
            if ($last.door_state -ne 'CLOSED' -or $null -ne $last.door_command) { throw 'Unexpected initial virtual state' }
        } while ($last.passengers.Count -ne 1)
        $stage='emulator-door.paused-open'
        Send-Door $first 'OPEN'
        do {
            $open=Read-SimSnapshot
            if ($open.door_state -ne 'CLOSED' -and $open.door_state -ne 'OPENING') { throw 'Unexpected state before OPEN' }
        } while ($open.door_state -eq 'CLOSED')
        if ($open.door_command -ne 'OPEN' -or $open.passengers[0].id -ne 1) { throw 'OPEN event or track missing while paused' }
        $frozenTime=$open.timestamp_us
        $frozenPassengers=ConvertTo-Json -InputObject @($open.passengers) -Depth 5 -Compress
        Send-Door $first 'OPEN'
        Expect-Door 'OPENING' ''
        Expect-Door 'OPEN' ''
        $stage='emulator-door.open-idle'
        # Leave the same receive pending through the silence window and the
        # subsequent command. Canceling it early would abort the WebSocket.
        $buffer=[byte[]]::new(65536)
        $cancel=[Threading.CancellationTokenSource]::new(9000)
        $silence=[Diagnostics.Stopwatch]::StartNew()
        try {
            $pending=$first.ReceiveAsync([ArraySegment[byte]]::new($buffer), $cancel.Token)
            if ($pending.Wait(5200)) { throw 'Unexpected Snapshot after OPEN without Radar input or a new command' }
            $quietMs=$silence.ElapsedMilliseconds
            $stage='emulator-door.hold'
            Send-Door $second 'HOLD'
            Expect-Door 'OPEN' 'HOLD' $pending $buffer
            Write-Host "PASS OPEN idle: no Snapshot for >=$($quietMs)ms; next update at $($silence.ElapsedMilliseconds)ms; timestamp_us=$frozenTime preserved"
        } finally { $cancel.Dispose() }
        Expect-Door 'OPEN' ''
        $second.Dispose(); $second=$null
        Start-Sleep -Milliseconds 300
        $stage='emulator-door.reverse'
        Send-Door $first 'CLOSE'
        Expect-Door 'CLOSING' 'CLOSE'
        Expect-Door 'CLOSING' ''
        Send-Door $first 'OPEN'
        Expect-Door 'OPENING' 'OPEN'
        Expect-Door 'OPENING' ''
        Send-Door $first 'CLOSE'
        Expect-Door 'CLOSING' 'CLOSE'
        Send-Door $first 'CLOSE'
        Expect-Door 'CLOSING' ''
        Expect-Door 'CLOSED' ''
        $stage='emulator-door.resume'
        $frame=Packet ([uint32]112) ([uint32]40)
        $peer.GetStream().Write($frame,0,$frame.Length)
        $resumed=Read-SimSnapshot
        if ($resumed.timestamp_us -le $frozenTime -or $resumed.door_state -ne 'CLOSED' -or $null -ne $resumed.door_command) {
            throw 'Fresh Radar observation did not resume independently of the door'
        }
        $stage='emulator-door.reconnect-from-open'
        $frozenTime=$resumed.timestamp_us
        $frozenPassengers=ConvertTo-Json -InputObject @($resumed.passengers) -Depth 5 -Compress
        Send-Door $first 'OPEN'
        Expect-Door 'OPENING' 'OPEN'
        Expect-Door 'OPENING' ''
        Expect-Door 'OPEN' ''
        $peer=Restart-SimRadar $peer 1 -CheckClosedWindow
        Expect-Reset $peer
        Write-Host 'PASS session 1 OPEN -> session 2 CLOSED, command=null, timestamp_us=0'

        $stage='emulator-door.reconnect-from-hold'
        $frozenTime=0
        $frozenPassengers='[]'
        Send-Door $first 'HOLD'
        Expect-Door 'OPENING' 'HOLD'
        Expect-Door 'OPENING' ''
        Expect-Door 'OPEN' ''
        $peer=Restart-SimRadar $peer 2
        Expect-Reset $peer
        Write-Host 'PASS session 2 HOLD -> session 3 CLOSED, command=null, timestamp_us=0'

        $stage='emulator-door.bounded-shutdown'
        # Complete the remaining retry with explicit EOF.
        $peer=Restart-SimRadar $peer 3
        $peer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        if (-not $child.WaitForExit(6000) -or $child.ExitCode -ne 1) { throw 'Expected bounded retry exhaustion' }
        $text=[IO.File]::ReadAllText($stderr)+[IO.File]::ReadAllText($stdout)
        if ($text -notmatch '\[tracking-demo\] session=1 frames=13 snapshots=\d+ exit=0' -or
            $text -notmatch '\[radar\] session=1 end[\s\S]*?last_door_state=CLOSED' -or
            $text -notmatch 'recovery exhausted' -or $text -match '10014|Signal frame failed') {
            throw 'Missing continued B observation, Radar frame accounting or clean retry exhaustion'
        }
        Write-Host 'PASS emulator-door: OPEN/CLOSE/HOLD, duplicate and reversal, paused Radar, v7/frozen timestamp, B observation, reconnect and bounded shutdown'
        return
    }

    $stage='radar.send'
    $clock=[Diagnostics.Stopwatch]::StartNew()
    for ($i=0; $i -lt 64; $i++) {
        # Intentional 20Hz input pacing, not a socket-error retry or workaround.
        $wait=50*$i-$clock.Elapsed.TotalMilliseconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($wait)) }
        $count=if($i -lt 60){40}else{0}
        $bytes=Packet ([uint32](100+$i)) $count
        $peer.GetStream().Write($bytes,0,$bytes.Length)
    }
    $stage='websocket.receive'
    $samples=[Collections.Generic.List[object]]::new(); $sawTrack=$false; $sawEmptyAfterTrack=$false
    for ($i=0; $i -lt 100; $i++) {
        $cancel=[Threading.CancellationTokenSource]::new(700)
        try {
            $a=Receive-Text $first $cancel; $b=Receive-Text $second $cancel
            if ($a -cne $b) { throw 'Two-client snapshots differ' }
        } catch {
            if ($cancel.IsCancellationRequested) { break } # Assert no old frames are emitted during input silence.
            throw
        } finally { $cancel.Dispose() }
        $json=ConvertFrom-Json -InputObject $a
        if ($null -ne $json.door_command -or $json.timestamp -notmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$') { throw 'v7 timestamp/command mismatch' }
        if ([Math]::Abs(([DateTime]::UtcNow-[DateTime]::Parse($json.timestamp).ToUniversalTime()).TotalSeconds) -gt 30) { throw 'Fixed/stale wall timestamp' }
        if ($samples.Count -gt 0 -and $json.timestamp_us -le $samples[$samples.Count-1].timestamp_us) { throw 'Non-increasing source time' }
        if ($json.passengers.Count -gt 0) {
            if ($json.passengers.Count -ne 1 -or $json.passengers[0].id -ne 1 -or
                [Math]::Abs($json.passengers[0].x-0.25) -gt 0.01 -or [Math]::Abs($json.passengers[0].y-0.8) -gt 0.01 -or
                $json.passengers[0].state -ne 'INSIDE' -or $null -ne $json.passengers[0].safety_state) { throw 'C track mapping mismatch' }
            $sawTrack=$true
        } elseif ($sawTrack) { $sawEmptyAfterTrack=$true }
        $samples.Add($json)
    }
    if ($samples.Count -lt 12 -or $samples.Count -gt 40 -or -not $sawTrack -or -not $sawEmptyAfterTrack) { throw 'Missing tracking, empty frame, or rate limit' }
    $meanUs=($samples[$samples.Count-1].timestamp_us-$samples[0].timestamp_us)/($samples.Count-1)
    if ($meanUs -lt 80000 -or $meanUs -gt 250000) { throw 'Publish interval outside bounded 20Hz fixture expectation' }
    $stage='backend.exit'; $peer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
    if (-not $child.WaitForExit(4000)) { throw 'Backend did not exit on EOF' }
    $text=[IO.File]::ReadAllText($stdout)+[IO.File]::ReadAllText($stderr)
    if ($child.ExitCode -ne 0 -or $text -notmatch '\[tracking-demo\] session=1 frames=64 snapshots=\d+ exit=0') { throw "Backend result missing/failed: $($child.ExitCode)" }
    if ($text -match '10014|Signal frame failed') { throw 'Backend processing/socket failure; no retry' }
    Write-Host "PASS frames=64 inputTargetHz=20 snapshots=$($samples.Count) meanSourceIntervalUs=$meanUs clients=2 trackingID=1 emptyAfterTrack=true silence=true EOF=true ExitCode=$($child.ExitCode)"
} catch {
    Write-Host "FAIL stage=$stage error=$($_.Exception.ToString()) logs=$logs"
    foreach($path in @($stdout,$stderr)) { if(Test-Path $path) { Write-Host (Read-LiveLog $path) } }
    throw
} finally {
    foreach($ws in @($first,$second)) { if($ws) { $ws.Dispose() } }
    if($peer){$peer.Dispose()}; if($listener){$listener.Stop()}
    if($child) {
        if(-not $child.HasExited){$child.Kill(); $null=$child.WaitForExit(1000)}
        $child.Dispose()
    }
}