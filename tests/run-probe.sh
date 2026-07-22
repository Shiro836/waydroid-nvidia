#!/bin/sh
# waydroid-nvidia GPU import probe — run on any box with an NVIDIA GPU.
# Works in docker containers too (screens the host via /proc and /sys first).
# Exit: 0 = GPU fine for the stack, 1 = import quirks found (the bug!),
#       2 = broken environment, 3 = host untestable (modeset off / closed KM).
cd "$(dirname "$0")"

echo "== kernel module:"
head -1 /proc/driver/nvidia/version 2>/dev/null || echo "  (no /proc/driver/nvidia — driver not loaded?)"
if head -1 /proc/driver/nvidia/version 2>/dev/null | grep -qv "Open Kernel Module"; then
    echo "== UNTESTABLE: closed kernel module — no dma_buf support. Pick another host."
    exit 3
fi

echo "== nvidia-drm modeset:"
ms=$(cat /sys/module/nvidia_drm/parameters/modeset 2>/dev/null || echo unreadable)
echo "   $ms"
if [ "$ms" = "N" ]; then
    echo "== UNTESTABLE: nvidia-drm.modeset=0 on the host."
    echo "   dma_buf paths are disabled; this host can't run the test. Pick another."
    exit 3
fi
# unreadable (root-only on newer drivers) or missing: fall through — the
# probe itself detects the condition via the absent dma_buf extension

echo "== driver:"
nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || true

# docker containers often miss the vulkan loader / ICD manifest — self-heal
if ! ldconfig -p 2>/dev/null | grep -q libvulkan.so.1; then
    command -v apt-get >/dev/null && { apt-get update -qq; apt-get install -qq -y libvulkan1 libxext6 libx11-6 libglx0 >/dev/null 2>&1; }
fi
if [ ! -e /usr/share/vulkan/icd.d/nvidia_icd.json ] && [ -e /usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0 ]; then
    mkdir -p /usr/share/vulkan/icd.d
    printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libGLX_nvidia.so.0","api_version":"1.3.242"}}\n' \
        > /usr/share/vulkan/icd.d/nvidia_icd.json
fi

echo "== probe:"
./nvimportprobe-portable
rc=$?
echo "== exit $rc  (0=fine, 1=QUIRK FOUND, 2=env broken, 3=host untestable)"
exit $rc
