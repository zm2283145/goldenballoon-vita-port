#!/bin/bash
# build_crossarch_x86_64.sh — Build the thin x86_64 mdkr64 the cross-architecture
# determinism gate compares against the native arm64 build.
#
# This is NOT build_universal.sh. That script asks for one fat executable and
# documents why it cannot have one; this script asks for a SECOND, separate,
# single-architecture build directory, which sidesteps every fat-binary problem
# because each configure fetches exactly one architecture's dependencies.
#
# tests/check_crossarch_determinism.py then runs both executables over the same
# ROM and input script and compares the authoritative [SIMHASH] streams. The
# gate REQUIRES thin binaries: a universal one would run its native slice under
# both roles and report a vacuous pass.
#
# Two obstacles on an Apple Silicon host, both handled here:
#
#   1. SDL2. CMakeLists.txt finds SDL2 through pkg-config only
#      (pkg_check_modules(SDL2 REQUIRED sdl2)). Apple Silicon Homebrew installs
#      an arm64-only dylib in /opt/homebrew and there is no x86_64 Homebrew
#      prefix under /usr/local on a stock machine, so an -arch x86_64 compile
#      links against an arm64 dylib and dies with "building for macOS-x86_64 but
#      attempting to link with file built for arm64". The fix is to build SDL2
#      from source for x86_64 with the existing macos/Scripts/build_release_sdl2.sh
#      and point PKG_CONFIG_PATH at its prefix.
#
#   2. wgpu-native. cmake/webgpu.cmake selects ONE prebuilt archive per
#      configure, keyed on CMAKE_SYSTEM_PROCESSOR. On macOS that variable comes
#      from the host `uname -m` and is NOT re-derived from
#      CMAKE_OSX_ARCHITECTURES, so an x86_64-targeted configure would still
#      fetch the aarch64 archive and fail the link inside libwgpu_native.a. A
#      Darwin/x86_64 archive is pinned and hash-verified upstream, so forcing
#      CMAKE_SYSTEM_PROCESSOR=x86_64 on the command line is enough.
#
# Usage: macos/Scripts/build_crossarch_x86_64.sh [options]
#   --build-dir PATH       CMake build directory (default: build-rel-x86_64)
#   --build-type TYPE      CMAKE_BUILD_TYPE (default: Release)
#   --deployment-target V  Minimum macOS version (default: 13.0)
#   --sdl-prefix PATH      Reuse an existing x86_64 SDL2 install prefix instead
#                          of building one
#   --skip-sdl             Assume PKG_CONFIG_PATH already resolves an x86_64
#                          sdl2.pc
#   --configure-only       Configure but do not compile
#
# The arm64 side is whatever build directory you already have; nothing here
# touches it. Build BOTH from the same commit and the same CMAKE_BUILD_TYPE, or
# the gate refuses the pair.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { printf "${GREEN}[INFO]${NC}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${NC}  %s\n" "$*"; }
error() { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }
die()   { error "$*"; exit 1; }

