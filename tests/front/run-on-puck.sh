#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mode="${1:-full}"
case "$mode" in rpc|full) ;; *) echo "Usage: $0 [rpc|full]" >&2; exit 2 ;; esac
SPEAKER=log ./build.sh tests
files=(build/{VoiceBridge,stock,prism-client}.exe
    ../gta11y-revo/final/nvdaControllerClient64.dll
    ../d4-assist-fork/third_party/prism/prism.dll tests/front/run.ps1)
if [[ "$mode" == full ]]; then
    files+=(build/{client64,client32,lifecycle,vb-fake-nvda,stuck-bridge,settings-test}.exe tests/front/log-settings.ps1 tools/install.ps1)
fi
for file in "${files[@]}"; do
    [[ -f "$file" ]] || { echo "Missing $file" >&2; exit 1; }
done

setup=$(../puckps.sh 20 <<'PS'
$ErrorActionPreference = 'Stop'
if ((Get-Process -Id $PID).SessionId -ne 0) { throw 'Tests require SSH in session zero.' }
New-Item -ItemType Directory -Path 'C:\VoiceBridge-test\front' | Out-Null
'VOICEBRIDGE_TEST_DIRECTORY_CREATED'
PS
)
if ! grep -qx 'VOICEBRIDGE_TEST_DIRECTORY_CREATED' <<<"$setup"; then
    printf '%s\n' "$setup" >&2
    exit 1
fi
cleanup() {
    local result=$? report
    report=$(../puckps.sh 20 <<'PS'
$ErrorActionPreference = 'Stop'
$path = 'C:\VoiceBridge-test\front'
if ((Get-Process -Id $PID).SessionId -ne 0) { throw 'Expected SSH session zero.' }
$left = @(Get-CimInstance Win32_Process | Where-Object {
    $_.SessionId -eq 0 -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith(($path + [IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase)
})
if ($left.Count) { throw 'Test executables are still running after the SSH test session closed.' }
if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
if (Test-Path -LiteralPath $path) { throw 'Test directory still exists.' }
'VOICEBRIDGE_TEST_DIRECTORY_REMOVED'
PS
)
    printf '%s\n' "$report"
    grep -qx 'VOICEBRIDGE_TEST_DIRECTORY_REMOVED' <<<"$report" || exit 1
    exit "$result"
}
trap cleanup EXIT
scp -q -o BatchMode=yes -o ConnectTimeout=10 "${files[@]}" 'puck:C:/VoiceBridge-test/front/'
output=$(../puckps.sh 180 <<PS
\$ErrorActionPreference = 'Stop'
\$path = 'C:\VoiceBridge-test\front'
try {
    \$script = [scriptblock]::Create([IO.File]::ReadAllText("\$path\run.ps1"))
    & \$script $mode \$path
    'VOICEBRIDGE_TESTS_PASSED'
} catch {
    [Console]::Error.WriteLine(\$_.ToString())
    [Console]::Error.WriteLine(\$_.ScriptStackTrace)
}
PS
)
printf '%s\n' "$output"
grep -qx 'VOICEBRIDGE_TESTS_PASSED' <<<"$output"
