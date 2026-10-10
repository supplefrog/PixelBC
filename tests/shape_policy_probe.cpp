// SPDX-License-Identifier: MIT
// Targeted acceptance of the two immutable whole-image backing exceptions.
// Existing BC/detex expectations and genuine-alpha classification are reused.
#define PVB_ETC2_NO_MAIN
#include "etc2_probe.cpp"

namespace {
PvbGetCompressionStats compression{};
PvbCompressionStats counters() { PvbCompressionStats s{}; compression(&s); return s; }
uint32_t levels(uint32_t w,uint32_t h) { uint32_t n=1; while(w>1 || h>1) { w=std::max(1u,w/2);h=std::max(1u,h/2);++n; } return n; }
VkImageCreateInfo description(VkFormat f,uint32_t w,uint32_t h,uint32_t layers,uint32_t mips) {
    VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; i.imageType=VK_IMAGE_TYPE_2D; i.format=f; i.extent={w,h,1}; i.mipLevels=mips; i.arrayLayers=layers;
    i.samples=VK_SAMPLE_COUNT_1_BIT; i.tiling=VK_IMAGE_TILING_OPTIMAL; i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; return i;
}
void create(App &a,DiagnosticResources &r,const VkImageCreateInfo &i) {
    VkDeviceImageMemoryRequirements q{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS}; q.pCreateInfo=&i;
    VkMemoryRequirements2 before{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; a.GetDeviceImageMemoryRequirements(a.device,&q,&before);
    auto alias=a.dp<PFN_vkGetDeviceImageMemoryRequirements>("vkGetDeviceImageMemoryRequirementsKHR");
    VkMemoryRequirements2 other{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; alias(a.device,&q,&other);
    check(before.memoryRequirements.size==other.memoryRequirements.size && before.memoryRequirements.alignment==other.memoryRequirements.alignment && before.memoryRequirements.memoryTypeBits==other.memoryRequirements.memoryTypeBits,"Core/KHR no-image memory queries agree");
    require(a.CreateImage(a.device,&i,nullptr,&r.image),"Shape image create"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,r.image,&req);
    check(req.size==before.memoryRequirements.size && req.alignment==before.memoryRequirements.alignment && req.memoryTypeBits==before.memoryRequirements.memoryTypeBits,"Actual image and no-image memory requirements agree exactly");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=req.size; allocation.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0);
    require(a.AllocateMemory(a.device,&allocation,nullptr,&r.memory),"Shape image allocate"); require(a.BindImageMemory(a.device,r.image,r.memory,0),"Shape image bind");
}
void routes(EncoderApp &a,bool require_native) {
    struct Route { uint32_t w,h; bool selected; };
    for(auto shape:{Route{512,1024,true},Route{256,2048,true},Route{511,1024,false},Route{512,1023,false},Route{512,1025,false},Route{257,2048,false},Route{256,2047,false},Route{256,2049,false},Route{1024,512,false},Route{2048,256,false},Route{7,5,false}}) {
        for(auto f:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK}) {
            DiagnosticResources r(a); auto i=description(f,shape.w,shape.h,2,levels(shape.w,shape.h));
            i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            const VkFormat views[]={f,VkFormat(srgb(f)?uint32_t(f)-1:uint32_t(f)+1)};
            VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; list.viewFormatCount=2; list.pViewFormats=views; i.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; i.pNext=&list;
            const auto before=counters(); create(a,r,i); const auto after=counters();
            const bool native=after.live_etc2_images==before.live_etc2_images+1;
            check(shape.selected?!native:(native || !require_native),"Both selected shapes are lossless; native neighbouring/transpose shapes retain ETC2");
            check(after.live_rgba_images+after.live_etc2_images==before.live_rgba_images+before.live_etc2_images+1,"Exactly one immutable backing is accounted");
            for(auto view_format:views) {
                VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image=r.image; v.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; v.format=view_format; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,i.mipLevels,0,2};
                require(a.CreateImageView(a.device,&v,nullptr,&r.view),"Shape mutable colorspace view"); a.DestroyImageView(a.device,std::exchange(r.view,VK_NULL_HANDLE),nullptr);
            }
            VkImageViewCreateInfo bad{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; bad.image=r.image; bad.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; bad.format=VK_FORMAT_BC3_UNORM_BLOCK; bad.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
            check(a.CreateImageView(a.device,&bad,nullptr,&r.view)==VK_ERROR_FORMAT_NOT_SUPPORTED,"Shape policy does not admit cross-family views");
            std::printf("SHAPE_ROUTE {\"format\":%u,\"width\":%u,\"height\":%u,\"mips\":%u,\"layers\":2,\"selected\":%s,\"actual_backing\":\"%s\"}\n",uint32_t(f),shape.w,shape.h,i.mipLevels,shape.selected?"true":"false",native?"etc2":"rgba");
        }
    }
    // Same measured shape must not extend the exception to BC1_RGB or BC3.
    for(auto f:{VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK}) {
        DiagnosticResources r(a); const auto before=counters(); create(a,r,description(f,256,2048,1,12)); const auto after=counters();
        check(f==VK_FORMAT_BC2_UNORM_BLOCK?after.live_rgba_images==before.live_rgba_images+1:(after.live_etc2_images==before.live_etc2_images+1 || !require_native),"Other formats retain their original measured-shape route");
    }
}
void chain(EncoderApp &a,VkFormat f,uint32_t fullw,uint32_t fullh,bool omit_dispatch) {
    constexpr uint32_t layers=2,padding=256; const auto count=levels(fullw,fullh);
    std::vector<Bytes> blocks,expected; std::vector<etc2_alpha::Mask> masks; std::vector<VkBufferImageCopy> regions; Bytes packed(padding,0xcc);
    for(uint32_t mip=0;mip<count;++mip) {
        const auto w=std::max(1u,fullw>>mip),h=std::max(1u,fullh>>mip); blocks.push_back(corpus(f,(w+3)/4,(h+3)/4,layers));
        expected.push_back(pixels(f,blocks.back(),w,h,layers,false)); masks.push_back(etc2_alpha::source_mask(f,blocks.back(),w,h,layers));
        VkBufferImageCopy c{}; c.bufferOffset=packed.size(); c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,layers}; c.imageExtent={w,h,1}; regions.push_back(c); packed.insert(packed.end(),blocks.back().begin(),blocks.back().end());
    }
    DiagnosticResources r(a); const auto before=counters(); create(a,r,description(f,fullw,fullh,layers,count)); const auto after=counters();
    check(after.live_rgba_images==before.live_rgba_images+1 && after.live_etc2_images==before.live_etc2_images,"Full-chain actual image uses one RGBA backing");
    r.input=a.buffer(packed.size()+64,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.output=a.buffer(expected[0].size()+64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); r.compressed=a.buffer(packed.size()+64,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memset(r.input.mapped,0xcc,packed.size()+64); std::memcpy(r.input.mapped,packed.data(),packed.size()); std::memset(r.output.mapped,0xa5,expected[0].size()+64); std::memset(r.compressed.mapped,0xa5,packed.size()+64);
    auto set=sample_descriptor(a,r,f,0,layers,false,expected[0].size());
    r.begin(); for(uint32_t mip=0;mip<count;++mip) image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_UNDEFINED,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Bind the actual caller sampling state before all intercepted uploads.
    const uint32_t push[8]={fullw,fullh,layers,0,0,srgb(f)?1u:0u,0,0}; a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,uint32_t(regions.size()),regions.data());
    for(uint32_t mip=0;mip<count;++mip) image_barrier(a,r.image,mip,layers,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT);
    if(!omit_dispatch) a.CmdDispatch(a.command,(fullw+7)/8,(fullh+7)/8,layers);
    a.CmdCopyImageToBuffer(a.command,r.image,VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,uint32_t(regions.size()),regions.data()); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT); a.submit();
    const auto uploaded=counters(); check(uploaded.rgba_upload_dispatches-after.rgba_upload_dispatches==count && uploaded.etc2_upload_dispatches==after.etc2_upload_dispatches,"Every selected full-chain region executes the RGBA conversion route");
    auto compare=[&](uint32_t mip,const char *name) {
        check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected[mip],masks[mip],1,name),"Selected image sampling matches original BC with exact genuine alpha");
        const auto *tail=static_cast<const uint8_t*>(r.output.mapped)+expected[mip].size(); bool intact=true; for(uint32_t n=0;n<64;++n) intact&=tail[n]==0xa5; check(intact,"Sampling output tail guard is intact");
    };
    compare(0,"shape_chain_base");
    Bytes expected_read(packed.size()+64,0xa5); for(uint32_t mip=0;mip<count;++mip) std::memcpy(expected_read.data()+regions[mip].bufferOffset,blocks[mip].data(),blocks[mip].size());
    check(!std::memcmp(r.compressed.mapped,expected_read.data(),expected_read.size()),"Whole mip-chain readback keeps every original BC byte and guards");
    check(!std::memcmp(r.input.mapped,packed.data(),packed.size()),"GPU uploads leave original source bytes unchanged");
    a.submit_ended(true); compare(0,"shape_chain_replay"); r.reset("Shape full-chain replay reset");
    PvbStats owned{}; a.stats(&owned); check(!owned.live_upload_resources,"Full-chain reset retires upload resources after replay");
    for(uint32_t mip=1;mip<count;++mip) {
        a.DestroyDescriptorPool(a.device,std::exchange(r.pool,VK_NULL_HANDLE),nullptr); a.DestroyImageView(a.device,std::exchange(r.view,VK_NULL_HANDLE),nullptr);
        set=sample_descriptor(a,r,f,mip,layers,false,expected[mip].size()); std::memset(r.output.mapped,0xa5,expected[0].size()+64);
        r.begin(); sample_dispatch(a,set,a.pipeline,regions[mip].imageExtent.width,regions[mip].imageExtent.height,layers,srgb(f)); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit(); compare(mip,"shape_chain_mip"); r.reset("Shape mip sampling reset");
    }
    std::printf("SHAPE_CHAIN {\"format\":%u,\"width\":%u,\"height\":%u,\"layers\":2,\"full_mips\":%u,\"original_source_bytes\":%zu,\"replay\":true,\"omit_dispatch_control\":%s}\n",uint32_t(f),fullw,fullh,count,packed.size()-padding,omit_dispatch?"true":"false");
}
void shape_copy(EncoderApp &a,VkFormat f,bool native) {
    DiagnosticResources r(a); constexpr uint32_t width=4,height=4,layers=1; const auto bc=corpus(f,1,1,layers);
    const auto lossless=pixels(f,bc,width,height,layers,false); const auto encoded=encode(a,f,bc,width,height,layers); const auto compressed=pixels(f,native?encoded:bc,width,height,layers,native);
    r.input=a.buffer(bc.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT); std::memcpy(r.input.mapped,bc.data(),bc.size()); r.output=a.buffer(lossless.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); r.compressed=a.buffer(bc.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto image_before=counters();
    create(a,r,description(f,512,1024,1,1)); make_image(a,f,8,8,1,0,false,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,r.transfer_images[0],r.transfer_memory[0]);
    const auto image_after=counters(); check(image_after.live_rgba_images-image_before.live_rgba_images==(native?1u:2u) && image_after.live_etc2_images-image_before.live_etc2_images==(native?1u:0u),"Copy fixture observes actual backing of each distinct image");
    auto set=sample_descriptor(a,r,f,0,1,false,lossless.size()); VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={4,4,1}; VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource=region.imageSubresource; copy.extent=region.imageExtent;
    const auto prior=counters();
    for(unsigned direction=0;direction<2;++direction) {
        const auto source=direction?r.transfer_images[0]:r.image,dest=direction?r.image:r.transfer_images[0];
        r.begin(); for(auto image:{r.image,r.transfer_images[0]}) image_barrier(a,image,0,1,direction?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,direction?VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        a.CmdCopyBufferToImage(a.command,r.input.handle,source,VK_IMAGE_LAYOUT_GENERAL,1,&region); image_barrier(a,source,0,1,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        a.CmdCopyImage(a.command,source,VK_IMAGE_LAYOUT_GENERAL,dest,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        image_barrier(a,dest,0,1,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        a.CmdCopyImageToBuffer(a.command,dest,VK_IMAGE_LAYOUT_GENERAL,r.compressed.handle,1,&region); host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        if(!direction) {
            // Temporarily sample the native destination through its own stored
            // backing, while the owning selected RGBA image stays alive.
            std::swap(r.image,r.transfer_images[0]); a.DestroyDescriptorPool(a.device,std::exchange(r.pool,VK_NULL_HANDLE),nullptr); a.DestroyImageView(a.device,std::exchange(r.view,VK_NULL_HANDLE),nullptr); set=sample_descriptor(a,r,f,0,1,false,lossless.size()); std::swap(r.image,r.transfer_images[0]);
        } else { a.DestroyDescriptorPool(a.device,std::exchange(r.pool,VK_NULL_HANDLE),nullptr); a.DestroyImageView(a.device,std::exchange(r.view,VK_NULL_HANDLE),nullptr); set=sample_descriptor(a,r,f,0,1,false,lossless.size()); }
        sample_dispatch(a,set,a.pipeline,width,height,layers,srgb(f)); host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); a.submit();
        check(!std::memcmp(r.compressed.mapped,bc.data(),bc.size()),"Shape-boundary image copies preserve exact original BC bytes");
        const auto &expected=direction?lossless:compressed; const auto mask=etc2_alpha::sampled_mask(f,bc,expected,width,height,layers,!direction && native);
        check(near_bytes(static_cast<const uint8_t*>(r.output.mapped),expected,mask,1,"shape_boundary_copy"),"Shape-boundary copies regenerate the independently decoded destination route"); r.reset("Shape copy reset");
    }
    check(!native || counters().cross_backing_regenerations>=prior.cross_backing_regenerations+2,"Both directions exercise actual mixed-backing regeneration");
}
}
int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IOLBF,0); std::setvbuf(stderr,nullptr,_IOLBF,0);
    if(argc<2) { std::fprintf(stderr,"usage: pixel_shape_policy_probe LIB [--require-native] [--validation] [--omit-final-dispatch]\n"); return 2; }
    try {
        bool require_native=false,validation=false,omit=false;
        for(int n=2;n<argc;++n) { const std::string v=argv[n]; if(v=="--require-native") require_native=true; else if(v=="--validation") validation=true; else if(v=="--omit-final-dispatch") omit=true; else throw std::runtime_error("Unknown option: "+v); }
        if(validation) setenv("VK_LAYER_ENABLES","VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",1);
        EncoderApp a; init(a,argv[1],validation); compression=reinterpret_cast<PvbGetCompressionStats>(dlsym(a.library,"pvb_get_compression_stats")); if(!compression) throw std::runtime_error("Missing compression observation ABI");
        const bool native=counters().native_etc2_devices!=0; check(!require_native || native,"Required native ETC2 device is available"); routes(a,require_native);
        for(auto f:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK}) { chain(a,f,512,1024,omit); chain(a,f,256,2048,omit); shape_copy(a,f,native); }
        case_test(a,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,512,1024,2,10,false,false,false,false,false,true);
        const auto s=counters(); check(!s.live_etc2_images && !s.live_rgba_images && !s.live_etc2_logical_bytes && !s.live_rgba_logical_bytes,"Shape probe retires all image backing ownership"); PvbStats owned{}; a.stats(&owned); check(!owned.live_upload_resources,"Shape probe retires all upload resources");
        const auto transfer=reinterpret_cast<PvbGetTransferStats>(dlsym(a.library,"pvb_get_transfer_stats")); PvbTransferStats stores{}; transfer(&stores); check(!stores.live_block_stores && !stores.live_block_bytes,"Shape probe retires every original-BC store"); a.shutdown();
        std::printf("SHAPE_RESULT {\"passed\":%u,\"failed\":%u,\"validation_errors\":%u,\"native_etc2\":%s,\"phone_performance_established\":false}\n",passed,failed,validation_errors,native?"true":"false"); return failed || validation_errors?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"SHAPE_ERROR: %s\n",e.what()); return 1; }
}
