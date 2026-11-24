[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Mock', 'Radar')][string]$Mode,
    [Parameter(Mandatory)][string]$BackendExe,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExpectedBackendSha256,
    [ValidateRange(1, 65535)][int]$WsPort = 8080,
    [ValidateRange(1, 86400)][int]$MockDuration = 600,
    [switch]$DoorSim,
    [switch]$EmulatorDoorSim,
    [switch]$RemoteControl,
    [ValidateNotNullOrEmpty()][string]$RadarHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$RadarPort = 29172,
    [string]$SignalConfig,
    [ValidateRange(1, 120)][int]$ReadyTimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$bridgeScript = Join-Path $PSScriptRoot 'bridge/bridge.js'
$localUrl = "ws://127.0.0.1:$WsPort/radar"

function Resolve-RequiredFile([string]$Path, [string]$Description) {
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $root $Path }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description file not found: $Path"
    }
    (Resolve-Path -LiteralPath $Path).Path
}

function ConvertTo-NativeArgument([string]$Value) {
    # ProcessStartInfo.ArgumentList is unavailable in Windows PowerShell 5.1.
    $quoted = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    '"' + [regex]::Replace($quoted, '(\\+)$', '$1$1') + '"'
}

function Start-Child([string]$Executable, [string[]]$Arguments, [switch]$Bridge) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = ($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $info.WorkingDirectory = $root
    $info.UseShellExecute = $false
    # Share the console so console Ctrl+C can also reach the children.
    $info.EnvironmentVariables.Remove('CONTROL_OPERATOR_TOKEN')
    if ($Bridge) {
        $info.EnvironmentVariables['LOCAL_URL'] = $localUrl
        $info.EnvironmentVariables['CONTROL_ENABLED'] = if ($RemoteControl) {'1'} else {'0'}
        if ($RemoteControl) { $info.EnvironmentVariables['CONTROL_LOCAL_URL'] = "ws://127.0.0.1:$WsPort/control-local" }
        else {
            $info.EnvironmentVariables.Remove('CONTROL_BRIDGE_TOKEN')
            $info.EnvironmentVariables.Remove('CONTROL_LOCAL_TOKEN')
            $info.EnvironmentVariables.Remove('CONTROL_REMOTE_URL')
        }
    }
    else {
        $info.EnvironmentVariables.Remove('REMOTE_URL')
        $info.EnvironmentVariables.Remove('CONTROL_BRIDGE_TOKEN')
        $info.EnvironmentVariables.Remove('CONTROL_REMOTE_URL')
        if (!$RemoteControl) { $info.EnvironmentVariables.Remove('CONTROL_LOCAL_TOKEN') }
    }
    [Diagnostics.Process]::Start($info)
}

function Test-BackendOwnsListener([int]$ChildId) {
    # Match a numeric endpoint, unconnected remote endpoint, and PID; state names can be localized.
    $pattern = '^\s*TCP\s+127\.0\.0\.1:' + $WsPort + '\s+0\.0\.0\.0:0\s+\S+\s+' + $ChildId + '\s*$'
    foreach ($line in (& "$env:SystemRoot/System32/netstat.exe" -ano -p tcp)) {
        if ($line -match $pattern) { return $true }
    }
    return $false
}

function Wait-Backend([Diagnostics.Process]$Child) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while (-not [ElevSafeLauncherConsole]::Requested -and $clock.Elapsed.TotalSeconds -lt $ReadyTimeoutSeconds) {
        if ($Child.HasExited) { throw "Backend exited before readiness (exit $($Child.ExitCode))." }
        $probe = New-Object System.Net.WebSockets.ClientWebSocket
        $cancel = New-Object System.Threading.CancellationTokenSource
        try {
            $remaining = [int]([TimeSpan]::FromSeconds($ReadyTimeoutSeconds).TotalMilliseconds - $clock.ElapsedMilliseconds)
            if ($remaining -le 0) { break }
            $cancel.CancelAfter([Math]::Min(500, $remaining))
            [void]$probe.ConnectAsync([uri]$localUrl, $cancel.Token).GetAwaiter().GetResult()
            if ((Test-BackendOwnsListener $Child.Id) -and -not $Child.HasExited) {
                try {
                    [void]$probe.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
                        'Readiness check complete', $cancel.Token).GetAwaiter().GetResult()
                }
                catch [System.Net.WebSockets.WebSocketException] { }
                catch [System.OperationCanceledException] { }
                return $true
            }
        }
        catch [System.Net.WebSockets.WebSocketException] { }
        catch [System.OperationCanceledException] { }
        finally {
            $probe.Dispose()
            $cancel.Dispose()
        }
        Start-Sleep -Milliseconds 100
    }
    if ([ElevSafeLauncherConsole]::Requested) { return $false }
    throw "Backend WebSocket readiness timed out on $localUrl (expected listener PID $($Child.Id))."
}

