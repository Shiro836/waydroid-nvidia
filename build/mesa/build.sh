#!/usr/bin/env bash
# build/mesa/build.sh SRCDIR [BUILDDIR]
# Cross-build the guest Venus driver (libvulkan_virtio.so) for android-x86_64.
# The ONE mesa build recipe: used by dev/build (persistent tree, incremental)
# and packaging/aur reproduce (fresh checkout). Generates a portable meson
# cross file from env — no hardcoded home path, no ccache dependency.
#
# Env:
#   NDK            NDK root            (default /opt/android-ndk)
#   NDK_PKGCONFIG  android pkgconfig   (default ~/repos/waydroid-nv/ndk-pkgconfig)
set -euo pipefail
SRCDIR="${1:?usage: build.sh SRCDIR [BUILDDIR]}"
BUILDDIR="${2:-$SRCDIR/build-android-x86_64}"
HERE="$(cd "$(dirname "$0")" && pwd)"

NDK="${NDK:-/opt/android-ndk}"
NDK_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_PKGCONFIG="${NDK_PKGCONFIG:-$HOME/repos/waydroid-nv/ndk-pkgconfig}"
[ -x "$NDK_BIN/x86_64-linux-android34-clang" ] || { echo "mesa/build.sh: NDK clang not at $NDK_BIN" >&2; exit 1; }

if [ ! -f "$BUILDDIR/build.ninja" ]; then
    echo "mesa/build.sh: fresh meson setup -> $BUILDDIR"
    mkdir -p "$BUILDDIR"
    cross="$BUILDDIR/nv-cross.ini"
    sed -e "s#@NDK_BIN@#$NDK_BIN#g" -e "s#@NDK_PKGCONFIG@#$NDK_PKGCONFIG#g" \
        "$HERE/android-x86_64-cross.ini.in" > "$cross"
    meson setup "$BUILDDIR" "$SRCDIR" --cross-file "$cross" \
        -Dplatforms=android -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
        -Dvulkan-drivers=virtio -Dgallium-drivers= -Dshared-glapi=disabled \
        -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dopengl=false \
        -Dplatform-sdk-version=34 -Dallow-fallback-for=libdrm
fi

ninja -C "$BUILDDIR" src/virtio/vulkan/libvulkan_virtio.so
echo "  -> $BUILDDIR/src/virtio/vulkan/libvulkan_virtio.so"
