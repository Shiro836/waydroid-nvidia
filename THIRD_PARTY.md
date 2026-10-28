# Third-party sources

Everything below is fetched at a pinned revision by the named recipe; nothing third-party is vendored except
where the "Used in" column says so. The recipe file is authoritative for the exact version. Licences: [LICENSE](LICENSE).

| Source | Pinned version | Used in |
|---|---|---|
| [nvidia-prolapse](https://github.com/Shiro836/nvidia-prolapse) — the driver: NVK over the Rust RM backend, `nvrm`, Vulkan HAL, gralloc, Codec2 decoder, bionic libdrm | `NVIDIA_PROLAPSE_REF` in `vendor-image/pins.env` (`tools/driver_source.py`) | built by its own `build.sh` (`packaging/native/build-native.sh`); its third-party sources (Mesa NVK/NAK/nil/gallium, minigbm, AOSP, libdrm, NVIDIA open headers, libvpx, …) and licences are listed in that repository |
| Mesa (Venus) | `a8ce4d82` | Venus guest driver, `patches/mesa/`, `build/mesa/` (`packaging/ci/pins.env`) |
| [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) | `dc35e4db` | Venus host renderer, `patches/virglrenderer/` |
| [ANGLE](https://chromium.googlesource.com/angle/angle) | `c1a25085` | guest GLES for all modes, `build/angle/`, `angle/patches/` |
| [Waydroid](https://github.com/waydroid/waydroid) | `a33a5c0b` | `patches/waydroid/`, `waydroid/0001-lxc-nvidia-native-mounts.patch`, `dev/build-waydroid` |
| [android_hardware_waydroid](https://github.com/waydroid/android_hardware_waydroid) | `a3e42399` | `hwcomposer/` (+ `hwcomposer/patches/`) |
| [android_external_minigbm](https://github.com/waydroid/android_external_minigbm) (Waydroid fork) | `16844bab` | Venus gralloc, hwcomposer headers (the driver's gralloc pins its own) |
| [LineageOS](https://github.com/LineageOS/android) 20 + Waydroid device/vendor trees | manifest pinned to 2026-09-27 (`vendor-image/manifests/`) | `vendor-image/` (vendor image from source), `build/lineage-20/` (SurfaceFlinger) |
| Waydroid images (LineageOS 20, x86_64) | system GAPPS + vendor MAINLINE 20260927 (sha256 in `hwcomposer/pins.env`) | link stubs for hwcomposer and the driver build, `vendor-image/` reference comparison |
| [WayDroid-ATV](https://github.com/WayDroid-ATV) (`waydroid-builds`, `android_vendor_waydroid`) | not used in builds | reference for vendor-module packaging and A14+ images (`vendor-image/notes.md`) |
| AOSP (`android.googlesource.com/platform/...`) | android-13 era commits: libhidl, libhwbinder, libfmq, fmtlib, frameworks/native, hardware/interfaces, system/tools/hidl, libbase, libhardware, system/core; frameworks/av `95be9bad` (tm-qpr3); libion, libdmabufheap (android-13.0.0_r79); LineageOS system_logging `373d54e7` | hwcomposer (`hwcomposer/pins.env`), EGL interface headers |
| [libdrm](https://gitlab.freedesktop.org/mesa/drm) | 2.4.133 (hwcomposer headers) | hwcomposer; the guest's bionic libdrm comes from the driver (glibc-shim mode checks it: `nvidia-prolapse/shim/libdrm-census`) |
| wayland 1.25.0, wayland-protocols 1.49, libffi 3.4.6, libxkbcommon 1.7.0 | sha256 in `hwcomposer/pins.env` | hwcomposer (static, bionic) |
| Android NDK | r29 (`packaging/ci/pins.env`, `hwcomposer/pins.env`); r30 for `nvidia-prolapse` shim/HAL defaults | all guest builds |
| NVIDIA [egl-x11](https://github.com/NVIDIA/egl-x11), [eglexternalplatform](https://github.com/NVIDIA/eglexternalplatform), [libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd) | egl-x11 `328a5e1b` (v1.0.6-5, its `src/base` compiled in, Apache-2.0), eglexternalplatform `cf9f1058`, libglvnd `faa23f21`, frameworks/native `2fe7eddf` | `nvidia-prolapse/egl` (glibc-shim mode's NVIDIA GLES front and platform) |
| [Magisk](https://github.com/topjohnwu/Magisk) (module format) | not shipped | `mcpe-vv-module/` is a Magisk module the user installs |
| [waydroid_script](https://github.com/casualsnek/waydroid_script) | not shipped | used by testers to install libhoudini (ARM translation); not called by this repo |
| [quinovax/waydroid-nvidia](https://github.com/quinovax/waydroid-nvidia) `packaging/install-from-release.sh` (fork) | main, read 2026-10-16 | ideas reused in `packaging/install.sh`: distro-family detection, per-family dependency lists, one SHA256SUMS line required per asset, absolute python interpreter for `waydroid.py` |
| [ErenEksen/waydroid-nvidia-optimized](https://github.com/ErenEksen/waydroid-nvidia-optimized) `install.sh` (fork) | main, read 2026-10-16 | idea reused in `packaging/install.sh`: verify-only `--check` before installing |
