#!/usr/bin/env bash
# packaging/install.sh — install a waydroid-nvidia release on any systemd distro (Arch users: AUR
# waydroid-nvidia-bin does the same). Installs exactly what the AUR package installs, then you run
#   sudo waydroid-nvidia-setup [-s GAPPS] (runs waydroid init if needed, starts the container); waydroid session start
#
#   curl -fsSL https://raw.githubusercontent.com/Shiro836/waydroid-nvidia/public-test/packaging/install.sh | sudo bash
#   sudo packaging/install.sh [--tag vX.Y.Z[-rcN]] [--from DIR] [--check] [--no-deps]
#   sudo packaging/install.sh --uninstall
#
#   --tag TAG     release to install (default: the newest release, prereleases included)
#   --from DIR    use release assets already in DIR (the four tarballs, the vendor image zip and channel JSON, the
#                 SELinux module .pp, SHA256SUMS, and the source tarball waydroid-nvidia-<TAG>.tar.gz) instead of
#                 downloading them; their <asset>.sigstore.json bundles, when present in DIR, are verified with
#                 cosign
#   --check       download and verify only; install nothing
#   --no-deps     do not install distro packages (each required tool is still checked by name)
#   --uninstall   put the official vendor image back (waydroid-nvidia-setup --restore-official-vendor), then
#                 remove every file a previous run installed (list in /var/lib/waydroid-nvidia/installed-files)
#
# Verification: every asset must have its own line in the release's SHA256SUMS. Every asset's build provenance is
# verified too when a verifier is available: cosign (>= 2.4.3, no GitHub login) against the Sigstore bundle the
# release ships next to each asset (<asset>.sigstore.json), else a logged-in GitHub CLI (gh attestation verify);
# without either the script says "not checked". The source tarball's commit (git get-tar-commit-id) must equal the commit recorded in the
# native bundle's BUILD-RECORD.json, so the tools and the binaries come from one CI build.
# Files owned by a distro package are never overwritten: the script stops and names the package.
#
# Credits: distro-family detection, per-family dependency lists, the one-line-per-asset SHA256SUMS rule
# and the interpreter pinning of /usr/lib/waydroid/waydroid.py follow quinovax/waydroid-nvidia
# packaging/install-from-release.sh; the verify-before-install --check mode follows
# ErenEksen/waydroid-nvidia-optimized install.sh.
set -euo pipefail
# Tool output parsed here and in every child (readelf, debugfs, lxc-info, systemctl, ...) is the untranslated text
export LC_ALL=C
unset LANGUAGE

GH_REPO=${WAYDROID_NVIDIA_REPO:-Shiro836/waydroid-nvidia}
WAYDROID_UPSTREAM=https://github.com/waydroid/waydroid
WAYDROID_SHA=a33a5c0b31d89d6ce687381104b30aff4dd2d330     # patches/waydroid/BASE
WAYDROID_TARBALL_SHA256=e7817beac7e26f7d54b2d9f49c847dab8e137c92e403825829e7abbd0b63c411
STATE=/var/lib/waydroid-nvidia
LIST=$STATE/installed-files
PREFIX=/usr/lib/waydroid-nvidia

die()  { echo "install.sh: $*" >&2; exit 1; }
info() { echo "==> $*"; }

TAG="" FROM="" CHECK=0 DEPS=1 UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG=${2:?--tag needs a value}; shift 2 ;;
        --from) FROM=$(cd "${2:?--from needs a directory}" && pwd); shift 2 ;;
        --check) CHECK=1; shift ;;
        --no-deps) DEPS=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,30p' "$0" 2>/dev/null || true; exit 0 ;;
        *) die "unknown argument: $1 (--help)" ;;
    esac
done
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
[ "$(uname -m)" = x86_64 ] || die "only x86_64 hosts are supported"

