// SPDX-License-Identifier: MIT
// Synthetic BC -> GPU ETC2 -> independently decoded / natively sampled checks.
// No texture assets, renderer settings, or game FPS are involved.
#define main pixel_bc_probe_main
#include "probe.cpp"
#undef main
#include "bc_etc2_encode_spv.h"
#include "bc_etc2_encode_bc1_spv.h"
#include "bc_etc2_encode_bc3_spv.h"
#include "bc7_cube_sample_spv.h"
#include "etc2_linear_sample_spv.h"
#include "compression_stats.h"
#include "oracle/detex/detex.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <fstream>
#include <memory>
#include "etc2_alpha_mask.h"
#include "etc2_adversarial.h"

namespace {
using Bytes=std::vector<uint8_t>;
using PixelBlock=std::array<uint8_t,64>;
using Clock=std::chrono::steady_clock;
constexpr uint32_t guard=256; // Vulkan storage-buffer offset alignment <=256.
uint32_t corpus_seed=0x9e75c381u;
struct EncoderContext;
// The derived owner releases every cached Vulkan child while App's device and
// callbacks still exist, both on explicit shutdown and exception unwinding.
struct EncoderApp:App {
    std::vector<std::unique_ptr<EncoderContext>> encoders;
    uint64_t encoder_creations=0,encoder_reuses=0,encoder_buffer_growths=0,encoder_dispatches=0;
    bool encoder_reported=false;
    ~EncoderApp();
    void shutdown() noexcept;
};
struct EncoderSelection {
    const std::vector<uint32_t> *common=nullptr,*bc1=nullptr,*bc3=nullptr;
    const std::vector<uint32_t> *select(VkFormat f) const {
        if(common) return common;
        if(f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK && f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) return bc1;
        if(f>=VK_FORMAT_BC3_UNORM_BLOCK && f<=VK_FORMAT_BC3_SRGB_BLOCK) return bc3;
        return nullptr; // BC2 remains the combined diagnostic preview module.
    }
};
struct Options {
    bool baseline=false,validation=false,require_native=false,bench=false,quality_only=false,bench_only=false,linear_only=false,literals_only=false,adversarial_only=false;
    bool corrupt_etc=false,corrupt_oracle=false,omit_dependency=false;
    uint32_t size=256,warmup=3,repeats=7,samples=32,texture_pattern=0;
    double rgb_limit=32,alpha_limit=8;
    std::vector<uint32_t> reference_module,reference_bc1,reference_bc3;
    std::string reference_path,reference_bc1_path,reference_bc3_path;
    bool external_reference() const { return !reference_module.empty() || !reference_bc1.empty() || !reference_bc3.empty(); }
    EncoderSelection reference_selection() const { return {reference_module.empty()?nullptr:&reference_module,reference_bc1.empty()?nullptr:&reference_bc1,reference_bc3.empty()?nullptr:&reference_bc3}; }
};
bool rgba(VkFormat f) { return f>=VK_FORMAT_BC1_RGBA_UNORM_BLOCK; }
bool srgb(VkFormat f) { return (uint32_t(f)&1u)==0; }
bool eligible(VkFormat f) { return (f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK && f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) || (f>=VK_FORMAT_BC3_UNORM_BLOCK && f<=VK_FORMAT_BC3_SRGB_BLOCK); }
bool lossy_expected(const Options &o,VkFormat f,bool native) { return !o.baseline && native && eligible(f); }
void verify_baseline_version(const char *path) {
    auto *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL); if(!handle) throw std::runtime_error(dlerror());
    const auto version=reinterpret_cast<PvbGetVersion>(dlsym(handle,"pvb_get_version"));
    const std::string actual=version?version():""; const auto closed=dlclose(handle);
    if(closed) throw std::runtime_error("Baseline version-check dlclose failed");
    if(actual!="0.1.2" && actual!="0.1.3" && actual!="0.1.4" && actual!=PVB_VERSION) throw std::runtime_error("Unsupported baseline version: "+actual+" (allowed: 0.1.2, 0.1.3, 0.1.4, current candidate)");
}
std::vector<uint32_t> load_reference_encoder(const char *path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in) throw std::runtime_error("Cannot open explicit reference SPIR-V");
    const auto bytes=in.tellg();
    if(bytes<20 || bytes>262144 || bytes%4) throw std::runtime_error("Invalid bounded reference SPIR-V size");
    std::vector<uint32_t> words(size_t(bytes)/4);in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()),bytes);
    if(!in || words.front()!=0x07230203u) throw std::runtime_error("Invalid reference SPIR-V header");
    return words;
}
uint64_t module_identity(const std::vector<uint32_t> &module) {
    uint64_t result=14695981039346656037ull;
    const auto *bytes=reinterpret_cast<const uint8_t*>(module.data());
    for(size_t n=0;n<module.size()*sizeof(uint32_t);++n) { result^=bytes[n];result*=1099511628211ull; }
    return result;
}
void json_string(const char *value) {
    std::putchar('"');
    for(const auto *p=reinterpret_cast<const unsigned char*>(value);*p;++p) {
        if(*p=='"' || *p=='\\') { std::putchar('\\');std::putchar(*p); }
        else if(*p<32) std::printf("\\u%04x",*p);
        else std::putchar(*p);
    }
    std::putchar('"');
}
void encoder_identity(const char *tag,const char *selection,const char *path,VkFormat f,const std::vector<uint32_t> &module) {
    std::printf("%s {\"selection\":",tag);json_string(selection);std::printf(",\"path\":");json_string(path);
    std::printf(",\"format\":%u,\"bytes\":%zu,\"fnv1a64_identity\":\"%016llx\",\"sha256_verification\":\"use immutable delivered manifest before execution\"}\n",uint32_t(f),module.size()*sizeof(uint32_t),(unsigned long long)module_identity(module));
}
uint32_t etc_bytes(VkFormat f) { return rgba(f)?16:8; }
VkFormat etc_format(VkFormat f) {
    return rgba(f)?(srgb(f)?VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)
                  :(srgb(f)?VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
}
PixelBlock etc_reference(VkFormat f,const uint8_t *data) {
    // detex stores uint32_t RGBA pixels; make the output explicitly aligned.
    alignas(8) PixelBlock result{};
    const bool valid=rgba(f)?detexDecompressBlockETC2_EAC(data,DETEX_MODE_MASK_ALL_MODES_ETC2,0,result.data())
                            :detexDecompressBlockETC2(data,DETEX_MODE_MASK_ALL_MODES_ETC2,0,result.data());
    if(!valid) throw std::runtime_error("Independent detex rejected ETC2 block");
    return result;
}
bool near_bytes(const uint8_t *actual,const Bytes &expected,const etc2_alpha::Mask &endpoints,uint32_t tolerance,const char *name) {
    if(expected.size()%4 || endpoints.size()!=expected.size()/4) throw std::runtime_error("Comparison requires an explicit per-pixel alpha endpoint mask");
    uint32_t difference=0; bool equal=true; uint64_t alpha_zero=0,alpha_opaque=0,rounded_zero=0,rounded_opaque=0;
    for(size_t n=0;n<expected.size();++n) {
        const auto delta=uint32_t(std::abs(int(actual[n])-int(expected[n])));
        difference=std::max(difference,delta);
        if(delta>tolerance && equal) {
            std::fprintf(stderr,"ETC_MISMATCH: %s byte=%zu got=%u expected=%u tolerance=%u\n",name,n,actual[n],expected[n],tolerance);
            equal=false;
        }
        if(n%4==3) {
            const auto endpoint=endpoints[n/4];
            if(endpoint==etc2_alpha::interpolated) { rounded_zero+=expected[n]==0; rounded_opaque+=expected[n]==255; continue; }
            if(endpoint!=etc2_alpha::zero && endpoint!=etc2_alpha::opaque) throw std::runtime_error("Invalid alpha endpoint mask");
            const uint32_t exact=endpoint==etc2_alpha::zero?0:255;
            alpha_zero+=exact==0; alpha_opaque+=exact==255;
            if(actual[n]!=exact || expected[n]!=exact) {
                if(equal) std::fprintf(stderr,"ETC_ALPHA_ENDPOINT_MISMATCH: %s byte=%zu got=%u expected=%u exact_source_or_codec_endpoint=%u\n",name,n,actual[n],expected[n],exact);
                equal=false;
            }
        }
    }
    max_difference=std::max(max_difference,difference);
    std::printf("ETC_COMPARE {\"name\":\"%s\",\"bytes\":%zu,\"max_difference\":%u,\"alpha_zero_exact_checks\":%llu,\"alpha_opaque_exact_checks\":%llu,\"rounded_zero_without_endpoint_checks\":%llu,\"rounded_opaque_without_endpoint_checks\":%llu,\"pass\":%s}\n",name,expected.size(),difference,(unsigned long long)alpha_zero,(unsigned long long)alpha_opaque,(unsigned long long)rounded_zero,(unsigned long long)rounded_opaque,equal?"true":"false");
    return equal;
}
void host_barrier(App &a,VkBuffer b,VkAccessFlags access,VkPipelineStageFlags stage_flags) {
    VkBufferMemoryBarrier h{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; h.buffer=b; h.size=VK_WHOLE_SIZE;
    h.srcQueueFamilyIndex=h.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; h.srcAccessMask=access; h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    a.CmdPipelineBarrier(a.command,stage_flags,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&h,0,nullptr);
}
void image_barrier(App &a,VkImage image,uint32_t mip,uint32_t layers,VkImageLayout old_layout,
                   VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to) {
    VkImageMemoryBarrier v{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; v.image=image; v.oldLayout=old_layout; v.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,layers}; v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    v.srcAccessMask=src; v.dstAccessMask=dst; a.CmdPipelineBarrier(a.command,from,to,0,0,nullptr,0,nullptr,1,&v);
}
bool native_support(App &a,VkFormat f) {
    VkPhysicalDeviceFeatures features{}; a.GetPhysicalDeviceFeatures(a.physical,&features); if(!features.textureCompressionETC2) return false;
    constexpr auto usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    // The wrapper admits mutable color views only when both partners support
    // the full contract. Match that policy even on a single-color test image.
    const auto partner=VkFormat(srgb(f)?uint32_t(f)-1u:uint32_t(f)+1u);
    for(auto native:{etc_format(f),etc_format(partner)}) {
        VkFormatProperties p{}; a.GetPhysicalDeviceFormatProperties(a.physical,native,&p); VkImageFormatProperties i{};
        if((p.optimalTilingFeatures&required)!=required || a.GetPhysicalDeviceImageFormatProperties(a.physical,native,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&i)!=VK_SUCCESS) return false;
    }
    return true;
}

// Distinct seed and corpus construction from the encoder's implementation and
// fitting tests. Every texture is made from legal BC bytes, never ETC payloads.
const char *classes[8]={"random","constant","high_contrast","gradient","transparent","alpha_extremes","alpha_ramp","checker"};
std::array<uint8_t,16> fixture(VkFormat f,uint32_t category,uint32_t index,uint32_t &seed) {
    std::array<uint8_t,16> b{}; const uint32_t bs=pvb::block_bytes(f);
    for(uint32_t n=0;n<bs;n+=4) write32(b.data()+n,random_word(seed));
    auto *color=b.data()+(f>=VK_FORMAT_BC2_UNORM_BLOCK?8:0);
    auto put16=[](uint8_t *p,uint32_t x) { p[0]=uint8_t(x); p[1]=uint8_t(x>>8); };
    if(category) {
        uint32_t e0=uint32_t(color[0])|(uint32_t(color[1])<<8),e1=uint32_t(color[2])|(uint32_t(color[3])<<8);
        if(category==1) e1=e0;
        if(category==2 || category==7) { e0=std::array<uint32_t,4>{0xf800,0x07e0,0x001f,0xffff}[index%4]; e1=uint32_t(~e0)&0xffff; }
        if(category==3) { e0=0xffff; e1=0; }
        if(category==4 && f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) { e0=0; e1=0xffff; }
        put16(color,e0); put16(color+2,e1);
        uint32_t indices=0;
        for(uint32_t y=0;y<4;++y) for(uint32_t x=0;x<4;++x) {
            uint32_t s=category==1?0:category==2?((x+y+index)&1):category==3?((x+index)%4):category==7?((x^y)&1):((x+y+index)%4);
            if(category==4 && f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) s=(index&1)?3:((x+y)&1?3:0);
            indices|=s<<(2*(y*4+x));
        }
        write32(color+4,indices);
        if(f>=VK_FORMAT_BC2_UNORM_BLOCK && f<=VK_FORMAT_BC2_SRGB_BLOCK) {
            for(uint32_t p=0;p<16;++p) {
                uint32_t alpha=category==4?0:category==5?((p+index)&1?15:0):category==6?p:(index&1?15:0);
                if(!(p&1)) b[p/2]=uint8_t(alpha); else b[p/2]|=uint8_t(alpha<<4);
            }
        }
        if(f>=VK_FORMAT_BC3_UNORM_BLOCK) {
            b[0]=category==4?0:255; b[1]=0; uint64_t selectors=0;
            for(uint32_t p=0;p<16;++p) {
                const uint32_t s=category==4?0:category==5?((p+index)&1):category==6?(p%8):(index&1?0:1);
                selectors|=uint64_t(s)<<(3*p);
            }
            for(uint32_t n=0;n<6;++n) b[n+2]=uint8_t(selectors>>(8*n));
        }
    }
    return b;
}
Bytes corpus(VkFormat f,uint32_t br,uint32_t bh,uint32_t layers) {
    Bytes result(size_t(br)*bh*layers*pvb::block_bytes(f)); uint32_t seed=corpus_seed+uint32_t(f)*0x431u;
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<bh;++y) for(uint32_t x=0;x<br;++x) {
        const uint32_t index=(l*bh+y)*br+x; auto b=fixture(f,index%8,index/8,seed);
        std::memcpy(result.data()+size_t(index)*pvb::block_bytes(f),b.data(),pvb::block_bytes(f));
    }
    return result;
}
const char *texture_pattern_name(VkFormat f,uint32_t pattern) {
    return f<VK_FORMAT_BC3_UNORM_BLOCK || !pattern ? "corpus" : pattern==1 ? "dense_skew" : "sparse_one_per_8x8_workgroup";
}
Bytes benchmark_source(VkFormat f,uint32_t br,uint32_t bh,uint32_t layers,uint32_t pattern) {
    if(f<VK_FORMAT_BC3_UNORM_BLOCK || !pattern) return corpus(f,br,bh,layers);
    Bytes result(size_t(br)*bh*layers*16);uint32_t seed=0x7ab39c15u;
    constexpr std::array<uint8_t,16> hard={0x13,0x9a,0x74,0xbd,0x08,0x4c,0xeb,0xe8,0x1d,0x9b,0x2b,0xac,0x99,0xc3,0x42,0x36};
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<bh;++y) for(uint32_t x=0;x<br;++x) {
        const uint32_t index=(l*bh+y)*br+x;
        const auto block=pattern==1 || (!(x%8) && !(y%8)) ? hard : fixture(f,1,index,seed);
        std::memcpy(result.data()+size_t(index)*16,block.data(),16);
    }
    return result;
}

