#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
case "${1:-}" in
    clean) rm -rf -- build; exit ;;
    ''|tests) ;;
    *) echo "Usage: $0 [clean|tests]" >&2; exit 2 ;;
esac
exec make -f src/front/build.mk SPEAKER="${SPEAKER:-log}" "${1:-all}"
