#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<'USAGE'
usage: tools/count-lines.sh [PATH...]

Count source lines in the project. With no PATH arguments, the script scans
the repository's main source, test, Lua, CMake, config, and tools paths.

The report groups files by language and shows physical blank, comment, code,
and total lines. Generated and external-code directories are skipped.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

default_paths=(
    "${repo_root}/CMakeLists.txt"
    "${repo_root}/CMakePresets.json"
    "${repo_root}/apps"
    "${repo_root}/benchmarks"
    "${repo_root}/cmake"
    "${repo_root}/configs"
    "${repo_root}/include"
    "${repo_root}/lua"
    "${repo_root}/src"
    "${repo_root}/tests"
    "${repo_root}/tools"
)

search_paths=()

if (($# == 0)); then
    for path in "${default_paths[@]}"; do
        [[ -e "${path}" ]] && search_paths+=("${path}")
    done
else
    for arg in "$@"; do
        if [[ "${arg}" = /* && -e "${arg}" ]]; then
            search_paths+=("${arg}")
        elif [[ -e "${PWD}/${arg}" ]]; then
            search_paths+=("${PWD}/${arg}")
        elif [[ -e "${repo_root}/${arg}" ]]; then
            search_paths+=("${repo_root}/${arg}")
        else
            echo "count-lines: path not found: ${arg}" >&2
            exit 1
        fi
    done
fi

if ((${#search_paths[@]} == 0)); then
    echo "count-lines: no input paths found" >&2
    exit 1
fi

file_kind() {
    local path="$1"
    local base="${path##*/}"

    case "${base}" in
        CMakeLists.txt)
            printf '%s\n' cmake
            return
            ;;
    esac

    case "${path}" in
        *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.ipp)
            printf '%s\n' c_cpp
            ;;
        *.cmake)
            printf '%s\n' cmake
            ;;
        *.lua)
            printf '%s\n' lua
            ;;
        *.sh)
            printf '%s\n' shell
            ;;
        *.json)
            printf '%s\n' json
            ;;
        *)
            printf '%s\n' other
            ;;
    esac
}

kind_label() {
    case "$1" in
        c_cpp) printf '%s\n' 'c/c++' ;;
        *) printf '%s\n' "$1" ;;
    esac
}

declare -a kind_order=()
declare -A seen=()
declare -A files=()
declare -A blanks=()
declare -A comments=()
declare -A codes=()
declare -A totals=()

ensure_kind() {
    local kind="$1"
    if [[ -z "${seen[${kind}]+x}" ]]; then
        seen["${kind}"]=1
        kind_order+=("${kind}")
        files["${kind}"]=0
        blanks["${kind}"]=0
        comments["${kind}"]=0
        codes["${kind}"]=0
        totals["${kind}"]=0
    fi
}

