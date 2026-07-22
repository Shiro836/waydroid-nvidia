# dev/ — the change → test loop

This repo is **patches + net-new source + build glue** (the AUR artifact). The
code is edited and built in three external trees; these scripts orchestrate them
so you never think about where anything lives.

| Role | Path | What it is |
|---|---|---|
| **This repo** | `~/repos/waydroid-nvidia` | AUR source of truth: `patches/`, `src/`, build glue, `dev/` |
| **Runtime** | `~/repos/waydroid` | the waydroid python checkout that actually runs (`waydroid.py`) |
| **Build trees** | `~/repos/waydroid-nv/{mesa,virglrenderer,hwcomposer-src,angle-src}` | native components; hold the WIP the patches capture |

All paths live in **`dev/env.sh`** and are overridable from the environment
(`WAYDROID_SRC=… dev/restart`).

## The loop

```sh
# 1. edit code in the relevant build tree (e.g. ~/repos/waydroid-nv/mesa)
# 2. one command: build -> deploy -> full restart -> health -> measure
dev/iter mesa          # or: hwc | virgl | angle | none

# 3. read the frame numbers, iterate. when a change is good:
dev/sync-patches       # regenerate patches/ + src/ from the trees (AUR honesty)
git add -A patches/ src/ && git commit
```

`dev/iter none` skips build/deploy — use it after a prop/cfg change.

## Individual commands

| Command | Does |
|---|---|
| `dev/build <comp>` | compile one component (`mesa`/`virgl`/`hwc`/`gralloc`/`angle`/`all`) via the shared recipe `build/<comp>/build.sh` |
| `dev/deploy <comp>` | install the built artifact via the `wd-deploy` root helper |
| `dev/restart` | the **only** allowed stack bounce (venus → container → session; never kill SF alone) |
| `dev/health` | post-restart checklist: boot, renderer, crash count, KWin errors, hwc diag |
| `dev/measure [pkg]` | canonical launcher-fling frame bench (median/p90/p95/p99/janky) |
| `dev/monitor [seconds] [label] [pkg]` | correlated app frames + per-context Venus traffic + GPU/health capture |
| `dev/sync-patches` | regen `patches/` + `src/` from the trees; prints a review diff, commits nothing |
| `dev/status` / `dev/logs` | truth snapshot / aggregated logs (`-f`, `gfx`, `crash`, `units`) |
| `dev/wd` / `dev/wdu` | run the runtime checkout as root / user |

## One recipe, dev + AUR share it

`dev/build` doesn't hardcode build commands — it calls `build/<comp>/build.sh`
against the persistent tree. The AUR validator `packaging/aur/reproduce.sh`
calls the *same* recipes against a fresh checkout at `patches/<comp>/BASE`. So
what you dev-test is what a stranger builds. Run the clean-room check anytime:

```sh
packaging/aur/reproduce.sh mesa virgl     # fully reproducible components
```

See `packaging/aur/README.md` + `PREREQS.md` for the full story and the gaps.

## Rules baked in (learned the hard way — see STATE.md)

- **Never** bounce composer/SurfaceFlinger individually — SF zombies at
  `flips=0`. Only `dev/restart`.
- After deploying a **guest** `.so` (mesa/hwc/angle), a full `dev/restart` is
  mandatory — the bind-mounted lib keeps its old inode otherwise.
- `virgl` is **host-side**: `dev/deploy virgl` restarts `wd-venus`; still do a
  full restart so the stack reconnects.
- No claims without numbers (`dev/measure`) or a guest `screencap` — never host
  screenshots (privacy).
- `sudo -n` only; a bare `sudo` without a tty burns PAM attempts and faillock
  locks the account. `dev/*` refuse rather than prompt.

## Patch-regen anchors (`dev/sync-patches`)

Verified ancestors; committed commits → numbered patches, uncommitted WIP → the
trailing `wip` patch:

| Component | BASE | committed → | WIP → |
|---|---|---|---|
| mesa | `a8ce4d8` | `0001`,`0002` | `0003-wip-ahb-memory-steering` |
| virglrenderer | `dc35e4d` | `0001`–`0003` | `0004-wip-gpu-alloc-and-global-priority` + net-new `src/` |
| hwcomposer | `7750307` | — | `0001-wip-nvidia-fixes` |
| waydroid | `a33a5c0` | — | `0001-nvidia-integration` |
