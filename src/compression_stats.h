// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
// Additive observation ABI: logical texel/block bytes exclude native allocation
// alignment, tiling, metadata and command-owned staging. No asset identifiers.
struct PvbCompressionStats {
    uint32_t version;
    uint64_t native_etc2_devices;
    uint64_t live_etc2_images, live_rgba_images;
    uint64_t live_etc2_logical_bytes, live_rgba_logical_bytes;
    uint64_t etc2_upload_dispatches, rgba_upload_dispatches;
    uint64_t cross_backing_regenerations;
};
using PvbGetCompressionStats=void (*)(PvbCompressionStats*);
