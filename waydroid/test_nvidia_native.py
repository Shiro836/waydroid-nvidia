#!/usr/bin/env python3
# Unit tests for NVIDIA-native mode (no root, no container, no GPU access needed).
#   python3 -m unittest -v waydroid/test_nvidia_native.py      (after dev/build-waydroid)
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TREE = os.path.join(REPO, "build", "waydroid")
V = "615.71.09"
RENDER = "/dev/dri/renderD129"


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


nn = load_module(os.path.join(HERE, "nvidia_native.py"), "nvidia_native_under_test")


def touch(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


ELF = {64: b"\x7fELF\x02", 32: b"\x7fELF\x01"}
ENVLINE = b"__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES=${ORIGIN}/nvnative_egl_android.json\n"
JSON = b'{"file_format_version": "1.0.0", "ICD": {"library_path": "libnvnative-egl-android.so"}}'


def make_fake_host(top):
    """A complete staging tree (lib64 + lib, incl. the staged egl dirs) + host libdirs + dev dir +
    ANGLE guest layout. Returns (root, hostlib, dev); the 32-bit host libdir is <top>/usr-lib32,
    the ANGLE root <top>/nv-guest."""
    root = os.path.join(top, "nvidia-glibc-shim-staging")
    for sub, bits in (("lib64", 64), ("lib", 32)):
        egl = os.path.join(root, "vendor", sub, "egl")
        for name in nn.EGL_FRONT:
            touch(os.path.join(egl, name), ELF[bits])
        for name in nn.EGL_ANGLE:
            touch(os.path.join(egl, name))                       # placeholders
            touch(os.path.join(top, "nv-guest", "vendor", sub, "egl", name), ELF[bits])
        lib = os.path.join(root, "vendor_extra", sub)
        touch(os.path.join(lib, "libnvnative-egl-android.so"), ELF[bits])
        touch(os.path.join(lib, "nvnative_egl_android.json"), JSON)
        touch(os.path.join(lib, "nvglibc-default-env"), b"# comment\n" + ENVLINE)
    for sub, shims in (("lib64", nn.SHIMS), ("lib", nn.SHIMS32)):
        lib = os.path.join(root, "vendor_extra", sub)
        for shim in shims:
            touch(os.path.join(lib, shim), b"\x7fELF")
        touch(os.path.join(lib, "host-glibc-version"), b"2.42\n")
        for blob in nn.BLOBS:
            touch(os.path.join(lib, blob.format(v=V)))          # placeholders
        os.symlink("libEGL_nvidia.so." + V, os.path.join(lib, "libEGL_nvidia.so.0"))
        touch(os.path.join(lib, "hw", "vulkan.nvidia.so"), b"\x7fELF")
        touch(os.path.join(lib, "libgbm_mesa_wrapper.so"), b"\x7fELF")
        touch(os.path.join(lib, "libdrm.so.2"), b"\x7fELF")
    touch(os.path.join(root, "vendor_extra", "lib64", nn.MINIGBM_CORE), ELF[64])
    touch(os.path.join(root, "vendor_extra", "lib", nn.MINIGBM_CORE), ELF[32])
    hostlib = os.path.join(top, "usr-lib")
    for d in (hostlib, hostlib + "32"):
        for blob in nn.BLOBS:
            touch(os.path.join(d, blob.format(v=V)), b"\x7fELF")
    dev = os.path.join(top, "dev")
    for node in ("nvidiactl", "nvidia-modeset", "nvidia0"):
        touch(os.path.join(dev, node))
    return root, hostlib, dev


def expected_entries(root, hostlib, dev):
    lib = os.path.join(root, "vendor_extra", "lib64")
    e = ["lxc.mount.entry = {}/vendor_extra vendor_extra none rbind 0 0".format(root)]
    for blob in nn.BLOBS:
        name = blob.format(v=V)
        e.append("lxc.mount.entry = {}/{} vendor_extra/lib64/{} none rbind,create=file 0 0".format(hostlib, name, name))
    for blob in nn.BLOBS:
        name = blob.format(v=V)
        e.append("lxc.mount.entry = {}32/{} vendor_extra/lib/{} none rbind,create=file 0 0".format(hostlib, name, name))
    e.append("lxc.mount.entry = {}/libgbm_mesa_wrapper.so vendor/lib64/libgbm_mesa_wrapper.so none bind 0 0".format(lib))
    e.append("lxc.mount.entry = {}/vendor_extra/lib/libgbm_mesa_wrapper.so vendor/lib/libgbm_mesa_wrapper.so none bind 0 0".format(root))
    e.append("lxc.mount.entry = {}/libminigbm_gralloc_gbm_mesa.so vendor/lib64/libminigbm_gralloc_gbm_mesa.so none bind 0 0".format(lib))
    e.append("lxc.mount.entry = {}/vendor_extra/lib/libminigbm_gralloc_gbm_mesa.so vendor/lib/libminigbm_gralloc_gbm_mesa.so none bind 0 0".format(root))
    e += egl_entries(root)
    for node in ("nvidiactl", "nvidia-modeset", "nvidia0"):
        e.append("lxc.mount.entry = {}/{} dev/{} none bind,create=file 0 0".format(dev, node, node))
    e.append("lxc.mount.entry = {} dev/dri/renderD129 none bind,create=file 0 0".format(RENDER))
    return e


def egl_entries(root, angle_root=None):
    """The staged egl dir per ABI, then ANGLE's three file binds onto its placeholders."""
    angle_root = angle_root or os.path.join(os.path.dirname(root), "nv-guest")
    e = []
    for sub in ("lib64", "lib"):
        e.append("lxc.mount.entry = {}/vendor/{}/egl vendor/{}/egl none bind 0 0".format(root, sub, sub))
        for x in ("EGL", "GLESv1_CM", "GLESv2"):
            e.append("lxc.mount.entry = {}/vendor/{}/egl/lib{}_angle.so vendor/{}/egl/lib{}_angle.so none bind 0 0"
                     .format(angle_root, sub, x, sub, x))
    return e


# /build.prop of the official vendor image (what glibc-shim, venus and dev-root run on)
STOCK_PROPS = {"ro.vendor.build.fingerprint": "waydroid/lineage_waydroid_x86_64/waydroid_x86_64:13/TQ3A/x:userdebug",
               "ro.vendor.build.date": "Sun Sep 27 20:51:59 UTC 2026", "ro.product.vendor.name": "lineage_waydroid_x86_64"}


class FakeHostMixin:
    module = nn

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.top = self._tmp.name
        self.root, self.hostlib, self.dev = make_fake_host(self.top)
        self.lib = os.path.join(self.root, "vendor_extra", "lib64")
        self.lib32 = os.path.join(self.root, "vendor_extra", "lib")
        m = self.module
        for attr, value in (("driver_version", lambda *a, **k: V),
                            ("nvidia_render_node", lambda *a, **k: RENDER),
                            ("HOST_LIBDIR", self.hostlib),
                            ("HOST_LIBDIR32", self.hostlib + "32"),
                            ("ANGLE_GUEST_ROOT", os.path.join(self.top, "nv-guest")),
                            ("DEV_DIR", self.dev),
                            ("image_build_props", lambda *a, **k: dict(STOCK_PROPS))):
            p = mock.patch.object(m, attr, value)
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self._tmp.cleanup()


class GeneratorTest(FakeHostMixin, unittest.TestCase):
    def assertRaisesNaming(self, *needles):
        with self.assertRaises(OSError) as cm:
            nn.lxc_nodes(self.root)
        msg = str(cm.exception)
        for needle in needles:
            self.assertIn(needle, msg)
        return msg

    def test_exact_entries(self):
        got = nn.lxc_nodes(self.root)
        self.assertEqual(got, expected_entries(self.root, self.hostlib, self.dev))
        self.assertEqual(len(got), 27)

    def test_render_node_bound_once(self):
        # drm_device = the NVIDIA node: gpu.py already emitted its entry, the generator adds none
        gpu_entry = "lxc.mount.entry = {} {} none bind,create=file,optional 0 0".format(RENDER, RENDER[1:])
        got = nn.lxc_nodes(self.root, existing=["lxc.mount.entry = tmpfs dev tmpfs nosuid 0 0", gpu_entry])
        self.assertEqual(got, expected_entries(self.root, self.hostlib, self.dev)[:-1])
        # another GPU's node (hybrid: drm_device = the iGPU) does not suppress ours
        other = "lxc.mount.entry = /dev/dri/renderD128 dev/dri/renderD128 none bind,create=file,optional 0 0"
        self.assertEqual(nn.lxc_nodes(self.root, existing=[other]),
                         expected_entries(self.root, self.hostlib, self.dev))

    def test_32bit_entries(self):
        # P6: the five i386 driver libraries from the 32-bit host libdir, plus the gralloc backend
        # bound over the vendor.img file the image's 32-bit mapper loads; shims, HAL and libdrm
        # are reached through the vendor_extra rbind like the 64-bit ones
        got = nn.lxc_nodes(self.root)
        dsts = [e.split()[3] for e in got]
        self.assertEqual([d for d in dsts if d.startswith(("vendor_extra/lib/", "vendor/lib/"))],
                         ["vendor_extra/lib/" + b.format(v=V) for b in nn.BLOBS] +
                         ["vendor/lib/libgbm_mesa_wrapper.so", "vendor/lib/libminigbm_gralloc_gbm_mesa.so",
                          "vendor/lib/egl"] +
                         ["vendor/lib/egl/" + n for n in nn.EGL_ANGLE])
        for e in got:
            if e.split()[3].startswith("vendor_extra/lib/"):
                self.assertTrue(e.split()[2].startswith(self.hostlib + "32/"), e)

    def test_each_32bit_shim_required(self):
        self.assertIn("ld-linux.so.2", nn.SHIMS32)
        self.assertNotIn("ld-linux-x86-64.so.2", nn.SHIMS32)
        for shim in nn.SHIMS32:
            with self.subTest(shim=shim):
                path = os.path.join(self.lib32, shim)
                os.rename(path, path + ".away")
                try:
                    self.assertRaisesNaming(path, "32-bit", "ARCH=x86", "dev/native-deploy")
                finally:
                    os.rename(path + ".away", path)

    def test_32bit_tree_required(self):
        for rel, needles in (("host-glibc-version", ("dev/native-deploy",)),
                             ("hw/vulkan.nvidia.so", ("32-bit", "Vulkan HAL", "android32")),
                             ("libdrm.so.2", ("VK_EXT_external_memory_dma_buf", "ARCH=x86"))):
            with self.subTest(rel=rel):
                path = os.path.join(self.lib32, rel)
                os.rename(path, path + ".away")
                try:
                    self.assertRaisesNaming(path, *needles)
                finally:
                    os.rename(path + ".away", path)
        link = os.path.join(self.lib32, "libEGL_nvidia.so.0")
        os.unlink(link)
        self.assertRaisesNaming(link, "libEGL_nvidia.so." + V)

    def test_each_32bit_blob_required(self):
        for blob in nn.BLOBS:
            with self.subTest(blob=blob):
                path = os.path.join(self.hostlib + "32", blob.format(v=V))
                os.rename(path, path + ".away")
                try:
                    self.assertRaisesNaming(path, V, "lib32-nvidia-utils")
                finally:
                    os.rename(path + ".away", path)

    def test_gbm_wrapper32_required(self):
        w = os.path.join(self.root, "vendor_extra", "lib", "libgbm_mesa_wrapper.so")
        os.unlink(w)
        self.assertRaisesNaming(w, "32-bit", "mediaserver", "android32", "dev/native-deploy")

    def test_each_shim_required(self):
        for shim in nn.SHIMS:
            with self.subTest(shim=shim):
                path = os.path.join(self.lib, shim)
                os.rename(path, path + ".away")
                try:
                    self.assertRaisesNaming(path, "dev/native-deploy")
                finally:
                    os.rename(path + ".away", path)

    def test_sidecar_missing_and_empty(self):
        path = os.path.join(self.lib, "host-glibc-version")
        open(path, "w").close()
        self.assertRaisesNaming(path, "dev/native-deploy")
        os.unlink(path)
        self.assertRaisesNaming(path, "dev/native-deploy")

    def test_soname_link(self):
        link = os.path.join(self.lib, "libEGL_nvidia.so.0")
        os.unlink(link)
        self.assertRaisesNaming(link, "libEGL_nvidia.so." + V, "dev/native-deploy")
        os.symlink("libEGL_nvidia.so.600.1.1", link)
        self.assertRaisesNaming(link, "dev/native-deploy")

    def test_hw_dir_required(self):
        hw = os.path.join(self.lib, "hw")
        os.unlink(os.path.join(hw, "vulkan.nvidia.so"))
        os.rmdir(hw)
        self.assertRaisesNaming(hw, "nvidia-prolapse/hal", "dev/native-deploy")

    def test_hal_required(self):
        hal = os.path.join(self.lib, "hw", "vulkan.nvidia.so")
        os.unlink(hal)
        self.assertRaisesNaming(hal, "Vulkan HAL", "nvidia-prolapse/hal", "dev/native-deploy")

    def test_gbm_wrapper_required(self):
        w = os.path.join(self.lib, "libgbm_mesa_wrapper.so")
        os.unlink(w)
        self.assertRaisesNaming(w, "gralloc", "nvidia-prolapse/gralloc", "dev/native-deploy")

    def test_minigbm_core_required(self):
        # minigbm's gbm_mesa core with our patch: without it SF's screenshot buffers are LINEAR
        c = os.path.join(self.lib, "libminigbm_gralloc_gbm_mesa.so")
        for content in (None, b"\x7fELF\x01"):        # missing; an ELF32 build
            with self.subTest(content=content):
                os.unlink(c)
                if content is not None:
                    touch(c, content)
                self.assertRaisesNaming(c, "LINEAR", "make -C nvidia-prolapse/gralloc minigbm",
                                        "dev/native-deploy")
                touch(c, ELF[64])

    def test_minigbm_core32_required(self):
        # the 32-bit core with our patches: without it 32-bit locks pass no access flags
        c = os.path.join(self.root, "vendor_extra", "lib", "libminigbm_gralloc_gbm_mesa.so")
        for content in (None, ELF[64]):               # missing; an ELF64 build
            with self.subTest(content=content):
                os.unlink(c)
                if content is not None:
                    touch(c, content)
                self.assertRaisesNaming(c, "32-bit", "read lock", "make -C nvidia-prolapse/gralloc minigbm",
                                        "dev/native-deploy")
                touch(c, ELF[32])

    def test_libdrm_required(self):
        d = os.path.join(self.lib, "libdrm.so.2")
        os.unlink(d)
        self.assertRaisesNaming(d, "VK_EXT_external_memory_dma_buf", "make -C nvidia-prolapse/libdrm",
                                "dev/native-deploy")

    def test_libdrm_not_mounted_separately(self):
        # reached through the vendor_extra rbind; no entry of its own
        self.assertFalse([e for e in nn.lxc_nodes(self.root) if "libdrm" in e])

    def test_each_blob_required(self):
        for blob in nn.BLOBS:
            with self.subTest(blob=blob):
                path = os.path.join(self.hostlib, blob.format(v=V))
                os.rename(path, path + ".away")
                try:
                    self.assertRaisesNaming(path, V, "nvidia-utils")
                finally:
                    os.rename(path + ".away", path)

    def test_device_nodes_required(self):
        for node in ("nvidiactl", "nvidia-modeset"):
            with self.subTest(node=node):
                path = os.path.join(self.dev, node)
                os.rename(path, path + ".away")
                try:
                    self.assertRaisesNaming(path, "nvidia-modprobe")
                finally:
                    os.rename(path + ".away", path)
        os.unlink(os.path.join(self.dev, "nvidia0"))
        self.assertRaisesNaming("nvidia<N>")


class EglStageTest(FakeHostMixin, unittest.TestCase):
    """G7d: the staged /vendor/lib{,64}/egl (our front + ANGLE) and the platform pieces, both ABIs."""

    def assertRaisesNaming(self, *needles):
        return GeneratorTest.assertRaisesNaming(self, *needles)

    def egl(self, sub):
        return os.path.join(self.root, "vendor", sub, "egl")

    def test_dir_bind_precedes_angle_binds(self):
        got = nn.lxc_nodes(self.root)
        for sub in ("lib64", "lib"):
            dsts = [e.split()[3] for e in got]
            d = dsts.index("vendor/{}/egl".format(sub))
            for name in nn.EGL_ANGLE:
                self.assertGreater(dsts.index("vendor/{}/egl/{}".format(sub, name)), d)
        # our front is reached through the directory (a new-inode redeploy is visible), never file-bound
        self.assertFalse([e for e in got if "nvnative" in e])
        # Mesa's EGL files are never mounted (minigbm's gbm_mesa core is not an EGL file)
        self.assertFalse([e for e in got if "mesa.so" in e and "/egl" in e.split()[3]])

    def test_each_front_required_both_abis(self):
        for sub, bits in (("lib64", 64), ("lib", 32)):
            for name in nn.EGL_FRONT:
                with self.subTest(sub=sub, name=name):
                    path = os.path.join(self.egl(sub), name)
                    os.rename(path, path + ".away")
                    try:
                        # the .away file is itself an unexpected file; the message names it first
                        self.assertRaisesNaming(self.egl(sub))
                    finally:
                        os.rename(path + ".away", path)
                    os.unlink(path)
                    try:
                        self.assertRaisesNaming(path, "make -C nvidia-prolapse/egl", "ELF{}".format(bits))
                    finally:
                        touch(path, ELF[bits])

    def test_front_wrong_abi_refused(self):
        path = os.path.join(self.egl("lib"), "libEGL_nvnative.so")
        touch(path, ELF[64])
        self.assertRaisesNaming(path, "ELF32")

    def test_angle_placeholder_and_source_required(self):
        for sub in ("lib64", "lib"):
            for name in nn.EGL_ANGLE:
                with self.subTest(sub=sub, name=name):
                    ph = os.path.join(self.egl(sub), name)
                    os.unlink(ph)
                    try:
                        self.assertRaisesNaming(ph, "placeholder")
                    finally:
                        touch(ph)
                    src = os.path.join(self.top, "nv-guest", "vendor", sub, "egl", name)
                    touch(src)                                  # empty = not provisioned
                    try:
                        self.assertRaisesNaming(src, "ANGLE", "ro.hardware.egl")
                    finally:
                        touch(src, b"\x7fELF")

    def test_egl_dir_required(self):
        import shutil
        shutil.rmtree(self.egl("lib"))
        self.assertRaisesNaming(self.egl("lib"), "/vendor/lib/egl", "32-bit")

    def test_stray_file_refused(self):
        # e.g. Mesa's driver copied in: the loader must see exactly the two staged drivers
        stray = os.path.join(self.egl("lib64"), "libEGL_mesa.so")
        touch(stray, ELF[64])
        self.assertRaisesNaming(self.egl("lib64"), "libEGL_mesa.so", "unexpected")

    def test_platform_pieces_required_both_abis(self):
        for sub, bits in (("lib64", 64), ("lib", 32)):
            lib = os.path.join(self.root, "vendor_extra", sub)
            for name, needles in (("libnvnative-egl-android.so", ("platform library", "ELF{}".format(bits))),
                                  ("nvnative_egl_android.json", ("platform library through it",)),
                                  ("nvglibc-default-env", ("__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES",))):
                with self.subTest(sub=sub, name=name):
                    path = os.path.join(lib, name)
                    os.rename(path, path + ".away")
                    try:
                        self.assertRaisesNaming(path, *needles)
                    finally:
                        os.rename(path + ".away", path)

    def test_json_must_name_platform(self):
        js = os.path.join(self.root, "vendor_extra", "lib", "nvnative_egl_android.json")
        touch(js, b'{"ICD": {"library_path": "libnvidia-egl-wayland.so.1"}}')
        self.assertRaisesNaming(js, "libnvidia-egl-wayland.so.1", "libnvnative-egl-android.so")

    def test_default_env_must_point_at_json(self):
        env = os.path.join(self.root, "vendor_extra", "lib64", "nvglibc-default-env")
        touch(env, b"# empty\n")
        self.assertRaisesNaming(env, "nvnative_egl_android.json")


class GpuReadyTest(unittest.TestCase):
    """ensure_gpu_ready(): the container-start precondition after the render node's device unit came up (boot race,
    2026-10-17: the ExecStartPre ran while nothing had created /dev/nvidia0 yet). A fake nvidia-modprobe makes each
    file it is asked for a symlink to /dev/null, so the files are character devices without CAP_MKNOD."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dev = os.path.join(self._tmp.name, "dev")
        self.proc = os.path.join(self._tmp.name, "gpus")
        os.makedirs(self.dev)
        self.render = os.path.join(self.dev, "dri", "renderD129")
        os.makedirs(os.path.dirname(self.render))
        os.symlink("/dev/null", self.render)
        self.gpus(0)
        self.calls = []
        p = mock.patch.object(nn, "nvidia_render_node", lambda *a, **k: self.render)
        p.start()
        self.addCleanup(p.stop)

    def gpus(self, *minors):
        for i, n in enumerate(minors):
            touch(os.path.join(self.proc, "0000:0{}:00.0".format(i + 1), "information"),
                  "Model: \t\t NVIDIA GeForce RTX 5090\nBus Location: \t 0000:0{}:00.0\nDevice Minor: \t {}\n"
                  "GPU Firmware: \t {}\n".format(i + 1, n, V).encode())

    def node(self, name):
        os.symlink("/dev/null", os.path.join(self.dev, name))

    def modprobe(self, argv, **kw):
        """nvidia-modprobe as nvidia-modprobe.c main() does it: -c N mknods minor N (255 = nvidiactl), -m the
        modeset file."""
        self.calls.append(argv[1:])
        args = argv[1:]
        for i, a in enumerate(args):
            if a == "-c":
                n = int(args[i + 1])
                name = "nvidiactl" if n == 255 else "nvidia{}".format(n)
                if not os.path.lexists(os.path.join(self.dev, name)):
                    self.node(name)
            elif a == "-m" and not os.path.lexists(os.path.join(self.dev, "nvidia-modeset")):
                self.node("nvidia-modeset")
        return types.SimpleNamespace(returncode=0, stdout="", stderr="")

    def ready(self, native=True, run=None, which=lambda name: "/usr/bin/" + name):
        return nn.ensure_gpu_ready(native, dev=self.dev, proc_gpus=self.proc, run=run or self.modprobe,
                                   which=which)

    def test_boot_state_old_generator_fails_new_step_creates_the_files(self):
        # the observed boot: module up, render node there, no /dev/nvidia* file yet
        with self.assertRaises(OSError) as cm:
            nn._device_nodes(self.dev, [])
        self.assertIn("no {}/nvidia<N> device node found".format(self.dev), str(cm.exception))
        created = self.ready()
        self.assertEqual(self.calls, [["-c", "0", "-c", "255"], ["-m"]])
        self.assertEqual(sorted(created), sorted(os.path.join(self.dev, n)
                                                 for n in ("nvidiactl", "nvidia0", "nvidia-modeset")))
        got = nn._device_nodes(self.dev, [])
        self.assertEqual(len(got), 4)                     # ctl, modeset, nvidia0, render

    def test_all_present_runs_nothing(self):
        for name in ("nvidiactl", "nvidia0", "nvidia-modeset"):
            self.node(name)
        self.assertEqual(self.ready(run=lambda *a, **k: self.fail("nvidia-modprobe ran")), [])

    def test_every_gpu_minor(self):
        self.gpus(0, 1)
        for name in ("nvidiactl", "nvidia0", "nvidia-modeset"):
            self.node(name)
        self.assertEqual(self.ready(), [os.path.join(self.dev, "nvidia1")])
        self.assertEqual(self.calls, [["-c", "0", "-c", "1", "-c", "255"]])

    def test_only_modeset_missing(self):
        for name in ("nvidiactl", "nvidia0"):
            self.node(name)
        self.ready()
        self.assertEqual(self.calls, [["-m"]])

    def test_no_nvidia_modprobe_names_the_package(self):
        with self.assertRaises(OSError) as cm:
            self.ready(which=lambda name: None)
        for word in ("nvidia-modprobe is not installed", "nvidia-utils", "systemctl start waydroid-container"):
            self.assertIn(word, str(cm.exception))

    def test_nvidia_modprobe_failure_is_loud(self):
        fail = lambda argv, **kw: types.SimpleNamespace(returncode=1, stdout="", stderr="NVIDIA: failed to load")
        with self.assertRaises(OSError) as cm:
            self.ready(run=fail)
        self.assertIn("exit 1", str(cm.exception))
        self.assertIn("NVIDIA: failed to load", str(cm.exception))

    def test_still_missing_after_nvidia_modprobe(self):
        noop = lambda argv, **kw: types.SimpleNamespace(returncode=0, stdout="", stderr="")
        with self.assertRaises(OSError) as cm:
            self.ready(run=noop)
        self.assertIn("still missing", str(cm.exception))

    def test_not_a_character_device(self):
        touch(os.path.join(self.dev, "nvidiactl"))
        for name in ("nvidia0", "nvidia-modeset"):
            self.node(name)
        with self.assertRaises(OSError) as cm:
            self.ready()
        self.assertIn("not a character device", str(cm.exception))

    def test_no_render_node_names_unit_and_remedy(self):
        os.unlink(self.render)
        for native in (True, False):
            with self.assertRaises(OSError) as cm:
                self.ready(native=native)
            for word in (nn.RENDER_DEVICE_UNIT, "nvidia_drm.modeset=1", "systemctl start waydroid-container"):
                self.assertIn(word, str(cm.exception))

    def test_no_render_node_in_sysfs(self):
        def none(*a, **k):
            raise OSError("NVIDIA native mode: no /dev/dri/renderD* has PCI vendor 0x10de in /sys/class/drm")
        with mock.patch.object(nn, "nvidia_render_node", none):
            with self.assertRaises(OSError) as cm:
                self.ready()
        self.assertIn("NVIDIA GPU not up at container start", str(cm.exception))

    def test_venus_needs_only_the_render_node(self):
        self.assertEqual(self.ready(native=False, run=lambda *a, **k: self.fail("nvidia-modprobe ran")), [])

    def test_gpu_minors(self):
        self.gpus(0, 2)
        self.assertEqual(nn.gpu_minors(self.proc), [0, 2])
        touch(os.path.join(self.proc, "0000:09:00.0", "information"), b"Model: x\n")
        with self.assertRaises(OSError):
            nn.gpu_minors(self.proc)
        with self.assertRaises(OSError) as cm:
            nn.gpu_minors(os.path.join(self._tmp.name, "empty"))
        self.assertIn("drives no GPU", str(cm.exception))


class HostProbeTest(unittest.TestCase):
    def test_render_node_by_vendor(self):
        with tempfile.TemporaryDirectory() as d:
            touch(os.path.join(d, "renderD128", "device", "vendor"), b"0x1002\n")
            touch(os.path.join(d, "renderD129", "device", "vendor"), b"0x10de\n")
            self.assertEqual(nn.nvidia_render_node(d), "/dev/dri/renderD129")
            os.unlink(os.path.join(d, "renderD129", "device", "vendor"))
            with self.assertRaises(OSError) as cm:
                nn.nvidia_render_node(d)
            self.assertIn("0x10de", str(cm.exception))

    def test_driver_version(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "version")
            touch(p, b"NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  615.71.09  Release Build  (root@host)\nGCC version: gcc 15\n")
            self.assertEqual(nn.driver_version(p), "615.71.09")
            touch(p, b"garbage\n")
            self.assertRaises(OSError, nn.driver_version, p)
            self.assertRaises(OSError, nn.driver_version, os.path.join(d, "absent"))


def make_source_root(root):
    """A minimal valid format-2 source NVK + Rust RM root (64-bit graphics ABI, no media)."""
    import hashlib
    import json
    artifacts = {}
    for sub, bits in (("lib64", 64), ("lib", 32)):
        files = ["vendor_extra/{}/{}".format(sub, n) for n in (nn.GBM_WRAPPER, nn.MINIGBM_CORE)]
        files += ["vendor/{}/egl/{}".format(sub, n) for n in nn.EGL_ANGLE]
        if bits == 64:
            files += ["vendor_extra/lib64/" + n for n in (nn.HAL, "libvulkan_nouveau.so", "libnv_bionic_native.so",
                                                          nn.LIBDRM)]
        for relative in files:
            touch(os.path.join(root, relative), ELF[bits])
            artifacts[relative] = hashlib.sha256(ELF[bits]).hexdigest()
    manifest = {"format": 2, "driver": "mesa-nvk-rust-rm", "graphics_abis": ["x86_64"],
                "artifacts": artifacts, "kernel_module": V,
                "gate": {"runner": "tools/native-vulkan-regression.py --run", "exit": 0,
                         "kernel_module": V, "finished": "2026-10-12T16:00:00+02:00",
                         "log_dir": "/x/gate", "exercised": ["vendor_extra/lib64/libvulkan_nouveau.so"]}}
    with open(os.path.join(root, nn.SOURCE_RM_MANIFEST), "w") as f:
        json.dump(manifest, f)
    return root


VENUS_SOCK = "/run/waydroid-venus/venus.sock"
CORE_BY_MODE = {
    # (mode, native_gl): the core properties make_base_props must write (None = absent)
    ("nvidia-prolapse", "off"): {"ro.hardware.vulkan": "nvidia", "ro.hardware.egl": "angle",
                             "ro.zygote.disable_gl_preload": "true", "mesa.vn.debug": None,
                             "mesa.vtest.socket.name": None},
    ("glibc-shim", "off"): {"ro.hardware.vulkan": "nvidia", "ro.hardware.egl": "angle",
                            "ro.zygote.disable_gl_preload": "true", "mesa.vn.debug": None,
                            "mesa.vtest.socket.name": None},
    ("glibc-shim", "on"): {"ro.hardware.vulkan": "nvidia", "ro.hardware.egl": "nvnative",
                           "ro.zygote.disable_gl_preload": "true", "mesa.vn.debug": None,
                           "mesa.vtest.socket.name": None},
    ("venus", "off"): {"ro.hardware.vulkan": "virtio", "ro.hardware.egl": "angle",
                       "ro.zygote.disable_gl_preload": None, "mesa.vn.debug": "vtest",
                       "mesa.vtest.socket.name": "/dev/venus/venus.sock"},
}
REFUSED_GL = {"nvidia-prolapse": "not available in nvidia-prolapse mode", "venus": "only glibc-shim mode has native GL"}


class DriverModeTest(unittest.TestCase):
    """nvidia_native.driver_mode(): the [waydroid] keys, pure (fake roots, no generator)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        top = os.path.realpath(self._tmp.name)
        self.rust = os.path.join(top, "nvidia-source-native-x")
        touch(os.path.join(self.rust, nn.SOURCE_RM_MANIFEST), b"{}")      # driver_mode only looks for it
        self.shim = os.path.join(top, "nvidia-glibc-shim-staging")
        os.makedirs(self.shim)
        self.links = {"nvidia-prolapse": os.path.join(top, "nvidia-prolapse"),
                      "glibc-shim": os.path.join(top, "nvidia-glibc-shim")}
        os.symlink(self.rust, self.links["nvidia-prolapse"])
        os.symlink(self.shim, self.links["glibc-shim"])
        p = mock.patch.object(nn, "ROOT_LINKS", self.links)
        p.start()
        self.addCleanup(p.stop)

    def cfg(self, mode=None, gl=None, **extra):
        w = {"nvidia_guest_layout": "2"}
        if mode:
            w["nvidia_driver_mode"] = mode
        if mode == "nvidia-prolapse":
            w["nvidia_prolapse_install"] = "dev-root"     # the staged-root shape these tests exercise
        if gl:
            w["nvidia_native_gl"] = gl
        w.update(extra)
        return {k: v for k, v in w.items() if v is not None}

    def refused(self, waydroid, *needles):
        with self.assertRaises(OSError) as cm:
            nn.driver_mode(waydroid)
        for needle in needles:
            self.assertIn(needle, str(cm.exception))
        return str(cm.exception)

    def test_no_nvidia_key_is_stock_waydroid(self):
        self.assertIsNone(nn.driver_mode({}))
        self.assertIsNone(nn.driver_mode({"arch": "x86_64", "drm_device": RENDER}))

    def test_every_mode_and_gl(self):
        for (mode, gl), core in CORE_BY_MODE.items():
            with self.subTest(mode=mode, gl=gl):
                dm = nn.driver_mode(self.cfg(mode, gl))
                self.assertEqual((dm.mode, dm.native_gl, dm.egl), (mode, gl == "on", core["ro.hardware.egl"]))
                self.assertEqual(dm.root, {"nvidia-prolapse": self.rust, "glibc-shim": self.shim, "venus": None}[mode])
                self.assertEqual(dm.link, self.links.get(mode))
                self.assertEqual(dm.native, mode != "venus")
                self.assertEqual(dm.venus_socket, VENUS_SOCK if mode == "venus" else None)

    def test_native_gl_defaults_off(self):
        for mode in nn.MODES:
            with self.subTest(mode=mode):
                dm = nn.driver_mode(self.cfg(mode))
                self.assertFalse(dm.native_gl)
                self.assertEqual(dm.egl, "angle")

    def test_native_gl_on_refused_outside_glibc_shim(self):
        for mode, needle in REFUSED_GL.items():
            with self.subTest(mode=mode):
                self.refused(self.cfg(mode, "on"), "nvidia_native_gl = on", needle, "nvidia_native_gl = off")
        msg = self.refused(self.cfg("nvidia-prolapse", "on"), nn.NATIVE_GL_STAGE)
        self.assertIn("build.py --gl", msg)

    def test_native_gl_value_must_be_on_or_off(self):
        self.refused(self.cfg("glibc-shim", "yes"), "nvidia_native_gl = yes", "on or off")

    def test_mode_required(self):
        for extra in ({"nvidia_guest_layout": "2"}, {"nvidia_native_gl": "off"}):
            with self.subTest(extra=extra):
                self.refused(extra, "nvidia_driver_mode is not", "nvidia-prolapse, glibc-shim, venus",
                             "dev/driver-mode set")

    def test_unknown_mode_refused(self):
        self.refused(self.cfg("native"), "nvidia_driver_mode = native", "nvidia-prolapse, glibc-shim, venus")

    def test_root_link_missing_dangling_or_not_a_link_refused(self):
        for mode, setter in (("nvidia-prolapse", "wd-native activate"), ("glibc-shim", "dev/native-deploy")):
            link = self.links[mode]
            target = os.readlink(link)
            with self.subTest(mode=mode):
                os.unlink(link)
                self.refused(self.cfg(mode), link, "does not exist", setter)
                os.symlink(target + "-gone", link)
                self.refused(self.cfg(mode), link, "dangling", setter)
                os.unlink(link)
                os.makedirs(link)
                self.refused(self.cfg(mode), link, "is not a symlink", setter)
                os.rmdir(link)
                os.symlink(target, link)
                nn.driver_mode(self.cfg(mode))

    def test_root_kind_must_match_mode(self):
        nn.repoint_link(self.links["nvidia-prolapse"], self.shim)
        nn.repoint_link(self.links["glibc-shim"], self.rust)
        self.refused(self.cfg("nvidia-prolapse"), "not a source NVK", "wd-native activate")
        self.refused(self.cfg("glibc-shim"), "is a source NVK", "dev/native-deploy")

    def test_retired_path_keys_refused(self):
        for key in nn.RETIRED_KEYS:
            for waydroid in ({key: "/var/lib/waydroid/x"}, self.cfg("nvidia-prolapse", **{key: "/var/lib/waydroid/x"})):
                with self.subTest(key=key, waydroid=waydroid):
                    self.refused(waydroid, key + " = /var/lib/waydroid/x", "no longer read", "dev/driver-mode set MODE",
                                 "strips", nn.VENUS_SOCKET)

    def test_init_defaults_written_only_into_a_fresh_cfg(self):
        fresh = {"arch": "x86_64"}
        msg = nn.init_defaults(fresh)
        self.assertEqual((fresh["nvidia_driver_mode"], fresh["nvidia_native_gl"]), ("nvidia-prolapse", "off"))
        self.assertIn("nvidia_driver_mode = nvidia-prolapse (default", msg)
        self.assertEqual(nn.driver_mode(fresh).egl, "angle")          # GL through ANGLE by default
        for existing in ({"nvidia_driver_mode": "venus"}, {"nvidia_native_gl": "on"}, {"nvidia_native_root": "/x"}):
            before = dict(existing)
            self.assertIsNone(nn.init_defaults(existing))
            self.assertEqual(existing, before)

    def test_tree_initializer_writes_the_defaults(self):
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "waydroid",
                           "tools", "actions", "initializer.py")
        if not os.path.exists(src):
            self.skipTest("run dev/build-waydroid first")
        with open(src) as f:
            self.assertIn("nvidia_native.init_defaults(cfg[\"waydroid\"])", f.read())

    def test_old_mode_name_refused_not_aliased(self):
        # rust-driver was renamed to nvidia-prolapse: refused with the new name and the link remedy
        for gl in (None, "off", "on"):
            with self.subTest(gl=gl):
                self.refused(self.cfg("rust-driver", gl), "nvidia_driver_mode = rust-driver was renamed to nvidia-prolapse",
                             "dev/driver-mode set nvidia-prolapse", self.links["nvidia-prolapse"],
                             "/var/lib/waydroid/nvidia-rust-driver", "dev/driver-mode link nvidia-prolapse")

    def test_legacy_staging_dir_at_the_link_path_named(self):
        link = self.links["nvidia-prolapse"]
        os.unlink(link)
        os.makedirs(link)                              # the old package's glibc-shim staging tree
        self.refused(self.cfg("nvidia-prolapse"), link + " is not a symlink", "move it aside", nn.GLIBC_SHIM_STAGING)

    def test_repoint_link(self):
        link = self.links["nvidia-prolapse"]
        self.assertEqual(nn.repoint_link(link, self.shim), self.rust)
        self.assertEqual(os.readlink(link), self.shim)
        fresh = os.path.join(os.path.dirname(link), "fresh-link")
        self.assertIsNone(nn.repoint_link(fresh, self.rust))
        self.assertEqual(os.readlink(fresh), self.rust)
        for bad in ("relative/dir", self.rust + "-missing"):
            with self.subTest(bad=bad):
                self.assertRaisesRegex(OSError, "not an existing absolute directory", nn.repoint_link, link, bad)
        self.assertRaisesRegex(OSError, "not a symlink", nn.repoint_link, self.shim, self.rust)
        self.assertEqual(os.readlink(link), self.shim)
        self.assertFalse([n for n in os.listdir(os.path.dirname(link)) if ".tmp-" in n])

    def test_switch_is_one_key(self):
        # the roots and the socket are conventions; only nvidia_driver_mode changes
        self.assertEqual([nn.driver_mode(self.cfg(m)).mode for m in nn.MODES], list(nn.MODES))

    def test_announce_names_mode_root_gl(self):
        line = nn.announce(nn.driver_mode(self.cfg("glibc-shim", "on")))
        for needle in ("glibc-shim", self.links["glibc-shim"] + " -> " + self.shim, "NVIDIA's own GLES",
                       "ro.hardware.egl=nvnative", "nvidia_native_gl = on"):
            self.assertIn(needle, line)
        line = nn.announce(nn.driver_mode(self.cfg("venus")))
        self.assertIn(VENUS_SOCK, line)
        self.assertIn("ANGLE", line)

    def test_mode_properties(self):
        for (mode, gl), core in CORE_BY_MODE.items():
            with self.subTest(mode=mode, gl=gl):
                with mock.patch.object(nn, "base_properties", return_value=[]):
                    self.assertEqual(dict(nn.mode_properties(nn.driver_mode(self.cfg(mode, gl)))), core)
        # the guest socket follows the host socket's name inside the dev/venus bind
        with mock.patch.object(nn, "VENUS_SOCKET", "/run/x/other.sock"):
            dm = nn.driver_mode(self.cfg("venus"))
        self.assertEqual(dict(nn.mode_properties(dm))["mesa.vtest.socket.name"], "/dev/venus/other.sock")

    def test_media_properties_only_from_native_roots(self):
        with mock.patch.object(nn, "base_properties", return_value=list(nn.MEDIA_BASE_PROPERTIES)) as bp:
            props = dict(nn.mode_properties(nn.driver_mode(self.cfg("nvidia-prolapse"))))
            bp.assert_called_once_with(self.rust)
            self.assertEqual(props["debug.stagefright.ccodec"], "4")
            bp.reset_mock()
            nn.mode_properties(nn.driver_mode(self.cfg("venus")))
            bp.assert_not_called()

    def test_hand_set_derived_property_refused_in_every_mode(self):
        for mode in nn.MODES:
            dm = nn.driver_mode(self.cfg(mode))
            nn.check_properties({"debug.hwui.renderer": "skiavk", "persist.waydroid.refresh_rate": "500"}, dm)
            for key in nn.derived_properties():
                with self.subTest(mode=mode, key=key):
                    with self.assertRaises(OSError) as cm, \
                            mock.patch.object(nn, "base_properties", return_value=[]):
                        nn.check_properties({key: "x"}, dm)
                    msg = str(cm.exception)
                    for needle in (key, mode, "nvidia_driver_mode", "Delete them", "upgrade -o", "dev/driver-mode"):
                        self.assertIn(needle, msg)

    def test_decode_route_property_refused_in_native_modes(self):
        for mode in ("nvidia-prolapse", "glibc-shim"):
            with self.subTest(mode=mode):
                with self.assertRaises(OSError) as cm:
                    nn.check_properties({nn.DECODE_ROUTE_PROPERTY: "off"}, nn.driver_mode(self.cfg(mode)))
                self.assertIn("waydroid prop set", str(cm.exception))

    def test_check_base_prop(self):
        dm = nn.driver_mode(self.cfg("venus"))
        path = os.path.join(self._tmp.name, "waydroid_base.prop")
        good = "ro.hardware.gralloc=gbm\nro.hardware.vulkan=virtio\nro.hardware.egl=angle\nmesa.vn.debug=vtest\n" \
               "mesa.vtest.socket.name=/dev/venus/venus.sock\n"
        touch(path, good.encode())
        nn.check_base_prop(path, dm)
        # generated for the native mode, cfg switched to venus without upgrade -o
        touch(path, b"ro.hardware.vulkan=nvidia\nro.hardware.egl=angle\nro.zygote.disable_gl_preload=true\n")
        with self.assertRaises(OSError) as cm:
            nn.check_base_prop(path, dm)
        for needle in ("venus", "ro.hardware.vulkan=virtio (has nvidia)", "ro.zygote.disable_gl_preload=(absent)",
                       "upgrade -o"):
            self.assertIn(needle, str(cm.exception))


