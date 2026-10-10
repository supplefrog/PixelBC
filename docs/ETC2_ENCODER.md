# ETC2 encoder and provenance

The GPU encoder consumes original BC blocks and emits native ETC2 blocks at upload time. It preserves dimensions, mips, layers and color space. BC1/BC3 recompression is lossy; exact original compressed readbacks are maintained separately. BC1_RGBA's binary source alpha coverage is exact. BC2 remains lossless RGBA because its intermediate-alpha representation needs a separate quality decision. BC4/5 signed and unsigned and BC7 remain on their established lossless routes.

RGB fitting adapts the MIT Leegao/etc2_encode mechanism pinned in [sources.lock.json](../sources.lock.json). The EAC modifier table originates from the ISC detex source. The driver uses weighted source palettes, valid-edge texel weighting, bounded legal ETC/EAC fitting and error recovery. It does not link the independent detex decoder; the tests do.

The BC1 production module is the weighted-only specialization. The BC3 production module uses corrected bounded alpha fitting/recovery. Source masks distinguish genuine rational alpha0/255 coverage from interpolants that a CPU reference rounds to an endpoint. Existing source quality gates are retained, including the known adversarial RGB failure documented in [VALIDATION.md](VALIDATION.md).

Native eligibility and the exact-shape RGBA exceptions are in [ARCHITECTURE.md](ARCHITECTURE.md). Full copyright/license notices are preserved in [THIRD_PARTY.md](THIRD_PARTY.md), [licenses/](../licenses/) and source headers. [ETC2_ENCODER_BOUNDED.md](ETC2_ENCODER_BOUNDED.md) describes the analytical prior art and specialization boundary.
