<#
.SYNOPSIS
    Two-instance LAN smoke test for the Samogonki/Moonshine Runners net race.

.DESCRIPTION
    Starts one copy of the game as the session host and another as the client,
    both entering the race directly (-netrace), and drives the whole handshake
    to a verdict:

        host:   net race start, host  ->  roster 2  ->  start tick  ->  racing
        client: net race start, client ->  sealed   ->  assigned slot -> racing

    Every expected step is a log marker with a deadline, so a hang is reported
    as a hang at a named step rather than as "it did not work". A copy that
    exits by itself is reported with its exit code, which is how a crash shows
    up: 0xC0000005 (access violation), 0xC0000409 (fast-fail / stack cookie),
    0xE06D7363 (unhandled C++ exception) and so on.

    With -Gdb the copies run under gdb through tools/gdb-net-*.gdb, and a crash
    leaves a backtrace next to the log. That is the mode to use when the test
    fails, because the exit code alone does not say where in the game it died.

.PARAMETER BinDir
    Directory the game runs from. It holds the resources, and that is where the
    game writes net-host.log / net-client.log.

.PARAMETER OutDir
    Where the collected logs and gdb traces are copied. Defaults to
    <BinDir>/net-test-out.

.PARAMETER StartTimeoutSec
    How long to wait for each handshake step.

.PARAMETER RaceSec
    How long both copies must then survive with the race running.

.PARAMETER JoinAddr
    Address the client joins. Defaults to loopback, which is what a test on one
    machine needs.

.EXAMPLE
    pwsh -File tools/net-race-test.ps1

.EXAMPLE
    # When it fails, rerun under gdb to catch the stack.
    pwsh -File tools/net-race-test.ps1 -Gdb
#>
[CmdletBinding()]
param(
    [string] $BinDir          = '',
    [string] $OutDir          = '',
    [int]    $StartTimeoutSec = 90,
    [int]    $RaceSec         = 30,
    [string] $JoinAddr        = '127.0.0.1',
    [switch] $Gdb,
    [string] $GdbExe          = 'C:/msys64/ucrt64/bin/gdb.exe',
    [string] $ExeName         = 'moonshine-runners.exe'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# $PSScriptRoot is not reliable inside a parameter default, so the layout is
# resolved once here instead.
$ToolsDir = Split-Path -Parent $PSCommandPath
if (-not $BinDir) { $BinDir = Join-Path $ToolsDir '..\bin' }
$BinDir = (Resolve-Path $BinDir).Path
if (-not $OutDir) { $OutDir = Join-Path $BinDir 'net-test-out' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$HostLog   = Join-Path $BinDir 'net-host.log'
$ClientLog = Join-Path $BinDir 'net-client.log'
$HostOut   = Join-Path $OutDir 'host.gdb.txt'
$ClientOut = Join-Path $OutDir 'client.gdb.txt'

$script:Failures = [System.Collections.Generic.List[string]]::new()

function Note([string] $msg) {
    Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $msg)
}

function Fail([string] $msg) {
    $script:Failures.Add($msg)
    Write-Host "  FAIL  $msg" -ForegroundColor Red
}

function Ok([string] $msg) {
    Write-Host "  ok    $msg" -ForegroundColor Green
}

# Wait for a regular expression to appear in a file. Returns the matching line,
# or $null on timeout. Tail-reads, so a large log is fine.
function Wait-Log([string] $Path, [string] $Pattern, [int] $TimeoutSec) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            $line = Get-Content -Path $Path -ErrorAction SilentlyContinue |
                    Select-String -Pattern $Pattern | Select-Object -First 1
            if ($line) { return $line.Line }
        }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function New-Args([string[]] $Extra) {
    return @('-lang','ru') + $Extra
}

