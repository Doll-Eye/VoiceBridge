#!/bin/bash
set -eo pipefail
cd "$(dirname "$0")"
if [[ ${1:-} == clean ]]; then
    if [[ -d build-b ]]; then
        find build-b -mindepth 1 -delete
    fi
    exit 0
fi
if [[ $# != 0 ]]; then
    echo "Usage: $0 [clean]" >&2
    exit 2
fi

compiler=/opt/homebrew/bin/x86_64-w64-mingw32-g++
flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -MMD -MP)
read -r -a libs < src/voice/LIBS.txt
sources=(src/voice/*.cpp)
objects=()
# Compile both production configurations separately from the test hooks.
for mode in plain unicode test; do
    mkdir -p "build-b/$mode"
    defines=()
    if [[ $mode != plain ]]; then
        defines=(-DUNICODE -D_UNICODE)
    fi
    mode_sources=("${sources[@]}")
    if [[ $mode == test ]]; then
        mode_sources+=(tests/voice/*.cpp)
    fi
    for source in "${mode_sources[@]}"; do
        extra=()
        if [[ $mode == test && $source != tests/voice/sapi-exit.cpp ]]; then
            extra=(-DVB_VOICE_TEST)
        fi
        object="build-b/$mode/$(basename "${source%.cpp}").o"
        rebuild=false
        if [[ ! -f $object || ! -f ${object%.o}.d || $source -nt $object || $0 -nt $object ]]; then
            rebuild=true
        else
            while IFS= read -r dependency; do
                if [[ ! -f $dependency || $dependency -nt $object ]]; then
                    rebuild=true
                    break
                fi
            done < <(awk '
                NR == 1 { sub(/^[^:]*:/, "") }
                { continued = sub(/\\$/, ""); for (i = 1; i <= NF; i++) print $i }
                !continued { exit }
            ' "${object%.o}.d")
        fi
        if $rebuild; then
            "$compiler" "${flags[@]}" "${defines[@]}" "${extra[@]}" -c "$source" -o "$object"
        fi
        if [[ $mode == test && $source == src/* ]]; then
            objects+=("$object")
        fi
    done
done

for source in tests/voice/*.cpp; do
    name=$(basename "${source%.cpp}")
    output="build-b/$name.exe"
    inputs=("build-b/test/$name.o")
    link_libs=(-lsapi -lole32)
    if [[ $name != sapi-exit ]]; then
        inputs=("${objects[@]}" "${inputs[@]}")
        link_libs=("${libs[@]}" -lpsapi)
    fi
    rebuild=false
    if [[ ! -f $output || $0 -nt $output || ( $name != sapi-exit && src/voice/LIBS.txt -nt $output ) ]]; then
        rebuild=true
    else
        for object in "${inputs[@]}"; do
            if [[ $object -nt $output ]]; then rebuild=true; break; fi
        done
    fi
    if $rebuild; then
        "$compiler" -static -s -mwindows "${inputs[@]}" -o "$output" "${link_libs[@]}"
    fi
done
