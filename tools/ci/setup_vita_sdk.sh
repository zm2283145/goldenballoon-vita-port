#!/usr/bin/env bash
# Locked toolchain bootstrap for an otherwise clean Ubuntu Actions runner.
set -euo pipefail

: "${RUNNER_TEMP:?Run this bootstrap on a GitHub Actions Ubuntu runner}"
: "${GITHUB_ENV:?Missing Actions environment file}"
: "${GITHUB_PATH:?Missing Actions PATH file}"

export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
work="$RUNNER_TEMP/goldenballoon-vita-sdk"
sdk_release=sdk-snapshot-20260825.611.1
packages_release=packages-2026.08-snapshot-20260916.45.1
vitagl_commit=1ffcb99e65d979722c37eedd7298e9487f8eedb1
vitashark_commit=df24065e65098b2d1ac533760109ad4367573f28

if [[ -e "$VITASDK" || -e "$work" ]]; then
    echo "Refusing to overwrite an existing VitaSDK/bootstrap directory" >&2
    exit 1
fi

sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl ca-certificates \
    bzip2 xz-utils pkg-config python3 zip unzip fakeroot file libarchive-tools
mkdir -p "$work/downloads"

download_checked() {
    local url="$1" checksum="$2" output="$3"
    curl --fail --silent --show-error --location --retry 5 --output "$output" "$url"
    printf '%s  %s\n' "$checksum" "$output" | sha256sum --check --strict -
}

archive="$work/downloads/vitasdk-bootstrap-x86_64-linux-gnu.tar.bz2"
download_checked \
    "https://github.com/vitasdk/autobuilds/releases/download/$sdk_release/$(basename "$archive")" \
    3d5be8ad0fae5de872c13328f3508a59b9e1eec504269eec64e47d06de78b098 \
    "$archive"
sudo install -d -o "$(id -u)" -g "$(id -g)" "$VITASDK"
tar -xjf "$archive" -C "$VITASDK" --strip-components=1
test "$(arm-vita-eabi-gcc -dumpfullversion)" = 15.2.0

package_paths=()
while read -r checksum package; do
    output="$work/downloads/$package"
    download_checked \
        "https://github.com/vitasdk/vitasdk-autobuild/releases/download/$packages_release/$package" \
        "$checksum" "$output"
    package_paths+=("$output")
done <<'PACKAGES'
93d2075b6b61a164224a2aad651d3d6787f31f3a19c3e2bada7c99844f9fae90 taihen-0.11-1-vita.pkg.tar.xz
a95d46d74f6e7acab5986592aec2432f8395af0975551d29dfe9a04421e25184 libmathneon-0.0.0.r11.g0faab81-1-vita.pkg.tar.xz
443f8e9820cf7196f0071deb59edad0cad17cae1ce64e8fceb1270cc705dbffb SceShaccCgExt-1.0.1-1-vita.pkg.tar.xz
53199dac0bfec090dd9eb9a8f438b8ee5641cedac269cc7518872f318ab748ee sdl2-2.32.8-1-vita.pkg.tar.xz
PACKAGES

# Install only verified local archives; do not refresh a floating SDK channel.
vdpm pacman -- -U --noconfirm --noscriptlet "${package_paths[@]}"

fetch_commit() {
    local url="$1" commit="$2" directory="$3"
    git init -q "$directory"
    git -C "$directory" remote add origin "$url"
    git -C "$directory" fetch -q --depth=1 origin "$commit"
    git -C "$directory" checkout -q --detach FETCH_HEAD
    test "$(git -C "$directory" rev-parse HEAD)" = "$commit"
}

fetch_commit https://github.com/Rinnegatamante/vitaShaRK.git \
    "$vitashark_commit" "$work/vitaShaRK"
make -C "$work/vitaShaRK" -j"$(nproc)" install
fetch_commit https://github.com/Rinnegatamante/vitaGL.git \
    "$vitagl_commit" "$work/vitaGL"
make -C "$work/vitaGL" -j"$(nproc)" \
    NO_DEBUG=1 NO_SPLASHSCREEN=0 SOFTFP_ABI=0 \
    HAVE_DEBUGGER=0 HAVE_PROFILING=0 HAVE_RAZOR=0 \
    HAVE_DEVKIT=0 HAVE_CPU_TRACER=0 install

vitagl_root="$work/vitagl-root"
install -d "$vitagl_root"
install -m 644 "$work/vitaGL/libvitaGL.a" "$vitagl_root/libvitaGL.a"
install -m 644 "$work/vitaGL/source/vitaGL.h" "$vitagl_root/vitaGL.h"
for library in vitaGL vitashark SceShaccCgExt taihen_stub mathneon SDL2; do
    test -s "$VITASDK/arm-vita-eabi/lib/lib${library}.a"
done
test -s "$VITASDK/share/vita.toolchain.cmake"
test -s "$VITASDK/arm-vita-eabi/include/SDL2/SDL.h"

{
    printf 'VITASDK=%s\n' "$VITASDK"
    printf 'MDKR_VITAGL_ROOT=%s\n' "$vitagl_root"
    printf 'VITA_SDK_RELEASE=%s\n' "$sdk_release"
    printf 'VITA_PACKAGES_RELEASE=%s\n' "$packages_release"
    printf 'VITAGL_COMMIT=%s\n' "$vitagl_commit"
    printf 'VITASHARK_COMMIT=%s\n' "$vitashark_commit"
} >> "$GITHUB_ENV"
printf '%s/bin\n' "$VITASDK" >> "$GITHUB_PATH"
