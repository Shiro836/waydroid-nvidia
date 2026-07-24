# Troubleshooting

Most field failures so far have been environment/config issues that
`waydroid-nvidia-setup` now detects or auto-fixes — **re-running
`sudo waydroid-nvidia-setup` is the first move for almost everything.**
It refuses loudly (with the fix in the message) on a missing NVIDIA node,
`nvidia-drm.modeset=0`, or a pre-minigbm vendor image, and it auto-removes
stale gralloc overrides left in `waydroid.cfg` by old installs.

## Quick health check

```sh
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
# healthy: GLES: ... ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))
sudo waydroid shell getprop ro.hardware.gralloc
# healthy: minigbm_gbm_mesa
sudo waydroid shell getprop ro.product.cpu.abilist
# ARM32 translation needs x86 plus armeabi-v7a in this list
```

## One-command debug capture

Restarts the stack, lets it run/crash for 12 s, and collects everything a bug
report needs into one file:

```sh
sudo -v; ( nvidia-smi -L; systemctl --user restart wd-venus; waydroid session stop; (waydroid session start &); sleep 12; echo "=== wd-venus journal:"; journalctl --user -u wd-venus --since "-1 min" --no-pager; echo "=== full logcat:"; sudo waydroid shell -- logcat -d ) > wdnv-debug.txt 2>&1
```

Skim it for personal data (a fresh crash-looping boot normally contains
none), then attach it to an issue.

## Known failure modes

| Symptom | Cause | Fix |
|---|---|---|
| `waydroid session start` refuses: "Venus render server socket … not accepting connections" | wd-venus isn't running in *your* user session | `systemctl --user enable --now wd-venus.service` (no sudo — sudo targets root's user manager) |
| SurfaceFlinger crash-loops with `Unable to generate SkSurface`, guest uses `allocator@2.0` / "Using fallback gralloc implementation" | Stale gralloc override in `waydroid.cfg` (e.g. the old software-rendering workaround `ro.hardware.gralloc=default`), or a pre-minigbm vendor image | Re-run `sudo waydroid-nvidia-setup` — it removes the override / refuses the old image and tells you |
| Setup refuses: vendor image too old | Migrated install carrying an ancient `vendor.img`; **`waydroid init -f` does NOT re-download the vendor** (it trusts a stored timestamp even if the file is deleted) | `sudo sed -i 's/^vendor_datetime.*/vendor_datetime = 0/' /var/lib/waydroid/waydroid.cfg && sudo rm -f /var/lib/waydroid/images/vendor.img && sudo waydroid init -f` — then re-run setup. If you use gapps, `waydroid init -f -s GAPPS` (plain `-f` silently resets the channel to VANILLA) |
| Setup refuses: `nvidia-drm.modeset` | modeset=0 disables **all** DMA-BUF support in the driver — nothing in this stack can work | Add `nvidia_drm.modeset=1` to kernel params or modprobe.d, reboot |
| Cursor invisible / screenshots black, everything else fine | No `/dev/udmabuf` access (CPU-mappable buffer path); wd-venus journal says exactly this | The package's udev rule grants it to the seated user — re-log-in once after install. Headless: `setfacl -m u:USER:rw /dev/udmabuf` |
| Images in `/etc/waydroid-extra/images` or `/usr/share/waydroid-extra/images` | waydroid silently prefers preinstalled images over downloads | Remove/move them if you want OTA images |
| ARM32-only app runs on LLVM/Lavapipe while the desktop is accelerated | The 32-bit `vendor/lib/hw/vulkan.virtio.so` is missing or is a renamed `vulkan.lvp.so` | Install a dual-ABI release and re-run `sudo waydroid-nvidia-setup`; it requires ELF32/`EM_386` for every `vendor/lib` payload |

For a translated app, confirm its process maps the 32-bit stack (replace the
package name if needed):

```sh
sudo waydroid shell -- sh -c '
  pid=$(pidof com.tencent.qqmusic | cut -d" " -f1)
  grep -E "/vendor/lib/(egl|hw)/(libGLESv2_angle|vulkan\.virtio)\.so" /proc/$pid/maps'
```

While playing audio or animating the UI, `nvidia-smi pmon` should show work in
the host renderer and logcat should not name `llvmpipe` or `Lavapipe`.

## Is my GPU/driver combination OK?

Requirements: NVIDIA **open kernel modules** (Turing or newer — the open KM
does not support older GPUs), driver 595.71+, `nvidia-drm.modeset=1`. The
stack is validated on Turing, Ampere, Ada and Blackwell.

To test any machine in ~30 seconds without installing anything, build the
probe from this repo and run it — it checks the exact buffer-sharing paths
the stack depends on and names whatever is missing:

```sh
gcc -O1 -o nvimportprobe tests/nvimportprobe.c -ldl
tests/run-probe.sh    # or ./nvimportprobe directly
```

Exit 0 = this machine can run the stack. Exit 3 = it can't, and the output
says why (closed kernel module, modeset off, …).
