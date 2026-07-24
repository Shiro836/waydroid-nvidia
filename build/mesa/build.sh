#!/usr/bin/env bash
# build/mesa/build.sh SRCDIR [BUILDDIR]
# Cross-build the guest Venus driver (libvulkan_virtio.so) for Android x86_64
# or x86.
# The ONE mesa build recipe: used by dev/build (persistent tree, incremental)
# and packaging/aur reproduce (fresh checkout). Generates a portable meson
# cross file from env — no hardcoded home path, no ccache dependency.
#
# Env:
#   NDK            NDK root            (default /opt/android-ndk)
#   NDK_PKGCONFIG  ABI-matched Android pkgconfig (default empty; use fallbacks)
#   ANDROID_ABI    x86_64 or x86       (default x86_64)
#   ANDROID_API    Android API level   (default 34)
set -euo pipefail
SRCDIR="${1:?usage: build.sh SRCDIR [BUILDDIR]}"
HERE="$(cd "$(dirname "$0")" && pwd)"

ANDROID_ABI="${ANDROID_ABI:-x86_64}"
ANDROID_API="${ANDROID_API:-34}"
case "$ANDROID_API" in
    ''|*[!0-9]*) echo "mesa/build.sh: ANDROID_API must be numeric (got '$ANDROID_API')" >&2; exit 1 ;;
esac

case "$ANDROID_ABI" in
    x86_64)
        NDK_TRIPLE=x86_64-linux-android
        MESON_CPU_FAMILY=x86_64
        MESON_CPU=x86_64
        ;;
    x86)
        # The NDK calls 32-bit x86 i686; Meson calls the family/cpu x86.
        NDK_TRIPLE=i686-linux-android
        MESON_CPU_FAMILY=x86
        MESON_CPU=x86
        ;;
    *)
        echo "mesa/build.sh: unsupported ANDROID_ABI '$ANDROID_ABI' (expected x86_64 or x86)" >&2
        exit 1
        ;;
esac
BUILDDIR="${2:-$SRCDIR/build-android-$ANDROID_ABI}"

NDK="${NDK:-/opt/android-ndk}"
NDK_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [ -z "${NDK_PKGCONFIG:-}" ]; then
    # The historical local pkg-config tree was x86_64-only. An empty,
    # ABI-specific directory makes Meson use its pinned libdrm fallback and
    # prevents an x86 build from silently consuming ELF64 dependencies.
    NDK_PKGCONFIG="$BUILDDIR/empty-pkgconfig"
    mkdir -p "$NDK_PKGCONFIG"
fi
NDK_TARGET="${NDK_TRIPLE}${ANDROID_API}"
[ -x "$NDK_BIN/$NDK_TARGET-clang" ] || {
    echo "mesa/build.sh: NDK clang not found: $NDK_BIN/$NDK_TARGET-clang" >&2
    exit 1
}

if [ -f "$BUILDDIR/build.ninja" ]; then
    if [ ! -f "$BUILDDIR/nv-cross.ini" ] || \
       ! grep -Fq "$NDK_TARGET-clang" "$BUILDDIR/nv-cross.ini"; then
        echo "mesa/build.sh: $BUILDDIR was configured for a different Android ABI/API" >&2
        echo "mesa/build.sh: use an ABI-specific build directory or remove that build directory" >&2
        exit 1
    fi
else
    echo "mesa/build.sh: fresh meson setup ($ANDROID_ABI, API $ANDROID_API) -> $BUILDDIR"
    mkdir -p "$BUILDDIR"
    cross="$BUILDDIR/nv-cross.ini"
    sed -e "s#@NDK_BIN@#$NDK_BIN#g" \
        -e "s#@NDK_PKGCONFIG@#$NDK_PKGCONFIG#g" \
        -e "s#@NDK_TARGET@#$NDK_TARGET#g" \
        -e "s#@CPU_FAMILY@#$MESON_CPU_FAMILY#g" \
        -e "s#@CPU@#$MESON_CPU#g" \
        "$HERE/android-cross.ini.in" > "$cross"
    meson setup "$BUILDDIR" "$SRCDIR" --cross-file "$cross" \
        -Dplatforms=android -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
        -Dvulkan-drivers=virtio -Dgallium-drivers= -Dshared-glapi=disabled \
        -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dopengl=false \
        -Dplatform-sdk-version="$ANDROID_API" -Dallow-fallback-for=libdrm
fi

ninja -C "$BUILDDIR" src/virtio/vulkan/libvulkan_virtio.so
echo "  -> $BUILDDIR/src/virtio/vulkan/libvulkan_virtio.so"
