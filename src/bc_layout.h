// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <limits>

namespace pvb {
inline bool is_bc(VkFormat f) { return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC5_SNORM_BLOCK) || f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK; }
inline bool is_signed(VkFormat f) { return f == VK_FORMAT_BC4_SNORM_BLOCK || f == VK_FORMAT_BC5_SNORM_BLOCK; }
inline uint32_t block_bytes(VkFormat f) { return f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ? 8 : 16; }
inline VkFormat rgba_format(VkFormat f) {
    if (is_signed(f)) return VK_FORMAT_R8G8B8A8_SNORM;
    if (f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC2_SRGB_BLOCK || f == VK_FORMAT_BC3_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK) return VK_FORMAT_R8G8B8A8_SRGB;
    return VK_FORMAT_R8G8B8A8_UNORM;
}
inline bool compatible_view(VkFormat image, VkFormat view) {
    if (image == view) return true;
    if ((image == VK_FORMAT_BC7_UNORM_BLOCK || image == VK_FORMAT_BC7_SRGB_BLOCK) &&
        (view == VK_FORMAT_BC7_UNORM_BLOCK || view == VK_FORMAT_BC7_SRGB_BLOCK)) return true;
    // Only color-space reinterpretation of the same decoded BC family is valid.
    return image <= VK_FORMAT_BC3_SRGB_BLOCK && view <= VK_FORMAT_BC3_SRGB_BLOCK && is_bc(view) &&
           (uint32_t(image) - 131) / 2 == (uint32_t(view) - 131) / 2;
}
inline bool checked_multiply(uint64_t a, uint64_t b, uint64_t &out) {
    if (b && a > UINT64_MAX / b) return false;
    out = a * b; return true;
}
struct CopyLayout { uint32_t blocks_row, blocks_height, layers; uint64_t input_bytes, output_bytes; };
inline bool copy_layout(VkFormat f, const VkBufferImageCopy &r, const VkExtent3D &image, uint32_t mips, uint32_t layers, CopyLayout &out) {
    if (!is_bc(f) || r.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || r.imageSubresource.mipLevel >= mips || !r.imageSubresource.layerCount ||
        r.imageSubresource.baseArrayLayer >= layers || r.imageSubresource.layerCount > layers - r.imageSubresource.baseArrayLayer ||
        r.imageOffset.x < 0 || r.imageOffset.y < 0 || r.imageOffset.z || r.imageExtent.depth != 1 || !r.imageExtent.width || !r.imageExtent.height ||
        (r.imageOffset.x % 4) || (r.imageOffset.y % 4) || (r.bufferOffset % block_bytes(f))) return false;
    const uint32_t w = std::max(1u, image.width >> std::min(r.imageSubresource.mipLevel, 31u));
    const uint32_t h = std::max(1u, image.height >> std::min(r.imageSubresource.mipLevel, 31u));
    const uint32_t x = uint32_t(r.imageOffset.x), y = uint32_t(r.imageOffset.y);
    if (x > w || r.imageExtent.width > w - x || y > h || r.imageExtent.height > h - y ||
        ((r.imageExtent.width % 4) && x + r.imageExtent.width != w) || ((r.imageExtent.height % 4) && y + r.imageExtent.height != h) ||
        (r.bufferRowLength && (r.bufferRowLength < r.imageExtent.width || r.bufferRowLength % 4)) ||
        (r.bufferImageHeight && (r.bufferImageHeight < r.imageExtent.height || r.bufferImageHeight % 4))) return false;
    const uint64_t br = (uint64_t(r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width) + 3) / 4;
    const uint64_t bh = (uint64_t(r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height) + 3) / 4;
    const uint64_t used_rows = (uint64_t(r.imageExtent.height) + 3) / 4;
    const uint64_t used_cols = (uint64_t(r.imageExtent.width) + 3) / 4;
    // Do not copy padding after the last used block; padded rows/layers still retain their offsets.
    uint64_t layer_stride, last_layer, last_row, input, output;
    if (!checked_multiply(br, bh, layer_stride) || !checked_multiply(r.imageSubresource.layerCount - 1, layer_stride, last_layer) ||
        !checked_multiply(used_rows - 1, br, last_row) || last_layer > UINT64_MAX - last_row || last_layer + last_row > UINT64_MAX - used_cols ||
        !checked_multiply(last_layer + last_row + used_cols, block_bytes(f), input) ||
        !checked_multiply(r.imageExtent.width, r.imageExtent.height, output) ||
        !checked_multiply(output, r.imageSubresource.layerCount, output) || !checked_multiply(output, 4, output)) return false;
    // Shader addressing uses 32-bit words, and Vulkan dispatch limits are checked separately.
    if (!input || !output || input > std::numeric_limits<uint32_t>::max() || output > std::numeric_limits<uint32_t>::max() || r.bufferOffset > UINT64_MAX - input) return false;
    out = {uint32_t(br), uint32_t(bh), r.imageSubresource.layerCount, input, output};
    return true;
}
}
