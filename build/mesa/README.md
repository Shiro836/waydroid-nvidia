# Android guest Venus build

This directory contains the Mesa cross-build entry point for the Android
guest Vulkan HAL used by waydroid-nvidia. The build output is an Android
`libvulkan_virtio.so`; it is not a host NVIDIA library and it does not run
Waydroid by itself.

## Files and related locations

| Path | Purpose |
|---|---|
| `build/mesa/build.sh` | Builds the guest Venus HAL for `x86_64` or `x86`/i686. |
| `build/mesa/android-cross.ini.in` | ABI-specific Meson cross-file template. |
| `build/angle/args-x86.gn` | 32-bit Android ANGLE GN arguments. |
| `build/angle/args-x86_64.gn` | 64-bit Android ANGLE GN arguments. |
| `patches/waydroid/0001-nvidia-integration.patch` | Adds the NVIDIA Venus LXC mounts, including the dual-ABI `vendor/lib` and `vendor/lib64` layout. |
| `packaging/aur/waydroid-nvidia-bin/PKGBUILD` | Installs the path-preserving guest payload. |
| `packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-setup` | Validates and installs the guest payload, then regenerates Waydroid mounts. |
| `packaging/host/wd-deploy` | Validates and deploys one development artifact into the guest tree. |
| `dev/build` and `dev/deploy` | Incremental build/deploy wrappers for the development loop. |
| `docs/building.md` and `docs/dev-workflow.md` | Full clean-room and development workflows. |

The guest app ABI paths are intentionally separate:

```text
guest/vendor/lib/hw/vulkan.virtio.so       ELF32 / EM_386
guest/vendor/lib/egl/*.so                  ELF32 / EM_386
guest/vendor/lib64/hw/vulkan.virtio.so    ELF64 / EM_X86_64
guest/vendor/lib64/egl/*.so               ELF64 / EM_X86_64
```

Gralloc, hwcomposer and SurfaceFlinger remain 64-bit system components.

## Build Mesa

Run these commands from the repository root. They require an Android NDK with
both `x86_64-linux-android34-clang` and `i686-linux-android34-clang`:

```sh
ANDROID_ABI=x86_64 NDK=/opt/android-ndk \
  build/mesa/build.sh /path/to/mesa /path/to/mesa/build-android-x86_64

ANDROID_ABI=x86 NDK=/opt/android-ndk \
  build/mesa/build.sh /path/to/mesa /path/to/mesa/build-android-x86
```

The resulting files are:

```text
/path/to/mesa/build-android-x86_64/src/virtio/vulkan/libvulkan_virtio.so
/path/to/mesa/build-android-x86/src/virtio/vulkan/libvulkan_virtio.so
```

Check the ABI and SONAME before staging an artifact:

```sh
file /path/to/mesa/build-android-x86/src/virtio/vulkan/libvulkan_virtio.so
readelf -hW /path/to/mesa/build-android-x86/src/virtio/vulkan/libvulkan_virtio.so \
  | grep -E 'Class:|Type:|Machine:'
readelf -dW /path/to/mesa/build-android-x86/src/virtio/vulkan/libvulkan_virtio.so \
  | grep SONAME
```

The 32-bit result must be `ELF32`, `Intel 80386`, `DYN`, and
`libvulkan_virtio.so`. The 64-bit result must be `ELF64`,
`Advanced Micro Devices X86-64`, `DYN`, and the same SONAME.

## Development loop

After the current dual-ABI Waydroid setup has been installed once, use:

```sh
dev/build mesa       # builds both x86_64 and x86 Venus HALs
dev/deploy mesa      # deploys both ABI files
dev/restart          # full container/session restart
dev/health           # renderer and crash checks
```

Build or deploy only one ABI when iterating:

```sh
dev/build mesa-x86
dev/deploy mesa-x86
dev/build mesa-x86_64
dev/deploy mesa-x86_64
```

`dev/deploy` refuses to write if `nvidia_guest_layout=2` is not active or if
the generated `config_nodes` does not mount the requested path. Run the
current `sudo waydroid-nvidia-setup` before the first development deploy.

## Install and verify

Use a release containing the dual-ABI guest tarballs, then run:

```sh
yay -S waydroid-nvidia-bin
waydroid init
sudo waydroid-nvidia-setup
sudo systemctl enable --now waydroid-container.service
systemctl --user enable --now wd-venus.service
waydroid session start
```

Inside the guest, verify the ABI list and renderer:

```sh
sudo waydroid shell getprop ro.dalvik.vm.native.bridge
sudo waydroid shell getprop ro.product.cpu.abilist
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
```

For an ARM32-only app, the expected path is:

```text
ARM32 app -> libhoudini.so -> x86 app process -> x86 ANGLE
           -> vendor/lib/hw/vulkan.virtio.so -> Venus socket -> NVIDIA
```

The build may use Docker to pin compiler dependencies, but the resulting
library is mounted into the normal Waydroid container. Docker is not a
Waydroid runtime.
