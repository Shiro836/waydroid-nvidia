#!/usr/bin/env bash
# build/hwcomposer/build.sh [SRCDIR] [OUTDIR]
# Standalone NDK build of hwcomposer.waydroid.so (waydroid hwcomposer, vendor
# module) against headers from AOSP android-13 clones and the image's own
# shared libraries. No AOSP build system required. Produces both the .so and a
# stripped .so. The ONE hwcomposer recipe (dev/build + packaging/aur).
#
# PREREQS (hand-provisioned today; see packaging/aur/PREREQS.md — the remaining
# gap before a fully clean-room build). All under $WNV by default:
#   aosp_* shallow header clones (libhidl, libhwbinder, fmq, system_core,
#     frameworks_native, libbase, libhardware, ...), android_system_logging,
#     hidl-out (our hidl-gen output), ndk-prefix (static wayland/ffi/xkbcommon),
#     minigbm, mesa/subprojects/libdrm-*. Plus the image rootfs for -l links.
#
# Env: WNV (prereq root, default ~/repos/waydroid-nv), ROOTFS (image libs,
#      default /var/lib/waydroid/rootfs), NDK (default /opt/android-ndk).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

WNV="${WNV:-$HOME/repos/waydroid-nv}"
ROOTFS="${ROOTFS:-/var/lib/waydroid/rootfs}"
NDK="${NDK:-/opt/android-ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin"
CXX="$NDK/x86_64-linux-android34-clang++"
CC="$NDK/x86_64-linux-android34-clang"
STRIP="$NDK/llvm-strip"

SRC="${1:-$WNV/hwcomposer-src/hwcomposer}"
OUT="${2:-$HERE/out}"
GEN="$HERE/gen"

# The final link resolves vendor.waydroid.*.so from the image rootfs, which is
# only present when the container has been started at least once.
[ -e "$ROOTFS/vendor/lib64/vendor.waydroid.display@1.0.so" ] || {
    echo "hwcomposer/build.sh: $ROOTFS/vendor/lib64 has no image libs." >&2
    echo "  Start the container once so rootfs mounts (dev/restart), or set" >&2
    echo "  ROOTFS to an unpacked vendor image. (Objects will compile but the" >&2
    echo "  link needs the vendor.waydroid HIDL libs.)" >&2
    exit 1
}

mkdir -p "$OUT"

INC=(
  -I"$SRC"
  -I"$GEN"
  -I"$WNV/hidl-out"
  -I"$WNV/ndk-prefix/include"
  -I"$WNV/aosp_libhidl/transport/include"
  -I"$WNV/aosp_libhidl/base/include"
  -I"$WNV/aosp_libhwbinder/include"
  -I"$WNV/aosp_fmq/include"
  -I"$WNV/aosp_fmq/base"
  -I"$WNV/aosp_system_core/libcutils/include"
  -I"$WNV/aosp_system_core/libutils/include"
  -I"$WNV/aosp_system_core/libsync/include"
  -I"$WNV/aosp_system_core"
  -I"$WNV/aosp_system_core/libsystem/include"
  -I"$WNV/aosp_system_core/libprocessgroup/include"
  -I"$WNV/android_system_logging/liblog/include"
  -I"$WNV/aosp_libbase/include"
  -I"$WNV/aosp_frameworks_native/libs/ui/include"
  -I"$WNV/aosp_frameworks_native/libs/nativewindow/include"
  -I"$WNV/aosp_frameworks_native/libs/nativebase/include"
  -I"$WNV/aosp_frameworks_native/libs/arect/include"
  -I"$WNV/aosp_frameworks_native/libs/math/include"
  -I"$WNV/aosp_frameworks_native/libs/gui/include"
  -I"$WNV/aosp_libhardware/include"
  -I"$WNV/minigbm"
  -I"$WNV/mesa/subprojects/libdrm-2.4.133"
  -I"$WNV/mesa/subprojects/libdrm-2.4.133/include/drm"
  -I"$WNV/mesa/subprojects/libdrm-2.4.133/android"
)

