#!/usr/bin/env python3
"""Unit tests of the release bundle check (no GPU, no root): python3 -m pytest -q packaging/native/test_release_gate.py"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_gate as gate  # noqa: E402


def bundle(tmp_path, files=None):
    files = files or {'payload/vendor_extra/lib64/libx.so': b'x', 'gate/bin/android/vk-compute': b'y'}
    for rel, data in files.items():
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_bytes(data)
    record = {'format': 2, 'commit': '0' * 40,
              'files': {rel: hashlib.sha256(data).hexdigest() for rel, data in files.items()}}
    (tmp_path / gate.RECORD).write_text(json.dumps(record))
    return tmp_path


def test_intact_bundle_passes(tmp_path):
    assert gate.verify_bundle(bundle(tmp_path))['commit'] == '0' * 40


def test_modified_file_refused(tmp_path):
    b = bundle(tmp_path)
    (b / 'gate/bin/android/vk-compute').write_bytes(b'z')
    with pytest.raises(RuntimeError, match='modified: gate/bin/android/vk-compute'):
        gate.verify_bundle(b)


def test_extra_and_missing_files_refused(tmp_path):
    b = bundle(tmp_path)
    (b / 'payload/extra.so').write_bytes(b'e')
    with pytest.raises(RuntimeError, match='not in the build record: payload/extra.so'):
        gate.verify_bundle(b)
    (b / 'payload/extra.so').unlink()
    (b / 'payload/vendor_extra/lib64/libx.so').unlink()
    with pytest.raises(RuntimeError, match='missing payload/vendor_extra/lib64/libx.so'):
        gate.verify_bundle(b)


def test_wrong_format_refused(tmp_path):
    b = bundle(tmp_path)
    record = json.loads((b / gate.RECORD).read_text())
    record['format'] = 1
    (b / gate.RECORD).write_text(json.dumps(record))
    with pytest.raises(RuntimeError, match='format 1'):
        gate.verify_bundle(b)


def test_elf_bits(tmp_path):
    (tmp_path / 'a').write_bytes(b'\x7fELF\x02rest')
    (tmp_path / 'b').write_bytes(b'\x7fELF\x01rest')
    (tmp_path / 'c').write_bytes(b'#!/bin/sh')
    assert [gate.elf_bits(tmp_path / n) for n in 'abc'] == [64, 32, None]


# ---------------------------------------------------------------- DT_NEEDED from the ELF file

def make_elf(bits=64, endian='<', needed=('libc.so', 'libdl.so'), runpath=None, dynamic=True, strtab_mapped=True,
             dt_null=True):
    """A minimal ELF object: header, PT_LOAD over the whole file, PT_DYNAMIC, .dynstr. Virtual addresses are
    file offsets + 0x1000, so DT_STRTAB must be translated through PT_LOAD."""
    import struct
    is64 = bits == 64
    ehsize, phentsize = (64, 56) if is64 else (52, 32)
    phnum = 2 if dynamic else 1
    strtab = b'\0'
    offsets = []
    for name in list(needed) + ([runpath] if runpath else []):
        offsets.append(len(strtab))
        strtab += name.encode() + b'\0'
    str_off = ehsize + phnum * phentsize
    dyn_off = str_off + len(strtab)
    dyn_off += -dyn_off % 8
    vbase = 0x1000
    tags = [(1, o) for o in offsets[:len(needed)]]
    if runpath:
        tags.append((29, offsets[-1]))
    tags += [(5, (str_off + vbase) if strtab_mapped else 0x900000), (10, len(strtab))]
    if dt_null:
        tags.append((0, 0))
    dfmt = end = endian
    dfmt += 'qQ' if is64 else 'iI'
    dyn = b''.join(struct.pack(dfmt, t, v) for t, v in tags)
    total = dyn_off + len(dyn)
    if is64:
        head = struct.pack(end + '4sBBBBB7sHHIQQQIHHHHHH', b'\x7fELF', 2, 1 if end == '<' else 2, 1, 0, 0, b'',
                           3, 62, 1, 0, ehsize, 0, 0, ehsize, phentsize, phnum, 64, 0, 0)
        load = struct.pack(end + 'IIQQQQQQ', 1, 5, 0, vbase, vbase, total, total, 0x1000)
        dynp = struct.pack(end + 'IIQQQQQQ', 2, 6, dyn_off, dyn_off + vbase, dyn_off + vbase, len(dyn), len(dyn), 8)
    else:
        head = struct.pack(end + '4sBBBBB7sHHIIIIIHHHHHH', b'\x7fELF', 1, 1 if end == '<' else 2, 1, 0, 0, b'',
                           3, 3, 1, 0, ehsize, 0, 0, ehsize, phentsize, phnum, 40, 0, 0)
        load = struct.pack(end + 'IIIIIIII', 1, 0, vbase, vbase, total, total, 5, 0x1000)
        dynp = struct.pack(end + 'IIIIIIII', 2, dyn_off, dyn_off + vbase, dyn_off + vbase, len(dyn), len(dyn), 6, 4)
    out = head + load + (dynp if dynamic else b'') + strtab
    return out + b'\0' * (dyn_off - len(out)) + dyn


@pytest.mark.parametrize('bits', [32, 64])
@pytest.mark.parametrize('endian', ['<', '>'])
def test_needed_from_the_file(tmp_path, bits, endian):
    f = tmp_path / 'lib.so'
    f.write_bytes(make_elf(bits, endian, runpath='/a:/b'))
    assert gate.needed(f) == ['libc.so', 'libdl.so']
    assert gate.elf_dynamic(f)['runpath'] == ['/a', '/b']


def test_needed_does_not_read_tool_output(tmp_path, monkeypatch):
    """Issue #28: binutils localises readelf ("Biblioteca compartida: [libdl.so]"); the closure came back empty and
    the gate died with 'library "libdl.so" not found'. A readelf on PATH that prints Spanish changes nothing now."""
    f = tmp_path / 'vk-compute'
    f.write_bytes(make_elf())
    fake = tmp_path / 'bin'
    fake.mkdir()
    (fake / 'readelf').write_text('#!/bin/sh\necho " 0x1 (NEEDED) Biblioteca compartida: [libdl.so]"\n')
    (fake / 'readelf').chmod(0o755)
    monkeypatch.setenv('PATH', f'{fake}:{os.environ["PATH"]}')
    monkeypatch.setenv('LC_ALL', 'es_ES.UTF-8')
    assert gate.needed(f) == ['libc.so', 'libdl.so']


def foreign_locales():
    try:
        out = subprocess.run(['locale', '-a'], capture_output=True, text=True).stdout.split()
    except OSError:
        return []
    return [l for l in out if l.split('.')[0] not in ('C', 'POSIX') and not l.startswith('en_')]


@pytest.mark.skipif(not shutil.which('readelf'), reason='binutils not installed')
@pytest.mark.parametrize('locale', foreign_locales()[:1] or [None])
def test_needed_matches_readelf_on_system_objects(locale):
    """Same names as `LC_ALL=C readelf -dW` for real objects; also under an installed non-English locale."""
    objs = [p for p in (shutil.which('ls'), shutil.which('python3'), '/usr/lib/libc.so.6', '/usr/lib32/libc.so.6',
                        '/lib/x86_64-linux-gnu/libc.so.6') if p and Path(p).is_file()]
    assert objs
    code = ('import json, sys; sys.path.insert(0, sys.argv[1]); import release_gate as g; '
            'print(json.dumps([g.elf_dynamic(p)["needed"] for p in sys.argv[2:]]))')
    env = dict(os.environ, LC_ALL=locale) if locale else dict(os.environ)
    got = json.loads(subprocess.check_output([sys.executable, '-c', code, str(Path(gate.__file__).parent), *objs],
                                             env=env, text=True))
    for obj, names in zip(objs, got):
        text = subprocess.check_output(['readelf', '-dW', obj], text=True, env=dict(os.environ, LC_ALL='C'))
        assert names == re.findall(r'\(NEEDED\)\s+Shared library: \[(.+?)\]', text), obj


@pytest.mark.parametrize('kwargs,message', [
    ({'dynamic': False}, 'no PT_DYNAMIC'),
    ({'strtab_mapped': False}, 'outside the file'),
    ({'dt_null': False}, 'without DT_NULL'),
    ({'needed': ()}, 'no DT_NEEDED entry'),
])
def test_damaged_or_empty_dynamic_section_is_an_error(tmp_path, kwargs, message):
    f = tmp_path / 'x.so'
    f.write_bytes(make_elf(**kwargs))
    with pytest.raises(gate.ElfError, match=message):
        gate.needed(f)


def test_truncated_and_non_elf_files(tmp_path):
    (tmp_path / 'cut').write_bytes(make_elf()[:70])
    (tmp_path / 'sh').write_bytes(b'#!/bin/sh\n' + b'\0' * 60)
    with pytest.raises(gate.ElfError, match='truncated'):
        gate.needed(tmp_path / 'cut')
    with pytest.raises(gate.ElfError, match='no ELF header'):
        gate.needed(tmp_path / 'sh')


def test_image_leaf_library_may_need_nothing(tmp_path):
    """ld-android.so of the user's image has no DT_NEEDED: extract_runtime accepts it (leaf_ok)."""
    f = tmp_path / 'ld-android.so'
    f.write_bytes(make_elf(needed=()))
    assert gate.needed(f, leaf_ok=True) == []