BUILD_DIR="build-rel-x86_64"
BUILD_TYPE="Release"
DEPLOYMENT_TARGET="13.0"
SDL_PREFIX=""
SKIP_SDL=false
CONFIGURE_ONLY=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)          [[ $# -ge 2 ]] || die "--build-dir requires a path"; BUILD_DIR="$2"; shift 2 ;;
        --build-type)         [[ $# -ge 2 ]] || die "--build-type requires a value"; BUILD_TYPE="$2"; shift 2 ;;
        --deployment-target)  [[ $# -ge 2 ]] || die "--deployment-target requires a value"; DEPLOYMENT_TARGET="$2"; shift 2 ;;
        --sdl-prefix)         [[ $# -ge 2 ]] || die "--sdl-prefix requires a path"; SDL_PREFIX="$2"; SKIP_SDL=true; shift 2 ;;
        --skip-sdl)           SKIP_SDL=true; shift ;;
        --configure-only)     CONFIGURE_ONLY=true; shift ;;
        -h|--help)            sed -n '2,45p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)                    die "unknown option: $1" ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || die "this script targets macOS hosts"

# Rosetta 2 is what actually EXECUTES the result on Apple Silicon. Compiling an
# x86_64 binary that the host cannot run produces a build the gate can only
# report as a launch failure, so check for it before spending the build.
if [[ "$(uname -m)" == "arm64" ]]; then
    if ! arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        die "this host cannot execute x86_64 binaries; run 'softwareupdate --install-rosetta' first"
    fi
    info "Rosetta 2 is present; the x86_64 build will be runnable here"
fi

cd "${PROJECT_ROOT}"

# ---------------------------------------------------------------------------
# 1. x86_64 SDL2
# ---------------------------------------------------------------------------
if [[ "${SKIP_SDL}" == false ]]; then
    # build_release_sdl2.sh requires the work directory basename to be
    # exactly sdl2-<version>, so the architecture goes in the parent.
    SDL_WORK_DIR="build-macos-deps/x86_64/sdl2-${MDKR_SDL2_VERSION:-2.32.10}"
    SDL_PREFIX="${PROJECT_ROOT}/${SDL_WORK_DIR}/install"
    if [[ -f "${SDL_PREFIX}/lib/pkgconfig/sdl2.pc" ]]; then
        info "reusing the x86_64 SDL2 already installed at ${SDL_PREFIX}"
    else
        info "building an x86_64 SDL2 from source (Homebrew has no x86_64 slice here)"
        "${SCRIPT_DIR}/build_release_sdl2.sh" \
            --arch x86_64 \
            --deployment-target "${DEPLOYMENT_TARGET}" \
            --work-dir "${SDL_WORK_DIR}"
    fi
fi

if [[ -n "${SDL_PREFIX}" ]]; then
    export PKG_CONFIG_PATH="${SDL_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
fi

# Fail here rather than 200 compiled objects later. pkg-config resolving an
# arm64 dylib is the single most likely way this build goes wrong, and the link
# error it produces names a file, not a cause.
if command -v pkg-config >/dev/null 2>&1; then
    SDL_LIBDIR="$(pkg-config --variable=libdir sdl2 2>/dev/null || true)"
    if [[ -z "${SDL_LIBDIR}" ]]; then
        die "pkg-config cannot find sdl2; pass --sdl-prefix or drop --skip-sdl"
    fi
    SDL_DYLIB="$(ls "${SDL_LIBDIR}"/libSDL2-2.0.0.dylib "${SDL_LIBDIR}"/libSDL2.dylib 2>/dev/null | head -n1 || true)"
    if [[ -n "${SDL_DYLIB}" ]]; then
        if ! lipo -archs "${SDL_DYLIB}" 2>/dev/null | tr ' ' '\n' | grep -qx "x86_64"; then
            die "pkg-config resolves sdl2 to ${SDL_DYLIB}, which has no x86_64 slice ($(lipo -archs "${SDL_DYLIB}" 2>/dev/null)). Prepend an x86_64 sdl2.pc to PKG_CONFIG_PATH"
        fi
        info "sdl2 resolves to ${SDL_DYLIB} ($(lipo -archs "${SDL_DYLIB}"))"
    fi
fi

# ---------------------------------------------------------------------------
# 2. Configure
# ---------------------------------------------------------------------------
info "configuring ${BUILD_DIR} for x86_64 (${BUILD_TYPE})"
cmake -S . -B "${BUILD_DIR}" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

if [[ "${CONFIGURE_ONLY}" == true ]]; then
    info "configure complete; skipping the build as requested"
    exit 0
fi

# ---------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------
info "building the mdkr64 target"
cmake --build "${BUILD_DIR}" --target mdkr64 \
    --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}"

BINARY="${BUILD_DIR}/mdkr64"
[[ -x "${BINARY}" ]] || die "expected an executable at ${BINARY}"

ARCHS="$(lipo -archs "${BINARY}")"
if [[ "${ARCHS}" != "x86_64" ]]; then
    die "built ${BINARY} reports architecture(s) '${ARCHS}', expected exactly x86_64. A fat binary runs its native slice and would make the determinism gate compare arm64 against arm64"
fi
info "built ${BINARY} (${ARCHS})"

cat <<EOF

Next: compare it against the native arm64 build.

  tests/check_crossarch_determinism.py \\
      --build build-rel \\
      --build-x86 ${BUILD_DIR} \\
      --rom baserom.us.v80.z64

Both trees must come from the same commit and the same CMAKE_BUILD_TYPE; the
gate refuses the pair otherwise. The x86_64 arm runs under Rosetta 2 and is
materially slower than the native one, so allow extra wall-clock time.
EOF
