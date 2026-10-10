# Architecture

PixelBC loads the installed Vulkan backend and intercepts the Vulkan texture paths needed for supported virtual BC images. On Android it exposes the custom-driver HAL entry and keeps the backend loaded for callback lifetime. It does not distribute or replace the vendor driver.

## Immutable image backing

`src/etc2_formats.h` and the shared selector in `src/wrapper.cpp` select native storage before image creation. Supported BC1_RGBA UNORM/sRGB images at exactly512×1024 or256×2048 use lossless RGBA. Other eligible BC1/BC3 images use ETC2 only after native sampling/filter/transfer and exact image-property checks. Other supported cases recover to RGBA. The same selector drives supported core/KHR create-without-image memory-requirement queries. Backing does not switch on upload, data or first use.

Eligibility is conservative: optimal 2D, supported mip/layer/cube shapes, single-sample sampled/transfer usages, and supported flags/views. BC2 is lossless; BC4/5 signed values and BC7 retain their established decode route. sRGB views preserve color-space behavior. Unsupported sparse/external/protected/storage/attachment/3D/linear contracts are explicitly rejected. BC6H is unsupported. Individual format queries remain conservative and global `textureCompressionBC` stays false.

## Upload and command ownership

Supported buffer-to-image uploads copy compressed input on the GPU, dispatch the selected decode/encoder, then copy the output to native image storage in caller command order. There is no additional asynchronous queue or submit, CPU texture readback, disk cache or first-use delay. Command-recording-owned arenas and descriptors remain alive for executable, pending and replayed command buffers, retiring on valid reset/free/device cleanup paths. State capture/replay restores compute bindings, compatible-layout disturbance, descriptor dynamic offsets and push-constant stage/byte history around internal work.

Original BC bytes are retained where transfer-source use requires them. Image copies between compatible virtual images regenerate the destination's appropriate backing; sampled lossy storage is not substituted for original compressed readback. See [transfer behavior](BC_TRANSFER_INTERNAL.md).

## Cost and remaining leverage

ETC2 reduces resident texture size and filtered sampling bandwidth but adds GPU conversion work. RGBA avoids fitting and costs more resident memory/bandwidth. The exact-shape policy is a bounded tradeoff, not a transparent cache. The wrapper does not know immutable game asset identities or CPU upload-buffer content versions; reused handles/offsets are insufficient cache keys.

The strongest remaining opportunities are in game renderer/shader source changes: correct content reuse/invalidation/lifetime, conversion avoidance/prewarming and resource/job scheduling. Cache applicability and native break-even are unproven. Conversion changes cannot resolve every independent CPU scheduling or thermal limitation.
