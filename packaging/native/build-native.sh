#!/usr/bin/env bash
# packaging/native/build-native.sh — build the whole native stack (nvidia-prolapse mode) from a clean
# checkout, then collect the release bundle. Independent targets build concurrently (dependency list below;
# NATIVE_PARALLEL=0: one at a time); NATIVE_JOBS sets ninja's -j (default: CPU count), NATIVE_LOG_DIR the
# per-target logs (default build/native-logs/<time>).
# One recipe for CI (.github/workflows/build.yml job "native", self-hosted runner) and developers.
#
#   packaging/native/build-native.sh [--driver-src DIR] [OUT_DIR]       default OUT_DIR=build/native-release
#
# The driver (NVK over the Rust RM backend, nvrm, the Vulkan HAL, gralloc, Codec2, bionic libdrm) is the
# driver repository github.com/Shiro836/nvidia-prolapse at the commit vendor-image/pins.env pins
# (NVIDIA_PROLAPSE_REF), fetched and verified into build/nvidia-prolapse-src by tools/driver_source.py and
# built by its own ./build.sh. --driver-src DIR (or NVIDIA_PROLAPSE_SRC=DIR) builds a local checkout
# instead; its commit and dirty flag go into BUILD-RECORD.json.
# Everything else is pinned in the component recipes (hwcomposer/pins.env, nvidia-prolapse/*/Makefile);
# this script only sequences them. Rust profile: release. Network is needed on the first run (pinned
# downloads, checksum-verified by each recipe).
#
# It runs in the build-environment container packaging/native/Containerfile (pinned base digest, Arch
# Linux Archive snapshot, Rust toolchain): started on the host, it builds or reuses that image (tagged with
# the Containerfile's hash) and re-runs itself inside, as the calling user, at lowered CPU/IO priority, with
# the checkout (and its persistent build/ cache) mounted at the same path. The host needs only docker.
# The container keeps network access: a cold build/ cache downloads pinned sources (Mesa, AOSP headers,
# crates, NDKs, OTA images), each checked against its pin. NATIVE_BUILDENV=host runs on the host as before
# (a developer machine with every tool installed); a missing tool then fails by name.
set -euo pipefail
REPO=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
PY=${PYTHON:-python3}
say() { printf '\n== %s\n' "$*"; }
die() { echo "build-native: $*" >&2; exit 1; }
if [ "${1:-}" = --driver-src ]; then
    [ -n "${2:-}" ] || die "--driver-src needs a driver checkout"
    NVIDIA_PROLAPSE_SRC=$(realpath -- "$2") || die "--driver-src $2: no such directory"
    export NVIDIA_PROLAPSE_SRC
    shift 2
fi
OUT=$(realpath -m -- "${1:-$REPO/build/native-release}")
# every pinned download goes through tools/fetch.py (mirrors, retries with backoff, verified before kept)
cd "$REPO"
# NDK r30: the Vulkan HAL's Vulkan 1.4 headers (the driver's hal/build-nvk*.py use /opt/android-ndk)
NDK_R30_URL=https://dl.google.com/android/repository/android-ndk-r30-linux.zip
NDK_R30_SHA256=753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2