# --- tools/helpers/lxc.py from build/waydroid (pinned upstream + baseline patch + ours) ------

GUEST = "/var/lib/waydroid/nv/guest"
BASELINE_GUEST_FILES = (
    "vendor/lib/hw/vulkan.virtio.so",
    "vendor/lib/egl/libEGL_angle.so",
    "vendor/lib/egl/libGLESv1_CM_angle.so",
    "vendor/lib/egl/libGLESv2_angle.so",
    "vendor/lib64/hw/vulkan.virtio.so",
    "vendor/lib64/egl/libEGL_angle.so",
    "vendor/lib64/egl/libGLESv1_CM_angle.so",
    "vendor/lib64/egl/libGLESv2_angle.so",
    "vendor/lib64/libgbm_mesa_wrapper.so",
    "vendor/lib64/hw/hwcomposer.waydroid.so",
    "system/bin/surfaceflinger",
)
# kept from the baseline loop in native mode; ANGLE's egl files are bound by the generator instead
KEPT = [f for f in BASELINE_GUEST_FILES if "virtio" not in f and "gbm" not in f and "/egl/" not in f]


def guest_entry(f):
    return "lxc.mount.entry = {}/{} {} none rbind,create=file 0 0".format(GUEST, f, f)


def _stub_gbinder():
    """Waydroid's modules import gbinder (python-gbinder, a C extension over libgbinder) at module
    level for its binder services. These tests only call the config generator, the property writer
    and the start/session prechecks, which never open binder; where the extension is not installed
    (CI runners), an empty module stands in so the imports succeed. Any binder call would raise
    AttributeError on the stub instead of passing silently."""
    try:
        import gbinder  # noqa: F401
    except ImportError:
        sys.modules["gbinder"] = types.ModuleType("gbinder")


