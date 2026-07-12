# AUR packaging (planned)

The endgame (Roadmap **M8.6**) is a reproducible AUR package that:

1. Clones each upstream at the pinned commit in `patches/<component>/BASE`.
2. Applies the patch series + drops in `src/` net-new files.
3. Cross-builds the guest bits (NDK) and host renderer, and folds everything
   into a proper Waydroid image — retiring the per-file `nv/` bind-mounts.

Prerequisite before packaging: the guest-framework rebuild (native 500 Hz,
LineageOS 20 image) so there are no more runtime bind-mount hacks to package.

`PKGBUILD` goes here once M8.6 lands.
