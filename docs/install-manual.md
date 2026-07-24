# Manual installation (non-Arch distros)

The release binaries install anywhere systemd + Waydroid's dependencies exist.
Host binaries are CI-built on Ubuntu 24.04 (glibc ≥ 2.39) and link `libepoxy`,
`libdrm`, `libgbm`, `libX11`, `expat`, plus the Vulkan loader at runtime.

From a checkout of this repo:

## 1. Patched waydroid

Two small patches: the config generator emits this stack's bind-mounts (so
`waydroid upgrade` keeps working), a venus-socket preflight at session start,
and `suspend_action = none` support.

```sh
git clone https://github.com/waydroid/waydroid.git && cd waydroid
git checkout a33a5c0b31d89d6ce687381104b30aff4dd2d330   # patches/waydroid/BASE
git apply ../patches/waydroid/0001-nvidia-integration.patch
sudo make install USE_NFTABLES=1     # =0 if your firewall is iptables
cd ..
```

## 2. Download and verify the release binaries

```sh
V=vX.Y.Z  # use the first release tag that includes the dual-ABI guest layout
B=https://github.com/Shiro836/waydroid-nvidia/releases/download/$V
curl -LO $B/waydroid-nvidia-host-x86_64-$V.tar.zst
curl -LO $B/waydroid-nvidia-guest-android-x86_64-$V.tar.zst
curl -LO $B/waydroid-nvidia-guest-prebuilts-$V.tar.zst
curl -LO $B/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
```

`SHA256SUMS` covers integrity. For **provenance** — proof an asset was built
by this repo's CI from this source — every tarball carries a SLSA attestation
(since v0.1.1; the surfaceflinger inside `guest-prebuilts` builds on the
maintainer's registered self-hosted runner, and its provenance says so):

```sh
gh attestation verify waydroid-nvidia-host-x86_64-$V.tar.zst --repo Shiro836/waydroid-nvidia
gh attestation verify waydroid-nvidia-guest-prebuilts-$V.tar.zst --repo Shiro836/waydroid-nvidia
```

## 3. Install the binaries

Paths must match the unit/setup script:

```sh
sudo mkdir -p /usr/lib/waydroid-nvidia/guest
sudo tar --zstd -xf waydroid-nvidia-host-x86_64-$V.tar.zst          -C /usr/lib/waydroid-nvidia
sudo tar --zstd -xf waydroid-nvidia-guest-android-x86_64-$V.tar.zst -C /usr/lib/waydroid-nvidia/guest
sudo tar --zstd -xf waydroid-nvidia-guest-prebuilts-$V.tar.zst      -C /usr/lib/waydroid-nvidia/guest
```

Dual-ABI release tarballs retain Android-relative paths below that guest
directory. Before running setup, both
`guest/vendor/lib/hw/vulkan.virtio.so` (ELF32) and
`guest/vendor/lib64/hw/vulkan.virtio.so` (ELF64) must exist; the setup helper
checks every ANGLE/Venus ELF before changing the Waydroid configuration.

## 4. Units, tmpfiles, udev rule, setup helper

```sh
P=packaging/aur/waydroid-nvidia-bin
sudo install -Dm644 $P/wd-venus.service        /etc/systemd/user/wd-venus.service
sudo install -Dm644 $P/waydroid-venus.tmpfiles /etc/tmpfiles.d/waydroid-venus.conf
sudo install -Dm644 $P/waydroid-nvidia.rules   /etc/udev/rules.d/70-waydroid-nvidia.rules
sudo install -Dm755 $P/waydroid-nvidia-setup   /usr/local/bin/waydroid-nvidia-setup
sudo systemd-tmpfiles --create /etc/tmpfiles.d/waydroid-venus.conf
sudo udevadm control --reload && sudo udevadm trigger /dev/udmabuf
```

The udev rule grants the seated desktop user access to `/dev/udmabuf`
(CPU-mappable buffers — cursors, screenshots). Re-log-in once after
installing it.

## 5. Initialize and start

Same as the AUR steps from `waydroid init` onward — the container unit is
`waydroid-container.service`, installed by `make install` in step 1:

```sh
waydroid init
sudo waydroid-nvidia-setup           # add --refresh <hz> to match your monitor
sudo systemctl enable --now waydroid-container.service
systemctl --user enable --now wd-venus.service
waydroid session start
```

If anything misbehaves, see [`troubleshooting.md`](troubleshooting.md).
