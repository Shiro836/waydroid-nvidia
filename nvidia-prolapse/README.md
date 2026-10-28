# nvidia-prolapse: NVIDIA GPU drivers inside Android

The driver parts that don't depend on Waydroid. They run in Android (bionic) processes and talk to the host's
`/dev/nvidia*` directly. Waydroid integration lives in `../waydroid/` and `../vendor-image/`.

The driver itself lives in its own repository, [nvidia-prolapse](https://github.com/Shiro836/nvidia-prolapse):
the Rust RM backend, NVK/NAK, the `nvrm` GLES driver, the Vulkan HAL (`hal/`), the minigbm `gbm_mesa` backend
(`gralloc/`), the Codec2 decoder (`media/`) and bionic libdrm (`libdrm/`). This repository builds it at the commit
`../vendor-image/pins.env` pins (`../tools/driver_source.py`: `build/nvidia-prolapse-src`, or a local checkout
with `NVIDIA_PROLAPSE_SRC`). `<driver checkout>` below means that directory.

`glibc-shim` mode, which loads the host's unmodified NVIDIA libraries (glibc builds) into bionic processes:

- `shim/`: the glibc-to-bionic adapter libraries. `shim/DESIGN.md` explains how they work.
- `egl/`: NVIDIA's own EGL/GLES as an Android EGL driver, with an external platform for Android windows.
  `egl/README.md`.
- `shim/libdrm-census/`: checks the driver's bionic libdrm for NVIDIA's ICD (it needs libdrm for dma-buf export):
  every `drm*` name each supported driver branch references.
- `probe/`: small compute and GLES probes for checking a setup.
- The Vulkan HAL in front of NVIDIA's ICD (own `VK_ANDROID_native_buffer`, ASTC emulation) and the gralloc backend
  are the driver checkout's `hal/` and `gralloc/` glibc-shim targets (`hal/AHB-CONTRACT.md` there).

Build and test, per component:

```sh
make -C shim && make -C shim ARCH=x86        # 64- and 32-bit
make -C shim/libdrm-census && make -C shim/libdrm-census ARCH=x86
make -C <driver checkout>/hal && make -C <driver checkout>/hal test   # host + Android builds, real-GPU pixel tests
make -C <driver checkout>/gralloc && make -C <driver checkout>/gralloc android32
```

In `glibc-shim` mode NVIDIA's libraries are loaded unmodified from the host and must match the kernel module's
version exactly. In every mode a failure is loud and names a remedy; nothing falls back silently.