# ---------------------------------------------------------------- media gate on a GPU without hardware decode

NONE_XML = ('<?xml version="1.0" encoding="utf-8" ?>\n<!-- Generated by nvc2-xmlgen from nv_video_device_open() on '
            'this GPU: no hardware decoders.\n     hardware video decode: not available on Tesla P100. -->\n'
            '<MediaCodecs>\n    <Decoders>\n    </Decoders>\n</MediaCodecs>\n')


def fake_media_gate(tmp_path, monkeypatch, xmlgen_exit, check_exit=None):
    """media_case_image() with a scripted nvc2-xmlgen (exit status and output as the tool's) and fake images."""
    import types
    payload = tmp_path / 'bundle/payload'
    service = payload / 'odm/bin/hw/svc'
    rust = payload / 'vendor_extra/lib64/libnv_bionic_native.so'
    for path, names in ((service, ('libc.so', 'libvendor.so')), (rust, ('libc.so',))):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(make_elf(needed=names))
    xmlgen = tmp_path / 'nvc2-xmlgen'
    check_exit = xmlgen_exit if check_exit is None else check_exit
    xmlgen.write_text(f'''#!/bin/sh
if [ "$1" = -o ]; then printf '%s' '{NONE_XML}' > "$2"; fi
echo "nvc2-xmlgen: hardware video decode: not available on Tesla P100 (no NVDEC class this decoder supports; supported from Turing): Android software decoders are used" >&2
[ "$1" = --check ] && exit {check_exit}
exit {xmlgen_exit}
''')
    xmlgen.chmod(0o755)
    work = tmp_path / 'work'
    (work / 'logs').mkdir(parents=True)
    g = types.SimpleNamespace(payload=payload, work=work, system_img='sys', progs=tmp_path / 'progs',
                              command=lambda target, argv: ({}, [str(xmlgen)]))
    native = types.SimpleNamespace(driver_version=lambda: '580.178.04', XMLGEN_MARK='Generated by nvc2-xmlgen',
                                   MEDIA_SERVICE='odm/bin/hw/svc', DEV_DIR=str(tmp_path / 'dev'),
                                   image_ls=lambda img, d: {'/lib64': ['libvendor.so'], '/system/lib64': ['libc.so']}.get(d, []),
                                   nvidia_render_node=lambda: '/dev/dri/renderD128',
                                   check_media_device_access=lambda paths: None)
    real = subprocess.check_output
    monkeypatch.setattr(gate.subprocess, 'check_output', lambda argv, **kw: (
        'U nv_video_decode\n' if '--undefined-only' in argv else 'T nv_video_decode\n') if argv[0] == 'nm'
        else real(argv, **kw))
    return gate.media_case_image(g, native, 'vendor.img')