# ---------------------------------------------------------------- SELinux
# Where SELinux is enabled, Waydroid runs confined by Fedora's policy for it (packaging/selinux, module "waydroid",
# built by CI and shipped as the release asset waydroid-nvidia-selinux-<tag>.pp): without it an enforcing host denies
# Android's service managers the binder context manager and the container never boots. Loaded at priority 400, above
# the 200 a distribution package uses, so a leftover module of the distro's own waydroid package is shadowed rather
# than duplicated; SEMODULE records that this script loaded it.
SEMODULE=$STATE/selinux-module
selinux_on() { command -v selinuxenabled >/dev/null 2>&1 && selinuxenabled; }
relabel() {   # the paths waydroid.fc labels (its /var/lib/waydroid(.*)? also covers /var/lib/waydroid-nvidia), where they exist
    local paths=() p
    for p in /usr/lib/waydroid /var/lib/waydroid /var/lib/waydroid-nvidia /run/waydroid-*; do [ -e "$p" ] && paths+=("$p"); done
    # -x: not into a mounted rootfs (a running container mounts its images under /var/lib/waydroid)
    [ ${#paths[@]} -eq 0 ] || restorecon -R -x "${paths[@]}" ||
        die "restorecon -R -x ${paths[*]} failed (see above); fix the labels with that command, then re-run"
}

# ---------------------------------------------------------------- uninstall
if [ "$UNINSTALL" = 1 ]; then
    [ -f "$LIST" ] || die "$LIST missing: nothing was installed by this script"
    if grep -qx 'nvidia_prolapse_install = vendor-image' /var/lib/waydroid/waydroid.cfg 2>/dev/null; then
        info "putting the official vendor image back (waydroid-nvidia-setup --restore-official-vendor)"
        /usr/bin/waydroid-nvidia-setup --restore-official-vendor ||
            die "the official vendor image was not restored (see above); fix that first, or run 'sudo waydroid init -f' and re-run --uninstall"
    fi
    dropin=/etc/systemd/system/waydroid-container.service.d/source-native.conf
    if [ -f "$dropin" ] && grep -q "$PREFIX/native" "$dropin"; then rm -f "$dropin"; info "removed $dropin"; fi
    n=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if [ -e "$f" ] || [ -L "$f" ]; then rm -f -- "$f"; n=$((n + 1)); fi
    done < "$LIST"
    # empty directories the install created
    sort -r "$LIST" | while IFS= read -r f; do d=$(dirname -- "$f"); rmdir --ignore-fail-on-non-empty -p -- "$d" 2>/dev/null || true; done
    rm -f "$LIST"
    if [ -f "$SEMODULE" ]; then
        semodule -X 400 -r waydroid ||
            die "semodule -X 400 -r waydroid failed (see above); remove the waydroid policy module with it, then re-run"
        rm -f "$SEMODULE"
        ! selinux_on || relabel
        info "removed the waydroid SELinux policy module; Waydroid's data is relabelled"
    fi
    systemctl daemon-reload 2>/dev/null || true
    systemctl reload dbus.service 2>/dev/null || systemctl reload dbus-broker.service 2>/dev/null || true
    info "removed $n files"
    cat <<'EOF'
Gate logs, old staged driver roots and the Waydroid data stay on disk. Remove the first two with
  sudo rm -rf /var/lib/waydroid-nvidia /var/lib/waydroid/nvidia-source-native-* /var/lib/waydroid/nvidia-prolapse
EOF
    exit 0
fi

# ---------------------------------------------------------------- distro family
detect_family() {   # attribute a system file to exactly one package manager
    local probe hits=""
    for probe in /usr/lib/os-release /etc/os-release /bin/sh; do [ -e "$probe" ] && break; done
    if command -v dpkg-query >/dev/null 2>&1 && dpkg-query -S "$probe" >/dev/null 2>&1; then hits="$hits debian"; fi
    if command -v rpm >/dev/null 2>&1 && rpm -qf "$probe" >/dev/null 2>&1; then hits="$hits fedora"; fi
    if command -v pacman >/dev/null 2>&1 && pacman -Qo "$probe" >/dev/null 2>&1; then hits="$hits arch"; fi
    hits=${hits# }
    case "$hits" in
        debian|fedora|arch) echo "$hits" ;;
        "") if command -v apt-get >/dev/null 2>&1; then echo debian
            elif command -v dnf >/dev/null 2>&1; then echo fedora
            elif command -v pacman >/dev/null 2>&1; then echo arch
            else die "cannot detect the package manager (apt, dnf or pacman); install the dependencies yourself and use --no-deps"; fi ;;
        *) die "ambiguous package manager for $probe ($hits)" ;;
    esac
}
owner_of() {   # the distro package owning a path, or nothing
    case "$FAMILY" in
        debian) dpkg-query -S "$1" 2>/dev/null | head -1 | cut -d: -f1 ;;
        fedora) rpm -qf "$1" 2>/dev/null | grep -v 'not owned' | head -1 ;;
        arch) pacman -Qqo "$1" 2>/dev/null | head -1 ;;
    esac
}
FAMILY=$(detect_family)
info "package manager family: $FAMILY"

