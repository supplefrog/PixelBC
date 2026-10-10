// SPDX-License-Identifier: MIT
// Preserve the exact actual Pixel 0.1.5 failure through the actual GPU module.
#define PVB_ETC2_NO_MAIN
#include "etc2_probe.cpp"
#include <fstream>

namespace {
std::vector<uint32_t> regression_module(const char *path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in) throw std::runtime_error("Cannot open regression SPIR-V");
    const auto bytes=in.tellg();
    if(bytes<20 || bytes>262144 || bytes%4) throw std::runtime_error("Invalid bounded regression SPIR-V");
    std::vector<uint32_t> words(size_t(bytes)/4);in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()),bytes);
    if(!in || words.front()!=0x07230203u) throw std::runtime_error("Invalid regression SPIR-V header");
    return words;
}
void source_quality(VkFormat f,const Bytes &bc,const Bytes &etc,uint32_t blocks,const char *name) {
    ErrorMetric rgb,alpha;uint64_t extreme_errors=0,outlier_blocks=0;
    for(uint32_t n=0;n<blocks;++n) {
        const auto original=reference(f,bc.data()+size_t(n)*pvb::block_bytes(f));
        const auto decoded=etc_reference(f,etc.data()+size_t(n)*etc_bytes(f));
        uint32_t peak=0;
        for(uint32_t p=0;p<16;++p) {
            for(uint32_t c=0;c<3;++c) if(original[p*4+3])
                rgb.add(uint32_t(std::abs(int(original[p*4+c])-int(decoded[p*4+c]))));
            const uint32_t error=uint32_t(std::abs(int(original[p*4+3])-int(decoded[p*4+3])));
            alpha.add(error);peak=std::max(peak,error);
            const auto endpoint=etc2_alpha::bc_source(f,bc.data()+size_t(n)*pvb::block_bytes(f),p).endpoint();
            extreme_errors+=endpoint==etc2_alpha::zero && decoded[p*4+3]!=0;
            extreme_errors+=endpoint==etc2_alpha::opaque && decoded[p*4+3]!=255;
        }
        if(peak>16) {
            ++outlier_blocks;
            std::printf("ALPHA_REGRESSION_OUTLIER name=%s format=%u block=%u peak=%u BC=",name,uint32_t(f),n,peak);
            for(uint32_t k=0;k<pvb::block_bytes(f);++k) std::printf("%02x",bc[size_t(n)*pvb::block_bytes(f)+k]);
            std::printf("\n");
        }
    }
    std::printf("ALPHA_REGRESSION {\"name\":\"%s\",\"seed\":%u,\"format\":%u,\"blocks\":%u,\"outlier_blocks\":%llu,\"extreme_alpha_errors\":%llu,",name,corpus_seed,uint32_t(f),blocks,(unsigned long long)outlier_blocks,(unsigned long long)extreme_errors);
    print_metric(rgb,"visible_rgb");std::printf(",");print_metric(alpha,"alpha");std::printf("}\n");
    check(rgb.rmse()<=32 && alpha.rmse()<=8 && alpha.maximum<=16 && !extreme_errors,
          "Original source/detex regression keeps RGB32, alpha8/max16 and genuine endpoints exact");
}
}

int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc<2 || argc>4) { std::fprintf(stderr,"Usage: pixel_etc2_alpha_regression explicit-library.so [explicit-reference-module.spv] [--validation]\n");return 2; }
    try {
        bool validation=false;std::vector<uint32_t> module;
        for(int n=2;n<argc;++n) {
            if(std::string(argv[n])=="--validation") validation=true;
            else if(module.empty()) module=regression_module(argv[n]);
            else throw std::runtime_error("Duplicate explicit regression module");
        }
        const auto *selected=module.empty()?nullptr:&module;
        EncoderApp a;init(a,argv[1],validation,false);oracle_anchors();
        const std::array<uint8_t,16> failure={0x40,0xe4,0x73,0x31,0x1d,0x95,0x83,0xf2,0xad,0xd4,0x2e,0x84,0xf0,0x25,0xd0,0x89};
        const std::array<uint8_t,16> skew={0x13,0x9a,0x74,0xbd,0x08,0x4c,0xeb,0xe8,0x1d,0x9b,0x2b,0xac,0x99,0xc3,0x42,0x36};
        const std::array<uint8_t,16> attempt1={0x11,0xa3,0xdf,0xac,0xaf,0x46,0x08,0x4f,0x61,0x2d,0x6d,0xbc,0xa1,0x31,0xe1,0x21};
        for(const auto f:{VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC3_SRGB_BLOCK}) for(const auto &fixture:{failure,skew,attempt1}) {
            const Bytes bc(fixture.begin(),fixture.end());const auto etc=encode(a,f,bc,4,4,1,selected);
            source_quality(f,bc,etc,1,fixture==failure?"pixel015_512_block10664":fixture==skew?"preserved_skew":"rejected016_attempt1_block32320");
        }
        // Identical original two-layer 512 corpus, not a filtered failing-pixel set.
        corpus_seed=2658517889u;
        for(const auto f:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK}) {
            const auto bc=corpus(f,128,128,2);const auto etc=encode(a,f,bc,512,512,2,selected);
            source_quality(f,bc,etc,32768,"original512_two_layer");
        }
        a.shutdown();
        std::printf("ALPHA_REGRESSION_RESULT: %u passed; %u failed; %u validation errors\n",passed,failed,validation_errors);
        return failed || validation_errors?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"ERROR: %s\n",e.what());return 2; }
}