def test_media_gate_without_nvdec_class_is_a_capability_gap(tmp_path, monkeypatch):
    media = fake_media_gate(tmp_path, monkeypatch, gate.XMLGEN_NO_NVDEC)
    assert media['hardware_decode'] is False and media['codecs'] == []
    assert media['reason'] == ('hardware video decode: not available on Tesla P100 (no NVDEC class this decoder '
                               'supports; supported from Turing): Android software decoders are used')
    assert [s['step'] for s in media['gate']['steps']] == ['xmlgen', 'validate-xml', 'xmlgen-check']
    assert media['gate']['exit'] == 0 and media['gate']['closure'] == {'svc': {'libc.so': 'llndk /system/lib64',
                                                                              'libvendor.so': 'vendor.img /lib64'}}


def test_media_gate_failures_still_fail(tmp_path, monkeypatch):
    with pytest.raises(RuntimeError, match='media gate FAILED at xmlgen .exit 1.'):
        fake_media_gate(tmp_path / 'a', monkeypatch, 1)               # rc7's tool on Pascal, or any real failure
    with pytest.raises(RuntimeError, match='media gate FAILED at xmlgen-check .exit 1.'):
        fake_media_gate(tmp_path / 'b', monkeypatch, gate.XMLGEN_NO_NVDEC, check_exit=1)
    with pytest.raises(RuntimeError, match='media gate FAILED at validate-xml'):
        fake_media_gate(tmp_path / 'c', monkeypatch, 0)               # exit 0 needs codecs in the listing
