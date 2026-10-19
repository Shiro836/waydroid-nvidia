#!/usr/bin/env python3
"""Install, gate and remove the nvidia-prolapse vendor image (the nvidia-prolapse path of waydroid-nvidia-setup).

  vendor_image.py install --zip ZIP --channel JSON [--native-gl on|off]     (root, container stopped)
  vendor_image.py restore-official                                         (root, container stopped)
  vendor_image.py status

The release ships our Waydroid VENDOR image (vendor-image/, built from source with every guest component as a vendor
module) as a Waydroid OTA zip plus its OTA channel JSON (packaging/vendor-ota.py). `install`:

  1. checks the zip against the channel JSON (sha256 = the entry's id, as `waydroid upgrade` does) and the image's
     identity props against the installed release bundle (ro.vendor.nvidia_prolapse.build_record = sha256 of
     its BUILD-RECORD.json): the package and the image must come from one release;
  2. checks that every bundle binary the image carries (Vulkan HAL, NVK, Rust RM, libdrm, nvrm, gralloc wrapper,
     Codec2 service) is byte-identical to the bundle's, then runs the release gate on those bytes on this GPU and
     loaded kernel module (packaging/native/release_gate.py; Codec2: nvc2-xmlgen writes this GPU's codec list);
  3. keeps the official vendor.img (the first time) in VENDOR_IMAGE_STATE/official/ with its vendor_ota and
     vendor_datetime, puts ours in its place, and points waydroid.cfg vendor_ota at the release's channel JSON
     with vendor_datetime = the image's build date, so `waydroid upgrade` and the in-guest updater see no newer
     vendor image (system OTAs stay official);
  4. writes the install record + codec list the LXC generator checks at every start (waydroid/nvidia_native.py
     check_vendor_image), selects nvidia_driver_mode = nvidia-prolapse with nvidia_prolapse_install =
     vendor-image, runs `waydroid upgrade -o`, `dev/driver-mode verify-active`, and the container unit's
     ExecStartPre drop-in. Any failure puts the previous image, waydroid.cfg, record and drop-in back.

A GPU without an NVDEC class the Codec2 decoder drives (before Turing) installs without hardware decode: the gate's
nvc2-xmlgen exits 3, the codec list has no codecs, and the image's service .rc (made oneshot) and a VINTF fragment
without the Codec2 HAL are written next to it for the generator to bind (nvidia_native.VENDOR_IMAGE_NO_NVDEC), so
Android uses its software decoders. The setup says so in one line.

A re-run with the same image on the same kernel module skips the gate. After an NVIDIA driver update the
generator refuses to start until `install` ran again (re-gate, new codec list).

`restore-official` puts the official vendor image and its vendor_ota/vendor_datetime back and removes the record;
waydroid.cfg's mode keys are left to the caller (waydroid-nvidia-setup --mode venus / --restore-official-vendor).
"""
import argparse
import configparser
import datetime
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import posixpath
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

REPO = Path(__file__).resolve().parents[2]
CFG = Path('/var/lib/waydroid/waydroid.cfg')
WORK = Path('/var/lib/waydroid')
# the bundle binaries the image carries as prebuilt vendor modules (vendor-image/product/vendor/nvidia-prolapse/
# Android.mk): bundle payload path -> path inside vendor.img (= /vendor)
IMAGE_PREBUILTS = {}
for _sub in ('lib64', 'lib'):
    for _name in ('hw/vulkan.nvidia.so', 'libvulkan_nouveau.so', 'libnv_bionic_native.so', 'libdrm.so.2',
                  'libgallium_dri_nvrm.so', 'libgbm_mesa_wrapper.so'):
        IMAGE_PREBUILTS[f'payload/vendor_extra/{_sub}/{_name}'] = f'/{_sub}/{_name}'
    for _lib in ('EGL', 'GLESv1_CM', 'GLESv2'):
        IMAGE_PREBUILTS[f'payload/vendor/{_sub}/egl/lib{_lib}_nvrm.so'] = f'/{_sub}/egl/lib{_lib}_nvrm.so'