def import_tree():
    if not os.path.isfile(os.path.join(TREE, "tools", "helpers", "nvidia_native.py")):
        raise unittest.SkipTest("build/waydroid not assembled; run dev/build-waydroid")
    with open(os.path.join(TREE, "tools", "helpers", "nvidia_native.py"), "rb") as a, \
            open(os.path.join(HERE, "nvidia_native.py"), "rb") as b:
        if a.read() != b.read():
            raise AssertionError("build/waydroid has a stale nvidia_native.py; regenerate the patch "
                                 "and run dev/build-waydroid")
    if TREE not in sys.path:
        sys.path.insert(0, TREE)
    _stub_gbinder()
    import tools.helpers.lxc as lxc
    import tools.helpers.nvidia_native as tree_nn
    import tools.helpers.gpu as gpu
    return lxc, tree_nn, gpu


class LxcAppArmorTest(unittest.TestCase):
    """lxc.py drop_unsupported_apparmor() (baseline patch): lxc.apparmor.profile stays only when this host's LXC
    parses it. Fedora's LXC is built without AppArmor and refuses the whole config (issue #27)."""
    BASE = "lxc.uts.name = waydroid\n\nlxc.apparmor.profile = unconfined\nlxc.no_new_privs = 1\n"
    REFUSED = "lxc-info: probe: ../src/lxc/confile.c: set_config_apparmor_profile: 1651 Invalid argument - Built without AppArmor support"

    def setUp(self):
        self.lxc, _, _ = import_tree()
        self.tmp = tempfile.TemporaryDirectory()
        self.config = os.path.join(self.tmp.name, "config")
        with open(self.config, "w") as f:
            f.write(self.BASE)

    def tearDown(self):
        self.tmp.cleanup()

    def drop(self, verdicts):
        import subprocess
        probed = []

        def run(argv, **kwargs):
            self.assertEqual(argv[:2], ["lxc-info", "-P"])
            self.assertEqual(kwargs["env"]["LC_ALL"], "C")
            with open(os.path.join(argv[2], "probe", "config")) as f:
                probed.append(f.read())
            code, err = verdicts.pop(0)
            return subprocess.CompletedProcess(argv, code, "", err)
        with mock.patch.object(self.lxc.subprocess, "run", side_effect=run), \
                self.assertLogs(level="INFO") if verdicts[-1][0] else contextlib.nullcontext():
            self.lxc.drop_unsupported_apparmor(self.config)
        with open(self.config) as f:
            return probed, f.read()

    def test_kept_where_lxc_parses_it(self):
        probed, text = self.drop([(0, ""), (0, "")])
        self.assertEqual(probed, ["", "lxc.apparmor.profile = unconfined\n"])
        self.assertEqual(text, self.BASE)

    def test_dropped_where_lxc_refuses_it(self):
        probed, text = self.drop([(0, ""), (1, self.REFUSED)])
        self.assertEqual(text, self.BASE.replace("lxc.apparmor.profile = unconfined\n", ""))

    def test_lxc_that_loads_nothing_is_an_error(self):
        with self.assertRaisesRegex(OSError, "cannot load an empty container config"):
            self.drop([(1, "lxc-info: broken"), (0, "")])

    def test_no_key_no_probe(self):
        with open(self.config, "w") as f:
            f.write("lxc.uts.name = waydroid\n")
        probed, _ = self.drop([(0, "")])
        self.assertEqual(probed, [])

    @unittest.skipUnless(shutil.which("lxc-info"), "LXC not installed")
    def test_real_lxc_answers(self):
        self.assertEqual(self.lxc.lxc_parses(""), (True, ""))
        ok, message = self.lxc.lxc_parses("lxc.no_such_key = 1\n")
        self.assertFalse(ok)
        self.assertIn('Unsupported config key "lxc.no_such_key"', message)


