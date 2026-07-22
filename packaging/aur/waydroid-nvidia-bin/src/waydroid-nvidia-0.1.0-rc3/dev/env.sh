#!/usr/bin/env bash
# env.sh — single source of truth for the dev loop's tree locations, build
# targets, deploy destinations and patch-regen anchors. Sourced by every dev/
# script. Everything is overridable from the environment, e.g.
#   WAYDROID_SRC=/somewhere/else dev/restart
#
# The three trees this repo orchestrates (see docs/dev-workflow.md):
#   REPO         this repo — AUR source of truth (patches/ + src/ + build glue)
#   WAYDROID_SRC the runtime waydroid python checkout (waydroid.py, live lxc.py)
#   WNV/*        native component build trees (mesa / virgl / hwcomposer / angle)

set -euo pipefail

# --- this repo (dir containing dev/); overridable for testing ---
: "${REPO:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# --- external trees ---
: "${WNV:=$HOME/repos/waydroid-nv}"
: "${WAYDROID_SRC:=$HOME/repos/waydroid}"

: "${MESA_TREE:=$WNV/mesa}"
: "${MESA_BUILD:=$MESA_TREE/build-android-x86_64}"
: "${VIRGL_TREE:=$WNV/virglrenderer}"
: "${VIRGL_BUILD:=$VIRGL_TREE/build}"
: "${HWC_TREE:=$WNV/hwcomposer-src}"
: "${HWC_BUILD:=$WNV/hwc-build}"
: "${ANGLE_TREE:=$WNV/angle-src}"

# --- toolchain ---
: "${NDK:=/opt/android-ndk}"
: "${NDK_BIN:=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin}"
: "${STRIP:=$NDK_BIN/llvm-strip}"

# --- runtime ---
: "${LXC:=-P /var/lib/waydroid/lxc -n waydroid}"
: "${VENUS_UNIT:=wd-venus.service}"
: "${CONTAINER_UNIT:=wd-container.service}"
: "${SESSION_UNIT:=wd-session.service}"
: "${DEPLOY:=/usr/local/sbin/wd-deploy}"

# --- patch-regen anchors (verified ancestors; see dev/sync-patches) ---
: "${MESA_BASE:=a8ce4d8}"
: "${VIRGL_BASE:=dc35e4d}"
: "${HWC_BASE:=7750307}"
: "${WAYDROID_BASE:=a33a5c0}"

# --- helpers ---
say()  { printf '\033[1;36m== %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!! %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31mxx %s\033[0m\n' "$*" >&2; exit 1; }

# guest shell (root inside the container), env cleaned so exec sh never fails.
# stdio goes through pipes, NOT the caller's fds: lxc-attach chowns its std
# fds to the container root (attach.c fix_stdio_permissions), which turns any
# redirect-target file into an unreadable root-owned 600 file.
guest() {
    sudo -n lxc-attach $LXC --clear-env -v PATH=/system/bin -- /system/bin/sh -c "$*" \
        < /dev/null 2> >(cat >&2) | cat
}

# The waydroid-dev sudoers allowlist grants NOPASSWD for exactly the commands
# the loop uses (lxc-attach/lxc-info/systemctl wd-container/wd-deploy) — probe
# one of those, NOT `sudo -n true`, which is not allowlisted and would demand
# cached credentials the loop doesn't actually need.
have_sudo() { sudo -n lxc-info --version >/dev/null 2>&1; }
need_sudo() { have_sudo || die "sudo -n unavailable — install the waydroid-dev sudoers allowlist or run 'sudo -v' first"; }
