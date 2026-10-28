#!/bin/bash
# Build the Waydroid vendor image from source on a persistent LineageOS tree (the self-hosted runner's).
#
# Usage: vendor-image/build-vendor.sh STAGE... (run from the repo checkout root)
#   preflight  tree, tools, disk (fails loudly, names the remedy)
#   checkout   put the tree in this job's state: the pinned manifest (tree-checkout.py) + Waydroid's base patches
#              (vendor/extra/waydroid-patches) + the job's own patch layer (VENDOR_PRODUCT=prolapse:
#              vendor-image/patches/<project>/*.patch; sf: patches/lineage-20/frameworks-native, build.yml's SF job;
#              stock: none, and our product layer is removed). Skipped when the tree's stamp says it is already
#              there (see "Tree state" below); otherwise only the projects that differ are touched.
#   product    (VENDOR_PRODUCT=prolapse) install the product layer (vendor-image/product -> device/nvidia-prolapse,
#              vendor/nvidia-prolapse; only changed files are rewritten) and the prebuilts:
#              NATIVE_BUNDLE (packaging/native/collect.py output: payload/ + BUILD-RECORD.json, sha256-checked)
#              and ANGLE_DIR (the angle job's stage: vendor/lib{,64}/egl/lib*_angle.so)
#   lunch      source envsetup + lunch; prints the product variables the build will use
#   build      m vendorimage into $VENDOR_OUT_DIR (separate from the SF job's out/)
#              (lunch and build run in the build-environment container, vendor-image/Containerfile)
#   collect    vendor.img + sha256 + manifest + tree state into $STAGE_DIR
#   compare    compare the built vendor.img with the official reference image (compare-vendor.py)
#   all        every stage above in order
# Environment: LINEAGE_TREE (default ~/repos/waydroid-nv/lineage-20), VENDOR_OUT_DIR (default
#   $LINEAGE_TREE/out-vendor-l20), STAGE_DIR (default ./stage-vendor), JOBS (default 3/4 of the CPUs),
#   MIN_FREE_GB (default 80), REF_CACHE (default ~/vendor-image-ci/ref), VENDOR_PRODUCT (stock = Waydroid's own
#   product, the reproduction; prolapse = lineage_nvidia_waydroid_x86_64 with our modules; default stock),
#   NATIVE_BUNDLE, ANGLE_DIR (prolapse only), M_TARGETS (build stage; default vendorimage), CCACHE_DIR (build stage;
#   default ~/vendor-image-ci/ccache, capped at CCACHE_MAXSIZE, default 40G; CCACHE_DISABLE=1 builds without it).
#   VENDOR_PRODUCT=sf only prepares the tree for build.yml's surfaceflinger job (checkout stage).
# Nothing here pushes or uploads; the workflow uploads $STAGE_DIR as an artifact.
set -o pipefail   # no -u/-e: AOSP envsetup.sh and its functions are not set -u clean (baseline build.sh note)
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
. "$HERE/pins.env"
TREE=${LINEAGE_TREE:-$HOME/repos/waydroid-nv/lineage-20}
VENDOR_PRODUCT=${VENDOR_PRODUCT:-stock}
case "$VENDOR_PRODUCT" in
    stock)    LUNCH=$L20_LUNCH; PRODUCT_DEVICE=$L20_PRODUCT; OUT=${VENDOR_OUT_DIR:-$TREE/out-vendor-l20} ;;
    prolapse) LUNCH=$L20_PROLAPSE_LUNCH; PRODUCT_DEVICE=$L20_PROLAPSE_DEVICE; OUT=${VENDOR_OUT_DIR:-$TREE/out-vendor-l20-prolapse} ;;
    sf)       LUNCH=$L20_LUNCH; PRODUCT_DEVICE=$L20_PRODUCT; OUT=$TREE/out ;;   # checkout only (build.yml builds SF)
    *) echo "ERROR: VENDOR_PRODUCT must be stock, prolapse or sf, not '$VENDOR_PRODUCT'" >&2; exit 1 ;;
