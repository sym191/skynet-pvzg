#!/usr/bin/env bash
set -euo pipefail
export TEAM_REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export TEAM_BUILD_ROOT="${TEAM_BUILD_ROOT:-${TEAM_REPO_ROOT}/build/original}"
export TEAM_INVITE_TTL="${TEAM_INVITE_TTL:-60}"
export TEAM_BATTLE_SERVICE="${TEAM_BATTLE_SERVICE:-}"
export TEAM_POLICY_MODULE="${TEAM_POLICY_MODULE:-team.policy}"
team_data_dir="${TEAM_DATA_DIR:-${TEAM_REPO_ROOT}/var/team}"
export TEAM_STATE_PATH="${TEAM_STATE_PATH:-${team_data_dir}/state.json}"
if [[ "${1:-}" == "--help" ]]; then
    cat <<'HELP'
Usage: ./start-server.sh [--build-only]
Starts original Skynet and the internal .team service.
First run builds original Skynet if needed. No client listener is started.
Environment: TEAM_INVITE_TTL, TEAM_DATA_DIR, TEAM_STATE_PATH, TEAM_BUILD_ROOT,
TEAM_BUILD_JOBS, TEAM_BATTLE_SERVICE, TEAM_POLICY_MODULE.
All path overrides must be absolute. See service/README.md.
HELP
    exit 0
fi
if [[ $# -gt 1 || ( $# -eq 1 && "$1" != "--build-only" ) ]]; then
    echo "Unknown option; use --help" >&2
    exit 2
fi
for team_path in "$TEAM_REPO_ROOT" "$TEAM_BUILD_ROOT" "$TEAM_STATE_PATH"; do
    if [[ "$team_path" != /* || "$team_path" == *'"'* || "$team_path" == *'\'* || "$team_path" == *$'\n'* ]]; then
        echo "Paths must be absolute and contain no quotes, backslashes or newlines" >&2
        exit 2
    fi
done
if [[ ! "$TEAM_INVITE_TTL" =~ ^[1-9][0-9]{0,8}$ ]]; then
    echo "TEAM_INVITE_TTL must be a positive integer of at most 9 digits" >&2
    exit 2
fi
if [[ ! -f "$TEAM_REPO_ROOT/third_party/skynet/Makefile" ]]; then
    git -C "$TEAM_REPO_ROOT" submodule update --init --recursive third_party/skynet
fi
team_needs_build=false
for artifact in bin/skynet cservice/snlua.so cservice/logger.so cservice/harbor.so cservice/gate.so luaclib/skynet.so; do
    [[ -f "$TEAM_BUILD_ROOT/$artifact" ]] || team_needs_build=true
done
if [[ "$team_needs_build" == true || ! -x "$TEAM_BUILD_ROOT/bin/skynet" || "${1:-}" == "--build-only" ]]; then
    mkdir -p "$TEAM_BUILD_ROOT/bin" "$TEAM_BUILD_ROOT/cservice" "$TEAM_BUILD_ROOT/luaclib"
    make -C "$TEAM_REPO_ROOT/third_party/skynet" linux -j"${TEAM_BUILD_JOBS:-2}" \
        SKYNET_BUILD_PATH="$TEAM_BUILD_ROOT/bin" CSERVICE_PATH="$TEAM_BUILD_ROOT/cservice" \
        LUA_CLIB_PATH="$TEAM_BUILD_ROOT/luaclib"
fi
[[ "${1:-}" != "--build-only" ]] || exit 0
umask 077
mkdir -p -- "$(dirname -- "$TEAM_STATE_PATH")"
# One process must own a durable state file. The lock stays held across exec.
exec 9>"${TEAM_STATE_PATH}.lock"
flock -n 9 || { echo "A team server already owns this state file" >&2; exit 1; }
echo "Starting original Skynet and .team service"
exec "$TEAM_BUILD_ROOT/bin/skynet" "$TEAM_REPO_ROOT/service/config.lua"
