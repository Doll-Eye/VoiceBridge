#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
harness=build-b/front-suite
stage="$harness/stage"
executables=(VoiceBridge stock prism-client lifecycle tolk-client client64 client32 vb-fake-nvda stuck-bridge settings-test signin-bridge)
support=(nvdaControllerClient64.dll Tolk.dll prism.dll run.ps1 log-settings.ps1 install.ps1)
restart_files=(restart-bridge.exe restart-driver.exe restart.ps1)

prepare_front() {
    mkdir -p "$stage"
    rm -f -- "$harness/ready"
    make -j1 -f src/front/build.mk BUILD="$harness/build" SPEAKER=log tests
    for file in "${executables[@]}"; do
        cp "$harness/build/$file.exe" "$stage/"
    done
    cp ../gta11y-revo/final/{nvdaControllerClient64,Tolk}.dll \
        ../d4-assist-fork/third_party/prism/prism.dll tests/front/{run,log-settings}.ps1 tools/install.ps1 "$stage/"
    : > "$harness/ready"
}

prepare_restart() {
    mkdir -p "$stage"
    rm -f -- "$harness/restart-ready"
    make -j1 -f src/front/build.mk BUILD="$harness/build" SPEAKER=log restart-tests
    cp "$harness/build/"{restart-bridge,restart-driver}.exe tests/front/restart.ps1 "$stage/"
    : > "$harness/restart-ready"
}

action="${1:-full}"
case "$action" in
    prepare) prepare_front; prepare_restart; exit ;;
    prepare-restart) prepare_restart; exit ;;
    run) mode="${2:-full}" ;;
    full) prepare_front; prepare_restart; mode=full ;;
    rpc) prepare_front; mode=rpc ;;
    restart) prepare_restart; mode=restart ;;
    clean) mode=full ;;
    *)
        echo "Usage: $0 [full|rpc|restart|prepare|prepare-restart|run [full|rpc|restart]|clean TEST_ID]" >&2
        exit 2
        ;;
esac

if [[ "$action" != clean ]]; then
    case "$mode" in
        full|rpc) [[ -f "$harness/ready" ]] || { echo 'Prepare the front suite first.' >&2; exit 1; } ;;
        restart) ;;
        *) echo 'Expected full, rpc, or restart.' >&2; exit 2 ;;
    esac
    if [[ "$mode" != rpc && ! -f "$harness/restart-ready" ]]; then
        echo 'Prepare the restart tests first.' >&2
        exit 1
    fi
fi

host=puck
if [[ "$action" == clean ]]; then
    test_id="${2:-}"
else
    test_id=$(uuidgen | tr '[:upper:]' '[:lower:]' | tr -d '-')
fi
[[ "$test_id" =~ ^[a-f0-9]{32}$ ]] || { echo 'Could not obtain a test ID.' >&2; exit 1; }
remote="C:/VoiceBridge-front-test-$test_id"
ssh_options=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=3)

puck_control() {
    local timeout=120
    if [[ "$1" == run ]]; then timeout=0; fi
    {
        printf "\$Action = '%s'\n\$TestId = '%s'\n\$Mode = '%s'\n" "$1" "$test_id" "$mode"
        printf 'try {\n& {\n'
        cat tests/front/puck-control.ps1
        cat <<'POWERSHELL'
} 2>&1 | ForEach-Object { [string]$_ -replace '[^\x00-\x7F]', '' }
} catch {
    [string]$_ -replace '[^\x00-\x7F]', ''
    exit 1
}
POWERSHELL
    } | ../puckps.sh "$timeout"
}

cleanup() {
    local result=$? report
    trap - EXIT
    if ! report=$(puck_control clean) || ! grep -qx 'VOICEBRIDGE_FRONT_REMOVED' <<<"$report"; then
        printf 'Cleanup of %s could not be verified.\n%s\n' "$remote" "$report" >&2
        exit 1
    fi
    printf '%s\n' "$report"
    exit "$result"
}

if [[ "$action" == clean ]]; then
    puck_control clean | grep -x 'VOICEBRIDGE_FRONT_REMOVED'
    exit
fi

# Cleanup rechecks a per-run marker, even if setup failed after creating the directory.
trap cleanup EXIT
setup=$(puck_control setup)
grep -qx 'VOICEBRIDGE_FRONT_CREATED' <<<"$setup" || { printf '%s\n' "$setup" >&2; exit 1; }
files=()
if [[ "$mode" != restart ]]; then
    for file in "${executables[@]}"; do files+=("$stage/$file.exe"); done
    for file in "${support[@]}"; do files+=("$stage/$file"); done
fi
if [[ "$mode" != rpc ]]; then
    for file in "${restart_files[@]}"; do files+=("$stage/$file"); done
fi
scp -q "${ssh_options[@]}" "${files[@]}" "$host:$remote/"
run_log="$harness/$test_id.log"
printf 'Front test output is being saved to %s.\n' "$run_log"
puck_control run | tee "$run_log"
grep -qx 'VOICEBRIDGE_FRONT_PASSED_AND_REMOVED' "$run_log"
trap - EXIT
