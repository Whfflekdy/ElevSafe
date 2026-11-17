[CmdletBinding()]
param([Parameter(Mandatory)][string]$BackendExe, [Parameter(Mandatory)][string]$SignalConfig)
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
function Receive-Text($socket, $cancel) {
    $bytes = [byte[]]::new(65536)
    $task = $socket.ReceiveAsync([ArraySegment[byte]]::new($bytes), $cancel.Token)
    $result = $task.GetAwaiter().GetResult()
    if (-not $result.EndOfMessage -or $result.MessageType -ne [Net.WebSockets.WebSocketMessageType]::Text) {
        throw 'Unexpected fragmented/close message in bounded fixture'
    }
    return [Text.Encoding]::UTF8.GetString($bytes,0,$result.Count)
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
    $child=Start-Process -FilePath $BackendExe -ArgumentList @('127.0.0.1',"$radarPort",'--signal-config',('"'+$SignalConfig+'"'),'--websocket','--ws-port',"$wsPort") -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
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
    foreach($path in @($stdout,$stderr)) { if(Test-Path $path) { Write-Host ([IO.File]::ReadAllText($path)) } }
    throw
} finally {
    foreach($ws in @($first,$second)) { if($ws) { $ws.Dispose() } }
    if($peer){$peer.Dispose()}; if($listener){$listener.Stop()}
    if($child) {
        if(-not $child.HasExited){$child.Kill(); $null=$child.WaitForExit(1000)}
        $child.Dispose()
    }
}