IMAGE_PREBUILTS['payload/odm/bin/hw/android.hardware.media.c2@1.2-service-nvprolapse'] = \
    '/odm/bin/hw/android.hardware.media.c2@1.2-service-nvprolapse'


def _load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


native = _load(REPO / 'waydroid/nvidia_native.py', 'nvidia_native')


def sn():
    return _load(REPO / 'dev/source-native', 'source_native')


def driver_mode_tool():
    return _load(REPO / 'dev/driver-mode', 'driver_mode')


def release_gate():
    return _load(REPO / 'packaging/native/release_gate.py', 'release_gate')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def now():
    return datetime.datetime.now().astimezone().isoformat(timespec='seconds')


def say(text):
    print(f'   {text}', flush=True)


def state_dir():
    return Path(native.VENDOR_IMAGE_STATE)


def read_cfg():
    cp = configparser.ConfigParser()
    cp.optionxform = str
    if not cp.read(CFG):
        raise RuntimeError(f'cannot read {CFG}: run `waydroid init` first')
    for section in ('waydroid', 'properties'):
        if not cp.has_section(section):
            cp.add_section(section)
    return cp


def render(cp):
    out = io.StringIO()
    cp.write(out)
    return out.getvalue()


def channel_entry(channel):
    """The single image entry of our OTA channel JSON (packaging/vendor-ota.py) and the channel's own URL."""
    try:
        doc = json.loads(Path(channel).read_text())
        (entry,) = doc['response']
        for key in ('datetime', 'filename', 'id', 'url', 'size'):
            entry[key]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError(f'{channel} is not a one-image Waydroid OTA channel ({error}); reinstall the package')
    if not entry['url'].endswith('/' + entry['filename']):
        raise RuntimeError(f'{channel}: url {entry["url"]} does not name {entry["filename"]}')
    return entry, posixpath.dirname(entry['url']) + '/' + Path(channel).name


def image_file(image, path, dest):
    """Dump one file out of an ext4 image (debugfs, read-only)."""
    dest.unlink(missing_ok=True)
    subprocess.run(['debugfs', '-R', f'dump {path} {dest}', str(image)], capture_output=True, check=True)
    if not dest.is_file():
        raise RuntimeError(f'{image} has no {path}')
    return dest


def check_prebuilts(image, record, work):
    """Every bundle binary the image carries is the bundle's, byte for byte (so the gate below tests the image)."""
    for rel, inside in sorted(IMAGE_PREBUILTS.items()):
        want = record['files'].get(rel)
        if want is None:
            raise RuntimeError(f'the release bundle has no {rel}; reinstall the package')
        got = sha256(image_file(image, inside, work / 'prebuilt'))
        if got != want:
            raise RuntimeError(f'{image} {inside} is {got[:16]}, the release bundle {rel} is {want[:16]}: the vendor image '
                               'and the package come from different builds; reinstall the package')
    (work / 'prebuilt').unlink(missing_ok=True)
    say(f'{len(IMAGE_PREBUILTS)} bundle binaries in the image are byte-identical to the release bundle')


def state_files():
    """Every file `install` writes into the state directory (backed up, restored and removed together)."""
    return (native.VENDOR_IMAGE_RECORD, native.VENDOR_IMAGE_XML, *native.VENDOR_IMAGE_NO_NVDEC.values())


def no_nvdec_files(image, work):
    """GPU without hardware decode: the image's Codec2 service .rc made oneshot and a VINTF fragment without the
    Codec2 HAL (nvidia_native.VENDOR_IMAGE_NO_NVDEC), written into `work`. Returns {image path: file}."""
    rc = image_file(image, '/' + native.MEDIA_RC, work / 'image-service.rc')
    try:
        text = native.no_nvdec_rc(rc.read_text())
    except OSError as error:
        raise RuntimeError(f'{image}: {error}; the vendor image and the package come from different builds; '
                           'reinstall the package')
    finally:
        rc.unlink()
    out = {native.MEDIA_RC: work / native.VENDOR_IMAGE_NO_NVDEC[native.MEDIA_RC],
           native.MEDIA_VINTF: work / native.VENDOR_IMAGE_NO_NVDEC[native.MEDIA_VINTF]}
    out[native.MEDIA_RC].write_text(text)
    out[native.MEDIA_VINTF].write_text(native.NO_NVDEC_VINTF_TEXT)
    return out


