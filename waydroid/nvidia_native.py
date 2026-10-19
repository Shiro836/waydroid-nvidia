#!/usr/bin/env python3
# waydroid-nvidia-prolapse: LXC mount entries and property rules for NVIDIA-native mode.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installed into the waydroid tree as tools/helpers/nvidia_native.py and called from
# tools/helpers/lxc.py (generate_nodes_lxc_config, make_base_props, start),
# tools/helpers/images.py (make_prop: per-session display properties), tools/actions/initializer.py (init_defaults;
# init_nodes through generate_nodes_lxc_config) and tools/actions/session_manager.py. The driver is chosen explicitly
# in waydroid.cfg:
#
#     [waydroid]
#     nvidia_driver_mode = nvidia-prolapse | glibc-shim | venus
#     nvidia_native_gl = off | on                                            (default off = ANGLE)
#     nvidia_prolapse_install = vendor-image | dev-root                      (nvidia-prolapse only; required)
#
# nvidia-prolapse has two install shapes, named explicitly (INSTALL_KEY):
#  * vendor-image (what users get, waydroid-nvidia-setup): every guest component (Vulkan HAL, NVK, Rust RM,
#    nvrm, gralloc, hwcomposer, ANGLE, Codec2 service) is a vendor module of OUR vendor.img, built from
#    source (vendor-image/), installed next to the official system.img. The generator binds no driver file:
#    only the device nodes and this GPU's media_codecs_c2.xml (nvc2-xmlgen output, VENDOR_IMAGE_STATE) over
#    the image's placeholder /odm/etc/media_codecs_c2.xml. The image is recognised positively by its build
#    props (VERSION_PROP, BUILD_RECORD_PROP) and must match the record the setup wrote after gating it on
#    the loaded kernel module (check_vendor_image()); a stock image refuses with the setup as the remedy.
#    `waydroid init` only installs official images and runs before the setup has installed ours, so at init that
#    refusal is deferred (init_nodes(): no mode entries, the next step logged) to the container start (check_start()).
#  * dev-root (developers): a staged source root bind-mounted over the official vendor image (below).
# glibc-shim, venus and dev-root run on the official vendor image and refuse ours (refuse_prolapse_image()).
#
# Paths are fixed conventions, not keys (ROOT_LINKS, VENUS_SOCKET, VENDOR_IMAGE_STATE):
#     /var/lib/waydroid/nvidia-prolapse -> the active source NVK + Rust RM root (dev-root; dev/source-native activate)
#     /var/lib/waydroid/nvidia-glibc-shim  -> the active blob staging root (dev/native-deploy)
#     /run/waydroid-venus/venus.sock          the host Venus render server (wd-venus.service)
# The links are resolved here; config_nodes carries the real target paths.
#
# Mode rules (driver_mode(); enforced in lxc.py and session_manager.py by the same patch):
#
#  * nvidia_driver_mode is required as soon as any nvidia_* key is set; no mode is inferred.
#    nvidia-prolapse = source NVK + Rust RM root (source-rm.json); glibc-shim = NVIDIA's own userspace
#    through our glibc shims (blob staging root); venus = waydroid-nvidia's Venus proxy, exactly the
#    baseline's mounts and socket precheck. A switch changes one key.
#    The retired path keys (nvidia_native_root, nvidia_rust_driver_root, nvidia_glibc_shim_root,
#    nvidia_venus_socket) are refused; `dev/driver-mode set MODE` strips them (no compatibility path).
#  * nvidia_native_gl = on selects the mode's own GLES: glibc-shim -> NVIDIA's GLES
#    (ro.hardware.egl = nvnative); nvidia-prolapse -> our gallium driver nvrm over the Rust RM
#    (ro.hardware.egl = nvrm), only when the root's source-rm.json has an enabled, hash-verified
#    native_gl record (_prolapse_gl); venus has none: on is refused there. A source root's egl
#    directory holds ANGLE's three files plus, when native_gl is staged, lib*_nvrm.so (and
#    vendor_extra/<lib>/libgallium_dri_nvrm.so); anything else is refused.
#  * Kept from the Venus build (waydroid-nvidia's guest layout 2, /var/lib/waydroid/nv/guest):
#    ANGLE (vendor/lib{,64}/egl/*_angle.so), hwcomposer.waydroid.so, system/bin/surfaceflinger.
#    glibc-shim and dev-root therefore need nvidia_guest_layout = 2 (the vendor-image install binds nothing
#    from it: ANGLE and hwcomposer are in the image, SurfaceFlinger is the system image's). In a blob root ANGLE's three files per
#    ABI are bound by this module (after the staged egl directory, below), not by lxc.py's loop.
#  * Not mounted in the native modes: the dev/venus socket dir, vendor/lib{,64}/hw/vulkan.virtio.so
#    and the Venus build's libgbm_mesa_wrapper.so; `session start` checks the socket in venus only.
#  * Every mode-derived property (DERIVED_PROPERTIES: ro.hardware.vulkan, ro.hardware.egl,
#    ro.zygote.disable_gl_preload, mesa.vn.debug, mesa.vtest.socket.name and the Codec2 listing
#    properties) is written by lxc.py make_base_props() from mode_properties(); a waydroid.cfg
#    [properties] entry for any of them is refused (check_properties()): [properties] is applied
#    last and would silently override the mode. lxc.py start() announces the mode and refuses a
#    waydroid_base.prop generated for another mode (check_base_prop()).
#
# What this module mounts, and why there:
#
#  * <root>/vendor_extra  ->  /vendor_extra      (the whole staging tree, rbind)
#    LineageOS 20's generated linker config (linkerconfig, /linkerconfig/ld.config.txt in the
#    booted guest) lists /vendor_extra/${LIB}, /vendor_extra/${LIB}/egl and
#    /vendor_extra/${LIB}/hw as search paths of the "sphal" namespace (what libvulkan and the
#    EGL loader use for vendor drivers) and of the vendor default namespace, and /vendor_extra
#    as a permitted path. /vendor_extra is an empty directory in system.img for exactly this
#    use. So: no ld.config edit, no overlayfs (broken on some hosts), no bind over files in
#    the read-only vendor.img. The staging tree (dev/native-deploy) holds:
#      lib64/libnvglibc.so + the six glibc-named fronts   glibc-compat shims
#      lib64/host-glibc-version                           sidecar read by the shims
#      lib64/libEGL_nvidia.so.0 -> libEGL_nvidia.so.<v>   the ICD's soname
#      lib64/<blob>.<v>                                   empty placeholders (see next item)
#      lib64/hw/vulkan.nvidia.so                          our Vulkan HAL; libvulkan finds it
#                                                         as hw/vulkan.<ro.hardware.vulkan>.so
#                                                         through the rbind, no own mount
#      lib64/libgbm_mesa_wrapper.so                       our gralloc backend (next-but-one item)
#      lib64/libminigbm_gralloc_gbm_mesa.so               minigbm's gbm_mesa core, rebuilt with
#                                                         our patch (item after the backend)
#      lib64/libdrm.so.2                                  upstream libdrm built for bionic
#                                                         (nvidia-prolapse/libdrm)
#    libdrm.so.2 is required, not a nicety. Measured on the host (driver 615.71.09): NVIDIA's
#    ICD advertises VK_EXT_external_memory_dma_buf only when dlopen("libdrm.so.2") succeeds in
#    its own namespace AND the NVIDIA render node is present (vkcaps with libdrm.so.2 denied
#    via LD_AUDIT: 290 device extensions, no dma_buf; loadable: 291). Without dma_buf the HAL
#    cannot offer VK_ANDROID_native_buffer, so no app gets VK_KHR_swapchain. Android ships its
#    libdrm only as an unversioned libdrm.so, so the staging tree carries one under the name the
#    driver asks for, in the sphal search path next to the ICD.
#
#  * /usr/lib/<blob>.<driver>  ->  /vendor_extra/lib64/<blob>.<driver>   (one bind per file)
#    NVIDIA's own userspace, straight from the host install, read-only, unmodified. The
#    version is read from the running kernel module (/proc/driver/nvidia/version), never
#    typed, because the RM refuses a userspace/kernel mismatch and we want that refusal to
#    happen *here*, with a file name, not inside an app.
#
#  * <root>/vendor_extra/lib64/libgbm_mesa_wrapper.so  ->  /vendor/lib64/libgbm_mesa_wrapper.so
#    Stock minigbm (gralloc.minigbm_gbm_mesa) loads its backend from the vendor image; the
#    file exists in vendor.img, so this is a plain bind over it (no create=file). Binding over
#    the stock path keeps the result independent of the vendor namespace's search order.
#
#  * <root>/vendor_extra/lib64/libminigbm_gralloc_gbm_mesa.so  ->  /vendor/lib64/libminigbm_gralloc_gbm_mesa.so
#    minigbm's gbm_mesa core (allocator service, 64-bit mapper and gralloc0 module NEED it), built
#    from the pinned minigbm 16844ba + nvidia-prolapse/gralloc/minigbm/0001-*.patch: CPU usage no
#    longer forces LINEAR on GPU render targets, so a CPU-mapped render target (SurfaceFlinger's
#    screenshot buffer, usage 0xb33) is block-linear, which NVIDIA's GLES can render into; the
#    backend's lock/unlock give the CPU a linear copy. Measured 2026-10-11: with the image's
#    library SurfaceFlinger on nvnative aborts at its first screenshot (LINEAR render target).
#    Plain bind over the vendor.img file. Patch 0002 passes each lock's access flags
#    (BO_MAP_READ/WRITE) to the backend, so a read lock never writes a block-linear buffer back.
#
#  * <root>/vendor_extra/lib/libminigbm_gralloc_gbm_mesa.so  ->  /vendor/lib/libminigbm_gralloc_gbm_mesa.so
#    The same core built for 32-bit x86 (the image's 32-bit mapper and gralloc0 module NEED it): it
#    allocates nothing, but without it 32-bit locks reach the backend without access flags.
#
#  * <root>/vendor_extra/lib/libgbm_mesa_wrapper.so  ->  /vendor/lib/libgbm_mesa_wrapper.so
#    The same backend built for 32-bit x86. The image's 32-bit processes (mediaserver and its
#    software codecs, 32-bit apps) run minigbm's 32-bit *mapper*, which imports and mmaps
#    gralloc buffers in-process through this library; without the bind they map NVIDIA dma-bufs
#    through the stock Mesa gbm wrapper on whatever render node gralloc.gbm.device names.
#    Measured 2026-10-10: mediaserver (x86) SIGBUS in SoftwareRenderer::render writing a
#    decoded frame into such a mapping, "Can't play this video" for every video. Mapping a
#    LINEAR buffer needs no NVIDIA library; a CPU lock of a block-linear (CPU-mapped render
#    target) buffer brings up the 32-bit ICD in that process for the staging copy. Allocation
#    stays in the 64-bit allocator service.
#
#  * <root>/vendor/lib64/egl  ->  /vendor/lib64/egl   (directory bind over the image's; same for lib)
#    Android 13's EGL loader looks for lib{EGL,GLESv1_CM,GLESv2}_<ro.hardware.egl>.so only in
#    /vendor/lib{,64}/egl and /system/lib{,64}/egl (frameworks/native android13-release
#    opengl/libs/EGL/Loader.cpp:390-397) and loads them by absolute path into the sphal namespace.
#    The staged directory holds exactly two drivers, so ro.hardware.egl switches between them with
#    a container restart (A/B):
#      lib{EGL,GLESv1_CM,GLESv2}_nvnative.so   NVIDIA's own EGL/GLES through our front
#                                              (nvidia-prolapse/egl), real files: a redeploy as a
#                                              new inode reaches every process started afterwards
#      lib{EGL,GLESv1_CM,GLESv2}_angle.so      empty placeholders; each gets a file bind of ANGLE
#                                              from /var/lib/waydroid/nv/guest/vendor/<lib>/egl
#                                              (emitted right after the directory bind, which
#                                              would otherwise hide them)
#    The image's Mesa files (lib*_mesa.so) are left out: Mesa has no driver for NVIDIA here. Any
#    other file in the directory is refused, so the loader's choice is always one of the two.
#    The front's platform library sits next to the shims, where the driver's lookup finds it:
#    vendor_extra/<lib>/libnvnative-egl-android.so, nvnative_egl_android.json (names it by
#    soname) and nvglibc-default-env (the shims' default environment, which points
#    __EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES at the JSON). Both drivers' files are mandatory in
#    both ABIs whichever one is selected.
#
#  * /dev/nvidiactl, /dev/nvidia<N>, /dev/nvidia-modeset, and the NVIDIA render node
#    (selected by PCI vendor 0x10de in sysfs, never by renderD number).
#
#  * 32-bit x86 (P6): the image's 32-bit processes (32-bit apps, ARM32 apps through houdini's x86
#    path, mediaserver) get the same stack under vendor_extra/lib, which LineageOS 20's linker
#    config searches as /vendor_extra/${LIB} for them: the i386 shims (ld-linux.so.2 instead of
#    ld-linux-x86-64.so.2), host-glibc-version, the libEGL_nvidia.so.0 soname link, placeholders,
#    hw/vulkan.nvidia.so, libdrm.so.2 (all built for i686), and the five i386 driver libraries
#    bind-mounted from /usr/lib32 (lib32-nvidia-utils) at the kernel module's version. Mandatory:
#    without it a 32-bit app's ANGLE finds no Vulkan device and every 32-bit GLES app fails.
#
# Nothing is 'optional': a missing piece fails `waydroid upgrade -o` with the entry's name
# instead of surfacing as an opaque failure deep inside the guest (project principle: no
# silent fallbacks). Every check here raises OSError with the remedy.
import glob
import sys
import json
import hashlib
import os
import re

