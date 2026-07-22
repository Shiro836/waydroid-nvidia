#!/bin/bash
# Full waydroid-nvidia HOST-stack matrix on an Ubuntu 22.04 box/container:
# builds virglrenderer (with our vtest_gpu_alloc) + mesa venus ICD from the
# public repo at pinned SHAs, then runs the venus + allocation + EGL matrix.
# Emits RESULT lines; exit 0 iff all pass.
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update -qq
apt-get install -qq -y git gcc g++ ninja-build python3-pip python3-venv \
    pkg-config bison flex glslang-tools zstd \
    libepoxy-dev libdrm-dev libgbm-dev libegl-dev libgles-dev libvulkan-dev \
    libx11-dev libwayland-dev libxext6 libglx0 libegl1 vulkan-tools >/dev/null

mkdir -p ~/wdnv && cd ~/wdnv
[ -d venv ] || python3 -m venv venv
source venv/bin/activate
pip -q install meson mako pyyaml packaging

[ -d waydroid-nvidia ] || git clone -q --depth 1 https://github.com/Shiro836/waydroid-nvidia
REPO=~/wdnv/waydroid-nvidia
source "$REPO/packaging/ci/pins.env"

shallow_at() {
    [ -d "$1/.git" ] && return 0
    git init -q "$1"; git -C "$1" remote add origin "$2"
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q FETCH_HEAD
}

echo "=== virglrenderer @$VIRGL_SHA"
shallow_at virglrenderer "$VIRGL_UPSTREAM" "$VIRGL_SHA"
git -C virglrenderer clean -qfd; git -C virglrenderer checkout -q -- . 2>/dev/null || true
for p in "$REPO"/patches/virglrenderer/*.patch; do git -C virglrenderer apply "$p"; done
REPO="$REPO" bash "$REPO/build/virglrenderer/build.sh" ~/wdnv/virglrenderer ~/wdnv/virgl-build

echo "=== mesa host venus ICD @$MESA_SHA"
shallow_at mesa "$MESA_UPSTREAM" "$MESA_SHA"
git -C mesa clean -qfd; git -C mesa checkout -q -- . 2>/dev/null || true
for p in "$REPO"/patches/mesa/*.patch; do git -C mesa apply "$p"; done
if [ ! -f mesa-build/build.ninja ]; then
    meson setup mesa-build mesa -Dvulkan-drivers=virtio -Dgallium-drivers= \
        -Dopengl=false -Dglx=disabled -Degl=disabled -Dgles1=disabled \
        -Dgles2=disabled -Dshared-glapi=disabled -Dplatforms= \
        -Dallow-fallback-for=libdrm -Dbuildtype=release
fi
ninja -C mesa-build >/dev/null

# container self-heal: NVIDIA vulkan ICD + glvnd EGL vendor manifests
if [ ! -e /usr/share/vulkan/icd.d/nvidia_icd.json ] && [ -e /usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0 ]; then
    mkdir -p /usr/share/vulkan/icd.d
    printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libGLX_nvidia.so.0","api_version":"1.3.242"}}\n' \
        > /usr/share/vulkan/icd.d/nvidia_icd.json
fi
if [ ! -e /usr/share/glvnd/egl_vendor.d/10_nvidia.json ] && [ -e /usr/lib/x86_64-linux-gnu/libEGL_nvidia.so.0 ]; then
    mkdir -p /usr/share/glvnd/egl_vendor.d
    printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0"}}\n' \
        > /usr/share/glvnd/egl_vendor.d/10_nvidia.json
fi

echo "=== compile tests"
mkdir -p tests-bin && cd tests-bin
gcc -O1 -o vnprobe "$REPO/tests/vnprobe.c" -lvulkan
gcc -O1 -o nvmods  "$REPO/tests/nvmods.c"  -lvulkan
gcc -O1 -o vkmem   "$REPO/tests/vkmem.c"   -lvulkan
gcc -O1 -o vkfmt   "$REPO/tests/vkfmt.c"   -lvulkan
gcc -O1 -o nvalloc "$REPO/tests/nvalloc.c" -lEGL -lGLESv2
cp "$REPO/tests/vnprobe.spv" .

echo "=== start venus server"
SOCK=/tmp/wdnv-venus.sock
rm -f "$SOCK"
RENDER_SERVER_EXEC_PATH=~/wdnv/virgl-build/server/virgl_render_server \
    ~/wdnv/virgl-build/vtest/virgl_test_server --venus --multi-clients \
    --socket-path "$SOCK" &> ~/wdnv/server.log &
SRV=$!
sleep 2
kill -0 $SRV || { echo "RESULT server: FAIL (died at start)"; cat ~/wdnv/server.log; exit 1; }

ICD=$(ls ~/wdnv/mesa-build/src/virtio/vulkan/virtio_devenv_icd.*.json)
fails=0
run_both() {
    local name=$1; shift
    "./$@" >/dev/null 2>&1 && echo "RESULT $name direct: PASS" || { echo "RESULT $name direct: FAIL($?)"; fails=$((fails+1)); }
    VK_ICD_FILENAMES="$ICD" VN_DEBUG=vtest VTEST_SOCKET_NAME="$SOCK" "./$@" >/dev/null 2>&1 \
        && echo "RESULT $name vtest: PASS" || { echo "RESULT $name vtest: FAIL($?)"; fails=$((fails+1)); }
}
run_both vnprobe vnprobe vnprobe.spv
run_both nvmods nvmods
run_both vkmem vkmem
run_both vkfmt vkfmt
# nvalloc is vtest-native: exercises VCMD_RESOURCE_ALLOC_GPU (our gralloc
# allocator) + EGL import as GL_TEXTURE_2D (the KWin display criterion)
out=$(./nvalloc "$SOCK" 2>&1) && echo "RESULT nvalloc(gpu-alloc+egl): PASS" \
    || { echo "RESULT nvalloc(gpu-alloc+egl): FAIL"; echo "$out" | tail -5; fails=$((fails+1)); }

kill $SRV 2>/dev/null || true
echo "=== SERVER LOG (errors only):"
grep -iE "error|fail" ~/wdnv/server.log | head -5 || true
echo "=== FULLSTACK DONE fails=$fails"
exit $fails
