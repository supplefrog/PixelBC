# BC7 fidelity probe

`pixel_bc7_probe` uses the actual runtime compute shader and the wrapper loaded by
absolute path. Its CPU oracle is the unchanged BC7 function in the pinned bcdec
v0.98 reference; runtime table/algorithm attribution and the MIT notice are in
`src/bc_decode.comp` and `licenses/bcdec-MIT.txt`.

The direct SSBO test requires exact RGBA8 bytes for 19,202 blocks plus 62 padding
blocks. Each mode has 2,048 seeded random blocks with explicit unary mode bits.
Additional fixtures exhaust every legal partition with eight endpoint/selector
patterns, every rotation and index-selection choice, zero-mode blocks and literal
mode6 endpoints. Output guards check buffer bounds. Mode counts and exact-byte
results are printed individually.

The 44 real wrapper uploads cover BC7 UNORM and sRGB, every mode, block edges,
pitched rows and layers, partial uploads, mip levels through 1x1, two-layer arrays,
and genuine cube views sampled at each face's texel centers. Mutable views run in
both color-space directions. The caller's sampling state is bound before upload,
including an early-destroyed compatible layout. Sampled bytes allow one LSB for
texture conversion; direct decoder bytes allow none. Every case also copies the
original compressed blocks back into the pitched upload buffer and requires
exact bytes, offsets and untouched padding. The image usage includes sampled,
transfer source and transfer destination, so these checks require the retained
compressed-transfer implementation. Capability checks require BC6 properties and
creation to remain unsupported and the core BC feature to stay false.

After the root CMake integration, run from the repository with the retained host
environment and headers:

```sh
source work/host-env.sh
cmake -S . -B work/unleashed-bc7-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVULKAN_HEADERS_DIR="$PWD/work/host-root/usr/include"
cmake --build work/unleashed-bc7-build -j 4 --target vulkan_pixel_bc pixel_bc7_probe
work/unleashed-bc7-build/pixel_bc7_probe \
  "$PWD/work/unleashed-bc7-build/libvulkan_pixel_bc.so" --validation
```

The BC7 executable needs generated `sample_spv.h`, `bc_decode_spv.h` and
`bc7_cube_sample_spv.h`; the last header embeds `tests/bc7_cube_sample.comp` using
symbol `bc7_cube_sample_spv`. The Android executable uses the same existing HAL
loader and normal-exit diagnostic ownership as `pixel_bc_probe`.

Author validation used llvmpipe Vulkan 1.3 with validation enabled: the shader
passed SPIR-V validation; the integrated transfer draft passed 20,044 checks,
44 uploads/readbacks, zero sampled-byte difference and zero validation errors.
The prior BC1–5 probe passed 345 checks and 67 uploads. Logs and the combined
scratch source snapshot are in `work/unleashed-bc7-build/` in the main checkout.
These are host synthetic results. Android compilation, Pixel execution, real
presentation, application visuals and performance require their separate gates.