class LxcIntegrationTest(FakeHostMixin, unittest.TestCase):
    """generate_nodes_lxc_config() / make_base_props() / start() of build/waydroid, every mode x GL."""

    def setUp(self):
        lxc, tree_nn, gpu = import_tree()
        self.lxc = lxc
        self.module = tree_nn          # FakeHostMixin patches the module lxc.py imports
        super().setUp()
        self.rust = make_source_root(os.path.join(self.top, "nvidia-source-native-x"))
        # the root conventions: links to the staged roots; the generator emits the real paths
        self.links = {"nvidia-prolapse": os.path.join(self.top, "nvidia-prolapse"),
                      "glibc-shim": os.path.join(self.top, "nvidia-glibc-shim")}
        os.symlink(self.rust, self.links["nvidia-prolapse"])
        os.symlink(self.root, self.links["glibc-shim"])
        p = mock.patch.object(self.module, "ROOT_LINKS", self.links)
        p.start()
        self.addCleanup(p.stop)
        for f in nn.VENUS_GUEST_FILES:                     # waydroid-nvidia's guest layout 2 (venus)
            if not os.path.exists(os.path.join(self.top, "nv-guest", f)):
                touch(os.path.join(self.top, "nv-guest", f), b"\x7fELF")
        p = mock.patch.object(gpu, "getDriNode", lambda args: ("", ""))
        p.start()
        self.addCleanup(p.stop)
        self.args = types.SimpleNamespace(
            config=os.path.join(self.top, "waydroid.cfg"), vendor_type="MAINLINE", work=self.top,
            BINDER_DRIVER="binder", VNDBINDER_DRIVER="vndbinder", HWBINDER_DRIVER="hwbinder")

    def write_cfg(self, waydroid, properties=None):
        with open(self.args.config, "w") as f:
            f.write("[waydroid]\n")
            for k, v in waydroid.items():
                if v is not None:
                    f.write("{} = {}\n".format(k, v))
            f.write("\n[properties]\n")
            for k, v in (properties or {}).items():
                f.write("{} = {}\n".format(k, v))

    def mode_cfg(self, mode, gl=None, properties=None, **extra):
        """Both root links and layout 2 in place; only the mode (and GL) select."""
        w = {"nvidia_driver_mode": mode, "nvidia_guest_layout": "2", "nvidia_native_gl": gl, "drm_device": RENDER}
        if mode == "nvidia-prolapse":
            w["nvidia_prolapse_install"] = "dev-root"
        w.update(extra)
        self.write_cfg(w, dict({"debug.hwui.renderer": "skiavk"}, **(properties or {})))

    def nodes(self):
        return self.lxc.generate_nodes_lxc_config(self.args)

    def refused(self, *needles, call=None):
        with self.assertRaises(OSError) as cm:
            (call or self.nodes)()
        for needle in needles:
            self.assertIn(needle, str(cm.exception))
        return str(cm.exception)

    def base_props(self):
        """make_base_props() on a fake MAINLINE host without host props or gralloc HAL; parsed result."""
        self.args.images_path = self.lxc.tools.config.defaults["preinstalled_images_paths"][0]
        with mock.patch.object(self.lxc.tools.helpers.props, "host_get", lambda args, prop: ""), \
             mock.patch.object(self.lxc.tools.helpers.gpu, "getDriNode", lambda args: (RENDER, "")), \
             mock.patch.object(self.lxc.tools.helpers.gpu, "getVulkanDriver", lambda args, node: ""):
            self.lxc.make_base_props(self.args)
        with open(os.path.join(self.top, "waydroid_base.prop")) as f:
            lines = [line.rstrip("\n") for line in f]
        return lines, dict(line.split("=", 1) for line in lines)

    def assertCoreProps(self, mode, gl):
        lines, props = self.base_props()
        for key, value in CORE_BY_MODE[(mode, gl)].items():
            if value is None:
                self.assertNotIn(key, props)
            else:
                self.assertEqual(props[key], value, key)
                self.assertEqual(sum(line.startswith(key + "=") for line in lines), 1, key)
        self.assertEqual(props["debug.hwui.renderer"], "skiavk")       # other [properties] still applied
        return props

    def assertNativeBlock(self, nodes, native):
        self.assertFalse([e for e in nodes if "vulkan.virtio.so" in e or " dev/venus " in e])
        self.assertEqual(nodes[-len(native):], native)
        self.assertEqual(nodes[-len(native) - len(KEPT):-len(native)], [guest_entry(f) for f in KEPT])

    # ---- glibc-shim
    def test_glibc_shim_gl_off_and_on(self):
        for gl in ("off", "on", None):
            with self.subTest(gl=gl):
                self.mode_cfg("glibc-shim", gl)
                nodes = self.nodes()
                self.assertNativeBlock(nodes, expected_entries(self.root, self.hostlib, self.dev))
                self.assertFalse([e for e in nodes if self.rust in e])
                # exactly our two wrappers (64-bit and the 32-bit mapper), never the Venus build's
                gbm = [e for e in nodes if "libgbm_mesa_wrapper.so" in e]
                self.assertEqual([e.split()[2] for e in gbm],
                                 [os.path.join(self.root, "vendor_extra", s, "libgbm_mesa_wrapper.so") for s in ("lib64", "lib")])
                # ANGLE is bound once per file, by the generator, after the staged egl directory
                self.assertEqual([e for e in nodes if "_angle.so" in e],
                                 [e for e in egl_entries(self.root) if "_angle.so" in e])
                props = self.assertCoreProps("glibc-shim", gl or "off")
                self.assertEqual(props["debug.stagefright.ccodec"], "0")     # blob root: no media

    def test_glibc_shim_gl_on_without_front_fails_upgrade(self):
        self.mode_cfg("glibc-shim", "on")
        front = os.path.join(self.root, "vendor", "lib", "egl", "libGLESv2_nvnative.so")
        os.unlink(front)
        self.refused(front)

    def test_glibc_shim_missing_pieces_fail_upgrade(self):
        self.mode_cfg("glibc-shim")
        for rel in ("hw/vulkan.nvidia.so", "libdrm.so.2"):
            with self.subTest(rel=rel):
                path = os.path.join(self.lib, rel)
                os.rename(path, path + ".away")
                try:
                    self.refused(rel)
                finally:
                    os.rename(path + ".away", path)

    def test_drm_device_is_the_nvidia_node(self):
        # gpu.py binds drm_device (only if the node exists); the generator must not bind it again
        self.mode_cfg("glibc-shim")
        real_exists = os.path.exists
        with mock.patch.object(self.lxc.tools.helpers.gpu, "getDriNode", lambda args: (RENDER, "")), \
                mock.patch("os.path.exists", lambda p: p == RENDER or real_exists(p)):
            nodes = self.nodes()
        render = [e for e in nodes if e.split()[2] == RENDER]
        self.assertEqual(render, ["lxc.mount.entry = {} {} none bind,create=file,optional 0 0".format(RENDER, RENDER[1:])])
        native = expected_entries(self.root, self.hostlib, self.dev)[:-1]
        self.assertEqual(nodes[-len(native):], native)

    def test_native_modes_need_layout_2(self):
        for mode in ("glibc-shim", "nvidia-prolapse"):
            with self.subTest(mode=mode):
                self.mode_cfg(mode, nvidia_guest_layout=None)
                self.refused("nvidia_guest_layout = 2")

    # ---- nvidia-prolapse
    def test_prolapse_gl_off(self):
        for gl in ("off", None):
            with self.subTest(gl=gl):
                self.mode_cfg("nvidia-prolapse", gl)
                nodes = self.nodes()
                source = self.module._source_rm_nodes(self.rust) + self.module._device_nodes(self.dev, [])
                self.assertNativeBlock(nodes, source)
                self.assertFalse([e for e in nodes if "/usr-lib" in e or "libEGL_nvidia" in e or self.root + "/" in e])
                self.assertIn("lxc.mount.entry = {}/vendor/lib64/egl vendor/lib64/egl none bind 0 0".format(self.rust), nodes)
                props = self.assertCoreProps("nvidia-prolapse", "off")
                self.assertEqual(props["debug.stagefright.ccodec"], "0")     # format 2: no media record

    def test_prolapse_gl_on_refused(self):
        self.mode_cfg("nvidia-prolapse", "on")
        self.refused("has no native_gl record", self.module.NATIVE_GL_STAGE)
        self.refused("has no native_gl record", call=self.base_props)

    def test_rust_driver_media_root_lists_codec2(self):
        self.mode_cfg("nvidia-prolapse")
        with mock.patch.object(self.module, "base_properties", return_value=list(nn.MEDIA_BASE_PROPERTIES)) as bp:
            lines, props = self.base_props()
        bp.assert_called_with(self.rust)
        for key, value in nn.MEDIA_BASE_PROPERTIES:
            self.assertEqual(props[key], value)
            self.assertEqual(sum(line.startswith(key + "=") for line in lines), 1, key)
        self.assertEqual(props["ro.hardware.gralloc"], "gbm")

    def test_rust_driver_root_kind_checked(self):
        self.module.repoint_link(self.links["nvidia-prolapse"], self.root)
        self.mode_cfg("nvidia-prolapse")
        self.refused("not a source NVK")

    # ---- venus
    def test_venus_is_the_baseline(self):
        self.mode_cfg("venus", "off")
        nodes = self.nodes()
        venus = ["lxc.mount.entry = /run/waydroid-venus dev/venus none rbind,create=dir 0 0"]
        venus += [guest_entry(f) for f in BASELINE_GUEST_FILES]
        self.assertEqual(nodes[-len(venus):], venus)
        self.assertFalse([e for e in nodes if self.root in e or self.rust in e or "vendor_extra" in e])
        props = self.assertCoreProps("venus", "off")
        self.assertEqual(props["debug.stagefright.ccodec"], "0")

    def test_venus_pre_layout_2_compat_kept(self):
        # the baseline's compatibility mounts for installs before the dual-ABI guest layout
        self.mode_cfg("venus", nvidia_guest_layout=None)
        nodes = self.nodes()
        self.assertIn("lxc.mount.entry = /var/lib/waydroid/nv/surfaceflinger system/bin/surfaceflinger none "
                      "rbind,create=file 0 0", nodes)

    def test_venus_needs_the_nvidia_render_node(self):
        # hybrid host: renderD128 is the other GPU; selected by PCI vendor (nvidia_render_node), never by number
        for drm in ("/dev/dri/renderD128", None):
            with self.subTest(drm=drm):
                self.mode_cfg("venus", drm_device=drm)
                self.refused("drm_device = " + RENDER, "0x10de", str(drm or "unset"))

    def test_venus_guest_files_required(self):
        self.mode_cfg("venus")
        for f in ("vendor/lib64/hw/vulkan.virtio.so", "vendor/lib64/libgbm_mesa_wrapper.so"):
            with self.subTest(f=f):
                path = os.path.join(self.top, "nv-guest", f)
                os.rename(path, path + ".away")
                try:
                    self.refused(f, "waydroid-nvidia-setup")
                finally:
                    os.rename(path + ".away", path)

    def test_venus_gl_on_refused(self):
        self.mode_cfg("venus", "on")
        self.refused("only glibc-shim mode has native GL")

    # ---- refusals common to all modes
    def test_mode_key_required(self):
        self.write_cfg({"nvidia_guest_layout": "2", "nvidia_native_gl": "off"})
        self.refused("nvidia_driver_mode is not")
        self.refused("nvidia_driver_mode is not", call=self.base_props)

    def test_retired_path_keys_fail_upgrade(self):
        for key in self.module.RETIRED_KEYS:
            for mode in (None, "nvidia-prolapse"):
                with self.subTest(key=key, mode=mode):
                    self.write_cfg({"nvidia_driver_mode": mode, "nvidia_guest_layout": "2", key: self.rust})
                    self.refused(key, "dev/driver-mode set MODE", "strips")
                    self.refused(key, call=self.base_props)

    def test_config_nodes_carry_the_link_targets(self):
        # repointing the link changes what upgrade -o emits; the link path itself never reaches config_nodes
        self.mode_cfg("nvidia-prolapse")
        nodes = self.nodes()
        self.assertFalse([e for e in nodes if self.links["nvidia-prolapse"] in e])
        self.assertIn("lxc.mount.entry = {}/vendor_extra vendor_extra none rbind 0 0".format(self.rust), nodes)

    def test_hand_set_derived_property_fails_upgrade(self):
        for mode in nn.MODES:
            for key in self.module.derived_properties():
                with self.subTest(mode=mode, key=key):
                    self.mode_cfg(mode, properties={key: "x"})
                    self.refused(key, "derived")
                    self.refused(key, "derived", call=self.base_props)

    def test_neither_mode(self):
        self.write_cfg({}, {})
        nodes = self.nodes()
        self.assertFalse([e for e in nodes if GUEST in e or "venus" in e or self.root in e])
        with mock.patch.object(self.module, "base_properties") as bp:
            lines, props = self.base_props()
        bp.assert_not_called()
        self.assertEqual(props["debug.stagefright.ccodec"], "0")
        self.assertEqual(props["ro.hardware.egl"], "mesa")             # stock Waydroid untouched

    # ---- container start: per-session display properties (images.make_prop)
    def session_props(self, rates):
        import tools.helpers.images as images
        session = {"user_name": "u", "user_id": "1000", "group_id": "1000", "waydroid_data": "/d",
                   "background_start": "true", "lcd_density": "0"}
        out = os.path.join(self.top, "waydroid.prop")
        with mock.patch.object(self.module, "active_refresh_rates", lambda: (rates, [])):
            with self.assertLogs(level="INFO") as logs:
                images.make_prop(self.args, session, out)
        with open(out) as f:
            lines = [line.rstrip("\n") for line in f]
        return lines, "\n".join(logs.output)

    def test_session_snap_window_follows_the_fastest_display(self):
        self.mode_cfg("nvidia-prolapse", "off", {nn.SNAP_PROPERTY: "1000000"})   # an old setup's value
        self.base_props()
        lines, log = self.session_props([164.999, 499.991])
        self.assertEqual([p for p in lines if p.startswith(nn.SNAP_PROPERTY + "=")], [nn.SNAP_PROPERTY + "=1000018"])
        self.assertIn("499.991 Hz", log)
        self.assertIn("derived at every container start", log)
        lines, log = self.session_props([60.0, 144.0])                        # stock 3 ms fits: not written
        self.assertFalse([p for p in lines if p.startswith(nn.SNAP_PROPERTY + "=")])
        self.assertIn("stock 3 ms", log)
        self.assertIn("waydroid.host.user=u", lines)                           # session values still there

    def test_session_snap_window_explicit_refresh_wins(self):
        self.mode_cfg("venus", properties={nn.REFRESH_PROPERTY: "250"})
        self.base_props()
        lines, log = self.session_props([499.991])
        self.assertIn(nn.SNAP_PROPERTY + "=2000000", lines)
        self.assertIn(nn.REFRESH_PROPERTY + "=250", lines)

    def test_session_props_stock_waydroid_untouched(self):
        self.write_cfg({}, {nn.SNAP_PROPERTY: "1000000"})
        self.base_props()
        import tools.helpers.images as images
        session = {"user_name": "u", "user_id": "1000", "group_id": "1000", "waydroid_data": "/d",
                   "background_start": "true", "lcd_density": "0"}
        out = os.path.join(self.top, "waydroid.prop")
        with mock.patch.object(self.module, "active_refresh_rates") as rates:
            images.make_prop(self.args, session, out)
        rates.assert_not_called()
        with open(out) as f:
            self.assertIn(nn.SNAP_PROPERTY + "=1000000\n", f.read())

    # ---- container start
    def test_start_announces_and_refuses_stale_base_prop(self):
        self.mode_cfg("glibc-shim", "on")
        self.base_props()                                  # waydroid_base.prop for glibc-shim + nvnative
        started = []
        with mock.patch.object(self.lxc.tools.helpers.run, "user", lambda *a, **k: started.append(a)), \
                mock.patch.object(self.lxc, "wait_for_running", lambda args: None):
            self.args.log = os.path.join(self.top, "log")
            with self.assertLogs(level="INFO") as logs:
                self.lxc.start(self.args)
            self.assertEqual(len(started), 1)
            self.assertIn("NVIDIA driver mode: glibc-shim", "\n".join(logs.output))
            self.assertIn("ro.hardware.egl=nvnative", "\n".join(logs.output))
            self.mode_cfg("venus")                         # switched without upgrade -o
            self.refused("ro.hardware.vulkan=virtio (has nvidia)", "upgrade -o", call=lambda: self.lxc.start(self.args))
            self.assertEqual(len(started), 1)              # lxc-start never ran