function Stop-OwnedChild([Diagnostics.Process]$Child, [string]$Name, [int]$GraceMs) {
    if ($null -eq $Child) { return }
    $childId = $Child.Id
    try {
        if (-not $Child.HasExited -and -not $Child.WaitForExit($GraceMs)) {
            try { $Child.Kill() }
            catch [System.InvalidOperationException] { if (-not $Child.HasExited) { throw } }
            if (-not $Child.WaitForExit(2000)) { throw 'Process did not exit after termination.' }
            [Console]::WriteLine("[$Name] PID $childId terminated (exit $($Child.ExitCode)).")
        }
        else { [Console]::WriteLine("[$Name] PID $childId exited ($($Child.ExitCode)).") }
    }
    catch {
        [Console]::Error.WriteLine("[$Name] Cleanup failed for PID $childId.")
        $script:cleanupFailed = $true
    }
    finally { $Child.Dispose() }
}

$exe = Resolve-RequiredFile $BackendExe 'Backend executable'
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($ExpectedBackendSha256 -and $hash -ne $ExpectedBackendSha256) { throw 'Backend SHA256 does not match the selected build.' }
$bridgeScript = Resolve-RequiredFile $bridgeScript 'Bridge script'
$null = Resolve-RequiredFile (Join-Path $PSScriptRoot 'relay/transport.js') 'Shared transport'
$node = Get-Command node.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $node) { throw 'Node.js was not found on PATH.' }
$version = & $node.Source --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^v(\d+)\.' -or [int]$Matches[1] -lt 20) {
    throw 'Node.js 20 or newer is required.'
}
$null = & $node.Source -e 'try { require(process.argv[1]); } catch { process.exitCode = 1; }' $bridgeScript
if ($LASTEXITCODE -ne 0) { throw 'Cannot load Bridge dependencies. Check deploy/bridge/node_modules/ws and the shared transport.' }

$remote = $null
if (-not [Uri]::TryCreate($env:REMOTE_URL, [UriKind]::Absolute, [ref]$remote) -or
    $remote.Scheme -notin @('ws', 'wss') -or -not $remote.Host -or
    $remote.UserInfo -or $remote.Fragment -or $remote.AbsolutePath -cne '/ingest') {
    throw 'Set REMOTE_URL to a ws/wss Relay ingest URL without user info or a fragment.'
}

$wsArgs = @('--ws-bind', '127.0.0.1', '--ws-port', "$WsPort")
if ($Mode -eq 'Mock') {
    if ($EmulatorDoorSim -or $PSBoundParameters.ContainsKey('SignalConfig') -or $PSBoundParameters.ContainsKey('RadarHost') -or
        $PSBoundParameters.ContainsKey('RadarPort')) { throw 'Radar options require -Mode Radar.' }
    $backendArgs = @('--ws-mock', '--ws-duration', "$MockDuration") + $wsArgs
    if ($DoorSim) { $backendArgs += '--ws-door-sim' }
}
else {
    if ($DoorSim -or $PSBoundParameters.ContainsKey('MockDuration')) { throw 'Mock options require -Mode Mock.' }
    if (-not $SignalConfig) { throw '-SignalConfig is required in Radar mode.' }
    $cfg = Resolve-RequiredFile $SignalConfig 'Signal configuration'
    $backendArgs = @($RadarHost, "$RadarPort", '--signal-config', $cfg, '--websocket') + $wsArgs
    if ($EmulatorDoorSim) { $backendArgs += '--emulator-door-sim' }
}
if ($RemoteControl) {
    if (!($DoorSim -or $EmulatorDoorSim)) { throw '-RemoteControl requires -DoorSim or -EmulatorDoorSim.' }
    $controlUrl = $null
    if (-not [Uri]::TryCreate($env:CONTROL_REMOTE_URL, [UriKind]::Absolute, [ref]$controlUrl) -or
        $controlUrl.Scheme -cne 'wss' -or !$controlUrl.Host -or $controlUrl.AbsolutePath -cne '/control-link' -or
        $controlUrl.UserInfo -or $controlUrl.Query -or $controlUrl.Fragment) {
        throw 'Set CONTROL_REMOTE_URL to an authenticated Relay wss control-link endpoint.'
    }
    if ($env:CONTROL_BRIDGE_TOKEN -cnotmatch '^[a-zA-Z0-9_-]{32,256}$' -or
        $env:CONTROL_LOCAL_TOKEN -cnotmatch '^[a-zA-Z0-9_-]{32,256}$' -or
        $env:CONTROL_BRIDGE_TOKEN -ceq $env:CONTROL_LOCAL_TOKEN) {
        throw 'Set independent Bridge and loopback control credentials.'
    }
    $backendArgs += '--ws-remote-control'
}

$listeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
if ($listeners | Where-Object { $_.Port -eq $WsPort -and $_.Address.ToString() -in @('127.0.0.1','0.0.0.0') }) {
    throw "WebSocket port $WsPort is already listening on loopback or all IPv4 addresses."
}

# Keep the console handler on a native callback thread, outside the PowerShell runspace.
# Clearing the inherited ignore flag also enables Ctrl+C for children sharing this console.
if (-not ('ElevSafeLauncherConsole' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ElevSafeLauncherConsole
{
    private delegate bool Handler(uint code);
    private static readonly Handler OnControl = Handle;
    private static volatile bool requested;
    public static bool Requested { get { return requested; } }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleCtrlHandler(Handler handler, bool add);

    public static bool Register()
    {
        requested = false;
        return SetConsoleCtrlHandler(null, false) && SetConsoleCtrlHandler(OnControl, true);
    }

    public static void Unregister() { SetConsoleCtrlHandler(OnControl, false); }

    private static bool Handle(uint code)
    {
        if (code != 0 && code != 1) return false;
        requested = true;
        return true;
    }
}
"@
}

$backend = $null
$bridge = $null
$cleanupFailed = $false
$handlerRegistered = [ElevSafeLauncherConsole]::Register()
try {
    Write-Host "[launcher] Backend $exe SHA256=$hash"
    if ($Mode -eq 'Mock') { Write-Host '[launcher] Mock mode (software simulation only).' }
    $backend = Start-Child $exe $backendArgs
    Write-Host "[launcher] Backend PID $($backend.Id). Waiting for $localUrl."
    if (-not (Wait-Backend $backend) -or [ElevSafeLauncherConsole]::Requested) { return }
    Write-Host '[launcher] Local WebSocket ready. Starting Bridge. Waiting for remote connection.'
    $bridge = Start-Child $node.Source @($bridgeScript) -Bridge
    Write-Host "[launcher] Bridge PID $($bridge.Id). Bridge logs local and remote connections separately."
    while (-not [ElevSafeLauncherConsole]::Requested -and -not $backend.HasExited -and -not $bridge.HasExited) {
        Start-Sleep -Milliseconds 100
    }
    if ([ElevSafeLauncherConsole]::Requested) { Write-Host '[launcher] Console interrupt received.' }
    elseif ($backend.HasExited) {
        if ($backend.ExitCode -ne 0) { throw "Backend exited with code $($backend.ExitCode)." }
        if ($bridge.HasExited -and $bridge.ExitCode -ne 0) { throw "Bridge exited with code $($bridge.ExitCode)." }
        Write-Host '[launcher] Backend finished normally.'
    }
    else { throw "Bridge exited while Backend was running (exit $($bridge.ExitCode))." }
}
finally {
    # Ctrl+C can cancel the PowerShell pipeline. Cleanup uses .NET and console output directly.
    Stop-OwnedChild $bridge 'bridge' 750
    Stop-OwnedChild $backend 'backend' 750
    if ($handlerRegistered) { [ElevSafeLauncherConsole]::Unregister() }
    if ($backend) {
        try {
            $remaining = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
            $inUse = [bool]($remaining | Where-Object { $_.Port -eq $WsPort -and $_.Address.ToString() -in @('127.0.0.1','0.0.0.0') })
            [Console]::WriteLine("[launcher] Loopback port $WsPort listening after cleanup: $inUse.")
        }
        catch { [Console]::Error.WriteLine('[launcher] Could not verify port state after cleanup.') }
    }
}
if ($cleanupFailed) { throw 'One or more child processes could not be cleaned up.' }
