$ErrorActionPreference = 'Stop'
if ((Get-Process -Id $PID).SessionId -ne 0) { throw 'Expected SSH session zero.' }
if ($TestId -cnotmatch '\A[a-f0-9]{32}\z') { throw 'Expected a unique front test ID.' }
$testPath = "C:\VoiceBridge-front-test-$TestId"
$marker = Join-Path $testPath 'front-test-id.txt'
$prefix = $testPath + '\'

function Assert-OwnedDirectory {
    $directory = Get-Item -LiteralPath $testPath
    if (!$directory.PSIsContainer -or ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        [IO.File]::ReadAllText($marker) -cne $TestId) {
        throw "Directory ownership could not be verified for $testPath."
    }
}

function Get-PuckConflicts {
    $launchers = @(
        'Battle.net.exe', 'Battle.net Launcher.exe', 'Diablo IV Launcher.exe',
        'Rockstar Games Launcher.exe', 'RockstarService.exe', 'LauncherPatcher.exe',
        'PlayGTAV.exe', 'GTAVLauncher.exe', 'steam.exe', 'steamwebhelper.exe',
        'XboxPcApp.exe', 'GameLaunchHelper.exe'
    )
    foreach ($process in Get-CimInstance Win32_Process) {
        if ($process.ProcessId -eq $PID) { continue }
        if ($process.Name -in 'Diablo IV.exe', 'GTA5.exe', 'GTA5_Enhanced.exe' -or
            ($process.ExecutablePath -match '\\(?:steamapps\\common|XboxGames)\\' -and $process.Name -notin $launchers)) {
            "Game $($process.Name) (PID $($process.ProcessId))"
            continue
        }
        if ($process.Name -in 'VoiceBridge.exe', 'speaker-test.exe' -or
            $process.ExecutablePath -match '^C:\\VoiceBridge-[^\\]*\\') {
            "VoiceBridge test $($process.Name) (PID $($process.ProcessId))"
            continue
        }
        if ($process.Name -notin 'powershell.exe', 'pwsh.exe') { continue }
        $command = $process.CommandLine
        if ($command -match '(?:^|\s)-(?:EncodedCommand|enc|e)\s+"?([a-zA-Z0-9+/=]+)') {
            try { $command = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($Matches[1])) }
            catch [FormatException] { continue }
        }
        if ($command -match '(?:\\|\s)(?:item4-series|cold-jenny|cpu-load|final-series|series)\.ps1\b') {
            "VoiceBridge runner $($process.Name) (PID $($process.ProcessId))"
        }
    }
}

function Wait-NoConflicts {
    while ($true) {
        $conflicts = @(Get-PuckConflicts)
        if (!$conflicts.Count) { return }
        'Waiting two minutes for PUCK to be free. Detected: ' + ($conflicts -join ', ')
        Start-Sleep -Seconds 120
    }
}

function Remove-OwnedDirectory {
    if (!(Test-Path -LiteralPath $testPath)) { return }
    Assert-OwnedDirectory
    Set-Location -LiteralPath $env:SystemRoot
    [IO.Directory]::SetCurrentDirectory($env:SystemRoot)
    foreach ($info in @(Get-CimInstance Win32_Process -Filter 'SessionId = 0')) {
        if (!$info.ExecutablePath -or
            !$info.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $process = Get-Process -Id $info.ProcessId -ErrorAction SilentlyContinue
        if (!$process) { continue }
        try {
            [void] $process.Handle
            if (!$process.HasExited -and $process.SessionId -eq 0 -and
                [string]::Equals($process.Path, $info.ExecutablePath, [StringComparison]::OrdinalIgnoreCase)) {
                $process.Kill()
                if (!$process.WaitForExit(5000)) { throw "Test process $($process.Id) did not exit." }
            }
        } finally {
            $process.Dispose()
        }
    }
    $left = @(Get-CimInstance Win32_Process -Filter 'SessionId = 0' | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($left.Count) { throw 'A staged test executable is still running.' }
    Remove-Item -LiteralPath $testPath -Recurse -Force
    if (Test-Path -LiteralPath $testPath) { throw "Unable to remove $testPath." }
}

if ($Action -eq 'setup') {
    if (Test-Path -LiteralPath $testPath) { throw "The test directory already exists: $testPath." }
    [void] (New-Item -ItemType Directory -Path $testPath)
    try {
        [IO.File]::WriteAllText($marker, $TestId)
    } catch {
        Remove-Item -LiteralPath $testPath
        throw
    }
    'VOICEBRIDGE_FRONT_CREATED'
    return
}
function Try-TestMutex($mutex) {
    try {
        $mutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        # An abandoned mutex is owned by this thread after WaitOne.
        $true
    }
}

function Wait-ForTestSlot($mutex) {
    while ($true) {
        $reason = 'The shared PUCK test mutex is held.'
        if (Try-TestMutex $mutex) {
            $available = $false
            try {
                $conflicts = @(Get-PuckConflicts)
                $available = !$conflicts.Count
                if ($available) { return }
                $reason = $conflicts -join ', '
            } finally {
                if (!$available) { $mutex.ReleaseMutex() }
            }
        }
        "Waiting two minutes for PUCK to be free. Detected: $reason"
        Start-Sleep -Seconds 120
    }
}

if ($Action -notin 'run', 'clean') { throw 'Expected setup, run, or clean.' }
$stageMutex = [Threading.Mutex]::new($false, "Local\VoiceBridge.PuckFront.$TestId")
$stageLocked = $false
try {
    $stageLocked = Try-TestMutex $stageMutex
    if (!$stageLocked) { throw 'The front stage is still in use.' }
    if ($Action -eq 'run') {
        $runMutex = [Threading.Mutex]::new($false, 'Local\VoiceBridge.PuckTestRun')
        $runLocked = $false
        try {
            Assert-OwnedDirectory
            if ($Mode -notin 'full', 'rpc', 'restart') { throw 'Expected a full, RPC, or restart front suite.' }
            Wait-ForTestSlot $runMutex
            $runLocked = $true
            if ($Mode -eq 'restart') {
                $test = [scriptblock]::Create([IO.File]::ReadAllText((Join-Path $testPath 'restart.ps1')))
                & $test $testPath ${function:Wait-NoConflicts}
            } else {
                $test = [scriptblock]::Create([IO.File]::ReadAllText((Join-Path $testPath 'run.ps1')))
                & $test $Mode $testPath ${function:Wait-NoConflicts}
            }
        } finally {
            try {
                Remove-OwnedDirectory
            } finally {
                if ($runLocked) { $runMutex.ReleaseMutex() }
                $runMutex.Dispose()
            }
        }
        'VOICEBRIDGE_FRONT_PASSED_AND_REMOVED'
    } else {
        Remove-OwnedDirectory
        'VOICEBRIDGE_FRONT_REMOVED'
    }
} finally {
    if ($stageLocked) { $stageMutex.ReleaseMutex() }
    $stageMutex.Dispose()
}