def container_stopped():
    if sn().container_state() != 'STOPPED':
        raise RuntimeError('stop the Waydroid session and container first (waydroid session stop; '
                           'sudo systemctl stop waydroid-container)')


def load_record():
    try:
        return native.load_vendor_image_record(str(state_dir()))
    except OSError:
        return None


def write_json(path, data):
    sn().write_atomic(path, json.dumps(data, indent=2) + '\n')


def install(args):
    if os.geteuid() != 0:
        raise RuntimeError('install needs root (sudo waydroid-nvidia-setup)')
    container_stopped()
    gate_mod, srcn, dmt = release_gate(), sn(), driver_mode_tool()
    bundle = gate_mod.BUNDLE
    record = gate_mod.verify_bundle(bundle)
    bundle_sha = sha256(bundle / gate_mod.RECORD)
    entry, channel_url = channel_entry(args.channel)
    zpath = Path(args.zip)
    if zpath.name != entry['filename']:
        raise RuntimeError(f'{zpath} is not {entry["filename"]} (the image the channel names)')
    say(f'checking {zpath.name} against the channel ({channel_url})')
    if sha256(zpath) != entry['id']:
        raise RuntimeError(f'{zpath} sha256 differs from the channel id {entry["id"][:16]}: reinstall the package')
    cp = read_cfg()
    w = cp['waydroid']
    images = Path(w.get('images_path') or native.IMAGES_PATH)
    live = images / 'vendor.img'
    system_img = images / 'system.img'
    if not system_img.is_file():
        raise RuntimeError(f'{system_img} missing: run `waydroid init` first')
    state = state_dir()
    state.mkdir(parents=True, exist_ok=True)
    os.chmod(state, 0o755)
    new = images / '.vendor.img.nvidia-prolapse-new'
    new.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(zpath) as z:
            if z.namelist() != ['vendor.img']:
                raise RuntimeError(f'{zpath} holds {z.namelist()}, expected exactly vendor.img')
            say(f'extracting vendor.img into {images}')
            with z.open('vendor.img') as src, open(new, 'wb') as dst:
                shutil.copyfileobj(src, dst, 16 << 20)
        os.chmod(new, 0o644)
        props = native.image_build_props(str(new))
        if props.get(native.BUILD_RECORD_PROP) != bundle_sha:
            raise RuntimeError(f'{zpath.name} carries release bundle {props.get(native.BUILD_RECORD_PROP)}, the package '
                               f'{bundle_sha} ({record.get("release")}): reinstall the package')
        if str(props.get('ro.vendor.build.date.utc')) != str(entry['datetime']):
            raise RuntimeError(f'{zpath.name} build date {props.get("ro.vendor.build.date.utc")} differs from the '
                               f'channel datetime {entry["datetime"]}')
        image_sha = sha256(new)
        version = props[native.VERSION_PROP]
        say(f'vendor image {version} ({props["ro.vendor.build.fingerprint"]})')
        kernel = native.driver_version()
        old = load_record()
        gl = args.native_gl or (w.get(native.GL_KEY) if w.get(native.GL_KEY) in ('on', 'off') else 'off')
        if old and old['vendor_img']['sha256'] == image_sha and old['kernel_module'] == kernel and \
                old['build_record_sha256'] == bundle_sha and (state / native.VENDOR_IMAGE_XML).is_file() and \
                sha256(state / native.VENDOR_IMAGE_XML) == old['media']['xml_sha256'] and \
                all((state / n).is_file() and sha256(state / n) == old['media']['no_nvdec'][t]
                    for t, n in native.VENDOR_IMAGE_NO_NVDEC.items() if not old['media']['hardware_decode']):
            say(f'already gated on kernel module {kernel} ({old["gate"]["log_dir"]}); not gating again')
            gate, media, native_gl_ok, xml = old['gate'], old['media'], old['native_gl'], state / native.VENDOR_IMAGE_XML
            no_nvdec = {} if media['hardware_decode'] else {t: state / n for t, n in native.VENDOR_IMAGE_NO_NVDEC.items()}
        else:
            gate_mod.LOG_ROOT.mkdir(parents=True, exist_ok=True)
            work = Path(tempfile.mkdtemp(prefix='vendor-image-check-', dir=gate_mod.LOG_ROOT))
            check_prebuilts(new, record, work)
            shutil.rmtree(work)
            print('== gating the vendor image on this GPU and kernel module (a few minutes; logs in '
                  f'{gate_mod.LOG_ROOT})', flush=True)
            _, vk, media_rec, gl_rec, _ = gate_mod.run(None, native, bundle, images=images, vendor_image=new)
            gate = {'exit': 0, 'kernel_module': vk['kernel_module'], 'log_dir': vk['log_dir'],
                    'finished': gl_rec['gate']['finished'], 'runner': vk['runner'],
                    'cases': len(vk['cases']) + len(gl_rec['gate']['cases'])}
            media = {'xml_sha256': media_rec['xml_sha256'], 'codecs': media_rec['codecs'], 'gate': media_rec['gate'],
                     'hardware_decode': media_rec['hardware_decode']}
            native_gl_ok = gl_rec['enabled']
            xml = Path(media_rec['xml'])
            no_nvdec = {} if media['hardware_decode'] else no_nvdec_files(new, xml.parent)
            if no_nvdec:
                media.update(reason=media_rec['reason'], no_nvdec={t: sha256(f) for t, f in no_nvdec.items()})
            if gate['kernel_module'] != kernel:
                raise RuntimeError(f'kernel module changed during the gate ({kernel} -> {gate["kernel_module"]})')
    except BaseException:
        new.unlink(missing_ok=True)
        raise
    if gl == 'on' and not native_gl_ok:
        new.unlink(missing_ok=True)
        raise RuntimeError('nvidia_native_gl = on, but nvrm did not pass the gate; use --native-gl off')

    # ---- switch (container stopped): everything below is undone on failure
    backup = WORK / ('before-vendor-image-' + time.strftime('%Y%m%d-%H%M%S'))
    backup.mkdir()
    shutil.copy2(CFG, backup / 'waydroid.cfg')
    for name in state_files():
        if (state / name).exists():
            shutil.copy2(state / name, backup / name)
    if srcn.OVERRIDE.exists():
        shutil.copy2(srcn.OVERRIDE, backup / 'source-native.conf')
    official = state / 'official'
    live_props = native.image_build_props(str(live)) if live.is_file() else {}
    previous = images / '.vendor.img.previous'
    previous.unlink(missing_ok=True)
    moved_official = False
    if live.is_file() and native.VERSION_PROP not in live_props:
        if (official / 'vendor.img').exists():
            say(f'replacing the earlier official backup in {official} with the current official image')
        official.mkdir(parents=True, exist_ok=True)
        os.chmod(official, 0o700)
        info = {'vendor_ota': w.get('vendor_ota'), 'vendor_datetime': w.get('vendor_datetime'),
                'sha256': sha256(live), 'fingerprint': live_props.get('ro.vendor.build.fingerprint'),
                'saved': now(), 'from': str(live)}
        os.replace(live, official / 'vendor.img.tmp') if os.stat(live).st_dev == os.stat(official).st_dev \
            else shutil.copy2(live, official / 'vendor.img.tmp')
        os.replace(official / 'vendor.img.tmp', official / 'vendor.img')
        write_json(official / 'official.json', info)
        moved_official = True
        say(f'official vendor image kept in {official} (restore: sudo waydroid-nvidia-setup --restore-official-vendor)')
    elif live.is_file():
        os.replace(live, previous)
    official_info = json.loads((official / 'official.json').read_text()) if (official / 'official.json').exists() else None
    old_text = CFG.read_text()
    try:
        os.replace(new, live)
        if Path(xml) != state / native.VENDOR_IMAGE_XML:          # a re-run without a new gate keeps the gated one
            shutil.copy2(xml, state / native.VENDOR_IMAGE_XML)
        os.chmod(state / native.VENDOR_IMAGE_XML, 0o644)
        for target, name in native.VENDOR_IMAGE_NO_NVDEC.items():
            if target not in no_nvdec:
                (state / name).unlink(missing_ok=True)
            elif Path(no_nvdec[target]) != state / name:
                shutil.copy2(no_nvdec[target], state / name)
            if target in no_nvdec:
                os.chmod(state / name, 0o644)
        rec = {'format': native.VENDOR_IMAGE_RECORD_FORMAT, 'version': version, 'build_record_sha256': bundle_sha,
               'release': record.get('release'), 'commit': record['commit'],
               'vendor_img': {'path': str(live), 'sha256': image_sha, 'zip': zpath.name,
                              'build_date_utc': props['ro.vendor.build.date.utc'],
                              'fingerprint': props['ro.vendor.build.fingerprint']},
               'channel': {'vendor_ota': channel_url, 'vendor_datetime': str(entry['datetime'])},
               'kernel_module': kernel, 'gate': gate, 'native_gl': native_gl_ok,
               'media': {**media, 'xml': str(state / native.VENDOR_IMAGE_XML)},
               'official_backup': official_info and {**official_info, 'path': str(official / 'vendor.img')},
               'installed': now()}
        write_json(state / native.VENDOR_IMAGE_RECORD, rec)
        # waydroid.cfg: our channel (so no OTA replaces the image), the mode, the shape, explicit GL
        cfg = dmt.switched(read_cfg(), 'nvidia-prolapse', gl, say=say, install='vendor-image')
        cw = cfg['waydroid']
        if cw.get('vendor_ota') != channel_url:
            say(f'vendor_ota = {channel_url} (was {cw.get("vendor_ota")})')
        cw['vendor_ota'] = channel_url
        cw['vendor_datetime'] = str(entry['datetime'])
        if cw.get(native.LAYOUT_KEY):
            say(f'dropping {native.LAYOUT_KEY} (the vendor image binds nothing from /var/lib/waydroid/nv/guest)')
            del cw[native.LAYOUT_KEY]
        new_text = render(cfg)
        if dmt.identity(old_text) != dmt.identity(new_text) or not old or old['vendor_img']['sha256'] != image_sha:
            srcn.invalidate_caches(backup)          # another driver: fresh HWUI/EGL shader caches
        srcn.write_atomic(CFG, new_text)
        subprocess.run([*srcn.waydroid_argv(), 'upgrade', '-o'], check=True)
        srcn.OVERRIDE.parent.mkdir(parents=True, exist_ok=True)
        srcn.write_atomic(srcn.OVERRIDE, srcn.render_override(None))
        subprocess.run(['systemctl', 'daemon-reload'], check=True)
        dmt.verify_active()
    except BaseException:
        print('vendor_image: install failed; putting the previous configuration back', file=sys.stderr, flush=True)
        if live.exists():
            live.unlink()
        if previous.exists():
            os.replace(previous, live)
        elif moved_official:
            shutil.copy2(official / 'vendor.img', live)
        shutil.copy2(backup / 'waydroid.cfg', CFG)
        for name in state_files():
            if (backup / name).exists():
                shutil.copy2(backup / name, state / name)
            else:
                (state / name).unlink(missing_ok=True)
        if (backup / 'source-native.conf').exists():
            shutil.copy2(backup / 'source-native.conf', srcn.OVERRIDE)
        else:
            srcn.OVERRIDE.unlink(missing_ok=True)
        subprocess.run(['systemctl', 'daemon-reload'])
        subprocess.run([*srcn.waydroid_argv(), 'upgrade', '-o'])
        raise
    previous.unlink(missing_ok=True)
    print(f'Installed the nvidia-prolapse vendor image {version} (gated on kernel module {kernel}; previous '
          f'configuration in {backup}).', flush=True)
    if not media['hardware_decode']:
        print(media['reason'], flush=True)