Bytes pixels(VkFormat f,const Bytes &blocks,uint32_t width,uint32_t height,uint32_t layers,bool encoded) {
    const uint32_t br=(width+3)/4,bh=(height+3)/4,bs=encoded?etc_bytes(f):pvb::block_bytes(f);
    Bytes result(size_t(width)*height*layers*4);
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<bh;++y) for(uint32_t x=0;x<br;++x) {
        const auto *block=blocks.data()+((size_t(l)*bh+y)*br+x)*bs;
        const auto decoded=encoded?etc_reference(f,block):reference(f,block);
        for(uint32_t py=0;py<4;++py) for(uint32_t px=0;px<4;++px) if(x*4+px<width && y*4+py<height)
            std::memcpy(result.data()+((size_t(l)*height+y*4+py)*width+x*4+px)*4,decoded.data()+(py*4+px)*4,4);
    }
    return result;
}
void storage_pipeline(App &a,DiagnosticResources &r,VkDescriptorSet &set,VkDeviceSize input_size,VkDeviceSize output_size,const std::vector<uint32_t> *module=nullptr) {
    VkDescriptorSetLayoutBinding binds[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ds.bindingCount=2; ds.pBindings=binds;
    require(a.CreateDescriptorSetLayout(a.device,&ds,nullptr,&r.descriptors),"Encoder descriptors");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,24}; VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&r.descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&range;
    require(a.CreatePipelineLayout(a.device,&pl,nullptr,&r.layout),"Encoder layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(bc_etc2_encode_spv); sm.pCode=bc_etc2_encode_spv;
    if(module) { sm.codeSize=module->size()*sizeof(uint32_t); sm.pCode=module->data(); }
    require(a.CreateShaderModule(a.device,&sm,nullptr,&r.shader),"Encoder shader");
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=r.shader; cp.stage.pName="main"; cp.layout=r.layout;
    require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&cp,nullptr,&r.pipeline),"Encoder pipeline");
    a.DestroyShaderModule(a.device,std::exchange(r.shader,VK_NULL_HANDLE),nullptr);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}; VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&size;
    require(a.CreateDescriptorPool(a.device,&pool,nullptr,&r.pool),"Encoder pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; alloc.descriptorPool=r.pool; alloc.descriptorSetCount=1; alloc.pSetLayouts=&r.descriptors;
    require(a.AllocateDescriptorSets(a.device,&alloc,&set),"Encoder set");
    VkDescriptorBufferInfo info[2]={{r.input.handle,guard,input_size},{r.output.handle,guard,output_size}};
    VkWriteDescriptorSet writes[2]{};
    for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&info[n]; }
    a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
}
struct EncoderContext {
    VkFormat format;
    std::vector<uint32_t> module;
    DiagnosticResources resources;
    VkDescriptorSet set{};
    size_t input_capacity=0,output_capacity=0;
    EncoderContext(EncoderApp &a,VkFormat f,const std::vector<uint32_t> &code):format(f),module(code),resources(a) {}
};
EncoderApp::~EncoderApp() { shutdown(); }
void EncoderApp::shutdown() noexcept {
    if(!encoders.empty()) {
        stage("before per-App encoder cache destruction");
        encoders.clear();
        stage("after per-App encoder cache destruction");
    }
    if(!encoder_reported && (encoder_creations || encoder_dispatches)) {
        std::printf("ETC_ENCODER_CACHE_SUMMARY {\"pipelines_created\":%llu,\"context_reuses\":%llu,\"buffer_growths\":%llu,\"completed_dispatches\":%llu,\"contexts_live\":%zu,\"destroyed_before_device\":%s,\"deferred_Vulkan_resources\":%s}\n",(unsigned long long)encoder_creations,(unsigned long long)encoder_reuses,(unsigned long long)encoder_buffer_growths,(unsigned long long)encoder_dispatches,encoders.size(),device && !diagnostic_deferred?"true":"false",diagnostic_deferred?"true":"false");
        encoder_reported=true;
    }
    App::shutdown();
}
const std::vector<uint32_t> &production_encoder(VkFormat f) {
    static const std::vector<uint32_t> bc1(std::begin(bc_etc2_encode_bc1_spv),std::end(bc_etc2_encode_bc1_spv));
    static const std::vector<uint32_t> bc3(std::begin(bc_etc2_encode_bc3_spv),std::end(bc_etc2_encode_bc3_spv));
    static const std::vector<uint32_t> diagnostic(std::begin(bc_etc2_encode_spv),std::end(bc_etc2_encode_spv));
    if(f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK && f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) return bc1;
    if(f>=VK_FORMAT_BC3_UNORM_BLOCK && f<=VK_FORMAT_BC3_SRGB_BLOCK) return bc3;
    return diagnostic;
}
void update_encoder_buffers(EncoderApp &a,EncoderContext &context,size_t input_size,size_t output_size) {
    auto &r=context.resources;
    // The shared command buffer was reset after the preceding completed use.
    // Updating descriptors or replacing buffers while it is executable is illegal.
    if(r.recorded || a.submission_pending) throw std::runtime_error("Encoder cache reuse before completed/reset submission");
    if(input_size>context.input_capacity) {
        auto replacement=a.buffer(input_size+2*guard,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        a.destroy(r.input);r.input=replacement;context.input_capacity=input_size;++a.encoder_buffer_growths;
    }
    if(output_size>context.output_capacity) {
        auto replacement=a.buffer(output_size+2*guard,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        a.destroy(r.output);r.output=replacement;context.output_capacity=output_size;++a.encoder_buffer_growths;
    }
    if(!r.pipeline) { storage_pipeline(a,r,context.set,input_size,output_size,&context.module);++a.encoder_creations; }
    else {
        VkDescriptorBufferInfo info[2]={{r.input.handle,guard,input_size},{r.output.handle,guard,output_size}};
        VkWriteDescriptorSet writes[2]{};
        for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[n].dstSet=context.set;writes[n].dstBinding=n;writes[n].descriptorCount=1;writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[n].pBufferInfo=&info[n]; }
        a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    }
}
EncoderContext &cached_encoder(EncoderApp &a,VkFormat f,size_t input_size,size_t output_size,const std::vector<uint32_t> *explicit_module) {
    constexpr size_t max_contexts=32,max_buffer=4*1024*1024,max_total_buffers=64*1024*1024;
    const auto &module=explicit_module?*explicit_module:production_encoder(f);
    if(a.submission_pending || !a.device) throw std::runtime_error("Encoder cache requires a live device and completed submission");
    if(module.size()<5 || module.size()>65536 || module.front()!=0x07230203u || !input_size || !output_size || input_size>max_buffer || output_size>max_buffer)
        throw std::runtime_error("Encoder cache request exceeds bounded module/buffer contract");
    EncoderContext *selected=nullptr;size_t total=0;
    for(auto &context:a.encoders) {
        if(context->resources.recorded) throw std::runtime_error("Encoder cache reuse while shared command buffer is recorded");
        total+=context->input_capacity+context->output_capacity+4*guard;
        if(context->format==f && context->module==module) selected=context.get();
    }
    const auto extra=selected?std::max(input_size,selected->input_capacity)+std::max(output_size,selected->output_capacity)-selected->input_capacity-selected->output_capacity:input_size+output_size+4*guard;
    if(total+extra>max_total_buffers || (!selected && a.encoders.size()>=max_contexts)) throw std::runtime_error("Encoder cache capacity exhausted");
    const bool reused=selected!=nullptr;
    if(!selected) {
        a.encoders.push_back(std::make_unique<EncoderContext>(a,f,module));selected=a.encoders.back().get();
        std::printf("ETC_ENCODER_CACHE_CREATE {\"format\":%u,\"contexts\":%zu,\"module_bytes\":%zu,\"fnv1a64_identity\":\"%016llx\",\"max_contexts\":%zu,\"max_total_buffer_bytes\":%zu}\n",uint32_t(f),a.encoders.size(),module.size()*sizeof(uint32_t),(unsigned long long)module_identity(module),max_contexts,max_total_buffers);
    }
    try { update_encoder_buffers(a,*selected,input_size,output_size); }
    catch(...) {
        // Keep partial buffer/pipeline creation owned and destroy it before the
        // caller may retry. Recording/pending invariant rejection changes none.
        if(!selected->resources.recorded && !a.submission_pending) {
            const auto found=std::find_if(a.encoders.begin(),a.encoders.end(),[&](const auto &entry) { return entry.get()==selected; });
            a.encoders.erase(found);
        }
        throw;
    }
    if(reused) ++a.encoder_reuses;
    return *selected;
}
Bytes encode(EncoderApp &a,VkFormat f,const Bytes &bc,uint32_t width,uint32_t height,uint32_t layers,const std::vector<uint32_t> *module=nullptr) {
    const uint32_t br=(width+3)/4,bh=(height+3)/4; const size_t out_size=size_t(br)*bh*layers*etc_bytes(f);
    if(bc.size()!=size_t(br)*bh*layers*pvb::block_bytes(f)) throw std::runtime_error("Encoder source size does not match explicit shape");
    auto &context=cached_encoder(a,f,bc.size(),out_size,module);auto &r=context.resources;
    std::memset(r.input.mapped,0x5a,context.input_capacity+2*guard); std::memcpy(static_cast<uint8_t*>(r.input.mapped)+guard,bc.data(),bc.size());
    std::memset(r.output.mapped,0xa5,context.output_capacity+2*guard);
    r.begin(); const uint32_t push[6]={uint32_t(f),width,height,layers,br,bh};
    VkBufferMemoryBarrier barriers[2]{};
    for(auto &b:barriers) { b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.size=VK_WHOLE_SIZE;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT; }
    barriers[0].buffer=r.input.handle;barriers[1].buffer=r.output.handle;
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,2,barriers,0,nullptr);
    a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&context.set,0,nullptr); a.CmdPushConstants(a.command,r.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
    a.CmdDispatch(a.command,(br+7)/8,(bh+7)/8,layers); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();++a.encoder_dispatches;
    auto *raw=static_cast<uint8_t*>(r.output.mapped);auto *input=static_cast<uint8_t*>(r.input.mapped); bool guards=true,input_guards=true;
    for(uint32_t n=0;n<guard;++n) { guards&=raw[n]==0xa5 && raw[guard+out_size+n]==0xa5;input_guards&=input[n]==0x5a && input[guard+bc.size()+n]==0x5a; }
    check(guards,"Encoder output prefix/suffix guards intact");
    check(input_guards && !std::memcmp(input+guard,bc.data(),bc.size()),"Encoder preserves BC input bytes and guards");
    Bytes result(raw+guard,raw+guard+out_size); r.reset("Encoder reset"); return result;
}
struct ErrorMetric {
    uint64_t count=0,sum=0,square=0; uint32_t maximum=0; std::array<uint64_t,256> histogram{};
    void add(uint32_t e) { ++count; sum+=e; square+=uint64_t(e)*e; maximum=std::max(maximum,e); ++histogram[e]; }
    double rmse() const { return count?std::sqrt(double(square)/double(count)):0; }
    uint32_t p95() const { uint64_t n=0; for(uint32_t p=0;p<256;++p) { n+=histogram[p]; if(n*100>=count*95) return p; } return 255; }
};
void print_metric(const ErrorMetric &m,const char *name) {
    const double r=m.rmse();
    std::printf("\"%s\":{\"count\":%llu,\"mae\":%.6f,\"rmse\":%.6f,\"p95\":%u,\"max\":%u,\"psnr_db\":",name,(unsigned long long)m.count,m.count?double(m.sum)/double(m.count):0,r,m.p95(),m.maximum);
    if(r>0) std::printf("%.6f",20*std::log10(255/r)); else std::printf("null");
    std::printf("}");
}
void quality(EncoderApp &a,const Options &o,const EncoderSelection &modules={}) {
    stage("ETC2 direct GPU encoding and independent detex quality begin");
    constexpr uint32_t width=128,height=64,br=width/4,bh=height/4;
    for(uint32_t format=131;format<=138;++format) {
        const auto f=VkFormat(format); auto bc=corpus(f,br,bh,1); auto etc=encode(a,f,bc,width,height,1,modules.select(f));
        ErrorMetric rgb,visible,alpha; std::array<ErrorMetric,8> crgb{},calpha{}; std::array<uint64_t,5> modes{};
        uint64_t zero=0,opaque=0,extreme_errors=0,floor_zero=0,floor_opaque=0; bool valid=true; uint32_t worst_alpha=16;
        for(uint32_t n=0;n<br*bh;++n) {
            const auto source=reference(f,bc.data()+size_t(n)*pvb::block_bytes(f));
            const auto *packed=etc.data()+size_t(n)*etc_bytes(f); PixelBlock decoded{};
            try { decoded=etc_reference(f,packed); } catch(...) { valid=false; continue; }
            const auto mode=detexGetModeETC2(packed+(rgba(f)?8:0)); if(mode<modes.size()) ++modes[mode]; else valid=false;
            for(uint32_t p=0;p<16;++p) {
                for(uint32_t c=0;c<3;++c) { const auto e=uint32_t(std::abs(int(source[p*4+c])-int(decoded[p*4+c]))); rgb.add(e); crgb[n%8].add(e); if(source[p*4+3]) visible.add(e); }
                const uint32_t e=uint32_t(std::abs(int(source[p*4+3])-int(decoded[p*4+3]))); alpha.add(e); calpha[n%8].add(e);
                if(e>worst_alpha) {
                    worst_alpha=e;
                    std::printf("ALPHA_OUTLIER format=%u block=%u class=%s pixel=%u source=%u decoded=%u error=%u BC=",format,n,classes[n%8],p,source[p*4+3],decoded[p*4+3],e);
                    for(uint32_t k=0;k<pvb::block_bytes(f);++k) std::printf("%02x",bc[size_t(n)*pvb::block_bytes(f)+k]);
                    std::printf(" ETC="); for(uint32_t k=0;k<etc_bytes(f);++k) std::printf("%02x",packed[k]); std::printf("\n");
                }
                const auto endpoint=etc2_alpha::bc_source(f,bc.data()+size_t(n)*pvb::block_bytes(f),p).endpoint();
                if(endpoint==etc2_alpha::zero) { ++zero; extreme_errors+=decoded[p*4+3]!=0; }
                if(endpoint==etc2_alpha::opaque) { ++opaque; extreme_errors+=decoded[p*4+3]!=255; }
                if(endpoint==etc2_alpha::interpolated) { floor_zero+=source[p*4+3]==0; floor_opaque+=source[p*4+3]==255; }
            }
        }
        check(valid,"Every GPU-generated ETC2 block independently decodes");
        check(modes[2]>0,"Held-out corpus exercises ETC2 T-mode packing");
        check(!extreme_errors,"Mathematical source alpha zero/255 remains exact after EAC encoding");
        check(visible.rmse()<=o.rgb_limit,"Held-out visible RGB RMSE meets predeclared sanity bound");
        check(alpha.rmse()<=o.alpha_limit,"Held-out alpha RMSE meets explicit broad sanity bound");
        // A BC2 block can contain all sixteen 0,17,...255 alpha values.
        // EAC's eight palette entries cannot cover that set within 16 LSB
        // while keeping zero/255 exact. Exhaustive valid-palette enumeration
        // proves 17 LSB is achievable and optimal; BC3 retains the 16 bound.
        const uint32_t alpha_max_limit=f>=VK_FORMAT_BC2_UNORM_BLOCK && f<=VK_FORMAT_BC2_SRGB_BLOCK?17:16;
        check(alpha.maximum<=alpha_max_limit,"Held-out alpha maximum error meets BC2 proven17 / BC3 predeclared16 bound");
        std::printf("ETC_QUALITY {\"format\":%u,\"seed\":%u,\"blocks\":%u,\"valid\":%s,\"wrapper_lossy_eligible\":%s,\"bc2_preview_only\":%s,\"original_alpha16_gate_pass\":%s,\"alpha_max_limit\":%u,\"mode_counts\":[%llu,%llu,%llu,%llu,%llu],\"zero_alpha_count\":%llu,\"opaque_alpha_count\":%llu,\"extreme_alpha_errors\":%llu,\"cpu_floor_zero_without_source_endpoint_count\":%llu,\"cpu_floor_opaque_without_source_endpoint_count\":%llu,\"endpoint_classification\":\"original_BC_alpha_rational\",",format,corpus_seed,br*bh,valid?"true":"false",eligible(f)?"true":"false",eligible(f)?"false":"true",alpha.maximum<=16?"true":"false",alpha_max_limit,(unsigned long long)modes[0],(unsigned long long)modes[1],(unsigned long long)modes[2],(unsigned long long)modes[3],(unsigned long long)modes[4],(unsigned long long)zero,(unsigned long long)opaque,(unsigned long long)extreme_errors,(unsigned long long)floor_zero,(unsigned long long)floor_opaque);
        print_metric(rgb,"rgb"); std::printf(","); print_metric(visible,"visible_rgb"); std::printf(","); print_metric(alpha,"alpha"); std::printf("}\n");
        for(uint32_t n=0;n<8;++n) { std::printf("ETC_CLASS {\"format\":%u,\"class\":\"%s\",",format,classes[n]); print_metric(crgb[n],"rgb"); std::printf(","); print_metric(calpha[n],"alpha"); std::printf("}\n"); }
    }
}

