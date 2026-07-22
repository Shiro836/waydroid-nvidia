#!/bin/bash
# LineageOS 20 + waydroid manifests sync (M8.6). Logs to sync.log.
set -uo pipefail
cd "$(dirname "$0")"

step() { echo "=== $(date '+%F %T') $*"; }

step "repo init"
repo init -u https://github.com/LineageOS/android.git -b lineage-20.0 --git-lfs </dev/null || exit 1

step "sync build/make (needed by manifest script)"
repo sync build/make --no-clone-bundle --current-branch -j8 || exit 1

step "generate waydroid local manifests (reviewed local copy)"
bash ./generate-manifest.sh || exit 1

step "full sync"
repo sync --no-clone-bundle --current-branch -j8 --force-sync || exit 1

step "DONE"
