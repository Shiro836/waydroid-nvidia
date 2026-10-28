# Pending Vulkan sync-file regression

`native-vulkan-async.c` uses public Vulkan entry points from an explicit ICD.
It selects an NVIDIA vendor device and verifies asynchronous release, binary
semaphore reuse, fence reset through export, and resource retirement. The probe
contains no RM calls, GPU packets, private driver interface, or shader spin loop.
It needs an independent producer that supplies a pending Linux `sync_file`.

```sh
mkdir -p build/native-vulkan-probe
cc -std=c11 -Wall -Wextra -Werror tools/native-vulkan-async.c \
  -ldl -pthread -o build/native-vulkan-probe/async-host
```

For bionic, replace `cc` with the NDK's `x86_64-linux-android33-clang` or
`i686-linux-android33-clang`, and name the outputs `async-android` and
`async-android32`. No Vulkan loader library is linked. Use the matching Android
ICD and extracted bionic linker/runtime when running outside Android.

## Producer protocol

The producer creates a real, initially unsignaled sync-file fence and a pipe,
then launches the probe with both descriptors inherited across `exec`:

```sh
PROBE --driver /absolute/path/to/ICD.so --gate-fd N --release-fd M
```

- `N` is a borrowed descriptor for the pending fence. The probe duplicates it
  for a temporary Vulkan semaphore import and leaves the original open.
- `M` is the borrowed pipe write end. One byte (`R`) requests that the producer
  signal its fence. The producer keeps the pipe read end and closes its copy
  of the write end after launching the child.
- The producer must also signal the fence on pipe EOF or after its own bounded
  watchdog, even if the child crashes. The tested fixture uses two seconds.
  It must keep the fence's backing resources alive until the child exits.
- The probe releases the producer on setup failures and has a separate 750 ms
  watchdog covering submission/export. A synchronous driver therefore fails
  the pending test but can complete its finite work after the gate opens.
- The fence must still be unsignaled immediately before submission. Fixture
  expiration during startup is a failure, not an asynchronous success.

An ordinary pipe/eventfd cannot substitute for `N`. A fixture may use an
available Linux software-sync producer or another documented external fence
producer; the probe itself imposes no driver-specific producer implementation.
`--allow-stock-driver` explicitly permits a proprietary NVIDIA positive control.
Otherwise proprietary NVIDIA userspace and the glibc shim in process maps fail.

## Checks

While the external fence is pending, the probe submits two render/release
sequences: GPU marker write and binary signal, followed by a binary wait and
exportable signal/fence. It exports and immediately reuses the same semaphore
and fence objects. All four exported descriptors must be real, unsignaled
sync-files before the producer is released. Export latency is printed.

The probe imports duplicates of both old semaphore payloads and queues two more
GPU markers. After releasing the producer, all four markers must be correct,
old descriptors must complete and survive destruction of the source handles,
and the exported fence must remain reset. It then executes 1,030 small marker
submissions in windows of 32, freeing completed command buffers each window.
Every marker and allocation guard must match, and final descriptor count must
match the initial count. The submission count exercises repeated queue reuse;
only driver-side observation can establish a particular hardware ring size.