void adversarial_quality(EncoderApp &a,const Options &o,const EncoderSelection &modules={}) {
    stage("Independent adversarial BC fixture quality begin");
    for(auto f:{VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC1_RGB_SRGB_BLOCK,
               VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
               VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC3_SRGB_BLOCK}) {
        for(const auto &c:etc2_adversarial::cases) {
            if(c.family==5 && f<VK_FORMAT_BC3_UNORM_BLOCK) continue;
            const auto bc=etc2_adversarial::blocks(f,c);
            const auto etc=encode(a,f,bc,c.width,c.height,c.layers,modules.select(f));
            const auto source=pixels(f,bc,c.width,c.height,c.layers,false);
            const auto decoded=pixels(f,etc,c.width,c.height,c.layers,true);
            const auto endpoints=etc2_alpha::source_mask(f,bc,c.width,c.height,c.layers);
            ErrorMetric rgb,alpha;uint64_t extreme_errors=0;
            for(size_t p=0;p<endpoints.size();++p) {
                for(uint32_t channel=0;channel<3;++channel) if(source[p*4+3])
                    rgb.add(uint32_t(std::abs(int(source[p*4+channel])-int(decoded[p*4+channel]))));
                alpha.add(uint32_t(std::abs(int(source[p*4+3])-int(decoded[p*4+3]))));
                if(endpoints[p]==etc2_alpha::zero) extreme_errors+=decoded[p*4+3]!=0;
                if(endpoints[p]==etc2_alpha::opaque) extreme_errors+=decoded[p*4+3]!=255;
            }
            check(rgb.rmse()<=o.rgb_limit,"Adversarial valid visible texels keep predeclared RGB RMSE32 bound");
            check(alpha.rmse()<=o.alpha_limit && alpha.maximum<=16,"Adversarial BC1/3 alpha keeps RMSE8 and maximum16 bounds");
            check(!extreme_errors,"Adversarial genuine source alpha0/255 remains exact");
            std::printf("ETC_ADVERSARIAL {\"case\":\"%s\",\"format\":%u,\"shape\":[%u,%u,%u],\"extreme_alpha_errors\":%llu,",c.name,uint32_t(f),c.width,c.height,c.layers,(unsigned long long)extreme_errors);
            print_metric(rgb,"visible_rgb");std::printf(",");print_metric(alpha,"alpha");std::printf("}\n");
        }
    }
}

void oracle_anchors() {
    const std::array<uint8_t,8> black={0,0,0,0,0,0,0,0}; const auto p=etc_reference(VK_FORMAT_BC1_RGB_UNORM_BLOCK,black.data());
    bool equal=true; for(uint32_t n=0;n<16;++n) equal&=p[n*4]==2 && p[n*4+1]==2 && p[n*4+2]==2 && p[n*4+3]==255;
    check(equal,"Independent literal ETC individual-mode anchor: RGB 2,2,2 alpha255");
    const std::array<uint8_t,8> t={4,0,0,2,0,0,0,0}; const auto q=etc_reference(VK_FORMAT_BC1_RGB_UNORM_BLOCK,t.data());
    equal=detexGetModeETC2(t.data())==2; for(uint32_t n=0;n<16;++n) equal&=q[n*4]==0 && q[n*4+1]==0 && q[n*4+2]==0 && q[n*4+3]==255;
    check(equal,"Independent literal ETC T-mode anchor: RGB 0,0,0 alpha255");
    std::array<uint8_t,16> eac{}; eac[0]=73; // Zero multiplier is legal for the decoder, unlike encoder search.
    auto r=etc_reference(VK_FORMAT_BC3_UNORM_BLOCK,eac.data()); equal=true;
    for(uint32_t n=0;n<16;++n) equal&=r[n*4+3]==73;
    check(equal,"Independent EAC constant-alpha decoder anchor");
    eac[0]=255; r=etc_reference(VK_FORMAT_BC3_UNORM_BLOCK,eac.data()); equal=true; for(uint32_t n=0;n<16;++n) equal&=r[n*4+3]==255;
    check(equal,"Independent EAC opaque-alpha decoder anchor");
}

