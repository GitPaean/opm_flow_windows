#!/usr/bin/env bash
# Build flow-gui against an existing OPM source/build tree. Does not build flow.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
opm_root="$(cd "${repo_dir}/../opm" 2>/dev/null && pwd || true)"
build_dir="${repo_dir}/build-gui-macos"
flow_path=""
launch=false

usage() {
    printf 'Usage: %s [--opm-root DIR] [--flow FILE] [--build-dir DIR] [--launch]\n' "$0"
    printf 'Defaults: sibling opm checkout, codex-release build, build-gui-macos output.\n'
}

while (($#)); do
    case "$1" in
        --opm-root|--flow|--build-dir)
            if (($# < 2)); then usage >&2; exit 2; fi
            case "$1" in
                --opm-root) opm_root="$2" ;;
                --flow) flow_path="$2" ;;
                --build-dir) build_dir="$2" ;;
            esac
            shift 2 ;;
        --launch) launch=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != Darwin ]]; then
    printf 'This script requires macOS.\n' >&2
    exit 1
fi
if [[ -z "$flow_path" ]]; then
    flow_path="${opm_root}/codex-release/opm-simulators/bin/flow"
fi
opm_source="${opm_root}/src/opm-common"
opm_build="${opm_root}/codex-release/opm-common"
if [[ ! -f "${opm_source}/opm/io/eclipse/ESmry.hpp" || ! -f "${opm_build}/lib/libopmcommon.a" ]]; then
    printf 'Missing opm-common source or library under %s; pass --opm-root.\n' "$opm_root" >&2
    exit 1
fi
if [[ ! -x "$flow_path" ]]; then
    printf 'Missing executable flow at %s; pass --flow.\n' "$flow_path" >&2
    exit 1
fi

qt_prefix="$(brew --prefix qt)"
cmake -S "${repo_dir}/flow-gui" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt_prefix" \
    -DFLOWGUI_OPM_SOURCE_DIR="$opm_source" \
    -DFLOWGUI_OPM_BUILD_DIR="$opm_build" \
    -DFLOWGUI_FMT_LIB=fmt::fmt \
    -DFLOWGUI_DEFAULT_SIMULATOR="$flow_path"
cmake --build "$build_dir" --parallel
"${build_dir}/flow-gui.app/Contents/MacOS/flow-gui" --version
if "$launch"; then
    open "${build_dir}/flow-gui.app"
else
    printf 'Ready: open "%s/flow-gui.app"\n' "$build_dir"
fi