if [ -z "${NATIVE_BUILDENV:-}" ]; then
    command -v docker >/dev/null && docker info >/dev/null 2>&1 \
        || die "docker is not usable by $(id -un): the build runs in packaging/native/Containerfile's image (or set NATIVE_BUILDENV=host)"
    BUILDENV_FILE=$REPO/packaging/native/Containerfile
    IMAGE=native-buildenv:$(sha256sum "$BUILDENV_FILE" | cut -c1-12)
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        say "build the build-environment image $IMAGE"
        nice -n 10 ionice -c2 -n7 docker build -q -t "$IMAGE" - < "$BUILDENV_FILE" || die "docker build of $BUILDENV_FILE failed"
    fi
    mkdir -p "$REPO/build/ndk-r30" "$REPO/build/.buildenv-home" "$(dirname "$OUT")"
    mounts=(-v "$REPO:$REPO" -v "$REPO/build/ndk-r30:/cache/ndk-r30")
    case "$OUT/" in "$REPO"/*) ;; *) mounts+=(-v "$(dirname "$OUT"):$(dirname "$OUT")") ;; esac
    if [ -n "${NATIVE_LOG_DIR:-}" ]; then
        mkdir -p "$NATIVE_LOG_DIR"
        case "$NATIVE_LOG_DIR/" in "$REPO"/*|"$(dirname "$OUT")"/*) ;; *) mounts+=(-v "$NATIVE_LOG_DIR:$NATIVE_LOG_DIR") ;; esac
    fi
    # a git worktree keeps its object store elsewhere (collect.py records the commit)
    common=$(cd "$REPO" && realpath -- "$(git rev-parse --git-common-dir)")
    case "$common/" in "$REPO"/*) ;; *) mounts+=(-v "$common:$common:ro") ;; esac
    # a local driver checkout (--driver-src) outside the checkout: mounted read-write (its build/ and out/ dirs),
    # its object store read-only when it is a git worktree
    if [ -n "${NVIDIA_PROLAPSE_SRC:-}" ]; then
        case "$NVIDIA_PROLAPSE_SRC/" in "$REPO"/*) ;; *) mounts+=(-v "$NVIDIA_PROLAPSE_SRC:$NVIDIA_PROLAPSE_SRC") ;; esac
        dcommon=$(cd "$NVIDIA_PROLAPSE_SRC" && realpath -- "$(git rev-parse --git-common-dir)")
        case "$dcommon/" in "$NVIDIA_PROLAPSE_SRC"/*|"$REPO"/*) ;; *) mounts+=(-v "$dcommon:$dcommon:ro") ;; esac
    fi
    say "running in $IMAGE as $(id -un)"
    exec docker run --rm --init --user "$(id -u):$(id -g)" "${mounts[@]}" -w "$REPO" \
        -e HOME="$REPO/build/.buildenv-home" -e CARGO_HOME="$REPO/build/.buildenv-home/cargo" \
        -e NVIDIA_PROLAPSE_SRC="${NVIDIA_PROLAPSE_SRC:-}" -e NATIVE_BUILDENV="$IMAGE" -e NATIVE_BUILDENV_FILE_SHA256="$(sha256sum "$BUILDENV_FILE" | cut -d' ' -f1)" \
        -e RELEASE_TAG="${RELEASE_TAG:-}" -e NATIVE_PARALLEL="${NATIVE_PARALLEL:-1}" \
        -e NATIVE_JOBS="${NATIVE_JOBS:-}" -e NATIVE_LOG_DIR="${NATIVE_LOG_DIR:-}" -e NATIVE_CCACHE="${NATIVE_CCACHE:-1}" \
        -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}" -e TERM=dumb "$IMAGE" \
        bash -c 'renice -n 10 -p $$ >/dev/null && ionice -c2 -n7 -p $$ || { echo "cannot lower the build priority" >&2; exit 1; }
                 exec packaging/native/build-native.sh "$0"' "$OUT"
fi

if [ "$NATIVE_BUILDENV" != host ]; then
    say "NDK r30 (sha256-checked into build/ndk-r30, mounted at /cache/ndk-r30 = /opt/android-ndk)"
    if [ ! -x /cache/ndk-r30/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang ]; then
        zip=/cache/ndk-r30/android-ndk-r30-linux.zip
        "$PY" tools/fetch.py --out "$zip" --sha256 "$NDK_R30_SHA256" "$NDK_R30_URL" || die "download failed: $NDK_R30_URL"
        unzip -q -o "$zip" -d /cache/ndk-r30
    fi
fi

if [ "$NATIVE_BUILDENV" != host ] && [ "${NATIVE_CCACHE:-1}" != 0 ]; then
    # ccache in front of every C/C++ compile of the Meson builds and the Codec2 service (tools/cc_launcher.py), cache
    # in the persistent build/ccache, size-capped (CCACHE_MAXSIZE, default 20G); NATIVE_CCACHE=0 builds without it.
    # A hit returns the object the same compiler made for the same preprocessed input and command line, so the
    # outputs are the same bytes (compared: docs/dev-setup.md "CI").
    command -v ccache >/dev/null || die "ccache missing from the build environment (packaging/native/Containerfile)"
    export NATIVE_CC_LAUNCHER=ccache CCACHE_DIR=$REPO/build/ccache CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-20G}
    mkdir -p "$CCACHE_DIR" && ccache -z >/dev/null
fi

say "preflight"
missing=()
for tool in git curl unzip debugfs meson ninja cmake cc c++ cargo rustup glslangValidator spirv-val readelf zstd "$PY"; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
[ ${#missing[@]} -eq 0 ] || die "missing tools: ${missing[*]}"
"$PY" -c 'import mako' 2>/dev/null || die "python mako missing (pip/pacman: python-mako)"
# the Vulkan HAL needs the Vulkan 1.4 headers of NDK r30 (the driver's hal/build-nvk.py); the rest uses
# the r29 that hwcomposer/provision.sh and the driver's android/provision.sh fetch
[ -x /opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang ] \
    || die "Android NDK r30 missing at /opt/android-ndk (the Vulkan HAL build uses it)"
# llvmpipe references for the GL gate (build-gate.py): host Mesa EGL through glvnd
[ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ] || die "host Mesa EGL (glvnd vendor 50_mesa.json) missing: install mesa + libglvnd"
for t in x86_64-linux-android i686-linux-android; do
    rustup target list --installed | grep -qx "$t" || die "rust target $t missing: rustup target add $t"
done

say "the driver: github.com/Shiro836/nvidia-prolapse"
if [ -n "${NVIDIA_PROLAPSE_SRC:-}" ]; then
    DRIVER=$("$PY" tools/driver_source.py path)
else
    DRIVER=$("$PY" tools/driver_source.py fetch) || die "fetching the pinned driver failed (vendor-image/pins.env NVIDIA_PROLAPSE_REF)"
fi
"$PY" tools/driver_source.py identity
if [ "$NATIVE_BUILDENV" != host ]; then
    # the image's one read-only toolchain must be the one the driver pins (rustup cannot install another here)
    want=$(sed -n 's/^channel *= *"\(.*\)"$/\1/p' "$DRIVER/rust-toolchain.toml")
    have=$(rustc +"$want" --version 2>/dev/null | cut -d' ' -f2 || true)
    [ -n "$want" ] && [ "$have" = "$want" ] || die "the driver pins Rust $want (rust-toolchain.toml), the build image has $(rustc --version): set RUST_VERSION=$want in packaging/native/Containerfile"
fi

say "hwcomposer toolchain + deps (NDK r29, AOSP headers, image link stubs)"
hwcomposer/provision.sh

say "system/vendor images (read-only: link stubs, runtime closures)"
# the image hwcomposer/provision.sh extracted its link stubs from: the pinned OTA download, or the
# local Waydroid images it preferred ($HWC_IMAGES / waydroid.cfg images_path)
SYSIMG=$(ls build/hwcomposer-work/deps/dl/system-img/*.img 2>/dev/null || true)
VENIMG=$(ls build/hwcomposer-work/deps/dl/vendor-img/*.img 2>/dev/null || true)
if [ -z "$SYSIMG" ] && [ "$NATIVE_BUILDENV" = host ]; then
    IMGDIR=${HWC_IMAGES:-$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg 2>/dev/null | head -1)}
    SYSIMG=${IMGDIR:+$IMGDIR/system.img} VENIMG=${IMGDIR:+$IMGDIR/vendor.img}
elif [ -z "$SYSIMG" ] || [ -z "$VENIMG" ]; then   # container: the pinned OTA images (hwcomposer/pins.env), as provision.sh fetches them
    # shellcheck source=../../hwcomposer/pins.env
    source hwcomposer/pins.env
    dl=build/hwcomposer-work/deps/dl
    for part in system vendor; do
        ls "$dl/$part-img/"*.img >/dev/null 2>&1 && continue
        u=IMG_${part^^}_URL s=IMG_${part^^}_SHA256 a=IMG_${part^^}_URL_ALT
        "$PY" tools/fetch.py --out "$dl/$part.zip" --sha256 "${!s}" "${!u}" "${!a}" || die "download failed: ${!u}"
        rm -rf "$dl/$part-img" && unzip -oq "$dl/$part.zip" -d "$dl/$part-img"
    done
    SYSIMG=$(ls $dl/system-img/*.img) VENIMG=$(ls $dl/vendor-img/*.img)
fi
[ -n "$SYSIMG" ] && [ -r "$SYSIMG" ] && [ -r "$VENIMG" ] || die "no system/vendor image: neither the pinned OTA download nor local Waydroid images (HWC_IMAGES)"
# absolute: sub-builds run from their own directories (the driver's gralloc make dumps link stubs from them)
SYSIMG=$(realpath -- "$SYSIMG") VENIMG=$(realpath -- "$VENIMG")
# the driver's android/provision.sh reads the same images (ANDROID_IMAGES = a directory with both)
mkdir -p build/driver-images
ln -sfn "$SYSIMG" build/driver-images/system.img
ln -sfn "$VENIMG" build/driver-images/vendor.img
export ANDROID_IMAGES=$REPO/build/driver-images

# ---------------------------------------------------------------- the targets, concurrently where independent
# Dependencies (everything else runs side by side):
#   driver (the driver repository's ./build.sh: NVK + Rust RM, nvrm, HAL, gralloc + minigbm, Codec2, bionic libdrm)
#   hwcomposer: only the steps above this section
#   driver -> glibc-shim (shims, the HAL over NVIDIA's ICD, gralloc backend, EGL front, libdrm census)
#   everything -> gate programs -> collect
# Each target logs to $LOGS/<target>.log; a failure names the target, prints the end of its log and stops
# the others. NATIVE_PARALLEL=0 runs the same targets one at a time, in declaration order. Each ninja keeps its -j
# and stops starting jobs while the load average exceeds the CPU count (NINJA_LOAD), so concurrent targets
# share the machine instead of multiplying it.
LOGS=${NATIVE_LOG_DIR:-$REPO/build/native-logs/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$LOGS"
: > "$LOGS/TIMES"
PARALLEL=${NATIVE_PARALLEL:-1}
JOBS=${NATIVE_JOBS:-}
JOBS=${JOBS:-$(nproc)}
export NINJA_LOAD=${NINJA_LOAD:-$(nproc)}
set -m                                 # every target in its own process group: a failure stops whole trees
T_START=$SECONDS

stop_all() { local p; for p in "${!BYPID[@]}"; do kill -TERM -- "-$p" 2>/dev/null || true; done; }
trap 'stop_all' INT TERM
declare -A DEPS=() CMD=() STATE=() BYPID=() T0=()
ORDER=()
target() {  # target NAME "DEP..." COMMAND...: declare (declaration order = the sequential order)
    local name=$1 deps=$2; shift 2
    DEPS[$name]=$deps; CMD[$name]=$(printf '%q ' "$@"); STATE[$name]=pending; ORDER+=("$name")
}
launch() {
    local name=$1
    T0[$name]=$SECONDS; STATE[$name]=running
    ( set -e; eval "${CMD[$name]}" ) > "$LOGS/$name.log" 2>&1 &
    BYPID[$!]=$name
    printf -- '-- %-15s started  (+%ss)\n' "$name" $(( SECONDS - T_START ))
}
run_targets() {   # start every target whose dependencies are done; the first failure stops everything
    local name dep ready running pid rc
    while :; do
        running=${#BYPID[@]}
        for name in "${ORDER[@]}"; do
            [ "${STATE[$name]}" = pending ] || continue
            [ "$PARALLEL" = 1 ] || [ "$running" -eq 0 ] || break
            ready=1
            for dep in ${DEPS[$name]}; do [ "${STATE[$dep]}" = done ] || ready=0; done
            [ "$ready" = 1 ] || { [ "$PARALLEL" = 1 ] && continue || break; }
            launch "$name"; running=$(( running + 1 ))
        done
        [ "${#BYPID[@]}" -gt 0 ] || break
        rc=0; wait -n -p pid || rc=$?
        name=${BYPID[$pid]}; unset "BYPID[$pid]"
        if [ "$rc" -ne 0 ]; then
            stop_all
            echo "build-native: target $name FAILED (exit $rc); its log: $LOGS/$name.log (last lines below)" >&2
            tail -n 60 "$LOGS/$name.log" >&2
            exit 1
        fi
        STATE[$name]=done
        printf -- '-- %-15s done in %ss\n' "$name" $(( SECONDS - T0[$name] ))
        echo "$name $(( SECONDS - T0[$name] ))" >> "$LOGS/TIMES"
    done
    for name in "${ORDER[@]}"; do
        [ "${STATE[$name]}" = done ] || { echo "build-native: target $name never became ready (deps: ${DEPS[$name]})" >&2; exit 1; }
    done
}

t_driver()     { JOBS=$JOBS "$DRIVER/build.sh"; }
t_hwcomposer() { hwcomposer/build.sh; }
# glibc-shim mode (NVIDIA's own userspace from the user's host through our glibc-compat shims): the guest side only.
# The shims are cut from the per-branch reference import lists (nvidia-prolapse/shim/imports): no NVIDIA driver here.
# The HAL and gralloc backend over NVIDIA's ICD are the driver repository's hal/ and gralloc/ glibc-shim targets;
# the libdrm they and NVIDIA's blobs load is the driver's, checked here for the blobs (shim/libdrm-census).
t_glibc_shim() { local why="release build: the builder has no NVIDIA driver; the per-branch reference lists are the census"
                 make -C nvidia-prolapse/shim REFERENCE_ONLY="$why"
                 make -C nvidia-prolapse/shim ARCH=x86 REFERENCE_ONLY="$why"
                 make -C nvidia-prolapse/shim/libdrm-census DRIVER="$DRIVER" NO_BLOB_CENSUS="$why"
                 make -C nvidia-prolapse/shim/libdrm-census DRIVER="$DRIVER" ARCH=x86 NO_BLOB_CENSUS="$why"
                 make -C "$DRIVER/hal" android android32
                 make -C "$DRIVER/gralloc" android android32 check check-minigbm SYSTEM_IMG="$SYSIMG" VENDOR_IMG="$VENIMG"
                 make -C nvidia-prolapse/egl sources DRIVER="$DRIVER"
                 make -C nvidia-prolapse/egl android android32 check DRIVER="$DRIVER"; }

say "targets ($([ "$PARALLEL" = 1 ] && echo concurrent || echo one at a time); ninja -j $JOBS -l $NINJA_LOAD; logs in $LOGS)"
target driver         ""                         t_driver
target hwcomposer     ""                         t_hwcomposer
target glibc-shim     "driver"                   t_glibc_shim
target gate           "driver glibc-shim"        "$PY" packaging/native/build-gate.py "$REPO/build/native-gate"
run_targets

say "collect the release bundle"
"$PY" packaging/native/collect.py "$OUT" "$REPO/build/native-gate"
echo "build-native: $(( SECONDS - T_START )) s for the targets; per target: $LOGS/TIMES"
[ -z "${NATIVE_CC_LAUNCHER:-}" ] || { echo "ccache ($CCACHE_DIR, max $CCACHE_MAXSIZE):"; ccache -s | sed 's/^/  /'; }
