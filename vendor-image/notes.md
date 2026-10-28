# Waydroid vendor image from source

Status 2026-10-16 (later): the vendor image is how nvidia-prolapse installs for users (section "Install path for
users"); installed live through the package and the setup, every gate passed.
Status 2026-10-16 (late): the nvidia-prolapse vendor image (all our components as vendor modules) builds from the
pinned tree and booted live with every gate passed (section "nvidia-prolapse product").
Status 2026-10-16: the official 20260927 MAINLINE vendor image **is reproduced from source** (stock Waydroid, none of
our modules yet). It has the same 630 entries (files, directories, links), SELinux labels, modes and properties. Of the 630, 348 are identical and
261 ELF files differ only in their build-id; 21 have the same ABI but different bytes (see "First build" below). The
design map further down is a proposal: nothing in it is implemented.

Goal: stop depending on the prebuilt Waydroid vendor image. We build the VENDOR image from
Waydroid's LineageOS trees, with our components as proper vendor modules, and keep the official SYSTEM image
(Treble split) so users still get official system OTAs. A13 (lineage-20) comes first; A14/A15 follow per plan Later 3.

## First build (2026-10-16)

- CI runs 38927398718, 38928285808 and 38930303614 each failed on one host prerequisite, fixed in turn. Run 38930303614
  did everything except Mesa: 13,865/13,997 actions in 16:22 (mm:ss), cold, 72 jobs. Then over ssh on the same tree,
  after Mesa's prerequisites were fixed: incremental `m vendorimage` 274 s (Mesa x86_64 + x86 + the image), 341 s for
  build + collect + compare. A full cold build is therefore about 21 min plus the Soong bootstrap. out-vendor-l20 is
  51 GB.
- Build environment = `vendor-image/Containerfile` (ubuntu 22.04 pinned by digest, the LineageOS host packages,
  meson 1.7.0 from pip, glslang 15.0.0 built from tag). `lunch` and `m` run inside it with the tree bind-mounted at the
  same path, `--user` = the runner user, `--network none`. Why: Soong replaces the PATH of its actions
  (build/soong/ui/build/path.go: prebuilts/build-tools + the out/.path interposer), so host tools the tree expects must
  be in system paths. Waydroid's Mesa rule runs `PATH=/usr/bin:/usr/local/bin:$PATH meson`
  (external/mesa/android/mesa3d_cross.mk:86). The LLVM host tools NEED libncurses.so.5. Mesa's radv BVH shaders need
  glslang >= 12.2 (meson.build:695). The builder host (Arch) has no meson, no mako in the system python and no ncurses 5, and gets
  no system changes. The official builds are containerized too (their paths are /out/lineage20).
  "Build sandboxing disabled due to nsjail error" in the container is expected: nsjail only makes sources read-only.
- The `VNDK library: libbinder's / libhidlbase's / android.hardware.configstore@1.x / graphics.mapper@4.0 ABI has
  INCOMPATIBLE CHANGES` lines are header-abi-diff reports and do not fail the build (it continued). They are expected:
  Waydroid's base-patches change libbinder (host binder), libhwbinder/libhidl and hidl-gen (host_hwbinder), so every
  HIDL interface library differs from AOSP's VNDK reference dumps. The official image is built from the same patches.
- `compare-official.md` against the official image: 0 only-in-either, 0 meta/label, 0 prop, 348 identical,
  261 elf-build-id-only, 21 elf-same-abi.
  - Build-id-only: the 16-byte md5 build-id is hashed over the unstripped output, whose debug info carries OUT_DIR
    (official `/out/lineage20`, ours `.../out-vendor-l20`).
  - The 21 same-ABI files:
    - 3 clearkey binaries (drm-service-lazy.clearkey, libclearkeycasplugin x2) embed a protobuf-generated source path
      that contains OUT_DIR (`../../out/lineage20/soong/...` vs the absolute path).
    - 18 Mesa meson-built libs (libgallium_dri, dri_gbm, lib*_mesa, vulkan.{intel,intel_hasvk,lvp,radeon}, both ABIs)
      differ only in `.rodata` size (libgallium +752 B, radv -32 B); `.text` and every other section are the same
      size, and no embedded paths differ. That fits embedded data produced by Mesa's host tools at other versions
      (e.g. SPIR-V from glslang). Not attributed further: the official build environment is unpublished.
  - Output sha256 of this build: vendor.img 7e6b3e5d…74ec3, manifest.xml 9fb5ddd7…e0e82.

## Files

| Path | What |
|---|---|
| `.github/workflows/vendor-image.yml` | job `vendor-l20` on `[self-hosted, lineage]`, dispatch + `v*` tags only, uploads `stage-vendor/` |
| `vendor-image/build-vendor.sh` | stages `preflight checkout lunch build collect compare` (or `all`); lunch/build run in the container |
| `vendor-image/Containerfile` | build environment (see "First build"); image tag = hash of this file |
| `vendor-image/pins.env` | lunch target, pinned manifest path, cutoff, official reference image URL + sha256 |
| `vendor-image/manifests/lineage-20-waydroid-20260927.xml` | full manifest: 1,254 projects, all branch-tracked ones pinned to SHAs |
| `vendor-image/manifests/lineage-manifest-0d12a65-overrides.tsv` | 6 projects that LineageOS's manifest moved from AOSP to LineageOS forks after our tree's sync |
| `vendor-image/project-delta.sh`, `cutoff-shas.sh`, `pin-manifest.py` | regenerate the pinned manifest for another image date |
| `vendor-image/tree-checkout.py` | reset an existing synced tree to a pinned manifest (`--check` = report only) |
| `vendor-image/compare-vendor.py` | compare two ext4 images file by file without root: mode, uid/gid, SELinux label, symlink, sha256, ELF ABI |

## Builder facts (self-hosted builder, measured 2026-10-16)

- Runner: `~/actions-runner`, systemd `actions.runner.<owner>-waydroid-nvidia.<builder>.service` active, registered to
  **Shiro836/waydroid-nvidia** (repo-level runner, `.runner` agentName <builder>). Only workflows in that repo can use it.
- Tree: `~/repos/waydroid-nv/lineage-20` (rsynced 2026-07-23 from the dev box, which no longer has a copy). It uses
  the LineageOS manifest 0f52348 (2026-05-18), Waydroid local manifests and `.waydroid-patches-applied` (Jul 12).
  `out/` is 16 GB (SF builds). The ANGLE checkout is `~/angle-ci`.
- Disk: 239 GB free on `/` (1.9 TB, 88 % used). 96 CPUs, 188 GB RAM, load average about 1.
- Bandwidth from the builder: GitHub release download 61.8 MB/s; googlesource single archive stream 4.8 MB/s. Small
  `git fetch`es of the Waydroid repos took 0.8 to 4 s each.
- The 2026-07 rsync used an unanchored `--exclude out`, which deleted every nested `out/` directory inside projects:
  external/icu (46 files), external/chromium-trace (166), device/generic/vulkan-cereal (87), external/cldr and some
  rust crates. `tree-checkout.py` restores them (reset --hard). The fix for future deliveries is
  `rsync --exclude /out`.

## Which sources match the official 20260927 image

The vendor.img `/build.prop` has `ro.vendor.build.date.utc=1790542319` (2026-09-27 20:51:59Z), builder `aleasto`,
fingerprint `waydroid/lineage_waydroid_x86_64/waydroid_x86_64:13/TQ3A.230901.001/aleasto09272251:userdebug/test-keys`.
Waydroid publishes no build manifest. The rule used here: the last first-parent commit at or before that time on every
branch-tracked project, and the LineageOS manifest as of that time (0d12a65, 2026-09-23).

- Waydroid repos at the cutoff (every one of them is also the remote head today):
  - hardware/waydroid 6e898e9 (2026-09-26); our manifest has since moved this one project to a3e4239 (2026-10-18,
    upstream's fix for the keyboard-enter/setFocusedTask deadlock), which the standalone hwcomposer pins too
  - vendor/extra 1b95b85 (2026-07-23)
  - device/waydroid/waydroid 2234f4b
  - external/mesa a6685db (Mesa 26.0.8 merge, 2026-07-23)
  - external/minigbm e5a7769 (2026-07-23)
  - prebuilts/mesa-tools b5de4e0
  - libdrm, llvm-project, alsa, pulseaudio and WaydroidUpdater are unchanged.
- LineageOS: of 291 branch-tracked projects, 41 moved since the July sync (`project-delta.sh`, 0 ls-remote errors):
  frameworks/av +8, frameworks/base +38, bionic, libpng, freetype, libfmq, Settings, ... Some of them got more commits
  after the cutoff (October ASB); the cutoff excludes those. AOSP projects stay on tag android-13.0.0_r75.
- The LineageOS manifest moved 6 projects from AOSP tags to LineageOS forks: libhevc, packages/apps/TV,
  BuiltInPrintService, incremental_delivery, libufdt and nfc (overrides tsv).
- Caveat: commit dates are not push dates, so the reconstruction is a best effort. `compare` measures how close it is.

## Dry run on the runner's tree (2026-10-16, ssh, no GitHub; superseded by "First build")

- `preflight`: passes (238 GB free, jobs 72).
- `checkout`: 1 min. It reset 83 projects and fetched the 6 fork revisions, then `apply-waydroid-patches`
  reported "All patches applied fine". It records tree state for 1,254 projects in `.vendor-image-tree-state.tsv`.
  The previous state (HEAD and dirty count per project) is in `~/vendor-image-prep/tree-before-20261016.tsv`; that
  state was the July sync, the Waydroid patches, and the baseline SF patch uncommitted in frameworks/native.
- `lunch lineage_waydroid_x86_64-userdebug`: 9 s. TARGET_ARCH x86_64, 2ND x86, PLATFORM_VERSION 13, SDK 33,
  BOARD_VNDK_VERSION current, PRODUCT_OUT `out-vendor-l20/target/product/waydroid_x86_64`.
- Not run: `build`, `collect`, `compare`. Build time and out-dir size are **not measured**. WayDroid-ATV quotes
  about 5 h for system + vendor on unspecified hardware and about 300 GB of total space. `MIN_FREE_GB` defaults to 80.
- The tree is shared with build.yml's surfaceflinger job. That job keys on `.waydroid-patches-applied` (still
  present after checkout) and applies its frameworks/native patch on top, so it now builds SF from the 09-27 sources.
  `tree-checkout.py` discards that uncommitted patch on the next vendor run.
- Leftovers on the builder: `~/vendor-image-prep/` (scripts, delta tables, fork bare repos; 12 MB) and
  `out-vendor-l20/` (73 MB). Nothing is running.

`compare-vendor.py` self-test: the clean 20260927 OTA vendor.img against the live one gives 628 identical files.
The only differences are Magisk's (`/etc/selinux/precompiled_sepolicy` rewritten without a SELinux label,
`vendor_file_contexts` changed, and two `.gz` backups added).