HOST_LIBDIR = "/usr/lib"
HOST_LIBDIR32 = "/usr/lib32"
DEV_DIR = "/dev"
PROC_VERSION = "/proc/driver/nvidia/version"
NVIDIA_VENDOR = "0x10de"
DEPLOY = "waydroid-nvidia-prolapse-setup, or dev/native-deploy in a dev checkout"
# The minimal ICD closure (from the libraries' DT_NEEDED and dlopen lists): libEGL_nvidia.so.0 *is* the
# Vulkan ICD; glsi/gpucomp are DT_NEEDED, eglcore/glvkspirv are dlopened by the driver.
BLOBS = (
    "libEGL_nvidia.so.{v}",
    "libnvidia-glsi.so.{v}",
    "libnvidia-eglcore.so.{v}",
    "libnvidia-gpucomp.so.{v}",
    "libnvidia-glvkspirv.so.{v}",
)
SHIMS = (
    "libnvglibc.so", "libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2",
    "ld-linux-x86-64.so.2",
)
SHIMS32 = (
    "libnvglibc.so", "libc.so.6", "libm.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2",
    "ld-linux.so.2",
)
GUEST_LIBDIR = "vendor_extra/lib64"
GUEST_LIBDIR32 = "vendor_extra/lib"
HAL = "hw/vulkan.nvidia.so"
GBM_WRAPPER = "libgbm_mesa_wrapper.so"
GBM_WRAPPER_GUEST = "vendor/lib64/" + GBM_WRAPPER
GBM_WRAPPER_GUEST32 = "vendor/lib/" + GBM_WRAPPER
MINIGBM_CORE = "libminigbm_gralloc_gbm_mesa.so"
MINIGBM_CORE_GUEST = "vendor/lib64/" + MINIGBM_CORE
MINIGBM_CORE_GUEST32 = "vendor/lib/" + MINIGBM_CORE
LIBDRM = "libdrm.so.2"
# EGL/GLES driver selection (ro.hardware.egl): the staged /vendor/lib{,64}/egl directory.
EGL_DRIVERS = ("nvnative", "angle")
EGL_LIBS = ("libEGL_{}.so", "libGLESv1_CM_{}.so", "libGLESv2_{}.so")
EGL_FRONT = tuple(n.format("nvnative") for n in EGL_LIBS)
EGL_ANGLE = tuple(n.format("angle") for n in EGL_LIBS)
EGL_STAGE = "vendor"                       # <root>/vendor/<lib>/egl -> guest vendor/<lib>/egl
ANGLE_GUEST_ROOT = "/var/lib/waydroid/nv/guest"
EGL_PLATFORM = "libnvnative-egl-android.so"
EGL_PLATFORM_JSON = "nvnative_egl_android.json"
DEFAULT_ENV = "nvglibc-default-env"
EGL_BUILD = "build: make -C nvidia-prolapse/egl, then " + DEPLOY


# ---------------------------------------------------------------- driver mode (waydroid.cfg)

MODE_KEY = "nvidia_driver_mode"
MODES = ("nvidia-prolapse", "glibc-shim", "venus")
# What `waydroid init` writes into a cfg that has no NVIDIA key yet (init_defaults): explicit keys in the
# cfg, announced in the log, never an implicit runtime default. Native GL (nvrm) is an explicit opt-in.
DEFAULT_MODE = "nvidia-prolapse"
DEFAULT_NATIVE_GL = "off"
GL_KEY = "nvidia_native_gl"
LAYOUT_KEY = "nvidia_guest_layout"
# nvidia-prolapse install shape (module header). What `waydroid init` writes with the default mode.
INSTALL_KEY = "nvidia_prolapse_install"
INSTALLS = ("vendor-image", "dev-root")
DEFAULT_INSTALL = "vendor-image"
SETUP = "sudo waydroid-nvidia-setup"
# Fixed conventions, the one place they are defined (read at call time: tests point them elsewhere).
ROOT_LINKS = {"nvidia-prolapse": "/var/lib/waydroid/nvidia-prolapse",
              "glibc-shim": "/var/lib/waydroid/nvidia-glibc-shim"}
VENUS_SOCKET = "/run/waydroid-venus/venus.sock"
# Default glibc-shim staging tree written by dev/native-deploy (the glibc-shim link points at it).
GLIBC_SHIM_STAGING = "/var/lib/waydroid/nvidia-glibc-shim-staging"
# The privileged helper (packaging/native/wd-native, one NOPASSWD sudoers rule): the packaged copy, or a
# developer's root-owned copy; WD_NATIVE_HELPER overrides. Only used in messages here.
WD_NATIVE_HELPERS = ("/usr/lib/waydroid-nvidia/wd-native", "/usr/local/sbin/wd-native")


def wd_native_helper():
    """Path of the wd-native helper to name/run: $WD_NATIVE_HELPER, else the first installed default."""
    if os.environ.get("WD_NATIVE_HELPER"):
        return os.environ["WD_NATIVE_HELPER"]
    return next((h for h in WD_NATIVE_HELPERS if os.path.exists(h)), WD_NATIVE_HELPERS[0])


LINK_SETTERS = {"nvidia-prolapse": "dev/source-native stage NEWROOT, then sudo -n wd-native activate NEWROOT (a "
                                   "developer checkout; packaged installs use nvidia_prolapse_install = vendor-image: "
                                   "sudo waydroid-nvidia-setup)",
                "glibc-shim": "sudo dev/native-deploy (it repoints the link after its generator check)"}
# Path keys of earlier configurations; any of them in [waydroid] is refused (dev/driver-mode set strips them).
LEGACY_ROOT_KEY = "nvidia_native_root"
# Mode values of earlier configurations: refused with the new name, never aliased (the link moved too).
RENAMED_MODES = {"rust-driver": ("nvidia-prolapse", "/var/lib/waydroid/nvidia-rust-driver")}
RETIRED_KEYS = (LEGACY_ROOT_KEY, "nvidia_rust_driver_root", "nvidia_glibc_shim_root", "nvidia_venus_socket")
NVIDIA_KEYS = (MODE_KEY, GL_KEY, LAYOUT_KEY, INSTALL_KEY) + RETIRED_KEYS
CFG_PATH = "/var/lib/waydroid/waydroid.cfg"
SWITCH_TOOL = "dev/driver-mode set MODE [--native-gl on|off] (waydroid-nvidia checkout)"
GL_NAMES = {"angle": "ANGLE on the Vulkan HAL", "nvnative": "NVIDIA's own GLES through our EGL front",
            "nvrm": "our own GLES driver nvrm over the Rust RM"}


class DriverMode:
    """The NVIDIA driver selection of one waydroid.cfg, validated (driver_mode()). `link` is the fixed
    convention path, `root` its resolved real target (what config_nodes carries)."""

    def __init__(self, mode, root, native_gl, egl, venus_socket, link=None, install=None):
        self.mode, self.root, self.native_gl, self.egl, self.venus_socket = mode, root, native_gl, egl, venus_socket
        self.link, self.install = link, install

    @property
    def native(self):
        return self.mode != "venus"

    @property
    def vendor_image(self):
        """nvidia-prolapse from our vendor.img: no driver root, no file binds."""
        return self.install == "vendor-image"

    @property
    def guest_layout(self):
        """Mounts waydroid-nvidia's layout-2 guest files (ANGLE/hwcomposer/surfaceflinger from nv/guest)."""
        return not self.vendor_image

    def __repr__(self):
        return "DriverMode({!r}, {!r}, native_gl={!r}, egl={!r}, install={!r})".format(
            self.mode, self.root, self.native_gl, self.egl, self.install)


def _cfg_error(text):
    return OSError("NVIDIA driver mode: {} ({}; then 'waydroid upgrade -o', or {})".format(
        text, CFG_PATH + " [waydroid]", SWITCH_TOOL))


def _prolapse_gl(root):
    """nvidia-prolapse GL: our own GLES driver nvrm (gallium over the Rust RM), staged by dev/source-native
    and announced by an enabled `native_gl` record in <root>/source-rm.json. Returns the
    ro.hardware.egl value ("nvrm") after verifying that record and the staged bytes
    (check_native_gl); refuses nvidia_native_gl = on with the restage remedy otherwise."""
    try:
        manifest = _load_source_rm(root, SOURCE_RM_FORMATS)
        record = manifest.get("native_gl")
        if not native_gl_enabled(manifest):
            why = ("is explicitly not staged ({})".format(record["reason"]) if isinstance(record, dict)
                   else "has no native_gl record (staged before the GL driver existed)")
            raise OSError("{} {}".format(os.path.join(root, SOURCE_RM_MANIFEST), why))
        check_native_gl(root, manifest)
    except OSError as e:
        raise _cfg_error("{} = on is not available in nvidia-prolapse mode: {}. Remedy: {} (or {} = off for "
                         "ANGLE)".format(GL_KEY, e, NATIVE_GL_STAGE, GL_KEY))
    return NVRM_EGL


def init_defaults(waydroid):
    """Called by `waydroid init` (tools/actions/initializer.py setup_config) on the [waydroid] mapping:
    a cfg without any NVIDIA key gets nvidia_driver_mode = DEFAULT_MODE and nvidia_native_gl =
    DEFAULT_NATIVE_GL written explicitly. A cfg that already names a mode (or any NVIDIA key) is left
    alone. Returns the text to log, or None when nothing was written."""
    if any(waydroid.get(k) for k in NVIDIA_KEYS):
        return None
    waydroid[MODE_KEY] = DEFAULT_MODE
    waydroid[GL_KEY] = DEFAULT_NATIVE_GL
    waydroid[INSTALL_KEY] = DEFAULT_INSTALL
    return ("NVIDIA driver mode: {} = {} (default; GL through ANGLE: {} = {}; {} = {}: run {} to install the "
            "nvidia-prolapse vendor image). Change it with {}".format(
                MODE_KEY, DEFAULT_MODE, GL_KEY, DEFAULT_NATIVE_GL, INSTALL_KEY, DEFAULT_INSTALL, SETUP, SWITCH_TOOL))


def root_link(mode):
    """(link, real target) of a native mode's root convention. Raises OSError naming the staging
    command when the link is missing, not a symlink, or dangling."""
    link = ROOT_LINKS[mode]
    if not os.path.islink(link):
        what = "is not a symlink" if os.path.lexists(link) else "does not exist"
        if mode == "nvidia-prolapse" and os.path.isdir(link) and not os.path.islink(link):
            what += (" (a directory here is the glibc-shim staging tree of the earlier waydroid-nvidia-prolapse "
                     "package or dev/native-deploy: move it aside, e.g. to {}, and point {} at it if you use "
                     "glibc-shim mode)".format(GLIBC_SHIM_STAGING, ROOT_LINKS["glibc-shim"]))
        raise OSError("NVIDIA driver mode {}: {} {}; it must be a symlink to the active staged root. Set it "
                      "with {}".format(mode, link, what, LINK_SETTERS[mode]))
    target = os.path.realpath(link)
    if not os.path.isdir(target):
        raise OSError("NVIDIA driver mode {}: {} -> {} is dangling (no such directory). Set it with {}".format(
            mode, link, os.readlink(link), LINK_SETTERS[mode]))
    return link, target