def restore_official(args):
    if os.geteuid() != 0:
        raise RuntimeError('restore-official needs root (sudo waydroid-nvidia-setup --restore-official-vendor)')
    container_stopped()
    state = state_dir()
    official = state / 'official'
    cp = read_cfg()
    w = cp['waydroid']
    images = Path(w.get('images_path') or native.IMAGES_PATH)
    live = images / 'vendor.img'
    live_props = native.image_build_props(str(live)) if live.is_file() else {}
    if live.is_file() and native.VERSION_PROP not in live_props:
        say(f'{live} is already an official vendor image ({native.describe_image(live_props)})')
    else:
        if not (official / 'vendor.img').is_file() or not (official / 'official.json').is_file():
            raise RuntimeError(f'no official vendor image backup in {official}. Remedy: sudo waydroid init -f '
                               '(downloads the official images again; apps and data are kept)')
        info = json.loads((official / 'official.json').read_text())
        if sha256(official / 'vendor.img') != info['sha256']:
            raise RuntimeError(f'{official}/vendor.img differs from its record; Remedy: sudo waydroid init -f')
        os.replace(official / 'vendor.img', live) if os.stat(official).st_dev == os.stat(images).st_dev \
            else shutil.copy2(official / 'vendor.img', live)
        w['vendor_ota'] = info['vendor_ota'] or w.get('vendor_ota', '')
        w['vendor_datetime'] = info['vendor_datetime'] or '0'
        sn().write_atomic(CFG, render(cp))
        (official / 'official.json').unlink()
        (official / 'vendor.img').unlink(missing_ok=True)
        say(f'official vendor image back in place ({info["fingerprint"]}); vendor_ota = {w["vendor_ota"]}')
    for name in state_files():
        (state / name).unlink(missing_ok=True)


