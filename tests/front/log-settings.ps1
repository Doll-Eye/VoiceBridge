$iniPath = Join-Path $TestDirectory 'VoiceBridge.ini'
$originalIni = [IO.File]::ReadAllBytes($iniPath)
try {
    Reset-Log
    @'
[log]
level=debug
text=1invalid
'@ | Set-Content -LiteralPath $iniPath -Encoding ASCII
    $logPath = Join-Path $TestDirectory 'bridge.log'
    $seed = 'x' * (1MB - 11)
    [IO.File]::WriteAllText($logPath, ($seed + "`n"))
    # A reader that did not grant delete sharing delays rotation. Entries
    # appended during that interval must follow the seed into the old file.
    $reader = [IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
    try {
        $supervisor = Start-Test VoiceBridge
        $server = Wait-Serving $supervisor
    } finally {
        $reader.Dispose()
    }
    Finish-Test (Start-Test stock) 'stock.txt'
    Assert-Exit (Start-Test VoiceBridge '--stop') 0
    Assert-Exit $supervisor 0
    Assert-Exit $server 0

    $old = @(Get-Content -Encoding UTF8 -LiteralPath bridge.log.old)
    if ($old.Count -lt 2 -or $old[0] -cne $seed) {
        throw 'The original log or entries appended while rotation was blocked are missing.'
    }
    $lines = @($old | Select-Object -Skip 1) + @(Read-BridgeLog)
    foreach ($writer in @("$($supervisor.Id) supervisor", "$($server.Id) server")) {
        foreach ($message in @('Started', 'Stopping')) {
            if (@($lines | Select-String -Pattern " $writer info $message").Count -ne 1) {
                throw "Expected exactly one $message entry for $writer after rotation."
            }
        }
    }
    if ($lines | Select-String -Pattern 'Speak:|Stock plain' -Quiet) {
        throw 'Malformed text setting enabled speech logging.'
    }
    'Both roles kept complete start and stop entries through rotation. A malformed text setting kept speech private.'
} finally {
    [IO.File]::WriteAllBytes($iniPath, $originalIni)
}

$TestDirectory = Join-Path $TestDirectory 'Settings With Spaces'
[void] (New-Item -ItemType Directory -Path $TestDirectory)
Copy-Item -LiteralPath VoiceBridge.exe -Destination $TestDirectory
Copy-Item -LiteralPath vb-fake-nvda.exe -Destination (Join-Path $TestDirectory 'VB Test NVDA.exe')
Push-Location -LiteralPath $TestDirectory
try {
    @'
[nvda]
processNames=   VB Test NVDA.exe ;
'@ | Set-Content -LiteralPath VoiceBridge.ini -Encoding ASCII
    $fake = Start-Test 'VB Test NVDA'
    Wait-Result 'fake.txt' 'fixture ready'
    Assert-StandAside "NVDA process $($fake.Id) is running"
    End-Fixture $fake
    'The bridge read its settings from a directory with spaces and matched an executable name containing spaces.'
} finally {
    Pop-Location
}