if [ "$DEPS" = 1 ] && [ "$CHECK" = 0 ]; then
    case "$FAMILY" in
        debian)
            apt-get update -q
            apt-get install -yq lxc python3 python3-gi python3-dbus nftables dnsmasq gir1.2-gtk-3.0 \
                binutils e2fsprogs util-linux zstd curl git make patch \
                libepoxy0 libdrm2 libgbm1 libx11-6 libexpat1 libvulkan1
            apt-get install -yq python3-gbinder || die "python3-gbinder is not available: add the Waydroid repository
(curl -fsSL https://repo.waydro.id | sudo bash) and re-run" ;;
        fedora)
            sepkgs=()
            ! selinux_on || sepkgs=(container-selinux)   # the waydroid policy module requires its types
            dnf install -y lxc python3 python3-gobject python3-dbus python3-gbinder nftables dnsmasq gtk3 \
                binutils e2fsprogs util-linux zstd curl git make patch \
                libepoxy libdrm mesa-libgbm libX11 expat vulkan-loader "${sepkgs[@]}" ;;
        arch)
            # Install only what is missing: `pacman -S --needed` would also UPGRADE installed-but-older packages one
            # by one, a partial upgrade that breaks other packages (e.g. expat vs lib32-expat). Never upgrade here.
            arch_pkgs=(lxc python python-gobject python-dbus python-gbinder nftables dnsmasq gtk3 \
                binutils e2fsprogs util-linux zstd curl git make patch libepoxy libdrm mesa libx11 expat vulkan-icd-loader)
            mapfile -t arch_missing < <(pacman -T "${arch_pkgs[@]}" || true)
            if [ "${#arch_missing[@]}" -gt 0 ]; then
                echo "==> installing missing packages: ${arch_missing[*]}"
                pacman -S --noconfirm "${arch_missing[@]}" \
                    || die "pacman could not install ${arch_missing[*]} without upgrading other packages; bring the system up to date first (sudo pacman -Syu), then re-run this script"
            fi ;;
    esac
fi
need=(curl tar zstd gzip sha256sum python3 git)          # verification (--check needs only these)
[ "$CHECK" = 1 ] || need+=(make patch readelf debugfs)
for tool in "${need[@]}"; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is required (install it${DEPS:+, or drop --no-deps}; --check installs no packages)"
done
# the tools and Waydroid's generator use Python 3.11 (hashlib.file_digest)
[ "$CHECK" = 1 ] || python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' ||
    die "Python 3.11 or newer is required, $(command -v python3) is $(python3 -V 2>&1) (Ubuntu 22.04 ships 3.10: use Ubuntu 24.04 or newer)"

# ---------------------------------------------------------------- assets
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
fetch() { curl --fail --location --proto '=https' --proto-redir '=https' --retry 2 --connect-timeout 15 -sS -o "$2" "$1"; }
if [ -z "$TAG" ]; then
    [ -z "$FROM" ] || die "--from needs --tag (the asset names carry it)"
    fetch "https://api.github.com/repos/$GH_REPO/releases?per_page=1" "$WORK/releases.json" ||
        die "cannot query the releases of $GH_REPO; pass --tag"
    TAG=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["tag_name"])' "$WORK/releases.json")