def repoint_link(link, target):
    """Atomically point the symlink `link` at `target` (absolute): new link next to it, then rename.
    Returns the previous target (None if there was no link)."""
    if not os.path.isabs(target) or not os.path.isdir(target):
        raise OSError("refusing to point {} at {}: not an existing absolute directory".format(link, target))
    if os.path.lexists(link) and not os.path.islink(link):
        raise OSError("refusing to replace {}: it exists and is not a symlink".format(link))
    previous = os.readlink(link) if os.path.islink(link) else None
    tmp = "{}.tmp-{}".format(link, os.getpid())
    if os.path.lexists(tmp):
        os.unlink(tmp)
    os.symlink(target, tmp)
    os.replace(tmp, link)
    fd = os.open(os.path.dirname(link), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    return previous


def driver_mode(waydroid):
    """DriverMode for the [waydroid] mapping, or None when no NVIDIA key is set (stock Waydroid).

    Raises OSError naming the key and the remedy for a missing/unknown mode, a retired path key, a
    missing/dangling/mismatched root link, and native_gl = on where the mode has no GL driver of its own."""
    present = [k for k in NVIDIA_KEYS if waydroid.get(k)]
    if not present:
        return None
    retired = [k for k in RETIRED_KEYS if waydroid.get(k)]
    if retired:
        raise _cfg_error("{} {} no longer read: paths are fixed conventions ({}, {}, socket {}). Remove {} with "
                         "'dev/driver-mode set MODE', which strips them".format(
                             ", ".join("{} = {}".format(k, waydroid.get(k)) for k in retired),
                             "is" if len(retired) == 1 else "are", ROOT_LINKS["nvidia-prolapse"],
                             ROOT_LINKS["glibc-shim"], VENUS_SOCKET, "it" if len(retired) == 1 else "them"))
    mode = waydroid.get(MODE_KEY)
    if not mode:
        raise _cfg_error("{} is set but {} is not; set it to one of {}".format(
            ", ".join(present), MODE_KEY, ", ".join(MODES)))
    if mode in RENAMED_MODES:
        new, old_link = RENAMED_MODES[mode]
        raise _cfg_error("{} = {} was renamed to {} (same driver). Set {} = {} with 'dev/driver-mode set {}'; "
                         "its root link is now {} (was {}): sudo dev/driver-mode link {} \"$(readlink -f {})\"".format(
                             MODE_KEY, mode, new, MODE_KEY, new, new, ROOT_LINKS[new], old_link, new, old_link))
    if mode not in MODES:
        raise _cfg_error("{} = {} is not one of {}".format(MODE_KEY, mode, ", ".join(MODES)))
    gl = waydroid.get(GL_KEY) or "off"
    if gl not in ("on", "off"):
        raise _cfg_error("{} = {} must be on or off (off = ANGLE)".format(GL_KEY, gl))
    native_gl = gl == "on"
    install = waydroid.get(INSTALL_KEY)
    if mode != "nvidia-prolapse" and install:
        raise _cfg_error("{} = {} belongs to {} = nvidia-prolapse; delete it for {}".format(
            INSTALL_KEY, install, MODE_KEY, mode))
    if mode == "nvidia-prolapse":
        if not install:
            raise _cfg_error("{} = nvidia-prolapse needs {} = vendor-image (the packaged install: {}) or dev-root (a "
                             "developer's staged root: dev/driver-mode set nvidia-prolapse --install dev-root)".format(
                                 MODE_KEY, INSTALL_KEY, SETUP))
        if install not in INSTALLS:
            raise _cfg_error("{} = {} is not one of {}".format(INSTALL_KEY, install, ", ".join(INSTALLS)))
        if install == "vendor-image":
            # what the image carries is checked against the setup's record by check_vendor_image() (root,
            # generator time); nvrm availability is part of that record
            return DriverMode(mode, None, native_gl, NVRM_EGL if native_gl else "angle", None, install=install)
    if mode == "venus":
        if native_gl:
            raise _cfg_error("{} = on is not available in venus mode (Venus has no GL driver of its own, GLES is "
                             "ANGLE on Venus); only glibc-shim mode has native GL. Set {} = off".format(GL_KEY, GL_KEY))
        return DriverMode(mode, None, False, "angle", VENUS_SOCKET)
    link, root = root_link(mode)
    source = os.path.exists(os.path.join(root, SOURCE_RM_MANIFEST))
    if mode == "nvidia-prolapse":
        if not source:
            raise OSError("NVIDIA driver mode nvidia-prolapse: {} -> {} has no {}: it is not a source NVK + Rust RM "
                          "root. Set it with {}".format(link, root, SOURCE_RM_MANIFEST, LINK_SETTERS[mode]))
        egl = _prolapse_gl(root) if native_gl else "angle"
        return DriverMode(mode, root, native_gl, egl, None, link, install=install)
    else:
        if source:
            raise OSError("NVIDIA driver mode glibc-shim: {} -> {} is a source NVK + Rust RM root ({}); glibc-shim "
                          "needs the blob staging root. Set it with {}".format(
                              link, root, SOURCE_RM_MANIFEST, LINK_SETTERS[mode]))
        egl = "nvnative" if native_gl else "angle"
    return DriverMode(mode, root, native_gl, egl, None, link)


# waydroid-nvidia's guest layout 2 (lxc.py mounts these from ANGLE_GUEST_ROOT in venus mode; the native
# modes use the last two plus ANGLE). Same list and order as the baseline patch's guest_files.
VENUS_GUEST_FILES = (
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
VENUS_PROVISION = "waydroid-nvidia's setup (waydroid-nvidia-setup) provisions them"


def check_venus(dm, waydroid):
    """venus mode: the baseline's mounts are not optional, so check their sources before LXC does, and
    gralloc must allocate on the NVIDIA render node (drm_device, selected by PCI vendor, never by number:
    renderD128 is another GPU on hybrid hosts)."""
    render = nvidia_render_node()
    if waydroid.get("drm_device") != render:
        raise _cfg_error("venus mode needs drm_device = {} (the NVIDIA render node, PCI vendor {}); it is {}".format(
            render, NVIDIA_VENDOR, waydroid.get("drm_device") or "unset"))
    if waydroid.get(LAYOUT_KEY) != "2":
        return                                   # baseline's pre-layout-2 compatibility mounts, unchanged
    missing = [f for f in VENUS_GUEST_FILES if not os.path.isfile(os.path.join(ANGLE_GUEST_ROOT, f))]
    if missing:
        raise OSError("NVIDIA driver mode venus: the Venus guest files {} are missing under {} ({})".format(
            ", ".join(missing), ANGLE_GUEST_ROOT, VENUS_PROVISION))


def announce(dm):
    """One line naming the mode, the root and the GL choice (logged at upgrade and container start)."""
    if dm.mode == "venus":
        where = "socket " + dm.venus_socket
    elif dm.vendor_image:
        where = "vendor image, {} = vendor-image".format(INSTALL_KEY)
    else:
        where = "root {} -> {}".format(dm.link, dm.root)
    return "NVIDIA driver mode: {} ({}); GL: {} (ro.hardware.egl={}, {} = {})".format(
        dm.mode, where, GL_NAMES[dm.egl], dm.egl, GL_KEY, "on" if dm.native_gl else "off")


# The core properties every mode decides (value None = absent in that mode).
#  ro.hardware.vulkan selects hw/vulkan.<value>.so in libvulkan's HAL search; the mesa.* pair points
#  Mesa's Venus driver at the vtest socket and has no meaning without it.
#  ro.zygote.disable_gl_preload = true (native modes): measured 2026-10-10 on the LineageOS 20 guest,
#  the 64-bit zygote preloads the graphics driver (ZygoteInit.maybePreloadGraphicsDriver, gated by this
#  property; frameworks/base android13-release core/java/com/android/internal/os/ZygoteInit.java:92-204).
#  That runs ANGLE -> our Vulkan HAL -> the driver and leaves fds on /dev/nvidia0, /dev/nvidiactl,
#  /dev/nvidia-modeset and the render node open in the zygote. The zygote fd allowlist
#  (core/jni/fd_utils.cpp:35-51, kPathAllowlist) has /dev/dri/renderD129 but nothing under /dev/nvidia,
#  so system_server aborts at fork with "JNI FatalError called: (system_server) Not allowlisted (39):
#  /dev/nvidiactl". Venus mode opens no /dev/nvidia* in the guest and keeps the preload (baseline).
CORE_PROPERTIES = ("ro.hardware.vulkan", "ro.hardware.egl", "ro.zygote.disable_gl_preload", "mesa.vn.debug",
                   "mesa.vtest.socket.name")


def mode_properties(dm):
    """[(key, value or None)] the mode writes into waydroid_base.prop (None: the key must be absent)."""
    if dm.mode == "venus":
        # waydroid-nvidia-setup's values; the socket directory is bound at dev/venus (lxc.py).
        props = [("ro.hardware.vulkan", "virtio"), ("ro.hardware.egl", "angle"),
                 ("ro.zygote.disable_gl_preload", None), ("mesa.vn.debug", "vtest"),
                 ("mesa.vtest.socket.name", "/dev/venus/" + os.path.basename(dm.venus_socket))]
        return props
    props = [("ro.hardware.vulkan", "nvidia"), ("ro.hardware.egl", dm.egl),
             ("ro.zygote.disable_gl_preload", "true"), ("mesa.vn.debug", None), ("mesa.vtest.socket.name", None)]
    if dm.vendor_image:
        return props + list(MEDIA_BASE_PROPERTIES)     # the image always carries the gated Codec2 service
    return props + base_properties(dm.root)


def derived_properties():
    """Every key some mode derives; [properties] may set none of them."""
    return CORE_PROPERTIES + tuple(k for k, _ in MEDIA_BASE_PROPERTIES)


def apply_mode_properties(props, dm):
    """make_base_props(): replace/remove the mode's keys in a list of 'key=value' lines."""
    for k, v in mode_properties(dm):
        props = [p for p in props if not p.startswith(k + "=")]
        if v is not None:
            props.append(k + "=" + v)
    return props


def check_properties(props, dm):
    """Raise OSError if waydroid.cfg [properties] sets a mode-derived property (or, in the native
    modes, the runtime-only decode route switch)."""
    derived = [key for key in derived_properties() if key in props]
    if derived:
        want = ", ".join("{}={}".format(k, v if v is not None else "(absent)") for k, v in mode_properties(dm))
        raise OSError("NVIDIA driver mode {}: waydroid.cfg [properties] sets {}; these are derived from "
                      "[waydroid] {}/{} (and, for the Codec2 listing, from the root's media record): this mode "
                      "writes {}. Delete them from {} [properties] and re-run 'waydroid upgrade -o' ({} does "
                      "both)".format(dm.mode, ", ".join(derived), MODE_KEY, GL_KEY, want, CFG_PATH, SWITCH_TOOL))
    if dm.native and DECODE_ROUTE_PROPERTY in props:
        # persist.* set at runtime is saved in the guest's /data/property and loaded after
        # waydroid_base.prop, so a [properties] entry would be silently shadowed by it.
        raise OSError("NVIDIA native mode: waydroid.cfg [properties] sets {0}; that switch is runtime-only. "
                      "Delete it from /var/lib/waydroid/waydroid.cfg, re-run 'waydroid upgrade -o', and use "
                      "'waydroid prop set {0} on|off' (applies live, persists across restarts).".format(
                          DECODE_ROUTE_PROPERTY))


def check_base_prop(path, dm):
    """Container start: waydroid_base.prop must carry exactly this mode's properties (a cfg switched
    without 'waydroid upgrade -o' would otherwise boot the previous mode's properties)."""
    try:
        with open(path) as f:
            have = dict(l.rstrip("\n").split("=", 1) for l in f if "=" in l)
    except OSError as e:
        raise OSError("NVIDIA driver mode: cannot read {} ({}); run 'waydroid upgrade -o'".format(path, e.strerror))
    wrong = ["{}={} (has {})".format(k, v if v is not None else "(absent)",
                                     have[k] if k in have else "nothing")
             for k, v in mode_properties(dm) if have.get(k) != v]
    if wrong:
        raise OSError("NVIDIA driver mode {}: {} was generated for another configuration; it needs {}. Run "
                      "'waydroid upgrade -o' with the container stopped ({} does it)".format(
                          dm.mode, path, "; ".join(wrong), SWITCH_TOOL))


# --- per-session properties: SurfaceFlinger's vsync snap window ---------------------------------
#
# The patched surfaceflinger (patches/lineage-20/frameworks-native/0001, bound in every mode) reads
# debug.sf.snap_to_same_vsync_within_ns once, when its scheduler is created
# (VsyncSchedule::createDispatch). VSyncDispatchTimerQueueEntry::schedule() treats two predicted
# vsyncs closer than that window as the same vsync; a window >= the display's vsync period collapses
# every timeslot and the dispatch timer parks at INT64_MAX: no frames. The stock 3 ms therefore stops
# SurfaceFlinger above 333 Hz. Predicted vsyncs of the same edge jitter around 0, those of the next
# edge around one period, so half the period is the window with the widest margin on both sides;
# the stock 3 ms stays wherever it is already below that (<= 166 Hz: the property is not written).
#
# The period to undercut is the one hwcomposer advertises: persist.waydroid.refresh_rate when it is
# set, else the highest current refresh of all wl_outputs, read when the composer opens at container
# start (android_hardware_waydroid hwcomposer.cpp hwc_open / wayland-hwc.cpp output_handle_mode).
# The host's active CRTC modes are the outputs the compositor drives at that moment; reading them
# here, at every container start, follows monitor changes between sessions the way hwcomposer does.
# The setup tool writes no snap value.
SNAP_PROPERTY = "debug.sf.snap_to_same_vsync_within_ns"
REFRESH_PROPERTY = "persist.waydroid.refresh_rate"
SF_SNAP_STOCK_NS = 3000000          # kSnapToSameVsyncWithin, frameworks/native VsyncSchedule.cpp
HWC_MAX_REFRESH_HZ = 1000           # hwcomposer ignores a persist.waydroid.refresh_rate above this
DRI_DIR = "/dev/dri"


def _ioc_rw(nr, size):
    """DRM_IOWR(nr, size) (asm-generic ioctl encoding, DRM_IOCTL_BASE 'd')."""
    return (3 << 30) | (size << 16) | (ord("d") << 8) | nr


# include/uapi/drm/drm_mode.h: struct drm_mode_card_res, struct drm_mode_crtc (+ drm_mode_modeinfo)
_CARD_RES = "=4Q4I4I"
_MODEINFO = "I10H3I32s"
_CRTC = "=QI6I" + _MODEINFO
_GETRESOURCES = 0xA0
_GETCRTC = 0xA1
_FLAG_INTERLACE = 1 << 4
_FLAG_DBLSCAN = 1 << 5


def _mode_refresh_hz(clock, htotal, vtotal, vscan, vrefresh, flags):
    """drm_mode_vrefresh() with full precision (kHz clock, so 2560x1440@499.991 is not 500)."""
    if not htotal or not vtotal:
        return float(vrefresh)
    hz = clock * 1000.0 / (htotal * vtotal)
    if flags & _FLAG_INTERLACE:
        hz *= 2
    if flags & _FLAG_DBLSCAN:
        hz /= 2
    if vscan > 1:
        hz /= vscan
    return hz


def active_refresh_rates(dri_dir=None):
    """([Hz of every CRTC scanning out now, all GPUs], [notes]). Read-only KMS queries
    (GETRESOURCES/GETCRTC need no DRM master); a card that cannot be read is named in the notes."""
    import array
    import fcntl
    import struct
    dri_dir = dri_dir or DRI_DIR
    res_size, crtc_size = struct.calcsize(_CARD_RES), struct.calcsize(_CRTC)
    rates, notes = [], []
    for card in sorted(glob.glob(os.path.join(dri_dir, "card[0-9]*"))):
        try:
            fd = os.open(card, os.O_RDONLY | os.O_CLOEXEC)
        except OSError as e:
            notes.append("display modes: cannot open {} ({})".format(card, e.strerror))
            continue
        try:
            res = bytearray(res_size)
            fcntl.ioctl(fd, _ioc_rw(_GETRESOURCES, res_size), res)
            count = struct.unpack(_CARD_RES, res)[5]
            if not count:
                continue
            ids = array.array("I", [0] * count)
            res = bytearray(struct.pack(_CARD_RES, 0, ids.buffer_info()[0], 0, 0, 0, count, 0, 0, 0, 0, 0, 0))
            fcntl.ioctl(fd, _ioc_rw(_GETRESOURCES, res_size), res)
            count = min(count, struct.unpack(_CARD_RES, res)[5])
            for crtc_id in ids[:count]:
                buf = bytearray(struct.pack(_CRTC, 0, 0, crtc_id, *([0] * 5), *([0] * 14), b""))
                fcntl.ioctl(fd, _ioc_rw(_GETCRTC, crtc_size), buf)
                v = struct.unpack(_CRTC, buf)
                if not v[7]:                                  # mode_valid: CRTC off
                    continue
                clock, htotal, vtotal, vscan, vrefresh, flags = v[8], v[12], v[17], v[18], v[19], v[20]
                hz = _mode_refresh_hz(clock, htotal, vtotal, vscan, vrefresh, flags)
                if hz > 0:
                    rates.append(hz)
        except OSError as e:
            notes.append("display modes: KMS query on {} failed ({})".format(card, e.strerror or e))
        finally:
            os.close(fd)
    return rates, notes


def snap_window_ns(refresh_hz):
    """The snap window for a display at refresh_hz, or None where the stock 3 ms is at most half the
    period (<= 166.7 Hz: leave SurfaceFlinger's own value)."""
    half_period = int(1e9 / refresh_hz) // 2
    return half_period if half_period < SF_SNAP_STOCK_NS else None


def session_properties(props, rates):
    """images.make_prop(): props ('key=value' lines, waydroid_base.prop + session values) with the snap
    window for the period hwcomposer will advertise; returns (props, [log lines]).

    rates: active_refresh_rates() of the host. An explicit persist.waydroid.refresh_rate in the
    props (waydroid.cfg [properties], e.g. the setup tool's --refresh) is what hwcomposer uses, so it
    decides; else the fastest active display does."""
    notes = []
    kept = []
    for p in props:
        if p.startswith(SNAP_PROPERTY + "="):
            notes.append("display: ignoring {} from waydroid.cfg [properties]; it is derived at every "
                         "container start".format(p))
            continue
        kept.append(p)
    forced = None
    for p in kept:
        if p.startswith(REFRESH_PROPERTY + "="):
            value = p.split("=", 1)[1].strip()
            if value.isdigit() and 0 < int(value) <= HWC_MAX_REFRESH_HZ:
                forced = int(value)
    if forced is not None:
        hz, why = float(forced), "{}={} (explicit)".format(REFRESH_PROPERTY, forced)
    elif rates:
        hz = max(rates)
        why = "fastest active display mode {:.3f} Hz (of {})".format(
            hz, ", ".join("{:.3f}".format(r) for r in sorted(rates, reverse=True)))
    else:
        notes.append("display: no active display mode found under {}; SurfaceFlinger keeps its stock "
                     "3 ms vsync snap window (enough up to 333 Hz)".format(DRI_DIR))
        return kept, notes
    window = snap_window_ns(hz)
    if window is None:
        notes.append("display: {}; SurfaceFlinger's stock 3 ms vsync snap window fits".format(why))
        return kept, notes
    kept.append("{}={}".format(SNAP_PROPERTY, window))
    notes.append("display: {}; {}={} (half the period)".format(why, SNAP_PROPERTY, window))
    return kept, notes


def driver_version(proc_path=None):
    """Version string of the loaded NVIDIA kernel module, e.g. '615.71.09'.

    proc_path defaults to PROC_VERSION, read at call time (tests point it at a fake file)."""
    proc_path = proc_path or PROC_VERSION
    try:
        with open(proc_path) as f:
            text = f.read()
    except OSError as e:
        raise OSError("NVIDIA native mode: cannot read {} ({}); is the nvidia kernel module "
                      "loaded?".format(proc_path, e.strerror))
    m = re.search(r"^NVRM version:.*?\s(\d+\.\d+(?:\.\d+)?)\s", text, re.M)
    if not m:
        raise OSError("NVIDIA native mode: no 'NVRM version: ... <x.y.z>' line in {}".format(proc_path))
    return m.group(1)


def nvidia_render_node(sysfs="/sys/class/drm"):
    """/dev/dri/renderD<N> of the NVIDIA GPU, chosen by PCI vendor id."""
    for vendor_file in sorted(glob.glob(os.path.join(sysfs, "renderD*", "device", "vendor"))):
        try:
            with open(vendor_file) as f:
                vendor = f.read().strip()
        except OSError:
            continue
        if vendor == NVIDIA_VENDOR:
            node = vendor_file.split(os.sep)[-3]
            return "/dev/dri/" + node
    raise OSError("NVIDIA native mode: no /dev/dri/renderD* has PCI vendor {} in {}".format(NVIDIA_VENDOR, sysfs))


def _binds(entry, node):
    """True if the lxc.mount.entry line binds host path `node` at the same path in the guest."""
    p = entry.split()
    return len(p) >= 4 and p[:2] == ["lxc.mount.entry", "="] and p[2] == node and p[3] == node.lstrip("/")


def _check_abi_tree(staging, sub, shims, v, bits):
    """Raise OSError unless <staging>/<sub> holds everything one ABI needs (64-bit: lib64)."""
    libdir = os.path.join(staging, sub)
    is32 = bits == 32
    what = " (32-bit)" if is32 else ""
    for shim in shims:
        if not os.path.isfile(os.path.join(libdir, shim)):
            raise OSError("NVIDIA native mode: {} is missing; deploy the{} glibc-compat shims "
                          "({}{})".format(os.path.join(libdir, shim), " 32-bit" if is32 else "",
                                          "build: make -C nvidia-prolapse/shim ARCH=x86, then " if is32 else "",
                                          DEPLOY))
    sidecar = os.path.join(libdir, "host-glibc-version")
    if not os.path.isfile(sidecar) or os.path.getsize(sidecar) == 0:
        raise OSError("NVIDIA native mode: {} is missing or empty; the deploy step ({}) writes "
                      "the host glibc version there".format(sidecar, DEPLOY))
    link = os.path.join(libdir, "libEGL_nvidia.so.0")
    want = BLOBS[0].format(v=v)
    if not os.path.islink(link) or os.readlink(link) != want:
        raise OSError("NVIDIA native mode: {} should be a symlink to {} (driver {} is loaded); "
                      "re-run {} after a driver update".format(link, want, v, DEPLOY))
    hwdir = os.path.join(libdir, "hw")
    if not os.path.isdir(hwdir):
        raise OSError("NVIDIA native mode: {} is missing; the{} Vulkan HAL lives there "
                      "(build nvidia-prolapse/hal, then {})".format(hwdir, what, DEPLOY))
    hal = os.path.join(libdir, HAL)
    if not os.path.isfile(hal):
        raise OSError("NVIDIA native mode: the{} Vulkan HAL {} is missing (build nvidia-prolapse/hal{}, "
                      "then {})".format(what, hal, " (android32)" if is32 else "", DEPLOY))
    wrapper = os.path.join(libdir, GBM_WRAPPER)
    if not os.path.isfile(wrapper):
        if is32:
            raise OSError("NVIDIA native mode: the 32-bit gralloc backend {} is missing; without it the image's "
                          "32-bit processes (mediaserver: video playback) map NVIDIA buffers through the stock "
                          "Mesa wrapper and crash (build: make -C nvidia-prolapse/gralloc android32, then "
                          "{})".format(wrapper, DEPLOY))
        raise OSError("NVIDIA native mode: the gralloc backend {} is missing (build nvidia-prolapse/gralloc, "
                      "then {})".format(wrapper, DEPLOY))
    core = os.path.join(libdir, MINIGBM_CORE)
    if not os.path.isfile(core) or _elf_class(core) != (32 if is32 else 64):
        if is32:
            raise OSError("NVIDIA native mode: {} is missing or not an ELF32 library; it is minigbm's gbm_mesa "
                          "core with our patches for the image's 32-bit mapper, without it 32-bit CPU locks "
                          "pass no access flags and a read lock writes block-linear buffers back (build: make "
                          "-C nvidia-prolapse/gralloc minigbm check-minigbm, then {})".format(core, DEPLOY))
        raise OSError("NVIDIA native mode: {} is missing or not an ELF64 library; it is minigbm's gbm_mesa "
                      "core with our patch (CPU usage does not force LINEAR on GPU render targets), without "
                      "it SurfaceFlinger's screenshot buffers are LINEAR, which NVIDIA's GLES cannot render "
                      "into (build: make -C nvidia-prolapse/gralloc minigbm check-minigbm, then {})".format(
                          core, DEPLOY))
    libdrm = os.path.join(libdir, LIBDRM)
    if not os.path.isfile(libdrm):
        raise OSError("NVIDIA native mode: {} is missing; without it NVIDIA's ICD does not "
                      "advertise VK_EXT_external_memory_dma_buf and the Vulkan HAL cannot offer "
                      "VK_ANDROID_native_buffer (build: make -C nvidia-prolapse/libdrm{}, then "
                      "{})".format(libdrm, " ARCH=x86" if is32 else "", DEPLOY))
    return libdir


def _elf_class(path):
    """32 or 64 for an ELF file, None otherwise."""
    try:
        with open(path, "rb") as f:
            head = f.read(5)
    except OSError:
        return None
    if len(head) < 5 or head[:4] != b"\x7fELF":
        return None
    return {1: 32, 2: 64}.get(head[4])


def _check_egl_tree(root, sub, bits, angle_root):
    """Raise OSError unless both EGL drivers are staged for one ABI. Returns the staged egl dir."""
    is32 = bits == 32
    what = " 32-bit" if is32 else ""
    libdir = os.path.join(root, "vendor_extra", sub)
    egldir = os.path.join(root, EGL_STAGE, sub, "egl")
    both = "; both drivers are staged so ro.hardware.egl can select either"
    if not os.path.isdir(egldir):
        raise OSError("NVIDIA native mode: {} is missing; it is bind-mounted over the guest's /vendor/{}/egl "
                      "and holds the{} EGL drivers ({})".format(egldir, sub, what, EGL_BUILD))
    have = set(os.listdir(egldir))
    want = set(EGL_FRONT + EGL_ANGLE)
    if have - want:
        raise OSError("NVIDIA native mode: {} has unexpected files {}; only {} belong there (the EGL loader "
                      "must find exactly the two staged drivers); remove them and re-run {}".format(
                          egldir, ", ".join(sorted(have - want)), ", ".join(EGL_FRONT + EGL_ANGLE), DEPLOY))
    for name in EGL_FRONT:
        path = os.path.join(egldir, name)
        if not os.path.isfile(path) or _elf_class(path) != bits:
            raise OSError("NVIDIA native mode: the{} EGL front {} is missing or not an ELF{} library ({}{})".format(
                what, path, bits, EGL_BUILD, both))
    for name in EGL_ANGLE:
        path = os.path.join(egldir, name)
        if not os.path.isfile(path):
            raise OSError("NVIDIA native mode: the placeholder {} is missing; ANGLE is bind-mounted onto it "
                          "({}{})".format(path, DEPLOY, both))
        src = os.path.join(angle_root, "vendor", sub, "egl", name)
        if not os.path.isfile(src) or os.path.getsize(src) == 0:
            raise OSError("NVIDIA native mode: the{} ANGLE library {} is missing (provisioned by "
                          "waydroid-nvidia-prolapse-setup; the patched build: angle/README.md){}".format(
                              what, src, both))
    plat = os.path.join(libdir, EGL_PLATFORM)
    if not os.path.isfile(plat) or _elf_class(plat) != bits:
        raise OSError("NVIDIA native mode: the{} EGL platform library {} is missing or not an ELF{} library; "
                      "without it NVIDIA's EGL has no Android window surfaces ({}{})".format(
                          what, plat, bits, EGL_BUILD, both))
    js = os.path.join(libdir, EGL_PLATFORM_JSON)
    try:
        with open(js) as f:
            lib = json.load(f)["ICD"]["library_path"]
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise OSError("NVIDIA native mode: {} is missing or unreadable ({}); NVIDIA's EGL finds the platform "
                      "library through it ({})".format(js, e, EGL_BUILD))
    if lib != EGL_PLATFORM:
        raise OSError("NVIDIA native mode: {} names '{}', not {} ({})".format(js, lib, EGL_PLATFORM, EGL_BUILD))
    env = os.path.join(libdir, DEFAULT_ENV)
    try:
        with open(env) as f:
            lines = [l.strip() for l in f]
    except OSError:
        lines = []
    if "__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES=${ORIGIN}/" + EGL_PLATFORM_JSON not in lines:
        raise OSError("NVIDIA native mode: {} is missing or does not point "
                      "__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES at ${{ORIGIN}}/{}; the shims answer the "
                      "driver's lookup from it ({})".format(env, EGL_PLATFORM_JSON, EGL_BUILD))
    return egldir


SOURCE_RM_MANIFEST = "source-rm.json"
# Format 3 = format 2 + a mandatory "media" record: either the Codec2 NVDEC decoder
# (nvidia-prolapse/media) staged and gated, or an explicit opt-out with a reason. Format 2 roots
# predate the media component (their format number says so) and stay valid without it.
SOURCE_RM_FORMAT = 3
SOURCE_RM_FORMATS = (2, 3)
SOURCE_RM_TOOL = "sudo python3 dev/source-native"   # run from the waydroid-nvidia-prolapse checkout
SOURCE_RM_BACKUPS = "/var/lib/waydroid/before-source-native-*"

# Codec2 NVDEC decoder (nvidia-prolapse/media). The vendor Codec2 runtime libraries the image lacks
# (nvidia-prolapse/media/vndk/build.sh) live in vendor_extra/lib64 (vendor default namespace search
# path, reached through the vendor_extra rbind). The service and its init/VINTF/seccomp/codec files
# live in <root>/odm, which replaces the guest's /vendor/odm (LineageOS 20: /odm/* are symlinks to
# /vendor/odm/*, and vendor.img's /odm holds only etc/{build.prop,group,passwd}, copied unchanged).
MEDIA_LIBS = (
    "android.hardware.media.c2@1.0.so", "android.hardware.media.c2@1.1.so",
    "android.hardware.media.c2@1.2.so", "libcodec2_hidl@1.0.so", "libcodec2_hidl@1.1.so",
    "libcodec2_hidl@1.2.so", "libcodec2_hidl_plugin.so", "libcodec2_vndk.so",
    "libstagefright_bufferpool@2.0.1.so",
)
MEDIA_SERVICE_NAME = "android.hardware.media.c2@1.2-service-nvprolapse"
MEDIA_SERVICE = "odm/bin/hw/" + MEDIA_SERVICE_NAME
MEDIA_INIT_SERVICE = "android-hardware-media-c2-nvprolapse"     # `service` name in the .rc
MEDIA_XML = "odm/etc/media_codecs_c2.xml"
MEDIA_RC = "odm/etc/init/android.hardware.media.c2@1.2-service-nvprolapse.rc"
MEDIA_VINTF = "odm/etc/vintf/manifest/android.hardware.media.c2@1.2-nvprolapse.xml"
MEDIA_ODM_FILES = (
    MEDIA_SERVICE,
    MEDIA_RC,
    MEDIA_VINTF,
    "odm/etc/seccomp_policy/android.hardware.media.c2@1.2-nvprolapse.policy",
    MEDIA_XML,
)
ODM_ORIGINALS = ("build.prop", "group", "passwd")                # vendor.img /odm/etc, unchanged
MEDIA_STAGE = "{} stage NEWROOT (builds: nvidia-prolapse/media/vndk/build.sh, its service stage, " \
              "make -C nvidia-prolapse/media test)".format(SOURCE_RM_TOOL)


# Our own GLES driver nvrm (driver-source native/mesa/ANDROID-GL.md, the guest integration contract).
# Per graphics ABI: lib{EGL,GLESv1_CM,GLESv2}_nvrm.so in the staged egl directory (Android 13's loader
# opens lib*_<ro.hardware.egl>.so only from /vendor/<lib>/egl or /system/<lib>/egl, see above) and
# libgallium_dri_nvrm.so in vendor_extra/<lib> (DT_NEEDED of all three; not /vendor/<lib>, where the
# image's own Mesa libgallium_dri.so lives). libgallium_dri_nvrm NEEDs libnv_bionic_native.so and
# libdrm.so.2: the ONE copy per ABI NVK uses, so the stage proves nvrm was linked against exactly that
# libnv_bionic_native.so (nvrm-build.json rust.sha256, recorded as native_gl.gate.rust_sha256).
NVRM_EGL = "nvrm"
NVRM_EGL_LIBS = tuple(n.format(NVRM_EGL) for n in EGL_LIBS)
NVRM_GALLIUM = "libgallium_dri_nvrm.so"
ABI_DIRS = {"x86_64": ("lib64", 64), "x86": ("lib", 32)}
NATIVE_GL_BUILD = ("PROLAPSE_ROOT=<checkout> python3 nvidia-prolapse/driver-source/native/mesa/build.py --gl "
                   "--target both --rust-profile release, from the driver-source checkout NVK's "
                   "libnv_bionic_native.so is built from")
NATIVE_GL_STAGE = "build the GL driver ({}), then {} stage NEWROOT and activate NEWROOT".format(
    NATIVE_GL_BUILD, SOURCE_RM_TOOL)


def native_gl_files(manifest, abis=None):
    """{relative path: ELF bits} of the nvrm files for the manifest's graphics ABIs (or `abis`)."""
    files = {}
    for abi in (abis or manifest["graphics_abis"]):
        sub, bits = ABI_DIRS[abi]
        for name in NVRM_EGL_LIBS:
            files["vendor/{}/egl/{}".format(sub, name)] = bits
        files["vendor_extra/{}/{}".format(sub, NVRM_GALLIUM)] = bits
    return files


def native_gl_enabled(manifest):
    """True iff the manifest records nvrm as staged and gated (check_native_gl verifies the bytes)."""
    record = manifest.get("native_gl")
    return isinstance(record, dict) and record.get("enabled") is True


def _sha256(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def check_native_gl(root, manifest):
    """Raise OSError unless the enabled native_gl record matches the staged bytes: exactly the nvrm
    files of every graphics ABI, right ELF class, recorded hashes (also in artifacts), and the staged
    libnv_bionic_native.so of each ABI is the one the gated nvrm build linked against."""
    record = manifest["native_gl"]
    files, artifacts = record["files"], manifest["artifacts"]
    want = native_gl_files(manifest)
    if set(files) != set(want):
        raise OSError("native_gl record lists {}, the graphics ABIs {} need {}".format(
            sorted(files), manifest["graphics_abis"], sorted(want)))
    for relative, bits in sorted(want.items()):
        path = os.path.join(root, relative)
        if _elf_class(path) != bits:
            raise OSError("native_gl file {} is missing or not ELF{}".format(path, bits))
        if artifacts.get(relative) != files[relative]:
            raise OSError("native_gl file {} is not recorded with the same hash in artifacts".format(relative))
        if _sha256(path) != files[relative]:
            raise OSError("native_gl file hash changed: {}".format(path))
    for abi in manifest["graphics_abis"]:
        relative = "vendor_extra/{}/libnv_bionic_native.so".format(ABI_DIRS[abi][0])
        linked = record["gate"]["rust_sha256"].get(abi)
        staged = artifacts.get(relative)
        if linked != staged or _sha256(os.path.join(root, relative)) != staged:
            raise OSError("nvrm ({}) was linked against libnv_bionic_native.so {}, but the root stages {} with {} "
                          "(NVK and nvrm must share one Rust RM library)".format(abi, linked, relative, staged))


def _check_source_rm_native_gl(root, manifest):
    """nvrm is either staged completely and hash-verified, or absent everywhere; the staged egl
    directories hold exactly ANGLE's three files plus, when staged, nvrm's three."""
    enabled = native_gl_enabled(manifest)
    if enabled:
        try:
            check_native_gl(root, manifest)
        except OSError as e:
            raise OSError("Source RM deployment {}: {}. Restage: {}".format(root, e, NATIVE_GL_STAGE))
    else:
        every = native_gl_files(manifest, abis=tuple(ABI_DIRS))
        stray = [r for r in every if os.path.lexists(os.path.join(root, r)) or r in manifest["artifacts"]]
        if stray:
            raise OSError("Source RM deployment {}: native GL is not staged, but nvrm files are present: {}. "
                          "Restage: {}".format(root, ", ".join(sorted(stray)), NATIVE_GL_STAGE))
    staged = native_gl_files(manifest) if enabled else {}
    for sub in ("lib64", "lib"):
        egldir = os.path.join(root, EGL_STAGE, sub, "egl")
        want = set(EGL_ANGLE) | {os.path.basename(r) for r in staged if r.startswith("vendor/{}/egl/".format(sub))}
        have = set(os.listdir(egldir)) if os.path.isdir(egldir) else set()
        if have != want:
            raise OSError("Source RM deployment {}: {} must hold exactly {} (it is bind-mounted over the guest's "
                          "/vendor/{}/egl); it holds {}. Restage: {}".format(
                              root, egldir, ", ".join(sorted(want)), sub, ", ".join(sorted(have)) or "nothing",
                              NATIVE_GL_STAGE))


def media_enabled(manifest):
    """True iff the (verified) manifest stages the Codec2 decoder."""
    media = manifest.get("media")
    return isinstance(media, dict) and media.get("enabled") is True


# Codec2 listing for a root that stages the decoder. Upstream make_base_props() writes
# debug.stagefright.ccodec=0 whenever it picks the gbm/default gralloc (line present since the
# initial commit 1f03938, no rationale recorded; android_vendor_waydroid's sibling 0011 "Don't use
# YV12 color format for video decoding" keys on the same gralloc set, and its 2025 Codec2 patches
# 0018/0019 work around c2.android.* output on minigbm). With 0, Codec2InfoBuilder::
# buildMediaCodecList (frameworks/av tm-qpr3 media/codec2/sfplugin/Codec2InfoBuilder.cpp:556-596)
# lists no Codec2 component, so c2.nvprolapse.* is never offered. 1 and 2 drop every non-c2.android
# component except *.avc.* (ranked last); 3 ranks c2.android.* first (rank 1), ahead of the hardware
# decoders; 4 keeps component ranks: c2.nvprolapse.* 0x10 (media/c2/NvC2Component.cpp), c2.android.*
# audio 8 / other 0x200 (media/codec2/vndk/C2Store.cpp:1003-1009), OMX.google.* audio 0x10 / other
# 0x210 (media/libstagefright/OmxInfoBuilder.cpp:141-167). 4 alone would move every software
# default from OMX.google.* (the path Waydroid ships with ccodec=0) to c2.android.*, and OMX.google.*
# names resolve to the c2.android.* aliases; the two OMX default-rank properties put OMX.google.*
# back ahead of c2.android.* and keep both behind the hardware decoders. Measured 2026-10-14 in the
# guest (process-local MediaCodecList): vp9/av1/hevc/avc -> c2.nvprolapse.* first, software types
# (aac, mp3, mpeg4, vp8) -> OMX.google.* first as with ccodec=0, c2.android.* listed last.
# Runtime route switch read by the Codec2 service on every list/create (nvidia-prolapse/media
# c2/NvC2Component.h kRouteProp): unset/on -> c2.nvprolapse.*, off -> the image's software decoders.
DECODE_ROUTE_PROPERTY = "persist.waydroid.nvidia_video_decode"
MEDIA_BASE_PROPERTIES = (
    ("debug.stagefright.ccodec", "4"),
    ("debug.stagefright.omx_default_rank.sw-audio", "4"),    # below c2.android.* audio (8)
    ("debug.stagefright.omx_default_rank.sw-other", "256"),  # 0x100: c2.nvprolapse 0x10 < it < c2.android 0x200
)


def base_properties(root):
    """(key, value) pairs NVIDIA native mode adds to waydroid_base.prop for the deployment at `root`.

    A source root whose manifest stages the media component gets MEDIA_BASE_PROPERTIES; any other root
    (blob root, format 2, media explicitly disabled) gets none, so Waydroid's ccodec=0 stays and only
    OMX codecs are listed. An unreadable or invalid manifest raises OSError (as lxc_nodes() does).
    """
    if not os.path.exists(os.path.join(root, SOURCE_RM_MANIFEST)):
        return []
    manifest = _load_source_rm(root, SOURCE_RM_FORMATS)
    return list(MEDIA_BASE_PROPERTIES) if media_enabled(manifest) else []


def _debugfs(image, request):
    """stdout of `debugfs -R request image` (read-only open); raises OSError on failure."""
    import subprocess
    try:
        # the C locale: the listing is parsed, and waydroid-container.service runs in the system's locale
        out = subprocess.run(["debugfs", "-R", request, image], capture_output=True, timeout=60,
                             env=dict(os.environ, LC_ALL="C"))
    except (OSError, subprocess.TimeoutExpired) as e:
        raise OSError("NVIDIA native mode: cannot run debugfs on {} ({}); e2fsprogs is needed to compare "
                      "the staged /odm with the image".format(image, e))
    if out.returncode != 0:
        raise OSError("NVIDIA native mode: debugfs -R '{}' {} failed: {}".format(
            request, image, out.stderr.decode(errors="replace").strip()))
    return out.stdout


def image_ls(image, path):
    """{name: octal mode} of one directory inside an ext4 image (no '.'/'..')."""
    entries = {}
    for line in _debugfs(image, "ls -p " + path).decode(errors="replace").splitlines():
        parts = line.split("/")
        if len(parts) > 5 and parts[5] not in ("", ".", ".."):
            entries[parts[5]] = int(parts[2], 8)
    if not entries:
        raise OSError("NVIDIA native mode: {} has no directory {} (or debugfs listed nothing)".format(image, path))
    return entries


def vendor_odm_files(vendor_img):
    """{name: (mode, bytes)} of vendor.img /odm/etc; raises unless /odm holds only etc/."""
    top = image_ls(vendor_img, "/odm")
    if set(top) != {"etc"}:
        raise OSError("NVIDIA native mode: {} /odm holds {}, expected only etc/; the staged /odm replaces "
                      "it and would hide the rest. Restage: {}".format(vendor_img, sorted(top), MEDIA_STAGE))
    files = {}
    for name, mode in image_ls(vendor_img, "/odm/etc").items():
        files[name] = (mode & 0o7777, _debugfs(vendor_img, "cat /odm/etc/" + name))
    return files


def _check_source_rm_media(root, manifest):
    """The media component is either staged completely and consistently, or absent everywhere."""
    odm = os.path.join(root, "odm")
    artifacts = manifest["artifacts"]
    libs = ["vendor_extra/lib64/" + n for n in MEDIA_LIBS]
    if not media_enabled(manifest):
        why = ("explicitly disabled ({})".format(manifest["media"]["reason"]) if manifest.get("media")
               else "format {} predates the media component".format(manifest["format"]))
        stray = [p for p in [odm] + [os.path.join(root, l) for l in libs] if os.path.lexists(p)]
        stray += [r for r in artifacts if r.startswith("odm/")]
        if stray:
            raise OSError("Source RM deployment {}: media is {}, but media files are present: {}. Restage: "
                          "{}".format(root, why, ", ".join(sorted(set(stray))), MEDIA_STAGE))
        return
    want = {r: 64 for r in libs}
    want[MEDIA_SERVICE] = 64
    for relative, bits in want.items():
        path = os.path.join(root, relative)
        if relative not in artifacts or _elf_class(path) != bits:
            raise OSError("Source RM deployment lacks the media ELF{} {}. Restage: {}".format(bits, path, MEDIA_STAGE))
    if not os.stat(os.path.join(root, MEDIA_SERVICE)).st_mode & 0o001:
        raise OSError("Source RM deployment: {} is not executable for others (init starts it as uid "
                      "mediacodec). Restage: {}".format(os.path.join(root, MEDIA_SERVICE), MEDIA_STAGE))
    expected = set(MEDIA_ODM_FILES) | {"odm/etc/" + n for n in ODM_ORIGINALS}
    recorded = {r for r in artifacts if r.startswith("odm/")}
    present = set()
    for directory, _, names in os.walk(odm):
        for name in names:
            present.add(os.path.relpath(os.path.join(directory, name), root))
    if recorded != expected or present != expected:
        raise OSError("Source RM deployment {}: the /odm replacement tree must hold exactly {}; manifest records "
                      "{}, tree holds {}. Restage: {}".format(root, sorted(expected), sorted(recorded),
                                                             sorted(present), MEDIA_STAGE))
    vendor_img = manifest["media"]["vendor_img"]
    image = vendor_odm_files(vendor_img)
    if set(image) != set(ODM_ORIGINALS):
        raise OSError("Source RM deployment {}: {} /odm/etc now holds {}, the staged tree carries {} (image "
                      "updated?). Restage: {}".format(root, vendor_img, sorted(image), list(ODM_ORIGINALS),
                                                     MEDIA_STAGE))
    for name, (mode, data) in image.items():
        with open(os.path.join(odm, "etc", name), "rb") as f:
            if f.read() != data:
                raise OSError("Source RM deployment {}: odm/etc/{} differs from {} /odm/etc/{} (image updated?). "
                              "Restage: {}".format(root, name, vendor_img, name, MEDIA_STAGE))


def check_media_device_access(paths, remedy=None):
    """The Codec2 service runs as uid mediacodec with groups camera/mediadrm/drmrpc/graphics. The
    container has no id map, so host owner/group never match those ids: every node the decoder
    opens must be a character device readable and writable by others."""
    import stat as st
    for path in paths:
        try:
            s = os.stat(path)
        except OSError as e:
            raise OSError("NVIDIA native mode: {} is missing ({}); the Codec2 NVDEC service needs it".format(
                path, e.strerror))
        if not st.S_ISCHR(s.st_mode) or s.st_mode & 0o006 != 0o006:
            raise OSError("NVIDIA native mode: {} is mode {:o}; the Codec2 NVDEC service (uid mediacodec, no "
                          "host group) needs a character device with o+rw. Remedy: {}".format(
                              path, s.st_mode, remedy or "NVIDIA device files NVreg_DeviceFileMode=0666 (modprobe.d) "
                              "/ a udev rule for the render node, or restage with --no-media REASON"))


def media_device_nodes(dev=None):
    """The NVIDIA character devices the Codec2 service opens whose mode the host keeps: /dev/nvidiactl and
    /dev/nvidia<N> (NVreg_DeviceFileMode, 0666 by default). It opens the render node too, but that node's host
    mode is the distro's (systemd's 50-udev-default.rules: 0660 before v258, 0666 after) until Waydroid makes
    every /dev/dri/renderD* 0777 right before each container start (tools/actions/container_manager.py
    set_permissions, then lxc.py start): check_start() checks it there, where it counts."""
    dev = dev or DEV_DIR
    return [os.path.join(dev, "nvidiactl")] + sorted(glob.glob(os.path.join(dev, "nvidia[0-9]*")))


def _load_source_rm(root, formats):
    """Parse <root>/source-rm.json; raise OSError unless its format is one of `formats`."""
    manifest_path = os.path.join(root, SOURCE_RM_MANIFEST)
    try:
        with open(manifest_path) as f:
            manifest = json.load(f)
        if manifest["driver"] != "mesa-nvk-rust-rm":
            raise ValueError("unknown source driver manifest")
        fmt = manifest["format"]
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise OSError("Source RM deployment manifest invalid: {}: {}".format(manifest_path, e))
    if fmt == 1 and 1 not in formats:
        raise OSError("Source RM deployment manifest {} is format 1: it records no regression gate run "
                      "and no NVIDIA kernel module pin, so nothing proves these artifacts were tested on "
                      "the loaded module. Remedy, from the waydroid-nvidia-prolapse checkout: {} adopt {} "
                      "(re-verifies the artifacts against build/, runs the regression gate and rewrites the "
                      "manifest as format {})".format(
                          manifest_path, SOURCE_RM_TOOL, root, SOURCE_RM_FORMAT))
    try:
        if fmt not in formats:
            raise ValueError("unsupported format {!r} (expected {})".format(fmt, " or ".join(map(str, formats))))
        abis = manifest["graphics_abis"]
        if not isinstance(abis, list) or not abis or set(abis) - {"x86_64", "x86"}:
            raise ValueError("invalid graphics_abis")
        artifacts = manifest["artifacts"]
        if not isinstance(artifacts, dict):
            raise ValueError("artifacts must be an object")
        if fmt >= 2:
            pinned = manifest["kernel_module"]
            if not isinstance(pinned, str) or not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", pinned):
                raise ValueError("invalid kernel_module {!r}".format(pinned))
            gate = manifest["gate"]
            if not isinstance(gate, dict) or gate.get("exit") != 0 or gate.get("kernel_module") != pinned:
                raise ValueError("gate record does not show a passing run on kernel module " + pinned)
            for key in ("runner", "finished", "log_dir"):
                if not isinstance(gate.get(key), str) or not gate[key]:
                    raise ValueError("gate record lacks " + key)
            exercised = gate.get("exercised")
            if not isinstance(exercised, list) or not exercised or set(exercised) - set(artifacts):
                raise ValueError("gate.exercised must list manifest artifacts")
        if fmt >= 3:
            media = manifest["media"]
            if not isinstance(media, dict) or not isinstance(media.get("enabled"), bool):
                raise ValueError("media must be an object with a boolean 'enabled'")
            if media["enabled"]:
                mgate = media.get("gate")
                if not isinstance(mgate, dict) or mgate.get("exit") != 0 or mgate.get("kernel_module") != pinned:
                    raise ValueError("media.gate does not show a passing media gate on kernel module " + pinned)
                if not isinstance(media.get("vendor_img"), str) or not os.path.isabs(media["vendor_img"]):
                    raise ValueError("media.vendor_img must be the absolute vendor.img path it was staged from")
                codecs = media.get("codecs")
                if not isinstance(codecs, list) or not codecs or not all(
                        isinstance(c, str) and c.startswith("c2.nvprolapse.") for c in codecs):
                    raise ValueError("media.codecs must list the staged c2.nvprolapse.* decoders")
            elif not isinstance(media.get("reason"), str) or not media["reason"].strip():
                raise ValueError("media.enabled = false needs a non-empty 'reason' (explicit opt-out)")
        if "native_gl" in manifest:
            if fmt < 3:
                raise ValueError("native_gl needs format 3")
            gl = manifest["native_gl"]
            if not isinstance(gl, dict) or not isinstance(gl.get("enabled"), bool):
                raise ValueError("native_gl must be an object with a boolean 'enabled'")
            if gl["enabled"]:
                if gl.get("egl") != NVRM_EGL:
                    raise ValueError("native_gl.egl must be {!r}".format(NVRM_EGL))
                if not isinstance(gl.get("files"), dict) or not gl["files"]:
                    raise ValueError("native_gl.files must map the staged nvrm files to their sha256")
                ggate = gl.get("gate")
                if not isinstance(ggate, dict) or ggate.get("exit") != 0 or ggate.get("kernel_module") != pinned:
                    raise ValueError("native_gl.gate does not show a passing GL smoke test on kernel module " + pinned)
                for key in ("runner", "finished", "log_dir"):
                    if not isinstance(ggate.get(key), str) or not ggate[key]:
                        raise ValueError("native_gl.gate lacks " + key)
                if not isinstance(ggate.get("rust_sha256"), dict):
                    raise ValueError("native_gl.gate.rust_sha256 must map each ABI to the Rust RM library nvrm linked")
            elif not isinstance(gl.get("reason"), str) or not gl["reason"].strip():
                raise ValueError("native_gl.enabled = false needs a non-empty 'reason' (explicit opt-out)")
    except (ValueError, KeyError, TypeError) as e:
        raise OSError("Source RM deployment manifest invalid: {}: {}".format(manifest_path, e))
    return manifest


def _check_source_rm_kernel(root, manifest):
    """The Rust RM sends whatever version /proc reports in the RM version check, so the RM itself
    accepts any module; its control/class layouts were verified only on the gated one."""
    pinned = manifest["kernel_module"]
    running = driver_version()
    if running != pinned:
        raise OSError("Source RM deployment {} was gated on NVIDIA kernel module {}, but the loaded module "
                      "is {}. Its RM control and class layouts are verified only on {}; refusing to issue "
                      "them to another RM. Remedy (this dev-root is a developer checkout's): verify and "
                      "rebuild the source stack on {}, then {} stage NEWROOT and {} activate NEWROOT; or go "
                      "back to a saved configuration with {} restore BACKUP (saved as {})".format(
                          root, pinned, running, pinned, running, SOURCE_RM_TOOL, SOURCE_RM_TOOL,
                          SOURCE_RM_TOOL, SOURCE_RM_BACKUPS))


def _check_source_rm_artifacts(root, manifest):
    """Every required artifact present with the right ELF class and its recorded SHA256;
    no proprietary driver or glibc shim anywhere in the tree."""
    abis = manifest["graphics_abis"]
    artifacts = manifest["artifacts"]
    required = {}
    for sub, bits, abi in (("lib64", 64, "x86_64"), ("lib", 32, "x86")):
        for name in (GBM_WRAPPER, MINIGBM_CORE):
            required["vendor_extra/{}/{}".format(sub, name)] = bits
        for name in EGL_ANGLE:
            required["vendor/{}/egl/{}".format(sub, name)] = bits
        if abi in abis:
            for name in (HAL, "libvulkan_nouveau.so", "libnv_bionic_native.so", LIBDRM):
                required["vendor_extra/{}/{}".format(sub, name)] = bits
    for relative, bits in required.items():
        path = os.path.join(root, relative)
        if relative not in artifacts or _elf_class(path) != bits:
            raise OSError("Source RM deployment lacks ELF{} {}. Rebuild and stage that ABI.".format(bits, path))
    for relative, digest in artifacts.items():
        if os.path.isabs(relative) or ".." in relative.split("/"):
            raise OSError("Source RM manifest path escapes deployment: " + relative)
        path = os.path.join(root, relative)
        try:
            with open(path, "rb") as f:
                actual = hashlib.file_digest(f, "sha256").hexdigest()
        except PermissionError as e:
            # root-only staged file (odm/etc/build.prop is 0600 like vendor.img): not missing, verify as root
            raise PermissionError(e.errno, "Source RM artifact not readable as uid {} (verify as root: sudo -n "
                                  "wd-native verify)".format(os.geteuid()), path) from e
        except OSError as e:
            raise OSError("Source RM artifact missing: {}: {}".format(path, e))
        if actual != digest:
            raise OSError("Source RM artifact hash changed: {}. Rebuild and restage.".format(path))
    for directory, _, names in os.walk(os.path.join(root, "vendor_extra")):
        for name in names:
            if name.startswith(("libnvidia-", "libEGL_nvidia", "libnvglibc", "libc.so.6", "libpthread.so.0")):
                raise OSError("Source RM deployment contains a proprietary driver/shim: " + os.path.join(directory, name))


def verify_source_rm(root, formats=SOURCE_RM_FORMATS):
    """Validate an explicit source-driver deployment; never mix it with blobs. Returns the manifest.

    The manifest records every staged artifact's SHA256, the regression gate that passed on those
    bytes and the NVIDIA kernel module it passed on (format 2). A partial, stale, ungated or
    differently-pinned deployment fails before LXC starts. Graphics ABIs are explicit; both mapper
    ABIs remain required by this Waydroid image. formats including 1 are for dev/source-native adopt only.
    Format 3 adds the media record (_check_source_rm_media): staged and gated, or explicitly disabled,
    and the optional native_gl record (_check_source_rm_native_gl; absent = nvrm not staged).
    """
    manifest = _load_source_rm(root, formats)
    if manifest["format"] >= 2:
        _check_source_rm_kernel(root, manifest)
    _check_source_rm_artifacts(root, manifest)
    if manifest["format"] >= 2:
        _check_source_rm_media(root, manifest)
        _check_source_rm_native_gl(root, manifest)
    return manifest


def _source_rm_nodes(root, manifest=None):
    manifest = manifest or verify_source_rm(root)
    nodes = ["lxc.mount.entry = {} vendor_extra none rbind 0 0".format(os.path.join(root, "vendor_extra"))]
    for sub in ("lib64", "lib"):
        for name in (GBM_WRAPPER, MINIGBM_CORE):
            nodes.append("lxc.mount.entry = {} vendor/{}/{} none bind 0 0".format(
                os.path.join(root, "vendor_extra", sub, name), sub, name))
        nodes.append("lxc.mount.entry = {} vendor/{}/egl none bind 0 0".format(
            os.path.join(root, "vendor", sub, "egl"), sub))
    if media_enabled(manifest):
        # Mandatory with media: the service, its .rc/VINTF/seccomp/codec files and vendor.img's three
        # /odm/etc files (verified equal to the image above) replace /vendor/odm as one tree.
        nodes.append("lxc.mount.entry = {} vendor/odm none rbind 0 0".format(os.path.join(root, "odm")))
    return nodes


def lxc_nodes(root, host_libdir=None, dev=None, existing=(), host_libdir32=None):
    """lxc.mount.entry lines for NVIDIA native mode. Raises OSError naming the missing piece.

    host_libdir/host_libdir32/dev/ANGLE_GUEST_ROOT default to the module constants, read at call time
    (tests point them at a fake tree). `existing`: the entries generated so far; when one already
    binds the NVIDIA render node (waydroid.cfg drm_device names it, so upstream gpu.py emitted it),
    it is not bound a second time.
    """
    host_libdir = host_libdir or HOST_LIBDIR
    host_libdir32 = host_libdir32 or HOST_LIBDIR32
    dev = dev or DEV_DIR
    if os.path.exists(os.path.join(root, SOURCE_RM_MANIFEST)):
        manifest = verify_source_rm(root)
        devices = _device_nodes(dev, existing)
        if media_enabled(manifest):
            check_media_device_access([os.path.join(dev, "nvidiactl")] +
                                      sorted(glob.glob(os.path.join(dev, "nvidia[0-9]*"))) +
                                      [nvidia_render_node()])
        return _source_rm_nodes(root, manifest) + devices
    v = driver_version()
    staging = os.path.join(root, "vendor_extra")
    libdir = _check_abi_tree(staging, "lib64", SHIMS, v, 64)
    libdir32 = _check_abi_tree(staging, "lib", SHIMS32, v, 32)
    egl = [(sub, _check_egl_tree(root, sub, bits, ANGLE_GUEST_ROOT)) for sub, bits in (("lib64", 64), ("lib", 32))]
    wrapper = os.path.join(libdir, GBM_WRAPPER)
    wrapper32 = os.path.join(libdir32, GBM_WRAPPER)

    nodes = ["lxc.mount.entry = {} vendor_extra none rbind 0 0".format(staging)]
    for hostdir, guestdir, pkg in ((host_libdir, GUEST_LIBDIR, "nvidia-utils"),
                                   (host_libdir32, GUEST_LIBDIR32, "lib32-nvidia-utils")):
        for blob in BLOBS:
            name = blob.format(v=v)
            src = os.path.join(hostdir, name)
            if not os.path.isfile(src):
                raise OSError("NVIDIA native mode: {} is missing although the kernel module is {} "
                              "({} not installed, or a different version?)".format(src, v, pkg))
            nodes.append("lxc.mount.entry = {} {}/{} none rbind,create=file 0 0".format(src, guestdir, name))
    nodes.append("lxc.mount.entry = {} {} none bind 0 0".format(wrapper, GBM_WRAPPER_GUEST))
    nodes.append("lxc.mount.entry = {} {} none bind 0 0".format(wrapper32, GBM_WRAPPER_GUEST32))
    nodes.append("lxc.mount.entry = {} {} none bind 0 0".format(os.path.join(libdir, MINIGBM_CORE),
                                                                MINIGBM_CORE_GUEST))
    nodes.append("lxc.mount.entry = {} {} none bind 0 0".format(os.path.join(libdir32, MINIGBM_CORE),
                                                                MINIGBM_CORE_GUEST32))
    # The directory first: a file bind made before it would be hidden by it.
    for sub, egldir in egl:
        guest = "vendor/{}/egl".format(sub)
        nodes.append("lxc.mount.entry = {} {} none bind 0 0".format(egldir, guest))
        for name in EGL_ANGLE:
            nodes.append("lxc.mount.entry = {} {}/{} none bind 0 0".format(
                os.path.join(ANGLE_GUEST_ROOT, "vendor", sub, "egl", name), guest, name))

    return nodes + _device_nodes(dev, existing)


def _device_nodes(dev, existing):
    nodes = []
    devices = [os.path.join(dev, "nvidiactl"), os.path.join(dev, "nvidia-modeset")]
    devices += sorted(glob.glob(os.path.join(dev, "nvidia[0-9]*")))
    if len(devices) < 3:
        raise OSError("NVIDIA native mode: no {}/nvidia<N> device node found".format(dev))
    for node in devices:
        if not os.path.exists(node):
            raise OSError("NVIDIA native mode: device node {} is missing (nvidia-modeset not "
                          "loaded? run nvidia-modprobe -m)".format(node))
        guest = os.path.join("dev", os.path.relpath(node, dev))
        nodes.append("lxc.mount.entry = {} {} none bind,create=file 0 0".format(node, guest))
    render = nvidia_render_node()
    # gpu.py's entry for drm_device is 'optional', but lxc.py's add_node_entry() emits it only for
    # a node that exists, so skipping ours never leaves the node unbound.
    if not any(_binds(e, render) for e in existing):
        nodes.append("lxc.mount.entry = {} {} none bind,create=file 0 0".format(render, render[1:]))
    return nodes


# ---------------------------------------------------------------- GPU readiness (container start)
#
# The container start needs the GPU the generator binds: the NVIDIA DRM render node (all modes; drm_device)
# and, for the native modes, /dev/nvidiactl, /dev/nvidia<minor> of every GPU and /dev/nvidia-modeset
# (_device_nodes). They have two different producers, so there are two steps:
#  * the render node is a kernel device (nvidia-drm, drm_dev_register) that udev announces. The package's
#    70-waydroid-nvidia-gpu.rules names the NVIDIA one (DRIVERS=="nvidia": by driver, never by node number) as the
#    systemd device unit RENDER_DEVICE_UNIT, and the container unit's drop-in (dev/source-native
#    render_override) Wants= it and is After= it: systemd starts the container once that node exists and
#    udev has initialized it, or after DefaultDeviceTimeoutSec, when ensure_gpu_ready() fails naming the
#    remedy. nvidia-drm loads only after nvidia and nvidia-modeset finished their module init, and nvidia's
#    init returns only after every GPU's probe (nv_pci_register_driver -> nv_pci_wait_for_probe_complete), so
#    from then on /proc/driver/nvidia/gpus lists every GPU the module drives.
#  * the /dev/nvidia* character files have no kernel device behind them: the modules only register char
#    regions (register_chrdev_region + cdev_add, no device_create), so neither devtmpfs nor udev creates
#    them. NVIDIA's userspace does, through nvidia-modprobe (open source; -c MINOR, -m), with the
#    NVreg_DeviceFile{UID,GID,Mode} the module was loaded with. Whether anything did so before the container
#    starts depends on unrelated packages (a distro udev rule, a CUDA/CDI service, a desktop client), so
#    ensure_gpu_ready() runs nvidia-modprobe itself for the files that are missing.
PROC_GPUS = "/proc/driver/nvidia/gpus"
NVIDIA_MODPROBE = "nvidia-modprobe"
NV_CTL_MINOR = 255                       # /dev/nvidiactl is 195:255
RENDER_DEVICE_ALIAS = "/sys/subsystem/waydroid_nvidia/render"   # SYSTEMD_ALIAS in 70-waydroid-nvidia-gpu.rules
RENDER_DEVICE_UNIT = "sys-subsystem-waydroid_nvidia-render.device"   # systemd-escape --path --suffix=device


def gpu_minors(proc_gpus=None):
    """Device minors of the GPUs the loaded NVIDIA kernel module drives (/proc/driver/nvidia/gpus/<bdf>/
    information, 'Device Minor:'), sorted. Raises OSError when the module drives none."""
    proc_gpus = proc_gpus or PROC_GPUS
    minors = []
    for info in sorted(glob.glob(os.path.join(proc_gpus, "*", "information"))):
        with open(info) as f:
            m = re.search(r"^Device Minor:\s*(\d+)\s*$", f.read(), re.M)
        if not m:
            raise OSError("NVIDIA GPU: no 'Device Minor:' line in {}".format(info))
        minors.append(int(m.group(1)))
    if not minors:
        raise OSError("NVIDIA GPU: the NVIDIA kernel module drives no GPU ({} lists none); check "
                      "journalctl -b -k | grep NVRM".format(proc_gpus))
    return sorted(minors)


def ensure_gpu_ready(native, dev=None, proc_gpus=None, run=None, which=None):
    """Container-start precondition (driver-mode verify-active, the waydroid-container.service ExecStartPre
    that runs after RENDER_DEVICE_UNIT): the NVIDIA render node exists and, for the native modes (`native`),
    so do /dev/nvidiactl, /dev/nvidia<minor> of every GPU and /dev/nvidia-modeset; missing character files
    are created with nvidia-modprobe. Returns the files it created. Raises OSError naming the remedy."""
    import shutil
    import stat as st
    import subprocess
    dev = dev or DEV_DIR
    run = run or subprocess.run
    which = which or shutil.which
    try:
        render = nvidia_render_node()
        if not os.path.exists(render):
            raise OSError("{} (NVIDIA by PCI vendor in sysfs) has no device file".format(render))
    except OSError as e:
        raise OSError("NVIDIA GPU not up at container start: {}. waydroid-container.service waited for {} (the "
                      "NVIDIA DRM render node, named by /usr/lib/udev/rules.d/70-waydroid-nvidia-gpu.rules) up to "
                      "systemd's DefaultDeviceTimeoutSec. Check: journalctl -b -k | grep -iE 'NVRM|nvidia-drm'; "
                      "lsmod | grep nvidia_drm. Remedy: load the NVIDIA kernel modules with nvidia-drm.modeset=1 "
                      "(kernel parameter nvidia_drm.modeset=1), then: sudo systemctl start waydroid-container".format(
                          e, RENDER_DEVICE_UNIT))
    if not native:
        return []
    minors = gpu_minors(proc_gpus)
    ctl, modeset = os.path.join(dev, "nvidiactl"), os.path.join(dev, "nvidia-modeset")
    gpus = [os.path.join(dev, "nvidia{}".format(n)) for n in minors]
    wanted = [ctl] + gpus + [modeset]
    missing = [p for p in wanted if not os.path.exists(p)]
    if missing:
        tool = which(NVIDIA_MODPROBE)
        if not tool:
            raise OSError("NVIDIA GPU: {} missing and {} is not installed. The NVIDIA kernel modules create no "
                          "device files; NVIDIA's {} does (Arch: nvidia-utils; Debian/Ubuntu: nvidia-modprobe; "
                          "the .run installer installs it). Remedy: install it, then: sudo systemctl start "
                          "waydroid-container".format(", ".join(missing), NVIDIA_MODPROBE, NVIDIA_MODPROBE))
        calls = []
        if ctl in missing or any(p in missing for p in gpus):
            # one -c per file: minor 255 is /dev/nvidiactl (nvidia-modprobe-utils.h NV_CTL_DEVICE_NUM)
            calls.append([tool] + [a for n in minors + [NV_CTL_MINOR] for a in ("-c", str(n))])
        if modeset in missing:
            calls.append([tool, "-m"])                                          # nvidia-modeset
        for argv in calls:
            out = run(argv, capture_output=True, text=True, timeout=120)
            if out.returncode != 0:
                raise OSError("NVIDIA GPU: '{}' failed (exit {}): {}".format(
                    " ".join(argv), out.returncode, (out.stderr or out.stdout).strip()[-400:]))
    for path in wanted:
        try:
            mode = os.stat(path).st_mode
        except OSError as e:
            raise OSError("NVIDIA GPU: {} is still missing after {} ({}); check NVreg_ModifyDeviceFiles in "
                          "/proc/driver/nvidia/params and journalctl -b -k | grep NVRM".format(
                              path, NVIDIA_MODPROBE, e.strerror))
        if not st.S_ISCHR(mode):
            raise OSError("NVIDIA GPU: {} is not a character device (mode {:o}); remove it, then: sudo systemctl "
                          "start waydroid-container".format(path, mode))
    return missing


# ---------------------------------------------------------------- nvidia-prolapse vendor image

# Build props our product writes into vendor.img /build.prop (vendor-image/build-vendor.sh, product stage):
# the release, and the sha256 of the native bundle's BUILD-RECORD.json whose binaries the image carries.
VERSION_PROP = "ro.vendor.nvidia_prolapse.version"
BUILD_RECORD_PROP = "ro.vendor.nvidia_prolapse.build_record"
# What the setup (packaging/native/vendor_image.py) leaves for the generator: the install record (written
# only after the image passed the gate on this GPU and kernel module) and this GPU's Codec2 listing.
VENDOR_IMAGE_STATE = "/var/lib/waydroid-nvidia/vendor-image"
VENDOR_IMAGE_RECORD = "install.json"
VENDOR_IMAGE_RECORD_FORMAT = 2
VENDOR_IMAGE_XML = "media_codecs_c2.xml"
XMLGEN_MARK = "Generated by nvc2-xmlgen"
# A GPU without an NVDEC class the Codec2 service drives (before Turing: the gate's nvc2-xmlgen exits 3) runs the
# image without hardware decode (install record media.hardware_decode = false). The setup then writes these files
# into VENDOR_IMAGE_STATE and the generator binds them over the image's: the service's .rc with `oneshot` (it
# starts once, checks the codec listing, logs the reason and exits) and a VINTF fragment that declares no Codec2
# HAL (the media framework then never waits for one and lists Android's software decoders only).
VENDOR_IMAGE_NO_NVDEC = {MEDIA_RC: "no-nvdec.rc", MEDIA_VINTF: "no-nvdec-vintf.xml"}
NO_NVDEC_VINTF_TEXT = ("<!-- Written by waydroid-nvidia-setup: this GPU has no NVDEC class the Codec2 NVDEC decoder "
                       "drives,\n     so no Codec2 HAL is declared. -->\n"
                       "<manifest version=\"1.0\" type=\"device\">\n</manifest>\n")


def no_nvdec_rc(image_rc):
    """The image's Codec2 service .rc with `oneshot` added: on a GPU without hardware decode the service runs
    once (it logs why and exits) instead of being restarted. Raises OSError unless the .rc is exactly one
    `service` section for MEDIA_INIT_SERVICE (anything else would need more than an added option)."""
    lines = image_rc.splitlines()
    sections = [l for l in lines if l and not l[0].isspace() and not l.startswith("#")]
    if len(sections) != 1 or sections[0].split()[:2] != ["service", MEDIA_INIT_SERVICE]:
        raise OSError("the vendor image's {} is not a single `service {}` section ({}); it cannot be run "
                      "once on a GPU without hardware decode".format(MEDIA_RC, MEDIA_INIT_SERVICE, sections))
    if any(l.split() == ["oneshot"] for l in lines):
        raise OSError("the vendor image's {} already says oneshot".format(MEDIA_RC))
    return image_rc.rstrip("\n") + "\n    # waydroid-nvidia-setup: this GPU has no hardware decoder; start once, log why\n" \
        "    oneshot\n"
IMAGES_PATH = "/var/lib/waydroid/images"
# The installed package's release bundle: when present, the image must carry exactly it.
PACKAGED_BUNDLE_RECORD = "/usr/lib/waydroid-nvidia/native/release/BUILD-RECORD.json"
RESTORE_OFFICIAL = "sudo waydroid-nvidia-setup --restore-official-vendor"


def vendor_img_path(waydroid):
    """The vendor.img Waydroid boots (waydroid.cfg images_path)."""
    return os.path.join(waydroid.get("images_path") or IMAGES_PATH, "vendor.img")


def image_build_props(image):
    """{key: value} of an ext4 vendor image's /build.prop (debugfs, read-only; root: vendor.img is 0600)."""
    if not os.path.isfile(image):
        raise OSError("NVIDIA native mode: {} is missing; run 'waydroid init' (then {})".format(image, SETUP))
    props = {}
    for line in _debugfs(image, "cat /build.prop").decode(errors="replace").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            props[key.strip()] = value.strip()
    if "ro.vendor.build.fingerprint" not in props:
        raise OSError("NVIDIA native mode: {} has no readable /build.prop (not a vendor image?)".format(image))
    return props


def describe_image(props):
    what = "the nvidia-prolapse vendor image {}".format(props[VERSION_PROP]) if VERSION_PROP in props else \
        "a vendor image without nvidia-prolapse"
    return "{} ({}, built {})".format(what, props.get("ro.vendor.build.fingerprint"), props.get("ro.vendor.build.date"))


def _sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def load_vendor_image_record(state=None):
    """The setup's install record (VENDOR_IMAGE_STATE/install.json), validated."""
    path = os.path.join(state or VENDOR_IMAGE_STATE, VENDOR_IMAGE_RECORD)
    try:
        with open(path) as f:
            record = json.load(f)
        if record["format"] != VENDOR_IMAGE_RECORD_FORMAT:
            raise ValueError("format {!r}, expected {}".format(record["format"], VENDOR_IMAGE_RECORD_FORMAT))
        for key in ("version", "build_record_sha256", "kernel_module"):
            if not isinstance(record[key], str) or not record[key]:
                raise ValueError("no " + key)
        for key in ("path", "sha256"):
            if not isinstance(record["vendor_img"][key], str):
                raise ValueError("no vendor_img." + key)
        if record["gate"]["exit"] != 0 or record["gate"]["kernel_module"] != record["kernel_module"]:
            raise ValueError("gate record does not show a passing gate on kernel module " + record["kernel_module"])
        if not isinstance(record["native_gl"], bool) or not isinstance(record["media"]["xml_sha256"], str):
            raise ValueError("native_gl/media.xml_sha256 missing")
        if not isinstance(record["media"].get("hardware_decode"), bool):
            raise ValueError("media.hardware_decode missing")
        no_nvdec = record["media"].get("no_nvdec")
        if not record["media"]["hardware_decode"] and (
                not isinstance(record["media"].get("reason"), str) or not isinstance(no_nvdec, dict) or
                set(no_nvdec) != set(VENDOR_IMAGE_NO_NVDEC) or not all(isinstance(v, str) for v in no_nvdec.values())):
            raise ValueError("media.reason and media.no_nvdec (the sha256 of " + ", ".join(sorted(VENDOR_IMAGE_NO_NVDEC))
                             + ") must be set without hardware decode")
    except FileNotFoundError:
        raise OSError("NVIDIA driver mode nvidia-prolapse: {} is missing: the vendor image was never gated and "
                      "installed on this machine. Remedy: {}".format(path, SETUP))
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise OSError("NVIDIA driver mode nvidia-prolapse: install record {} is invalid ({}). Remedy: {}".format(
            path, e, SETUP))
    return record


def check_vendor_image(dm, waydroid, state=None, packaged_record=None):
    """nvidia_prolapse_install = vendor-image: the booted vendor.img is ours, the very image the setup gated on
    this kernel module (sha256), from the installed package's release; the codec listing is the one that
    gate generated. Returns the install record; raises OSError naming the remedy."""
    state = state or VENDOR_IMAGE_STATE
    packaged_record = packaged_record or PACKAGED_BUNDLE_RECORD
    image = vendor_img_path(waydroid)
    props = image_build_props(image)
    where = "NVIDIA driver mode nvidia-prolapse ({} = vendor-image)".format(INSTALL_KEY)
    if VERSION_PROP not in props or BUILD_RECORD_PROP not in props:
        raise OSError("{}: {} is not the nvidia-prolapse vendor image: it is {} (no {} in its /build.prop). An "
                      "official vendor image replaced it ('waydroid init -f', or a vendor OTA from Waydroid's own "
                      "channel), or the setup never installed it. Remedy: {} (installs the vendor image of the "
                      "installed release and keeps this one as the official backup)".format(
                          where, image, describe_image(props), VERSION_PROP, SETUP))
    record = load_vendor_image_record(state)
    problems = []
    if record["vendor_img"]["path"] != image:
        problems.append("the setup installed {}, Waydroid boots {}".format(record["vendor_img"]["path"], image))
    if record["build_record_sha256"] != props[BUILD_RECORD_PROP]:
        problems.append("{} carries release bundle {} ({}), the setup gated {} ({})".format(
            image, props[BUILD_RECORD_PROP][:12], props[VERSION_PROP], record["build_record_sha256"][:12],
            record["version"]))
    elif _sha256_file(image) != record["vendor_img"]["sha256"]:
        problems.append("{} is not the file the setup gated (sha256 {} expected)".format(
            image, record["vendor_img"]["sha256"][:16]))
    if os.path.isfile(packaged_record) and _sha256_file(packaged_record) != record["build_record_sha256"]:
        problems.append("the installed package carries release bundle {} ({}), the vendor image {} (the package "
                        "was updated after the setup ran)".format(_sha256_file(packaged_record)[:12], packaged_record,
                                                                 record["build_record_sha256"][:12]))
    if problems:
        raise OSError("{}: {}. Remedy: {}".format(where, "; ".join(problems), SETUP))
    running = driver_version()
    if running != record["kernel_module"]:
        raise OSError("{}: the vendor image {} was gated on NVIDIA kernel module {}, the loaded module is {} (driver "
                      "update). Its RM control and class layouts are verified only on the gated module, and the codec "
                      "list describes it. Remedy: {} (re-gates the image on the loaded module and regenerates the "
                      "codec list)".format(where, record["version"], record["kernel_module"], running, SETUP))
    if dm.native_gl and not record["native_gl"]:
        raise _cfg_error("{} = on: the install record of {} has no passing nvrm gate. Set {} = off (ANGLE), or re-run "
                         "{}".format(GL_KEY, record["version"], GL_KEY, SETUP))
    xml = os.path.join(state, VENDOR_IMAGE_XML)
    try:
        with open(xml, "rb") as f:
            data = f.read()
    except OSError as e:
        raise OSError("{}: this GPU's Codec2 listing {} is missing ({}); without it the Codec2 NVDEC service refuses "
                      "to start. Remedy: {} (generates it with nvc2-xmlgen)".format(where, xml, e.strerror, SETUP))
    if XMLGEN_MARK.encode() not in data or hashlib.sha256(data).hexdigest() != record["media"]["xml_sha256"]:
        raise OSError("{}: {} is not the nvc2-xmlgen output the setup's gate recorded. Remedy: {}".format(
            where, xml, SETUP))
    if not record["media"]["hardware_decode"]:
        for target, name in sorted(VENDOR_IMAGE_NO_NVDEC.items()):
            path = os.path.join(state, name)
            if not os.path.isfile(path) or _sha256_file(path) != record["media"]["no_nvdec"][target]:
                raise OSError("{}: {} (the image's /{} for a GPU without hardware decode) is missing or not the file "
                              "the setup wrote. Remedy: {}".format(where, path, target, SETUP))
    return record


def _codec_list_entry(state=None):
    """The vendor-image install's bind of this GPU's Codec2 listing over the image's placeholder (the image's
    /odm/etc/media_codecs_c2.xml lists no codecs, vendor-image/product)."""
    return "lxc.mount.entry = {} vendor/{} none bind,ro 0 0".format(
        os.path.join(state or VENDOR_IMAGE_STATE, VENDOR_IMAGE_XML), MEDIA_XML)


def vendor_image_nodes(dm, waydroid, dev=None, existing=(), state=None):
    """lxc.mount.entry lines of the vendor-image install: device nodes and the codec listing, and on a GPU without
    hardware decode the service .rc and VINTF fragment of VENDOR_IMAGE_NO_NVDEC; nothing else."""
    state = state or VENDOR_IMAGE_STATE
    dev = dev or DEV_DIR
    record = check_vendor_image(dm, waydroid, state=state)
    devices = _device_nodes(dev, existing)
    check_media_device_access(media_device_nodes(dev))
    no_nvdec = [] if record["media"]["hardware_decode"] else [
        "lxc.mount.entry = {} vendor/{} none bind,ro 0 0".format(os.path.join(state, name), target)
        for target, name in sorted(VENDOR_IMAGE_NO_NVDEC.items())]
    return devices + [_codec_list_entry(state)] + no_nvdec


def refuse_prolapse_image(dm, waydroid):
    """glibc-shim, venus and dev-root bind their files over the OFFICIAL vendor image; ours would shadow them
    (its /vendor/<lib>/hw/vulkan.nvidia.so comes before /vendor_extra in the sphal search path)."""
    image = vendor_img_path(waydroid)
    props = image_build_props(image)
    if VERSION_PROP in props:
        what = "{} = {}".format(MODE_KEY, dm.mode) + (" with {} = dev-root".format(INSTALL_KEY) if dm.install else "")
        raise OSError("NVIDIA driver mode: {} runs on the official vendor image, but {} is {}. Remedy: {} (puts the "
                      "official image back{}), or {} = nvidia-prolapse with {} = vendor-image".format(
                          what, image, describe_image(props), RESTORE_OFFICIAL,
                          "; sudo waydroid-nvidia-setup --mode venus does it too" if dm.mode == "venus" else "",
                          MODE_KEY, INSTALL_KEY))


def mode_nodes(dm, waydroid, existing=()):
    """Every lxc.mount.entry the configured mode adds (lxc.py generate_nodes_lxc_config, check_cfg)."""
    if dm.vendor_image:
        return vendor_image_nodes(dm, waydroid, existing=existing)
    refuse_prolapse_image(dm, waydroid)
    return lxc_nodes(dm.root, existing=existing) if dm.native else []


# `waydroid init` installs official images only (downloaded or preinstalled); the nvidia-prolapse vendor image comes
# from the setup afterwards, and on a fresh install the setup itself runs `waydroid init` first. So at init the
# vendor-image install's entries are deferred instead of refused (init_nodes), and that refusal is made at container
# start (check_start), where booting the wrong vendor image would actually happen.
INIT_NEXT_STEP = ("NVIDIA driver mode nvidia-prolapse: Waydroid is initialized, but the container will not start "
                  "until the nvidia-prolapse vendor image is installed. Next step: " + SETUP)


def init_nodes(dm, waydroid, existing=()):
    """`waydroid init` (initializer.py init -> lxc.py generate_nodes_lxc_config): mode_nodes(), except that the
    vendor-image install accepts a vendor.img that is not (or no longer) the image the setup gated. Returns
    (entries, pending): pending is None, or check_vendor_image()'s refusal; the entries then lack the mode's nodes
    and the container start refuses (check_start) until the setup installed the image and regenerated them. Every
    other check (the cfg, the other modes' roots and images) refuses here as it does at upgrade."""
    if dm.vendor_image:
        try:
            check_vendor_image(dm, waydroid)
        except OSError as e:
            return [], "{} (deferred: 'waydroid init' installs official images only)".format(e)
    return mode_nodes(dm, waydroid, existing=existing), None


def check_start(dm, waydroid, config_nodes, state=None):
    """Container start (lxc.py start), vendor-image install: the booted vendor.img is the image the setup gated
    (check_vendor_image; a stock image after 'waydroid init -f' refuses here with the setup as the remedy), and
    config_nodes carries the generator's entries for it, not the deferred set `waydroid init` wrote before the
    image was installed (init_nodes). The other modes refuse at generator time only."""
    if not dm.vendor_image:
        return
    record = check_vendor_image(dm, waydroid, state=state)
    check_media_device_access([nvidia_render_node()], remedy="Waydroid's container start makes every "
                              "/dev/dri/renderD* 0777 just before this check (container_manager.py set_permissions); "
                              "restart waydroid-container.service, and report it if the mode stays")
    try:
        with open(config_nodes) as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise OSError("NVIDIA driver mode nvidia-prolapse: cannot read {} ({}). Remedy: {}".format(
            config_nodes, e.strerror, SETUP))
    if _codec_list_entry(state) not in lines:
        raise OSError("NVIDIA driver mode nvidia-prolapse ({} = vendor-image): {} was generated while the vendor "
                      "image was not installed (by 'waydroid init'): it binds no NVIDIA device and no codec list. "
                      "Remedy: {}".format(INSTALL_KEY, config_nodes, SETUP))
    no_nvdec = [l for l in lines if any(os.path.join(state or VENDOR_IMAGE_STATE, n) + " " in l
                                        for n in VENDOR_IMAGE_NO_NVDEC.values())]
    if len(no_nvdec) != (0 if record["media"]["hardware_decode"] else len(VENDOR_IMAGE_NO_NVDEC)):
        raise OSError("NVIDIA driver mode nvidia-prolapse ({} = vendor-image): {} does not match the install record "
                      "(hardware video decode {}). Remedy: sudo waydroid upgrade -o".format(
                          INSTALL_KEY, config_nodes, "available" if record["media"]["hardware_decode"] else
                          "not available"))


def load_cfg(path):
    """[waydroid] and [properties] of a waydroid.cfg as dicts (configparser, as tools.config does)."""
    import configparser
    cfg = configparser.ConfigParser()
    cfg.optionxform = str                     # property names are case-sensitive
    if not cfg.read(path):
        raise OSError("NVIDIA driver mode: cannot read {}".format(path))
    section = lambda name: dict(cfg[name]) if cfg.has_section(name) else {}
    return section("waydroid"), section("properties")


def check_cfg(path, nodes=True):
    """Everything 'waydroid upgrade -o' checks for the cfg at `path`, without writing anything:
    the mode, the [properties] rules and (nodes=True) the native generator. Returns (dm, entries)."""
    waydroid, properties = load_cfg(path)
    dm = driver_mode(waydroid)
    if dm is None:
        raise OSError("NVIDIA driver mode: {} sets no {} (stock Waydroid, nothing to check)".format(path, MODE_KEY))
    check_properties(properties, dm)
    mode_properties(dm)                      # reads the root's manifest (media record)
    if dm.native and dm.guest_layout and waydroid.get(LAYOUT_KEY) != "2":
        raise _cfg_error("{} reuses ANGLE, hwcomposer and surfaceflinger from /var/lib/waydroid/nv/guest and "
                         "needs {} = 2".format(dm.mode + (" (dev-root)" if dm.install else "") + " mode", LAYOUT_KEY))
    if dm.mode == "venus":
        check_venus(dm, waydroid)
    entries = mode_nodes(dm, waydroid) if nodes else []
    return dm, entries


def _main(argv):
    import argparse
    parser = argparse.ArgumentParser(description="NVIDIA driver mode generator (prints LXC mount entries)")
    parser.add_argument("root", nargs="?", help="a staged root: print its mount entries (no cfg involved)")
    parser.add_argument("--cfg", help="check a waydroid.cfg (mode, [properties], generator); announce on stderr")
    parser.add_argument("--describe", action="store_true",
                        help="with --cfg: print mode/root/gl/egl and the derived properties (no generator run)")
    parser.add_argument("--session-props", metavar="PROPFILE",
                        help="print the per-session display properties a container start would derive for "
                             "PROPFILE (e.g. /var/lib/waydroid/waydroid_base.prop) on this host now")
    args = parser.parse_args(argv)
    if args.session_props:
        with open(args.session_props) as f:
            base = f.read().splitlines()
        rates, notes = active_refresh_rates()
        props, more = session_properties(base, rates)
        for note in notes + more:
            print(note, file=sys.stderr)
        for p in props:
            if p.startswith(SNAP_PROPERTY + "=") or p.startswith(REFRESH_PROPERTY + "="):
                print(p)
        return
    if args.cfg:
        if args.root:
            parser.error("give either ROOT or --cfg")
        dm, entries = check_cfg(args.cfg, nodes=not args.describe)
        print(announce(dm), file=sys.stderr)
        if args.describe:
            print("mode={}\ninstall={}\nlink={}\nroot={}\nnative_gl={}\negl={}".format(
                dm.mode, dm.install or "", dm.link or "", dm.root or "", "on" if dm.native_gl else "off", dm.egl))
            for k, v in mode_properties(dm):
                print("prop.{}={}".format(k, v if v is not None else ""))
        else:
            print("\n".join(entries))
        return
    if args.describe:
        parser.error("--describe needs --cfg")
    print("\n".join(lxc_nodes(args.root or GLIBC_SHIM_STAGING)))


if __name__ == "__main__":
    import sys
    try:
        _main(sys.argv[1:])
    except OSError as e:
        print(e, file=sys.stderr)
        sys.exit(1)
