# PixelBC 0.1.9

PixelBC is an experimental Android Vulkan wrapper for stock Pixel 7 / Mali-G710. It exposes individually supported BC1–5 and BC7 texture formats and delegates ordinary Vulkan work to the installed system driver. Applications must support importing a custom driver; this project does not replace Android's system driver or modify an APK.

[Download the 0.1.9 experimental import ZIP and checksums](https://github.com/supplefrog/PixelBC/releases/tag/v0.1.9), then select the ZIP in your application's custom-driver picker.

**The default 0.1.9 profile uses lossy ETC2 recompression for eligible BC1/BC3 sampled images.** Other supported cases use lossless RGBA decoding. BC1_RGBA UNORM/sRGB images whose top-level dimensions are exactly **512×1024 or 256×2048** use lossless RGBA for the entire image, including every mip and layer. This reduces conversion work for those two shapes while increasing resident memory and sampling bandwidth. It is a measured experimental policy, not a general size threshold.

Focused local Pixel testing passed the 0.1.9 policy probe and game runtime checks for NFS Most Wanted Mobile 0.4.0 and Unleashed Android 0.6.0. The tester reported satisfaction and fewer Sonic hitches. These observations do not establish matched heavy-scene performance, hitch elimination, input latency, or universal visual fidelity. A retained synthetic adversarial RGB case exceeds the project's quality gate. Read [validation and limits](docs/VALIDATION.md) before use.

## Supported scope

- BC1–5 unsigned/signed as applicable, plus BC7, with individual format queries. BC6H is unsupported and the global `textureCompressionBC` feature remains false.
- Optimal 2D images, including supported arrays, cube images and mip chains; conservative sampled/transfer usage and view restrictions.
- Original BC bytes remain available for supported transfer-source copies/readbacks, independently of lossy sampled storage.
- Eligible native ETC2 routes require the backend's format and image-property support. Other supported images recover to RGBA; unsupported contracts are rejected explicitly.

See [architecture](docs/ARCHITECTURE.md) and [transfer behavior](docs/BC_TRANSFER_INTERNAL.md) for the precise boundaries. An application's global-BC gate or unsupported texture usage may prevent integration; format support alone does not establish application compatibility.

## Build from source

Use Linux, Python 3, CMake, Ninja, Vulkan headers, `glslangValidator` and `spirv-val`. The Android build is ARM64/API 29 with static C++, pinned NDK r30 (`30.0.16248370`). Exact dependency identities are in [sources.lock.json](sources.lock.json). The tested shader tools were glslang 15.2.0 and SPIRV-Tools 2025.1.

```bash
export ANDROID_NDK_ROOT=/path/to/android-ndk-r30
bash scripts/build_android.sh work/build-android
```

The default options are `PVB_ETC2=ON`, `PVB_BC1_SHAPE_RGBA=ON`, `PVB_UPLOAD_ARENAS=ON`, `PVB_COMPACT_HISTORY=ON`, and `PVB_SHARED_DECODE=OFF`. A separate lossless build can be made with `PVB_ETC2=OFF`; it uses more texture memory. Preserve a known working package before testing another build.

For a compact import ZIP, preserve the completed build and package a copy:

```bash
cp -a work/build-android work/package-android
"$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
  --strip-unneeded work/package-android/libvulkan_pixel_bc.so
python3 scripts/package.py work/package-android --variant shape-rgba \
  --readelf "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
```

The packaging tool checks the Android HAL ABI, system-library dependencies, 16 KiB ELF load alignment and `NODELETE`, and includes the required notices. It refuses to overwrite preserved output files. Source export does not include binaries, toolchains or game data. The frozen packaging script's conservative "UNVERIFIED" device metadata is historical; the bounded reported device results and remaining limits are recorded separately in [VALIDATION.md](docs/VALIDATION.md).

## Check a build

Run the offscreen probes against an explicit library path; they do not launch a game:

```bash
timeout 45s ./pixel_shape_policy_probe ./libvulkan_pixel_bc.so --require-native
timeout 45s ./pixel_shape_policy_probe ./libvulkan_pixel_bc.so --require-native --omit-final-dispatch
```

The first command must pass; the second intentionally fails sampled-work checks and must still shut down normally. [VALIDATION.md](docs/VALIDATION.md) also describes host and quality checks and the retained failures. Software Vulkan checks are correctness evidence, not Pixel performance measurements.

## Status and remaining work

The bounded wrapper investigation is stopped at 0.1.9. The strongest remaining improvement opportunities are in **game renderer/shader source changes**: content-based texture reuse with correct invalidation and lifetime, avoiding or prewarming conversions, and resource/job scheduling. These are opportunities to investigate, not a promise that one patch fixes every hitch. The MW renderer finding is recorded in [codepdbh/nfsmw-android issue 1668](https://github.com/codepdbh/nfsmw-android/issues/1668). Renderer or APK implementation is outside this wrapper snapshot.

See [HANDOFF.md](HANDOFF.md) for the current stop state. PixelBC is MIT licensed with third-party notices in [THIRD_PARTY.md](docs/THIRD_PARTY.md) and [licenses/](licenses/).
