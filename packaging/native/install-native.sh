#!/usr/bin/env bash
# packaging/native/install-native.sh — install the nvidia-prolapse mode's release bundle and the tools that
# stage, gate and activate it, into a package root. Used by the AUR PKGBUILD (package()) and by
# packaging/install.sh; one file list for both.
#
#   install-native.sh SRC NATIVE_TARBALL VENDOR_ZIP VENDOR_CHANNEL DESTDIR [TAG]
#
#   SRC             source tree of the same release (the tag's checkout or source tarball)
#   NATIVE_TARBALL  waydroid-nvidia-native-x86_64-<tag>.tar.zst (packaging/native/collect.py layout)
#   VENDOR_ZIP      waydroid-nvidia-vendor-x86_64-<tag>.zip: the nvidia-prolapse vendor image as a Waydroid OTA zip
#   VENDOR_CHANNEL  waydroid-nvidia-vendor-ota-<tag>.json: its OTA channel (packaging/vendor-ota.py)
#   DESTDIR         package root ("/" for a direct install)
#
# Result (all root-owned, nothing writable by users; packaging/native/wd-native trusts this tree):
#   /usr/lib/waydroid-nvidia/native/PACKAGED                 marks a packaged install (dev/source-native)
#   /usr/lib/waydroid-nvidia/native/{dev,waydroid,tools,nvidia-prolapse/shim,packaging/native}/...
#   /usr/lib/waydroid-nvidia/native/release/                 the bundle: BUILD-RECORD.json, payload/, gate/, glibc-shim/,
#                                                            driver/ (the driver files the gates run)
#   /usr/lib/waydroid-nvidia/vendor/                         the vendor image zip + channel JSON (waydroid-nvidia-setup
#                                                            installs the image: packaging/native/vendor_image.py)
#   /usr/lib/waydroid-nvidia/wd-native                       root helper (packaging/native/sudoers.example)
# The bundle is checked against its own BUILD-RECORD.json here and again by every gate run.
set -euo pipefail
die() { echo "install-native: $*" >&2; exit 1; }
[ $# -ge 5 ] && [ $# -le 6 ] || die "usage: install-native.sh SRC NATIVE_TARBALL VENDOR_ZIP VENDOR_CHANNEL DESTDIR [TAG]"
SRC=$(cd "$1" && pwd) TARBALL=$2 VZIP=$3 VCHANNEL=$4 DESTDIR=$5 TAG=${6:-}
[ -f "$SRC/dev/source-native" ] && [ -f "$SRC/packaging/native/release_gate.py" ] || die "$SRC is not a waydroid-nvidia source tree"
[ -f "$TARBALL" ] || die "$TARBALL missing"
[ -f "$VZIP" ] && [ -f "$VCHANNEL" ] || die "vendor image assets missing ($VZIP, $VCHANNEL)"
LIB=$DESTDIR/usr/lib/waydroid-nvidia
NAT=$LIB/native

files=(
    dev/source-native
    dev/driver-mode
    waydroid/nvidia_native.py
    tools/driver_source.py
    packaging/native/release_gate.py
    packaging/native/vendor_image.py
    packaging/native/glibc_shim.py
    nvidia-prolapse/shim/tools/import-census.py
    nvidia-prolapse/shim/dispositions.tsv
    nvidia-prolapse/shim/dispositions-x86.tsv
)
for f in "${files[@]}"; do
    [ -f "$SRC/$f" ] || die "$SRC/$f missing"
    mode=0644; [ -x "$SRC/$f" ] && mode=0755
    install -Dm"$mode" "$SRC/$f" "$NAT/$f"
done
install -Dm0755 "$SRC/packaging/native/wd-native" "$LIB/wd-native"

rm -rf "$NAT/release"
install -d -m0755 "$NAT/release"
tar --zstd -C "$NAT/release" --no-same-owner -xf "$TARBALL"
python3 -I - "$NAT/release" <<'EOF'
import hashlib, json, pathlib, sys
bundle = pathlib.Path(sys.argv[1])
record = json.loads((bundle / 'BUILD-RECORD.json').read_text())
if record.get('format') != 2:
    sys.exit(f'install-native: {bundle}/BUILD-RECORD.json is format {record.get("format")}, expected 2')
have = {str(p.relative_to(bundle)) for top in ('payload', 'gate') for p in (bundle / top).rglob('*') if p.is_file()}
if have != set(record['files']):
    sys.exit(f'install-native: bundle files differ from BUILD-RECORD.json: {sorted(have ^ set(record["files"]))[:5]}')
bad = [r for r, d in record['files'].items() if hashlib.sha256((bundle / r).read_bytes()).hexdigest() != d]
if bad:
    sys.exit(f'install-native: {len(bad)} bundle files fail their sha256 (first {bad[0]})')
# glibc-shim mode's tree (packaging/native/glibc_shim.py), when the release ships one
shim = record.get('glibc_shim_files') or {}
have_shim = {str(p.relative_to(bundle)) for p in (bundle / 'glibc-shim').rglob('*') if p.is_file()}
if have_shim != set(shim):
    sys.exit(f'install-native: glibc-shim files differ from BUILD-RECORD.json: {sorted(have_shim ^ set(shim))[:5]}')
bad = [r for r, d in shim.items() if hashlib.sha256((bundle / r).read_bytes()).hexdigest() != d]
if bad:
    sys.exit(f'install-native: {len(bad)} glibc-shim files fail their sha256 (first {bad[0]})')
# the driver files the gates run (packaging/native/collect.py DRIVER_FILES)
drv = record.get('driver_files') or {}
have_drv = {str(p.relative_to(bundle)) for p in (bundle / 'driver').rglob('*') if p.is_file()}
if not drv or have_drv != set(drv):
    sys.exit(f'install-native: driver files differ from BUILD-RECORD.json: {sorted(have_drv ^ set(drv))[:5]}')
bad = [r for r, d in drv.items() if hashlib.sha256((bundle / r).read_bytes()).hexdigest() != d]
if bad:
    sys.exit(f'install-native: {len(bad)} driver files fail their sha256 (first {bad[0]})')
print(f'install-native: release bundle verified, {len(have)} files + {len(shim)} glibc-shim files + {len(drv)} '
      f'driver files, commit {record["commit"][:12]}, driver {record["driver"]["commit"][:12]}')
EOF
find "$NAT/release" -type d -exec chmod 0755 {} +
find "$NAT/release" -type f -path '*/gate/bin/*' -exec chmod 0755 {} +
find "$NAT/release" -type f ! -path '*/gate/bin/*' -exec chmod 0644 {} +
chmod 0755 "$NAT/release/payload/odm/bin/hw/"* 2>/dev/null || true
# the vendor image: the zip must be the one its channel names (waydroid's own check: sha256 = id)
python3 -I - "$VZIP" "$VCHANNEL" <<'EOF'
import hashlib, json, os, sys
z, c = sys.argv[1:3]
(entry,) = json.load(open(c))['response']
digest = hashlib.sha256()
with open(z, 'rb') as f:
    for block in iter(lambda: f.read(1 << 20), b''):
        digest.update(block)
if os.path.basename(z) != entry['filename'] or digest.hexdigest() != entry['id']:
    sys.exit(f'install-native: {z} is not the image {c} names ({entry["filename"]}, sha256 {entry["id"]})')
print(f'install-native: vendor image {entry["filename"]} matches its channel (datetime {entry["datetime"]})')
EOF
rm -rf "$LIB/vendor"
install -Dm0644 "$VZIP" "$LIB/vendor/$(basename "$VZIP")"
install -Dm0644 "$VCHANNEL" "$LIB/vendor/$(basename "$VCHANNEL")"
printf 'waydroid-nvidia packaged install%s\n' "${TAG:+ $TAG}" > "$NAT/PACKAGED"
chmod 0644 "$NAT/PACKAGED"
echo "install-native: tools and bundle installed under $NAT"