Host marker reads occur only after completion. Pending export is established by
fence polling, avoiding a host/GPU memory race. A host-set `VkEvent` is deliberately
not used as the gate: [VUID-vkSetEvent-event-09543](https://docs.vulkan.org/refpages/latest/refpages/source/vkSetEvent.html)
forbids setting an event waited on by a pending command buffer. The externally
submitted producer also respects the dependency requirement for
[SYNC_FD semaphore export](https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphoreGetFdInfoKHR.html).

This gate establishes synchronization and tiny GPU transfers. It complements
compute, offscreen graphics, DMA-BUF interop, and Android HAL tests; it does not
establish presentation or Vulkan conformance.

## Incoming error gate

`--error-gate` uses the same pending fixture and release pipe. On the release
request, the producer must complete its fence with an error instead of success.
The probe requires `vkWaitForFences` and `vkDeviceWaitIdle` to report
`VK_ERROR_DEVICE_LOST`, and independently checks a negative incoming
`SYNC_IOC_FILE_INFO` status. Successful rendering and ring checks are skipped;
object teardown and descriptor accounting remain mandatory. A successful wait
cannot pass this mode. This models a failed external producer, without asking
the GPU to execute invalid commands.

## Cross-handle conversion

`--convert`, or `NV_FENCE_CONVERT=1` inherited from the launcher, additionally
checks advertised binary semaphore conversion across two independent logical
devices. It cannot be combined with `--error-gate`. The external producer
protocol, release watchdogs and normal pending/ring checks remain unchanged.

Before creating conversion payloads, the probe checks both import/export
features, both `compatibleHandleTypes` masks, and each direction of
`exportFromImportedHandleTypes`. A missing advertised combination prints the
raw masks and returns 77. The existing Rust launcher treats any nonzero child
exit as failure, so a capability skip appears as a failed launcher test; its
child log distinguishes the reason.

The original exportable binary payload is created with **both** handle types.
Each of its two pending SYNC_FD exports follows this chain:

1. Temporarily import SYNC_FD into a binary semaphore on the first device.
2. Export that active imported payload as OPAQUE_FD.
3. Import it on the second device, permanently for the first payload and
   temporarily for the second.
4. Export it as SYNC_FD before submitting any wait on the opaque semaphore.
5. Temporarily import the resulting sync-file copy and GPU-wait it before a
   marker write on the second device.

Both round-trip descriptors must initially be real, pending sync-files. The
consumer's finite completion wait must time out before producer release.
After release, both sync-files must report success, consumer markers and guards
must match, and descriptors must remain valid after payload destruction. A
fresh signal/wait cycle also exercises the permanent payload restored after
the temporary opaque import was consumed by copy export.

SYNC_FD imports are temporary, as required by
[VkImportSemaphoreFdInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportSemaphoreFdInfoKHR.html).
The external fixture represents an already-submitted producer, satisfying the
dependency rule for
[semaphore SYNC_FD export](https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphoreGetFdInfoKHR.html).
Future host-timeline signals and host events cannot substitute for that fixture
in this conversion test. Creation compatibility and conversion permissions are
separate fields in
[VkExternalSemaphoreProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalSemaphoreProperties.html).

The stock RTX 5090/615.71.09 control skips conversion: SYNC_FD reports features
`0x3`, compatible/imported masks `0x10`; OPAQUE_FD reports features `0x3`, masks
`0x1`. These disjoint masks prohibit the requested conversion on that driver.
The probe returned 77 and the strict fixture returned 101, recorded in
`build/fence-design/stock-convert.log`; no unsupported conversion was attempted.
This is a capability observation, not a stock conversion pass or a native
runtime result.

The native adapter now deliberately exposes the same boundary through two
Mesa sync types: binary/timeline OPAQUE_FD and binary-only SYNC_FD. Its expected
conversion result is also a capability skip, not a successful round trip.
The probe remains useful for detecting accidental re-advertisement of
cross-type conversion. Same-type opaque sharing and sync-file transfer have
separate positive regressions; a live exporter worker must not be treated as
an external payload lifetime guarantee.

With conversion disabled, both the edited probe and an independently compiled
unchanged `HEAD:tools/native-vulkan-async.c` pass the stock pending-export and
1,030-marker checks, then fail the same strict lifecycle descriptor check
(`6 → 29`). See `stock-async-{before,after}-convert.log` in that directory. This
pre-existing stock lifetime result is not relaxed by the conversion option.

The driver checkout's `tests/vulkan/run.py` (`tools/driver_source.py path`) prints checked commands for compute,
offscreen graphics, DMA-BUF/sync-file interop and HAL ANB/ASTC/AHB probes:

```sh
python "$(python3 tools/driver_source.py path)/tests/vulkan/run.py"
python "$(python3 tools/driver_source.py path)/tests/vulkan/run.py" --target host --suite core
```

Add `--run` only when the GPU slot is available and the async gate has passed.
Tests run serially, stop at the first failure, and retain separate logs under
its `--logs` directory. ANB uses its strict pending-release requirement.
Android AHB requires a separate real-guest gralloc test; the runner includes
only the existing host AHB fixture. The runner does not deploy or start Waydroid.

On Android, the probe initializes the public `libcutils` tracing runtime before
capturing the descriptor baseline. AOSP's `atrace_init()` opens `trace_marker`
once per process and has no shutdown API; its library reference stays alive.
A second initialization must not add descriptors. The probe also runs two extra
instance/device creation and destruction cycles after the GPU test, requiring
unchanged descriptor counts each time. On mismatch it prints descriptor targets.
This accounts for the tracing runtime explicitly while retaining strict checks
for driver-owned resources. Liblog's process logging socket is closed through
its public cleanup entry point before each comparison.
