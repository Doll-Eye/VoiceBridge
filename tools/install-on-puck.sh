#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

case "$#:${1-}" in
    0:) mode=install ;;
    1:--check) mode=check ;;
    1:--uninstall) mode=uninstall ;;
    *) echo "Usage: $0 [--check|--uninstall]" >&2; exit 2 ;;
esac

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
stage=
if [[ "$mode" == install ]]; then
    exe_hash=$(shasum -a 256 build/VoiceBridge.exe | cut -d ' ' -f1)
    ini_hash=$(shasum -a 256 VoiceBridge.ini | cut -d ' ' -f1)
    [[ "$exe_hash" =~ ^[[:xdigit:]]{64}$ && "$ini_hash" =~ ^[[:xdigit:]]{64}$ ]]
    stage=VoiceBridge-stage-$(uuidgen)
    [[ "$stage" =~ ^VoiceBridge-stage-[[:xdigit:]-]{36}$ ]]

    cleanup_stage() {
        local result=$?
        if ! remote <<PS
\$ErrorActionPreference = 'Stop'
\$staging = Join-Path \$env:LOCALAPPDATA '$stage'
if (Test-Path -LiteralPath \$staging) { Remove-Item -LiteralPath \$staging -Recurse -Force }
PS
        then
            echo "Could not remove PUCK's temporary folder $stage." >&2
            if [[ "$result" == 0 ]]; then result=1; fi
        fi
        exit "$result"
    }
    trap cleanup_stage EXIT

    destination=$(remote <<PS | tr -d '\r'
\$ErrorActionPreference = 'Stop'
\$staging = Join-Path \$env:LOCALAPPDATA '$stage'
New-Item -ItemType Directory -Path \$staging | Out-Null
\$staging.Replace([char]92, [char]47)
PS
)
    [[ "$destination" =~ ^[A-Za-z]:/[^[:cntrl:]]*/$stage$ ]] || {
        echo 'PUCK did not return an installation path.' >&2
        exit 1
    }
    scp -q -o BatchMode=yes -o ConnectTimeout=10 \
        build/VoiceBridge.exe VoiceBridge.ini "puck:$destination/"
fi

{
    printf "\$mode = '%s'\n\$exeHash = '%s'\n\$iniHash = '%s'\n\$stage = '%s'\n" \
        "$mode" "$exe_hash" "$ini_hash" "$stage"
    cat tools/install.ps1
} | remote | tr -d '\r'