fi
[[ "$TAG" =~ ^v[0-9A-Za-z.+-]+$ ]] || die "invalid tag '$TAG'"
info "release $TAG"
ASSETS=(waydroid-nvidia-host-x86_64-$TAG.tar.zst waydroid-nvidia-guest-android-x86_64-$TAG.tar.zst
        waydroid-nvidia-guest-prebuilts-$TAG.tar.zst waydroid-nvidia-native-x86_64-$TAG.tar.zst
        waydroid-nvidia-vendor-x86_64-$TAG.zip waydroid-nvidia-vendor-ota-$TAG.json)
SRC_TAR=waydroid-nvidia-$TAG.tar.gz
SEPP=waydroid-nvidia-selinux-$TAG.pp     # packaging/selinux built by CI; releases before it have none
base=https://github.com/$GH_REPO/releases/download/$TAG
if [ -n "$FROM" ]; then
    [ -f "$FROM/SHA256SUMS" ] || die "$FROM/SHA256SUMS missing"; cp -- "$FROM/SHA256SUMS" "$WORK/"
else
    fetch "$base/SHA256SUMS" "$WORK/SHA256SUMS" || die "cannot download $base/SHA256SUMS"
fi
if grep -Eq " [ *]?(\./)?${SEPP//./\\.}\$" "$WORK/SHA256SUMS"; then
    ASSETS+=("$SEPP")
elif selinux_on && [ "$CHECK" = 0 ]; then
    die "release $TAG ships no SELinux policy module ($SEPP), which Waydroid needs on this SELinux host.
Install a newer release (--tag), or install on a host without SELinux"
fi
if [ -n "$FROM" ]; then
    for f in "${ASSETS[@]}" "$SRC_TAR"; do
        [ -f "$FROM/$f" ] || die "$FROM/$f missing"; cp -- "$FROM/$f" "$WORK/$f"
    done
    for f in "${ASSETS[@]}"; do [ ! -f "$FROM/$f.sigstore.json" ] || cp -- "$FROM/$f.sigstore.json" "$WORK/"; done
else
    for f in "${ASSETS[@]}"; do info "downloading $f"; fetch "$base/$f" "$WORK/$f" || die "cannot download $base/$f"; done
    # Sigstore attestation bundles (releases from v0.2.0-rc2 on): a missing one only means cosign cannot be used
    for f in "${ASSETS[@]}"; do fetch "$base/$f.sigstore.json" "$WORK/$f.sigstore.json" 2>/dev/null || rm -f "$WORK/$f.sigstore.json"; done
    fetch "https://github.com/$GH_REPO/archive/refs/tags/$TAG.tar.gz" "$WORK/$SRC_TAR" || die "cannot download the $TAG source"
fi
# one line per asset (sha256sum -c --ignore-missing would pass a SHA256SUMS that omits an asset)
: > "$WORK/verify"
for f in "${ASSETS[@]}"; do
    grep -E "^[0-9a-f]{64} [ *]?(\./)?${f//./\\.}\$" "$WORK/SHA256SUMS" | sed -E 's#([ *])\./#\1#' >> "$WORK/verify" ||
        die "SHA256SUMS has no line for $f; refusing to install it"
done
(cd "$WORK" && sha256sum -c --strict --quiet verify) || die "SHA256 verification failed"
info "SHA256SUMS: ${#ASSETS[@]} assets verified"
# Build provenance. 1) cosign against the release's Sigstore bundles: no GitHub account needed; the signing
# certificate must name this repository's build.yml at this tag, issued for GitHub Actions. cosign before 2.4.3
# cannot read the bundle format GitHub writes (v0.3), so it does not count as a verifier.
# 2) gh attestation verify needs a logged-in GitHub CLI. Under sudo root usually isn't logged in, so run gh as the
# user who invoked sudo when that user is; an unauthenticated gh is "not checked", never "does not verify".
verifier="" why=""
have_bundles=1
for f in "${ASSETS[@]}"; do [ -f "$WORK/$f.sigstore.json" ] || have_bundles=0; done
cosign_ver=""
if command -v cosign >/dev/null 2>&1; then
    cosign_ver=$(cosign version --json 2>/dev/null | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["gitVersion"].lstrip("v"))' 2>/dev/null || true)