void make_image(App &a,VkFormat f,uint32_t width,uint32_t height,uint32_t layers,uint32_t mip,bool cube,VkImageUsageFlags usage,VkImage &image,VkDeviceMemory &memory) {
    VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; i.imageType=VK_IMAGE_TYPE_2D; i.format=f; i.extent={width,height,1}; i.mipLevels=mip+1; i.arrayLayers=layers; i.samples=VK_SAMPLE_COUNT_1_BIT; i.tiling=VK_IMAGE_TILING_OPTIMAL; i.usage=usage;
    if(cube) i.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    require(a.CreateImage(a.device,&i,nullptr,&image),"ETC acceptance image"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,image,&req);
    VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; m.allocationSize=req.size; m.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0);
    require(a.AllocateMemory(a.device,&m,nullptr,&memory),"ETC acceptance memory"); require(a.BindImageMemory(a.device,image,memory,0),"ETC acceptance bind");
}
VkPipeline cube_pipeline(App &a,DiagnosticResources &r) {
    VkShaderModuleCreateInfo s{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; s.codeSize=sizeof(bc7_cube_sample_spv); s.pCode=bc7_cube_sample_spv;
    require(a.CreateShaderModule(a.device,&s,nullptr,&r.shader),"ETC cube shader"); VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; p.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; p.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; p.stage.module=r.shader; p.stage.pName="main"; p.layout=a.layout;
    require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&p,nullptr,&r.pipeline),"ETC cube pipeline"); a.DestroyShaderModule(a.device,std::exchange(r.shader,VK_NULL_HANDLE),nullptr); return r.pipeline;
}
VkDescriptorSet sample_descriptor(App &a,DiagnosticResources &r,VkFormat f,uint32_t mip,uint32_t layers,bool cube,size_t output_size) {
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image=r.image; v.viewType=cube?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D_ARRAY; v.format=f; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,layers};
    require(a.CreateImageView(a.device,&v,nullptr,&r.view),"ETC acceptance view");
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; VkDescriptorPoolCreateInfo p{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; p.maxSets=1; p.poolSizeCount=2; p.pPoolSizes=sizes;
    require(a.CreateDescriptorPool(a.device,&p,nullptr,&r.pool),"ETC acceptance descriptor pool"); VkDescriptorSetAllocateInfo d{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; d.descriptorPool=r.pool; d.descriptorSetCount=1; d.pSetLayouts=&a.descriptors; VkDescriptorSet set{};
    require(a.AllocateDescriptorSets(a.device,&d,&set),"ETC acceptance set");
    VkDescriptorImageInfo image{a.sampler,r.view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buffer{r.output.handle,0,output_size}; VkWriteDescriptorSet writes[2]{};
    for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&image; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&buffer;
    a.UpdateDescriptorSets(a.device,2,writes,0,nullptr); return set;
}
void sample_dispatch(App &a,VkDescriptorSet set,VkPipeline pipeline,uint32_t width,uint32_t height,uint32_t layers,bool is_srgb) {
    const uint32_t push[8]={width,height,layers,0,0,is_srgb?1u:0u,0,0};
    a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push);
    a.CmdDispatch(a.command,(width+7)/8,(height+7)/8,layers);
}
// raw_native uploads the encoder's ETC bytes directly to a native ETC image.
// wrapper checks upload the original BC bytes to an emulated BC image instead.
void sample_case(EncoderApp &a,const Options &o,VkFormat f,uint32_t full_width,uint32_t full_height,uint32_t layers,uint32_t mip,bool cube,bool raw_native,bool native) {
    const uint32_t width=std::max(1u,full_width>>mip),height=std::max(1u,full_height>>mip),br=(width+3)/4,bh=(height+3)/4;
    const auto bc=corpus(f,br,bh,layers); auto etc=encode(a,f,bc,width,height,layers);
    if(o.corrupt_etc && !raw_native && lossy_expected(o,f,native)) { etc[rgba(f)?8:0]^=0x70; }
    auto expected=pixels(f,lossy_expected(o,f,native) || raw_native?etc:bc,width,height,layers,lossy_expected(o,f,native) || raw_native);
    const auto endpoints=etc2_alpha::sampled_mask(f,bc,expected,width,height,layers,lossy_expected(o,f,native) || raw_native,raw_native);
    if(o.corrupt_oracle) for(size_t n=0;n<expected.size();n+=4) expected[n]=uint8_t(expected[n]^0x80);
    const auto &input_bytes=raw_native?etc:bc; DiagnosticResources r(a);
    r.input=a.buffer(input_bytes.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); r.compressed=a.buffer(bc.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memcpy(r.input.mapped,input_bytes.data(),input_bytes.size()); std::memset(r.output.mapped,0xa5,expected.size()); std::memset(r.compressed.mapped,0xa5,bc.size());
    const auto image_format=raw_native?etc_format(f):f;
    make_image(a,image_format,full_width,full_height,layers,mip,cube,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,r.image,r.memory);
    const auto set=sample_descriptor(a,r,image_format,mip,layers,cube,expected.size()); const auto pipeline=cube?cube_pipeline(a,r):a.pipeline;
    VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,layers}; region.imageExtent={width,height,1};
    r.begin(); image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Bind before intercepted upload to test caller state restoration.
    const uint32_t push[8]={width,height,layers,0,0,srgb(f)?1u:0u,0,0}; a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    if(!o.omit_dependency) image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    a.CmdDispatch(a.command,(width+7)/8,(height+7)/8,layers); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if(!raw_native) {
        image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        a.CmdCopyImageToBuffer(a.command,r.image,VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&region); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    a.submit(); char name[140]; std::snprintf(name,sizeof(name),"%s_format%u_%ux%u_layers%u_mip%u_cube%d",raw_native?"native_ETC":"wrapper_BC",uint32_t(f),width,height,layers,mip,cube);
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,endpoints,1,name),raw_native?"Native compressed sampling matches independent detex (+/-1 LSB)":"Wrapper sampled pixels match selected route independent oracle (+/-1 LSB)");
    if(!raw_native) check(!std::memcmp(r.compressed.mapped,bc.data(),bc.size()),"Original BC bytes survive lossy texture storage/readback exactly");
    r.reset("ETC sample reset");
}

void copy_partial_case(EncoderApp &a,const Options &o,VkFormat f,bool native) {
    constexpr uint32_t width=12,height=8,layers=2,br=3,bh=2;
    auto bc=corpus(f,br,bh,layers); auto updated=bc;
    // Complete image followed by a one-block two-layer partial overwrite.
    uint32_t seed=0x571028eu; Bytes patch(size_t(layers)*pvb::block_bytes(f));
    for(uint32_t l=0;l<layers;++l) { auto b=fixture(f,2,l+17,seed); std::memcpy(patch.data()+size_t(l)*pvb::block_bytes(f),b.data(),pvb::block_bytes(f)); std::memcpy(updated.data()+(size_t(l)*br*bh+4)*pvb::block_bytes(f),b.data(),pvb::block_bytes(f)); }
    auto etc=encode(a,f,updated,width,height,layers); const auto expected=pixels(f,lossy_expected(o,f,native)?etc:updated,width,height,layers,lossy_expected(o,f,native));
    const auto endpoints=etc2_alpha::sampled_mask(f,updated,expected,width,height,layers,lossy_expected(o,f,native));
    DiagnosticResources r(a); r.input=a.buffer(bc.size()+patch.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); r.compressed=a.buffer(bc.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memcpy(r.input.mapped,bc.data(),bc.size()); std::memcpy(static_cast<uint8_t*>(r.input.mapped)+bc.size(),patch.data(),patch.size());
    constexpr auto usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    make_image(a,f,width,height,layers,0,false,usage,r.image,r.memory); make_image(a,f,width,height,layers,0,false,usage,r.transfer_images[0],r.transfer_memory[0]);
    const auto set=sample_descriptor(a,r,f,0,layers,false,expected.size());
    VkBufferImageCopy full{}; full.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers}; full.imageExtent={width,height,1};
    VkBufferImageCopy partial=full; partial.bufferOffset=bc.size(); partial.imageOffset={4,4,0}; partial.imageExtent={4,4,1};
    r.begin(); image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); image_barrier(a,r.transfer_images[0],0,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&full); image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&partial);
    image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource=full.imageSubresource; copy.extent=full.imageExtent;
    a.CmdCopyImage(a.command,r.image,VK_IMAGE_LAYOUT_GENERAL,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    image_barrier(a,r.transfer_images[0],0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyImageToBuffer(a.command,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&full); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    sample_dispatch(a,set,a.pipeline,width,height,layers,srgb(f)); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();
    check(!std::memcmp(r.compressed.mapped,updated.data(),updated.size()),"BC image copy after partial overwrite preserves original bytes on all layers");
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,endpoints,1,"partial_BC_source"),"Partial compressed update preserves untouched sampled pixels"); r.reset("ETC partial reset");
    // Read back the copied destination using an actual sampled view as well.
    a.DestroyImageView(a.device,std::exchange(r.view,VK_NULL_HANDLE),nullptr); a.DestroyDescriptorPool(a.device,std::exchange(r.pool,VK_NULL_HANDLE),nullptr);
    std::swap(r.image,r.transfer_images[0]); std::swap(r.memory,r.transfer_memory[0]); const auto copied_set=sample_descriptor(a,r,f,0,layers,false,expected.size());
    r.begin(); image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); sample_dispatch(a,copied_set,a.pipeline,width,height,layers,srgb(f)); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,endpoints,1,"copied_BC_destination"),"Copied BC destination samples the same compressed route"); r.reset("ETC copy sample reset");
}

