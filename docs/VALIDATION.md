# PixelBC 0.1.2 validation

Tested locally on Pixel 7, Mali-G710, Android 17, stock ARM r54p3 backend. Results below are observations, not broad device support guarantees.

## Correctness and lifecycle

Two Android probe runs each passed 535 BC1–5 checks and 20,046 BC7 checks, zero failures, natural process exit. BC7 tests cover all eight modes and independent reference comparison. Cloud Linux llvmpipe tests passed 533 BC1–5 checks, 20,044 BC7 checks, nine CTest cases and zero synchronization-validation errors. Fault/lifecycle/ownership fixtures are included. Android validation layers were unavailable on the phone. Cloud builds were repeated twice with byte-identical library, probes, shaders and package.

The locally imported candidate ZIP SHA256 was `c0b7bf95db6f6cd6ef33e0432170b8659c89933bef2818d198a1e4c796fcd585`; library SHA256 was `8ed57f27f6900b14a1d40fbdc2b5f7c2fb6924d381a30bd77d70097f8b60d88e`. This source export does not redistribute that binary package.

## NFSMW

Actual game tested: victorgbd/NFSMW-Recompiled-Mobile 0.4.0, USA game edition, native renderer pinned at codepdbh/nfsmw-android commit `6df1501974d0db84893f5a3ca465b616414f6961`. Logs confirmed wrapper activation and first GPU BC decode; a 3D race rendered. This does not establish tests of codepdbh's own APK. Subjective smoother play was reported, but resolution/upscaling/settings also changed during the broader session: no isolated wrapper FPS gain is claimed.

## Unleashed

SansNope/UnleashedRecomp-Android 0.6.0, source pin `f9ad6baffe32637ff45f34825c247513e8b379d7`. Import and ordinary loading succeeded. To test GPU decode, the app's existing `driver_import/force_bc.txt` override was enabled after a bounded asset/source audit. BC1/2/3 and embedded BC7 decoding appeared in runtime logs; Apotos hub and a heavier populated daytime area rendered without observed missing textures or driver rejection. A relaunch repeated first GPU decode and returned to playable gameplay.

All 11,604 inspected LZX objects decoded, yielding 23,197 DDS headers: 14,170 BC1, 321 BC2, 8,550 BC3, 123 BGRA8 and 33 legacy FourCC-zero headers. The latter could not select BC/DX10 BC6 in the pinned parser, but precise uncompressed masks remain unclassified. Recognized shapes were 2D/cube, depth one. The source's 74 embedded DDS resources included 17 BC7 textures. Some opaque containers remain outside texture coverage; future mods and every stage are not certified.

The same stationary hub ran near 59 FPS on both CPU-transcode and GPU-decode paths. PSS rose from 1,167,911 KiB to 1,793,650 KiB (about 611 MiB extra); thermal status differed, so this was not a controlled benchmark. A heavier-area GPU capture averaged 42.25 FPS with no matching CPU capture. Smoother play was reported, but no measured FPS gain is established. Retained original BC blocks support compressed readback/copies; decoded RGBA increases texture storage substantially. This route primarily targets CPU texture conversion, not per-frame draw-call/CPU bottlenecks.

Recommended integration: opt-in, per-format and exact image-usage support checks, with CPU fallback retained. Do not make the blanket BC override default. Additional memory-constrained-device, stage, synchronization and sustained tests are needed before broader rollout.