fi
if [ "$have_bundles" = 1 ] && [ -n "$cosign_ver" ] &&
   [ "$(printf '%s\n' 2.4.3 "${cosign_ver%%[-+]*}" | sort -V | head -1)" = 2.4.3 ]; then
    verifier=cosign
fi
gh_run=(gh)
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ] && command -v runuser >/dev/null 2>&1; then
    gh_run=(runuser -u "$SUDO_USER" -- gh)
    chmod -R a+rX "$WORK"
fi
if [ -z "$verifier" ]; then
    if [ -n "$FROM" ]; then why="local assets from $FROM without cosign >= 2.4.3 and their .sigstore.json bundles"
    elif ! command -v gh >/dev/null 2>&1; then why="neither cosign >= 2.4.3${cosign_ver:+ (have $cosign_ver)} nor the GitHub CLI (gh) installed"
    elif ! "${gh_run[@]}" auth status >/dev/null 2>&1; then why="no cosign >= 2.4.3${cosign_ver:+ (have $cosign_ver)}, and gh is not logged in (run: gh auth login)"
    else verifier=gh
    fi
    [ "$have_bundles" = 1 ] || [ -n "$FROM" ] || why="$why; this release ships no .sigstore.json bundles"
fi
case "$verifier" in
    cosign)
        for f in "${ASSETS[@]}"; do
            # cosign refuses blobs above COSIGN_MAX_ATTACHMENT_SIZE (default 128 MiB; the vendor zip is larger)
            out=$(COSIGN_MAX_ATTACHMENT_SIZE=$(( $(stat -c %s "$WORK/$f") + 1 )) cosign verify-blob-attestation --bundle "$WORK/$f.sigstore.json" --type slsaprovenance1 \
                --certificate-identity "https://github.com/$GH_REPO/.github/workflows/build.yml@refs/tags/$TAG" \
                --certificate-oidc-issuer https://token.actions.githubusercontent.com "$WORK/$f" 2>&1) ||
                die "build provenance of $f does not verify (cosign verify-blob-attestation --bundle $f.sigstore.json): ${out##*$'\n'}"
        done
        info "build provenance verified (cosign $cosign_ver, Sigstore bundles: build.yml at $TAG in $GH_REPO)" ;;
    gh)
        for f in "${ASSETS[@]}"; do
            "${gh_run[@]}" attestation verify "$WORK/$f" --repo "$GH_REPO" >/dev/null ||
                die "build provenance of $f does not verify (gh attestation verify)"
        done
        info "build provenance verified (gh attestation verify)" ;;
    *)  info "build provenance NOT verified ($why): SHA256SUMS only" ;;
esac
RECORD_COMMIT=$(tar --zstd -xOf "$WORK/waydroid-nvidia-native-x86_64-$TAG.tar.zst" ./BUILD-RECORD.json |
                python3 -I -c 'import json,sys; print(json.load(sys.stdin)["commit"])')
# git reads only the first header: gzip then dies of SIGPIPE, which pipefail would report
SRC_COMMIT=$(gzip -dc "$WORK/$SRC_TAR" 2>/dev/null | git get-tar-commit-id || true)
[[ "$SRC_COMMIT" =~ ^[0-9a-f]{40}$ ]] || die "$SRC_TAR carries no commit id (not a git archive of the release tag)"
[ "$SRC_COMMIT" = "$RECORD_COMMIT" ] || die "source tarball is commit $SRC_COMMIT, the native bundle was built from $RECORD_COMMIT"
info "source $SRC_COMMIT = native bundle build commit"
if [ "$CHECK" = 1 ]; then info "--check: verified, nothing installed"; exit 0; fi

