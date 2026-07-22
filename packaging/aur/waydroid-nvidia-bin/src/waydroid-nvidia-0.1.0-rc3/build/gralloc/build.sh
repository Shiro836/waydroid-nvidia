#!/usr/bin/env bash
# build/gralloc/build.sh [OUTDIR]
# Build the net-new gralloc backend (libgbm_mesa_wrapper.so): a single C file
# that replaces minigbm's gbm_mesa wrapper and allocates NVIDIA-native buffers
# over the vtest socket (VCMD_RESOURCE_ALLOC_GPU). NDK-compiled, bionic.
#
# Env:
#   NDK        NDK root                         (default /opt/android-ndk)
#   MINIGBM    minigbm checkout (for the header) (default ~/repos/waydroid-nv/minigbm)
set -euo pipefail
OUTDIR="${1:-$(cd "$(dirname "$0")" && pwd)/out}"
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"

NDK="${NDK:-/opt/android-ndk}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android34-clang"
MINIGBM="${MINIGBM:-$HOME/repos/waydroid-nv/minigbm}"
SRC="$REPO/src/minigbm-vtest/vtest_wrapper.c"
HDR_DIR="$MINIGBM/gbm_mesa_driver"   # provides gbm_mesa_wrapper.h

[ -x "$CC" ] || { echo "gralloc/build.sh: NDK clang not at $CC" >&2; exit 1; }
[ -f "$HDR_DIR/gbm_mesa_wrapper.h" ] || { echo "gralloc/build.sh: need minigbm at $MINIGBM (gbm_mesa_wrapper.h)" >&2; exit 1; }

mkdir -p "$OUTDIR"
echo "gralloc/build.sh: compiling libgbm_mesa_wrapper.so"
"$CC" -O2 -fPIC -shared -Wall \
    -I"$HDR_DIR" \
    -o "$OUTDIR/libgbm_mesa_wrapper.so" "$SRC" \
    -llog -Wl,-soname,libgbm_mesa_wrapper.so
echo "  -> $OUTDIR/libgbm_mesa_wrapper.so"