void mixed_copy_case(EncoderApp &a,const Options &o,VkFormat f,bool native) {
    constexpr uint32_t width=8,height=8,layers=2;
    auto bc=corpus(f,2,2,layers); auto replacement=bc; uint32_t seed=0x03720af1u;
    for(uint32_t n=0;n<8;++n) { auto b=fixture(f,2,n+32,seed); std::memcpy(replacement.data()+size_t(n)*pvb::block_bytes(f),b.data(),pvb::block_bytes(f)); }
    auto encoded=encode(a,f,bc,width,height,layers); auto encoded_replacement=encode(a,f,replacement,width,height,layers);
    const auto first=pixels(f,lossy_expected(o,f,native)?encoded:bc,width,height,layers,lossy_expected(o,f,native));
    const auto second=pixels(f,lossy_expected(o,f,native)?encoded_replacement:replacement,width,height,layers,lossy_expected(o,f,native));
    const auto first_endpoints=etc2_alpha::sampled_mask(f,bc,first,width,height,layers,lossy_expected(o,f,native));
    const auto second_endpoints=etc2_alpha::sampled_mask(f,replacement,second,width,height,layers,lossy_expected(o,f,native));
    auto counters=reinterpret_cast<PvbGetCompressionStats>(dlsym(a.library,"pvb_get_compression_stats")); PvbCompressionStats before{}; if(counters) counters(&before);
    DiagnosticResources r(a); r.input=a.buffer(bc.size()*2,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(first.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); r.compressed=a.buffer(bc.size()*2,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memcpy(r.input.mapped,bc.data(),bc.size()); std::memcpy(static_cast<uint8_t*>(r.input.mapped)+bc.size(),replacement.data(),replacement.size());
    constexpr auto sampled=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    constexpr auto transfer_only=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    make_image(a,f,width,height,layers,0,false,sampled,r.image,r.memory); // sampled destination C
    make_image(a,f,width,height,layers,0,false,sampled,r.transfer_images[0],r.transfer_memory[0]); // sampled source A
    make_image(a,f,width,height,layers,0,false,transfer_only,r.transfer_images[1],r.transfer_memory[1]); // RGBA transfer-only B
    if(counters) {
        PvbCompressionStats now{}; counters(&now); const bool lossy=lossy_expected(o,f,native);
        check(now.live_etc2_images-before.live_etc2_images==(lossy?2u:0u),"Mixed-copy sampled images select the expected ETC2 route");
        check(now.live_rgba_images-before.live_rgba_images==(lossy?1u:3u),"Mixed-copy transfer-only image selects RGBA backing");
    } else std::printf("ETC_COUNTERS: null (compression diagnostics unsupported by explicit baseline)\n");
    const auto set=sample_descriptor(a,r,f,0,layers,false,first.size());
    VkBufferImageCopy full{}; full.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers}; full.imageExtent={width,height,1};
    VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource=full.imageSubresource; copy.extent=full.imageExtent;
    auto read_and_sample=[&]() {
        image_barrier(a,r.transfer_images[1],0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        a.CmdCopyImageToBuffer(a.command,r.transfer_images[1],VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&full);
        image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        auto read=full; read.bufferOffset=bc.size(); a.CmdCopyImageToBuffer(a.command,r.image,VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&read); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        sample_dispatch(a,set,a.pipeline,width,height,layers,srgb(f)); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    };
    r.begin(); for(auto image:{r.image,r.transfer_images[0],r.transfer_images[1]}) image_barrier(a,image,0,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,1,&full);
    image_barrier(a,r.transfer_images[0],0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyImage(a.command,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,r.transfer_images[1],VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    image_barrier(a,r.transfer_images[1],0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyImage(a.command,r.transfer_images[1],VK_IMAGE_LAYOUT_GENERAL,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy); read_and_sample(); a.submit();
    const auto *read=static_cast<const uint8_t*>(r.compressed.mapped);
    check(!std::memcmp(read,bc.data(),bc.size()) && !std::memcmp(read+bc.size(),bc.data(),bc.size()),"ETC2 -> RGBA -> ETC2 copies preserve original BC bytes on both destinations");
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),first,first_endpoints,1,"mixed_ETC_RGBA_ETC"),"Mixed backing copies regenerate sampled destination from original BC bytes"); r.reset("Mixed copy first reset");
    r.begin(); image_barrier(a,r.transfer_images[1],0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    auto upload=full; upload.bufferOffset=bc.size(); a.CmdCopyBufferToImage(a.command,r.input.handle,r.transfer_images[1],VK_IMAGE_LAYOUT_GENERAL,1,&upload);
    image_barrier(a,r.transfer_images[1],0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyImage(a.command,r.transfer_images[1],VK_IMAGE_LAYOUT_GENERAL,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy); read_and_sample(); a.submit();
    check(!std::memcmp(read,replacement.data(),replacement.size()) && !std::memcmp(read+bc.size(),replacement.data(),replacement.size()),"RGBA source -> ETC2 destination uses newly uploaded canonical BC bytes");
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),second,second_endpoints,1,"mixed_RGBA_new_ETC"),"RGBA-to-ETC2 copy does not inherit lossless source texture pixels"); r.reset("Mixed copy final reset");
    if(counters && lossy_expected(o,f,native)) { PvbCompressionStats now{}; counters(&now); check(now.cross_backing_regenerations-before.cross_backing_regenerations>=3,"Mixed-copy diagnostics prove all three cross-backing regenerations"); }
}

void srgb_calibration(App &a) {
    DiagnosticResources r(a); constexpr uint32_t width=8; Bytes expected(width*4);
    constexpr uint8_t values[8]={0,1,10,11,64,127,200,255};
    for(uint32_t n=0;n<width;++n) { for(uint32_t c=0;c<3;++c) expected[n*4+c]=values[(n+c)%8]; expected[n*4+3]=values[n]; }
    r.input=a.buffer(expected.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); std::memcpy(r.input.mapped,expected.data(),expected.size());
    make_image(a,VK_FORMAT_R8G8B8A8_SRGB,width,1,1,0,false,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,r.image,r.memory);
    const auto set=sample_descriptor(a,r,VK_FORMAT_R8G8B8A8_SRGB,0,1,false,expected.size()); VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,1,1};
    r.begin(); image_barrier(a,r.image,0,1,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    image_barrier(a,r.image,0,1,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); sample_dispatch(a,set,a.pipeline,width,1,1,true); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,etc2_alpha::decoded_mask(expected),1,"literal_sRGB_calibration"),"Literal sRGB encode/decode transfer and sampling calibration (+/-1 LSB)"); r.reset("sRGB calibration reset");
}

uint32_t native_literals(App &a,const Options &o) {
    stage("Raw native ETC/EAC literal sampling and compressed-byte readback begin"); uint32_t skips=0;
    std::array<Bytes,3> payloads={Bytes(8,0),Bytes{4,0,0,2,0,0,0,0},Bytes(16,0)};
    payloads[2][0]=73; // Decoder-legal multiplier zero: literal constant alpha73.
    const std::array<VkFormat,3> formats={VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK};
    const char *names[3]={"ETC_individual_RGB2_alpha255","ETC_T_RGB0_alpha255","EAC_RGB2_alpha73"};
    for(uint32_t n=0;n<3;++n) {
        const auto f=formats[n]; if(!native_support(a,f) || o.baseline) { ++skips; std::printf("SKIP: raw native literal %s requires native ETC2 and the candidate enabled feature\n",names[n]); if(o.require_native) check(false,"Required raw native ETC/EAC literal sampling unavailable"); continue; }
        const auto decoded=etc_reference(f,payloads[n].data()); Bytes expected(decoded.begin(),decoded.end()); DiagnosticResources r(a);
        r.input=a.buffer(payloads[n].size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.compressed=a.buffer(payloads[n].size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); std::memcpy(r.input.mapped,payloads[n].data(),payloads[n].size()); std::memset(r.compressed.mapped,0xa5,payloads[n].size()); std::memset(r.output.mapped,0xa5,expected.size());
        make_image(a,etc_format(f),4,4,1,0,false,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,r.image,r.memory); const auto set=sample_descriptor(a,r,etc_format(f),0,1,false,expected.size());
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={4,4,1}; r.begin(); image_barrier(a,r.image,0,1,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        if(!o.omit_dependency) image_barrier(a,r.image,0,1,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT);
        sample_dispatch(a,set,a.pipeline,4,4,1,false); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.CmdCopyImageToBuffer(a.command,r.image,VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&copy); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.submit();
        const bool exact=!std::memcmp(r.compressed.mapped,payloads[n].data(),payloads[n].size());
        std::printf("ETC_NATIVE_LITERAL {\"name\":\"%s\",\"native_format\":%u,\"first_actual_rgba\":[%u,%u,%u,%u],\"first_expected_rgba\":[%u,%u,%u,%u],\"compressed_bytes_exact\":%s,\"payload_hex\":\"",names[n],uint32_t(etc_format(f)),static_cast<uint8_t*>(r.output.mapped)[0],static_cast<uint8_t*>(r.output.mapped)[1],static_cast<uint8_t*>(r.output.mapped)[2],static_cast<uint8_t*>(r.output.mapped)[3],expected[0],expected[1],expected[2],expected[3],exact?"true":"false"); for(auto byte:payloads[n]) std::printf("%02x",byte); std::printf("\"}\n");
        check(exact,"Raw native ETC/EAC copy returns every literal compressed byte exactly"); check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,etc2_alpha::decoded_mask(expected),0,names[n]),"Raw native ETC/EAC literals sample exact independent RGBA anchors"); r.reset("Raw native literal reset");
    }
    return skips;
}

void lossless_bc7(App &a) {
    constexpr uint32_t width=32,height=32,blocks=64; Bytes bc(blocks*16),expected(width*height*4); uint32_t seed=0x047c2319;
    for(uint32_t n=0;n<blocks;++n) {
        auto *block=bc.data()+n*16; for(uint32_t k=0;k<16;k+=4) write32(block+k,random_word(seed)); const uint32_t mode=n%8;
        block[0]=uint8_t((block[0]&~((1u<<(mode+1))-1u))|(1u<<mode)); alignas(8) std::array<uint8_t,16> aligned{}; std::memcpy(aligned.data(),block,16); PixelBlock decoded{}; bcdec_bc7(aligned.data(),decoded.data(),16);
        for(uint32_t y=0;y<4;++y) for(uint32_t x=0;x<4;++x) std::memcpy(expected.data()+(((n/8)*4+y)*width+(n%8)*4+x)*4,decoded.data()+(y*4+x)*4,4);
    }
    DiagnosticResources r(a); r.input=a.buffer(bc.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); r.compressed=a.buffer(bc.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT); std::memcpy(r.input.mapped,bc.data(),bc.size());
    make_image(a,VK_FORMAT_BC7_UNORM_BLOCK,width,height,1,0,false,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,r.image,r.memory); const auto set=sample_descriptor(a,r,VK_FORMAT_BC7_UNORM_BLOCK,0,1,false,expected.size());
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1}; r.begin(); image_barrier(a,r.image,0,1,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy); image_barrier(a,r.image,0,1,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT);
    sample_dispatch(a,set,a.pipeline,width,height,1,false); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.CmdCopyImageToBuffer(a.command,r.image,VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&copy); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.submit();
    check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,etc2_alpha::decoded_mask(expected),1,"lossless_BC7_all_modes"),"ETC2 policy keeps every BC7 mode lossless against independent bcdec"); check(!std::memcmp(r.compressed.mapped,bc.data(),bc.size()),"ETC2 policy preserves original BC7 bytes exactly"); r.reset("Lossless BC7 reset");
}

