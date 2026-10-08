// SPDX-License-Identifier: MIT
// Independent signed BC4/5 arithmetic oracle, compiled under both plain-char modes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#define BCDEC_IMPLEMENTATION
#include "reference_bcdec.h"
namespace {
unsigned checks=0, failures=0;
void check(bool value) { ++checks; if(!value) ++failures; }
int endpoint(unsigned byte) { return byte<128 ? int(byte) : int(byte)-256; }
int canonical(unsigned byte) { return std::max(-127,endpoint(byte)); }
void selectors(uint8_t *b) { uint64_t word=0; for(unsigned p=0;p<16;++p) word|=uint64_t(p%8)<<(p*3); for(unsigned n=0;n<6;++n) b[n+2]=uint8_t(word>>(n*8)); }
uint32_t next(uint32_t &s) { s^=s<<13; s^=s>>17; s^=s<<5; return s; }
}
int main() {
    // Exhaust every endpoint pair and every selector; floating reference has
    // separate interpolation arithmetic, integer reference may round by 1 LSB.
    alignas(8) std::array<uint8_t,16> block{}; std::array<signed char,32> decoded{}; std::array<float,32> floats{};
    for(unsigned a=0;a<256;++a) for(unsigned z=0;z<256;++z) {
        block[0]=uint8_t(a); block[1]=uint8_t(z); selectors(block.data());
        bcdec__bc4_block(block.data(),decoded.data(),4,1,1);
        bcdec__bc4_block_float(block.data(),floats.data(),4,1,1);
        const int first=canonical(a), last=canonical(z), divisor=first>last?7:5;
        for(unsigned p=0;p<16;++p) {
            unsigned s=p%8; double ideal=s==0?first:s==1?last:first<=last&&s>=6?(s==6?-127:127):double((divisor-int(s)+1)*first+(int(s)-1)*last)/divisor;
            check(std::abs(int(decoded[p])-int(std::floor(ideal+0.5)))<=1);
            check(std::abs(double(floats[p])-ideal/127.0)<0.000001);
        }
    }
    block[0]=128; block[1]=127; selectors(block.data());
    bcdec__bc4_block(block.data(),decoded.data(),4,1,1);
    constexpr int palette[8]={-127,127,-76,-25,25,76,-127,127};
    for(unsigned p=0;p<16;++p) check(decoded[p]==palette[p%8]);
    // Pixel's three exact random fixtures: same input seed as the GPU probe,
    // literal expected anchors recorded before patching the upstream snapshot.
    for(unsigned format:{140u,142u}) for(unsigned fixture=1;fixture<=3;++fixture) {
        unsigned width=fixture==1?9:7,height=fixture==1?7:5,mip=fixture==1?0:fixture-1;
        uint32_t seed=0x7410d0abu+format*101+width+height+mip;
        for(unsigned n=0;n<(format==142?16u:8u);n+=4) { uint32_t word=next(seed); for(unsigned j=0;j<4;++j) block[n+j]=uint8_t(word>>(8*j)); }
        decoded.fill(0); bcdec__bc4_block(block.data(),decoded.data(),8,2,1);
        if(format==142) bcdec__bc4_block(block.data()+8,decoded.data()+1,8,2,1);
        constexpr int anchors[2][3]={{-127,-17,-127},{54,-94,3}};
        unsigned byte=format==140?0:fixture==1?2:1;
        check(decoded[byte]==anchors[format==142][fixture-1]);
    }
    std::printf("SIGNED_REFERENCE: char_signed=%d; %u checks; %u failures\n",char(-1)<0,checks,failures);
    return failures?1:0;
}