def status(args):
    cp = read_cfg()
    w = dict(cp['waydroid'])
    image = native.vendor_img_path(w)
    try:
        props = native.image_build_props(image)
        print(f'vendor.img: {image}: {native.describe_image(props)}')
    except OSError as error:
        print(f'vendor.img: {error}')
    record = load_record()
    print(f'install record: {state_dir() / native.VENDOR_IMAGE_RECORD}: ' +
          (f'{record["version"]}, gated on {record["kernel_module"]} ({record["gate"]["log_dir"]})' if record else 'none'))
    if record:
        print('hardware video decode: ' + ', '.join(record['media']['codecs']) if record['media']['hardware_decode']
              else record['media']['reason'])
    print(f'vendor_ota = {w.get("vendor_ota")}, vendor_datetime = {w.get("vendor_datetime")}')
    try:
        dm = native.driver_mode(w)
        if dm is not None and dm.vendor_image:
            native.check_vendor_image(dm, w)
            print('PASS    the configured vendor-image install is consistent')
    except OSError as error:
        print(f'FAIL    {error}')
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('install')
    p.add_argument('--zip', required=True)
    p.add_argument('--channel', required=True)
    p.add_argument('--native-gl', choices=['on', 'off'])
    sub.add_parser('restore-official')
    sub.add_parser('status')
    args = parser.parse_args()
    return {'install': install, 'restore-official': restore_official, 'status': status}[args.command](args) or 0


if __name__ == '__main__':
    # tools whose output is parsed (lxc-info, debugfs, nm, modinfo, systemctl) print C-locale text, here and in
    # every child process (waydroid upgrade, the gate)
    os.environ['LC_ALL'] = 'C'
    os.environ.pop('LANGUAGE', None)
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(f'vendor_image: {error}')