struct LinearSampler {
    App &a; VkSampler old{},owned{};
    explicit LinearSampler(App &app):a(app),old(app.sampler) {
        VkSamplerCreateInfo i{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; i.magFilter=i.minFilter=VK_FILTER_LINEAR; i.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; i.addressModeU=i.addressModeV=i.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; i.maxLod=32;
        require(a.CreateSampler(a.device,&i,nullptr,&owned),"ETC linear sampler"); a.sampler=owned;
    }
    ~LinearSampler() {
        a.sampler=old;
        if(a.submission_pending) { const auto idle=a.DeviceWaitIdle(a.device); if(idle!=VK_SUCCESS && idle!=VK_ERROR_DEVICE_LOST) { check(false,"Linear sampler retained because GPU completion is unknown"); return; } a.submission_pending=false; }
        if(owned) a.DestroySampler(a.device,owned,nullptr);
    }
};
double linear_channel(uint8_t b,bool is_srgb) {
    const double c=double(b)/255; return !is_srgb?c:c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4);
}
Bytes bilinear_reference(const Bytes &decoded,uint32_t width,uint32_t height,uint32_t layers,bool is_srgb) {
    Bytes result(decoded.size());
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) for(uint32_t c=0;c<4;++c) {
        double value=0;
        for(uint32_t dy=0;dy<2;++dy) for(uint32_t dx=0;dx<2;++dx) value+=linear_channel(decoded[((size_t(l)*height+std::min(y+dy,height-1))*width+std::min(x+dx,width-1))*4+c],is_srgb && c<3)*0.25;
        if(is_srgb && c<3) value=value<=0.0031308?value*12.92:1.055*std::pow(value,1.0/2.4)-0.055;
        result[((size_t(l)*height+y)*width+x)*4+c]=uint8_t(std::lround(std::clamp(value,0.0,1.0)*255));
    }
    return result;
}
void alpha_fixtures() {
    // CPU-only regression controls; none loads a wrapper or initializes Vulkan.
    const auto f=VK_FORMAT_BC3_UNORM_BLOCK;
    const std::array<uint8_t,16> reported={0x03,0x00,0x8e,0x55,0x44,0x72,0x4b,0x24,0xe5,0x3f,0x79,0x0a,0xc7,0xad,0x34,0x9d};
    const auto fraction=etc2_alpha::bc_source(f,reported.data(),0);
    const auto decoded=reference(f,reported.data());
    check(fraction.numerator==6 && fraction.denominator==7 && fraction.endpoint()==etc2_alpha::interpolated && decoded[3]==0,"Reported BC3 alpha is positive 6/7 despite CPU floor zero");
    const Bytes expected(decoded.begin(),decoded.begin()+4); auto rounded=expected; rounded[3]=1;
    check(near_bytes(rounded.data(),expected,{fraction.endpoint()},1,"fixture_BC3_6_over_7_rounds_to_one"),"BC3 6/7 floor/round disagreement permits the existing one-LSB tolerance");
    auto too_far=expected; too_far[3]=2;
    check(!near_bytes(too_far.data(),expected,{fraction.endpoint()},1,"fixture_BC3_6_over_7_two_LSB_rejected"),"Non-endpoint alpha still rejects differences beyond one LSB");

    auto bc3=[](uint8_t a0,uint8_t a1,uint32_t selector) { Bytes b(16); b[0]=a0; b[1]=a1; b[2]=uint8_t(selector); return b; };
    auto endpoint_control=[&](const Bytes &bc,uint8_t wrong,const char *name) {
        const auto source=reference(f,bc.data()); Bytes pixel(source.begin(),source.begin()+4); auto actual=pixel; actual[3]=wrong;
        const auto mask=etc2_alpha::source_mask(f,bc,1,1,1);
        check(!near_bytes(actual.data(),pixel,mask,1,name),"Genuine mathematical BC3 zero/255 rejects a one-LSB endpoint mutation");
    };
    endpoint_control(bc3(0,3,0),1,"fixture_literal_endpoint_zero_to_one_rejected");
    endpoint_control(bc3(3,255,1),254,"fixture_literal_endpoint_255_to_254_rejected");
    endpoint_control(bc3(3,4,6),1,"fixture_special_selector_zero_to_one_rejected");
    endpoint_control(bc3(3,4,7),254,"fixture_special_selector_255_to_254_rejected");
    endpoint_control(bc3(0,0,2),1,"fixture_exact_interpolated_zero_to_one_rejected");
    endpoint_control(bc3(255,255,2),254,"fixture_exact_interpolated_255_to_254_rejected");
    const auto near_opaque=bc3(254,255,5); const auto opaque_fraction=etc2_alpha::bc_source(f,near_opaque.data(),0);
    const auto opaque_source=reference(f,near_opaque.data()); Bytes opaque_pixel(opaque_source.begin(),opaque_source.begin()+4); auto opaque_round=opaque_pixel; opaque_round[3]=255;
    check(opaque_fraction.numerator==1274 && opaque_fraction.denominator==5 && opaque_fraction.endpoint()==etc2_alpha::interpolated && opaque_pixel[3]==254,"BC3 five-step interpolant 254.8 is not a mathematical opaque endpoint");
    check(near_bytes(opaque_round.data(),opaque_pixel,{opaque_fraction.endpoint()},1,"fixture_BC3_254_point_8_rounds_to_255"),"BC3 five-step floor/round disagreement retains one-LSB tolerance");
    bool palettes=true;
    const uint32_t seven[8]={21,0,18,15,12,9,6,3},five[8]={0,15,3,6,9,12,0,1275};
    for(uint32_t selector=0;selector<8;++selector) {
        const auto high=bc3(3,0,selector),low=bc3(0,3,selector);
        const auto a=etc2_alpha::bc_source(f,high.data(),0),b=etc2_alpha::bc_source(f,low.data(),0);
        palettes&=a.numerator*7==seven[selector]*a.denominator && b.numerator*5==five[selector]*b.denominator;
    }
    check(palettes,"All BC3 selectors reconstruct both exact rational alpha palettes");
    Bytes bc1(8); bc1[2]=bc1[3]=255; bc1[4]=3;
    check(etc2_alpha::bc_source(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,bc1.data(),0).endpoint()==etc2_alpha::zero && etc2_alpha::bc_source(VK_FORMAT_BC1_RGB_UNORM_BLOCK,bc1.data(),0).endpoint()==etc2_alpha::opaque,"BC1 RGBA transparent selector and RGB opaque contract remain distinct");
    Bytes bc2(16); bc2[0]=0xf0; bc2[1]=1;
    check(etc2_alpha::bc_source(VK_FORMAT_BC2_UNORM_BLOCK,bc2.data(),0).endpoint()==etc2_alpha::zero && etc2_alpha::bc_source(VK_FORMAT_BC2_UNORM_BLOCK,bc2.data(),1).endpoint()==etc2_alpha::opaque && etc2_alpha::bc_source(VK_FORMAT_BC2_UNORM_BLOCK,bc2.data(),2).numerator==17,"BC2 explicit alpha nibble endpoints remain exact");

    const auto transparent=bc3(0,3,0); Bytes wrong_codec={2,2,2,1};
    const auto wrapper_mask=etc2_alpha::sampled_mask(f,transparent,wrong_codec,1,1,1,true);
    check(!near_bytes(wrong_codec.data(),wrong_codec,wrapper_mask,1,"fixture_wrapper_wrong_codec_endpoint_rejected"),"Matching sampled and encoded-oracle bytes cannot hide an original BC zero endpoint failure");
    for(uint8_t alpha:{uint8_t(0),uint8_t(255)}) {
        std::array<uint8_t,16> literal{}; literal[0]=alpha;
        const auto native=etc_reference(f,literal.data()); Bytes pixel(native.begin(),native.begin()+4); auto actual=pixel; actual[3]=alpha==0?1:254;
        check(!near_bytes(actual.data(),pixel,etc2_alpha::decoded_mask(pixel),1,alpha==0?"fixture_native_EAC_zero_to_one_rejected":"fixture_native_EAC_255_to_254_rejected"),"Raw detex EAC integer endpoints retain exact checks");
    }
    auto uniform_bc3=[&](uint8_t alpha,uint32_t selector) {
        auto block=bc3(alpha,alpha==3?0:alpha,0); uint64_t indices=0;
        for(uint32_t p=0;p<16;++p) indices|=uint64_t(selector)<<(3*p);
        for(uint32_t n=0;n<6;++n) block[n+2]=uint8_t(indices>>(8*n));
        return block;
    };
    const auto positive_block=uniform_bc3(3,6);
    Bytes filtered_floor=pixels(f,positive_block,2,2,1,false); auto filtered_round=filtered_floor; filtered_round[3]=1;
    const auto floor_mask=etc2_alpha::bilinear_mask(etc2_alpha::source_mask(f,positive_block,2,2,1),2,2,1);
    check(near_bytes(filtered_round.data(),bilinear_reference(filtered_floor,2,2,1,false),floor_mask,1,"fixture_filtered_positive_fraction_floor_zero"),"Filtered rounded zero has no endpoint constraint when contributing source fractions are positive");
    const auto zero_block=uniform_bc3(0,2); filtered_floor=pixels(f,zero_block,2,2,1,false); filtered_round=filtered_floor; filtered_round[3]=1;
    check(!near_bytes(filtered_round.data(),filtered_floor,etc2_alpha::bilinear_mask(etc2_alpha::source_mask(f,zero_block,2,2,1),2,2,1),2,"fixture_filtered_true_zero_to_one_rejected"),"Filtering genuine zero texels still rejects one-LSB mutation under the existing two-LSB filter tolerance");
    const auto opaque_block=uniform_bc3(255,2); filtered_floor=pixels(f,opaque_block,2,2,1,false);
    filtered_round=filtered_floor; filtered_round[3]=254;
    check(!near_bytes(filtered_round.data(),filtered_floor,etc2_alpha::bilinear_mask(etc2_alpha::source_mask(f,opaque_block,2,2,1),2,2,1),2,"fixture_filtered_true_255_to_254_rejected"),"Filtering genuine opaque texels still rejects one-LSB mutation under filter tolerance");
    check(etc2_alpha::bilinear_mask({1,1,1,0},2,2,1)[0]==etc2_alpha::interpolated,"One non-endpoint neighbor removes the exact filtered endpoint constraint");

    constexpr uint32_t width=512,height=512,layers=2,layer=1,x=416,y=288,block_index=25704;
    constexpr size_t pixel=(size_t(layer)*height+y)*width+x,byte=pixel*4+3;
    const auto saved_seed=corpus_seed; corpus_seed=2658517889u; const auto bc=corpus(f,width/4,height/4,layers); corpus_seed=saved_seed;
    const auto mask=etc2_alpha::source_mask(f,bc,width,height,layers);
    check(pixel==410016 && byte==1640067 && (layer*(height/4)+y/4)*(width/4)+x/4==block_index && (y%4)*4+x%4==0,"Reported 512-pixel byte maps to the original layer, texel, block, and selector");
    check(!std::memcmp(bc.data()+size_t(block_index)*16,reported.data(),16) && mask[pixel]==etc2_alpha::interpolated,"Exact seeded 512 BC corpus reconstructs the reported positive-alpha block and non-endpoint mask");
    std::printf("ETC_ALPHA_FIXTURE_RECONSTRUCTION {\"seed\":2658517889,\"format\":137,\"width\":512,\"height\":512,\"layers\":2,\"byte\":%zu,\"pixel\":%zu,\"layer\":1,\"x\":416,\"y\":288,\"block\":25704,\"block_pixel\":0,\"BC\":\"03008e5544724b24e53f790ac7ad349d\",\"selector\":6,\"numerator\":6,\"denominator\":7,\"cpu_floor_alpha\":0,\"rounded_alpha\":1,\"mathematical_zero\":false,\"mathematical_opaque\":false,\"one_LSB_tolerance\":1}\n",byte,pixel);
}
void linear_case(EncoderApp &a,const Options &o,VkFormat f,bool raw_native,bool native) {
    constexpr uint32_t width=8,height=8,layers=2; auto bc=corpus(f,2,2,layers); uint32_t seed=0x05415d33u;
    // Ensure an opaque constant block next to varying colors, plus the corpus's
    // transparent block. Filtered zero/255 alpha results must still be exact.
    const auto opaque=fixture(f,1,1,seed); std::memcpy(bc.data()+pvb::block_bytes(f),opaque.data(),pvb::block_bytes(f));
    auto etc=encode(a,f,bc,width,height,layers); const bool encoded=raw_native || lossy_expected(o,f,native); const auto decoded=pixels(f,encoded?etc:bc,width,height,layers,encoded);
    const auto expected=bilinear_reference(decoded,width,height,layers,srgb(f)); const auto &input=raw_native?etc:bc;
    const auto endpoints=etc2_alpha::bilinear_mask(etc2_alpha::sampled_mask(f,bc,decoded,width,height,layers,encoded,raw_native),width,height,layers);
    LinearSampler sampler(a); DiagnosticResources r(a); r.input=a.buffer(input.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); std::memcpy(r.input.mapped,input.data(),input.size());
    const auto image_format=raw_native?etc_format(f):f; make_image(a,image_format,width,height,layers,0,false,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,r.image,r.memory); const auto set=sample_descriptor(a,r,image_format,0,layers,false,expected.size());
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize=sizeof(etc2_linear_sample_spv); shader.pCode=etc2_linear_sample_spv; require(a.CreateShaderModule(a.device,&shader,nullptr,&r.shader),"ETC linear shader");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module=r.shader; pipeline.stage.pName="main"; pipeline.layout=a.layout; require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.pipeline),"ETC linear pipeline"); a.DestroyShaderModule(a.device,std::exchange(r.shader,VK_NULL_HANDLE),nullptr);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers}; copy.imageExtent={width,height,1}; r.begin(); image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); sample_dispatch(a,set,r.pipeline,width,height,layers,srgb(f)); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();
    char name[80]; std::snprintf(name,sizeof(name),"bilinear_%s_format%u",raw_native?"native_ETC":"wrapper_BC",uint32_t(f)); check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,endpoints,2,name),"Actual ETC2/RGBA linear filtering matches independent linear-space interpolation (+/-2 RGB LSB, exact alpha endpoints)"); r.reset("ETC linear reset");
}
uint32_t linear_checks(EncoderApp &a,const Options &o) {
    stage("Actual ETC2 and wrapper bilinear sampling checks begin"); uint32_t skips=0;
    for(auto f:{VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC1_RGB_SRGB_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC3_SRGB_BLOCK}) {
        const bool native=native_support(a,f); if(!native) { ++skips; std::printf("SKIP: native ETC2 bilinear sampling unsupported for format%u\n",uint32_t(f)); if(o.require_native) check(false,"Required native ETC2 bilinear sampling unavailable"); }
        linear_case(a,o,f,false,native); if(native && !o.baseline) linear_case(a,o,f,true,true);
    }
    return skips;
}

