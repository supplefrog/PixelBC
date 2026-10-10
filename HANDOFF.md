# PixelBC 0.1.9 handoff

This is the current source-only snapshot. Runtime, shader, build, independent test, license and dependency-pin files preserve the tested 0.1.9 bytes. Public documentation is refreshed; private project guidance, workflow history, diagnostic inventories and binary artifacts are excluded.

## Current behavior and identity

The default profile uses native lossy ETC2 for eligible BC1/BC3 images. Exactly 512×1024 and 256×2048 top-level BC1_RGBA UNORM/sRGB images use lossless RGBA for all mips/layers, selected immutably at creation. BC2, BC4/5 signed and unsigned, BC7 and supported noneligible cases retain lossless recovery. Original BC transfer readbacks remain byte-faithful. Global BC capability remains disabled; BC6H and unsupported Vulkan contracts remain unsupported.

The tested ARM64/API 29 library is 410,624 bytes, SHA256 `07136d6c152855980f05ab1eb9fee674c196c8a1d5bd127be3de5c1ab8d656ea`. Its unchanged import ZIP is 157,198 bytes, SHA256 `7f24046895dc7ffab473be98edea848c547b4b3ae3dbc5ef810d4133f03a9def`. Neither binary is included in this source export. [file-index.json](file-index.json) identifies the exported files; build inputs are in [sources.lock.json](sources.lock.json).

## Evidence and limits

Reported local Pixel 7 / Mali-G710 policy acceptance: **286 passed / 0 failed**. Missing-dispatch control: **278 passed / 8 expected failures**, exit 1, normal cleanup. Focused runtime was verified in MW Mobile 0.4.0 and Unleashed Android 0.6.0; both selected 0.1.9 at handoff. User satisfaction and noticeably fewer Sonic hitches are subjective acceptance, not a controlled heavy-area result.

Lossy ETC2 changes sampled color/alpha and saves residency compared with RGBA. The shape-selected RGBA route reverses that memory/bandwidth tradeoff. An unchanged synthetic adversarial RGB RMSE **45.108863** exceeds the RGB32 guard; it remains failed evidence. There is no universal quality or Vulkan conformance claim, no three qualifying 60-second racing runs meeting the hitch target, no input-latency proof, and no Android validation-layer proof. Recent capped plaza comparisons did not resolve the designated hub/night areas. Possible slight color/shadow differences at low brightness were uncertain and were not display-diagnosed. Details and reproducibility boundaries are in [VALIDATION.md](docs/VALIDATION.md).

## Stop state

No further wrapper optimization is scheduled. Existing recovery packages remain available to their owner. No game renderer, APK, saves, Android security or thermal controls were changed by this export.

The **strongest remaining improvement opportunities are game renderer/shader source changes**: trustworthy content-based texture reuse/invalidation/lifetime, conversion avoidance or prewarming before first use, and resource/job scheduling. GPU block memoization has no established runtime hit rate or native break-even; no cache is implemented. Independent CPU scheduling and thermal limits can remain relevant even if conversion work is reduced. The [existing MW renderer issue 1668](https://github.com/codepdbh/nfsmw-android/issues/1668) records the source-side finding. Further renderer/APK work requires a separate request; no particular source change is promised to remove all hitches.
