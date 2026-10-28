#!/usr/bin/env python3
"""Artifact check for the bionic libdrm.so.2 (run with `python3 -I`); exits non-zero on any violation.

Reads the real ELF with llvm-readelf and the host's NVIDIA blobs:

  - ELF64 x86-64 (--arch x86: ELF32 i386), ET_DYN; DT_SONAME is exactly libdrm.so.2
  - DT_NEEDED is a subset of the bionic set (--allow-needed); no glibc SONAME
  - every drm* name the driver's blobs reference is a defined dynamic symbol, GLOBAL, DEFAULT
    visibility, FUNC: the names of every supported driver branch (--reference, the lists in
    imports/<version>-<arch>.names cut from each branch's libraries), plus a census computed now
    from the given blobs (--blob: the loaded driver's closure, as the shims' tools/find-driver.py
    finds it), like `strings -a | grep -E '^drm[A-Z][A-Za-z0-9_]+$'`
  - ABI shape against the host's own libdrm (--host-libdrm): the exported drm* sets are
    compared; when the host library is the pinned version, any difference is an error,
    otherwise the difference is printed
"""
import argparse
import os
import re
import subprocess
import sys

DRM_NAME = re.compile(rb"drm[A-Z][A-Za-z0-9_]+")
# printable run as GNU strings -a sees it (ASCII 0x20-0x7e plus TAB), minimum length 4
PRINTABLE = re.compile(rb"[\x20-\x7e\t]{4,}")
GLIBC_SONAMES = re.compile(r"^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux.*)$")


def readelf(tool, args, path):
    return subprocess.run([tool] + args + [path], check=True, capture_output=True, text=True).stdout


def census(blobs):
    """{name: [blob basenames]} for every drm* string in the blobs."""
    seen = {}
    for b in blobs:
        data = open(b, "rb").read()
        for run in PRINTABLE.findall(data):
            if DRM_NAME.fullmatch(run):
                seen.setdefault(run.decode(), []).append(os.path.basename(b))
    return seen


def dyn_exports(tool, so):
    """{name: (type, bind, vis)} of the defined dynamic symbols (version suffix stripped)."""
    out = {}
    for line in readelf(tool, ["--dyn-syms", "-W"], so).splitlines():
        m = re.match(r"^\s*(\d+): ([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)\s*(\S*)$", line)
        if not m or m.group(1) == "0" or m.group(7) == "UND":
            continue
        name = m.group(8).split("@")[0]
        out[name] = (m.group(4), m.group(5), m.group(6))
    return out


def blob_census(blobs, errors):
    """drm* names the given NVIDIA blobs reference (glibc-shim mode loads them)."""
    want = census(blobs)
    if not want:
        errors.append(f"blob census found no drm* names in {blobs}: the census itself is broken")
    per_blob = {}
    for name, where in want.items():
        for w in where:
            per_blob[w] = per_blob.get(w, 0) + 1
    print(f"check-libdrm: host driver: {len(want)} distinct drm* names referenced "
          f"({', '.join(f'{os.path.basename(b)} {per_blob.get(os.path.basename(b), 0)}' for b in blobs)})")
    return want


