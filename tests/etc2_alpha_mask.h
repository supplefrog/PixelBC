// SPDX-License-Identifier: MIT
// Probe-only alpha endpoint classification before any integer rounding.
#pragma once
#include <vulkan/vulkan.h>
#include "bc_transfer_layout.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace etc2_alpha {
enum Endpoint : uint8_t { interpolated=0, zero=1, opaque=2 };
struct Fraction {
    uint32_t numerator,denominator;
    Endpoint endpoint() const {
        return numerator==0?zero:numerator==255*denominator?opaque:interpolated;
    }
};
inline Fraction bc_source(VkFormat format,const uint8_t *block,uint32_t pixel) {
    if(pixel>=16) throw std::runtime_error("BC alpha pixel outside block");
    if(format==VK_FORMAT_BC1_RGB_UNORM_BLOCK || format==VK_FORMAT_BC1_RGB_SRGB_BLOCK) return {255,1};
    if(format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format==VK_FORMAT_BC1_RGBA_SRGB_BLOCK) {
        const uint32_t c0=uint32_t(block[0])|(uint32_t(block[1])<<8),c1=uint32_t(block[2])|(uint32_t(block[3])<<8);
        const uint32_t selector=(block[4+pixel/4]>>(2*(pixel%4)))&3;
        return {c0<=c1 && selector==3?0u:255u,1};
    }
    if(format==VK_FORMAT_BC2_UNORM_BLOCK || format==VK_FORMAT_BC2_SRGB_BLOCK)
        return {uint32_t((block[pixel/2]>>(4*(pixel%2)))&15)*17,1};
    if(format==VK_FORMAT_BC3_UNORM_BLOCK || format==VK_FORMAT_BC3_SRGB_BLOCK) {
        uint64_t selectors=0; for(uint32_t n=0;n<6;++n) selectors|=uint64_t(block[n+2])<<(8*n);
        const uint32_t selector=uint32_t((selectors>>(3*pixel))&7),a0=block[0],a1=block[1];
        if(selector==0) return {a0,1};
        if(selector==1) return {a1,1};
        if(a0>a1) return {(8-selector)*a0+(selector-1)*a1,7};
        if(selector<=5) return {(6-selector)*a0+(selector-1)*a1,5};
        return {selector==6?0u:255u,1};
    }
    throw std::runtime_error("BC alpha fraction requires unsigned BC1/2/3");
}
using Mask=std::vector<uint8_t>;
inline Mask source_mask(VkFormat format,const std::vector<uint8_t> &bc,uint32_t width,uint32_t height,uint32_t layers) {
    const uint32_t br=(width+3)/4,bh=(height+3)/4,bs=pvb::block_bytes(format);
    if(bc.size()!=size_t(br)*bh*layers*bs) throw std::runtime_error("BC alpha mask size mismatch");
    Mask mask(size_t(width)*height*layers);
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
        const auto *block=bc.data()+((size_t(l)*bh+y/4)*br+x/4)*bs;
        mask[(size_t(l)*height+y)*width+x]=bc_source(format,block,(y%4)*4+x%4).endpoint();
    }
    return mask;
}
inline Mask decoded_mask(const std::vector<uint8_t> &rgba) {
    if(rgba.size()%4) throw std::runtime_error("Decoded alpha mask requires RGBA pixels");
    Mask mask(rgba.size()/4);
    for(size_t n=0;n<mask.size();++n) mask[n]=rgba[n*4+3]==0?zero:rgba[n*4+3]==255?opaque:interpolated;
    return mask;
}
inline Mask sampled_mask(VkFormat format,const std::vector<uint8_t> &bc,const std::vector<uint8_t> &decoded,uint32_t width,uint32_t height,uint32_t layers,bool encoded,bool raw_native=false) {
    // ETC/EAC outputs have integer alpha endpoints. A wrapper must also keep
    // genuine original BC zero/255 exact, even if its encoded oracle is wrong.
    if(raw_native) return decoded_mask(decoded);
    auto mask=source_mask(format,bc,width,height,layers);
    if(encoded) {
        const auto native=decoded_mask(decoded);
        if(native.size()!=mask.size()) throw std::runtime_error("Sampled alpha mask size mismatch");
        for(size_t n=0;n<mask.size();++n) if(mask[n]==interpolated) mask[n]=native[n];
    }
    return mask;
}
inline Mask bilinear_mask(const Mask &source,uint32_t width,uint32_t height,uint32_t layers) {
    if(source.size()!=size_t(width)*height*layers) throw std::runtime_error("Filtered alpha mask size mismatch");
    Mask mask(source.size());
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
        const size_t pixel=(size_t(l)*height+y)*width+x; const auto endpoint=source[pixel];
        bool same=endpoint!=interpolated;
        for(uint32_t dy=0;dy<2;++dy) for(uint32_t dx=0;dx<2;++dx)
            same&=source[(size_t(l)*height+std::min(y+dy,height-1))*width+std::min(x+dx,width-1)]==endpoint;
        mask[pixel]=same?endpoint:uint8_t(interpolated);
    }
    return mask;
}
}
