#!/bin/bash
# LineageOS 20 waydroid build (M8.6). Usage:
#   ./build.sh sf      -> fast path: just the surfaceflinger binary
#   ./build.sh images  -> full systemimage + vendorimage
# Logs to build-<target>.log. Applies waydroid patches + our patches/lineage-20.
set -o pipefail  # no -u: AOSP envsetup and its functions are not set -u clean
cd "$(dirname "$0")"
TARGET="${1:-sf}"

export TERM=dumb
source build/envsetup.sh >/dev/null || exit 1

# waydroid's own patch stack (idempotent-ish: skip if marker exists)
if [ ! -f .waydroid-patches-applied ]; then
    apply-waydroid-patches && touch .waydroid-patches-applied || exit 1
fi

# our patches (git am-able; applied with git apply --3way, skip if present)
for p in ~/repos/waydroid-nvidia/patches/lineage-20/frameworks-native/*.patch; do
    [ -e "$p" ] || continue
    if git -C frameworks/native apply --check "$p" 2>/dev/null; then
        git -C frameworks/native apply "$p" && echo "applied: $p"
    else
        echo "skip (already applied or conflict — verify manually): $p"
    fi
done

lunch lineage_waydroid_x86_64-userdebug || exit 1

case "$TARGET" in
  sf)     m surfaceflinger ;;
  images) m systemimage vendorimage ;;
  *)      echo "unknown target $TARGET"; exit 1 ;;
esac