struct QueryOwner {
    App &a; VkQueryPool pool{}; uint32_t bits=0; double period=0;
    PFN_vkCmdResetQueryPool reset{}; PFN_vkCmdWriteTimestamp timestamp{}; PFN_vkGetQueryPoolResults results{}; PFN_vkDestroyQueryPool destroy{};
    explicit QueryOwner(App &app):a(app) {
        uint32_t count=0; a.GetPhysicalDeviceQueueFamilyProperties(a.physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); a.GetPhysicalDeviceQueueFamilyProperties(a.physical,&count,families.data()); bits=families[a.family].timestampValidBits;
        VkPhysicalDeviceProperties p{}; a.GetPhysicalDeviceProperties(a.physical,&p); period=p.limits.timestampPeriod;
        reset=reinterpret_cast<PFN_vkCmdResetQueryPool>(a.gdpa(a.device,"vkCmdResetQueryPool")); timestamp=reinterpret_cast<PFN_vkCmdWriteTimestamp>(a.gdpa(a.device,"vkCmdWriteTimestamp")); results=reinterpret_cast<PFN_vkGetQueryPoolResults>(a.gdpa(a.device,"vkGetQueryPoolResults")); destroy=reinterpret_cast<PFN_vkDestroyQueryPool>(a.gdpa(a.device,"vkDestroyQueryPool"));
        auto create=reinterpret_cast<PFN_vkCreateQueryPool>(a.gdpa(a.device,"vkCreateQueryPool"));
        if(bits && period>0 && reset && timestamp && results && destroy && create) { VkQueryPoolCreateInfo i{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; i.queryType=VK_QUERY_TYPE_TIMESTAMP; i.queryCount=2; require(create(a.device,&i,nullptr,&pool),"ETC benchmark timestamps"); }
    }
    ~QueryOwner() {
        if(pool) {
            if(a.submission_pending) { const auto idle=a.DeviceWaitIdle(a.device); if(idle!=VK_SUCCESS && idle!=VK_ERROR_DEVICE_LOST) { check(false,"Benchmark query resources retained because GPU completion is unknown"); return; } a.submission_pending=false; }
            destroy(a.device,pool,nullptr);
        }
    }
    void begin() { if(pool) { reset(a.command,pool,0,2); timestamp(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,0); } }
    void end() { if(pool) timestamp(a.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,1); }
    double ms() { uint64_t ticks[2]{}; if(!pool) return -1; require(results(a.device,pool,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"ETC benchmark timestamp results"); const uint64_t mask=bits==64?UINT64_MAX:(uint64_t(1)<<bits)-1; return double((ticks[1]-ticks[0])&mask)*period/1e6; }
};
double elapsed(Clock::time_point x,Clock::time_point y) { return std::chrono::duration<double,std::milli>(y-x).count(); }
void benchmark(EncoderApp &a,const Options &o) {
    stage("ETC2 warmed conversion and steady sampling benchmark begin");
    for(auto f:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK}) {
        const bool native=native_support(a,f); constexpr uint32_t layers=2; uint32_t levels=1; for(uint32_t n=o.size;n>1;n>>=1) ++levels;
        if(o.require_native) check(native,"Required native ETC2 benchmark route is available");
        Bytes packed; std::vector<VkBufferImageCopy> regions; Bytes top;
        for(uint32_t mip=0;mip<levels;++mip) {
            const uint32_t side=std::max(1u,o.size>>mip); auto bc=benchmark_source(f,(side+3)/4,(side+3)/4,layers,o.texture_pattern); VkBufferImageCopy r{}; r.bufferOffset=packed.size(); r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,layers}; r.imageExtent={side,side,1}; regions.push_back(r); packed.insert(packed.end(),bc.begin(),bc.end()); if(!mip) top=std::move(bc);
        }
        auto encoded=encode(a,f,top,o.size,o.size,layers,o.reference_selection().select(f));
        const auto original=pixels(f,top,o.size,o.size,layers,false),recompressed=pixels(f,encoded,o.size,o.size,layers,true);
        const auto genuine=etc2_alpha::source_mask(f,top,o.size,o.size,layers);
        ErrorMetric rgb_error,alpha_error;uint64_t endpoint_errors=0;
        for(size_t p=0;p<genuine.size();++p) {
            for(uint32_t c=0;c<3;++c) if(original[p*4+3]) rgb_error.add(uint32_t(std::abs(int(original[p*4+c])-int(recompressed[p*4+c]))));
            alpha_error.add(uint32_t(std::abs(int(original[p*4+3])-int(recompressed[p*4+3]))));
            if(genuine[p]==etc2_alpha::zero) endpoint_errors+=recompressed[p*4+3]!=0;
            if(genuine[p]==etc2_alpha::opaque) endpoint_errors+=recompressed[p*4+3]!=255;
        }
        check(rgb_error.rmse()<=o.rgb_limit && alpha_error.rmse()<=o.alpha_limit && alpha_error.maximum<=16 && !endpoint_errors,"Explicit benchmark reference independently passes original BC/detex quality and genuine endpoint gates outside timing");
        std::printf("ETC_BENCH_REFERENCE_QUALITY {\"format\":%u,\"extreme_alpha_errors\":%llu,",uint32_t(f),(unsigned long long)endpoint_errors);
        print_metric(rgb_error,"visible_rgb");std::printf(",");print_metric(alpha_error,"alpha");std::printf("}\n");
        const auto expected=pixels(f,lossy_expected(o,f,native)?encoded:top,o.size,o.size,layers,lossy_expected(o,f,native));
        const auto endpoints=etc2_alpha::sampled_mask(f,top,expected,o.size,o.size,layers,lossy_expected(o,f,native));
        DiagnosticResources r(a); r.input=a.buffer(packed.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); std::memcpy(r.input.mapped,packed.data(),packed.size());
        make_image(a,f,o.size,o.size,layers,levels-1,false,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,r.image,r.memory); const auto set=sample_descriptor(a,r,f,0,layers,false,expected.size()); QueryOwner query(a);
        auto counters=reinterpret_cast<PvbGetCompressionStats>(dlsym(a.library,"pvb_get_compression_stats"));
        std::printf("ETC_BENCH_TEXTURE_PATTERN {\"format\":%u,\"pattern\":\"%s\",\"sparse_rule\":\"one difficult invocation per8x8 block workgroup; clipped mips have higher density\"}\n",uint32_t(f),texture_pattern_name(f,o.texture_pattern));
        std::printf("ETC_BENCH_CONFIG {\"format\":%u,\"size\":%u,\"layers\":%u,\"mip_regions\":%u,\"source_bytes\":%zu,\"sampling_dispatches\":%u,\"warmups\":%u,\"repeats\":%u,\"native_etc2_supported\":%s,\"baseline\":%s,\"gpu_timestamps_supported\":%s,\"timestamp_valid_bits\":%u,\"timestamp_period_ns\":%.9g,\"compression_counters_supported\":%s}\n",uint32_t(f),o.size,layers,levels,packed.size(),o.samples,o.warmup,o.repeats,native?"true":"false",o.baseline?"true":"false",query.pool?"true":"false",query.bits,query.period,counters?"true":"false");
        r.begin(); for(uint32_t mip=0;mip<levels;++mip) image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.submit(); r.reset("ETC benchmark initial reset");
        for(uint32_t i=0;i<o.warmup+o.repeats;++i) {
            PvbCompressionStats before{},after{}; if(counters) counters(&before);
            const auto start=Clock::now(); r.begin(); query.begin();
            for(uint32_t mip=0;mip<levels;++mip) image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,uint32_t(regions.size()),regions.data()); query.end(); require(a.EndCommandBuffer(a.command),"ETC conversion benchmark end"); const auto recorded=Clock::now(); if(counters) counters(&after);
            a.submit_ended(); const auto completed=Clock::now(); const auto gpu=query.ms();
            std::printf("ETC_BENCH_CONVERSION {\"format\":%u,\"iteration\":%u,\"warmup\":%s,\"cpu_record_ms\":%.6f,\"submit_wait_ms\":%.6f,\"gpu_upload_conversion_transfer_ms\":",uint32_t(f),i,i<o.warmup?"true":"false",elapsed(start,recorded),elapsed(recorded,completed)); if(query.pool) std::printf("%.6f",gpu); else std::printf("null");
            if(counters) std::printf(",\"etc2_dispatches\":%llu,\"rgba_dispatches\":%llu,\"live_texture_etc2_bytes\":%llu,\"live_texture_rgba_bytes\":%llu",(unsigned long long)(after.etc2_upload_dispatches-before.etc2_upload_dispatches),(unsigned long long)(after.rgba_upload_dispatches-before.rgba_upload_dispatches),(unsigned long long)after.live_etc2_logical_bytes,(unsigned long long)after.live_rgba_logical_bytes); else std::printf(",\"compression_counters\":null"); std::printf("}\n"); r.reset("ETC conversion benchmark reset");
        }
        // The upload window is finished. No upload commands are recorded in the
        // steady sampling loop; all decoding/comparison remains outside timing.
        r.begin(); image_barrier(a,r.image,0,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); sample_dispatch(a,set,a.pipeline,o.size,o.size,layers,false); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();
        check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,endpoints,1,"bench_conversion_oracle"),"Conversion benchmark independent sampled correctness outside timing"); r.reset("ETC benchmark correctness reset");
        for(uint32_t i=0;i<o.warmup+o.repeats;++i) {
            PvbCompressionStats before{},after{}; if(counters) counters(&before);
            const auto start=Clock::now(); r.begin(); query.begin();
            for(uint32_t n=0;n<o.samples;++n) {
                sample_dispatch(a,set,a.pipeline,o.size,o.size,layers,false);
                if(n+1<o.samples) { VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; b.buffer=r.output.handle; b.size=VK_WHOLE_SIZE; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.srcAccessMask=b.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr); }
            }
            query.end(); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); require(a.EndCommandBuffer(a.command),"ETC sampling benchmark end"); const auto recorded=Clock::now(); a.submit_ended(); const auto completed=Clock::now(); const auto gpu=query.ms(); if(counters) counters(&after);
            if(counters) check(before.etc2_upload_dispatches==after.etc2_upload_dispatches && before.rgba_upload_dispatches==after.rgba_upload_dispatches,"Steady sampling records no texture upload/conversion dispatch");
            std::printf("ETC_BENCH_SAMPLING {\"format\":%u,\"iteration\":%u,\"warmup\":%s,\"dispatches\":%u,\"texels\":%llu,\"cpu_record_ms\":%.6f,\"submit_wait_ms\":%.6f,\"gpu_sampling_dispatches_with_barriers_ms\":",uint32_t(f),i,i<o.warmup?"true":"false",o.samples,(unsigned long long)(uint64_t(o.size)*o.size*layers*o.samples),elapsed(start,recorded),elapsed(recorded,completed)); if(query.pool) std::printf("%.6f",gpu); else std::printf("null"); std::printf("}\n"); r.reset("ETC sampling benchmark reset");
        }
        check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,endpoints,1,"bench_final_sample_oracle"),"Steady sampling benchmark independent correctness outside timing");
    }
}
}

