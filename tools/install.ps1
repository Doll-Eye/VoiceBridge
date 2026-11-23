$ErrorActionPreference = 'Stop'
$name = 'VoiceBridge'
$folder = Join-Path $env:LOCALAPPDATA $name
$exe = Join-Path $folder 'VoiceBridge.exe'
$ini = Join-Path $folder 'VoiceBridge.ini'
$log = Join-Path $folder 'bridge.log'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$user = $identity.Name

function Get-BridgeTask {
    Get-ScheduledTask -TaskName $name -TaskPath '\' -ErrorAction SilentlyContinue
}

function Show-Status {
    $task = Get-BridgeTask
    if ($task) { "Task $name is $($task.State)." } else { "Task $name is absent." }
    Get-Content -Encoding UTF8 -LiteralPath $log -Tail 20 -ErrorAction SilentlyContinue |
        Where-Object { $_ -match '^\S+ \S+ \d+ (server|supervisor) (info|error) ' }
}

if ($mode -eq 'check') {
    Show-Status
    return
}

$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited

function Get-BridgeProcesses {
    Get-CimInstance -ClassName Win32_Process -Filter "Name = 'VoiceBridge.exe'" |
        Where-Object { $_.ExecutablePath -eq $exe }
}

function Stop-Bridge {
    if (!(Get-BridgeProcesses)) { return }
    # The stop event is session-local. Run --stop in the main task's desktop.
    $stopName = 'VoiceBridge-Stop-' + [Guid]::NewGuid().ToString('N')
    $action = New-ScheduledTaskAction -Execute $exe -Argument '--stop' -WorkingDirectory $folder
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $stopName -TaskPath '\' -Action $action -Principal $principal -Settings $settings | Out-Null
    try {
        $initialRun = (Get-ScheduledTaskInfo -TaskName $stopName -TaskPath '\').LastRunTime
        Start-ScheduledTask -TaskName $stopName -TaskPath '\'
        for ($i = 0; $i -lt 140; $i++) {
            $state = (Get-ScheduledTask -TaskName $stopName -TaskPath '\').State
            $status = Get-ScheduledTaskInfo -TaskName $stopName -TaskPath '\'
            if ($status.LastRunTime -ne $initialRun -and $state -eq 'Ready') {
                if ($status.LastTaskResult -ne 0) { throw "VoiceBridge --stop returned $($status.LastTaskResult)." }
                if (Get-BridgeProcesses) { throw 'A process from the installed VoiceBridge executable is still running.' }
                return
            }
            Start-Sleep -Milliseconds 250
        }
        throw 'VoiceBridge --stop did not complete in the signed-in Windows session.'
    } finally {
        try {
            Stop-ScheduledTask -TaskName $stopName -TaskPath '\' -ErrorAction SilentlyContinue
        } finally {
            Unregister-ScheduledTask -TaskName $stopName -TaskPath '\' -Confirm:$false
        }
    }
}

function Uninstall-Bridge {
    $task = Get-BridgeTask
    $enabled = $task -and $task.Settings.Enabled
    if ($enabled) { Disable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null }
    try {
        Stop-Bridge
        if ($task) { Unregister-ScheduledTask -TaskName $name -TaskPath '\' -Confirm:$false }
    } catch {
        if ($enabled) { Enable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null }
        throw
    }
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    'VoiceBridge was removed.'
}

function Confirm-Launch([datetime]$launched) {
    $pattern = '^(?<stamp>\S+ \S+) (?<id>\d+) (?<role>server|supervisor) info (Serving NvdaCtlr\.(?<session>\d+)\..+|Waiting for NVDA process (?<nvda>\d+))$'
    for ($i = 0; $i -lt 40; $i++) {
        $lines = @(foreach ($path in "$log.old", $log) {
            Get-Content -Encoding UTF8 -LiteralPath $path -Tail 40 -ErrorAction SilentlyContinue
        })
        if ($lines.Count -and (Get-BridgeTask).State -eq 'Running') {
            $processes = @(Get-BridgeProcesses | Where-Object { $_.CreationDate -ge $launched -and $_.SessionId -ne 0 })
            for ($j = $lines.Count - 1; $j -ge 0; $j--) {
                if (!($lines[$j] -match $pattern)) { continue }
                $id = [int]$Matches.id
                $stamp = [datetime]::MinValue
                if (![datetime]::TryParseExact($Matches.stamp, 'yyyy-MM-dd HH:mm:ss.fff',
                        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$stamp)) {
                    continue
                }
                $process = $processes | Where-Object { $_.ProcessId -eq $id -and $_.CreationDate.AddMilliseconds(-1) -le $stamp }
                if (!$process) { continue }
                if ($Matches.role -eq 'server' -and $Matches.session -eq $process.SessionId) {
                    $lines[$j]
                    return
                }
                if ($Matches.role -eq 'supervisor' -and $Matches.nvda) {
                    $nvda = Get-Process -Id ([int]$Matches.nvda) -ErrorAction SilentlyContinue
                    if ($nvda -and $nvda.SessionId -eq $process.SessionId) {
                        'VoiceBridge started and is waiting for NVDA to exit before it starts serving.'
                        return
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 250
    }
    Show-Status
    throw 'The task was started, but the log did not confirm a live server or a supervisor waiting for NVDA.'
}

function Install-Bridge {
    $staging = Join-Path $env:LOCALAPPDATA $stage
    $hashes = @{ $exe = $exeHash; $ini = $iniHash }
    foreach ($path in $hashes.Keys) {
        $source = Join-Path $staging ([IO.Path]::GetFileName($path))
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne $hashes[$path]) {
            throw "SHA-256 did not match for $source."
        }
    }

    $action = New-ScheduledTaskAction -Execute $exe -WorkingDirectory $folder
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $trigger.Delay = 'PT20S'
    $options = @{
        AllowStartIfOnBatteries = $true
        DontStopIfGoingOnBatteries = $true
        ExecutionTimeLimit = [TimeSpan]::Zero
        RestartCount = 3
        RestartInterval = New-TimeSpan -Minutes 1
        MultipleInstances = 'IgnoreNew'
    }
    $settings = New-ScheduledTaskSettingsSet @options
    $definition = @{
        TaskName = $name
        TaskPath = '\'
        Action = $action
        Trigger = $trigger
        Principal = $principal
        Settings = $settings
        Force = $true
    }

    $task = Get-BridgeTask
    $enabled = $task -and $task.Settings.Enabled
    $restart = $enabled -and $task.State -eq 'Running'
    if ($task) { $previousTask = Export-ScheduledTask -TaskName $name -TaskPath '\' }
    $backups = @{}
    $registering = $false
    $keepBackups = $false
    try {
        if ($enabled) { Disable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null }
        Stop-Bridge
        New-Item -ItemType Directory -Path $folder -Force | Out-Null
        # Keep the old pair until the replacement's launch is confirmed.
        foreach ($path in $exe, $ini) {
            $backup = $null
            if (Test-Path -LiteralPath $path) {
                $backup = "$path.$stage.bak"
                [IO.File]::Copy($path, $backup)
            }
            $backups[$path] = $backup
        }
        foreach ($path in $exe, $ini) {
            $source = Join-Path $staging ([IO.Path]::GetFileName($path))
            if ($backups[$path]) {
                [IO.File]::Replace($source, $path, [NullString]::Value)
            } else {
                [IO.File]::Move($source, $path)
            }
        }
        $registering = $true
        Register-ScheduledTask @definition | Out-Null
        $launched = Get-Date
        Start-ScheduledTask -TaskName $name -TaskPath '\'
        Confirm-Launch $launched
    } catch {
        $failure = $_
        try {
            if ($registering) {
                $current = Get-BridgeTask
                if ($current -and $current.Settings.Enabled) {
                    Disable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null
                }
                Stop-Bridge
            }
            foreach ($path in $backups.Keys) {
                if ($backups[$path]) {
                    [IO.File]::Copy($backups[$path], $path, $true)
                } elseif (Test-Path -LiteralPath $path) {
                    Remove-Item -LiteralPath $path -Force
                }
            }
            if ($registering) {
                if ($task) {
                    Register-ScheduledTask -TaskName $name -TaskPath '\' -Xml $previousTask -Force | Out-Null
                } elseif (Get-BridgeTask) {
                    Unregister-ScheduledTask -TaskName $name -TaskPath '\' -Confirm:$false
                }
            }
            if ($enabled) { Enable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null }
            if ($restart -and (Get-BridgeTask).State -ne 'Running') {
                Start-ScheduledTask -TaskName $name -TaskPath '\'
            }
        } catch {
            $keepBackups = $true
            throw "Install failed: $failure. Rollback failed: $_. Any file backups were kept in $folder."
        }
        throw $failure
    } finally {
        if (!$keepBackups) {
            foreach ($backup in $backups.Values) {
                if ($backup) { Remove-Item -LiteralPath $backup -Force }
            }
        }
    }
}

# All installer commands run in the same SSH session namespace. Downloads use
# separate staging directories, and only one install or uninstall mutates the task.
$mutex = [Threading.Mutex]::new($false, "Local\VoiceBridge.Install.$($identity.User.Value)")
$locked = $false
try {
    try {
        $locked = $mutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $locked = $true
    }
    if (!$locked) { throw 'Another VoiceBridge install or uninstall is in progress.' }
    if ($mode -eq 'install') {
        Install-Bridge
    } elseif ($mode -eq 'uninstall') {
        Uninstall-Bridge
    } else {
        throw "Unknown installer mode: $mode"
    }
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