DEFS=(
  -DLOG_TAG=\"hwcomposer\"
  -D__ANDROID_VNDK__
  -D__ANDROID_USE_LIBLOG_SAFETY_NET=0
)

CXXFLAGS=(-O2 -fPIC -std=c++17 -isystem "$HERE/libcxx-override" -Wall -Wno-unused-parameter -Wno-deprecated-declarations -fno-exceptions -fno-rtti)

SRCS=(
  "$HERE/libcxx_compat.cpp"
  "$SRC/extension.cpp"
  "$SRC/gralloc_handler.cpp"
  "$SRC/hwcomposer.cpp"
  "$SRC/wayland-hwc.cpp"
  "$SRC/WaydroidClipboard.cpp"
  "$SRC/WaydroidWindow.cpp"
  "$SRC/egl-tools.cpp"
  "$SRC/modes/waydroid_mode.cpp"
  "$SRC/modes/closed.cpp"
  "$SRC/modes/full-ui.cpp"
  "$SRC/modes/multi-window.cpp"
  "$SRC/modes/single-window.cpp"
)
CSRCS=(
  "$GEN/wayland-android-protocol.c"
  "$GEN/xdg-shell-protocol.c"
  "$GEN/viewporter-protocol.c"
  "$GEN/presentation-time-protocol.c"
  "$GEN/linux-dmabuf-unstable-v1-protocol.c"
  "$GEN/tablet-unstable-v2-protocol.c"
  "$GEN/relative-pointer-unstable-v1-protocol.c"
  "$GEN/pointer-constraints-unstable-v1-protocol.c"
  "$GEN/fractional-scale-v1-protocol.c"
  "$GEN/idle-inhibit-unstable-v1-protocol.c"
)

OBJS=()
for s in "${SRCS[@]}"; do
  o=$OUT/$(basename "${s%.cpp}").o
  echo "CXX $(basename "$s")"
  $CXX "${CXXFLAGS[@]}" "${DEFS[@]}" "${INC[@]}" -c "$s" -o "$o"
  OBJS+=("$o")
done
for s in "${CSRCS[@]}"; do
  o=$OUT/$(basename "${s%.c}").o
  echo "CC  $(basename "$s")"
  $CC -O2 -fPIC "${INC[@]}" -c "$s" -o "$o"
  OBJS+=("$o")
done

echo "LINK hwcomposer.waydroid.so"
$CXX -shared -o "$OUT/hwcomposer.waydroid.so" "${OBJS[@]}" \
  -nostdlib++ \
  "$WNV/ndk-prefix/lib/libwayland-client.a" \
  "$WNV/ndk-prefix/lib/libffi.a" \
  "$WNV/ndk-prefix/lib/libxkbcommon.a" \
  -L"$ROOTFS/system/lib64" \
  -L"$ROOTFS/apex/com.android.vndk.v33/lib64" \
  -L"$ROOTFS/vendor/lib64" \
  -lc++ -llog -lutils -lcutils -lhardware -lhidlbase -lsync -lui -ldrm -lEGL -lGLESv2 \
  -l:vendor.waydroid.clipboard@1.0.so \
  -l:vendor.waydroid.display@1.0.so \
  -l:vendor.waydroid.display@1.1.so \
  -l:vendor.waydroid.display@1.2.so \
  -l:vendor.waydroid.task@1.0.so \
  -l:vendor.waydroid.window@1.0.so \
  -l:vendor.waydroid.window@1.1.so \
  -l:vendor.waydroid.window@1.2.so \
  -Wl,--no-undefined -Wl,-soname,hwcomposer.waydroid.so

echo "STRIP hwcomposer.waydroid.stripped.so"
$STRIP -o "$OUT/hwcomposer.waydroid.stripped.so" "$OUT/hwcomposer.waydroid.so"
ls -l "$OUT/hwcomposer.waydroid.so" "$OUT/hwcomposer.waydroid.stripped.so"
