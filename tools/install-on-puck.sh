#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

mode=install
if [[ $# -gt 0 ]]; then
    case "$1" in
        --check) mode=check ;;
        --uninstall) mode=uninstall ;;
        *) echo "Usage: $0 [--check|--uninstall]" >&2; exit 2 ;;
    esac
fi

# Pass the script on stdin so it fits within Windows OpenSSH's command limit.
bootstrap=$(iconv -f UTF-8 -t UTF-16LE <<'PS' | base64 | tr -d '\n'
try {
    & ([scriptblock]::Create([Console]::In.ReadToEnd()))
} catch {
    [Console]::Error.WriteLine($_.ToString())
    exit 1
}
PS
)
remote() {
    ssh -T -o BatchMode=yes -o ConnectTimeout=10 puck \
        "powershell -NoProfile -NonInteractive -EncodedCommand $bootstrap"
}

exe_hash=
ini_hash=
if [[ "$mode" == install ]]; then
    exe_hash=$(shasum -a 256 build/VoiceBridge.exe | cut -d ' ' -f1)
    ini_hash=$(shasum -a 256 VoiceBridge.ini | cut -d ' ' -f1)
    destination=$(remote <<'PS' | tr -d '\r'
$ErrorActionPreference = 'Stop'
$folder = Join-Path $env:LOCALAPPDATA 'VoiceBridge'
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$folder.Replace('\', '/')
PS
)
    [[ "$destination" =~ ^[A-Za-z]:/[^[:cntrl:]]*/VoiceBridge$ ]] || {
        echo 'PUCK did not return an installation path.' >&2
        exit 1
    }
    scp -q -o BatchMode=yes -o ConnectTimeout=10 \
        build/VoiceBridge.exe "puck:$destination/VoiceBridge.exe.new"
    scp -q -o BatchMode=yes -o ConnectTimeout=10 \
        VoiceBridge.ini "puck:$destination/VoiceBridge.ini.new"
fi

{
    printf "\$mode = '%s'\n\$exeHash = '%s'\n\$iniHash = '%s'\n" "$mode" "$exe_hash" "$ini_hash"
    cat tools/install.ps1
} | remote | tr -d '\r'
