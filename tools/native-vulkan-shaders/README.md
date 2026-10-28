# Independent native Vulkan compute probe

`../native-vulkan-probe.c` uses public Vulkan 1.0 entry points obtained from an
explicit library path. It has no knowledge of RM handles, GPU packets, compiler
internals, or the driver's private diagnostic ABI. The shaders are ordinary
GLSL compiled to SPIR-V; the driver must compile their supplied bytes.

Build from the repository root (outputs remain in ignored `build/`):

```sh
mkdir -p build/native-vulkan-probe
cc -std=c11 -Wall -Wextra -Werror tools/native-vulkan-probe.c -ldl \
  -o build/native-vulkan-probe/probe-host
build/hwcomposer-work/ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang \
  -std=c11 -Wall -Wextra -Werror tools/native-vulkan-probe.c -ldl \
  -o build/native-vulkan-probe/probe-android
cc -std=c11 -Wall -Wextra -Werror tools/native-vulkan-copy-indirect.c -ldl \
  -o build/native-vulkan-probe/copy-indirect-host
glslangValidator -V --target-env vulkan1.0 tools/native-vulkan-shaders/generate.comp \
  -o build/native-vulkan-probe/generate.spv
glslangValidator -V --target-env vulkan1.0 tools/native-vulkan-shaders/transform.comp \
  -o build/native-vulkan-probe/transform.spv
spirv-val --target-env vulkan1.0 build/native-vulkan-probe/generate.spv
spirv-val --target-env vulkan1.0 build/native-vulkan-probe/transform.spv
```

Run a host-built driver:

```sh
build/native-vulkan-probe/probe-host \
  --driver /absolute/path/to/source-built-driver.so \
  --shader build/native-vulkan-probe/generate.spv \
  --transform build/native-vulkan-probe/transform.spv
```

For bionic, use the Android executable and Android driver. When using the
extracted Android linker, both executable and driver paths must be absolute:

```sh
repo=$(pwd)
driver=$(python3 tools/driver_source.py path)    # the driver checkout, built
NV_NATIVE_COMPILER_PATH="$driver/build/nak-android/src/nouveau/native-bridge/libnv_native_nak.so" \
LD_LIBRARY_PATH="$driver/build/bionic-runtime64/lib64:$driver/build/nvk-android/runtime:$driver/build/android-work/deps/image-rootfs/system/lib64" "$driver/build/bionic-runtime64/linker64" \
  "$repo/build/native-vulkan-probe/probe-android" \
  --driver "$driver/build/nv-rm-rust/x86_64-linux-android/release/libnv_bionic_native.so" \
  --shader "$repo/build/native-vulkan-probe/generate.spv" \
  --transform "$repo/build/native-vulkan-probe/transform.spv"
```

The Android probes share `../native-vulkan-runtime.h`: they initialize the
platform `libcutils` tracing runtime before descriptor accounting and close
liblog's process logger after Vulkan teardown. The runtime directories above
therefore include the extracted Android platform libraries. The persistent
`trace_marker` descriptor is checked separately through repeated initialization;
Vulkan resource cleanup still requires exact descriptor equality.

An optional desktop loader check passes `--loader --driver /absolute/libvulkan.so`
and requires `VK_DRIVER_FILES=/absolute/icd.json`. This exercises desktop ICD
discovery; it is not the Android HAL loading protocol. `--allow-stock-driver`
allows a positive-control run against the stock NVIDIA driver. Without that
explicit flag, a mapped NVIDIA userspace library or glibc shim fails the test.
`--negative` additionally checks that an unknown device extension is rejected
with `VK_ERROR_EXTENSION_NOT_PRESENT`, then requests one core feature the driver
reported unsupported and expects `VK_ERROR_FEATURE_NOT_PRESENT`. The feature
check is reported as not applicable if every core feature is advertised.

`../native-vulkan-copy-indirect.c` (host only: the NDK r29 Vulkan headers predate
`VK_KHR_copy_memory_indirect`) records `vkCmdCopyMemoryIndirectKHR` on a
compute-only queue family, a transfer-write to shader-read barrier, then a
dispatch of `transform.spv` that reads the copied words; 1000 iterations, every
output word checked. It exits 77 when the device has no compute-only family:

```sh
build/native-vulkan-probe/copy-indirect-host \
  --driver /absolute/path/to/libvulkan_nouveau.so \
  --transform build/native-vulkan-probe/transform.spv
```

For Mesa ICDs exporting `vk_icdGetInstanceProcAddr`, add `--icd` to direct
invocations. The driver checkout's `tests/vulkan/run.py` prepares the current full-NVK
compute, graphics, interop, host copy-indirect and HAL commands, and defaults to a dry run.

The probe selects a physical device by NVIDIA vendor ID and a queue by its
compute capability. It enables no extensions or optional features and requires
host-visible, coherent buffer memory. It checks:

- Two distinct shaders, six counts (1, 63, 64, 65, 257, 1024), and partial groups.
- Different push constants between consecutive dispatches in one command buffer.
- Two storage-buffer bindings, with buffers rebound between rounds.
- Nonzero buffer-memory binding and descriptor offsets, respecting queried limits.
- Shader-write/read barriers and a transfer into a third allocation.
- Every payload word and every surrounding allocation canary (4,422 payload words
  across all rounds); integer arithmetic deliberately wraps at 32 bits.
- Unsignalled fence status, zero-timeout waits, submitted completion, and reset.
- Unchanged file-descriptor count after destroying all Vulkan objects and
  unloading the library, including any failed device creations under `--negative`.

A pass demonstrates this compute/copy subset only. It does not establish Vulkan
conformance, graphics, images, presentation, Android app sandboxing, or Waydroid
integration. Errors terminate the process; the kernel reclaims its resources.
