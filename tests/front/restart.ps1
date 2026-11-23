param([string] $TestDirectory, [scriptblock] $WaitNoConflicts)
$ErrorActionPreference = 'Stop'
if ((Get-Process -Id $PID).SessionId -ne 0 -or
    ($TestDirectory -ine 'C:\VoiceBridge-test\front' -and $TestDirectory -notmatch '\AC:\\VoiceBridge-front-test-[a-f0-9]{32}\z')) {
    throw 'Expected the unique front test directory in SSH session zero.'
}
if (Get-Process -Name nvda,nvda_uiAccess,nvda_noUIAccess,VoiceBridge,restart-bridge -ErrorAction SilentlyContinue |
    Where-Object { $_.SessionId -eq 0 }) { throw 'An NVDA or VoiceBridge process already exists in the SSH session.' }
@'
[log]
level=debug
text=0
[nvda]
startsAtSignIn=no
'@ | Set-Content -LiteralPath (Join-Path $TestDirectory 'VoiceBridge.ini') -Encoding ASCII

foreach ($modeName in 'stalled', 'crash', 'recurring-stall', 'poison', 'slow-probe', 'healthy-run') {
    & $WaitNoConflicts
    $logPath = Join-Path $TestDirectory 'bridge.log'
    Remove-Item -LiteralPath $logPath -ErrorAction SilentlyContinue
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $TestDirectory 'restart-driver.exe'
    $info.WorkingDirectory = $TestDirectory
    $info.Arguments = $modeName
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.EnvironmentVariables['VOICEBRIDGE_FRONT_TEST_DIRECTORY'] = $TestDirectory
    $info.EnvironmentVariables['VOICEBRIDGE_TEST_ONLY_JENNY'] = '1'
    $info.EnvironmentVariables.Remove('VOICEBRIDGE_TEST_ONLY_DAVID')
    $child = [Diagnostics.Process]::Start($info)
    try {
        if (!$child.WaitForExit(75000)) { throw "The $modeName restart fixture timed out." }
        $summary = Join-Path $TestDirectory "restart-$modeName.txt"
        if (Test-Path -LiteralPath $summary) { Get-Content -LiteralPath $summary }
        if ($child.ExitCode -ne 0 -or !(Test-Path -LiteralPath $summary)) {
            if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath }
            throw "The $modeName restart fixture failed with exit $($child.ExitCode)."
        }
    } finally {
        if (!$child.HasExited) {
            $child.Kill()
            [void] $child.WaitForExit(5000)
        }
        $child.Dispose()
    }
}
