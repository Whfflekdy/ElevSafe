[CmdletBinding()]
param([Parameter(Mandatory)][string]$BackendExe, [Parameter(Mandatory)][string]$SignalConfig, [switch]$EmulatorDoorSim, [switch]$DoorArbitration, [switch]$AutoCycle, [switch]$PeriodicSnapshot, [switch]$PeriodicReconnect)
$ErrorActionPreference = 'Stop'
$BackendExe = (Resolve-Path -LiteralPath $BackendExe).Path
$SignalConfig = (Resolve-Path -LiteralPath $SignalConfig).Path
$logs = Join-Path (Split-Path $BackendExe) ('tracking-fixture/' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $logs
if ($DoorArbitration) {
    if (-not $EmulatorDoorSim) { throw 'DoorArbitration requires EmulatorDoorSim' }
    # Add a threshold strip to the existing identity-transform TCP fixture.
    $config=[IO.File]::ReadAllText($SignalConfig)
    $config=$config.Replace('door.x_min = 100','door.x_min = 0').Replace('door.x_max = 101','door.x_max = 1')
    $config=$config.Replace('door.y_min = 100','door.y_min = -0.2').Replace('door.y_max = 101','door.y_max = 0.2')
    $config=$config.Replace('door.z_min = 100','door.z_min = 0').Replace('door.z_max = 101','door.z_max = 2')
    $SignalConfig=Join-Path $logs 'door-arbitration.cfg'
    [IO.File]::WriteAllText($SignalConfig,$config)
}

function Packet([uint32]$frameId, [uint32]$count, [single]$y = 0.8) {
    # Existing retina_protocol_test / websocket_radar_fixture wire layout, no new protocol.
    $memory = [IO.MemoryStream]::new(); $writer = [IO.BinaryWriter]::new($memory)
    try {
        foreach ($word in @([uint32]0,[uint32]2882356001,[uint32]0,[uint32]0,
            [uint32](16 + 24 * $count),[uint32]0,[uint32]0,[uint32]0,[uint32]0)) { $writer.Write($word) }
        $writer.Write([uint64]578437695752307201); $writer.Write($frameId); $writer.Write($count)
        for ($j=0; $j -lt $count; $j++) {
            foreach ($v in @([single]0.25,$y,[single]1,[single]0,[single]10)) { $writer.Write($v) }
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
    $radarHost=if($PeriodicSnapshot -and -not $EmulatorDoorSim -and -not $PeriodicReconnect){'localhost'}else{'127.0.0.1'}
    $arguments=@($radarHost,"$radarPort",'--signal-config',('"'+$SignalConfig+'"'),'--websocket','--ws-port',"$wsPort")
    if ($EmulatorDoorSim) { $arguments+=@('--emulator-door-sim','--radar-reconnect','--radar-reconnect-eof') }
    if ($AutoCycle) { $arguments+='--emulator-auto-cycle' }
    if ($PeriodicSnapshot) { $arguments+='--ws-periodic-snapshot' }
    if ($PeriodicReconnect) { $arguments+=@('--radar-reconnect','--radar-reconnect-eof') }
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

    if ($PeriodicSnapshot) {
        function Send-AfterQuiet($radarPeer, [switch]$AllowInFlight) {
            $buffer=[byte[]]::new(65536)
            $cancel=[Threading.CancellationTokenSource]::new(5000)
            try {
                while ($true) {
                    $pending=$first.ReceiveAsync([ArraySegment[byte]]::new($buffer),$cancel.Token)
                    if (-not $pending.Wait(380)) { break }
                    if (-not $AllowInFlight) { throw 'Periodic Snapshot before the first observation of a session' }
                    $null=Read-SimSnapshot $pending $buffer
                    $AllowInFlight=$false  # One old write may already have been queued before EOF.
                }
                $bytes=Packet 100 40
                $radarPeer.GetStream().Write($bytes,0,$bytes.Length)
                return Read-SimSnapshot $pending $buffer
            } finally { $cancel.Dispose() }
        }

        $stage='periodic.first-observation'
        $initial=Send-AfterQuiet $peer
        $initialState=if($AutoCycle){'OPENING'}else{$initial.door_state}
        $initialCommand=if($AutoCycle){'OPEN'}else{$null}
        if ($initial.timestamp_us -ne 0 -or $initial.door_state -ne $initialState -or
            $initial.door_command -ne $initialCommand -or $initial.passengers.Count -ne 0) {
            throw 'Incorrect first Snapshot'
        }
        $cancel=[Threading.CancellationTokenSource]::new(2500)
        try {
            $other=ConvertFrom-Json -InputObject (Receive-Text $second $cancel)
            if ((ConvertTo-Json -InputObject $initial -Depth 5 -Compress) -cne
                (ConvertTo-Json -InputObject $other -Depth 5 -Compress)) { throw 'Viewer first Snapshots differ' }
        } finally { $cancel.Dispose() }

        $stage='periodic.active-input'
        $feed=[Diagnostics.Stopwatch]::StartNew()
        for ($i=0; $i -lt 12; ++$i) {
            $wait=50*$i-$feed.Elapsed.TotalMilliseconds
            if ($wait -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($wait)) }
            $bytes=Packet ([uint32](101+$i)) 40
            $peer.GetStream().Write($bytes,0,$bytes.Length)
        }
        $lastTime=-1
        $fixed=$null
        for ($i=0; $i -lt 23; ++$i) {
            $s=Read-SimSnapshot
            if ($null -ne $s.door_command) { throw 'Initial command was repeated or an unexpected command was applied' }
            if ($s.timestamp_us -lt $lastTime) { throw 'Timestamp regressed inside one TCP session' }
            if ($s.timestamp_us -eq $lastTime -and $s.passengers.Count -eq 1) { $fixed=$s; break }
            $lastTime=$s.timestamp_us
        }
        if ($null -eq $fixed -or $fixed.timestamp_us -le 0 -or $fixed.passengers[0].id -ne 1 -or
            $fixed.passengers[0].state -ne 'INSIDE' -or $null -ne $fixed.passengers[0].safety_state) {
            throw 'Missing tracked passenger on the latest observation'
        }

        $stage='periodic.paused-input'
        # The second Viewer deliberately stops reading; the first must continue unaffected.
        $duration=if($EmulatorDoorSim){7100}elseif($PeriodicReconnect){3100}else{10100}
        $paused=[Diagnostics.Stopwatch]::StartNew()
        $count=0
        $sawOpen=$false
        $wallChanged=$false
        $track=ConvertTo-Json -InputObject @($fixed.passengers) -Depth 5 -Compress
        do {
            $s=Read-SimSnapshot
            $count++
            $wallChanged=$wallChanged -or $s.timestamp -ne $fixed.timestamp
            if ($s.timestamp_us -ne $fixed.timestamp_us -or $null -ne $s.door_command -or
                (ConvertTo-Json -InputObject @($s.passengers) -Depth 5 -Compress) -cne $track) {
                throw "Paused observation changed at $($paused.ElapsedMilliseconds)ms: $($s.timestamp_us) expected $($fixed.timestamp_us)"
            }
            if ($s.timestamp -notmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$') { throw 'Invalid UTC publication time' }
            if ($EmulatorDoorSim) {
                if ($s.door_state -notin @('OPENING','OPEN')) { throw 'Periodic tick drove automatic CLOSE without input' }
                if ($s.door_state -eq 'OPEN') { $sawOpen=$true }
            }
        } while ($paused.ElapsedMilliseconds -lt $duration)
        $viewerMean=$paused.Elapsed.TotalMilliseconds/$count
        if (-not $wallChanged -or $viewerMean -lt 75 -or $viewerMean -gt 180 -or ($EmulatorDoorSim -and -not $sawOpen)) {
            throw "Incorrect idle publication: received=$count meanMs=$viewerMean wallChanged=$wallChanged sawOpen=$sawOpen"
        }
        Write-Host "PASS periodic idle: mode=$radarHost emulator=$EmulatorDoorSim pauseMs=$($paused.ElapsedMilliseconds) viewer=$count viewerMeanMs=$viewerMean timestamp_us=$($fixed.timestamp_us)"

        if ($EmulatorDoorSim) {
            $stage='periodic.manual-close'
            Send-Door $first 'CLOSE'
            $closeEventCount=0
            $closed=$false
            $motion=[Diagnostics.Stopwatch]::StartNew()
            do {
                $s=Read-SimSnapshot
                if ($s.timestamp_us -ne $fixed.timestamp_us) { throw 'Manual motion changed Radar time' }
                if ($null -ne $s.door_command) {
                    if ($s.door_command -ne 'CLOSE' -or $s.door_state -ne 'CLOSING') { throw 'Invalid applied CLOSE' }
                    $closeEventCount++
                }
                $closed=$s.door_state -eq 'CLOSED'
            } while (-not $closed -and $motion.ElapsedMilliseconds -lt 5000)
            if (-not $closed -or $closeEventCount -ne 1 -or $null -ne $s.door_command) { throw 'Manual motion/event did not complete once' }
            Write-Host 'PASS periodic paused manual CLOSE: one applied event and CLOSED/null'
        }

        $stage='periodic.eof'
        if ($PeriodicReconnect -or $EmulatorDoorSim) {
            $peer=Restart-SimRadar $peer 1
            $reset=Send-AfterQuiet $peer -AllowInFlight
            if ($reset.timestamp_us -ne 0 -or $null -ne $reset.door_command -or $reset.passengers.Count -ne 0 -or
                ($EmulatorDoorSim -and $reset.door_state -ne 'CLOSED')) { throw 'Prior session leaked into reconnect' }
            $peer=Restart-SimRadar $peer 2
            $peer=Restart-SimRadar $peer 3
        }
        $peer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        if (-not $child.WaitForExit(6500)) { throw 'Backend did not finish after EOF' }
        $expectedExit=if($PeriodicReconnect -or $EmulatorDoorSim){1}else{0}
        if ($child.ExitCode -ne $expectedExit) { throw "Unexpected Backend exit $($child.ExitCode)" }
        $text=Read-LiveLog $stderr
        if ($text -notmatch '\[ws-periodic\] session=1 published=(\d+) mean_interval_us=(\d+)') { throw 'Backend publication metrics missing' }
        $published=[int]$Matches[1]
        $meanUs=[int]$Matches[2]
        if ($published -lt $count -or $meanUs -lt 95000 -or $meanUs -gt 180000 -or
            $text -notmatch '\[tracking-demo\] session=1 frames=13 snapshots=\d+ exit=0' -or
            $text -match '10014|Signal frame failed|\[ws-periodic\] stopped') { throw 'Backend cadence/processing metrics invalid' }
        if ($expectedExit -eq 1 -and $text -notmatch '\[ws-periodic\] session=3 published=0 mean_interval_us=0') {
            throw 'Periodic publication during a reconnect with no observation'
        }
        Write-Host "PASS periodic EOF: backendPublished=$published backendMeanUs=$meanUs viewerMeanMs=$viewerMean ExitCode=$($child.ExitCode)"
        return
    }

    if ($DoorArbitration -or $AutoCycle) {
        $script:arbFrame=100
        $script:arbTime=-1
        function Next-Radar([single]$y) {
            # Pace observations beyond the Snapshot limiter so the fixture can observe each one.
            Start-Sleep -Milliseconds 125
            $bytes=Packet ([uint32]$script:arbFrame) 40 $y
            $script:arbFrame++
            $peer.GetStream().Write($bytes,0,$bytes.Length)
            do { $s=Read-SimSnapshot } while ($s.timestamp_us -le $script:arbTime)
            $script:arbTime=$s.timestamp_us
            return $s
        }
        function Expect-SimState([string]$state, [string]$command) {
            $s=Read-SimSnapshot
            if ($s.door_state -cne $state -or [string]$s.door_command -cne $command) {
                throw "Wanted $state / '$command'; got $($s.door_state) / '$($s.door_command)'"
            }
            return $s
        }
        function Send-ManualAndComplete([string]$command, [string]$moving, [string]$done) {
            Send-Door $first $command
            $null=Expect-SimState $moving $command
            $null=Expect-SimState $moving ''
            $null=Expect-SimState $done ''
        }
        function Occupy-Threshold {
            # Move a matched track into the threshold gradually.
            foreach ($y in @(0.4,0.32,0.24,0.16,0.08,0.0,0.0,0.0)) {
                $s=Next-Radar $y
                if ($s.door_state -ne 'OPEN') { throw 'Door closed on threshold approach' }
                if ($s.door_command -eq 'HOLD') {
                    if ($s.passengers.Count -ne 1 -or $s.passengers[0].state -notin @('ENTERING','EXITING') -or
                    $null -ne $s.passengers[0].safety_state) { throw 'HOLD did not carry the C threshold phase' }
                    return $s.passengers[0].id
                }
            }
            throw 'No automatic HOLD from threshold occupancy'
        }
        function Clear-Threshold($id) {
            foreach ($y in @(0.08,0.16,0.24,0.32,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4)) {
                $s=Next-Radar $y
                if ($s.door_state -ne 'OPEN' -or $null -ne $s.door_command) { throw 'Premature or duplicate command before the pause' }
            }
            if ($s.passengers.Count -ne 1 -or $s.passengers[0].id -ne $id -or
                    $s.passengers[0].state -ne 'INSIDE') { throw 'Expected the same track to clear the threshold' }
        }
        function Expect-ResumeClose($id) {
            $firstFresh=$null
            $fresh=0
            for ($i=0; $i -lt 18; ++$i) {
                $s=Next-Radar 0.4
                if ($null -eq $firstFresh) { $firstFresh=$s.timestamp_us }
                $fresh++
                if ($s.passengers.Count -ne 1 -or $s.passengers[0].id -ne $id -or
                    $s.passengers[0].state -ne 'INSIDE') { throw 'C track ID or phase lost on resume' }
                if ($s.door_command -eq 'CLOSE') {
                    if ($s.door_state -ne 'CLOSING' -or $fresh -lt 3 -or $s.timestamp_us-$firstFresh -lt 1000000) {
                        throw 'CLOSE escaped the fresh-input guard'
                    }
                    Write-Host "PASS gated auto CLOSE: snapshots=$fresh spanUs=$($s.timestamp_us-$firstFresh) id=$id"
                    return
                }
                if ($s.door_state -ne 'OPEN' -or $null -ne $s.door_command) {
                    throw 'Rejected or duplicate C request leaked into v7'
                }
            }
            throw 'Automatic CLOSE did not run after stable input'
        }

        if ($AutoCycle) {
            $stage='auto-cycle.arrival'
            $s=Next-Radar 0.4
            if ($s.door_state -ne 'OPENING' -or $s.door_command -ne 'OPEN' -or $s.timestamp_us -ne 0) {
                throw 'First fresh observation did not apply AUTO OPEN'
            }
            $null=Expect-SimState 'OPENING' ''
            $s=Expect-SimState 'OPEN' ''
            if ($s.timestamp_us -ne 0) { throw 'OPEN completion changed the paused source timestamp' }
            $stage='auto-cycle.close'
            if ($DoorArbitration) {
                $id=Occupy-Threshold
                Clear-Threshold $id
                # Keep the socket open while making the last C observation stale.
                Start-Sleep -Milliseconds 2300
                Expect-ResumeClose $id
            } else {
                $dwell=[Diagnostics.Stopwatch]::StartNew()
                do {
                    $s=Next-Radar 0.4
                    if ($s.door_state -eq 'OPEN' -and $null -eq $s.door_command) { continue }
                    if ($s.door_state -ne 'CLOSING' -or $s.door_command -ne 'CLOSE' -or
                        $dwell.ElapsedMilliseconds -lt 3800) {
                        throw "Unexpected normal cycle at $($dwell.ElapsedMilliseconds)ms: $($s.door_state) / $($s.door_command)"
                    }
                    break
                } while ($dwell.ElapsedMilliseconds -lt 6500)
                if ($s.door_state -ne 'CLOSING') { throw 'Normal CLOSE missing after the dwell' }
                Write-Host "PASS normal CLOSE: viewer elapsedMs=$($dwell.ElapsedMilliseconds) from OPEN"
            }
            $null=Expect-SimState 'CLOSING' ''
            $null=Expect-SimState 'CLOSED' ''
            $stage='auto-cycle.reconnect'
            $peer=Restart-SimRadar $peer 1
            Expect-Reset $peer
            $peer=Restart-SimRadar $peer 2
            $peer=Restart-SimRadar $peer 3
            $peer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
            if (-not $child.WaitForExit(6000) -or $child.ExitCode -ne 1) { throw 'Expected bounded retry exhaustion' }
            Write-Host "PASS auto cycle: occupied=$DoorArbitration, OPEN -> CLOSE -> CLOSED, v7, no second arrival after reconnect"
            return
        }

        $stage='arbitration.closed'
        $s=Next-Radar 0.4
        if ($s.door_state -ne 'CLOSED' -or $null -ne $s.door_command) { throw 'Door opened without a manual request' }
        $stage='arbitration.manual-open'
        Send-ManualAndComplete 'OPEN' 'OPENING' 'OPEN'
        $stage='arbitration.auto-hold'
        $id=Occupy-Threshold
        Clear-Threshold $id
        $stage='arbitration.pause-and-resume'
        Start-Sleep -Milliseconds 2300
        Expect-ResumeClose $id
        $stage='arbitration.close-while-paused'
        $null=Expect-SimState 'CLOSING' ''
        $null=Expect-SimState 'CLOSED' ''
        $s=Next-Radar 0.4
        if ($s.door_state -ne 'CLOSED' -or $null -ne $s.door_command) { throw 'Completed auto CLOSE replayed' }

        $stage='arbitration.stale-c-after-manual'
        Send-ManualAndComplete 'OPEN' 'OPENING' 'OPEN'
        $id=Occupy-Threshold
        Clear-Threshold $id
        # No Radar observes either CLOSED or OPENING in this manual cycle.
        Send-ManualAndComplete 'CLOSE' 'CLOSING' 'CLOSED'
        Send-ManualAndComplete 'OPEN' 'OPENING' 'OPEN'
        Expect-ResumeClose $id

        $null=Expect-SimState 'CLOSING' ''
        $stage='arbitration.reconnect'
        $peer=Restart-SimRadar $peer 1
        Expect-Reset $peer
        $peer=Restart-SimRadar $peer 2
        $peer=Restart-SimRadar $peer 3
        $peer.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        if (-not $child.WaitForExit(6000) -or $child.ExitCode -ne 1) { throw 'Expected bounded retry exhaustion' }
        Write-Host 'PASS arbitration: auto HOLD/CLOSE, manual CLOSED not observed, resume guard, track identity, v7, reconnect'
        return
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