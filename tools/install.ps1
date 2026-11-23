$ErrorActionPreference = 'Stop'
$name = 'VoiceBridge'
$folder = Join-Path $env:LOCALAPPDATA $name
$exe = Join-Path $folder 'VoiceBridge.exe'
$ini = Join-Path $folder 'VoiceBridge.ini'
$log = Join-Path $folder 'bridge.log'
$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$task = Get-ScheduledTask -TaskName $name -TaskPath '\' -ErrorAction SilentlyContinue

function Show-Status {
    if ($task) { "Task $name is $($task.State)." } else { "Task $name is absent." }
    if (Test-Path -LiteralPath $log) {
        Get-Content -Encoding UTF8 -LiteralPath $log -Tail 20 |
            Where-Object { $_ -match '^\S+ \S+ \d+ (server|supervisor) (info|error) ' }
    }
}

if ($mode -eq 'check') {
    Show-Status
    return
}

$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
function Stop-Bridge {
    if (!(Test-Path -LiteralPath $exe)) { return }
    # SSH is in session zero. Run --stop in the same session as the main task.
    $stopName = 'VoiceBridge-Stop-' + [Guid]::NewGuid().ToString('N')
    $action = New-ScheduledTaskAction -Execute $exe -Argument '--stop' -WorkingDirectory $folder
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $stopName -TaskPath '\' -Action $action -Principal $principal -Settings $settings | Out-Null
    try {
        $initialRun = (Get-ScheduledTaskInfo -TaskName $stopName -TaskPath '\').LastRunTime
        Start-ScheduledTask -TaskName $stopName -TaskPath '\'
        for ($i = 0; $i -lt 80; $i++) {
            $status = Get-ScheduledTaskInfo -TaskName $stopName -TaskPath '\'
            $state = (Get-ScheduledTask -TaskName $stopName -TaskPath '\').State
            if ($status.LastRunTime -ne $initialRun -and $state -eq 'Ready') {
                if ($status.LastTaskResult -ne 0) { throw "VoiceBridge --stop returned $($status.LastTaskResult)." }
                return
            }
            Start-Sleep -Milliseconds 250
        }
        throw 'VoiceBridge --stop did not complete in the signed-in Windows session.'
    } finally {
        Unregister-ScheduledTask -TaskName $stopName -TaskPath '\' -Confirm:$false
    }
}

if ($mode -eq 'uninstall') {
    if ($task) { Disable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null }
    Stop-Bridge
    if ($task) { Unregister-ScheduledTask -TaskName $name -TaskPath '\' -Confirm:$false }
    if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    'VoiceBridge was removed.'
    return
}

$hashes = @{ "$exe.new" = $exeHash; "$ini.new" = $iniHash }
foreach ($staged in $hashes.Keys) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $staged).Hash -ne $hashes[$staged]) {
        throw "SHA-256 did not match for $staged."
    }
}
if ($task) { Disable-ScheduledTask -TaskName $name -TaskPath '\' | Out-Null }
Stop-Bridge
foreach ($path in $exe, $ini) {
    if (Test-Path -LiteralPath $path) {
        [IO.File]::Replace("$path.new", $path, [NullString]::Value)
    } else {
        [IO.File]::Move("$path.new", $path)
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
Register-ScheduledTask @definition | Out-Null
Start-ScheduledTask -TaskName $name -TaskPath '\'
for ($i = 0; $i -lt 40; $i++) {
    if (Test-Path -LiteralPath $log) {
        $lines = @(Get-Content -Encoding UTF8 -LiteralPath $log -Tail 30 |
                    Select-String ' (\d+) server info Serving (NvdaCtlr\..+)$')
        if ($lines.Count) {
            $serverId = [int] $lines[-1].Matches[0].Groups[1].Value
            $server = Get-Process -Id $serverId -ErrorAction SilentlyContinue
            if ($server -and $server.SessionId -ne 0 -and $server.Path -eq $exe) {
                $lines[-1].Line
                return
            }
        }
    }
    Start-Sleep -Milliseconds 250
}
$task = Get-ScheduledTask -TaskName $name -TaskPath '\'
Show-Status
throw 'The task was started, but the log has no live serving process. A running NVDA can keep VoiceBridge waiting.'
