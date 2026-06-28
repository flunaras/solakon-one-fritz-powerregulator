#!/usr/bin/env bash
# build.sh — Build solakon-one-fritz-powerregulator inside a Docker container.
#
# Usage:
#   ./docker/build.sh [--distro <alias>|all] [--build-type Release|Debug] [--static]
#
# Available distro aliases:
#   opensuse-tumbleweed-x86_64   openSUSE Tumbleweed, x86_64 (native build)
#   opensuse-tumbleweed-aarch64  openSUSE Tumbleweed, aarch64 (cross-compiled)
#   debian-12-x86_64             Debian 12 (bookworm), x86_64
#   raspbian-bookworm-aarch64    Raspberry Pi OS bookworm, aarch64 (cross-compiled)
#   raspbian-bookworm-armhf      Raspberry Pi OS bookworm, armhf / 32-bit (cross-compiled)
#   raspbian-bullseye-armhf      Raspberry Pi OS bullseye, armhf / 32-bit (cross-compiled)
#                                  Uses httplib v0.14.3 (OpenSSL 1.1.1 on bullseye)
#   all                          Build all of the above (default)
#
# Build type options (default: Release):
#   Release                      Optimized build. Suitable for deployment.
#   Debug                        Debug symbols enabled. For troubleshooting.
#
# Linking options:
#   --static                     Link OpenSSL and the C++ runtime statically.
#                                libc stays shared (required for DNS at runtime).
#                                Produces a self-contained binary with no OpenSSL
#                                runtime dependency.  Off by default (dynamic).
#
# The script:
#   1. Builds (or reuses) a Docker image from docker/Dockerfile.<name>
#   2. Runs cmake + ninja + cpack inside the container as the current host user
#      (--user $(id -u):$(id -g)) so output files are owned by you
#   3. Copies the resulting binary and package into a hierarchical directory tree:
#
#        build/<family>/<distro>/<arch>/   ← cmake / ninja / cpack work tree
#        out/<family>/<distro>/<arch>/     ← final binary and package
#
#      where <family> is "opensuse" or "debian"/"raspbian", <distro> is the
#      short distro name (e.g. "tumbleweed", "12"), and <arch> is "x86_64" or
#      "aarch64" for openSUSE targets, and "amd64"/"arm64"/"armhf" for Debian.
#
#        out/
#        ├── opensuse/
#        │   └── tumbleweed/
#        │       ├── x86_64/
#        │       │   ├── solakon-one-fritz-powerregulator
#        │       │   └── solakon-one-fritz-powerregulator-1.0.0-1.x86_64.rpm
#        │       └── aarch64/
#        │           ├── solakon-one-fritz-powerregulator
#        │           └── solakon-one-fritz-powerregulator-1.0.0-1.aarch64.rpm
#        ├── debian/
#        │   └── 12/amd64/
#        │       ├── solakon-one-fritz-powerregulator
#        │       └── solakon-one-fritz-powerregulator_1.0.0-1_amd64.deb
#        └── raspbian/
#            ├── bookworm/
#            │   ├── arm64/
#            │   │   ├── solakon-one-fritz-powerregulator
#            │   │   └── solakon-one-fritz-powerregulator_1.0.0-1_arm64.deb
#            │   └── armhf/
#            │       ├── solakon-one-fritz-powerregulator
#            │       └── solakon-one-fritz-powerregulator_1.0.0-1_armhf.deb
#            └── bullseye/
#                └── armhf/
#                    ├── solakon-one-fritz-powerregulator
#                    └── solakon-one-fritz-powerregulator_1.0.0-1_armhf.deb

set -euo pipefail

# ── Resolve project root (directory that contains CMakeLists.txt) ─────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────
DISTRO="all"
BUILD_TYPE="Release"
STATIC="OFF"

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
        --static)
            STATIC="ON"
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

# Validate distro choice
case "$DISTRO" in
    opensuse-tumbleweed-x86_64|\
    opensuse-tumbleweed-aarch64|\
    debian-12-x86_64|\
    raspbian-bookworm-aarch64|\
    raspbian-bookworm-armhf|\
    raspbian-bullseye-armhf|\
    all) ;;
    *)
        echo "ERROR: --distro must be one of:" >&2
        echo "  opensuse-tumbleweed-x86_64, opensuse-tumbleweed-aarch64," >&2
        echo "  debian-12-x86_64, raspbian-bookworm-aarch64," >&2
        echo "  raspbian-bookworm-armhf, raspbian-bullseye-armhf, all" >&2
        exit 1
        ;;