esac
STAGE=${STAGE_DIR:-$PWD/stage-vendor}
JOBS=${JOBS:-$(( $(nproc) * 3 / 4 ))}
MIN_FREE_GB=${MIN_FREE_GB:-80}
REF_CACHE=${REF_CACHE:-$HOME/vendor-image-ci/ref}
MANIFEST=$REPO/$L20_MANIFEST
PRODUCT_OUT=$OUT/target/product/$PRODUCT_DEVICE
PRODUCT_DIRS="device/nvidia-prolapse vendor/nvidia-prolapse"
NICE="nice -n 10 ionice -c2 -n7"   # prefix for host binaries; lunch/m lower their own priority in the container

die() { echo "::error::$*" >&2; echo "ERROR: $*" >&2; exit 1; }
step() { echo "=== $(date '+%F %T') $*"; }

preflight() {
    step preflight
    [ -f "$TREE/build/envsetup.sh" ] && [ -d "$TREE/.repo" ] || die "no synced LineageOS tree at $TREE (set LINEAGE_TREE; deliver a tree by rsync from a known-good copy, never a cold repo sync on the runner host)"
    [ -f "$MANIFEST" ] || die "pinned manifest $MANIFEST missing"
    for t in git python3 debugfs readelf nm curl unzip sha256sum docker; do command -v $t >/dev/null || die "missing tool $t"; done
    docker info >/dev/null 2>&1 || die "docker is not usable by $(id -un) (lunch and m run in vendor-image/Containerfile's image; add the runner user to the docker group)"
    local free; free=$(df -BG --output=avail "$TREE" | tail -1 | tr -dc 0-9)
    [ "$free" -ge "$MIN_FREE_GB" ] || die "only ${free} GB free on $TREE's filesystem, need $MIN_FREE_GB (rm -rf $OUT reclaims the last vendor build)"
    git config user.email >/dev/null || [ -n "${GIT_COMMITTER_EMAIL:-}" ] || die "git identity missing: Waydroid's apply-patches.sh uses git am (set GIT_AUTHOR_*/GIT_COMMITTER_*)"
    echo "tree $TREE, out $OUT, ${free} GB free, jobs $JOBS"
}

# ---------------------------------------------------------------- Tree state
# The tree is shared by three jobs that each need their own state: this script's stock and prolapse images and
# build.yml's surfaceflinger job (VENDOR_PRODUCT=sf). The state is layered:
#   base   = pinned manifest + Waydroid's base patches, recorded per project in $TREE/.ci-tree-base.tsv, keyed by
#            the manifest's hash (line 1: "# key ...")
#   layer  = the job's own patch series (vendor-image/patches, or patches/lineage-20 for SF), keyed by the hash of
#            every patch with its target project
# $TREE/.ci-tree-stamp.tsv records the whole tree (every project's HEAD) under the key "base + layer" after a job
# put it there; it is deleted before anything in the tree is changed, so an interrupted job leaves no stamp.
# checkout: stamp key == this job's key and every project still at its recorded HEAD with no tracked changes
# (tree-checkout.py --verify) -> nothing is touched (projects that drifted are put back at their stamped commits).
# Otherwise, with a matching base record, only the projects that
# differ from the base (the previous job's layer, stray edits) are reset to it, then this job's layer is applied;
# without one, the full pinned checkout + Waydroid patches run and the base is recorded anew. Untouched projects keep
# their file mtimes, which is what spares Soong/Kati their regeneration and make its rebuilds.
BASE_REC=$TREE/.ci-tree-base.tsv
STAMP=$TREE/.ci-tree-stamp.tsv
STAMP_RESTORE=$TREE/.ci-tree-stamp-restore.tsv   # (tree-checkout.py tells a state file by its .tsv suffix)

