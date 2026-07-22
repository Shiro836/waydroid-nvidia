# Cutting a release

1. **Green CI** on the release commit (`build.yml`: mesa, virglrenderer,
   gralloc, waydroid-patch-check).
2. **Tag & push** — the release job packages CI artifacts, writes SHA256SUMS,
   attaches SLSA provenance attestations and creates a prerelease:
   ```sh
   git tag v<X.Y.Z>[-rcN] && git push origin v<X.Y.Z>[-rcN]
   ```
3. **Upload the guest prebuilts** (hwcomposer, ANGLE ×3, surfaceflinger —
   components without scripted CI provisioning yet, see PREREQS.md). Assemble
   from a verified-running `/var/lib/waydroid/nv` (README.txt + sums inside;
   the tarball itself is *not* CI-attested — say so in release notes):
   ```sh
   gh release upload v<tag> waydroid-nvidia-guest-prebuilts-v<tag>.tar.zst
   ```
4. **PKGBUILD**: bump `pkgver`/`_tag` in
   `packaging/aur/waydroid-nvidia-bin/PKGBUILD`, replace checksum SKIPs
   (`updpkgsums`), test `makepkg -s` from a clean clone, commit.
5. **AUR**: regenerate `.SRCINFO` (`makepkg --printsrcinfo > .SRCINFO`) and
   push PKGBUILD + .SRCINFO + local sources to the AUR repo.

Verify any CI-built asset:
```sh
gh attestation verify <asset> --repo Shiro836/waydroid-nvidia
```
