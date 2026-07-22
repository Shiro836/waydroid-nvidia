#!/usr/bin/env bash
# Clean-room provisioning for the hwcomposer build (closes the PREREQS.md gap).
# Builds every hand-provisioned $WNV piece from pinned sources:
#   stage clones    — AOSP header repos, shallow at pinned SHAs
#   stage hidlgen   — standalone hidl-gen (build/hidl-gen CMake)
#   stage hidlout   — vendor.waydroid + android.hidl/hardware C++ headers
#   stage staticdeps— static wayland-client/libffi/libxkbcommon for bionic
#   stage imagelibs — link-time .so stubs extracted from the pinned OTA images
#                     (debugfs, no mounts — works unprivileged in CI)
#
# Usage: provision.sh WNV_DIR HWC_SRC [stage...]   (default: all stages)
# Env:   NDK (default /opt/android-ndk), REPO (repo root, auto-detected)
set -euo pipefail

WNV="${1:?usage: provision.sh WNV_DIR HWC_SRC [stage...]}"
HWC_SRC="${2:?usage: provision.sh WNV_DIR HWC_SRC [stage...]}"
shift 2
STAGES=("${@:-clones hidlgen hidlout staticdeps imagelibs}")
[ $# -gt 0 ] && STAGES=("$@") || STAGES=(clones hidlgen hidlout staticdeps imagelibs)

NDK="${NDK:-/opt/android-ndk}"
REPO="${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}"
API=30
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
TARGET=x86_64-linux-android

source "$REPO/packaging/ci/pins.env"
source "$REPO/packaging/ci/hwc-pins.env"
mkdir -p "$WNV"

has_stage() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

fetch_aosp() { # dir repo sha
    local dir="$WNV/$1" repo="$AOSP_BASE/$2" sha="$3"
    if [ -e "$dir/.provisioned" ] && [ "$(cat "$dir/.provisioned")" = "$sha" ]; then
        echo "  $1: already at $sha"; return 0
    fi
    rm -rf "$dir"; git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$repo" "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
    echo "$sha" > "$dir/.provisioned"
    echo "  $1: fetched $sha"
}

if has_stage clones; then
    echo "== stage clones"
    fetch_aosp aosp_fmq                 system/libfmq           "$AOSP_FMQ_SHA"
    fetch_aosp aosp_fmtlib              external/fmtlib         "$AOSP_FMTLIB_SHA"
    fetch_aosp aosp_frameworks_native   frameworks/native       "$AOSP_FRAMEWORKS_NATIVE_SHA"
    fetch_aosp aosp_hardware_interfaces hardware/interfaces     "$AOSP_HARDWARE_INTERFACES_SHA"
    fetch_aosp aosp_hidl                system/tools/hidl       "$AOSP_HIDL_SHA"
    fetch_aosp aosp_libbase             system/libbase          "$AOSP_LIBBASE_SHA"
    fetch_aosp aosp_libcxx              external/libcxx         "$AOSP_LIBCXX_SHA"
    fetch_aosp aosp_libcxxabi           external/libcxxabi      "$AOSP_LIBCXXABI_SHA"
    fetch_aosp aosp_libhardware         hardware/libhardware    "$AOSP_LIBHARDWARE_SHA"
    fetch_aosp aosp_libhidl             system/libhidl          "$AOSP_LIBHIDL_SHA"
    fetch_aosp aosp_libhwbinder         system/libhwbinder      "$AOSP_LIBHWBINDER_SHA"
    fetch_aosp aosp_system_core         system/core             "$AOSP_SYSTEM_CORE_SHA"
    # minigbm (cros_gralloc handle layout) — same pin the gralloc job uses
    if [ ! -e "$WNV/minigbm/.provisioned" ]; then
        rm -rf "$WNV/minigbm"; git init -q "$WNV/minigbm"
        git -C "$WNV/minigbm" fetch -q --depth 1 "$MINIGBM_UPSTREAM" "$MINIGBM_SHA"
        git -C "$WNV/minigbm" checkout -q FETCH_HEAD
        echo "$MINIGBM_SHA" > "$WNV/minigbm/.provisioned"
        echo "  minigbm: fetched $MINIGBM_SHA"
    fi
    # liblog for the host hidl-gen build (LineageOS mirror)
    if [ ! -e "$WNV/android_system_logging/.provisioned" ]; then
        rm -rf "$WNV/android_system_logging"; git init -q "$WNV/android_system_logging"
        git -C "$WNV/android_system_logging" fetch -q --depth 1 "$SYSTEM_LOGGING_URL" "$SYSTEM_LOGGING_SHA"
        git -C "$WNV/android_system_logging" checkout -q FETCH_HEAD
        echo "$SYSTEM_LOGGING_SHA" > "$WNV/android_system_logging/.provisioned"
        echo "  android_system_logging: fetched $SYSTEM_LOGGING_SHA"
    fi
fi

if has_stage hidlgen; then
    echo "== stage hidlgen"
    cmake -S "$REPO/build/hidl-gen" -B "$WNV/hidl-gen-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DWNV="$WNV" >/dev/null
    cmake --build "$WNV/hidl-gen-build" -j"$(nproc)" >/dev/null
    "$WNV/hidl-gen-build/hidl-gen" --version || true
fi

if has_stage hidlout; then
    echo "== stage hidlout"
    HG="$WNV/hidl-gen-build/hidl-gen"
    OUTD="$WNV/hidl-out"
    rm -rf "$OUTD"; mkdir -p "$OUTD"
    ROOTS=(-r "vendor.waydroid:$HWC_SRC/interfaces"
           -r "android.hidl:$WNV/aosp_libhidl/transport"
           -r "android.hardware:$WNV/aosp_hardware_interfaces")
    for pkg in vendor.waydroid.clipboard@1.0 \
               vendor.waydroid.display@1.0 vendor.waydroid.display@1.1 vendor.waydroid.display@1.2 \
               vendor.waydroid.task@1.0 \
               vendor.waydroid.window@1.0 vendor.waydroid.window@1.1 vendor.waydroid.window@1.2 \
               android.hidl.base@1.0 \
               android.hidl.manager@1.0 android.hidl.manager@1.1 android.hidl.manager@1.2 \
               android.hidl.memory@1.0 \
               android.hardware.graphics.common@1.0 android.hardware.graphics.common@1.1 \
               android.hardware.graphics.common@1.2 \
               android.hardware.graphics.composer@2.1; do
        "$HG" -o "$OUTD" -L c++-headers "${ROOTS[@]}" "$pkg"
    done
    echo "  generated $(find "$OUTD" -name '*.h' | wc -l) headers"
fi

if has_stage staticdeps; then
    echo "== stage staticdeps"
    PREFIX="$WNV/ndk-prefix"
    mkdir -p "$PREFIX" "$WNV/dl"
    export CC="$TC/bin/${TARGET}${API}-clang" CXX="$TC/bin/${TARGET}${API}-clang++"
    export AR="$TC/bin/llvm-ar" RANLIB="$TC/bin/llvm-ranlib" STRIP="$TC/bin/llvm-strip"

    # libffi (autotools)
    if [ ! -e "$PREFIX/lib/libffi.a" ]; then
        curl -sSfLo "$WNV/dl/libffi.tar.gz" "$LIBFFI_URL"
        rm -rf "$WNV/build-libffi"; mkdir -p "$WNV/build-libffi"
        tar -C "$WNV/build-libffi" --strip-components=1 -xzf "$WNV/dl/libffi.tar.gz"
        (cd "$WNV/build-libffi" && ./configure --host=$TARGET --prefix="$PREFIX" \
            --disable-shared --enable-static --disable-docs >/dev/null && \
            make -j"$(nproc)" >/dev/null && make install >/dev/null)
        echo "  libffi $LIBFFI_VERSION"
    fi

    # cross file shared by the meson builds. pkg_config_path applies to
    # TARGET deps only (finds our cross-built libffi); native deps like the
    # host wayland-scanner keep the system pkg-config environment.
    CROSS="$WNV/ndk-cross.ini"
    cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'
[built-in options]
pkg_config_path = '$PREFIX/lib/pkgconfig'
[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

    # wayland (meson; client libs only). The cross build needs a NATIVE
    # wayland-scanner >= the target version — distro scanners can be older
    # (Ubuntu 24.04: 1.22 vs our 1.25), so build one from the same tarball.
    if [ ! -e "$PREFIX/lib/libwayland-client.a" ]; then
        curl -sSfLo "$WNV/dl/wayland.tar.xz" "$WAYLAND_URL"
        rm -rf "$WNV/build-wayland"; mkdir -p "$WNV/build-wayland"
        tar -C "$WNV/build-wayland" --strip-components=1 -xJf "$WNV/dl/wayland.tar.xz"
        NATPREFIX="$WNV/native-prefix"
        if [ ! -x "$NATPREFIX/bin/wayland-scanner" ]; then
            env -u CC -u CXX -u AR -u RANLIB -u STRIP \
                meson setup "$WNV/build-wayland/native" "$WNV/build-wayland" \
                -Dprefix="$NATPREFIX" -Dlibraries=false -Dscanner=true \
                -Ddocumentation=false -Dtests=false -Ddtd_validation=false >/dev/null
            ninja -C "$WNV/build-wayland/native" install >/dev/null
            echo "  native wayland-scanner $WAYLAND_VERSION"
        fi
        export PKG_CONFIG_PATH_FOR_BUILD="$NATPREFIX/lib/x86_64-linux-gnu/pkgconfig:$NATPREFIX/lib/pkgconfig:$NATPREFIX/share/pkgconfig"
        export PATH="$NATPREFIX/bin:$PATH"
        meson setup "$WNV/build-wayland/b" "$WNV/build-wayland" --cross-file "$CROSS" \
            -Ddefault_library=static -Dprefix="$PREFIX" \
            -Ddocumentation=false -Dtests=false -Ddtd_validation=false \
            -Dscanner=false
        ninja -C "$WNV/build-wayland/b" install >/dev/null
        echo "  wayland $WAYLAND_VERSION"
    fi

    # libxkbcommon (meson)
    if [ ! -e "$PREFIX/lib/libxkbcommon.a" ]; then
        curl -sSfLo "$WNV/dl/xkbcommon.tar.xz" "$XKBCOMMON_URL"
        rm -rf "$WNV/build-xkb"; mkdir -p "$WNV/build-xkb"
        tar -C "$WNV/build-xkb" --strip-components=1 -xJf "$WNV/dl/xkbcommon.tar.xz"
        meson setup "$WNV/build-xkb/b" "$WNV/build-xkb" --cross-file "$CROSS" \
            -Ddefault_library=static -Dprefix="$PREFIX" \
            -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
            -Denable-tools=false -Denable-xkbregistry=false >/dev/null
        # xkbcommon 1.7 has no tests toggle and its tests need API-31 ICU —
        # build and install only the library target
        ninja -C "$WNV/build-xkb/b" libxkbcommon.a >/dev/null
        install -Dm644 "$WNV/build-xkb/b/libxkbcommon.a" "$PREFIX/lib/libxkbcommon.a"
        mkdir -p "$PREFIX/include"
        cp -r "$WNV/build-xkb/include/xkbcommon" "$PREFIX/include/"
        cat > "$PREFIX/lib/pkgconfig/xkbcommon.pc" <<EOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: xkbcommon
Description: XKB API common to servers and clients
Version: $XKBCOMMON_VERSION
Libs: -L\${libdir} -lxkbcommon
Cflags: -I\${includedir}
EOF
        echo "  xkbcommon $XKBCOMMON_VERSION (library only)"
    fi
fi

if has_stage staticdeps; then
    # libdrm headers (build.sh includes them from mesa's subproject layout)
    if [ ! -d "$WNV/mesa/subprojects/libdrm-$LIBDRM_VERSION" ]; then
        curl -sSfLo "$WNV/dl/libdrm.tar.xz" "$LIBDRM_URL"
        mkdir -p "$WNV/mesa/subprojects"
        tar -C "$WNV/mesa/subprojects" -xJf "$WNV/dl/libdrm.tar.xz"
        echo "  libdrm headers $LIBDRM_VERSION"
    fi
fi

if has_stage imagelibs; then
    echo "== stage imagelibs"
    ROOTFS="$WNV/image-rootfs"
    if [ ! -e "$ROOTFS/.provisioned" ]; then
        mkdir -p "$WNV/dl" "$ROOTFS"
        for part in system vendor; do
            zipvar="IMG_$(echo $part | tr a-z A-Z)_URL"; shavar="IMG_$(echo $part | tr a-z A-Z)_SHA256"
            zip="$WNV/dl/$part.zip"
            [ -f "$zip" ] || curl -sSfLo "$zip" "${!zipvar}"
            echo "${!shavar}  $zip" | sha256sum -c >/dev/null
            unzip -oq "$zip" -d "$WNV/dl/$part-img"
        done
        # extract only what the link needs; debugfs needs no privileges.
        # NOTE: rdump creates the leaf dir itself — pre-create parents only.
        SYSIMG=$(ls "$WNV"/dl/system-img/*.img); VENIMG=$(ls "$WNV"/dl/vendor-img/*.img)
        rm -rf "$ROOTFS"; mkdir -p "$ROOTFS/system" "$ROOTFS/apex/com.android.vndk.v33" "$ROOTFS/vendor"
        debugfs -R "rdump /system/lib64 $ROOTFS/system" "$SYSIMG" 2>&1 | grep -v '^debugfs' || true
        debugfs -R "rdump /system/apex/com.android.vndk.v33/lib64 $ROOTFS/apex/com.android.vndk.v33" "$SYSIMG" 2>&1 | grep -v '^debugfs' || true
        debugfs -R "rdump /lib64 $ROOTFS/vendor" "$VENIMG" 2>&1 | grep -v '^debugfs' || true
        # the vndk apex may be a flattened dir or an .apex file depending on
        # the image; verify the libs the link actually needs are present
        NLIBS=$(find "$ROOTFS" -name '*.so' | wc -l)
        for lib in "$ROOTFS/vendor/lib64/vendor.waydroid.display@1.0.so" \
                   "$ROOTFS/system/lib64/libhidlbase.so"; do
        [ -f "$lib" ] || { echo "ERROR: expected $lib missing after extraction" >&2; exit 1; }
        done
        touch "$ROOTFS/.provisioned"
    fi
    echo "  rootfs libs: $(find "$ROOTFS" -name '*.so' | wc -l) shared objects"
fi

echo "== provisioning complete: $WNV"
