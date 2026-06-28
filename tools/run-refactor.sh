#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_type="${SKPVZG_BUILD_TYPE:-debug}"
build_dir="${repo_root}/build/linux-${build_type}"
skynet_bin="${build_dir}/refactor/bin/skynet"
config_path="${build_dir}/generated/configs/skynet_refactor.lua"

if [[ ! -x "${skynet_bin}" || ! -f "${config_path}" ]]; then
    "${repo_root}/tools/build.sh" refactor "${build_type}"
fi

exec "${skynet_bin}" "${config_path}"
