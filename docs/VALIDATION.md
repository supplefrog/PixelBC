# Validation and limitations

This page distinguishes retained build/check evidence from reported local device acceptance. Preparing this source export ran byte-identity, dependency-closure, documentation and privacy checks only; it did not rebuild or rerun a device experiment.

## Reported local acceptance

The exact 0.1.9 library and package hashes are in [HANDOFF.md](../HANDOFF.md). On stock Pixel 7 / Mali-G710, the targeted policy probe passed 286/0. Its missing-final-dispatch control produced 278/8 and the intended exit 1 with natural cleanup. These checks exercise actual image backing, query/create agreement, sampled data, original-source alpha and resource paths, rather than inferring correctness from allocation sizes.

Focused game runtime was verified for NFS Most Wanted Mobile 0.4.0 and Unleashed Android 0.6.0. Both selected 0.1.9 at handoff. User reports of fewer Sonic hitches are subjective; there is no matched high-load performance proof. No three qualifying 60-second racing runs passed max displayed gap 50 ms and p99 25 ms, and input latency was not established. Recent capped plaza comparisons did not resolve the designated hub/night scenes. Slight color/shadow differences were uncertain at low brightness, not a confirmed regression or display diagnosis. Android Vulkan validation layers were unavailable during these device tests.

## Retained source/build evidence

The prior ARM64/API 29 static-C++ builds reproduced the stripped library byte-for-byte. The packaged ELF was checked for 16 KiB load alignment, `NODELETE`, Android HAL ABI and public system-library dependencies. Exact build/dependency pins remain in [sources.lock.json](../sources.lock.json). This export preserves tested code bytes but does not promise that arbitrary tool versions produce the same binary.

Production shader identities from those builds:

| Module | Bytes | SHA256 |
| --- | ---: | --- |
| BC decode | 33,720 | `4172fae7cea312276365c1691f7b1db915b1abbce5d1c1753b111c169ab7691e` |
| Weighted BC1 ETC2 specialization | 42,856 | `2d0a4e4630632de4bca66abf6f0222783835fb01ebb9625a9bf225bab06f39d5` |
| Corrected BC3 ETC2 specialization | 91,616 | `a5ef619b789d0d0b3805d82c15b928b2f915ff9c6ab4a9a1858a9d054e9bc785` |

Independent references are the retained bcdec portability-patched CPU decoder and pinned detex ETC/EAC decoder. Probes compare original BC quality separately from native sampling of independently decoded encoded blocks. Genuine rational source alpha 0/255 coverage remains exact; rounded interpolants are classified separately. RGB RMSE32, alpha RMSE8 and BC3 maximum alpha error16 guards are unchanged.

**Known failure:** a retained synthetic adversarial RGB fixture yields visible RGB RMSE45.108863, above32, including the prior baseline. It was not reclassified as passing or excluded. A retained SwiftShader native-sampling diagnostic also failed (282/2); this is not Pixel acceptance or a universal backend verdict. Synthetic held-out passes do not certify all game textures, mods or Vulkan usage.

## Reproducible check entry points

With a software Vulkan ICD, build and run the host suite using `bash scripts/test_host.sh`. Some quality/backend checks have the retained failures above; inspect results rather than treating every nonzero exit as a new regression. The source includes ownership/failure, transfer, state-history, signed-reference, backend-lifetime and native-compression contracts.

With built Android probes and an explicit library in a temporary directory:

```bash
timeout 45s ./pixel_shape_policy_probe ./libvulkan_pixel_bc.so --require-native
timeout 45s ./pixel_shape_policy_probe ./libvulkan_pixel_bc.so --require-native --omit-final-dispatch
timeout 45s ./pixel_etc2_alpha_regression ./libvulkan_pixel_bc.so
```

The omission run is a failing control. The legacy uniform ETC2 probe/benchmark is not a mixed-policy oracle: it assumes one backing route for its arm. Use the shape-policy probe for 0.1.9 routing and sampling. Do not label the mixed library wholly ETC2 or wholly RGBA.

Earlier synthetic native comparisons supported testing RGBA on the two selected shapes: complete upload work was cheaper while filtered sampling and residency were more expensive. They used serial logical devices and synthetic fixtures, not game frames. Cloud software timing is not Mali performance; conversion savings alone do not prove a gameplay hitch fix. No sampled-output expectation is derived from candidate readback, and no quality gate was weakened for this export.
