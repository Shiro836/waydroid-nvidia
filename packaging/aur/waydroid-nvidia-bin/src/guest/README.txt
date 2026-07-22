waydroid-nvidia guest prebuilts (android-x86_64)
================================================
These binaries are built from the recipes in the waydroid-nvidia repo but are
NOT yet produced by CI (their build provisioning is being scripted — see
packaging/aur/PREREQS.md). Until then they are built and uploaded by the
maintainer; sha256 sums are listed in the release's SHA256SUMS.

  hwcomposer.waydroid.so   waydroid hwcomposer + NVIDIA fixes
                           (base 7750307 + patches/hwcomposer, build/hwcomposer)
  lib*_angle.so            ANGLE (GL-on-Vulkan), source build c1a25085dd9e
                           (build/angle/args.gn)
  surfaceflinger           LineageOS 20 SF + vsync-snap prop patch
                           (patches/lineage-20, build/lineage-20) — optional,
                           only needed for >240 Hz displays