layer_series() {   # "project<TAB>patch" per line, in apply order, for the job's layer
    case "$VENDOR_PRODUCT" in
        stock) ;;
        prolapse) local dir proj p
                  for dir in "$HERE"/patches/*/*/; do
                      proj=${dir#"$HERE"/patches/}; proj=${proj%/}
                      for p in "$dir"*.patch; do printf '%s\t%s\n' "$proj" "$p"; done
                  done ;;
        sf)       local p; for p in "$REPO"/patches/lineage-20/frameworks-native/*.patch; do
                      [ -e "$p" ] && printf 'frameworks/native\t%s\n' "$p"; done ;;
    esac
}

checkout() {
    local base_key layer_key want log series proj p t0=$SECONDS
    [ -f "$TREE/build/envsetup.sh" ] && [ -d "$TREE/.repo" ] || die "no synced LineageOS tree at $TREE (set LINEAGE_TREE)"
    base_key="manifest-$(sha256sum < "$MANIFEST" | cut -c1-16)"
    series=$(layer_series)
    layer_key="$VENDOR_PRODUCT-$(printf '%s\n' "$series" | while IFS=$'\t' read -r proj p; do
        [ -n "$p" ] && printf '%s %s\n' "$proj" "$(sha256sum < "$p")"; done | sha256sum | cut -c1-16)"
    want="$base_key $layer_key"
    log=$(mktemp)
    if [ "$(head -1 "$STAMP" 2>/dev/null)" = "# key $want" ]; then
        step "tree stamp is this job's ($want): verify every project"
        if $NICE python3 "$HERE/tree-checkout.py" "$TREE" "$STAMP" --verify -j 16 > "$log" 2>&1; then
            tail -1 "$log"
            echo "tree already in this job's state: checkout skipped ($(( SECONDS - t0 )) s)"
            touch "$TREE/.waydroid-patches-applied"
            return
        fi
        echo "the tree differs from its stamp:"; grep -v '^summary' "$log" | head -20
        # stray edits or a moved HEAD: put those projects back at their stamped commits (present locally)
        rm -f "$STAMP_RESTORE"; mv "$STAMP" "$STAMP_RESTORE"
        if $NICE python3 "$HERE/tree-checkout.py" "$TREE" "$STAMP_RESTORE" -j 8 > "$log" 2>&1; then
            cat "$log"; mv "$STAMP_RESTORE" "$STAMP"
            echo "restored the stamped state ($(( SECONDS - t0 )) s)"
            touch "$TREE/.waydroid-patches-applied"
            return
        fi
        cat "$log"; rm -f "$STAMP_RESTORE"
    else
        echo "tree stamp: $(head -1 "$STAMP" 2>/dev/null || echo none); this job: # key $want"
    fi
    rm -f "$STAMP"
    if [ "$(head -1 "$BASE_REC" 2>/dev/null)" = "# key $base_key" ]; then
        step "restore the recorded base (pinned manifest + Waydroid patches) where projects differ"
        $NICE python3 "$HERE/tree-checkout.py" "$TREE" "$BASE_REC" -j 8 > "$log" 2>&1 || rm -f "$BASE_REC"
        cat "$log"
    fi
    if [ "$(head -1 "$BASE_REC" 2>/dev/null)" != "# key $base_key" ]; then
        rm -f "$BASE_REC"
        step "checkout $MANIFEST"
        $NICE python3 "$HERE/tree-checkout.py" "$TREE" "$MANIFEST" -j 8 || die "tree-checkout failed (see above)"
        rm -f "$TREE/.waydroid-patches-applied"
        step "apply Waydroid patches"
        ( cd "$TREE" && export TERM=dumb && source build/envsetup.sh >/dev/null && apply-waydroid-patches ) 2>&1 | tee "$log" | grep -E "Project|Applying|Conflicts|Already|INFO|WARNING|ALERT" || true
        grep -q "All patches applied fine" "$log" || die "Waydroid patches did not apply cleanly on the pinned tree (log above)"
        record_state "$BASE_REC" "$base_key"
    fi
    # the baseline's build.sh (developer SF builds) keys its own re-apply on this marker
    touch "$TREE/.waydroid-patches-applied"
    if [ "$VENDOR_PRODUCT" = stock ]; then
        local d; for d in $PRODUCT_DIRS; do rm -rf "${TREE:?}/$d"; done   # not repo projects: the product stage installs them
    fi
    if [ -n "$series" ]; then
        step "apply the $VENDOR_PRODUCT patch layer"
        while IFS=$'\t' read -r proj p; do
            [ -d "$TREE/$proj/.git" ] || [ -f "$TREE/$proj/.git" ] || die "patch $p: no project $proj in $TREE"
            # a git am stopped by an earlier failed job would refuse to start; its HEAD is already restored above
            [ -d "$(git -C "$TREE/$proj" rev-parse --git-path rebase-apply)" ] && git -C "$TREE/$proj" am --quit
            git -C "$TREE/$proj" am -q -3 "$p" || { git -C "$TREE/$proj" am --abort; die "$p does not apply to $proj"; }
        done <<< "$series"
        cut -f1 <<< "$series" | uniq -c | while read -r n proj; do
            echo "$proj: $n patches, HEAD $(git -C "$TREE/$proj" rev-parse --short HEAD)"; done
    fi
    record_state "$STAMP" "$want"
    echo "tree in this job's state after $(( SECONDS - t0 )) s"
}

record_state() {   # record_state FILE KEY: every manifest project's HEAD under "# key KEY"
    $NICE python3 "$HERE/tree-checkout.py" "$TREE" "$MANIFEST" --record "$1.part" -j 16 >/dev/null || die "cannot record the tree state"
    { echo "# key $2"; cat "$1.part"; } > "$1.tmp" && rm -f "$1.part" && mv "$1.tmp" "$1"
    echo "recorded $(( $(wc -l < "$1") - 1 )) projects in $1 (key $2)"
}

# copy SRC/ to DST/ rewriting only files whose content differs (no -t: an unchanged file keeps its mtime, so the
# Android build sees no change); --delete removes what SRC no longer has
sync_tree() { mkdir -p "$2" && rsync -rlp --checksum --delete "$1/" "$2/"; }
write_if_changed() {   # write_if_changed FILE (content on stdin)
    local tmp; tmp=$(mktemp) && cat > "$tmp"
    if cmp -s "$tmp" "$1"; then rm -f "$tmp"; else mv "$tmp" "$1" && chmod 0644 "$1"; fi
}

product() {
    [ "$VENDOR_PRODUCT" = prolapse ] || die "the product stage is for VENDOR_PRODUCT=prolapse"
    head -1 "$STAMP" 2>/dev/null | grep -q " prolapse-" || die "run the checkout stage first (VENDOR_PRODUCT=prolapse: pinned tree + Waydroid patches + vendor-image/patches)"
    local nb=${NATIVE_BUNDLE:-} ad=${ANGLE_DIR:-}
    [ -n "$nb" ] && [ -f "$nb/BUILD-RECORD.json" ] && [ -d "$nb/payload" ] || die "NATIVE_BUNDLE=$nb is not a native release bundle (payload/ + BUILD-RECORD.json from packaging/native/collect.py)"
    [ -n "$ad" ] && [ -f "$ad/vendor/lib64/egl/libGLESv2_angle.so" ] && [ -f "$ad/vendor/lib/egl/libGLESv2_angle.so" ] || die "ANGLE_DIR=$ad has no vendor/lib{,64}/egl/libGLESv2_angle.so (the angle job's stage)"
    step "verify the native bundle against its BUILD-RECORD.json"
    python3 -I - "$nb" <<'PY' || die "native bundle $nb does not match its BUILD-RECORD.json"
import hashlib, json, os, sys
nb = sys.argv[1]
rec = json.load(open(os.path.join(nb, "BUILD-RECORD.json")))
# payload/ and driver/ (the driver's media/odm files the product layer takes)
want = {p: h for p, h in rec["files"].items() if p.startswith("payload/")}
want.update(rec.get("driver_files") or {})
bad = [p for p, h in want.items() if hashlib.sha256(open(os.path.join(nb, p), "rb").read()).hexdigest() != h]
if not any(p.startswith("driver/media/odm/") for p in want):
    sys.exit("the bundle carries no driver/media/odm files (BUILD-RECORD.json driver_files)")
print("bundle", rec.get("release") or "-", "commit", rec["commit"], "dirty", rec["dirty"],
      "driver", (rec.get("driver") or {}).get("commit"), len(want), "payload + driver files")
sys.exit(1 if bad else print("sha256 ok") or 0)
PY
    step "install the product layer"
    # identity.mk and prebuilts/ are generated below, not in vendor-image/product: kept out of --delete
    for d in $PRODUCT_DIRS; do
        mkdir -p "$TREE/$d"
        rsync -rlp --checksum --delete --exclude=/identity.mk --exclude=/prebuilts "$HERE/product/$d/" "$TREE/$d/"
    done
    # identity of this image (waydroid/nvidia_native.py reads it from the installed vendor.img): the native bundle it
    # carries, by the sha256 of its BUILD-RECORD.json, which the installed package's bundle must equal
    local rec_sha version
    rec_sha=$(sha256sum "$nb/BUILD-RECORD.json" | cut -d' ' -f1)
    version=$(python3 -I -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r.get("release") or "git-" + r["commit"][:12])' "$nb/BUILD-RECORD.json")
    [[ "$version" =~ ^[A-Za-z0-9._+-]+$ ]] || die "bundle release/commit '$version' is not a property-safe version string"
    printf '%s\n' "# Generated by vendor-image/build-vendor.sh (product stage) from NATIVE_BUNDLE; not in the source tree." \
        'PRODUCT_VENDOR_PROPERTIES += \' \
        "    ro.vendor.nvidia_prolapse.version=$version \\" \
        "    ro.vendor.nvidia_prolapse.build_record=$rec_sha" | write_if_changed "$TREE/vendor/nvidia-prolapse/identity.mk"
    echo "identity: ro.vendor.nvidia_prolapse.version=$version build_record=$rec_sha"
    local pre=$TREE/vendor/nvidia-prolapse/prebuilts
    mkdir -p "$pre/native" "$pre/angle"
    sync_tree "$nb/payload" "$pre/native/payload"
    rsync -rlp --checksum "$nb/BUILD-RECORD.json" "$pre/native/"
    sync_tree "$nb/driver/media/odm/etc" "$pre/media/odm/etc"   # the driver's rc, VINTF fragment, seccomp policy
    mkdir -p "$pre/angle/vendor"
    rsync -rlp --checksum --delete --include='*/' --include='lib*_angle.so' --exclude='*' "$ad/vendor/" "$pre/angle/vendor/"
    ( cd "$pre/angle" && find . -name '*.so' -exec sha256sum {} + | sort -k2 ) | write_if_changed "$pre/angle/SHA256SUMS"
}