esac

# Normalize build type to canonical form (case-insensitive input)
BUILD_TYPE_LOWER="${BUILD_TYPE,,}"
case "$BUILD_TYPE_LOWER" in
    release) BUILD_TYPE="Release" ;;
    debug)   BUILD_TYPE="Debug"   ;;
    *)
        echo "ERROR: --build-type must be Release or Debug (got: $BUILD_TYPE)" >&2
        exit 1
        ;;
esac

# ── Map alias → distribution family ──────────────────────────────────────────
family_for_distro() {
    case "$1" in
        debian-*)   echo "debian"   ;;
        raspbian-*) echo "raspbian" ;;
        *)          echo "opensuse" ;;
    esac
}

# ── Map alias → Dockerfile suffix ────────────────────────────────────────────
dockerfile_for_distro() {
    case "$1" in
        opensuse-tumbleweed-x86_64)  echo "tumbleweed"               ;;
        opensuse-tumbleweed-aarch64) echo "tumbleweed-aarch64"       ;;
        debian-12-x86_64)            echo "debian"                   ;;
        raspbian-bookworm-aarch64)   echo "raspbian-aarch64"         ;;
        raspbian-bookworm-armhf)     echo "raspbian-armhf"           ;;
        raspbian-bullseye-armhf)     echo "raspbian-bullseye-armhf"  ;;
    esac
}

# ── Map alias → CPU architecture ─────────────────────────────────────────────
arch_for_distro() {
    case "$1" in
        opensuse-tumbleweed-aarch64|\
        raspbian-bookworm-aarch64) echo "aarch64" ;;
        raspbian-bookworm-armhf|\
        raspbian-bullseye-armhf)   echo "armv7l"  ;;
        *)                         echo "x86_64"  ;;
    esac
}

# ── Map alias → distro directory name ────────────────────────────────────────
dir_name_for_distro() {
    case "$1" in
        opensuse-tumbleweed-x86_64)  echo "tumbleweed" ;;
        opensuse-tumbleweed-aarch64) echo "tumbleweed" ;;
        debian-12-x86_64)            echo "12"         ;;
        raspbian-bookworm-aarch64)   echo "bookworm"   ;;
        raspbian-bookworm-armhf)     echo "bookworm"   ;;
        raspbian-bullseye-armhf)     echo "bullseye"   ;;
    esac
}

# ── Map RPM/ELF arch → Debian arch name ──────────────────────────────────────
deb_arch_for_arch() {
    case "$1" in
        x86_64)  echo "amd64"  ;;
        aarch64) echo "arm64"  ;;
        armv7l)  echo "armhf"  ;;
        *)       echo "$1"     ;;
    esac
}

