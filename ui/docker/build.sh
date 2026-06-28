#!/usr/bin/env bash
# build.sh — Build solakon-one-fritz-powerregulator-ui inside a Docker container.
#
# Mirrors the style of github.com/flunaras/solakon-one-ui's own docker/build.sh
# (and this repo's own docker/build.sh for the CLI/service binary), but builds
# the Qt6 desktop UI in ui/ as its own standalone CMake project (see
# ui/CMakeLists.txt's SOLAKON_UI_STANDALONE mode) so it produces its own
# RPM/DEB, independent of the CLI/service package.
#
# Usage:
#   ./docker/build.sh [--distro <alias>|all] [--build-type Release|Debug]
#
# Available distro aliases:
#   opensuse-tumbleweed-x86_64   openSUSE Tumbleweed, x86_64 (native build, produces RPM)
#   opensuse-tumbleweed-aarch64  openSUSE Tumbleweed, aarch64 (cross-compiled, produces RPM)
#   ubuntu-24.04-x86_64          Ubuntu 24.04 (noble), x86_64 (native build, produces DEB)
#   all                          Build all of the above (default)
#
# Build type options (default: Release):
#   Release                      Optimized build. Suitable for deployment.
#   Debug                        Debug symbols enabled (enables qDebug output).
#
# The script:
#   1. Builds (or reuses) a Docker image from ui/docker/Dockerfile.<name>
#   2. Runs cmake + ninja + cpack inside the container as the current host
#      user (--user $(id -u):$(id -g)) so output files are owned by you
#   3. Copies the resulting binary and package into a hierarchical directory
#      tree, alongside (but distinct from) the CLI/service tool's own tree:
#
#        build/<family>/<distro>/<arch>/   ← cmake / ninja / cpack work tree
#        out/<family>/<distro>/<arch>/     ← final binary and package
#
#        out/
#        ├── opensuse/
#        │   └── tumbleweed/
#        │       ├── x86_64/
#        │       │   ├── solakon-one-fritz-powerregulator-ui
#        │       │   └── solakon-one-fritz-powerregulator-ui-1.0.0-1.x86_64.rpm
#        │       └── aarch64/
#        │           ├── solakon-one-fritz-powerregulator-ui
#        │           └── solakon-one-fritz-powerregulator-ui-1.0.0-1.aarch64.rpm
#        └── ubuntu/
#            └── 24.04/
#                └── amd64/
#                    ├── solakon-one-fritz-powerregulator-ui
#                    └── solakon-one-fritz-powerregulator-ui_1.0.0-1_amd64.deb

set -euo pipefail

# ── Resolve paths ─────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UI_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_ROOT="$(cd "${UI_ROOT}/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────
DISTRO="all"
BUILD_TYPE="Release"

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --distro)
            DISTRO="$2"
            shift 2
            ;;
        --distro=*)
            DISTRO="${1#*=}"
            shift
            ;;
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --build-type=*)
            BUILD_TYPE="${1#*=}"
            shift
            ;;
        -h|--help)
            sed -n '/^# Usage:/,/^[^#]/{ /^[^#]/d; s/^# \{0,2\}//; p }' "$0"
            exit 0
            ;;
        *)
            echo "ERROR: Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

case "$DISTRO" in
    opensuse-tumbleweed-x86_64|opensuse-tumbleweed-aarch64|ubuntu-24.04-x86_64|all) ;;
    *)
        echo "ERROR: --distro must be one of:" >&2
        echo "  opensuse-tumbleweed-x86_64, opensuse-tumbleweed-aarch64," >&2
        echo "  ubuntu-24.04-x86_64, all" >&2
        exit 1
        ;;
esac

BUILD_TYPE_LOWER="${BUILD_TYPE,,}"
case "$BUILD_TYPE_LOWER" in
    release) BUILD_TYPE="Release" ;;
    debug)   BUILD_TYPE="Debug"   ;;
    *)
        echo "ERROR: --build-type must be Release or Debug (got: $BUILD_TYPE)" >&2
        exit 1
        ;;
esac

# ── Per-distro metadata ───────────────────────────────────────────────────────
dockerfile_for_distro() {
    case "$1" in
        opensuse-tumbleweed-x86_64)  echo "tumbleweed"           ;;
        opensuse-tumbleweed-aarch64) echo "tumbleweed-aarch64"   ;;
        ubuntu-24.04-x86_64)         echo "ubuntu-24.04"         ;;
    esac
}

family_for_distro() {
    case "$1" in
        opensuse-tumbleweed-x86_64)  echo "opensuse" ;;
        opensuse-tumbleweed-aarch64) echo "opensuse" ;;
        ubuntu-24.04-x86_64)         echo "ubuntu"   ;;
    esac
}

dir_name_for_distro() {
    case "$1" in
        opensuse-tumbleweed-x86_64)  echo "tumbleweed" ;;
        opensuse-tumbleweed-aarch64) echo "tumbleweed" ;;
        ubuntu-24.04-x86_64)         echo "24.04"      ;;
    esac
}

arch_for_distro() {
    case "$1" in
        opensuse-tumbleweed-aarch64) echo "aarch64" ;;
        *)                           echo "x86_64"  ;;
    esac
}

deb_arch_for_arch() {
    case "$1" in
        x86_64)  echo "amd64" ;;
        aarch64) echo "arm64" ;;
        *)       echo "$1"    ;;
    esac
}

pkg_type_for_distro() {
    case "$1" in
        opensuse-tumbleweed-x86_64)  echo "RPM" ;;
        opensuse-tumbleweed-aarch64) echo "RPM" ;;
        ubuntu-24.04-x86_64)         echo "DEB" ;;
    esac
}

