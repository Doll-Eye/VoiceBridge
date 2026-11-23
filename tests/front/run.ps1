param([ValidateSet('rpc','full')] [string] $Mode, [string] $TestDirectory)
$ErrorActionPreference = 'Stop'
if ((Get-Process -Id $PID).SessionId -ne 0) { throw 'Expected SSH session zero.' }
Set-Location -LiteralPath $TestDirectory
@'
[log]
level=debug
text=1
[nvda]
processNames=nvda.exe;nvda_uiAccess.exe;nvda_noUIAccess.exe;VB-FAKE-NVDA.exe;VoiceBridge.exe
'@ | Set-Content -LiteralPath VoiceBridge.ini -Encoding ASCII
$children = [Collections.Generic.List[Diagnostics.Process]]::new()

function Start-Test([string] $Name, [string] $Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $TestDirectory "$Name.exe"
    $info.WorkingDirectory = $TestDirectory
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $child = [Diagnostics.Process]::Start($info)
    $children.Add($child)
    $child
}

function Finish-Test($Child, [string] $Result) {
    if (!$Child.WaitForExit(10000)) { throw "Test timed out: $Result" }
    if (Test-Path -LiteralPath $Result) { Get-Content -LiteralPath $Result }
    if ($Child.ExitCode -ne 0) { throw "Test failed: $Result (exit $($Child.ExitCode))" }
    if (!(Test-Path -LiteralPath $Result)) { throw "No result: $Result" }
}

function Read-BridgeLog {
    if (Test-Path bridge.log) { Get-Content -Encoding UTF8 -LiteralPath bridge.log }
}

function Wait-Serving($Started, [int] $Previous = 0) {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $lines = @(Read-BridgeLog | Select-String -Pattern ' (\d+) server info Serving (NvdaCtlr\..+)$')
        if ($lines.Count) {
            $match = $lines[-1].Matches[0]
            $serverId = [int] $match.Groups[1].Value
            if ($serverId -ne $Previous) {
                $server = Get-Process -Id $serverId -ErrorAction SilentlyContinue
                if ($server -and $server.SessionId -eq 0 -and
                    $server.Path -eq (Join-Path $TestDirectory 'VoiceBridge.exe')) {
                    return $server
                }
            }
        }
        if ($Started.HasExited) {
            Read-BridgeLog | ForEach-Object { [Console]::Error.WriteLine($_) }
            throw "Bridge exited early: $($Started.ExitCode)"
        }
        Start-Sleep -Milliseconds 40
    }
    throw 'Bridge did not start serving.'
}

function Check-LogTail([string[]] $Expected, [string] $Filter) {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $lines = @(Read-BridgeLog | ForEach-Object {
            if ($_ -match $Filter) { $Matches[0] }
        })
        if ($lines.Count -ge $Expected.Count) { break }
        Start-Sleep -Milliseconds 20
    }
    if ($lines.Count -lt $Expected.Count) { throw 'Missing controller calls in the log' }
    $tail = @($lines | Select-Object -Last $Expected.Count)
    for ($i = 0; $i -lt $Expected.Count; $i++) {
        if ($tail[$i] -cne $Expected[$i]) { throw "Log mismatch at $i (length $($tail[$i].Length))" }
    }
}

function Assert-Exit($Child, [int] $Code) {
    if (!$Child.WaitForExit(5000)) { throw "Process $($Child.Id) did not exit" }
    if ($Child.ExitCode -ne $Code) {
        throw "Process $($Child.Id) exited with $($Child.ExitCode), expected $Code"
    }
}

function End-Fixture($Fixture) {
    if (!$Fixture.HasExited) { $Fixture.Kill() }
    if (!$Fixture.WaitForExit(5000)) { throw 'Fixture did not exit' }
}

function Wait-Result([string] $File, [string] $Marker) {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ((Test-Path $File) -and (Select-String -Path $File -Pattern $Marker -Quiet)) { return }
        Start-Sleep -Milliseconds 20
    }
    throw "Fixture did not become ready: $File"
}

function Reset-Log {
    Remove-Item -LiteralPath 'bridge.log','bridge.log.old' -Force -ErrorAction SilentlyContinue
}

function Assert-StandAside([string] $Reason) {
    Assert-Exit (Start-Test VoiceBridge '--serve') 3
    if (!(Read-BridgeLog | Select-String -Pattern "Standing aside because $Reason" -Quiet)) {
        throw "Missing stand-aside reason: $Reason"
    }
}

