// SPDX-License-Identifier: MIT
// Source-BC fixtures specified independently of any ETC encoder output.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>
#include "bc_transfer_layout.h"

namespace etc2_adversarial {
struct Case { const char *name; uint32_t width,height,layers,family; };
inline constexpr std::array<Case,10> cases={{{"chromatic_two_color",32,16,2,0},
 {"gray_gradient",32,16,2,1},{"quantization_boundaries",32,16,2,2},
 {"alpha_palette_orders",32,32,2,3},{"coverage_and_fraction",32,16,2,4},
 {"one_texel_cube",1,1,6,4},{"tiny_mip",2,3,2,2},
 {"partial_cube_blocks",7,5,6,0},{"narrow_array_blocks",9,2,2,3},{"bc3_skew_with_exact_extremes",4,4,1,5}}};
inline std::vector<uint8_t> blocks(VkFormat format,const Case &c) {
    const uint32_t br=(c.width+3)/4,bh=(c.height+3)/4,bs=pvb::block_bytes(format);
    std::vector<uint8_t> result(size_t(br)*bh*c.layers*bs);
    constexpr std::array<std::array<uint16_t,2>,8> colors={{{0xf800,0x001f},
      {0x07e0,0xf81f},{0xffe0,0x001f},{0x07ff,0xf800},{0xffff,0},
      {0xf81f,0x07e0},{0x001f,0xffe0},{0xf800,0x07ff}}};
    constexpr std::array<std::array<uint8_t,2>,16> alphas={{{0,1},{1,0},
      {3,0},{0,3},{255,254},{254,255},{255,0},{0,255},
      {17,239},{239,17},{128,127},{127,128},{42,85},{85,42},{0,0},{255,255}}};
    auto put16=[](uint8_t *p,uint16_t v) { p[0]=uint8_t(v);p[1]=uint8_t(v>>8); };
    for(size_t n=0;n<size_t(br)*bh*c.layers;++n) {
        auto *b=result.data()+n*bs;
        if(c.family==5) {
            constexpr std::array<uint8_t,16> skew={0x13,0x9a,0x74,0xbd,0x08,0x4c,0xeb,0xe8,0x1d,0x9b,0x2b,0xac,0x99,0xc3,0x42,0x36};
            if(bs!=16) throw std::runtime_error("BC3-only skew fixture requested for another block size");
            std::memcpy(b,skew.data(),skew.size());continue;
        }
        auto *rgb=b+(format>=VK_FORMAT_BC3_UNORM_BLOCK?8:0);
        uint16_t a=0xffff,z=0;
        if(c.family==0) { a=colors[n%colors.size()][0];z=colors[n%colors.size()][1]; }
        if(c.family==2) { a=uint16_t((n*977u)^0x7befu);z=uint16_t(a^(1u<<((n/8)%16))); }
        if(c.family==3) { a=0xffff;z=0; }
        if(c.family==4) { a=0;z=0xffff; }
        put16(rgb,a);put16(rgb+2,z);uint32_t indices=0;
        for(uint32_t p=0;p<16;++p) {
            const uint32_t x=p%4,y=p/4;
            const uint32_t index=c.family==0?((x+y+n)&1u):
              c.family==1?((x+n)&3u):c.family==2?((p+n)&3u):
              c.family==4?((p+n)%3==0?3u:uint32_t((p+n)&1u)):((p*3+n)&3u);
            indices|=index<<(p*2);
        }
        for(uint32_t k=0;k<4;++k) rgb[4+k]=uint8_t(indices>>(8*k));
        if(format>=VK_FORMAT_BC3_UNORM_BLOCK) {
            b[0]=alphas[n%alphas.size()][0];b[1]=alphas[n%alphas.size()][1];
            if(c.family==4) { b[0]=3;b[1]=0; }
            uint64_t selectors=0;
            for(uint32_t p=0;p<16;++p) {
                const uint32_t index=c.family==4?(p%4==0?6u:p%4==1?1u:0u):uint32_t((p*5+n)&7u);
                selectors|=uint64_t(index)<<(p*3);
            }
            for(uint32_t k=0;k<6;++k) b[2+k]=uint8_t(selectors>>(8*k));
        }
    }
    return result;
}
}
