// SPDX-License-Identifier: MIT
#pragma once
#include "bc_layout.h"
namespace pvb {
// BC2's full sixteen-level alpha ramp cannot meet the original 16-LSB
// maximum-error guard with eight EAC values and exact 0/255. Keep its backing
// lossless in this first candidate; the encoder's BC2 diagnostic is not routing.
inline bool etc2_eligible(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) ||
           f==VK_FORMAT_BC3_UNORM_BLOCK || f==VK_FORMAT_BC3_SRGB_BLOCK;
}
inline VkFormat etc2_format(VkFormat f) {
    const bool srgb=rgba_format(f)==VK_FORMAT_R8G8B8A8_SRGB;
    if(f<=VK_FORMAT_BC1_RGB_SRGB_BLOCK) return srgb?VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    return srgb?VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
}
inline bool is_etc2_backing(VkFormat f) {
    return f==VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK || f==VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK ||
           f==VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK || f==VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
}
inline uint32_t etc2_block_bytes(VkFormat f) { return f<=VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK?8:16; }
inline bool etc2_output_size(VkFormat backing,uint32_t width,uint32_t height,uint32_t layers,uint64_t &out) {
    return checked_multiply((uint64_t(width)+3)/4,(uint64_t(height)+3)/4,out) &&
           checked_multiply(out,layers,out) && checked_multiply(out,etc2_block_bytes(backing),out) && out && out<=UINT32_MAX;
}
}
