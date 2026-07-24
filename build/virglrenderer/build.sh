#!/usr/bin/env bash
# build/virglrenderer/build.sh SRCDIR [BUILDDIR]
# Build the host Venus renderer (virgl_test_server + render server). The ONE
# virgl build recipe: drops the net-new vtest allocator source into the tree,
# then meson + ninja. Used by dev/build and packaging/aur reproduce.
#
# The net-new src (src/virglrenderer-vtest/vtest_gpu_alloc.{c,h}) is referenced
# by the vtest/meson.build change in patch 0004, so it must sit in vtest/.
set -euo pipefail
SRCDIR="${1:?usage: build.sh SRCDIR [BUILDDIR]}"
BUILDDIR="${2:-$SRCDIR/build}"
# repo root = three levels up from this script (build/virglrenderer/build.sh)
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"

echo "virgl/build.sh: install net-new vtest allocator source"
mkdir -p "$SRCDIR/vtest/"
install -m 0644 "$REPO/src/virglrenderer-vtest/vtest_gpu_alloc.c" "$SRCDIR/vtest/"
install -m 0644 "$REPO/src/virglrenderer-vtest/vtest_gpu_alloc.h" "$SRCDIR/vtest/"
install -m 0644 "$REPO/src/vtest_alloc_formats.h" "$SRCDIR/vtest/"

if [ ! -f "$BUILDDIR/build.ninja" ]; then
    echo "virgl/build.sh: fresh meson setup -> $BUILDDIR"
    meson setup "$BUILDDIR" "$SRCDIR" -Dvenus=true -Drender-server-worker=auto
fi

ninja -C "$BUILDDIR"
echo "  -> $BUILDDIR/vtest/virgl_test_server (+ server/virgl_render_server)"