count_file() {
    local kind="$1"
    local path="$2"

    awk -v kind="${kind}" '
function trim(s) {
    sub(/^[[:space:]]+/, "", s)
    sub(/[[:space:]]+$/, "", s)
    return s
}

function classify_c_like(line, s, code_seen, marker, before) {
    s = line
    if (trim(s) == "") {
        return "blank"
    }

    while (1) {
        if (in_c_block) {
            if (match(s, /\*\//)) {
                s = substr(s, RSTART + RLENGTH)
                in_c_block = 0
                continue
            }
            return code_seen ? "code" : "comment"
        }

        if (match(s, /\/[/*]/)) {
            before = substr(s, 1, RSTART - 1)
            if (trim(before) != "") {
                code_seen = 1
            }

            marker = substr(s, RSTART, 2)
            if (marker == "//") {
                return code_seen ? "code" : "comment"
            }

            s = substr(s, RSTART + 2)
            if (match(s, /\*\//)) {
                s = substr(s, RSTART + RLENGTH)
                continue
            }

            in_c_block = 1
            return code_seen ? "code" : "comment"
        }

        if (trim(s) != "") {
            code_seen = 1
        }
        return code_seen ? "code" : "comment"
    }
}

function classify_lua(line, s, code_seen, before) {
    s = line
    if (trim(s) == "") {
        return "blank"
    }

    while (1) {
        if (in_lua_block) {
            if (match(s, /\]\]/)) {
                s = substr(s, RSTART + RLENGTH)
                in_lua_block = 0
                continue
            }
            return code_seen ? "code" : "comment"
        }

        if (match(s, /--/)) {
            before = substr(s, 1, RSTART - 1)
            if (trim(before) != "") {
                code_seen = 1
            }

            if (substr(s, RSTART, 4) == "--[[") {
                s = substr(s, RSTART + 4)
                if (match(s, /\]\]/)) {
                    s = substr(s, RSTART + RLENGTH)
                    continue
                }

                in_lua_block = 1
                return code_seen ? "code" : "comment"
            }

            return code_seen ? "code" : "comment"
        }

        if (trim(s) != "") {
            code_seen = 1
        }
        return code_seen ? "code" : "comment"
    }
}

function classify_hash(line, s) {
    s = trim(line)
    if (s == "") {
        return "blank"
    }
    return (s ~ /^#/) ? "comment" : "code"
}

function classify_plain(line, s) {
    s = trim(line)
    return (s == "") ? "blank" : "code"
}

{
    total += 1

    if (kind == "c_cpp") {
        bucket = classify_c_like($0)
    } else if (kind == "lua") {
        bucket = classify_lua($0)
    } else if (kind == "shell" || kind == "cmake") {
        bucket = classify_hash($0)
    } else {
        bucket = classify_plain($0)
    }

    if (bucket == "blank") {
        blank += 1
    } else if (bucket == "comment") {
        comment += 1
    } else {
        code += 1
    }
}

END {
    printf "%d %d %d %d\n", blank + 0, comment + 0, code + 0, total + 0
}
' "${path}"
}

while IFS= read -r -d '' file; do
    kind="$(file_kind "${file}")"
    ensure_kind "${kind}"

    read -r blank_count comment_count code_count total_count < <(count_file "${kind}" "${file}")

    files["${kind}"]=$((files["${kind}"] + 1))
    blanks["${kind}"]=$((blanks["${kind}"] + blank_count))
    comments["${kind}"]=$((comments["${kind}"] + comment_count))
    codes["${kind}"]=$((codes["${kind}"] + code_count))
    totals["${kind}"]=$((totals["${kind}"] + total_count))
done < <(
    find "${search_paths[@]}" \
        \( -type d \( \
            -name .git -o \
            -name build -o \
            -name third_party -o \
            -name .cache -o \
            -name .cmake \
        \) -prune \) -o \
        -type f \( \
            -name CMakeLists.txt -o \
            -name '*.cmake' -o \
            -name '*.c' -o \
            -name '*.cc' -o \
            -name '*.cpp' -o \
            -name '*.cxx' -o \
            -name '*.h' -o \
            -name '*.hh' -o \
            -name '*.hpp' -o \
            -name '*.hxx' -o \
            -name '*.ipp' -o \
            -name '*.lua' -o \
            -name '*.sh' -o \
            -name '*.json' \
        \) -print0 | sort -z
)

printf '%-12s %8s %10s %10s %10s %10s\n' language files blank comment code total
printf '%-12s %8s %10s %10s %10s %10s\n' '------------' '--------' '----------' '----------' '----------' '----------'

total_files=0
total_blanks=0
total_comments=0
total_codes=0
total_lines=0

for kind in "${kind_order[@]}"; do
    printf '%-12s %8d %10d %10d %10d %10d\n' \
        "$(kind_label "${kind}")" \
        "${files[${kind}]}" \
        "${blanks[${kind}]}" \
        "${comments[${kind}]}" \
        "${codes[${kind}]}" \
        "${totals[${kind}]}"

    total_files=$((total_files + files["${kind}"]))
    total_blanks=$((total_blanks + blanks["${kind}"]))
    total_comments=$((total_comments + comments["${kind}"]))
    total_codes=$((total_codes + codes["${kind}"]))
    total_lines=$((total_lines + totals["${kind}"]))
done

printf '%-12s %8s %10s %10s %10s %10s\n' '------------' '--------' '----------' '----------' '----------' '----------'
printf '%-12s %8d %10d %10d %10d %10d\n' total "${total_files}" "${total_blanks}" "${total_comments}" "${total_codes}" "${total_lines}"
