# Minecraft Bedrock — Vibrant Visuals unlock (Magisk module)

Makes **Vibrant Visuals** selectable in Minecraft Bedrock on GPUs Mojang doesn't
whitelist — Waydroid/Venus, emulators, and "unsupported" phones — by patching the
device-tier gate the game ships in its APK. No re-sign, no reinstall, worlds intact.

## How it works

Minecraft gates Vibrant Visuals (and Ray Tracing) on a **device tier** looked up
from `assets/assets/tiers.bin` inside `base.apk`. That file is base64-encoded JSON:

```json
{ "gpu": { "Adreno (TM) 640": { "tier": 4, "hash": 1551874612 }, ... } }
```

RenderDragon computes a hash of the GPU's identity strings and matches it against
the `hash` field (the key name is cosmetic). VV needs tier ≥ 4. Unlisted GPUs get
no match → tier 0 → greyed out.

The hash is a **base-257 right-to-left polynomial finished with ×(2³¹+1)**:

```
low31(s) = ( Σ ord(cᵢ)·257ⁱ  +  R(len s) ) mod 2³¹      # i = left index
R(L)     = 257·R(L-1) + 1 mod 2³¹                        # anchor R from any stock entry
```

The top bit varies with the final ×(2³¹+1), so both `lo` and `lo|0x80000000` are emitted.

On an ANGLE-over-Vulkan stack (Waydroid), RenderDragon performs **several** tier
lookups for one GPU and requires *all* to pass — empirically the full
`GL_RENDERER`, the Vulkan device name, the mesa `"<name> (%s)"` template (the
literal `%s`!), the driver string, and the vendor. So the patcher over-generates:
the renderer, every parenthesised substring, their leading-word / trailing-`(…)` /
`(…)→(%s)` transforms, the driver tail, and the vendor token — then sets every
device tier to 5.

## What it does at boot (`service.sh`, Magisk `late_start`)

1. Detect the GPU: `GL_RENDERER` from `dumpsys SurfaceFlinger`.
2. Find the installed `base.apk` (`pm path com.mojang.minecraftpe`).
3. `vvpatch base.apk base.apk.vvnew "<renderer>"` — repacks the APK with a patched
   `tiers.bin` (all tiers → 5, correct hashes injected for the detected GPU).
4. Swap in place, **preserving owner/mode/mtime/SELinux context** so PackageManager
   does not re-verify the signature on the next boot. (Verified: survives reboot.)
5. Stamp `<size> <mtime>`; skip on later boots unless Minecraft was updated.

Ray Tracing stays greyed — it is compiled out of the Android build (there is no
DXR/RTX renderer in `libminecraftpe.so`), so no tier patch can bring it back.

## Build

```sh
NDK=/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
$NDK/x86_64-linux-android24-clang -O2 -o vvpatch src/vvpatch.c src/miniz.c -DMINIZ_NO_TIME
```

(For ARM Waydroid/phones, cross-compile `vvpatch` for `aarch64-linux-android` too
and have `service.sh` pick the right ABI.)

## Install

Zip `module.prop`, `service.sh`, `vvpatch` into a Magisk module zip and flash it,
or drop them under `/data/adb/modules/mcpe_vv/` and reboot. Requires root (Magisk).