# ---------------------------------------------------------------- stage into a package root
ROOT=$WORK/root SRC=$WORK/src
mkdir -p "$ROOT" "$SRC"
tar -xzf "$WORK/$SRC_TAR" -C "$SRC" --strip-components=1
[ -f "$SRC/packaging/native/install-native.sh" ] || die "$SRC_TAR is not a waydroid-nvidia source tree"
info "patched waydroid ($WAYDROID_SHA + patches/waydroid/0001 + waydroid/0001-lxc-nvidia-native-mounts)"
fetch "$WAYDROID_UPSTREAM/archive/$WAYDROID_SHA.tar.gz" "$WORK/waydroid.tar.gz" || die "cannot download waydroid $WAYDROID_SHA"
echo "$WAYDROID_TARBALL_SHA256  $WORK/waydroid.tar.gz" | sha256sum -c --quiet || die "waydroid source tarball checksum mismatch"
mkdir -p "$WORK/waydroid" && tar -xzf "$WORK/waydroid.tar.gz" -C "$WORK/waydroid" --strip-components=1
patch -s -d "$WORK/waydroid" -p1 < "$SRC/patches/waydroid/0001-nvidia-integration.patch"
patch -s -d "$WORK/waydroid" -p1 < "$SRC/waydroid/0001-lxc-nvidia-native-mounts.patch"
cmp -s "$SRC/waydroid/nvidia_native.py" "$WORK/waydroid/tools/helpers/nvidia_native.py" || die "patched generator differs from the release's"
NFT=1; command -v nft >/dev/null 2>&1 || NFT=0
make -s -C "$WORK/waydroid" install DESTDIR="$ROOT" USE_NFTABLES=$NFT >/dev/null
# an absolute interpreter that has Waydroid's bindings: `#!/usr/bin/env python3` follows the caller's PATH
PYBIN=""
for cand in /usr/bin/python3 /usr/local/bin/python3; do
    [ -x "$cand" ] && "$cand" -c 'import dbus, gi, gbinder' >/dev/null 2>&1 && { PYBIN=$cand; break; }
done
[ -n "$PYBIN" ] || die "no system python3 with the dbus, gobject and gbinder bindings (install python3-dbus, python3-gi/python3-gobject, python3-gbinder)"
"$PYBIN" -c 'import sys; sys.exit(sys.version_info < (3, 11))' || die "$PYBIN is $("$PYBIN" -V 2>&1); Python 3.11 or newer is required"
sed -i "1s|^#!.*python3.*$|#!$PYBIN|" "$ROOT/usr/lib/waydroid/waydroid.py"

info "host Venus renderer, guest stack, nvidia-prolapse vendor image, bundle and tools"
mkdir -p "$WORK/host" "$WORK/guest"
tar --zstd -xf "$WORK/waydroid-nvidia-host-x86_64-$TAG.tar.zst" -C "$WORK/host"
tar --zstd -xf "$WORK/waydroid-nvidia-guest-android-x86_64-$TAG.tar.zst" -C "$WORK/guest"
tar --zstd -xf "$WORK/waydroid-nvidia-guest-prebuilts-$TAG.tar.zst" -C "$WORK/guest"
for f in virgl_test_server virgl_render_server libvirglrenderer.so.1; do
    install -Dm755 "$WORK/host/$f" "$ROOT$PREFIX/$f"
done
for rel in vendor/lib/hw/vulkan.virtio.so vendor/lib/egl/libEGL_angle.so vendor/lib/egl/libGLESv1_CM_angle.so \
           vendor/lib/egl/libGLESv2_angle.so vendor/lib64/hw/vulkan.virtio.so vendor/lib64/egl/libEGL_angle.so \
           vendor/lib64/egl/libGLESv1_CM_angle.so vendor/lib64/egl/libGLESv2_angle.so \
           vendor/lib64/libgbm_mesa_wrapper.so vendor/lib64/hw/hwcomposer.waydroid.so; do
    [ -f "$WORK/guest/$rel" ] || die "release guest payload lacks $rel"
    install -Dm644 "$WORK/guest/$rel" "$ROOT$PREFIX/guest/$rel"
