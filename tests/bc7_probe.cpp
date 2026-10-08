// SPDX-License-Identifier: MIT
// Reuse the checked Vulkan/Android loader, diagnostics ownership and sampler.
// The BC7 oracle is the separately implemented pinned bcdec CPU decoder.
#define main pixel_bc_probe_main
#include "probe.cpp"
#undef main
#include "bc7_cube_sample_spv.h"

namespace {
using Bc7Block=std::array<uint8_t,16>;
struct Bc7Fixture { Bc7Block block; uint32_t mode; int literal=-1; };
uint32_t bc7_seed=0xbc7f060au;
void bc7_set_bits(Bc7Block &block,uint32_t offset,uint32_t count,uint32_t value) {
    for(uint32_t n=0;n<count;++n) {
        auto &byte=block[(offset+n)/8]; const uint8_t mask=uint8_t(1u<<((offset+n)%8));
        byte=uint8_t((byte&~mask)|(((value>>n)&1u)?mask:0u));
    }
}
Bc7Block bc7_random_block(uint32_t mode) {
    Bc7Block block{};
    for(uint32_t n=0;n<16;n+=4) write32(block.data()+n,random_word(bc7_seed));
    bc7_set_bits(block,0,mode+1,1u<<mode);
    return block;
}
std::array<uint8_t,64> bc7_reference(const uint8_t *bytes) {
    alignas(8) Bc7Block block{}; std::memcpy(block.data(),bytes,16);
    std::array<uint8_t,64> result{}; bcdec_bc7(block.data(),result.data(),16); return result;
}
std::vector<Bc7Fixture> bc7_corpus() {
    std::vector<Bc7Fixture> result;
    // Explicit unary mode bits ensure every mode is exercised equally.
    for(uint32_t mode=0;mode<8;++mode) for(uint32_t n=0;n<2048;++n)
        result.push_back({bc7_random_block(mode),mode});
    // Exhaust every partition, including its subset anchors. Payload extremes
    // exercise p-bit expansion, largest legal selectors and bit-word crossings.
    for(uint32_t mode:{0u,1u,2u,3u,7u}) {
        const uint32_t bits=mode==0?4:6;
        for(uint32_t partition=0;partition<(1u<<bits);++partition) for(uint32_t pattern=0;pattern<8;++pattern) {
            Bc7Block block=bc7_random_block(mode);
            if(pattern<4) { block.fill(std::array<uint8_t,4>{0,255,0x55,0xaa}[pattern]); bc7_set_bits(block,0,mode+1,1u<<mode); }
            else if(pattern<6) for(uint32_t bit=32;bit<128;++bit) bc7_set_bits(block,bit,1,pattern==4?0:1);
            bc7_set_bits(block,mode+1,bits,partition); result.push_back({block,mode});
        }
    }
    for(uint32_t mode:{4u,5u}) for(uint32_t rotation=0;rotation<4;++rotation)
        for(uint32_t selection=0;selection<(mode==4?2u:1u);++selection) for(uint32_t n=0;n<32;++n) {
            Bc7Block block=bc7_random_block(mode);
            if(n<4) { block.fill(std::array<uint8_t,4>{0,255,0x55,0xaa}[n]); bc7_set_bits(block,0,mode+1,1u<<mode); }
            bc7_set_bits(block,mode+1,2,rotation);
            if(mode==4) bc7_set_bits(block,mode+3,1,selection);
            result.push_back({block,mode});
        }
    for(uint32_t n=0;n<256;++n) { auto block=bc7_random_block(7); block[0]=0; result.push_back({block,8,0}); }
    Bc7Block zero{}; zero[0]=0x40; result.push_back({zero,6,0});
    Bc7Block full{}; full.fill(255); full[0]=0xc0; result.push_back({full,6,255});
    return result;
}
void bc7_capabilities(App &a) {
    VkPhysicalDeviceFeatures features{}; a.GetPhysicalDeviceFeatures(a.physical,&features);
    check(!features.textureCompressionBC,"BC core flag remains false (BC6 is unsupported)");
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; a.GetPhysicalDeviceFeatures2(a.physical,&features2);
    check(!features2.features.textureCompressionBC,"Features2 BC core flag remains false");
    const VkFormatFeatureFlags supported=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for(auto format:{VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK}) {
        VkFormatProperties p{}; a.GetPhysicalDeviceFormatProperties(a.physical,format,&p);
        check((p.optimalTilingFeatures&supported)==supported && !(p.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && !p.linearTilingFeatures && !p.bufferFeatures,"BC7 sampled/upload capabilities are truthful");
        VkFormatProperties3 p3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3}; VkFormatProperties2 p2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2}; p2.pNext=&p3; a.GetPhysicalDeviceFormatProperties2(a.physical,format,&p2);
        check(p3.optimalTilingFeatures==p.optimalTilingFeatures && !p3.linearTilingFeatures && !p3.bufferFeatures,"BC7 extended properties agree");
        check(pvb::is_bc(format) && pvb::block_bytes(format)==16,"BC7 layout uses 16-byte compressed blocks");
    }
    check(pvb::rgba_format(VK_FORMAT_BC7_UNORM_BLOCK)==VK_FORMAT_R8G8B8A8_UNORM && pvb::rgba_format(VK_FORMAT_BC7_SRGB_BLOCK)==VK_FORMAT_R8G8B8A8_SRGB,"BC7 canonical color spaces");
    check(pvb::compatible_view(VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK) && pvb::compatible_view(VK_FORMAT_BC7_SRGB_BLOCK,VK_FORMAT_BC7_UNORM_BLOCK) && !pvb::compatible_view(VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK),"BC7 mutable views stay in the BC7 family");
    for(auto format:{VK_FORMAT_BC6H_UFLOAT_BLOCK,VK_FORMAT_BC6H_SFLOAT_BLOCK}) {
        check(!pvb::is_bc(format),"BC6 is outside the supported layout formats");
        VkFormatProperties p{}; a.GetPhysicalDeviceFormatProperties(a.physical,format,&p);
        check(!p.optimalTilingFeatures && !p.linearTilingFeatures && !p.bufferFeatures,"BC6 format properties remain unsupported");
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={4,4,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image{}; const auto status=a.CreateImage(a.device,&info,nullptr,&image);
        check(status==VK_ERROR_FORMAT_NOT_SUPPORTED,"BC6 image creation remains unsupported");
        if(status==VK_SUCCESS) a.DestroyImage(a.device,image,nullptr);
    }
}
void bc7_direct(App &a) {
    stage("BC7 exact direct decoder corpus begin");
    auto fixtures=bc7_corpus(); constexpr uint32_t br=64;
    const uint32_t bh=(uint32_t(fixtures.size())+br-1)/br,width=br*4,height=bh*4;
    const uint64_t output_bytes=uint64_t(width)*height*4,input_bytes=uint64_t(br)*bh*16;
    DiagnosticResources owned(a);
    owned.input=a.buffer(input_bytes+128,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    owned.output=a.buffer(output_bytes+128,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(owned.input.mapped,0,size_t(input_bytes+128));
    std::memset(owned.output.mapped,0xa5,size_t(output_bytes+128));
    auto raw=static_cast<uint8_t*>(owned.input.mapped)+64;
    for(size_t n=0;n<fixtures.size();++n) std::memcpy(raw+n*16,fixtures[n].block.data(),16);
    VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ds.bindingCount=2; ds.pBindings=bindings;
    require(a.CreateDescriptorSetLayout(a.device,&ds,nullptr,&owned.descriptors),"BC7 direct descriptors");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,24}; VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&owned.descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&range;
    require(a.CreatePipelineLayout(a.device,&pl,nullptr,&owned.layout),"BC7 direct layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(bc_decode_spv); sm.pCode=bc_decode_spv;
    require(a.CreateShaderModule(a.device,&sm,nullptr,&owned.shader),"BC7 runtime decoder shader");
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=owned.shader; cp.stage.pName="main"; cp.layout=owned.layout;
    require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&cp,nullptr,&owned.pipeline),"BC7 direct pipeline");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}; VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&pool_size;
    require(a.CreateDescriptorPool(a.device,&pool,nullptr,&owned.pool),"BC7 direct pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=owned.pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&owned.descriptors; VkDescriptorSet set{};
    require(a.AllocateDescriptorSets(a.device,&allocate,&set),"BC7 direct set");
    VkDescriptorBufferInfo buffers[2]={{owned.input.handle,64,input_bytes},{owned.output.handle,64,output_bytes}}; VkWriteDescriptorSet writes[2]{};
    for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&buffers[n]; }
    a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    owned.begin(); uint32_t push[6]={145,width,height,1,br,bh};
    a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,owned.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,owned.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,owned.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push); a.CmdDispatch(a.command,(width+7)/8,(height+7)/8,1);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=owned.output.handle; host.size=VK_WHOLE_SIZE; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); a.submit();
    auto bytes=static_cast<uint8_t*>(owned.output.mapped),actual=bytes+64;
    uint32_t mode_counts[9]{},mode_failures[9]{};
    for(size_t n=0;n<size_t(br)*bh;++n) {
        auto expected=bc7_reference(raw+n*16); bool equal=true;
        const uint32_t bx=uint32_t(n)%br,by=uint32_t(n)/br;
        for(uint32_t py=0;py<4;++py) for(uint32_t px=0;px<4;++px) for(uint32_t ch=0;ch<4;++ch) {
            const size_t offset=(size_t(by*4+py)*width+bx*4+px)*4+ch;
            if(actual[offset]!=expected[(py*4+px)*4+ch]) { if(equal) std::fprintf(stderr,"BC7_RAW_MISMATCH: block=%zu mode=%u pixel=%u channel=%u got=%u expected=%u\n",n,n<fixtures.size()?fixtures[n].mode:8,py*4+px,ch,actual[offset],expected[(py*4+px)*4+ch]); equal=false; }
        }
        check(equal,"BC7 exact direct bytes match independent bcdec");
        if(n<fixtures.size()) {
            ++mode_counts[fixtures[n].mode];
            if(!equal) ++mode_failures[fixtures[n].mode];
            if(fixtures[n].literal>=0) check(std::all_of(expected.begin(),expected.end(),[&](uint8_t v){return v==fixtures[n].literal;}),"BC7 direct literal endpoints or reserved zero mode");
        }
        if(n>=fixtures.size()||fixtures[n].mode==8) check(std::all_of(expected.begin(),expected.end(),[](uint8_t v){return v==0;}),"Invalid zero-mode reference is literal transparent black");
    }
    check(std::all_of(bytes,bytes+64,[](uint8_t v){return v==0xa5;}) && std::all_of(actual+output_bytes,actual+output_bytes+64,[](uint8_t v){return v==0xa5;}),"BC7 direct decoder preserves SSBO bounds guards");
    Bc7Block zero{}; zero[0]=0x40; const auto transparent=bc7_reference(zero.data());
    check(std::all_of(transparent.begin(),transparent.end(),[](uint8_t v){return v==0;}),"Mode6 zero endpoints have literal RGBA zero");
    Bc7Block full{}; full.fill(255); full[0]=0xc0; const auto white=bc7_reference(full.data());
    check(std::all_of(white.begin(),white.end(),[](uint8_t v){return v==255;}),"Mode6 full endpoints and pbits have literal RGBA255");
    for(uint32_t mode=0;mode<9;++mode) std::printf("BC7_RAW_MODE: %u blocks=%u exact_bytes=%u\n",mode,mode_counts[mode],mode_failures[mode]?0u:1u);
    std::printf("BC7_RAW_BLOCKS: %zu (plus %zu transparent padding blocks)\n",fixtures.size(),size_t(br)*bh-fixtures.size());
    owned.reset("BC7 direct command reset");
    stage("BC7 exact direct decoder corpus complete");
}
void bc7_sample_case(App &a,VkFormat format,VkFormat view_format,uint32_t width,uint32_t height,uint32_t layers,uint32_t mip,bool padded,bool copy2,uint32_t mode,bool partial=false) {
    DiagnosticResources owned(a);
    const uint32_t w=std::max(1u,width>>mip),h=std::max(1u,height>>mip);
    VkBufferImageCopy region{}; region.bufferOffset=64; region.bufferRowLength=padded?((w+3)/4+2)*4:0; region.bufferImageHeight=padded?((h+3)/4+1)*4:0; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,layers}; region.imageExtent={w,h,1};
    if(partial) { region.imageOffset={4,4,0}; region.imageExtent={w-4,h-4,1}; }
    pvb::CopyLayout sizes{}; if(!pvb::copy_layout(format,region,{width,height,1},mip+1,layers,sizes)) throw std::runtime_error("BC7 sample layout");
    owned.input=a.buffer(sizes.input_bytes+64,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT); owned.output=a.buffer(sizes.output_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(owned.input.mapped,0xcd,size_t(sizes.input_bytes+64)); std::memset(owned.output.mapped,0xa5,size_t(sizes.output_bytes));
    const uint32_t rw=region.imageExtent.width,rh=region.imageExtent.height;
    std::vector<uint8_t> expected(size_t(sizes.output_bytes)); auto raw=static_cast<uint8_t*>(owned.input.mapped)+64;
    for(uint32_t layer=0;layer<layers;++layer) for(uint32_t by=0;by<(rh+3)/4;++by) for(uint32_t bx=0;bx<(rw+3)/4;++bx) {
        auto block=bc7_random_block((mode+layer+by+bx)%8); auto pointer=raw+((size_t(layer)*sizes.blocks_height+by)*sizes.blocks_row+bx)*16;
        std::memcpy(pointer,block.data(),16); auto decoded=bc7_reference(block.data());
        for(uint32_t py=0;py<4;++py) for(uint32_t px=0;px<4;++px) if(by*4+py<rh && bx*4+px<rw)
            std::memcpy(expected.data()+((size_t(layer)*rh+by*4+py)*rw+bx*4+px)*4,decoded.data()+(py*4+px)*4,4);
    }
    std::vector<uint8_t> original(static_cast<uint8_t*>(owned.input.mapped),static_cast<uint8_t*>(owned.input.mapped)+sizes.input_bytes+64);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={width,height,1}; info.mipLevels=mip+1; info.arrayLayers=layers; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const bool cube=layers==6 && width==height; if(cube) info.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkFormat views[2]={VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK}; VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; list.viewFormatCount=2; list.pViewFormats=views;
    if(format!=view_format) { info.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; info.pNext=&list; }
    VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS}; query.pCreateInfo=&info; VkMemoryRequirements2 before{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; a.GetDeviceImageMemoryRequirements(a.device,&query,&before);
    require(a.CreateImage(a.device,&info,nullptr,&owned.image),"BC7 image"); VkMemoryRequirements memory{}; a.GetImageMemoryRequirements(a.device,owned.image,&memory);
    check(before.memoryRequirements.size==memory.size && before.memoryRequirements.memoryTypeBits==memory.memoryTypeBits,"BC7 precreation and created memory requirements agree");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=memory.size; allocate.memoryTypeIndex=a.memory_index(memory.memoryTypeBits,0);
    require(a.AllocateMemory(a.device,&allocate,nullptr,&owned.memory),"BC7 image memory"); require(a.BindImageMemory(a.device,owned.image,owned.memory,0),"BC7 image bind");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=owned.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format=view_format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,layers};
    if(cube) view.viewType=VK_IMAGE_VIEW_TYPE_CUBE;
    require(a.CreateImageView(a.device,&view,nullptr,&owned.view),"BC7 sample view");
    VkDescriptorPoolSize pool_sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=2; pool.pPoolSizes=pool_sizes;
    require(a.CreateDescriptorPool(a.device,&pool,nullptr,&owned.pool),"BC7 sample pool"); VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=owned.pool; ds.descriptorSetCount=1; ds.pSetLayouts=&a.descriptors; VkDescriptorSet set{};
    require(a.AllocateDescriptorSets(a.device,&ds,&set),"BC7 sample set");
    VkDescriptorImageInfo image_descriptor{a.sampler,owned.view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buffer_descriptor{owned.output.handle,0,sizes.output_bytes}; VkWriteDescriptorSet writes[2]{};
    for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&image_descriptor; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&buffer_descriptor; a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    if(cube) {
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(bc7_cube_sample_spv); sm.pCode=bc7_cube_sample_spv;
        require(a.CreateShaderModule(a.device,&sm,nullptr,&owned.shader),"BC7 cube sampling shader");
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=owned.shader; cp.stage.pName="main"; cp.layout=a.layout;
        require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&cp,nullptr,&owned.pipeline),"BC7 cube sampling pipeline");
    }
    const auto image_layout=copy2?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
    owned.begin(); VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=image_layout; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.image=owned.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,layers};
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    uint32_t push[8]={rw,rh,layers,0,0,view_format==VK_FORMAT_BC7_SRGB_BLOCK?1u:0u,uint32_t(region.imageOffset.x),uint32_t(region.imageOffset.y)};
    // Bind the caller sampler before the decoder is injected: state restoration
    // is part of this real wrapper test. Destroy its compatible layout early.
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,32}; VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&a.descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&range;
    require(a.CreatePipelineLayout(a.device,&pl,nullptr,&owned.layout),"BC7 caller compatible layout");
    a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,cube?owned.pipeline:a.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,owned.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,owned.layout,range.stageFlags,0,sizeof(push),push);
    a.DestroyPipelineLayout(a.device,std::exchange(owned.layout,VK_NULL_HANDLE),nullptr);
    if(copy2) { VkBufferImageCopy2 r{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2}; r.bufferOffset=region.bufferOffset; r.bufferRowLength=region.bufferRowLength; r.bufferImageHeight=region.bufferImageHeight; r.imageSubresource=region.imageSubresource; r.imageOffset=region.imageOffset; r.imageExtent=region.imageExtent;
        VkCopyBufferToImageInfo2 copy{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2}; copy.srcBuffer=owned.input.handle; copy.dstImage=owned.image; copy.dstImageLayout=image_layout; copy.regionCount=1; copy.pRegions=&r; a.CmdCopyBufferToImage2(a.command,&copy);
    } else a.CmdCopyBufferToImage(a.command,owned.input.handle,owned.image,image_layout,1,&region);
    barrier.oldLayout=image_layout; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    a.CmdDispatch(a.command,(rw+7)/8,(rh+7)/8,layers);
    // Copy original compressed BC7 blocks back through the caller's pitched
    // layout. Reuse the upload buffer after its transfer reads have completed.
    barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkBufferMemoryBarrier reuse{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; reuse.buffer=owned.input.handle; reuse.size=VK_WHOLE_SIZE; reuse.srcQueueFamilyIndex=reuse.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; reuse.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; reuse.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&reuse,1,&barrier);
    a.CmdCopyImageToBuffer(a.command,owned.image,VK_IMAGE_LAYOUT_GENERAL,owned.input.handle,1,&region);
    VkBufferMemoryBarrier host[2]{};
    for(uint32_t n=0;n<2;++n) { host[n].sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; host[n].buffer=n==0?owned.output.handle:owned.input.handle; host[n].size=VK_WHOLE_SIZE; host[n].srcQueueFamilyIndex=host[n].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host[n].srcAccessMask=n==0?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT; host[n].dstAccessMask=VK_ACCESS_HOST_READ_BIT; }
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,2,host,0,nullptr); a.submit(copy2);
    check(std::memcmp(owned.input.mapped,original.data(),original.size())==0,"BC7 retained compressed readback is exact including offset and padding");
    auto actual=static_cast<uint8_t*>(owned.output.mapped); bool equal=true; uint32_t delta=0;
    for(size_t n=0;n<expected.size();++n) { const auto difference=uint32_t(std::abs(int(actual[n])-int(expected[n]))); delta=std::max(delta,difference); if(difference>1) { if(equal) std::fprintf(stderr,"BC7_SAMPLE_MISMATCH: format=%u view=%u %ux%u layers=%u mip=%u mode=%u byte=%zu got=%u expected=%u\n",uint32_t(format),uint32_t(view_format),rw,rh,layers,mip,mode,n,actual[n],expected[n]); equal=false; } }
    max_difference=std::max(max_difference,delta); check(equal,"BC7 sampled pixels match independent bcdec (+/-1 LSB)");
    std::printf("BC7_SAMPLE: format=%u view=%u %ux%u layers=%u mip=%u cube=%u mode=%u max_difference=%u\n",uint32_t(format),uint32_t(view_format),rw,rh,layers,mip,cube?1u:0u,mode,delta);
    owned.reset("BC7 sampled command reset"); PvbStats stats{}; a.stats(&stats); check(!stats.live_upload_resources,"BC7 command reset releases decoder resources");
}
}
int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IOLBF,0); std::atexit([]{stage("BC7 process atexit (normal termination)");});
    if(argc<2) { std::fprintf(stderr,"Usage: pixel_bc7_probe /absolute/path/libvulkan_pixel_bc.so [--validation]\n"); return 2; }
    const bool validation=argc>2 && !std::strcmp(argv[2],"--validation");
    try {
        App app; init(app,argv[1],validation); bc7_capabilities(app); bc7_direct(app); stage("BC7 sampled wrapper cases begin");
        uint32_t uploads=0;
        for(auto format:{VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK}) {
            for(uint32_t mode=0;mode<8;++mode) {
                bc7_sample_case(app,format,format,4,4,1,0,false,false,mode); ++uploads;
                bc7_sample_case(app,format,format,9,7,2,0,true,true,mode); ++uploads;
            }
            for(uint32_t mip=1;mip<=3;++mip) { bc7_sample_case(app,format,format,9,7,2,mip,true,(mip%2)==0,mip); ++uploads; }
            bc7_sample_case(app,format,format,8,8,6,0,true,true,4); ++uploads;
            bc7_sample_case(app,format,format,7,7,2,0,true,false,7,true); ++uploads;
            bc7_sample_case(app,format,format==VK_FORMAT_BC7_UNORM_BLOCK?VK_FORMAT_BC7_SRGB_BLOCK:VK_FORMAT_BC7_UNORM_BLOCK,9,7,2,0,true,true,5); ++uploads;
        }
        PvbStats stats{}; app.stats(&stats); check(stats.decode_dispatches==uploads,"Every BC7 upload recorded a real GPU decoder dispatch"); check(!stats.backend_recursion_rejections && !stats.recording_failures && !stats.live_upload_resources,"BC7 run leaves no recording failure, recursion or upload resources");
        app.shutdown(); check(!validation_errors,"BC7 Vulkan validation clean including teardown");
        std::printf("BC7_GPU_UPLOADS: %u\nMAX_REFERENCE_DIFFERENCE: %u LSB\nVALIDATION_ENABLED: %d\nVALIDATION_ERRORS: %u\nBACKEND: %s\n",uploads,max_difference,validation?1:0,validation_errors,stats.backend);
    } catch(const std::exception &e) { ++failed; std::fprintf(stderr,"ERROR: %s\n",e.what()); }
    std::printf("RESULT: %u passed; %u failed\n",passed,failed); std::printf("STAGE: BC7 main returning status=%d\n",failed?1:0); return failed?1:0;
}
