#!/usr/bin/env bash
# reproduce.sh — clean-room validation of the stranger's build path, and the
# blueprint for the eventual PKGBUILD build(). For each component it makes a
# FRESH checkout at patches/<comp>/BASE, applies the patch series + net-new
# src, and builds via the SAME build/<comp>/build.sh the dev loop uses.
#
#   packaging/aur/reproduce.sh [mesa|virgl|hwc|gralloc|waydroid|all]
#
# Source of the fresh checkout:
#   SRC_MODE=worktree  (default) git worktree off the local clone — fast, no download
#   SRC_MODE=clone            git clone upstream fresh — true from-scratch (slow)
#
# mesa/virgl build fully clean-room. hwc/gralloc compile here but still lean on
# hand-provisioned prereqs under $WNV (see PREREQS.md) — flagged, not hidden.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
source "$REPO/dev/env.sh"

WORK="${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/wdnv-repro.XXXXXX")}"
SRC_MODE="${SRC_MODE:-worktree}"
declare -A UPSTREAM=(
  [mesa]=https://gitlab.freedesktop.org/mesa/mesa.git
  [virgl]=https://gitlab.freedesktop.org/virgl/virglrenderer.git
  [hwc]=https://github.com/waydroid/android_hardware_waydroid
  [waydroid]=https://github.com/waydroid/waydroid.git
)
declare -A LOCAL=([mesa]="$MESA_TREE" [virgl]="$VIRGL_TREE" [hwc]="$HWC_TREE" [waydroid]="$WAYDROID_SRC")
declare -A BASE=([mesa]="$MESA_BASE" [virgl]="$VIRGL_BASE" [hwc]="$HWC_BASE" [waydroid]="$WAYDROID_BASE")

PASS=(); FAIL=()
ok()  { echo -e "\033[1;32m  PASS\033[0m $*"; PASS+=("$1"); }
bad() { echo -e "\033[1;31m  FAIL\033[0m $*"; FAIL+=("$1"); }

verify_android_elf() {
    local file="$1" abi="$2" expected_class expected_machine header dynamic
    case "$abi" in
        x86)
            expected_class=ELF32
            expected_machine="Intel 80386"
            ;;
        x86_64)
            expected_class=ELF64
            expected_machine="Advanced Micro Devices X86-64"
            ;;
        *)
            echo "unknown Android ABI '$abi'" >&2
            return 1
            ;;
    esac

    [ -f "$file" ] || { echo "missing artifact: $file" >&2; return 1; }
    command -v readelf >/dev/null 2>&1 || {
        echo "readelf is required to verify $file" >&2
        return 1
    }
    header=$(LC_ALL=C readelf -h -- "$file" 2>/dev/null) || {
        echo "not a readable ELF file: $file" >&2
        return 1
    }
    grep -Eq "^[[:space:]]*Class:[[:space:]]+${expected_class}[[:space:]]*$" <<<"$header" &&
        grep -Eq "^[[:space:]]*Machine:[[:space:]]+${expected_machine}[[:space:]]*$" <<<"$header" &&
        grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN([[:space:]]|$)' <<<"$header" || return 1
    dynamic=$(LC_ALL=C readelf -dW -- "$file" 2>/dev/null) || return 1
    grep -Eq 'SONAME.*\[libvulkan_virtio\.so\]' <<<"$dynamic"
}

checkout() { # comp -> prints fresh checkout dir on fd, worktrees tracked for cleanup
    local c="$1"; local dir="$WORK/$c"
    if [ "$SRC_MODE" = clone ]; then
        git clone -q "${UPSTREAM[$c]}" "$dir"
        git -C "$dir" checkout -q "${BASE[$c]}"
    else
        git -C "${LOCAL[$c]}" worktree add -q --detach "$dir" "${BASE[$c]}"
        echo "$c" >> "$WORK/.worktrees"
    fi
    echo "$dir"
}