class SnapWindowTest(unittest.TestCase):
    """SurfaceFlinger's vsync snap window derived from the display refresh (no host access)."""

    def test_window_is_half_the_period_above_166_hz(self):
        self.assertIsNone(nn.snap_window_ns(60))
        self.assertIsNone(nn.snap_window_ns(144))
        self.assertIsNone(nn.snap_window_ns(166))                  # 3.012 ms half period: stock fits
        self.assertEqual(nn.snap_window_ns(167), 2994011)
        self.assertEqual(nn.snap_window_ns(240), 2083333)
        self.assertEqual(nn.snap_window_ns(500), 1000000)          # the value measured live at 500 Hz
        self.assertEqual(nn.snap_window_ns(499.991), 1000018)
        self.assertEqual(nn.snap_window_ns(1000), 500000)

    def test_session_properties(self):
        S, R = nn.SNAP_PROPERTY, nn.REFRESH_PROPERTY
        props, notes = nn.session_properties(["a=1", S + "=7"], [164.999, 499.991])
        self.assertEqual(props, ["a=1", S + "=1000018"])
        self.assertIn("ignoring " + S + "=7", notes[0])
        props, _ = nn.session_properties(["a=1", R + "=500"], [60.0])           # explicit refresh decides
        self.assertEqual(props, ["a=1", R + "=500", S + "=1000000"])
        props, _ = nn.session_properties([R + "=144"], [500.0])
        self.assertEqual(props, [R + "=144"])
        for bad in ("0", "1001", "abc", ""):                                    # hwcomposer ignores these
            props, _ = nn.session_properties([R + "=" + bad], [500.0])
            self.assertEqual(props[-1], S + "=1000000", bad)
        props, notes = nn.session_properties([S + "=1000000"], [])
        self.assertEqual(props, [])
        self.assertIn("no active display mode", notes[-1])

    def test_mode_refresh(self):
        self.assertAlmostEqual(nn._mode_refresh_hz(2455250, 2944, 1668, 0, 500, 0), 499.9906, places=3)
        self.assertAlmostEqual(nn._mode_refresh_hz(74250, 2200, 1125, 0, 60, 1 << 4), 60.0)   # interlaced
        self.assertEqual(nn._mode_refresh_hz(0, 0, 0, 0, 75, 0), 75.0)

    def test_active_refresh_rates_names_unreadable_cards(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(nn.active_refresh_rates(d), ([], []))
            touch(os.path.join(d, "card0"))                                     # not a DRM node
            touch(os.path.join(d, "renderD128"))                                # never queried
            rates, notes = nn.active_refresh_rates(d)
            self.assertEqual(rates, [])
            self.assertEqual(len(notes), 1)
            self.assertIn("card0", notes[0])


class SessionPrecheckTest(unittest.TestCase):
    """session start: the Venus socket precheck runs in venus mode only."""

    class PastPrecheck(Exception):
        pass

    def setUp(self):
        import_tree()
        import tools.config
        import tools.helpers.props
        import tools.actions.session_manager as sm
        self.sm = sm
        self._tmp = tempfile.TemporaryDirectory()
        d = self._tmp.name
        wayland = os.path.join(d, "wayland-0")
        touch(wayland)
        self.shim = os.path.join(d, "shim")
        os.makedirs(self.shim)
        os.symlink(self.shim, os.path.join(d, "nvidia-glibc-shim"))
        self.args = types.SimpleNamespace(config=os.path.join(d, "waydroid.cfg"))
        self.dead_socket = os.path.join(d, "venus.sock")      # nothing listens here
        import tools.helpers.nvidia_native as tree_nn
        for p in (mock.patch.object(tree_nn, "VENUS_SOCKET", self.dead_socket),
                  mock.patch.object(tree_nn, "ROOT_LINKS", {"nvidia-prolapse": os.path.join(d, "absent"),
                                                            "glibc-shim": os.path.join(d, "nvidia-glibc-shim")}),
                  mock.patch.object(sm.dbus.service, "BusName", lambda *a, **k: object()),
                  # no session bus needed: start() only hands it to the (mocked) BusName
                  mock.patch.object(sm.dbus, "SessionBus", lambda *a, **k: object()),
                  mock.patch.dict(tools.config.session_defaults,
                                  {"wayland_display": wayland, "waydroid_data": d}),
                  mock.patch.object(tools.helpers.props, "host_get",
                                    mock.Mock(side_effect=self.PastPrecheck))):
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def write_cfg(self, text):
        with open(self.args.config, "w") as f:
            f.write(text)

    def test_venus_mode_checks_socket(self):
        self.write_cfg("[waydroid]\nnvidia_driver_mode = venus\n")
        with self.assertLogs(level="ERROR") as logs, self.assertRaises(SystemExit):
            self.sm.start(self.args)
        self.assertIn(self.dead_socket, "\n".join(logs.output))

    def test_native_mode_skips_socket(self):
        self.write_cfg("[waydroid]\nnvidia_driver_mode = glibc-shim\n")
        self.assertRaises(self.PastPrecheck, self.sm.start, self.args)

    def test_retired_key_refused(self):
        self.write_cfg("[waydroid]\nnvidia_driver_mode = venus\nnvidia_venus_socket = {}\n".format(self.dead_socket))
        with self.assertRaises(OSError) as cm:
            self.sm.start(self.args)
        self.assertIn("dev/driver-mode set MODE", str(cm.exception))

PROC = ("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  {}  Release Build  (root@host)\n"
        "GCC version: gcc 16\n")


class SourceRmDeploymentTest(unittest.TestCase):
    def setUp(self):
        import hashlib
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = os.path.join(self.tmp.name, "root")
        self.proc = os.path.join(self.tmp.name, "proc-version")
        self.set_running(V)
        p = mock.patch.object(nn, "PROC_VERSION", self.proc)
        p.start()
        self.addCleanup(p.stop)
        artifacts = {}
        for sub, bits in (("lib64", 64), ("lib", 32)):
            files = ["vendor_extra/{}/{}".format(sub, n) for n in (nn.GBM_WRAPPER, nn.MINIGBM_CORE)]
            files += ["vendor/{}/egl/{}".format(sub, n) for n in nn.EGL_ANGLE]
            if bits == 64:
                files += ["vendor_extra/lib64/" + n for n in (nn.HAL, "libvulkan_nouveau.so", "libnv_bionic_native.so", nn.LIBDRM)]
            for relative in files:
                touch(os.path.join(self.root, relative), ELF[bits])
                artifacts[relative] = hashlib.sha256(ELF[bits]).hexdigest()
        self.manifest = {"format": 2, "driver": "mesa-nvk-rust-rm", "graphics_abis": ["x86_64"],
                         "artifacts": artifacts, "kernel_module": V,
                         "gate": {"runner": "tools/native-vulkan-regression.py --run", "exit": 0,
                                  "kernel_module": V, "finished": "2026-10-12T16:00:00+02:00",
                                  "log_dir": "/x/gate", "exercised": ["vendor_extra/lib64/libvulkan_nouveau.so"]}}
        self.write()

    def set_running(self, version):
        touch(self.proc, PROC.format(version).encode())

    def write(self, **changes):
        import json
        manifest = dict(self.manifest, **changes)
        with open(os.path.join(self.root, "source-rm.json"), "w") as f:
            json.dump(manifest, f)

    def test_source_mounts_do_not_include_host_blobs(self):
        with mock.patch.object(nn, "_device_nodes", return_value=[]):
            nodes = nn.lxc_nodes(self.root)
        self.assertEqual(len(nodes), 7)
        self.assertFalse(any("/usr/lib" in n or "libEGL_nvidia" in n for n in nodes))
        self.assertTrue(any("vendor/lib64/egl" in n for n in nodes))

    def test_format2_returns_manifest(self):
        self.assertEqual(nn.verify_source_rm(self.root)["kernel_module"], V)

    def test_format1_refused_with_adopt_remedy(self):
        self.write(format=1)
        with self.assertRaises(OSError) as cm:
            nn._source_rm_nodes(self.root)
        msg = str(cm.exception)
        self.assertIn("format 1", msg)
        self.assertIn("adopt " + self.root, msg)

    def test_format1_accepted_only_for_adopt(self):
        manifest = dict(self.manifest, format=1)
        del manifest["gate"], manifest["kernel_module"]
        self.write(**manifest)
        os.unlink(self.proc)      # adopt's verification does not depend on the running module
        self.assertEqual(nn.verify_source_rm(self.root, formats=(1, 2))["format"], 1)

    def test_unknown_format_refused(self):
        self.write(format=4)
        self.assertRaisesRegex(OSError, "unsupported format 4", nn._source_rm_nodes, self.root)

    def test_kernel_mismatch_refused(self):
        self.set_running("615.78")
        with self.assertRaises(OSError) as cm:
            nn._source_rm_nodes(self.root)
        msg = str(cm.exception)
        for needle in ("gated on NVIDIA kernel module " + V, "loaded module is 615.78", "stage NEWROOT",
                       "activate NEWROOT", "restore BACKUP", nn.SOURCE_RM_BACKUPS):
            self.assertIn(needle, msg)

    def test_unreadable_proc_refused(self):
        os.unlink(self.proc)
        self.assertRaisesRegex(OSError, "cannot read .*proc-version", nn._source_rm_nodes, self.root)

    def test_gate_must_have_passed_on_the_pinned_module(self):
        for gate in (dict(self.manifest["gate"], exit=1), dict(self.manifest["gate"], kernel_module="615.78"),
                     dict(self.manifest["gate"], log_dir=""), "passed"):
            self.write(gate=gate)
            self.assertRaisesRegex(OSError, "manifest invalid", nn._source_rm_nodes, self.root)

    def test_pin_must_be_a_version(self):
        self.write(kernel_module="")
        self.assertRaisesRegex(OSError, "invalid kernel_module", nn._source_rm_nodes, self.root)

    def test_exercised_must_be_manifest_artifacts(self):
        self.write(gate=dict(self.manifest["gate"], exercised=["vendor_extra/lib64/libfoo.so"]))
        self.assertRaisesRegex(OSError, "gate.exercised", nn._source_rm_nodes, self.root)

    def test_changed_artifact_is_rejected(self):
        touch(os.path.join(self.root, "vendor_extra/lib64/libnv_bionic_native.so"), ELF[64] + b"changed")
        with self.assertRaisesRegex(OSError, "hash changed"):
            nn._source_rm_nodes(self.root)

    def test_mixed_proprietary_tree_is_rejected(self):
        touch(os.path.join(self.root, "vendor_extra/lib64/libnvglibc.so"), ELF[64])
        with self.assertRaisesRegex(OSError, "proprietary driver/shim"):
            nn._source_rm_nodes(self.root)


class NativeGlRecordTest(unittest.TestCase):
    """nvidia-prolapse + nvidia_native_gl = on: nvrm only from an enabled, hash-verified native_gl record."""

    def setUp(self):
        import hashlib
        import json
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        top = os.path.realpath(self._tmp.name)
        self.root = make_source_root(os.path.join(top, "root"))
        self.proc = os.path.join(top, "proc-version")
        touch(self.proc, PROC.format(V).encode())
        link = os.path.join(top, "nvidia-prolapse")
        os.symlink(self.root, link)
        for p in (mock.patch.object(nn, "PROC_VERSION", self.proc),
                  mock.patch.object(nn, "ROOT_LINKS", {"nvidia-prolapse": link, "glibc-shim": link + "-shim"})):
            p.start()
            self.addCleanup(p.stop)
        path = os.path.join(self.root, nn.SOURCE_RM_MANIFEST)
        with open(path) as f:
            self.manifest = json.load(f)
        self.manifest.update(format=3, media={"enabled": False, "reason": "test"})
        files = {}
        for relative in nn.native_gl_files(self.manifest):
            data = ELF[64] + relative.encode()
            touch(os.path.join(self.root, relative), data)
            files[relative] = hashlib.sha256(data).hexdigest()
        self.manifest["artifacts"].update(files)
        rust = self.manifest["artifacts"]["vendor_extra/lib64/libnv_bionic_native.so"]
        self.record = {"enabled": True, "egl": "nvrm", "files": files,
                       "gate": {"runner": "gl-android-smoke.py", "exit": 0, "kernel_module": V, "finished": "now",
                                "log_dir": "/l", "rust_sha256": {"x86_64": rust}}}
        self.write()

    def write(self, record="keep"):
        import json
        manifest = dict(self.manifest)
        if record == "keep":
            manifest["native_gl"] = self.record
        elif record is not None:
            manifest["native_gl"] = record
        with open(os.path.join(self.root, nn.SOURCE_RM_MANIFEST), "w") as f:
            json.dump(manifest, f)

    def mode(self):
        return nn.driver_mode({"nvidia_driver_mode": "nvidia-prolapse", "nvidia_native_gl": "on",
                               "nvidia_prolapse_install": "dev-root"})

    def refused(self, *needles):
        with self.assertRaises(OSError) as cm:
            self.mode()
        for needle in ("nvidia_native_gl = on is not available in nvidia-prolapse mode", nn.NATIVE_GL_STAGE) + needles:
            self.assertIn(needle, str(cm.exception))

    def test_record_present_selects_nvrm(self):
        dm = self.mode()
        self.assertEqual((dm.egl, dm.native_gl), ("nvrm", True))
        self.assertIn(("ro.hardware.egl", "nvrm"), nn.mode_properties(dm))
        self.assertIn("our own GLES driver nvrm", nn.announce(dm))
        nn.verify_source_rm(self.root)          # egl dir with ANGLE + nvrm files is accepted
        off = nn.driver_mode({"nvidia_driver_mode": "nvidia-prolapse", "nvidia_native_gl": "off",
                              "nvidia_prolapse_install": "dev-root"})
        self.assertEqual(off.egl, "angle")      # staged nvrm does not change the off default

    def test_record_absent_refused(self):
        for relative in nn.native_gl_files(self.manifest):
            os.unlink(os.path.join(self.root, relative))
            del self.manifest["artifacts"][relative]
        self.write(record=None)
        self.refused("has no native_gl record")
        nn.verify_source_rm(self.root)          # today's roots stay valid
        self.write(record={"enabled": False, "reason": "GL driver not built"})
        self.refused("explicitly not staged (GL driver not built)")
        nn.verify_source_rm(self.root)

    def test_hash_mismatch_refused(self):
        touch(os.path.join(self.root, "vendor_extra/lib64/libgallium_dri_nvrm.so"), ELF[64] + b"rebuilt")
        self.refused("native_gl file hash changed")
        self.assertRaisesRegex(OSError, "hash changed: .*libgallium_dri_nvrm.so", nn.verify_source_rm, self.root)

    def test_rust_library_mismatch_refused(self):
        self.record["gate"]["rust_sha256"] = {"x86_64": "0" * 64}
        self.write()
        self.refused("linked against libnv_bionic_native.so " + "0" * 64)
        self.assertRaisesRegex(OSError, "share one Rust RM library", nn.verify_source_rm, self.root)

    def test_record_must_list_exactly_the_abi_files(self):
        del self.record["files"]["vendor/lib64/egl/libGLESv1_CM_nvrm.so"]
        self.write()
        self.refused("native_gl record lists")

    def test_failed_or_foreign_gate_is_invalid(self):
        for gate in (dict(self.record["gate"], exit=1), dict(self.record["gate"], kernel_module="615.78")):
            self.write(record=dict(self.record, gate=gate))
            self.refused("native_gl.gate")

    def test_stray_nvrm_files_without_record_refused(self):
        self.write(record=None)
        self.assertRaisesRegex(OSError, "native GL is not staged, but nvrm files are present", nn.verify_source_rm,
                               self.root)

    def test_unknown_file_in_egl_dir_refused(self):
        touch(os.path.join(self.root, "vendor/lib64/egl/libEGL_mesa.so"), ELF[64])
        self.assertRaisesRegex(OSError, "must hold exactly", nn.verify_source_rm, self.root)


# ---------------------------------------------------------------- nvidia-prolapse vendor image

BUNDLE_SHA = "31d326fd" + "0" * 56
OUR_PROPS = dict(STOCK_PROPS, **{"ro.vendor.nvidia_prolapse.version": "v0.2.0-rc1",
                                 "ro.vendor.nvidia_prolapse.build_record": BUNDLE_SHA,
                                 "ro.vendor.build.fingerprint": "waydroid/lineage_nvidia_waydroid_x86_64/x:13/T/u:userdebug"})


def make_vendor_img(path, props):
    """A real ext4 image with /build.prop (debugfs reads it as from vendor.img); no root needed."""
    import subprocess
    src = tempfile.mkdtemp(dir=os.path.dirname(path))
    with open(os.path.join(src, "build.prop"), "w") as f:
        f.write("# test\n" + "".join("{}={}\n".format(k, v) for k, v in props.items()))
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", "-d", src, path, "2M"], check=True, capture_output=True)
    return path


class VendorImageTest(unittest.TestCase):
    """nvidia_prolapse_install = vendor-image: image identity, install record, nodes, refusals."""

    def setUp(self):
        import hashlib
        self._tmp = tempfile.TemporaryDirectory()
        self.top = self._tmp.name
        self.images = os.path.join(self.top, "images")
        os.makedirs(self.images)
        self.img = make_vendor_img(os.path.join(self.images, "vendor.img"), OUR_PROPS)
        self.state = os.path.join(self.top, "state")
        os.makedirs(self.state)
        self.xml = os.path.join(self.state, nn.VENDOR_IMAGE_XML)
        with open(self.xml, "w") as f:
            f.write("<!-- Generated by nvc2-xmlgen -->\n<MediaCodecs/>\n")
        self.dev = os.path.join(self.top, "dev")
        for node in ("nvidiactl", "nvidia-modeset", "nvidia0"):
            touch(os.path.join(self.dev, node))
        sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()
        self.record = {"format": 1, "version": "v0.2.0-rc1", "build_record_sha256": BUNDLE_SHA, "kernel_module": V,
                       "vendor_img": {"path": self.img, "sha256": sha(self.img)},
                       "gate": {"exit": 0, "kernel_module": V, "log_dir": "/x"}, "native_gl": True,
                       "media": {"xml_sha256": sha(self.xml)}}
        self.write_record()
        self.access = []
        for attr, value in (("VENDOR_IMAGE_STATE", self.state), ("DEV_DIR", self.dev),
                            ("PACKAGED_BUNDLE_RECORD", os.path.join(self.top, "no-package")),
                            ("driver_version", lambda *a: V), ("nvidia_render_node", lambda *a: RENDER),
                            ("check_media_device_access", self.access.append)):
            p = mock.patch.object(nn, attr, value)
            p.start()
            self.addCleanup(p.stop)
        self.cfg = {"nvidia_driver_mode": "nvidia-prolapse", "nvidia_prolapse_install": "vendor-image",
                    "nvidia_native_gl": "off", "images_path": self.images}

    def tearDown(self):
        self._tmp.cleanup()

    def write_record(self, **change):
        record = dict(self.record, **change)
        with open(os.path.join(self.state, nn.VENDOR_IMAGE_RECORD), "w") as f:
            json.dump(record, f)

    def nodes(self, **cfg):
        w = dict(self.cfg, **cfg)
        return nn.mode_nodes(nn.driver_mode(w), w)

    def refused(self, *needles, **cfg):
        with self.assertRaises(OSError) as cm:
            self.nodes(**cfg)
        for needle in needles:
            self.assertIn(needle, str(cm.exception))
        return str(cm.exception)

    def test_driver_mode(self):
        dm = nn.driver_mode(self.cfg)
        self.assertEqual((dm.mode, dm.install, dm.root, dm.link, dm.egl), ("nvidia-prolapse", "vendor-image", None,
                                                                            None, "angle"))
        self.assertTrue(dm.vendor_image and dm.native and not dm.guest_layout)
        self.assertEqual(nn.driver_mode(dict(self.cfg, nvidia_native_gl="on")).egl, "nvrm")
        self.assertIn("vendor image", nn.announce(dm))
        props = dict(nn.mode_properties(dm))
        self.assertEqual((props["ro.hardware.vulkan"], props["ro.hardware.egl"], props["debug.stagefright.ccodec"]),
                         ("nvidia", "angle", "4"))

    def test_install_key_is_explicit(self):
        for w, needle in (({"nvidia_driver_mode": "nvidia-prolapse"}, "needs nvidia_prolapse_install"),
                          ({"nvidia_driver_mode": "nvidia-prolapse", "nvidia_prolapse_install": "image"}, "is not one of"),
                          ({"nvidia_driver_mode": "venus", "nvidia_prolapse_install": "vendor-image"},
                           "belongs to nvidia_driver_mode = nvidia-prolapse")):
            with self.subTest(w=w):
                with self.assertRaises(OSError) as cm:
                    nn.driver_mode(w)
                self.assertIn(needle, str(cm.exception))

    def test_nodes_are_devices_and_the_codec_list_only(self):
        nodes = self.nodes()
        xml = "lxc.mount.entry = {} vendor/odm/etc/media_codecs_c2.xml none bind,ro 0 0".format(self.xml)
        self.assertEqual(nodes[-1], xml)
        self.assertEqual(sorted(e.split()[2] for e in nodes[:-1]),
                         sorted([os.path.join(self.dev, n) for n in ("nvidiactl", "nvidia-modeset", "nvidia0")] + [RENDER]))
        self.assertFalse([e for e in nodes if "vendor_extra" in e or "nv/guest" in e or "/egl" in e])
        self.assertEqual(len(self.access), 1)          # the Codec2 service's device access

    def test_stock_image_refused_with_setup_remedy(self):
        make_vendor_img(self.img, STOCK_PROPS)
        self.refused("is not the nvidia-prolapse vendor image", "lineage_waydroid_x86_64", "waydroid init -f",
                     nn.SETUP)

    def test_record_missing_or_mismatched(self):
        os.unlink(os.path.join(self.state, nn.VENDOR_IMAGE_RECORD))
        self.refused("never gated", nn.SETUP)
        self.write_record(build_record_sha256="f" * 64)
        self.refused("carries release bundle 31d326fd0000", nn.SETUP)
        self.write_record(vendor_img={"path": self.img, "sha256": "0" * 64})
        self.refused("not the file the setup gated", nn.SETUP)
        self.write_record(vendor_img={"path": "/elsewhere/vendor.img", "sha256": self.record["vendor_img"]["sha256"]})
        self.refused("Waydroid boots " + self.img)
        self.write_record(gate={"exit": 1, "kernel_module": V, "log_dir": "/x"})
        self.refused("is invalid", "passing gate")

    def test_kernel_module_pin(self):
        self.write_record(kernel_module="610.57", gate={"exit": 0, "kernel_module": "610.57", "log_dir": "/x"})
        self.refused("gated on NVIDIA kernel module 610.57, the loaded module is " + V, "regenerates the codec list")

    def test_package_bundle_must_match(self):
        other = os.path.join(self.top, "BUILD-RECORD.json")
        touch(other, b"{}")
        with mock.patch.object(nn, "PACKAGED_BUNDLE_RECORD", other):
            self.refused("the installed package carries release bundle", "package was updated", nn.SETUP)

    def test_codec_list(self):
        with open(self.xml, "a") as f:
            f.write("<!-- edited -->\n")
        self.refused("is not the nvc2-xmlgen output")
        os.unlink(self.xml)
        self.refused("Codec2 listing", "is missing", nn.SETUP)

    def test_nvrm_needs_its_gate(self):
        self.assertTrue(self.nodes(nvidia_native_gl="on"))
        self.write_record(native_gl=False)
        self.refused("no passing nvrm gate", nvidia_native_gl="on")

    def test_other_modes_refuse_our_image(self):
        for w in ({"nvidia_driver_mode": "venus"}, {"nvidia_driver_mode": "glibc-shim"},
                  {"nvidia_driver_mode": "nvidia-prolapse", "nvidia_prolapse_install": "dev-root"}):
            with self.subTest(w=w), mock.patch.object(nn, "root_link", lambda m: ("/l", "/r")), \
                    mock.patch.object(nn, "_prolapse_gl", lambda r: "nvrm"), \
                    mock.patch.object(nn.os.path, "exists", lambda p: p.endswith(nn.SOURCE_RM_MANIFEST)
                                      if w.get("nvidia_prolapse_install") else False):
                dm = nn.driver_mode(w)
                with self.assertRaises(OSError) as cm:
                    nn.refuse_prolapse_image(dm, dict(w, images_path=self.images))
                self.assertIn("runs on the official vendor image", str(cm.exception))
                self.assertIn(nn.RESTORE_OFFICIAL, str(cm.exception))
        make_vendor_img(self.img, STOCK_PROPS)
        nn.refuse_prolapse_image(nn.driver_mode({"nvidia_driver_mode": "venus"}), {"images_path": self.images})

    # ---- `waydroid init` (init_nodes) and container start (check_start)
    def test_init_defers_a_stock_image(self):
        make_vendor_img(self.img, STOCK_PROPS)
        entries, pending = nn.init_nodes(nn.driver_mode(self.cfg), self.cfg)
        self.assertEqual(entries, [])
        for needle in ("is not the nvidia-prolapse vendor image", "lineage_waydroid_x86_64", nn.SETUP,
                       "'waydroid init' installs official images only"):
            self.assertIn(needle, pending)
        self.assertEqual(self.access, [])               # no device was looked at

    def test_init_defers_every_vendor_image_refusal(self):
        os.unlink(os.path.join(self.state, nn.VENDOR_IMAGE_RECORD))
        entries, pending = nn.init_nodes(nn.driver_mode(self.cfg), self.cfg)
        self.assertEqual(entries, [])
        self.assertIn("never gated", pending)

    def test_init_with_the_gated_image_emits_the_nodes(self):
        entries, pending = nn.init_nodes(nn.driver_mode(self.cfg), self.cfg)
        self.assertIsNone(pending)
        self.assertEqual(entries, self.nodes())

    def test_init_keeps_the_other_refusals(self):
        """Only the vendor-image install defers; venus on our image still refuses at init."""
        w = {"nvidia_driver_mode": "venus", "images_path": self.images}
        with self.assertRaises(OSError) as cm:
            nn.init_nodes(nn.driver_mode(w), w)
        self.assertIn("runs on the official vendor image", str(cm.exception))

    def test_start_check(self):
        dm = nn.driver_mode(self.cfg)
        config_nodes = os.path.join(self.top, "config_nodes")
        with open(config_nodes, "w") as f:
            f.write("lxc.mount.entry = tmpfs mnt tmpfs nodev 0 0\n" + "".join(e + "\n" for e in self.nodes()))
        nn.check_start(dm, self.cfg, config_nodes)
        with open(config_nodes, "w") as f:              # what init wrote before the setup installed the image
            f.write("lxc.mount.entry = tmpfs mnt tmpfs nodev 0 0\n")
        with self.assertRaises(OSError) as cm:
            nn.check_start(dm, self.cfg, config_nodes)
        self.assertIn("generated while the vendor image was not installed", str(cm.exception))
        self.assertIn(nn.SETUP, str(cm.exception))
        make_vendor_img(self.img, STOCK_PROPS)          # 'waydroid init -f' put the official image back
        with self.assertRaises(OSError) as cm:
            nn.check_start(dm, self.cfg, config_nodes)
        self.assertIn("is not the nvidia-prolapse vendor image", str(cm.exception))
        self.assertIn(nn.SETUP, str(cm.exception))
        with self.assertRaises(OSError):
            nn.check_start(dm, self.cfg, os.path.join(self.top, "absent"))
        nn.check_start(nn.driver_mode({"nvidia_driver_mode": "venus"}), {}, os.path.join(self.top, "absent"))


class VendorImageLxcTest(LxcIntegrationTest):
    """generate_nodes_lxc_config()/make_base_props() of build/waydroid with the vendor-image install."""

    def setUp(self):
        super().setUp()
        self.vi = VendorImageTest("test_driver_mode")
        self.vi.setUp()
        self.addCleanup(self.vi.tearDown)
        for attr, value in (("VENDOR_IMAGE_STATE", self.vi.state), ("check_media_device_access", lambda paths: None),
                            ("PACKAGED_BUNDLE_RECORD", os.path.join(self.top, "no-package")),
                            ("image_build_props", nn.image_build_props)):
            p = mock.patch.object(self.module, attr, value)
            p.start()
            self.addCleanup(p.stop)
        self.write_cfg({"nvidia_driver_mode": "nvidia-prolapse", "nvidia_prolapse_install": "vendor-image",
                        "nvidia_native_gl": "off", "drm_device": RENDER, "images_path": self.vi.images},
                       {"debug.hwui.renderer": "skiavk"})

    def test_vendor_image_mounts(self):
        nodes = self.nodes()
        self.assertFalse([e for e in nodes if "nv/guest" in e or "vendor_extra" in e or "surfaceflinger" in e
                          or "hwcomposer" in e or "/egl" in e or "dev/venus" in e])
        self.assertIn("lxc.mount.entry = {} vendor/odm/etc/media_codecs_c2.xml none bind,ro 0 0".format(self.vi.xml),
                      nodes)
        self.assertTrue([e for e in nodes if e.split()[2] == os.path.join(self.dev, "nvidiactl")])

    def test_vendor_image_base_props(self):
        lines, props = self.base_props()
        for k, v in (("ro.hardware.vulkan", "nvidia"), ("ro.hardware.egl", "angle"),
                     ("ro.zygote.disable_gl_preload", "true"), ("debug.stagefright.ccodec", "4")):
            self.assertEqual(props.get(k), v, k)

    def test_stock_image_fails_upgrade(self):
        make_vendor_img(os.path.join(self.vi.images, "vendor.img"), STOCK_PROPS)
        self.refused("is not the nvidia-prolapse vendor image", "sudo waydroid-nvidia-setup")

    def test_fresh_init_on_the_official_image_then_setup(self):
        """The first install: `waydroid init` (run by waydroid-nvidia-setup, or by hand) writes the default mode
        (vendor-image) while vendor.img is the official image it just downloaded. init completes and says what is
        next; the container start refuses with the setup as the remedy until the setup installed the vendor image
        and regenerated the entries (`waydroid upgrade -o`); then it starts."""
        import shutil
        import tools.actions.initializer as initializer
        import tools.config
        helpers = initializer.helpers
        ours = os.path.join(self.top, "ours.img")
        shutil.copy2(self.vi.img, ours)                       # the gated image, as the setup will install it
        make_vendor_img(self.vi.img, STOCK_PROPS)             # what init downloads: the official vendor image
        os.unlink(self.args.config)                           # not initialized
        lxc_dir = os.path.join(self.top, "lxc")
        config_nodes = os.path.join(lxc_dir, "waydroid", "config_nodes")

        def set_lxc_config(args):                             # upstream's tail: generate, then write config_nodes
            nodes = self.lxc.generate_nodes_lxc_config(args)
            os.makedirs(os.path.dirname(config_nodes), exist_ok=True)
            with open(config_nodes, "w") as f:
                f.writelines(n + "\n" for n in nodes)

        started = []
        self.args.__dict__.update(force=False, images_path=self.vi.images, system_channel=None, vendor_channel=None,
                                  rom_type=None, system_type="GAPPS", log=os.path.join(self.top, "log"))
        with mock.patch.dict(tools.config.defaults, {"lxc": lxc_dir, "rootfs": os.path.join(self.top, "rootfs"),
                                                     "overlay": os.path.join(self.top, "overlay"),
                                                     "overlay_rw": os.path.join(self.top, "overlay_rw"),
                                                     "preinstalled_images_paths": []}), \
                mock.patch.object(helpers.arch, "host", lambda: "x86_64"), \
                mock.patch.object(helpers.props, "host_get", lambda args, prop: ""), \
                mock.patch.object(helpers.drivers, "setupBinderNodes", lambda args: None), \
                mock.patch.object(helpers.drivers, "probeAshmemDriver", lambda args: None), \
                mock.patch.object(helpers.http, "retrieve",      # no device codename channel, MAINLINE's exists
                                  lambda url, headers=None: (404, b"") if url.endswith("/.json") else (200, b"")), \
                mock.patch.object(helpers.images, "get", lambda args: None), \
                mock.patch.object(helpers.lxc, "setup_host_perms", lambda args: None), \
                mock.patch.object(helpers.lxc, "set_lxc_config", set_lxc_config), \
                mock.patch.object(helpers.gpu, "getDriNode", lambda args: (RENDER, "")), \
                mock.patch.object(helpers.gpu, "getVulkanDriver", lambda args, node: ""), \
                mock.patch.object(helpers.run, "user", lambda *a, **k: started.append(a)), \
                mock.patch.object(self.lxc, "wait_for_running", lambda args: None):
            with self.assertLogs(level="INFO") as logs:
                initializer.init(self.args)
            # init completed: the default mode in the cfg, the mode's properties, the deferred entries
            waydroid, _ = nn.load_cfg(self.args.config)
            self.assertEqual((waydroid["nvidia_driver_mode"], waydroid["nvidia_prolapse_install"]),
                             ("nvidia-prolapse", "vendor-image"))
            self.assertTrue(waydroid["system_ota"].endswith("/GAPPS.json"))
            with open(config_nodes) as f:
                nodes = f.read()
            self.assertNotIn("nvidia", nodes)
            self.assertNotIn("media_codecs_c2.xml", nodes)
            with open(os.path.join(self.top, "waydroid_base.prop")) as f:
                self.assertIn("ro.hardware.vulkan=nvidia\n", f.read())
            warnings = [r.getMessage() for r in logs.records if r.levelname == "WARNING"]
            self.assertEqual(len(warnings), 2, warnings)
            self.assertIn("is not the nvidia-prolapse vendor image", warnings[0])
            self.assertEqual(logs.records[-1].getMessage(), nn.INIT_NEXT_STEP)    # init ends with the next step
            self.assertTrue(nn.INIT_NEXT_STEP.endswith("sudo waydroid-nvidia-setup"))
            # container start: refused, lxc-start never runs
            self.refused("is not the nvidia-prolapse vendor image", "Remedy: sudo waydroid-nvidia-setup",
                         call=lambda: self.lxc.start(self.args))
            self.assertEqual(started, [])
            # the setup installs the gated image ...
            shutil.copy2(ours, self.vi.img)
            self.refused("generated while the vendor image was not installed", "sudo waydroid-nvidia-setup",
                         call=lambda: self.lxc.start(self.args))
            self.assertEqual(started, [])
            # ... and runs `waydroid upgrade -o` (no init flag: the generator's strict path)
            self.args.nvidia_init = False
            set_lxc_config(self.args)
            self.lxc.start(self.args)
            self.assertEqual(len(started), 1)
            self.assertEqual(started[0][1][:2], ["lxc-start", "-P"])


for _name in [n for n in dir(LxcIntegrationTest) if n.startswith("test_")]:   # only its setUp/helpers are reused
    if _name not in VendorImageLxcTest.__dict__:
        setattr(VendorImageLxcTest, _name, None)


if __name__ == "__main__":
    unittest.main()
