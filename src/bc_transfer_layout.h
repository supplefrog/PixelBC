// SPDX-License-Identifier: MIT
#pragma once
#include "bc_layout.h"

namespace pvb {
inline bool is_any_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
// Mip-major canonical compressed storage. Each mip contains tightly packed
// block rows for every array layer; all offsets remain block-byte aligned.
struct BlockMip { uint64_t offset, layer_bytes; uint32_t blocks_row, blocks_height; };
inline bool block_mip(VkFormat format, VkExtent3D extent, uint32_t mip, uint32_t layers, uint64_t offset, BlockMip &out, uint64_t &end) {
    if (!is_bc(format) || !extent.width || !extent.height || extent.depth != 1 || mip >= 32 || !layers) return false;
    const uint32_t row = uint32_t((uint64_t(std::max(1u, extent.width >> mip)) + 3) / 4);
    const uint32_t height = uint32_t((uint64_t(std::max(1u, extent.height >> mip)) + 3) / 4);
    uint64_t layer, total;
    if (!checked_multiply(row, height, layer) || !checked_multiply(layer, block_bytes(format), layer) ||
        !checked_multiply(layer, layers, total) || offset > UINT64_MAX - total) return false;
    out = {offset, layer, row, height}; end = offset + total; return true;
}
}