done
install -Dm755 "$WORK/guest/system/bin/surfaceflinger" "$ROOT$PREFIX/guest/system/bin/surfaceflinger"
bash "$SRC/packaging/native/install-native.sh" "$SRC" "$WORK/waydroid-nvidia-native-x86_64-$TAG.tar.zst" \
    "$WORK/waydroid-nvidia-vendor-x86_64-$TAG.zip" "$WORK/waydroid-nvidia-vendor-ota-$TAG.json" "$ROOT" "$TAG"
A=$SRC/packaging/aur/waydroid-nvidia-bin
install -Dm644 "$A/wd-venus.service" "$ROOT/usr/lib/systemd/user/wd-venus.service"
install -Dm644 "$A/waydroid-venus.tmpfiles" "$ROOT/usr/lib/tmpfiles.d/waydroid-venus.conf"
install -Dm644 "$A/waydroid-nvidia.rules" "$ROOT/usr/lib/udev/rules.d/70-waydroid-nvidia.rules"
install -Dm644 "$A/waydroid-nvidia-gpu.rules" "$ROOT/usr/lib/udev/rules.d/70-waydroid-nvidia-gpu.rules"
install -Dm755 "$A/waydroid-nvidia-setup" "$ROOT/usr/bin/waydroid-nvidia-setup"
install -Dm755 "$SRC/packaging/install.sh" "$ROOT$PREFIX/install.sh"     # for --uninstall

# ---------------------------------------------------------------- conflicts, copy, file list
(cd "$ROOT" && find . \( -type f -o -type l \) | sed 's#^\.##' | sort) > "$WORK/files"
conflicts=()
while IFS= read -r f; do
    if [ -e "$f" ] || [ -L "$f" ]; then
        pkg=$(owner_of "$f" || true)
        [ -z "$pkg" ] || conflicts+=("$f ($pkg)")
    fi
done < "$WORK/files"
[ ${#conflicts[@]} -eq 0 ] || die "these files belong to distro packages; remove them first (e.g. the distro's waydroid):
  ${conflicts[*]:0:5}"
if [ -f "$LIST" ]; then   # upgrade: drop files the previous release had and this one does not
    comm -23 <(sort "$LIST") "$WORK/files" | while IFS= read -r f; do rm -f -- "$f"; done
fi
# existing directories keep their owner, mode and times (--no-overwrite-dir)
tar -C "$ROOT" -cf - . | tar -C / -xpf - --no-overwrite-dir --no-same-owner
mkdir -p "$STATE" && cp "$WORK/files" "$LIST"
if selinux_on; then
    # semodule names a module after its file: install it as waydroid.pp
    mkdir "$WORK/selinux" && cp "$WORK/$SEPP" "$WORK/selinux/waydroid.pp"
    semodule -X 400 -i "$WORK/selinux/waydroid.pp" ||
        die "semodule could not load the waydroid policy module (see above); without it an enforcing host refuses the container"
    echo "waydroid 400" > "$SEMODULE"
    relabel
    info "SELinux: policy module waydroid loaded (priority 400); /usr/lib/waydroid, /var/lib/waydroid{,-nvidia} relabelled"
fi
systemd-tmpfiles --create /usr/lib/tmpfiles.d/waydroid-venus.conf 2>/dev/null || true
# What a package manager's post-install hooks would do: the running services must see the new files.
# Without the DBus reload the container manager's bus name is refused by the old policy and
# waydroid-container.service fails to start ("Request to own name refused by policy").
udevadm control --reload 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true
systemctl reload dbus.service 2>/dev/null || systemctl reload dbus-broker.service 2>/dev/null \
    || die "could not reload the system DBus configuration; run: sudo systemctl reload dbus (or reboot), then sudo waydroid-nvidia-setup"
info "installed $(wc -l < "$LIST") files (list: $LIST; remove with: sudo $PREFIX/install.sh --uninstall)"
cat <<'EOF'

Next (README steps 2 and 3):
  sudo waydroid-nvidia-setup     (downloads the Android image if needed, checks your GPU, installs the vendor image,
                                  starts the container; add -s GAPPS for Google apps on a fresh install)
  waydroid session start
EOF
