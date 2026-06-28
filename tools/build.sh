#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${1:-all}"
build_type="${2:-debug}"

case "${target}" in
    all|original|refactor)
        ;;
    *)
        echo "usage: tools/build.sh [all|original|refactor] [debug|release]" >&2
        exit 1
        ;;
esac

case "${build_type}" in
    debug|release)
        ;;
    *)
        echo "usage: tools/build.sh [all|original|refactor] [debug|release]" >&2
        exit 1
        ;;
esac

cmake --preset "linux-${build_type}" -S "${repo_root}"
cmake --build --preset "${target}-${build_type}"