#ifndef PVB_ETC2_NO_MAIN
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc<2) { std::fprintf(stderr,"Usage: pixel_etc2_probe --alpha-fixtures-only\n   or: pixel_etc2_probe explicit-library.so [--baseline] [--validation] [--require-native] [--quality-only|--adversarial-only|--native-literals-only|--linear-only|--bench-only|--bench] [--reference-encoder-spv path | --reference-bc1-spv path --reference-bc3-spv path (bench-only)] [--texture-pattern corpus|dense-skew|sparse-skew (bench-only)] [--seed uint32] [--size 16..512] [--warmup 0..30] [--repeats 1..30] [--samples 1..128] [--rgb-rmse-limit LSB] [--alpha-rmse-limit LSB] [--corrupt-etc|--corrupt-oracle|--omit-dependency]\n"); return 2; }
    try {
        if(std::string(argv[1])=="--alpha-fixtures-only") {
            if(argc!=2) throw std::runtime_error("--alpha-fixtures-only accepts no library or additional options");
            alpha_fixtures();
            std::printf("ETC_ALPHA_FIXTURE_RESULT {\"passed\":%u,\"failed\":%u,\"GPU_initialized\":false,\"expected_negative_comparisons\":12,\"rgb_rmse_limit\":32,\"alpha_rmse_limit\":8,\"bc3_alpha_max_error_limit\":16}\n",passed,failed);
            return failed?1:0;
        }
        Options o;
        for(int n=2;n<argc;++n) {
            const auto option=std::string(argv[n]);
            if(option=="--baseline") o.baseline=true;
            else if(option=="--validation") o.validation=true;
            else if(option=="--require-native") o.require_native=true;
            else if(option=="--bench") o.bench=true;
            else if(option=="--bench-only") { o.bench=true; o.bench_only=true; }
            else if(option=="--quality-only") o.quality_only=true;
            else if(option=="--adversarial-only") { o.adversarial_only=true; o.quality_only=true; }
            else if(option=="--linear-only") o.linear_only=true;
            else if(option=="--native-literals-only") o.literals_only=true;
            else if(option=="--corrupt-etc") o.corrupt_etc=true;
            else if(option=="--corrupt-oracle") o.corrupt_oracle=true;
            else if(option=="--omit-dependency") o.omit_dependency=true;
            else if(option=="--texture-pattern") { if(n+1>=argc) throw std::runtime_error("Missing texture pattern");const std::string pattern=argv[++n];if(pattern=="corpus") o.texture_pattern=0;else if(pattern=="dense-skew") o.texture_pattern=1;else if(pattern=="sparse-skew") o.texture_pattern=2;else throw std::runtime_error("Unknown texture pattern"); }
            else if(option=="--reference-encoder-spv") { if(n+1>=argc || !o.reference_module.empty()) throw std::runtime_error("Missing or duplicate reference encoder path");o.reference_path=argv[++n];o.reference_module=load_reference_encoder(o.reference_path.c_str()); }
            else if(option=="--reference-bc1-spv") { if(n+1>=argc || !o.reference_bc1.empty()) throw std::runtime_error("Missing or duplicate BC1 reference path");o.reference_bc1_path=argv[++n];o.reference_bc1=load_reference_encoder(o.reference_bc1_path.c_str()); }
            else if(option=="--reference-bc3-spv") { if(n+1>=argc || !o.reference_bc3.empty()) throw std::runtime_error("Missing or duplicate BC3 reference path");o.reference_bc3_path=argv[++n];o.reference_bc3=load_reference_encoder(o.reference_bc3_path.c_str()); }
            else if(option=="--seed") { if(n+1>=argc) throw std::runtime_error("Missing seed value"); char *end=nullptr; const auto value=std::strtoull(argv[++n],&end,0); if(!end || *end || value>UINT32_MAX) throw std::runtime_error("Seed must be an unsigned 32-bit integer"); corpus_seed=uint32_t(value); }
            else {
                if(n+1>=argc) throw std::runtime_error("Missing option value");
                char *end=nullptr; const double value=std::strtod(argv[++n],&end);
                if(!end || *end || !std::isfinite(value) || value<0 || value>512) throw std::runtime_error("Invalid bounded option value");
                if(option=="--rgb-rmse-limit") { if(value>32) throw std::runtime_error("RGB bound may only tighten the predeclared 32 LSB limit"); o.rgb_limit=value; }
                else if(option=="--alpha-rmse-limit") { if(value>8) throw std::runtime_error("Alpha bound may only tighten the predeclared 8 LSB limit"); o.alpha_limit=value; }
                else { if(std::floor(value)!=value) throw std::runtime_error("Integer option required");
                    if(option=="--size") o.size=uint32_t(value); else if(option=="--warmup") o.warmup=uint32_t(value); else if(option=="--repeats") o.repeats=uint32_t(value); else if(option=="--samples") o.samples=uint32_t(value); else throw std::runtime_error("Unknown option"); }
            }
        }
        if(o.size<16 || o.size>512 || (o.size&(o.size-1)) || o.warmup>30 || !o.repeats || o.repeats>30 || !o.samples || o.samples>128) throw std::runtime_error("Benchmark option outside bounds");
        if(o.quality_only && o.bench) throw std::runtime_error("--quality-only cannot be combined with benchmark mode");
        if(o.linear_only && (o.quality_only || o.bench)) throw std::runtime_error("--linear-only cannot be combined with quality or benchmark modes");
        if(o.literals_only && (o.quality_only || o.linear_only || o.bench)) throw std::runtime_error("--native-literals-only cannot be combined with other modes");
        if(o.quality_only && o.require_native) throw std::runtime_error("--require-native needs sampled acceptance checks; omit --quality-only");
        if((o.corrupt_etc || o.corrupt_oracle || o.omit_dependency) && (o.quality_only || o.bench_only || o.linear_only || o.literals_only)) throw std::runtime_error("Negative controls require the wrapper/native sampling checks");
        if(o.omit_dependency && !o.validation) throw std::runtime_error("--omit-dependency requires --validation to prove the synchronization hazard");
        if(o.texture_pattern && !o.bench_only) throw std::runtime_error("Recovery texture patterns require --bench-only");
        if(o.external_reference() && (!o.bench_only || o.baseline)) throw std::runtime_error("External encoder requires --bench-only, without --baseline; weighted wrapper remains native lossy");
        if(!o.reference_module.empty() && (!o.reference_bc1.empty() || !o.reference_bc3.empty())) throw std::runtime_error("Common and per-format external encoders are mutually exclusive");
        if(o.reference_bc1.empty()!=o.reference_bc3.empty()) throw std::runtime_error("Per-format external encoders require both BC1 and BC3 paths");
        if(o.baseline || o.external_reference()) verify_baseline_version(argv[1]);
        if(o.validation) {
            constexpr const char *sync="VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT";
            const auto *old=std::getenv("VK_LAYER_ENABLES"); std::string enables=old?old:"";
            if(enables.find(sync)==std::string::npos) { if(!enables.empty()) enables+=","; enables+=sync; if(setenv("VK_LAYER_ENABLES",enables.c_str(),1)) throw std::runtime_error("Cannot request synchronization validation"); }
            std::printf("SYNC_VALIDATION_REQUESTED: true\n");
        }
        for(auto f:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK}) {
            const auto *selected=o.reference_selection().select(f);
            const auto &module=selected?*selected:production_encoder(f);
            const char *path=selected?(!o.reference_module.empty()?o.reference_path.c_str():f==VK_FORMAT_BC3_UNORM_BLOCK?o.reference_bc3_path.c_str():o.reference_bc1_path.c_str()):f==VK_FORMAT_BC3_UNORM_BLOCK?"embedded:bc_etc2_encode_bc3_spv":f==VK_FORMAT_BC2_UNORM_BLOCK?"embedded:bc_etc2_encode_spv":"embedded:bc_etc2_encode_bc1_spv";
            encoder_identity("ETC_REFERENCE_ENCODER",selected?"external":"embedded_production",path,f,module);
        }
        EncoderApp a; init(a,argv[1],o.validation,!o.baseline && !o.external_reference()); oracle_anchors();
        if(!o.bench_only && !o.linear_only && !o.literals_only) {
            if(!o.adversarial_only) quality(a,o);
            adversarial_quality(a,o);
        }
        if(!o.baseline) check(dlsym(a.library,"pvb_get_compression_stats")!=nullptr,"Candidate exposes additive compression observation ABI");
        uint32_t skipped=0;
        if(!o.quality_only && !o.bench_only && !o.linear_only) skipped+=native_literals(a,o);
        if(!o.quality_only && !o.bench_only && !o.linear_only && !o.literals_only) {
        srgb_calibration(a);
        for(uint32_t format=131;format<=138;++format) {
            const auto f=VkFormat(format); const bool native=native_support(a,f);
            std::printf("ETC_NATIVE_STATUS {\"format\":%u,\"native_etc_format\":%u,\"supported\":%s,\"lossy_eligible\":%s,\"wrapper_expected_route\":\"%s\"}\n",format,uint32_t(etc_format(f)),native?"true":"false",eligible(f)?"true":"false",lossy_expected(o,f,native)?"etc2_lossy":"rgba_lossless");
            if(!native) { ++skipped; std::printf("SKIP: native ETC2 sampling unsupported for format %u; direct GPU encoding quality remains exercised\n",format); if(o.require_native) check(false,"Required native ETC2 compressed sampling unavailable"); }
            if(o.corrupt_etc && eligible(f) && !lossy_expected(o,f,native)) check(false,"Corrupt-ETC control requires the candidate compressed route; use a native ETC2 backend without --baseline");
            sample_case(a,o,f,7,5,2,0,false,false,native);
            sample_case(a,o,f,16,16,6,1,true,false,native);
            sample_case(a,o,f,16,8,2,4,false,false,native);
            copy_partial_case(a,o,f,native);
            mixed_copy_case(a,o,f,native);
            if(native && !o.baseline) { sample_case(a,o,f,7,5,2,0,false,true,true); sample_case(a,o,f,16,16,6,1,true,true,true); }
            else if(native) std::printf("SKIP: raw native ETC2 sampling omitted for --baseline; that wrapper does not enable the ETC2 device feature\n");
        }
        // Unchanged lossless checks remain meaningful on the mixed policy path.
        for(auto f:{VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC4_SNORM_BLOCK,VK_FORMAT_BC5_UNORM_BLOCK,VK_FORMAT_BC5_SNORM_BLOCK}) case_test(a,f,7,5,2,0,true,true,false,false);
        lossless_bc7(a);
        }
        if(!o.quality_only && !o.bench_only && !o.literals_only) skipped+=linear_checks(a,o);
        if(o.bench) benchmark(a,o);
        if(o.omit_dependency && skipped) check(false,"Missing-dependency control requires a backend supporting native ETC2; rerun with the documented compressed backend");
        auto compression=reinterpret_cast<PvbGetCompressionStats>(dlsym(a.library,"pvb_get_compression_stats"));
        if(compression) { PvbCompressionStats s{}; compression(&s); std::printf("ETC_COMPRESSION_COUNTERS {\"version\":%u,\"native_etc2_devices\":%llu,\"etc2_dispatches\":%llu,\"rgba_dispatches\":%llu,\"cross_backing_regenerations\":%llu,\"live_etc2_images\":%llu,\"live_rgba_images\":%llu,\"live_etc2_logical_bytes\":%llu,\"live_rgba_logical_bytes\":%llu}\n",s.version,(unsigned long long)s.native_etc2_devices,(unsigned long long)s.etc2_upload_dispatches,(unsigned long long)s.rgba_upload_dispatches,(unsigned long long)s.cross_backing_regenerations,(unsigned long long)s.live_etc2_images,(unsigned long long)s.live_rgba_images,(unsigned long long)s.live_etc2_logical_bytes,(unsigned long long)s.live_rgba_logical_bytes); check(!s.live_etc2_images && !s.live_rgba_images && !s.live_etc2_logical_bytes && !s.live_rgba_logical_bytes,"ETC2/RGBA wrapper texture backing counters return to zero"); }
        else std::printf("ETC_COMPRESSION_COUNTERS: null (unsupported by explicit baseline)\n");
        PvbStats stats{}; a.stats(&stats); check(!stats.live_upload_resources,"ETC probe releases intercepted upload resources after command reset");
        auto transfer=reinterpret_cast<PvbGetTransferStats>(dlsym(a.library,"pvb_get_transfer_stats")); if(transfer) { PvbTransferStats s{}; transfer(&s); check(!s.live_block_stores && !s.live_block_bytes,"ETC probe releases original-block stores"); }
        a.shutdown();
        std::printf("ETC_RESULT {\"passed\":%u,\"failed\":%u,\"validation_errors\":%u,\"native_format_skips\":%u,\"quality_executed\":%s,\"seed\":%u,\"rgb_rmse_limit\":%.6f,\"alpha_rmse_limit\":%.6f,\"bc2_preview_alpha_max_error_bound\":17,\"bc2_wrapper_backing\":\"rgba_lossless\",\"bc3_alpha_max_error_limit\":16,\"visual_acceptance_established\":false,\"gameplay_fps_measured\":false}\n",passed,failed,validation_errors,skipped,o.bench_only || o.linear_only || o.literals_only?"false":"true",corpus_seed,o.rgb_limit,o.alpha_limit);
        return failed || validation_errors?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"ETC_ERROR: %s\n",e.what()); return 1; }
}
#endif