do_mesa() {
    say "mesa: fresh $MESA_BASE + series -> cross-build x86 + x86_64 libvulkan_virtio.so"
    local d; d=$(checkout mesa) || { bad mesa "checkout"; return; }
    git -C "$d" am -q "$REPO"/patches/mesa/0001-*.patch "$REPO"/patches/mesa/0002-*.patch \
        && git -C "$d" apply "$REPO/patches/mesa/0003-wip-ahb-memory-steering.patch" \
        || { bad mesa "patch apply"; return; }

    local abi builddir artifact
    for abi in x86_64 x86; do
        builddir="$d/build-android-$abi"
        artifact="$builddir/src/virtio/vulkan/libvulkan_virtio.so"
        if ANDROID_ABI="$abi" "$REPO/build/mesa/build.sh" "$d" "$builddir"; then
            if verify_android_elf "$artifact" "$abi"; then
                ok "mesa-$abi" "$abi libvulkan_virtio.so built and ELF header verified"
            else
                bad "mesa-$abi" "$abi artifact has the wrong ELF class/machine"
            fi
        else
            bad "mesa-$abi" "$abi build"
        fi
    done
}

do_virgl() {
    say "virgl: fresh $VIRGL_BASE + series -> build virgl_test_server"
    local d; d=$(checkout virgl) || { bad virgl "checkout"; return; }
    git -C "$d" am -q "$REPO"/patches/virglrenderer/000{1,2,3}-*.patch \
        && git -C "$d" apply "$REPO/patches/virglrenderer/0004-wip-gpu-alloc-and-global-priority.patch" \
        || { bad virgl "patch apply"; return; }
    if REPO="$REPO" "$REPO/build/virglrenderer/build.sh" "$d" "$d/build"; then
        [ -f "$d/build/vtest/virgl_test_server" ] \
            && ok virgl "virgl_test_server built" || bad virgl "artifact missing"
    else bad virgl "build"; fi
}

do_hwc() {
    say "hwc: fresh $HWC_BASE + patch -> NDK build (uses \$WNV prereqs, see PREREQS.md)"
    local d; d=$(checkout hwc) || { bad hwc "checkout"; return; }
    git -C "$d" apply "$REPO/patches/hwcomposer/0001-wip-nvidia-fixes.patch" \
        || { bad hwc "patch apply"; return; }
    if "$REPO/build/hwcomposer/build.sh" "$d/hwcomposer" "$WORK/hwc-out"; then
        [ -f "$WORK/hwc-out/hwcomposer.waydroid.stripped.so" ] \
            && ok hwc "hwcomposer.waydroid.so built" || bad hwc "artifact missing"
    else bad hwc "build (prereqs under \$WNV?)"; fi
}

do_gralloc() {
    say "gralloc: NDK compile net-new backend (needs \$MINIGBM header, see PREREQS.md)"
    if REPO="$REPO" "$REPO/build/gralloc/build.sh" "$WORK/gralloc-out"; then
        [ -f "$WORK/gralloc-out/libgbm_mesa_wrapper.so" ] \
            && ok gralloc "libgbm_mesa_wrapper.so built" || bad gralloc "artifact missing"
    else bad gralloc "build (minigbm prereq?)"; fi
}

do_waydroid() {
    say "waydroid: fresh $WAYDROID_BASE + patch (python — apply-check only)"
    local d; d=$(checkout waydroid) || { bad waydroid "checkout"; return; }
    git -C "$d" apply --check "$REPO/patches/waydroid/0001-nvidia-integration.patch" \
        && ok waydroid "0001 applies" || bad waydroid "patch apply"
}

cleanup() {
    [ -f "$WORK/.worktrees" ] || { rm -rf "$WORK"; return; }
    while read -r c; do git -C "${LOCAL[$c]}" worktree remove --force "$WORK/$c" 2>/dev/null; done < "$WORK/.worktrees"
    for c in mesa virgl hwc waydroid; do git -C "${LOCAL[$c]}" worktree prune 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "reproduce: SRC_MODE=$SRC_MODE  WORK=$WORK"
case "${1:-all}" in
    mesa) do_mesa ;; virgl) do_virgl ;; hwc) do_hwc ;; gralloc) do_gralloc ;; waydroid) do_waydroid ;;
    all) do_mesa; do_virgl; do_hwc; do_gralloc; do_waydroid ;;
    *) die "usage: reproduce.sh [mesa|virgl|hwc|gralloc|waydroid|all]" ;;
esac

echo; echo "===== summary ====="
echo "PASS: ${PASS[*]:-none}"
echo "FAIL: ${FAIL[*]:-none}"
[ ${#FAIL[@]} -eq 0 ]
