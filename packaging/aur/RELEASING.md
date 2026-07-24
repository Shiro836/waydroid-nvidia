# Cutting a release

Everything is CI-built and attested; there are no manual asset uploads.
Prereq (one-time, already done): the self-hosted runner with the `lineage`
label must be registered and online — it builds ANGLE (persistent
`~/angle-ci` checkout) and surfaceflinger (from the lineage-20 tree on the
runner host).

1. **Preflight** — run the full pipeline without releasing (also exercises
   the self-hosted jobs; ~10 min warm):
   ```sh
   gh workflow run build.yml
   gh run watch $(gh run list --workflow build.yml -L 1 --json databaseId -q '.[0].databaseId') --exit-status
   ```
   Debug any red job locally/hermetically first (container with CI parity);
   don't iterate by pushing.

   Confirm the Mesa matrix produced both `guest-vulkan-virtio-x86` and
   `guest-vulkan-virtio-x86_64`, and inspect the assembled archives before
   tagging. The guest tree must contain separate `vendor/lib` and
   `vendor/lib64` Vulkan/ANGLE files; no app-facing `.so` may be flat.

2. **Tag & push** — the release job rebuilds at the tag, assembles all four
   tarballs (host, guest, guest-prebuilts, SHA256SUMS), attaches SLSA
   attestations to every asset, and publishes. `-rcN` tags become
   prereleases; plain version tags full releases:
   ```sh
   git tag v<X.Y.Z>[-rcN] && git push origin v<X.Y.Z>[-rcN]
   ```

3. **PKGBUILD**: bump `pkgver`/`_tag` in
   `packaging/aur/waydroid-nvidia-bin/PKGBUILD`, then `updpkgsums`, test
   `makepkg -s` from a clean copy (run it in a temp dir — `makepkg` litters
   `src/`/`pkg/` and stray dirs break naive `cp *` chains), commit.

4. **AUR**: `makepkg --printsrcinfo > .SRCINFO`; copy PKGBUILD, .SRCINFO and
   the local source files (service, tmpfiles, setup, install, udev rules) to
   the AUR clone at `~/repos/waydroid-nvidia-bin-aur`, commit, push.

Verify any asset:
```sh
gh attestation verify <asset> --repo Shiro836/waydroid-nvidia
```
