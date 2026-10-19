# waydroid-nvidia

GPU-accelerated [Waydroid](https://waydro.id) on NVIDIA cards. Android apps and games render on your NVIDIA GPU
inside the normal Waydroid container, with no VM. The default mode uses only the NVIDIA kernel module from your
system; the rest of the driver is built from source.

This `public-test` branch is the next version and replaces `main` once testers confirm it works.
Please report problems as issues titled `[public-test] ...` ([what to include](docs/troubleshooting.md#reporting-a-problem)).

## Why switch from Venus

- **Up to 4× faster:** no proxy, the driver runs right inside Android
- **No shader stutter:** Minecraft's first item switch ~0.2 s (Venus: 1.8 s)
- **Hardware video decode:** H.264, HEVC, VP9, AV1 (RTX 20 and newer)
- **Closed kernel module works too**, and older GPUs (GTX 10/900, Titan V) are in testing

## Does my setup work?

✅ works in Waydroid · 🧪 driver tests pass, Waydroid not tried yet · ⚠️ known bug · ⚪ untested · ❌ no

| | default (`nvidia-prolapse`) | `glibc-shim` | `venus` |
|---|---|---|---|
| RTX 50 (Blackwell) | ✅ | ✅ | ✅ |
| RTX 40, RTX 30, RTX 20 / GTX 16 | 🧪 | ⚪ | ✅ |
| Titan V (Volta) | 🧪 | ⚪ | ❌ |
| GTX 10 (Pascal) | ⚠️ native GL: some texture formats crash the GPU | ⚪ | ❌ |
| GTX 900 (Maxwell 2) | ⚠️ some GL tests fail | ⚪ | ❌ |
| Older GPUs | ❌ | ⚪ | ❌ |
| Driver 615, open kernel module | ✅ | ✅ | ✅ |
| Driver 580–610, open module | 🧪 | ❌ | ✅ |
| Driver 550–610, closed module | 🧪 (550.107 is refused) | ⚪ | ❌ |
| Driver 535 | ⚪ | ❌ | ⚪ |
| Driver 470 and older | ❌ | ❌ | ❌ |

Only the official Android 13 image (LineageOS 20) is supported. Hardware video decode (H.264, HEVC, VP9, AV1)
works in the default mode on RTX 20 and newer. On older GPUs the setup says so and Android's software decoders
play video instead.

You also need `nvidia-drm.modeset=1`, a Wayland desktop running on the NVIDIA GPU (tested on KDE Plasma 6), and
the usual Waydroid kernel support (binder).

## Install

Current release: [v0.2.0-rc1](https://github.com/Shiro836/waydroid-nvidia/releases/tag/v0.2.0-rc1) (prerelease).
This works on any systemd distro. On Arch use it too: the AUR package still has the previous release.

```sh
curl -fsSL https://raw.githubusercontent.com/Shiro836/waydroid-nvidia/public-test/packaging/install.sh | sudo bash
sudo waydroid-nvidia-setup
waydroid session start
```

The installer verifies every download and will not overwrite files owned by a distro package, so remove an older
`waydroid` or `waydroid-nvidia-bin` package first. `waydroid-nvidia-setup` checks your system, tests the driver on
your GPU, and only then installs our Android vendor image. Your official vendor image is kept as a backup. If a
check fails, it says why and leaves everything as it was.

On a fresh install the setup runs `waydroid init` for you. For Google apps, use `sudo waydroid-nvidia-setup -s GAPPS`
instead (the default is `VANILLA`, without them). On an existing install `-s` only checks the system type; to switch,
run `sudo waydroid init -f -s GAPPS`, then `sudo waydroid-nvidia-setup` again (apps and data are kept).

Run `sudo waydroid-nvidia-setup` again after every NVIDIA driver or package update; Waydroid refuses to start until
you do. `sudo waydroid-nvidia-setup --restore-official-vendor` takes you back to stock Waydroid. Uninstalling is
described in [docs/install-manual.md](docs/install-manual.md#uninstall).

## Driver modes

| Mode | How it works | Pick it when |
|---|---|---|
| `nvidia-prolapse` (default) | Our open-source driver (Mesa NVK plus our own NVIDIA backend) runs inside Android and talks to the kernel module directly | almost always: it has video decode and the widest driver support |
| `venus` | Android's Vulkan calls go over a socket to the NVIDIA driver on the host | the default mode fails on your machine; slowest, needs the open kernel module |
| `glibc-shim` | Your host's own NVIDIA driver files are loaded inside Android | developers only: needs the exact driver version it was built for |

Switch with `sudo waydroid-nvidia-setup --mode venus` (or `--mode nvidia-prolapse`). Apps get OpenGL ES through
ANGLE by default. `--native-gl on` uses our own GL driver (`nvrm`) instead; it is a bit faster but newer, so
compare with `--native-gl off` before reporting a GL bug.

## Known issues

- With `--native-gl on`, GTA San Andreas sometimes shows black frames.
- Above about 333 Hz the display can stall. Cap the rate with `sudo waydroid-nvidia-setup --refresh <hz>`.
- Android 14 and 15 have no official Waydroid images and are untested.

## More

[Troubleshooting](docs/troubleshooting.md) · [installer options and uninstall](docs/install-manual.md) ·
[how it works](docs/architecture.md) · [building](docs/building.md) · [developing](docs/dev-setup.md) ·
[third-party sources](THIRD_PARTY.md)

Our code is MIT licensed. Patches to other projects keep their upstream licenses; see [LICENSE](LICENSE).