function Start-Target([string] $Role, [string[]] $GameArgs, [string] $GdbFile, [string] $GdbOut) {
    if ($Gdb) {
        $gdbArgs = @('-q','-batch','-x',(Join-Path $ToolsDir $GdbFile),'--args',$ExeName) + $GameArgs
        $p = Start-Process -FilePath $GdbExe -ArgumentList $gdbArgs -WorkingDirectory $BinDir `
                           -PassThru -RedirectStandardOutput $GdbOut `
                           -RedirectStandardError ($GdbOut + '.err')
    }
    else {
        $p = Start-Process -FilePath (Join-Path $BinDir $ExeName) -ArgumentList $GameArgs `
                           -WorkingDirectory $BinDir -PassThru
    }
    Note "$Role started (pid $($p.Id))$(if ($Gdb) { ' under gdb' })"
    return $p
}

# A copy that exited on its own, before we asked it to, crashed. Reports the
# exit code and flags the well-known NTSTATUS values by name so a failure is
# readable without a decoder ring.
function Test-Alive([string] $Role, $Proc) {
    if ($null -eq $Proc -or -not $Proc.HasExited) { return $true }

    $code = [uint32]$Proc.ExitCode
    $name = switch ($code) {
        0xC0000005 { 'access violation' }
        0xC0000409 { 'fast-fail / stack cookie' }
        0xC0000374 { 'heap corruption' }
        0xE06D7363 { 'unhandled C++ exception' }
        0xC000001D { 'illegal instruction' }
        0xC0000094 { 'integer divide by zero' }
        default    { 'exit' }
    }
    Fail "$Role died on its own: $name (0x$($code.ToString('X8')))"
    return $false
}

function Stop-Target($Proc) {
    if ($null -eq $Proc -or $Proc.HasExited) { return }
    # Kill the tree: under gdb the game is a child, and gdb on its own would
    # leave it running.
    & taskkill.exe /T /F /PID $Proc.Id 2>&1 | Out-Null
    Start-Sleep -Milliseconds 500
}

# ---------------- run ----------------

Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ExeName)) -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
foreach ($f in @($HostLog,$ClientLog,$HostOut,$ClientOut)) { Remove-Item $f -Force -ErrorAction SilentlyContinue }

Note "bin   $BinDir"
Note "out   $OutDir"
Note "mode  $(if ($Gdb) { 'gdb' } else { 'direct' })"
if ($Gdb) {
    foreach ($need in @($GdbExe, (Join-Path $ToolsDir 'gdb-net-host.gdb'), (Join-Path $ToolsDir 'gdb-net-client.gdb'))) {
        if (-not (Test-Path $need)) { throw "gdb mode needs $need" }
    }
}

$hostProc = $null
$clientProc = $null

try {
    # 1. Host up and into the race.
    $hostProc = Start-Target 'host' (New-Args @('-nethost','-netrace')) 'gdb-net-host.gdb' $HostOut
    if (-not (Wait-Log $HostLog 'net race start, host' $StartTimeoutSec)) {
        Fail "host never reached the race (no 'net race start, host' in $StartTimeoutSec s)"
    } else { Ok 'host entered the race' }

    # 2. Client up and into the race.
    $clientProc = Start-Target 'client' (New-Args @('-netjoin',$JoinAddr,'-netrace')) 'gdb-net-client.gdb' $ClientOut
    if (-not (Wait-Log $ClientLog 'net race start, client' $StartTimeoutSec)) {
        Fail "client never reached the race (no 'net race start, client' in $StartTimeoutSec s)"
    } else { Ok 'client entered the race' }

    # 3. The roster sees both, on both sides.
    if (-not (Wait-Log $HostLog 'roster now 2 player' $StartTimeoutSec)) {
        Fail "host never saw two racers (handshake failed)"
    } else { Ok 'host roster: 2 players' }

    if (-not (Wait-Log $ClientLog 'roster now 2 player' $StartTimeoutSec)) {
        Fail "client never saw the full roster"
    } else { Ok 'client roster: 2 players' }

    if (-not (Wait-Log $ClientLog 'assigned slot [0-9]+' $StartTimeoutSec)) {
        Fail "client was never given a slot"
    } else { Ok 'client assigned a slot' }

    # 4. The agreed start tick, and both actually running.
    if (-not (Wait-Log $HostLog 'start tick [0-9]+ reached, time running' $StartTimeoutSec)) {
        Fail "host never started the race"
    } else { Ok 'host started the race' }

    if (-not (Wait-Log $ClientLog 'start tick [0-9]+ reached, time running' $StartTimeoutSec)) {
        Fail "client never started the race"
    } else { Ok 'client started the race' }

    # 5. Both survive a race window and keep reporting live entities. A copy
    #    that crashes mid-race is caught here rather than at the next step.
    Note "racing for $RaceSec s..."
    $end = (Get-Date).AddSeconds($RaceSec)
    $hostSawEntities = $false
    $clientSawEntities = $false
    while ((Get-Date) -lt $end) {
        if (-not (Test-Alive 'host' $hostProc)) { break }
        if (-not (Test-Alive 'client' $clientProc)) { break }
        if (-not $hostSawEntities   -and (Wait-Log $HostLog   'entities [0-9]+ projectile' 1)) { $hostSawEntities   = $true }
        if (-not $clientSawEntities -and (Wait-Log $ClientLog 'entities [0-9]+ projectile' 1)) { $clientSawEntities = $true }
    }
    if ($hostSawEntities)   { Ok 'host reported live entities' }   else { Fail 'host reported no live entities during the race' }
    if ($clientSawEntities) { Ok 'client reported live entities' } else { Fail 'client reported no live entities during the race' }

    # 6. The field is actually driving. Three of the five racers are bots and
    #    have to move on their own, so a report below that means the AI or the
    #    input routing is broken even though the handshake looked fine.
    foreach ($side in @(@('host', $HostLog), @('client', $ClientLog))) {
        $name = $side[0]; $log = $side[1]; $moved = -1
        if (Test-Path $log) {
            $line = Get-Content $log -ErrorAction SilentlyContinue |
                    Select-String -Pattern '(\d+) of (\d+) racers moved' | Select-Object -Last 1
            if ($line) { $moved = [int]$line.Matches[0].Groups[1].Value }
        }
        if ($moved -lt 0)        { Fail "${name} never reported racer motion" }
        elseif ($moved -lt 3)    { Fail "${name}: only $moved racer(s) moved, expected the bots" }
        else                     { Ok "${name}: $moved racers moving" }
    }
}
finally {
    Stop-Target $clientProc
    Stop-Target $hostProc
}

# Collect whatever was produced, crash or not.
foreach ($pair in @(@($HostLog,'host.log'), @($ClientLog,'client.log'))) {
    if (Test-Path $pair[0]) { Copy-Item $pair[0] (Join-Path $OutDir $pair[1]) -Force }
}

if ($Gdb) {
    # In gdb mode a crash shows as gdb stopping on an exception; the backtrace
    # is what matters, so surface it instead of letting it sit in a file.
    foreach ($pair in @(@($HostOut,'host'), @($ClientOut,'client'))) {
        if (-not (Test-Path $pair[0])) { continue }
        $trace = Get-Content $pair[0] -ErrorAction SilentlyContinue
        if ($trace | Select-String -Pattern 'Program received signal|target exception|SIGSEGV|BACKTRACE') {
            if (-not ($trace | Select-String -Pattern 'target exception|received signal')) { continue }
            Fail "$($pair[1]) stopped in the debugger"
            $start = ($trace | Select-String '=== EXCEPTION BACKTRACE ===' | Select-Object -First 1).LineNumber
            if ($start) {
                Write-Host "---- $($pair[1]) backtrace ----"
                $trace[($start-1)..([Math]::Min($start + 24, $trace.Count - 1))] | ForEach-Object { Write-Host "  $_" }
            }
        }
    }
}

Write-Host ''
if ($script:Failures.Count -eq 0) {
    Write-Host 'RESULT: PASS' -ForegroundColor Green
    exit 0
}

Write-Host "RESULT: FAIL ($($script:Failures.Count) problem(s))" -ForegroundColor Red
foreach ($f in $script:Failures) { Write-Host "  - $f" }
Write-Host "logs: $OutDir"
exit 1