try {
    $server = Start-Test VoiceBridge '--serve'
    [void] (Wait-Serving $server)
    Finish-Test (Start-Test stock) 'stock.txt'
    Check-LogTail @('Speak: Stock plain', ('Speak: Caf' + [char]0xE9 + ' d' + [char]0xE9 + 'j' + [char]0xE0 + ' vu ' + [char]0x4E2D + [char]0x6587), ('Speak: ' + ('x' * 4000)), 'Speak: <b>5 < 6 & 7', 'Cancel') '(Speak: .*|Cancel)$'
    if (@(Select-String -Path bridge.log -Pattern 'Speak: ').Count -ne 4) { throw 'Expected exactly four logged stock phrases' }
    'Stock speech log matched all four nonempty phrases in order.'
    Finish-Test (Start-Test prism-client) 'prism.txt'
    Check-LogTail @('Cancel', 'Speak: PRISM plain', 'IsSpeaking: false', 'Cancel') '(Speak: .*|IsSpeaking: .*|Cancel)$'
    'PRISM speech, cancel and isSpeaking all reached the silent speaker.'
    if ($Mode -eq 'rpc') {
        Assert-Exit (Start-Test VoiceBridge '--stop') 0
        Assert-Exit $server 0
        return
    }

    Finish-Test (Start-Test client64) 'client64.txt'
    Check-LogTail @('Speak: WIDL x64', 'Cancel', 'IsSpeaking: false', ('Speak: ' + ('y' * 100000))) '(Speak: .*|IsSpeaking: .*|Cancel)$'
    Finish-Test (Start-Test client32) 'client32.txt'
    Check-LogTail @('Speak: WIDL x86', 'Cancel', 'IsSpeaking: false') '(Speak: .*|IsSpeaking: .*|Cancel)$'
    'Both WIDL clients matched the speech logs. The 100,001-unit phrase was cut to 100,000.'

    Finish-Test (Start-Test lifecycle "quit $($server.Id)") 'lifecycle.txt'
    Assert-Exit $server 0

    Reset-Log
    $server = Start-Test VoiceBridge '--serve'
    [void] (Wait-Serving $server)
    Finish-Test (Start-Test lifecycle "endsession $($server.Id)") 'lifecycle.txt'
    Assert-Exit $server 0

    Reset-Log
    Finish-Test (Start-Test lifecycle 'window') 'lifecycle.txt'

    foreach ($clash in 'window','endpoint','process') {
        Reset-Log
        if ($clash -eq 'process') {
            $fixture = Start-Test vb-fake-nvda
            Wait-Result 'fake.txt' 'fixture ready'
            Assert-StandAside 'NVDA process'
        } else {
            Remove-Item lifecycle.txt
            $fixture = Start-Test lifecycle "hold-$clash"
            Wait-Result 'lifecycle.txt' 'Fixture ready'
            if ($clash -eq 'window') {
                Assert-StandAside 'the NVDA window already exists'
            } else {
                Assert-StandAside 'the controller answered'
            }
        }
        End-Fixture $fixture
        "Standing aside at start passed for $clash (exit 3)."
    }

    Reset-Log
    $supervisor = Start-Test VoiceBridge
    $server = Wait-Serving $supervisor
    $duplicateTimer = [Diagnostics.Stopwatch]::StartNew()
    Assert-Exit (Start-Test VoiceBridge) 0
    $duplicateMs = $duplicateTimer.Elapsed.TotalMilliseconds
    if ($supervisor.HasExited -or $server.HasExited) { throw 'Original supervisor or server exited' }
    'Second supervisor exited in {0:F2} ms. The original supervisor and server remained running.' -f $duplicateMs
    Finish-Test (Start-Test lifecycle "appear $($server.Id)") 'lifecycle.txt'
    $server = Wait-Serving $supervisor $server.Id
    $stopTimer = [Diagnostics.Stopwatch]::StartNew()
    Assert-Exit (Start-Test VoiceBridge '--stop') 0
    $stopMs = $stopTimer.Elapsed.TotalMilliseconds
    if (!$supervisor.HasExited -or !$server.HasExited) { throw '--stop returned before both roles exited' }
    '--stop waited for both roles and returned in {0:F2} ms.' -f $stopMs

    Reset-Log
    Remove-Item lifecycle.txt
    $fixture = Start-Test lifecycle 'hold-window'
    Wait-Result 'lifecycle.txt' 'Fixture ready'
    $supervisor = Start-Test VoiceBridge
    $backoffDeadline = [Diagnostics.Stopwatch]::StartNew()
    do {
        $retries = @(Read-BridgeLog | Select-String -Pattern 'Retrying server after (\d+) ms')
        if ($retries.Count -ge 4) { break }
        Start-Sleep -Milliseconds 50
    } while ($backoffDeadline.Elapsed.TotalMilliseconds -lt 12000)
    $delays = @($retries | ForEach-Object { $_.Matches[0].Groups[1].Value })
    if (($delays -join ',') -ne '1000,2000,4000,8000') { throw "Unexpected back-off: $delays" }
    $intervals = for ($i = 1; $i -lt $retries.Count; $i++) {
        $a = [DateTime]::ParseExact($retries[$i - 1].Line.Substring(0,23), 'yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture)
        $b = [DateTime]::ParseExact($retries[$i].Line.Substring(0,23), 'yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture)
        $ms = ($b - $a).TotalMilliseconds
        if ($ms -lt [int]$delays[$i - 1] -or $ms -gt ([int]$delays[$i - 1] + 1000)) { throw "Unexpected retry interval: $ms ms" }
        $ms
    }
    Assert-Exit (Start-Test VoiceBridge '--stop') 0
    if (!$supervisor.HasExited) { throw 'Could not stop the supervisor during back-off' }
    End-Fixture $fixture
    'Back-off scheduled 1000, 2000, 4000, 8000 ms. Measured retry intervals: ' + ($intervals -join ', ') + ' ms.'

    Reset-Log
    $supervisor = Start-Test VoiceBridge
    $server = Wait-Serving $supervisor
    Finish-Test (Start-Test stock '--cost') 'stock.txt'
    Check-LogTail @('Speak: Timing') '(Speak: Timing)$'
    Start-Sleep -Milliseconds 750
    $supervisor.Refresh()
    $server.Refresh()
    $supervisorCpu = $supervisor.TotalProcessorTime
    $serverCpu = $server.TotalProcessorTime
    $idleTimer = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds 60
    $idleSeconds = $idleTimer.Elapsed.TotalSeconds
    $supervisor.Refresh()
    $server.Refresh()
    if ($server.HasExited -or $supervisor.HasExited) { throw 'Bridge exited during idle measurement' }
    $serverMs = ($server.TotalProcessorTime - $serverCpu).TotalMilliseconds
    $supervisorMs = ($supervisor.TotalProcessorTime - $supervisorCpu).TotalMilliseconds
    'Idle for {0:F2} s. CPU time {1:F2} ms total ({2:F2} supervisor, {3:F2} server).' -f $idleSeconds, ($serverMs + $supervisorMs), $supervisorMs, $serverMs
    'Private memory {0:F2} MiB supervisor and {1:F2} MiB server.' -f ($supervisor.PrivateMemorySize64 / 1MB), ($server.PrivateMemorySize64 / 1MB)
    Assert-Exit (Start-Test VoiceBridge '--stop') 0
    Assert-Exit $supervisor 0

    Reset-Log
    (Get-Content VoiceBridge.ini) -replace '^text=1$', 'text=0' | Set-Content VoiceBridge.ini -Encoding ASCII
    # Near the rotation limit, a startup entry must make the old file rotate.
    [IO.File]::WriteAllBytes((Join-Path $TestDirectory 'bridge.log'), [byte[]]::new(1MB - 10))
    [IO.File]::WriteAllText((Join-Path $TestDirectory 'bridge.log.old'), 'old copy to replace')
    $server = Start-Test VoiceBridge '--serve'
    $rotation = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Item bridge.log.old -ErrorAction SilentlyContinue).Length -ne (1MB - 10)) {
        if ($server.HasExited -or $rotation.ElapsedMilliseconds -gt 5000) { throw 'Log rotation did not complete at startup' }
        Start-Sleep -Milliseconds 30
    }
    [void] (Wait-Serving $server)
    Finish-Test (Start-Test stock) 'stock.txt'
    Assert-Exit (Start-Test VoiceBridge '--stop') 0
    Assert-Exit $server 0
    if ((Get-Item bridge.log.old).Length -ne (1MB - 10)) { throw 'Log did not rotate into its single backup' }
    if (Read-BridgeLog | Select-String -Pattern 'Speak:|Caf|Stock plain' -Quiet) { throw 'Speech leaked with text=0' }
    'Log rotation kept one backup, and debug level with text=0 did not record spoken text.'
} finally {
    foreach ($child in $children) {
        if (!$child.HasExited) {
            $child.Kill()
            [void] $child.WaitForExit(5000)
        }
        $child.Dispose()
    }
    # The supervisor creates an untracked server process. On any failed test,
    # clean up only exact executables in the owned SSH test directory.
    foreach ($proc in @(Get-Process VoiceBridge,vb-fake-nvda -ErrorAction SilentlyContinue)) {
        if ($proc.SessionId -eq 0 -and $proc.Path -and
            (Split-Path -Parent $proc.Path) -eq $TestDirectory) {
            $proc.Kill()
            [void] $proc.WaitForExit(5000)
        }
    }
}