def reference_names(paths, errors):
    """{name: [list file]} of the per-branch reference lists (one drm* name per line)."""
    want = {}
    for p in paths:
        names = [l.strip() for l in open(p) if l.strip()]
        bad = [n for n in names if not DRM_NAME.fullmatch(n.encode())]
        if bad or not names:
            errors.append(f"{p}: not a list of drm* names ({bad[:3] or 'empty'})")
        for n in names:
            want.setdefault(n, []).append(os.path.basename(p))
    if paths:
        print(f"check-libdrm: {len(want)} distinct drm* names over {len(paths)} driver reference lists")
    return want


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--allow-needed", required=True)
    ap.add_argument("--arch", default="x86_64", choices=("x86_64", "x86"))
    ap.add_argument("--blob", action="append", default=[], help="a driver library to take a census of (repeatable)")
    ap.add_argument("--reference", action="append", default=[],
                    help="a driver branch's drm* name list, imports/<version>-<arch>.names (repeatable)")
    ap.add_argument("--census-out", help="write the names the --blob census found (one per line) to this file")
    ap.add_argument("--host-libdrm", default="/usr/lib/libdrm.so.2")
    ap.add_argument("--pinned-version", required=True, help="e.g. 2.4.134")
    ap.add_argument("--no-blob-census", metavar="REASON",
                    help="skip the NVIDIA-blob census (no NVIDIA userspace on this builder); the reason is printed")
    ap.add_argument("so")
    a = ap.parse_args()
    R, so = a.readelf, a.so
    errors = []

    hdr = readelf(R, ["-h"], so)
    want_class, want_machine = (("ELF64", "Advanced Micro Devices X86-64") if a.arch == "x86_64"
                                else ("ELF32", "Intel 80386"))
    for key, want in (("Class", want_class), ("Type", "DYN"), ("Machine", want_machine)):
        m = re.search(rf"^\s*{key}:\s+(.*)$", hdr, re.M)
        if not m or not m.group(1).startswith(want):
            errors.append(f"ELF {key} is {m.group(1) if m else '?'}, expected {want}")

    dyn = readelf(R, ["-d"], so)
    soname = re.findall(r"\(SONAME\)\s+Library soname: \[(.*)\]", dyn)
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.*)\]", dyn)
    if soname != ["libdrm.so.2"]:
        errors.append(f"DT_SONAME {soname}, expected ['libdrm.so.2']")
    allowed = set(a.allow_needed.split(","))
    for n in needed:
        if GLIBC_SONAMES.match(n):
            errors.append(f"DT_NEEDED {n} is a glibc SONAME")
        elif n not in allowed:
            errors.append(f"DT_NEEDED {n} not in the bionic set {sorted(allowed)}")
    if "TEXTREL" in dyn:
        errors.append("has TEXTREL")
    print(f"check-libdrm: SONAME {soname} NEEDED {needed}")

    want = reference_names(a.reference, errors)
    if a.no_blob_census:
        print(f"check-libdrm: census of the host's NVIDIA libraries skipped: {a.no_blob_census}")
    elif not a.blob:
        errors.append("no --blob given and no --no-blob-census reason")
    else:
        found = blob_census(a.blob, errors)
        if a.census_out:
            with open(a.census_out, "w") as f:
                f.write("".join(n + "\n" for n in sorted(found)))
        for n, where in found.items():
            want.setdefault(n, []).extend(where)
    have = dyn_exports(R, so)
    missing = sorted(n for n in want if n not in have)
    wrong = sorted(f"{n} {have[n]}" for n in want if n in have and have[n] != ("FUNC", "GLOBAL", "DEFAULT"))
    if missing:
        errors.append(f"{len(missing)} drm* names the driver references are not exported: {' '.join(missing)}")
    if wrong:
        errors.append(f"exported but not FUNC GLOBAL DEFAULT: {'; '.join(wrong)}")
    print(f"check-libdrm: {len(want) - len(missing) - len(wrong)}/{len(want)} driver-referenced drm* names "
          f"exported FUNC GLOBAL DEFAULT")

    host_real = os.path.realpath(a.host_libdrm)
    if not os.path.isfile(host_real):
        errors.append(f"host libdrm {a.host_libdrm} not found (needed for the ABI-shape comparison)")
    else:
        # upstream names the non-Android file libdrm.so.2.<patch>.0 (meson.build 'version')
        m = re.search(r"libdrm\.so\.2\.(\d+)\.0$", host_real)
        host_ver = f"2.4.{m.group(1)}" if m else "unknown"
        host = dyn_exports(R, host_real)
        hd = {n for n in host if n.startswith("drm")}
        od = {n for n in have if n.startswith("drm")}
        only_host, only_ours = sorted(hd - od), sorted(od - hd)
        print(f"check-libdrm: ABI shape vs host {host_real} (libdrm {host_ver}): host exports {len(hd)} drm*, "
              f"ours {len(od)}; host-only: {' '.join(only_host) or 'none'}; ours-only: {' '.join(only_ours) or 'none'}")
        if host_ver == a.pinned_version and (only_host or only_ours):
            errors.append(f"same version {host_ver} as the host but different drm* export sets")
        elif host_ver != a.pinned_version:
            print(f"check-libdrm: NOTE host libdrm is {host_ver}, pin is {a.pinned_version}; the diff above is informational")

    if errors:
        for e in errors:
            print(f"check-libdrm: FAIL {so}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"check-libdrm: OK {so}")


if __name__ == "__main__":
    main()