# ── Build function ────────────────────────────────────────────────────────────
build_for_distro() {
    local distro="$1"
    local dockerfile_suffix
    dockerfile_suffix="$(dockerfile_for_distro "${distro}")"
    local image="solakon-one-fritz-powerregulator-builder-${dockerfile_suffix}"
    local dockerfile="${SCRIPT_DIR}/Dockerfile.${dockerfile_suffix}"

    # Project version — read from the project() call in CMakeLists.txt
    local version
    version="$(grep -A3 '^project(solakon-one-fritz-powerregulator' "${PROJECT_ROOT}/CMakeLists.txt" \
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

    local dir_arch
    case "${family}" in
        debian|raspbian) dir_arch="${deb_arch}" ;;
        *)               dir_arch="${arch}"     ;;
    esac

    local build_dir="${PROJECT_ROOT}/build/${family}/${dir_name}/${dir_arch}"
    local out_dir="${PROJECT_ROOT}/out/${family}/${dir_name}/${dir_arch}"

    # Cross-compilation toolchain flag
    local toolchain_flag=""
    case "${distro}" in
        opensuse-tumbleweed-aarch64)
            toolchain_flag="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-aarch64.cmake"
            ;;
        raspbian-bookworm-aarch64)
            toolchain_flag="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-raspbian-aarch64.cmake"
            ;;
        raspbian-bookworm-armhf)
            toolchain_flag="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-raspbian-armhf.cmake"
            ;;
        raspbian-bullseye-armhf)
            toolchain_flag="-DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-raspbian-bullseye-armhf.cmake"
            ;;
    esac

    # cpp-httplib tag override: bullseye has OpenSSL 1.1.1; httplib >= v0.15.0
    # refuses to compile against OpenSSL < 3.0.0 when HTTPLIB_REQUIRE_OPENSSL=ON.
    local httplib_tag_flag=""
    case "${distro}" in
        raspbian-bullseye-armhf)
            httplib_tag_flag="-DSOLAKON_HTTPLIB_TAG=v0.14.3"
            ;;
    esac

    local pkg_type="RPM"
    case "${distro}" in
        debian-*|raspbian-*) pkg_type="DEB" ;;
    esac

    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo "  Building for: ${distro}  (${version}  ${arch})"
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
    echo "      Source  : ${PROJECT_ROOT}  →  /src  (read-only)"
    echo "      Build   : ${build_dir}  →  /build  (writable)"
    echo "      User    : $(id -u):$(id -g)"
    echo "      Build type: ${BUILD_TYPE}"
    echo "      Static    : ${STATIC}"

    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "${PROJECT_ROOT}:/src:ro" \
        -v "${build_dir}:/build" \
        -v "/etc/passwd:/etc/passwd:ro" \
        -v "/etc/group:/etc/group:ro" \
        "${image}" \
        bash -c "
            set -euo pipefail
            echo '  [cmake] Configuring ...'
            rm -f /build/CMakeCache.txt
            cmake_args=(/src -B /build -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCPACK_GENERATOR=${pkg_type} -DSOLAKON_FRITZ_POWERREGULATOR_STATIC=${STATIC})
            [[ -n '${toolchain_flag}' ]] && cmake_args+=('${toolchain_flag}')
            [[ -n '${httplib_tag_flag}' ]] && cmake_args+=('${httplib_tag_flag}')
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
    if [[ ! -f "${build_dir}/solakon-one-fritz-powerregulator" ]]; then
        echo "ERROR: Expected binary '${build_dir}/solakon-one-fritz-powerregulator' not found." >&2
        exit 1
    fi
    cp "${build_dir}/solakon-one-fritz-powerregulator" "${out_dir}/solakon-one-fritz-powerregulator"
    echo "      Binary : ${out_dir}/solakon-one-fritz-powerregulator"

    # ── Step 4: Copy package ──────────────────────────────────────────────────
    echo ""
    echo "[4/4] Copying ${pkg_type} package to out/${family}/${dir_name}/${dir_arch}/ ..."

    local pkg_file=""
    case "${pkg_type}" in
        RPM) pkg_file="$(find "${build_dir}" -maxdepth 1 -name "solakon-one-fritz-powerregulator*.rpm" | sort | tail -1)" ;;
        DEB) pkg_file="$(find "${build_dir}" -maxdepth 1 -name "solakon-one-fritz-powerregulator*.deb" | sort | tail -1)" ;;
    esac

    if [[ -z "${pkg_file}" ]]; then
        echo "WARNING: No ${pkg_type} file found in '${build_dir}' — cpack may have failed." >&2
    else
        local pkg_out=""
        case "${pkg_type}" in
            RPM) pkg_out="${out_dir}/solakon-one-fritz-powerregulator-${version}-${release}.${arch}.rpm" ;;
            DEB) pkg_out="${out_dir}/solakon-one-fritz-powerregulator_${version}-${release}_${deb_arch}.deb" ;;
        esac
        cp "${pkg_file}" "${pkg_out}"
        echo "      Package: ${pkg_out}"
    fi
}

# ── Dispatch ──────────────────────────────────────────────────────────────────
if [[ "$DISTRO" == "all" ]]; then
    build_for_distro "opensuse-tumbleweed-x86_64"
    build_for_distro "opensuse-tumbleweed-aarch64"
    build_for_distro "debian-12-x86_64"
    build_for_distro "raspbian-bookworm-aarch64"
    build_for_distro "raspbian-bookworm-armhf"
    build_for_distro "raspbian-bullseye-armhf"
else
    build_for_distro "$DISTRO"
fi

echo ""
echo "Done."