# lunch and m run in the build-environment container (vendor-image/Containerfile: why, and what it holds): the tree is
# bind-mounted at the same path, the container runs as this user with no network, and the image tag is the
# Containerfile's hash, so a changed Containerfile builds a new image instead of reusing a stale one.
BUILDENV_FILE=$HERE/Containerfile
BUILDENV_IMAGE=vendor-image-buildenv:$(sha256sum "$BUILDENV_FILE" | cut -c1-12)

buildenv() {
    if ! docker image inspect "$BUILDENV_IMAGE" >/dev/null 2>&1; then
        step "build the build-environment image $BUILDENV_IMAGE"
        $NICE docker build -q -t "$BUILDENV_IMAGE" - < "$BUILDENV_FILE" || die "docker build of $BUILDENV_FILE failed"
    fi
    mkdir -p "$OUT"
    docker run --rm --network none "$BUILDENV_IMAGE" sh -c 'dpkg-query -W; python3 -m pip freeze; meson --version' \
        > "$OUT/buildenv-packages.txt" || die "cannot run $BUILDENV_IMAGE"
    echo "build environment $BUILDENV_IMAGE ($(tail -1 "$OUT/buildenv-packages.txt" | sed 's/^/meson /'))"
}

# ccache: CC_WRAPPER/CXX_WRAPPER put it in front of every C/C++ compile, in Soong (cc/config/global.go CcWrapper) and in
# make (core/binary.mk my_cc_wrapper). Not LineageOS's USE_CCACHE switch: that also adds -Wno-unused-command-line-argument
# to every compile (cc/config/global.go), which changes each ELF's build-id against an uncached build, and sets ccache
# sloppiness (time_macros, include_file_mtime, file_macro) and CCACHE_BASEDIR=/; plain ccache defaults hash all of it.
# Measured 2026-10-17: the USE_CCACHE build differed from the uncached one in 276 build-ids. The cache lives on the
# runner host, mounted into the container, capped at CCACHE_MAXSIZE. CCACHE_DISABLE=1 builds without it (switching
# changes the compile commands: one full rebuild).
CCACHE_HOST_DIR=${CCACHE_DIR:-$HOME/vendor-image-ci/ccache}
CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-40G}
ccache_env() {
    [ "${CCACHE_DISABLE:-}" = 1 ] && return
    mkdir -p "$CCACHE_HOST_DIR"
    printf '%s\n' -v "$CCACHE_HOST_DIR:$CCACHE_HOST_DIR" -e CC_WRAPPER=/usr/bin/ccache -e CXX_WRAPPER=/usr/bin/ccache \
        -e CCACHE_DIR="$CCACHE_HOST_DIR"
}
ccache_run() {   # the container's ccache on the host cache dir
    [ "${CCACHE_DISABLE:-}" = 1 ] && return
    local cc=(); mapfile -t cc < <(ccache_env)
    docker run --rm --network none --user "$(id -u):$(id -g)" "${cc[@]}" "$BUILDENV_IMAGE" ccache "$@"
}