## nvidia-prolapse product (2026-10-16): all components as vendor modules

**Result:** built on the builder over ssh (cold, out-vendor-l20-prolapse; final incremental `m vendorimage` 436 s)
and booted live; every gate passed (boot, SurfaceFlinger on ANGLE/NVK and nvrm, H.264 decode, games, 0 Xid).
vendor.img sha256 f6cd35a13fdb99ec…; all 25 prebuilt files in it are byte-identical to the bundle/ANGLE inputs.

`VENDOR_PRODUCT=prolapse vendor-image/build-vendor.sh checkout product lunch build collect compare` with
`NATIVE_BUNDLE` (packaging/native/collect.py output) and `ANGLE_DIR` (the angle job's stage). The CI job
`vendor-l20-prolapse` takes both from the newest successful build.yml run on the same ref (or `L20_NATIVE_RUN_ID`).
Lunch `lineage_nvidia_waydroid_x86_64-userdebug`, out dir `out-vendor-l20-prolapse`.

- **Product layer** (`vendor-image/product`, rsynced into the tree by the product stage; the checkout stage removes it
  again so the stock reproduction stays stock). These are not repo projects yet; published repos plus a local manifest
  would be the next form.
  - `device/nvidia-prolapse`: product `lineage_nvidia_waydroid_x86_64` inherits Waydroid's
    `lineage_waydroid_x86_64.mk` and `vendor/nvidia-prolapse/prolapse.mk`. The `_waydroid_x86_64` suffix is kept
    because `device/waydroid/waydroid/device.mk:166,242` select on it. Device `waydroid_nvidia_x86_64` exists only
    so that its BoardConfig (Waydroid's + `BOARD_VENDOR_SEPOLICY_DIRS`) is the one picked. `ro.product.device` still
    comes from the system image.
  - `vendor/nvidia-prolapse`:
    - `Android.mk`: Make prebuilts, because `libdrm.so.2` needs an installed name that is not `<stem>.so`, and
      `LOCAL_OVERRIDES_MODULES` replaces stock modules.
    - `prolapse.mk`: packages and vendor default props.
    - `sepolicy/vendor/file_contexts`.
- **Modules:**
  - Prebuilts from the release bundle (sha256-checked against BUILD-RECORD.json): `vulkan.nvidia`,
    `libvulkan_nouveau`, `libnv_bionic_native` and `libdrm.so.2` (Vulkan); `lib{EGL,GLESv1_CM,GLESv2}_nvrm` in egl/
    and `libgallium_dri_nvrm` (nvrm GL).
  - ANGLE (pinned + angle/patches) replaces AOSP's `libEGL_angle` etc. Our `libgbm_mesa_wrapper` replaces Mesa's.
  - The Codec2 service is a prebuilt on /odm (`LOCAL_ODM_MODULE`; the binary opens /odm/etc paths, and on Waydroid
    /odm = vendor.img /odm) with its rc, VINTF fragment, seccomp policy and media_codecs_c2.xml. Its 9
    HIDL/Codec2 libraries are the tree's vendor variants (`*.vendor`), not the bundle's.
  - Built in-tree:
    - `hardware/waydroid` with `patches/hardware/waydroid`: our hwcomposer 0001-0016 rebased onto 6e898e9 (0001
      conflicts resolved to our side; upstream's leak/null-handle/0660-FIFO fixes kept), since 10-27 onto a3e4239 (applies
      without conflicts) plus an `Android.bp` commit
      for prime-copy.cpp, and 0018: the Android display follows the window (a display reconnect sent from hwc's own
      resize thread, enabled by `ro.vendor.composer.safe_primary_reconnect`, which `prolapse.mk` sets); 0019 lets
      the single-window per-app window drive that size too (issue #5; multi-window cards keep the fixed display); 0020 names a
      failed EGL read-back of a wl_shm copy (pointer, unimportable layers, snapshots) instead of showing a blank buffer. 0021: window creation
      never waits for the Wayland thread (its configure is dispatched on the creation queue by the composer thread).
    - `hardware/interfaces` with `patches/hardware/interfaces`: composer 2.1 HAL 0001 makes a primary display
      reconnect safe while frames are in flight (hwc2on1 re-reads the configs without holding its state mutex
      across the Display's; ComposerClient keeps the display's cached buffer handles instead of freeing them).
    - `external/minigbm` with `patches/external/minigbm`: gralloc 0001-0007 (0001-0005 rebased onto e5a7769; 0006 YV12 decoder output + carrier size; 0007 = gralloc 0006, CPU-cached memory for buffers the CPU reads often); e5a7769's YVU420
      fix is carried into our first-map import.
    - `external/wayland-protocols`: the 7 XMLs hwcomposer uses, at wayland-protocols 1.49.
  - Everything is on standard `/vendor/<lib>{,/hw,/egl}` and `/odm` paths. Nothing needs `/vendor_extra` or a
    linkerconfig change.
- **Default props** (vendor build.prop): `ro.hardware.vulkan=nvidia`, `ro.hardware.egl=angle`,
  `ro.zygote.disable_gl_preload=true` and the Codec2 ranks. `/vendor/waydroid.prop` is loaded last (Waydroid
  system/core 0015), so an install can still select nvrm.

### What the product changes against the official 20260927 image (`compare-official.md`, prolapse build)

- **Added:** 36 entries.
  - Vulkan HAL, NVK, Rust RM and libdrm.so.2 (both ABIs).
  - nvrm: egl libs + libgallium_dri_nvrm (both ABIs).
  - The 9 Codec2/HIDL vendor libs (lib64).
  - /odm: bin/hw service, etc/init rc, VINTF fragment, seccomp policy, placeholder media_codecs_c2.xml.
- **Replaced (elf-abi):**
  - ANGLE ×6.
  - hwcomposer ×2.
  - libminigbm_gralloc_gbm_mesa ×2.
  - libgbm_mesa_wrapper ×2 (relabelled same_process_hal_file).
- **Changed:**
  - `/etc/selinux/vendor_file_contexts` (our labels) and NOTICE.
  - build.prop of vendor/odm/odm_dlkm/vendor_dlkm: product/device name plus our default props.
- **Same as the stock reproduction:** 255 ELFs differ only in build-id, and 21 have the same ABI (Mesa/clearkey,
  explained above).

### Build fixes this needed

- In-tree `-Werror -Wmissing-field-initializers`: Vulkan create-info structs and Wayland listener tables in
  hwcomposer. The module turns that warning off (same code as the standalone build). xdg_wm_base is bound at v1
  (wayland-hwc.cpp:2505), so the v4 `configure_bounds` slot left zero is never called.
- `media_codecs_c2.xml` lists the HOST GPU's decoders (nvc2-xmlgen from nv_video_query_caps()), so no image can carry
  the right one.
  - The image ships a placeholder without codecs; the install binds the host-generated file over
    `/odm/etc/media_codecs_c2.xml`.
  - The service refuses to start while the file and the live caps differ, so a missing bind fails loudly.
  - The release payload carries only the service binary: rc, VINTF and seccomp come from the driver's media/odm/etc (the bundle's driver/media/odm/etc).
- `python-is-python3` in the build environment: a LineageOS lunch helper calls `python`.

## Install path for users (2026-10-16 late): the vendor image IS the nvidia-prolapse install

The bind-mounted driver root is gone from the user flow. `waydroid-nvidia-setup` installs this release's vendor
image; developers keep the staged root as `nvidia_prolapse_install = dev-root` (docs/dev-setup.md).

- **Identity:** the product stage writes `vendor/nvidia-prolapse/identity.mk` (not in the source tree) from
  `NATIVE_BUNDLE`: `ro.vendor.nvidia_prolapse.version` (bundle `release`, else `git-<commit12>`) and
  `ro.vendor.nvidia_prolapse.build_record` (sha256 of the bundle's BUILD-RECORD.json). `collect` refuses an image
  without them. Rebuilt on the builder 2026-10-16 18:22–18:32 UTC (`checkout product lunch build collect compare`,
  m 597 s after the CI stock run had reset the tree): `v0.2.0-rc1`, build_record `31d326fd…` (= the rc1 bundle),
  ro.vendor.build.date.utc 1792174961, vendor.img sha256 `570e8b91…`.
- **Release:** build.yml calls vendor-image.yml (`workflow_call`, `prebuilts-from-this-run`: guest-native and
  guest-angle of the same run, stock reproduction skipped). The release job runs `packaging/vendor-ota.py`:
  `waydroid-nvidia-vendor-x86_64-<tag>.zip` (vendor.img only; 206 MB for the 686 MB image) and
  `waydroid-nvidia-vendor-ota-<tag>.json`, the format of Waydroid's channels; both in SHA256SUMS and the attestation.
- **OTA:** Waydroid's updater (`tools/helpers/images.py get()`; the in-guest updater reads `waydroid.vendor_ota`
  and validates against the same channel in `replace()`) downloads only an entry whose `datetime` is newer than
  `vendor_datetime`, and checks sha256 = `id`. The setup sets `vendor_ota` = the release's own channel JSON on
  GitHub releases and `vendor_datetime` = the image's build date, so no update replaces our image, and system OTAs
  stay official. Not served from a moving channel on purpose: the image must match the package's bundle and gate
  programs, so it changes only with the package. An empty `vendor_ota` would make `waydroid upgrade` fail
  (`retrieve('')` -> ValueError). `waydroid init -f` resets the channel and brings the official image back: the
  generator then refuses and names the setup.
- **Install** (`packaging/native/vendor_image.py install`): zip sha256 = channel id; image build_record = the
  package bundle's; 19 bundle binaries in the image byte-identical to the bundle; release gate on those bytes
  (`release_gate.run(vendor_image=...)`, Codec2 case `media_case_image`: nvc2-xmlgen, validate_xml, --check, the
  service's NEEDED against vendor.img /lib64 + VNDK apex + LL-NDK, nv_video symbols, device access); official
  vendor.img moved to `/var/lib/waydroid-nvidia/vendor-image/official/` with its vendor_ota/datetime; record
  `install.json` + `media_codecs_c2.xml`; cfg switch; `upgrade -o`; verify-active; drop-in. Rollback on any failure.
- **Generator** (`nvidia_native.check_vendor_image`, every upgrade -o and container start through verify-active):
  identity props, record, image sha256, kernel module, package bundle, codec list. Mounts: the device nodes and
  `media_codecs_c2.xml` (`bind,ro`) only. glibc-shim, venus and dev-root refuse our image.
- **Open:** the stock SurfaceFlinger (no snap-window patch; `debug.sf.snap_to_same_vsync_within_ns` is still
  derived but only the patched SF reads it). A real GitHub release has not been made, so the online
  `waydroid upgrade` against the channel URL is untested (the JSON logic is unit-tested against images.get()'s rule).

## Design map: our components as vendor modules (proposal, written before the product layer above)

Shape: one product layer of our own and **no edits to Waydroid's trees**. A local manifest adds two repos:

- `device/nvidia-prolapse`: AndroidProducts.mk with lunch `lineage_waydroid_nvidia_x86_64-userdebug`, inheriting
  `device/waydroid/waydroid/waydroid_x86_64/lineage_waydroid_x86_64.mk`
- `vendor/nvidia-prolapse`: the modules plus `prolapse.mk`

Waydroid's own changes reach us as the pinned manifest. Our changes to Waydroid sources are patch series applied
after `apply-waydroid-patches`. Prior art: WayDroid-ATV forks Waydroid's device tree and adds vendor modules there
(android_device_waydroid_waydroid lineage-23.2 `device.mk:82-114`: hwcomposer.drm_minigbm, hwcomposer.redroid,
minigbm_dmabuf/minigbm allocator+mapper); its BUILDING.md is the same repo init + generate-manifest +
apply-waydroid-patches + `make vendorimage` flow with per-version lunch targets (A14 `-ap2a-`, A15 `-bp1a-`). We replace stock modules with Soong `overrides`.
That `overrides` works for prebuilt vendor modules on A13 is **not verified**.

| Component | Today (image dependency) | As a vendor module | APIs used (DT_NEEDED vs the image's LL-NDK / VNDK-SP / VNDK-core lists, `com.android.vndk.current/etc/*.libraries.33.txt`) |
|---|---|---|---|
| Vulkan HAL `vulkan.nvidia.so` (32/64) | `/vendor_extra/<lib>/hw` via the image's patched linkerconfig | Option 1: `cc_prebuilt_library_shared`, `vendor: true`, `relative_install_path: "hw"`, from CI artifacts. Option 2: `cc_library_shared` from the driver's `hal/` sources. Lands in `/vendor/<lib>/hw` = stock sphal path | LL-NDK only (liblog, libdl, libc) |
| NVK `libvulkan_nouveau.so`, `libnv_bionic_native.so` (Rust RM), `libdrm.so.2` | `/vendor_extra/<lib>` | Prebuilt vendor modules from the existing CI build (the driver's `mesa/build.py`, cargo for bionic). The A13 Soong Rust toolchain is too old for the crate: **not checked**, so prebuilt is the realistic first step. Label `same_process_hal_file` | LL-NDK + VNDK-SP libhardware; ours: libnv_bionic_native, libdrm.so.2 |
| nvrm GL `lib{EGL,GLESv1_CM,GLESv2}_nvrm.so` + `libgallium_dri_nvrm.so` | staged egl dir bound over `/vendor/<lib>/egl` | Prebuilt vendor modules: egl libs with `relative_install_path: "egl"`, gallium in `/vendor/<lib>`. Selected by `ro.hardware.egl` as today | LL-NDK + VNDK-SP libhardware |
| ANGLE (our pin + ReBAR patch) | file binds of `/var/lib/waydroid/nv/guest/.../egl/*_angle.so` (Venus build's guest layout) | Official vendor already ships AOSP tm-qpr3 ANGLE (external/angle 353815e, 2023-01; `device.mk:82-84`). Ours: prebuilt from the ANGLE CI job, `stem: "libEGL_angle"` etc. + `overrides: ["libEGL_angle", ...]` | LL-NDK only |
| gralloc: our `libgbm_mesa_wrapper.so` + patched `libminigbm_gralloc_gbm_mesa.so` | binds over `/vendor/<lib>/` files; built against minigbm 16844ba + A13 header clones + image stubs | minigbm patches 0001-0007 applied to in-tree external/minigbm (e5a7769, which already carries "Fix YVU420"), built by Soong; the wrapper replaces `external/minigbm/gbm_mesa_driver/Android.mk:33` `libgbm_mesa_wrapper` (override) | wrapper: LL-NDK; minigbm core: LL-NDK + VNDK-SP libcutils/libc++ + vendor libdrm |
| hwcomposer | `/var/lib/waydroid/nv/guest/vendor/lib64/hw/hwcomposer.waydroid.so` bound over the image's; standalone NDK build with hidl-gen, AOSP header clones, link stubs from the image, libcxx_compat | Patches 0001-0015 (+ sf-crash's 0016) rebased onto in-tree hardware/waydroid 6e898e9 and built by Soong. That removes provision.sh's stubs, aidl-stubs and libcxx overrides. Upstream f6c3e3a "Record why the Wayland connection died" and 48979b5 overlap our 0006/0004 area: needs a rebase review (sf-crash owns hwcomposer/ now) | in-tree, against the tree's own libs |
| Codec2 NVDEC service + libs | `/odm` replaced by a staged tree (byte-copy of vendor.img `/odm/etc` + exact-image check); 9 Codec2/HIDL vendor libs rebuilt standalone (media/vndk/build.sh) against Waydroid's patched libhidl headers | `cc_binary` `vendor: true` in `/vendor/bin/hw`; .rc in `/vendor/etc/init`; VINTF fragment via `vintf_fragments`; seccomp policy via `PRODUCT_COPY_FILES` to `/vendor/etc/seccomp_policy`; media_codecs_c2.xml to `/vendor/etc`; ccodec props as `PRODUCT_VENDOR_PROPERTIES`. android.hardware.media.c2@1.x, libcodec2_hidl@1.x, libcodec2_vndk and bufferpool@2.0.1 come from frameworks/av / hardware/interfaces in-tree as vendor variants. That drops media/vndk/build.sh, its 2 patches, the /odm copy and the vendor.img check. Label: `mediacodec_exec` (the OMX service's label in the official image) | VNDK-core (libcodec2, libstagefright_foundation, libbinder, bufferqueue, ...) + VNDK-SP; legitimate for a vendor process. Waydroid's official vendor ships no Codec2 HAL (only OMX) |
| seccomp / appsandbox | appsandbox is a test harness using bionic's genseccomp; app seccomp is system-side (zygote) | Nothing in vendor except the Codec2 service policy (row above) | n/a |
| LXC mounts (`waydroid/nvidia_native.py`) | rbind `/vendor_extra`, binds over gbm/minigbm/egl/hwc/ANGLE/SF, `/odm` rbind, device nodes | Left: device nodes, the render node by PCI vendor, and the mode/props generator. glibc-shim mode still needs the host blobs bound at the host driver version (they cannot be in an image). rust-driver mode needs no file binds | n/a |
| SELinux | none (host has no SELinux, `reference/host.md`) | `BOARD_VENDOR_SEPOLICY_DIRS` += ours: `same_process_hal_file` for every sphal lib, `mediacodec_exec` for the service. Waydroid labels Mesa's sphal libs `vendor_file` (e.g. `/lib64/libgallium_dri.so`) and only ANGLE `same_process_hal_file` | n/a |

Nothing we ship needs a system-private library. The graphics side is sphal-clean (LL-NDK + VNDK-SP); the media side
uses VNDK-core, which a vendor service may. Classification command: readelf over the live root
`nvidia-source-native-mesa262c-release-20261016` against the 33 lists (`libGLESv1_CM_angle -> libGLESv2_angle` is a
sibling in the same egl dir).

Not settled:

- **Dev loop.** A vendor image per driver change is slow. Keep a dev-only bind overlay of `/vendor/<lib>/...` files
  (today's mechanism, narrowed). The shipped path is the image.
- **Runtime checks that move.** The source manifest's kernel-module pin and gates run at stage time today. With an
  image they become a start-time preflight in the generator (still loud, principle 5).
- **Mesa in our vendor.** Keep Waydroid's Mesa/minigbm (hybrid iGPU hosts, `TARGET_USE_MESA` stays on) or drop it to
  save LLVM build time. Not decided yet.

### The patched SurfaceFlinger

`../waydroid-nvidia/patches/lineage-20/frameworks-native/0001` is the only SF change. It makes
`kSnapToSameVsyncWithin` (VsyncSchedule.cpp, 3 ms) tunable via `debug.sf.snap_to_same_vsync_within_ns`. Without it,
at ≥ ~333 Hz every vsync falls in the snap window and the dispatch timer parks at INT64_MAX: no frames (baseline log
2026-07-13, M8.6). This host sets `persist.waydroid.refresh_rate = 500` and the prop to 1000000 (waydroid.cfg:23).

The patch has nothing to do with the GL driver (nvrm/ANGLE/NVK), so nvrm does not remove the need. It is needed only
above ~333 Hz. SF is a system binary, so a vendor image cannot carry it. Ways out:

1. Upstream it to Waydroid (`base-patches-33/frameworks/native`, which already holds 15 Waydroid SF/native patches).
   It reaches users with an official system OTA. No upstream PR exists.
2. Until then, cap the refresh rate below ~333 Hz when the stock SF runs. Our generator refuses a higher
   `persist.waydroid.refresh_rate` with a named remedy (no silent clamp).
3. Keep the prebuilt SF only as an explicit opt-in for >333 Hz, rebuilt by the SF job from the **same pinned tree** as
   the system image it runs against. Today's prebuilt was built against older libs than the 20260927 system.

Whether Android 14+ makes this window configurable: **not checked**.

## CI speedups (2026-10-17): tree stamps, native checkout, compiler caches

Measured on the builder with the jobs' exact step commands (ssh, no GitHub), against the v0.2.0-rc1 tag run
(38968629902), which took 30 min of self-hosted runner time: ANGLE 46 s, native 12:05, SF 5:07, vendor image 12:29.

- **The native job never had a persistent build/.** Every other self-hosted job's checkout step runs
  `git clean -ffdx` in the shared runner workspace, so build/ (NDKs, Mesa object store and worktrees, Meson/cargo
  build dirs) was gone before each native build: 6 min of downloads and provisioning plus a full Mesa rebuild every
  time. The job now builds in its own checkout `~/native-ci/waydroid-nvidia` (fetched from the workspace clone at
  the run's commit). Warm run with nothing changed: 63 s instead of 12 min. The bundle bytes change once with the new
  build path (Rust and debug info record it); from then on they are stable.
- **Tree stamps** (build-vendor.sh "Tree state"): a job whose stamp holds verifies 1,254 projects in 2 s and touches
  nothing; switching between the SF and the prolapse state resets 1 to 3 projects (2 to 4 s) instead of 38 projects
  plus a full Waydroid patch re-apply (39 s). What a switch still costs: the prolapse layer changes
  hardware/waydroid's `hwcomposer/Android.bp`, so after an SF job the vendor build re-runs Soong (about 135 s), and
  the SF build after a vendor job does too (125 s). Ninja work drops from 346 s to 21 s (vendor) and 75 s to 19 s (SF).
- **Kati ran on every vendor build** (80 s) because each `docker run` got a random hostname and Kati regenerates when
  BUILD_HOSTNAME changes (verbose.log: "Environment variable BUILD_HOSTNAME was modified"). The container now has a
  fixed hostname.
- **ccache**, native: 1,346 of 1,346 compiles hit on the second run, bundle byte-identical to the all-miss run
  (66 of 66 files). Vendor: LineageOS's own `USE_CCACHE` adds `-Wno-unused-command-line-argument` to every compile,
  which changed 276 build-ids against the uncached rc1 image; build-vendor.sh uses CC_WRAPPER/CXX_WRAPPER instead.
  With it the first (all-miss) build took 28:37 for 26,018 actions (a one-time full rebuild: the compile commands
  change) and its vendor.img equals rc1's in 653 files; the only differences are /build.prop (date, identity) and
  the 12 native-bundle prebuilts, which changed with the native build path.