# ── Build function ────────────────────────────────────────────────────────────
build_for_distro() {
    local distro="$1"
    local dockerfile_suffix
    dockerfile_suffix="$(dockerfile_for_distro "${distro}")"
    local image="solakon-one-fritz-powerregulator-ui-builder-${dockerfile_suffix}"
    local dockerfile="${SCRIPT_DIR}/Dockerfile.${dockerfile_suffix}"

    local version
    version="$(grep -A3 'project(solakon-one-fritz-powerregulator-ui' "${UI_ROOT}/CMakeLists.txt" \
                | grep -oP 'VERSION\s+\K\d+\.\d+\.\d+')"
    local release="1"

    local arch
    arch="$(arch_for_distro "${distro}")"
    local deb_arch
    deb_arch="$(deb_arch_for_arch "${arch}")"

    local family
    family="$(family_for_distro "${distro}")"
    local dir_name
    dir_name="$(dir_name_for_distro "${distro}")"

    local pkg_type
    pkg_type="$(pkg_type_for_distro "${distro}")"

    local dir_arch
    case "${pkg_type}" in
        DEB) dir_arch="${deb_arch}" ;;
        *)   dir_arch="${arch}"     ;;
    esac

    local build_dir="${UI_ROOT}/build/${family}/${dir_name}/${dir_arch}"
    local out_dir="${UI_ROOT}/out/${family}/${dir_name}/${dir_arch}"

    # Cross-compilation toolchain flag
    local toolchain_flag=""
    case "${distro}" in
        opensuse-tumbleweed-aarch64)
            toolchain_flag="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-aarch64.cmake"
            ;;
    esac

    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo "  Building UI for: ${distro}  (${version}  ${arch})"
    echo "════════════════════════════════════════════════════════════"

    # ── Step 1: Build (or update) the Docker image ────────────────────────────
    echo ""
    echo "[1/4] Building Docker image '${image}' ..."
    docker build \
        --pull \
        --file "${dockerfile}" \
        --tag  "${image}" \
        "${PROJECT_ROOT}"
    echo "      Docker image ready."

    # ── Step 2: Run cmake + ninja + cpack inside the container ────────────────
    mkdir -p "${build_dir}" "${out_dir}"

    echo ""
    echo "[2/4] Running cmake + ninja + cpack inside container ..."
    echo "      Source  : ${UI_ROOT}  →  /src  (read-only)"
    echo "      Build   : ${build_dir}  →  /build  (writable)"
    echo "      User    : $(id -u):$(id -g)"
    echo "      Build type: ${BUILD_TYPE}"

    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "${UI_ROOT}:/src:ro" \
        -v "${build_dir}:/build" \
        -v "/etc/passwd:/etc/passwd:ro" \
        -v "/etc/group:/etc/group:ro" \
        "${image}" \
        bash -c "
            set -euo pipefail
            echo '  [cmake] Configuring ...'
            rm -f /build/CMakeCache.txt
            cmake_args=(/src -B /build -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCPACK_GENERATOR=${pkg_type})
            [[ -n '${toolchain_flag}' ]] && cmake_args+=('${toolchain_flag}')
            cmake \"\${cmake_args[@]}\"
            echo '  [ninja] Building ...'
            ninja -C /build
            echo '  [cpack] Packaging ...'
            cd /build && cpack --config CPackConfig.cmake
        "
    echo "      Build + package succeeded."

    # ── Step 3: Copy binary ───────────────────────────────────────────────────
    echo ""
    echo "[3/4] Copying binary to out/${family}/${dir_name}/${dir_arch}/ ..."
    if [[ ! -f "${build_dir}/solakon-one-fritz-powerregulator-ui" ]]; then
        echo "ERROR: Expected binary '${build_dir}/solakon-one-fritz-powerregulator-ui' not found." >&2
        exit 1
    fi
    cp "${build_dir}/solakon-one-fritz-powerregulator-ui" "${out_dir}/solakon-one-fritz-powerregulator-ui"
    echo "      Binary : ${out_dir}/solakon-one-fritz-powerregulator-ui"

    # ── Step 4: Copy package ──────────────────────────────────────────────────
    echo ""
    echo "[4/4] Copying ${pkg_type} package to out/${family}/${dir_name}/${dir_arch}/ ..."

    local pkg_file=""
    case "${pkg_type}" in
        RPM) pkg_file="$(find "${build_dir}" -maxdepth 1 -name "solakon-one-fritz-powerregulator-ui*.rpm" | sort | tail -1)" ;;
        DEB) pkg_file="$(find "${build_dir}" -maxdepth 1 -name "solakon-one-fritz-powerregulator-ui*.deb" | sort | tail -1)" ;;
    esac

    if [[ -z "${pkg_file}" ]]; then
        echo "WARNING: No ${pkg_type} file found in '${build_dir}' — cpack may have failed." >&2
    else
        local pkg_out=""
        case "${pkg_type}" in
            RPM) pkg_out="${out_dir}/solakon-one-fritz-powerregulator-ui-${version}-${release}.${arch}.rpm" ;;
            DEB) pkg_out="${out_dir}/solakon-one-fritz-powerregulator-ui_${version}-${release}_${deb_arch}.deb" ;;
        esac
        cp "${pkg_file}" "${pkg_out}"
        echo "      Package: ${pkg_out}"
    fi
}

# ── Dispatch ──────────────────────────────────────────────────────────────────
if [[ "$DISTRO" == "all" ]]; then
    build_for_distro "opensuse-tumbleweed-x86_64"
    build_for_distro "opensuse-tumbleweed-aarch64"
    build_for_distro "ubuntu-24.04-x86_64"
else
    build_for_distro "$DISTRO"
fi

echo ""
echo "Done."