in_build_env() {   # eval "$1" after envsetup + lunch, inside the build-environment container, at lowered CPU/IO
                   # priority (m and lunch are shell functions, so the container's shell lowers itself first)
    local mounts=(-v "$TREE:$TREE") cc=()
    case "$OUT/" in "$TREE"/*) ;; *) mounts+=(-v "$OUT:$OUT") ;; esac
    mapfile -t cc < <(ccache_env)
    mkdir -p "$OUT/.buildenv-home"
    # a fixed hostname: Kati re-reads every makefile when BUILD_HOSTNAME changes (80 s), and docker gives each
    # container a random one
    docker run --rm --init --network none --hostname vendor-image-buildenv --user "$(id -u):$(id -g)" \
        "${mounts[@]}" "${cc[@]}" -w "$TREE" \
        -e HOME="$OUT/.buildenv-home" -e USER="$(id -un)" -e TERM=dumb -e OUT_DIR="$OUT" \
        -e L20_LUNCH="$LUNCH" -e CMD="$1" "$BUILDENV_IMAGE" \
        bash -c 'renice -n 10 -p $$ >/dev/null && ionice -c2 -n7 -p $$ || { echo "cannot lower the build priority" >&2; exit 1; }
                 source build/envsetup.sh >/dev/null && lunch "$L20_LUNCH" >/dev/null && eval "$CMD"'
}

lunch_stage() {
    step "lunch $LUNCH"
    buildenv
    in_build_env 'for v in TARGET_PRODUCT TARGET_BUILD_VARIANT TARGET_ARCH TARGET_2ND_ARCH PLATFORM_VERSION PLATFORM_SDK_VERSION BOARD_VNDK_VERSION PRODUCT_OUT; do echo "$v=$(get_build_var $v)"; done' \
        || die "lunch $LUNCH failed"
}

build() {
    local targets=${M_TARGETS:-vendorimage}
    step "m $targets -j$JOBS (OUT_DIR=$OUT)"
    buildenv
    # 75c6a29 linked the tree sysroot's ncurses 5 into the host tools' RUNPATH; the container provides libncurses5 now
    rm -f "$OUT/host/linux-x86/lib64/libncurses.so.5" "$OUT/host/linux-x86/lib64/libtinfo.so.5"
    local t0=$SECONDS
    ccache_run --max-size="$CCACHE_MAXSIZE" >/dev/null && ccache_run --zero-stats >/dev/null || die "cannot set up ccache in $CCACHE_HOST_DIR"
    in_build_env "m -j$JOBS $targets" || die "m $targets failed"
    echo "build time: $(( SECONDS - t0 )) s"
    ccache_run --show-stats
    du -sh "$OUT" 2>/dev/null
}

collect() {
    step "collect into $STAGE"
    [ -f "$PRODUCT_OUT/vendor.img" ] || die "no $PRODUCT_OUT/vendor.img (run the build stage)"
    mkdir -p "$STAGE"
    cp "$PRODUCT_OUT/vendor.img" "$STAGE/vendor.img"
    cp "$MANIFEST" "$STAGE/manifest.xml"
    cp "$STAMP" "$STAGE/tree-state.tsv" || die "no tree stamp $STAMP (run the checkout stage)"
    cp "$OUT/buildenv-packages.txt" "$STAGE/buildenv-packages.txt" 2>/dev/null
    if [ "$VENDOR_PRODUCT" = prolapse ]; then
        cp "$TREE/vendor/nvidia-prolapse/prebuilts/native/BUILD-RECORD.json" "$STAGE/native-BUILD-RECORD.json"
        cp "$TREE/vendor/nvidia-prolapse/prebuilts/angle/SHA256SUMS" "$STAGE/angle-SHA256SUMS"
    fi
    debugfs -R "cat /build.prop" "$STAGE/vendor.img" 2>/dev/null > "$STAGE/vendor-build.prop"
    if [ "$VENDOR_PRODUCT" = prolapse ]; then
        local rec_sha
        rec_sha=$(sha256sum "$STAGE/native-BUILD-RECORD.json" | cut -d' ' -f1)
        grep -qx "ro.vendor.nvidia_prolapse.build_record=$rec_sha" "$STAGE/vendor-build.prop" ||
            die "vendor.img /build.prop lacks ro.vendor.nvidia_prolapse.build_record=$rec_sha (identity.mk not built in?)"
        grep -q '^ro.vendor.nvidia_prolapse.version=' "$STAGE/vendor-build.prop" || die "vendor.img lacks ro.vendor.nvidia_prolapse.version"
    fi
    ( cd "$STAGE" && sha256sum vendor.img manifest.xml > SHA256SUMS && cat SHA256SUMS )
}

reference() {
    mkdir -p "$REF_CACHE"
    local zip=$REF_CACHE/vendor-$L20_REF_VENDOR_SHA256.zip img=$REF_CACHE/vendor-$L20_REF_VENDOR_SHA256.img
    if [ ! -f "$img" ]; then
        # tools/fetch.py: alternative source, retries, stalled mirrors given up, kept only with the pinned sha256
        python3 "$REPO/tools/fetch.py" --out "$zip" --sha256 "$L20_REF_VENDOR_SHA256" "$L20_REF_VENDOR_URL" \
            "$L20_REF_VENDOR_URL_ALT" >&2 || die "cannot download $L20_REF_VENDOR_URL"
        echo "$L20_REF_VENDOR_SHA256  $zip" | sha256sum -c >/dev/null || die "reference vendor zip checksum mismatch ($zip)"
        unzip -p "$zip" vendor.img > "$img.part" && mv "$img.part" "$img" || die "no vendor.img in $zip"
    fi
    echo "$img"
}

compare() {
    step "compare with the official image"
    local ref; ref=$(reference) || exit 1
    [ -f "$STAGE/vendor.img" ] || die "no $STAGE/vendor.img (run collect)"
    python3 -I "$HERE/compare-vendor.py" "$ref" "$STAGE/vendor.img" --report "$STAGE/compare-official.md" > /dev/null
    local rc=$?
    sed -n '1,20p' "$STAGE/compare-official.md"
    # differences are a result to read, not a build failure: the job uploads the report either way
    echo "compare exit $rc (0 = identical apart from volatile build props)"
}

[ $# -gt 0 ] || { sed -n '2,20p' "$0"; exit 2; }
for s in "$@"; do
    case "$s" in
        preflight) preflight ;; checkout) checkout ;; product) product ;; lunch) lunch_stage ;; build) build ;;
        collect) collect ;; compare) compare ;;
        all) preflight; checkout; [ "$VENDOR_PRODUCT" = stock ] || product; lunch_stage; build; collect; compare ;;
        *) die "unknown stage $s" ;;
    esac
done
