// SPDX-License-Identifier: MIT
// Same explicit-library workload on Android and host. No game assets or FPS.
#define main pixel_bc_probe_main
#include "probe.cpp"
#undef main
#include "perf_stats.h"
#include <chrono>

namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point a,Clock::time_point b) { return std::chrono::duration<double,std::milli>(b-a).count(); }
struct BenchCase {
    VkFormat format; uint32_t width,height,mip;
    VkBufferImageCopy region{}; VkImageView view=VK_NULL_HANDLE; VkDescriptorSet set=VK_NULL_HANDLE;
    Buffer input{},output{}; std::vector<uint8_t> expected;
};
struct BenchOwner {
    App &a; std::vector<BenchCase> cases;
    std::array<VkImage,3> images{}; std::array<VkDeviceMemory,3> memories{};
    VkDescriptorPool pool=VK_NULL_HANDLE; VkQueryPool queries=VK_NULL_HANDLE;
    PFN_vkDestroyQueryPool destroy_query=nullptr;
    ~BenchOwner() { clear(); }
    bool clear() noexcept {
        // App shutdown is the final fallback owner if GPU completion is unknown.
        if(!a.device) return cases.empty();
        if(a.submission_pending) {
            const auto idle=a.DeviceWaitIdle(a.device);
            if(idle!=VK_SUCCESS && idle!=VK_ERROR_DEVICE_LOST) return false;
        }
        a.submission_pending=false;
        if(a.command) a.FreeCommandBuffers(a.device,a.pool,1,&a.command);
        a.command=VK_NULL_HANDLE;
        if(queries) destroy_query(a.device,std::exchange(queries,VK_NULL_HANDLE),nullptr);
        if(pool) a.DestroyDescriptorPool(a.device,std::exchange(pool,VK_NULL_HANDLE),nullptr);
        for(auto &c:cases) { if(c.view) a.DestroyImageView(a.device,c.view,nullptr); a.destroy(c.input); a.destroy(c.output); }
        cases.clear();
        for(size_t i=0;i<3;++i) { if(images[i]) a.DestroyImage(a.device,std::exchange(images[i],VK_NULL_HANDLE),nullptr); if(memories[i]) a.FreeMemory(a.device,std::exchange(memories[i],VK_NULL_HANDLE),nullptr); }
        return true;
    }
};
void bench_prepare(BenchOwner &b,uint32_t size,uint32_t format_mask) {
    App &a=b.a; uint32_t levels=1; for(uint32_t n=size;n>1;n>>=1) ++levels;
    b.cases.reserve(levels*3);
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,levels*3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,levels*3}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=levels*3; pi.poolSizeCount=2; pi.pPoolSizes=ps;
    require(a.CreateDescriptorPool(a.device,&pi,nullptr,&b.pool),"Benchmark pool");
    constexpr VkFormat formats[3]={VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC7_UNORM_BLOCK};
    uint32_t seed=0x013bc713;
    for(uint32_t fi=0;fi<3;++fi) {
        if(!(format_mask&(1u<<fi))) continue;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=formats[fi]; ii.extent={size,size,1}; ii.mipLevels=levels; ii.arrayLayers=2; ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        require(a.CreateImage(a.device,&ii,nullptr,&b.images[fi]),"Benchmark image");
        VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,b.images[fi],&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0);
        require(a.AllocateMemory(a.device,&ai,nullptr,&b.memories[fi]),"Benchmark image memory"); require(a.BindImageMemory(a.device,b.images[fi],b.memories[fi],0),"Benchmark bind image");
        for(uint32_t mip=0;mip<levels;++mip) {
            b.cases.push_back({formats[fi],std::max(1u,size>>mip),std::max(1u,size>>mip),mip}); auto &c=b.cases.back();
            c.region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,2}; c.region.imageExtent={c.width,c.height,1};
            const uint32_t br=(c.width+3)/4,bh=(c.height+3)/4,bs=pvb::block_bytes(c.format);
            c.input=a.buffer(uint64_t(br)*bh*2*bs,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            c.output=a.buffer(uint64_t(c.width)*c.height*2*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); c.expected.resize(size_t(c.width)*c.height*2*4);
            auto *raw=static_cast<uint8_t*>(c.input.mapped);
            for(uint32_t layer=0;layer<2;++layer) for(uint32_t y=0;y<bh;++y) for(uint32_t x=0;x<br;++x) {
                auto *block=raw+((layer*bh+y)*br+x)*bs;
                for(uint32_t n=0;n<bs;n+=4) write32(block+n,random_word(seed));
                std::array<uint8_t,64> decoded{};
                if(fi==2) { // Cycle all eight BC7 modes; copy to oracle-aligned storage.
                    const uint32_t mode=(x+y+layer+mip)%8; block[0]=uint8_t((block[0]&~((1u<<(mode+1))-1u))|(1u<<mode));
                    alignas(8) std::array<uint8_t,16> aligned{}; std::memcpy(aligned.data(),block,16); bcdec_bc7(aligned.data(),decoded.data(),16);
                } else decoded=reference(c.format,block);
                for(uint32_t py=0;py<4;++py) for(uint32_t px=0;px<4;++px) if(x*4+px<c.width && y*4+py<c.height)
                    std::memcpy(c.expected.data()+((size_t(layer)*c.height+y*4+py)*c.width+x*4+px)*4,decoded.data()+(py*4+px)*4,4);
            }
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=b.images[fi]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format=c.format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,2}; require(a.CreateImageView(a.device,&vi,nullptr,&c.view),"Benchmark view");
            VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=b.pool; ds.descriptorSetCount=1; ds.pSetLayouts=&a.descriptors; require(a.AllocateDescriptorSets(a.device,&ds,&c.set),"Benchmark descriptor");
            VkDescriptorImageInfo image{a.sampler,c.view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buffer{c.output.handle,0,c.expected.size()}; VkWriteDescriptorSet writes[2]{};
            for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=c.set; writes[n].dstBinding=n; writes[n].descriptorCount=1; }
            writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&image; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&buffer; a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
        }
    }
    a.begin();
    for(auto image:b.images) {
        if(!image) continue;
        VkImageMemoryBarrier v{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; v.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; v.newLayout=VK_IMAGE_LAYOUT_GENERAL; v.image=image; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,2}; v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; v.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&v);
    }
    a.submit();
}
void bench_barrier(App &a,VkImage image,const BenchCase &c,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to) {
    VkImageMemoryBarrier v{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; v.oldLayout=v.newLayout=VK_IMAGE_LAYOUT_GENERAL; v.image=image; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,c.mip,1,0,2}; v.srcQueueFamilyIndex=v.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; v.srcAccessMask=src; v.dstAccessMask=dst;
    a.CmdPipelineBarrier(a.command,from,to,0,0,nullptr,0,nullptr,1,&v);
}
void bench_history(App &a,const BenchCase &c,uint32_t history) {
    const uint32_t push[8]={c.width,c.height,2,0,0,0,0,0};
    a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.pipeline);
    for(uint32_t n=0;n<history;++n) { a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&c.set,0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push); }
}
void bench_check(BenchOwner &b) {
    auto &a=b.a; require(a.ResetCommandBuffer(a.command,0),"Benchmark correctness reset"); a.begin();
    for(auto &c:b.cases) {
        const uint32_t fi=c.format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK?0:c.format==VK_FORMAT_BC3_UNORM_BLOCK?1:2;
        bench_barrier(a,b.images[fi],c,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        bench_history(a,c,1); a.CmdDispatch(a.command,(c.width+7)/8,(c.height+7)/8,2);
        VkBufferMemoryBarrier h{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; h.buffer=c.output.handle; h.size=VK_WHOLE_SIZE; h.srcQueueFamilyIndex=h.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; h.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&h,0,nullptr);
    }
    a.submit();
    for(auto &c:b.cases) {
        const auto *actual=static_cast<const uint8_t*>(c.output.mapped); bool equal=true;
        for(size_t n=0;n<c.expected.size();++n) if(std::abs(int(actual[n])-int(c.expected[n]))>1) { equal=false; std::fprintf(stderr,"BENCH_MISMATCH format=%u mip=%u byte=%zu got=%u expected=%u\n",uint32_t(c.format),c.mip,n,actual[n],c.expected[n]); break; }
        check(equal,"Benchmark independent sampled pixels outside timing");
    }
    if(failed) throw std::runtime_error("Benchmark correctness failed");
}
}
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc<2) { std::fprintf(stderr,"Usage: pixel_bc_bench explicit-library.so [--warmup N] [--repeats N] [--size 16..512 power-of-two] [--history 1..128] [--format all|bc1|bc3|bc7] [--validation]\n"); return 2; }
    uint32_t warmup=3,repeats=9,size=256,history=8,format_mask=7; bool validation=false;
    try {
        for(int n=2;n<argc;++n) {
            if(!std::strcmp(argv[n],"--validation")) { validation=true; continue; }
            if(!std::strcmp(argv[n],"--format") && n+1<argc) {
                const auto f=std::string(argv[++n]); format_mask=f=="all"?7:f=="bc1"?1:f=="bc3"?2:f=="bc7"?4:0;
                if(!format_mask) throw std::runtime_error("Invalid format");
                continue;
            }
            if(n+1>=argc) throw std::runtime_error("Missing option value");
            const auto option=std::string(argv[n]); char *end=nullptr; auto value=std::strtoul(argv[++n],&end,10); if(!end||*end||value>512) throw std::runtime_error("Invalid bounded option");
            if(option=="--warmup") warmup=uint32_t(value); else if(option=="--repeats") repeats=uint32_t(value); else if(option=="--size") size=uint32_t(value); else if(option=="--history") history=uint32_t(value); else throw std::runtime_error("Unknown option");
        }
        if(warmup>30||!repeats||repeats>60||size<16||size>512||(size&(size-1))||!history||history>128) throw std::runtime_error("Option outside bounds");
        App a; init(a,argv[1],validation,false); BenchOwner b{a}; bench_prepare(b,size,format_mask);
        auto perf=reinterpret_cast<PvbGetPerfStats>(dlsym(a.library,"pvb_get_perf_stats"));
        uint32_t count=0; a.GetPhysicalDeviceQueueFamilyProperties(a.physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); a.GetPhysicalDeviceQueueFamilyProperties(a.physical,&count,families.data());
        const uint32_t bits=families[a.family].timestampValidBits; VkPhysicalDeviceProperties props{}; a.GetPhysicalDeviceProperties(a.physical,&props);
        auto reset_query=a.dp<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool"); auto timestamp=a.dp<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp"); auto query_results=a.dp<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults");
        const bool timing=bits>0 && props.limits.timestampPeriod>0;
        if(timing) { b.destroy_query=a.dp<PFN_vkDestroyQueryPool>("vkDestroyQueryPool"); VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType=VK_QUERY_TYPE_TIMESTAMP; qi.queryCount=2; require(a.dp<PFN_vkCreateQueryPool>("vkCreateQueryPool")(a.device,&qi,nullptr,&b.queries),"Benchmark timestamp query pool"); }
        std::printf("BENCH_CONFIG {\"format_mask\":%u,\"size\":%u,\"layers\":2,\"regions\":%zu,\"history\":%u,\"warmups\":%u,\"repeats\":%u,\"timestamp_bits\":%u,\"timestamp_period_ns\":%.9g,\"gpu_timing_supported\":%s,\"allocation_counters_supported\":%s,\"validation\":%s}\n",format_mask,size,b.cases.size(),history,warmup,repeats,bits,double(props.limits.timestampPeriod),timing?"true":"false",perf?"true":"false",validation?"true":"false");
        // Timed window includes buffer transfer + BC compute + RGBA image copy;
        // pure decode-kernel duration is not isolated. CPU excludes submission/wait.
        for(uint32_t iteration=0;iteration<warmup+repeats;++iteration) {
            PvbPerfStats before{},after{}; if(perf) perf(&before);
            const auto start=Clock::now(); require(a.ResetCommandBuffer(a.command,0),"Benchmark reset"); const auto reset_done=Clock::now(); a.begin();
            if(timing) { reset_query(a.command,b.queries,0,2); timestamp(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,b.queries,0); }
            for(const auto &c:b.cases) {
                const uint32_t fi=c.format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK?0:c.format==VK_FORMAT_BC3_UNORM_BLOCK?1:2;
                bench_barrier(a,b.images[fi],c,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                bench_history(a,c,history); a.CmdCopyBufferToImage(a.command,c.input.handle,b.images[fi],VK_IMAGE_LAYOUT_GENERAL,1,&c.region);
            }
            if(timing) timestamp(a.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,b.queries,1);
            require(a.EndCommandBuffer(a.command),"Benchmark end"); const auto recorded=Clock::now(); if(perf) perf(&after);
            a.submit_ended(); double gpu=-1;
            if(timing) { uint64_t ticks[2]{}; require(query_results(a.device,b.queries,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"Benchmark timestamps"); const uint64_t mask=bits==64?UINT64_MAX:(uint64_t(1)<<bits)-1; gpu=double((ticks[1]-ticks[0])&mask)*double(props.limits.timestampPeriod)/1e6; }
            std::printf("BENCH_SAMPLE {\"iteration\":%u,\"warmup\":%s,\"cpu_reset_ms\":%.6f,\"cpu_record_ms\":%.6f,\"gpu_upload_decode_transfer_ms\":",iteration,iteration<warmup?"true":"false",ms(start,reset_done),ms(reset_done,recorded));
            if(timing) std::printf("%.6f",gpu); else std::printf("null");
            if(perf) std::printf(",\"buffer_create_calls\":%llu,\"memory_allocate_calls\":%llu,\"descriptor_pool_create_calls\":%llu,\"requested_buffer_bytes\":%llu,\"replayed_binds\":%llu,\"replayed_pushes\":%llu",(unsigned long long)(after.buffer_create_calls-before.buffer_create_calls),(unsigned long long)(after.memory_allocate_calls-before.memory_allocate_calls),(unsigned long long)(after.descriptor_pool_create_calls-before.descriptor_pool_create_calls),(unsigned long long)(after.requested_buffer_bytes-before.requested_buffer_bytes),(unsigned long long)(after.replayed_binds-before.replayed_binds),(unsigned long long)(after.replayed_pushes-before.replayed_pushes));
            else std::printf(",\"allocation_counters\":null");
            std::printf("}\n");
            if(iteration==0) { // Exercise ended replay before reset/reuse.
                a.submit_ended(); bench_check(b);
            }
        }
        bench_check(b); require(a.ResetCommandBuffer(a.command,0),"Benchmark final reset");
        PvbStats s{}; a.stats(&s); check(!s.live_upload_resources,"Benchmark upload cleanup");
        check(b.clear(),"Benchmark native resources safely released");
        auto transfer=reinterpret_cast<PvbGetTransferStats>(dlsym(a.library,"pvb_get_transfer_stats"));
        if(transfer) { PvbTransferStats t{}; transfer(&t); check(!t.live_block_stores && !t.live_block_bytes,"Benchmark original-block stores released"); }
        a.shutdown(); // Include cleanup failures in the exit code and result.
        std::printf("BENCH_RESULT {\"passed\":%u,\"failed\":%u,\"validation_errors\":%u,\"pure_kernel_timing_supported\":false,\"gameplay_fps_measured\":false}\n",passed,failed,validation_errors);
        return failed||validation_errors?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"BENCH_ERROR: %s\n",e.what()); return 1; }
}
