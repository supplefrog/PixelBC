# PixelBC

Experimental Android Vulkan wrapper for stock Mali, tested on Pixel 7. It decodes BC1–5 and BC7 textures with GPU compute into native uncompressed images, and forwards rendering and presentation to the installed vendor Vulkan HAL. It can be imported through compatible custom-driver loaders such as the ones in NFSMW Mobile and Unleashed Recomp Android.

This is a user-space compatibility layer, not a replacement Mali driver. No vendor driver binaries or game assets are distributed. The implementation was developed with AI assistance and tested with independent CPU references, fault fixtures, and actual device probes. Maintainer review is welcome.

## Status and limits

Version 0.1.2 is an experimental candidate. Pixel probes passed twice: 535 BC1–5 checks and 20,046 BC7 checks per run, with no failures and normal exits. Actual MW Mobile 0.4.0 races and Unleashed Android 0.6.0 Apotos gameplay rendered with GPU BC decode active. These are focused tests, not full Vulkan conformance or whole-game certification. See [validation and tradeoffs](docs/VALIDATION.md).

Supported emulated images are optimal-tiling 2D, single-sample sampled/transfer images, including mip chains and array/cube layers. BC6, linear tiling, storage/attachment usage, external image chains, sparse/protected images and other shapes are unsupported. Broader command-buffer and queue limitations are documented in [architecture](docs/ARCHITECTURE.md) and [transfer semantics](docs/BC_TRANSFER_INTERNAL.md).

`textureCompressionBC` deliberately remains false because BC6 is unsupported. Consumers should query each required format and exact image shape/usage and retain fallback for unsupported cases. Do not globally force every BC format on every device. This does not add descriptor indexing or fix unrelated shader/pipeline limitations.

GPU decode can remove CPU ETC2 recompression during texture loading. It costs decoded texture memory; transfer-source images also retain original compressed blocks. No controlled FPS improvement is established. On the tested Sonic hub, process PSS increased by about 611 MiB and both paths ran near 59 FPS. Other devices and games remain untested.

## Build

Use Linux, Bash, Python 3, CMake, Ninja, glslangValidator, spirv-val and Android NDK r30 (30.0.16248370). Exact tool/source pins are in [sources.lock.json](sources.lock.json). With the host shader tools on PATH:

```sh
export ANDROID_NDK_ROOT=/path/to/android-ndk-r30
bash scripts/build_android.sh work/build-android
python3 scripts/package.py work/build-android \
  --readelf "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
```

The package script produces a flat importable ZIP with metadata, library, LICENSE and NOTICE. Its original candidate metadata conservatively says phone behavior is unverified; current focused device results are documented separately rather than silently changing the tested runtime. The stock backend currently opens `/vendor/lib64/hw/vulkan.mali.so`; other vendor layouts require inspection.

For Linux host tests, install Vulkan headers, a Vulkan implementation and validation layers, then run `bash scripts/test_host.sh`. Host success does not establish Android or Mali compatibility. The two Android probe executables are built alongside the wrapper; inspect their usage before running them through ADB. Android validation layers were not available in the reported Pixel runs.

Import only into an app with a compatible custom-driver loader. Restart the app to load a changed library. Keep the app's original driver selection available for recovery. Unleashed's tested app-owned `force_bc.txt` override was a bounded vanilla-game experiment, not a recommended default integration; safe per-format fallback remains the proposed upstream route.

## Provenance and license

MIT wrapper, with bcdec MIT notices and AOSP Apache-2.0 interface notices retained. NDK runtime notices are included for packaged builds. See [third-party details](docs/THIRD_PARTY.md). Runtime source, shaders, tests, CMake and build/package scripts are preserved from the tested 0.1.2 source; documentation was trimmed for this public export. No private development history, skills, device identifiers, logs, saves, APKs or game assets are included. This export has not been independently rebuilt after publication.
