# Development workflow

How to hack on the stack with a fast edit → build → deploy → measure loop.
For one-off builds see [`building.md`](building.md); this page is for
iterating.

## One-time setup

### 1. Working trees

The dev loop builds from persistent upstream checkouts (incremental builds),
not fresh clones. Default layout (every path overridable via `dev/env.sh`
environment variables):

```sh
WNV=~/repos/waydroid-nv          # prereq/tree root
git clone https://gitlab.freedesktop.org/mesa/mesa.git            $WNV/mesa
git clone https://gitlab.freedesktop.org/virgl/virglrenderer.git  $WNV/virglrenderer
git clone https://github.com/waydroid/android_hardware_waydroid   $WNV/hwcomposer-src
git clone https://github.com/waydroid/waydroid                    ~/repos/waydroid
```

Check each out at its pinned base (`patches/*/BASE`, full SHAs in
`packaging/ci/pins.env`) and apply the matching `patches/` series. For the
hwcomposer prerequisites (AOSP headers, hidl-gen, static deps, image libs),
run the same script CI uses:

```sh
build/hwcomposer/provision.sh "$WNV" "$WNV/hwcomposer-src"
```

You also need the Android NDK (`NDK`, default `/opt/android-ndk`) and, for
SurfaceFlinger work, a LineageOS 20 tree (`build/lineage-20/sync.sh` — ~150 GB;
sync with `-j4`, the mirrors throttle higher parallelism).

### 2. Privileged helper (recommended)

Deploying guest libraries needs root writes into `/var/lib/waydroid`. Rather
than broad sudo rules, install the validating helper and allow only it:

```sh
sudo install -Dm755 packaging/host/wd-deploy /usr/local/sbin/wd-deploy
```

Before the first `dev/deploy` of a guest library, install the current patched
Waydroid package and run `sudo waydroid-nvidia-setup` once. That selects
`nvidia_guest_layout=2` and regenerates `config_nodes` with the path-preserving
`vendor/lib` and `vendor/lib64` mounts. The helper checks both settings and
rejects a deploy that would otherwise write to an inactive path.

sudoers pattern (via `visudo -f /etc/sudoers.d/waydroid-dev`):

```
yourname ALL=(root) NOPASSWD: /usr/local/sbin/wd-deploy *
yourname ALL=(root) NOPASSWD: /usr/bin/lxc-attach -P /var/lib/waydroid/lxc -n waydroid *
yourname ALL=(root) NOPASSWD: /usr/bin/lxc-info *
yourname ALL=(root) NOPASSWD: /usr/bin/systemctl start waydroid-container.service, \
    /usr/bin/systemctl stop waydroid-container.service, \
    /usr/bin/systemctl restart waydroid-container.service
```

Deliberately **not** passwordless: `waydroid upgrade` (executes repo code as
root) and anything with wildcard filesystem writes — `wd-deploy` exists
precisely so `cp`/`chmod`/`systemd-run` wildcards aren't needed (those are
trivial arbitrary-root escalations).

## The loop

```sh
dev/build <mesa|virgl|hwc|gralloc>   # incremental build via build/<comp>/build.sh
dev/deploy <component>               # push into /var/lib/waydroid via wd-deploy
dev/restart                          # full stack restart (see rule below)
dev/health                           # window exists, GLES on Venus, 0 crashes, 0 KWin errors
dev/measure                          # canonical launcher-fling bench (frame percentiles)
dev/iter                             # all of the above in sequence
dev/status | dev/logs [gfx|units]    # quick state / filtered logs
dev/sync-patches                     # regenerate patches/ + src/ from the trees
```

Iron rules learned the hard way:

- **Always restart the whole stack** (`dev/restart`); never kill
  composer/SurfaceFlinger individually — SF zombies with `flips=0`.
- **After deploying any guest `.so`, a full container restart is mandatory** —
  bind-mounted libraries keep their old inode for running processes.
- Bind-mount targets must be real files in the image, never symlinks.
- New mounts/props go into the *generators* (`lxc.py`,
  `waydroid.cfg [properties]`) — anything hand-edited into `config_nodes` or
  `waydroid_base.prop` is silently lost on `waydroid upgrade`.
- One change at a time → restart → measure; claims need numbers or a guest
  screencap (`waydroid shell screencap`), not "looks fine".
- `patches/` is generated output (`dev/sync-patches`) — never hand-edit;
  `src/` is canonical even where build scripts copy it into the trees.

## Useful recipes

Run commands inside the guest:

```sh
sudo lxc-attach -P /var/lib/waydroid/lxc -n waydroid --clear-env \
  -v PATH=/system/bin -- /system/bin/sh -c 'getprop ro.hardware.gralloc'
```

Health checklist after a restart:

```sh
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES        # ANGLE .. Venus .. NVIDIA
sudo waydroid shell -- logcat -d -b crash | grep -c Cmdline   # 0
journalctl --user -t kwin_wayland | grep -c GL_INVALID        # 0
journalctl --user -u wd-venus -f                              # client connections live
```

Canonical smoothness bench (numbers go in your notes, compare
before/after):

```sh
sudo waydroid shell -- sh -c '
  dumpsys gfxinfo com.android.launcher3 reset >/dev/null
  for i in 1 2 3 4 5; do input swipe 2000 700 500 700 100; sleep 0.5; \
    input swipe 500 700 2000 700 100; sleep 0.5; done
  dumpsys gfxinfo com.android.launcher3 | grep -E "Total frames|Janky|50th|90th|99th"'
```

Guest test binaries (host-side probes live in `tests/`):

```sh
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android34-clang \
  -O1 -o out-android test.c -lEGL -lGLESv2 -landroid -llog
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android34-clang \
  -O1 -o out-android32 test.c -lEGL -lGLESv2 -landroid -llog
# deliver via: cp out-android ~/.local/share/waydroid/data/local/tmp/  (= guest /data/local/tmp)
```

`dev/build mesa` and `dev/build angle` build both ABIs. Append `-x86` or
`-x86_64` to build or deploy just one, for example `dev/deploy mesa-x86`.

Grow the guest log buffer per boot (`persist.logd.size` does not stick):
`sudo waydroid shell -- logcat -G 16M`.
