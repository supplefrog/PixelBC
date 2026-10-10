// SPDX-License-Identifier: MIT
// Actual wrapper entry points against deterministic native format/creation mocks.
#include "../src/wrapper.cpp"
namespace {
unsigned passed=0,failed=0,creates=0,destroys=0;
uint64_t sequence=400;
bool native_failure=false,missing_filter=false,reject_mutable=false;
uint32_t native_extent=1024,native_mips=11;
VkFormat last_created=VK_FORMAT_UNDEFINED,last_view=VK_FORMAT_UNDEFINED;
unsigned codec_fault=0,shader_calls=0,compute_calls=0,live_shaders=0,live_pipelines=0;
void check(bool c,const char *n) { if(c) ++passed; else { ++failed; std::fprintf(stderr,"FAIL: %s\n",n); } }
VKAPI_ATTR void VKAPI_CALL format_properties(VkPhysicalDevice,VkFormat f,VkFormatProperties *out) {
    *out={}; out->optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if(missing_filter && pvb::is_etc2_backing(f)) out->optimalTilingFeatures&=~VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL image_properties_mock(VkPhysicalDevice,VkFormat,VkImageType,VkImageTiling,VkImageUsageFlags,VkImageCreateFlags flags,VkImageFormatProperties *out) {
    if(reject_mutable && (flags&VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    *out={{native_extent,native_extent,1},native_mips,12,VK_SAMPLE_COUNT_1_BIT,1u<<28}; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice,const VkImageCreateInfo *info,const VkAllocationCallbacks*,VkImage *out) {
    ++creates; last_created=info->format;
    if(native_failure) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=reinterpret_cast<VkImage>(++sequence); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice,VkImage,const VkAllocationCallbacks*) { ++destroys; }
VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice,const VkImageViewCreateInfo *info,const VkAllocationCallbacks*,VkImageView *out) { last_view=info->format; *out=reinterpret_cast<VkImageView>(++sequence); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL memory_requirements(VkDevice,const VkDeviceImageMemoryRequirements *info,VkMemoryRequirements2 *out) { out->memoryRequirements={uint64_t(info->pCreateInfo->format)*1024,16,1}; }
VKAPI_ATTR VkResult VKAPI_CALL descriptor_layout(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout *out) { *out=reinterpret_cast<VkDescriptorSetLayout>(++sequence); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL pipeline_layout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout *out) { *out=reinterpret_cast<VkPipelineLayout>(++sequence); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL shader_module(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule *out) {
    ++shader_calls;
    if(codec_fault%2==1 && shader_calls==2+(codec_fault-1)/2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=reinterpret_cast<VkShaderModule>(++sequence); ++live_shaders; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_shader(VkDevice,VkShaderModule,const VkAllocationCallbacks*) { --live_shaders; }
VKAPI_ATTR VkResult VKAPI_CALL compute_pipeline(VkDevice,VkPipelineCache,uint32_t,const VkComputePipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline *out) {
    *out=reinterpret_cast<VkPipeline>(++sequence); ++live_pipelines;
    ++compute_calls;
    return codec_fault && codec_fault%2==0 && compute_calls==2+(codec_fault-2)/2?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pipeline_mock(VkDevice,VkPipeline,const VkAllocationCallbacks*) { --live_pipelines; }
VKAPI_ATTR void VKAPI_CALL destroy_pipeline_layout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroy_descriptor_layout(VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*) {}
}
int main() {
    auto i=std::make_shared<Instance>(); i->GetPhysicalDeviceFormatProperties=format_properties; i->GetPhysicalDeviceImageFormatProperties=image_properties_mock;
    auto d=std::make_shared<Device>(); d->instance=i; d->handle=reinterpret_cast<VkDevice>(1); d->physical=reinterpret_cast<VkPhysicalDevice>(2);
    d->CreateImage=create_image; d->DestroyImage=destroy_image; d->CreateImageView=create_view; d->GetDeviceImageMemoryRequirements=memory_requirements;
    devices[d->handle]=d; d->native_etc2=true;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK; ci.extent={7,5,1}; ci.mipLevels=3; ci.arrayLayers=6; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    for(uint32_t f=131;f<=146;++f) {
        if(!pvb::is_bc(VkFormat(f))) continue;
        ci.format=VkFormat(f); VkImage image=VK_NULL_HANDLE;
        VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS}; query.pCreateInfo=&ci;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; pvbGetDeviceImageMemoryRequirements(d->handle,&query,&req);
        const auto expected=PVB_ETC2 && pvb::etc2_eligible(ci.format)?pvb::etc2_format(ci.format):pvb::rgba_format(ci.format);
        check(req.memoryRequirements.size==uint64_t(expected)*1024,"No-image query selects expected backing");
        check(pvbCreateImage(d->handle,&ci,nullptr,&image)==VK_SUCCESS && last_created==expected,"Actual create selects expected same backing");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.format=ci.format; vi.viewType=VK_IMAGE_VIEW_TYPE_CUBE; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,3,0,6}; VkImageView view{};
        check(pvbCreateImageView(d->handle,&vi,nullptr,&view)==VK_SUCCESS && last_view==expected,"View maps stored backing, cube and colorspace");
        PvbCompressionStats stats{}; pvb_get_compression_stats(&stats);
        check(stats.live_etc2_images+stats.live_rgba_images==1,"Residency accounting records exactly one live image");
        pvbDestroyImage(d->handle,image,nullptr);
    }
    ci.format=VK_FORMAT_BC3_UNORM_BLOCK;
    for(unsigned mode=0;mode<6;++mode) {
        auto altered=ci; d->native_etc2=true; missing_filter=false; reject_mutable=false;
        if(mode==0) d->native_etc2=false;
        if(mode==1) missing_filter=true;
        if(mode==2) altered.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(mode==3) altered.extent.width=2048;
        if(mode==4) altered.arrayLayers=24;
        if(mode==5) { altered.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; reject_mutable=true; }
        check(select_backing(*d,altered)==pvb::rgba_format(altered.format),"Unsupported feature/filter/usage/extent/layers/flags safely chooses RGBA");
    }
    missing_filter=false; reject_mutable=false; d->native_etc2=true;
    // Literal policy boundaries, through the actual create/query/view entry
    // points. Native limits deliberately admit both measured shapes: a native
    // extent fallback must not accidentally make a missing policy pass.
    native_extent=8192; native_mips=14;
    struct Shape { uint32_t w,h; bool lossless; };
    for(const auto shape:{Shape{512,1024,true},Shape{256,2048,true},Shape{511,1024,false},Shape{512,1023,false},Shape{512,1025,false},Shape{257,2048,false},Shape{256,2047,false},Shape{256,2049,false},Shape{1024,512,false},Shape{2048,256,false}}) {
        for(const auto format:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK,VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK}) {
            auto shaped=ci; shaped.format=format; shaped.extent={shape.w,shape.h,1}; shaped.mipLevels=12; shaped.arrayLayers=2; shaped.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            const VkFormat pair=pvb::rgba_format(format)==VK_FORMAT_R8G8B8A8_SRGB?VkFormat(uint32_t(format)-1):VkFormat(uint32_t(format)+1);
            const VkFormat views[]={format,pair}; VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; list.viewFormatCount=2; list.pViewFormats=views; shaped.pNext=&list;
            const bool selected=PVB_BC1_SHAPE_RGBA && shape.lossless && (format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format==VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
            const auto expected=PVB_ETC2 && !selected?pvb::etc2_format(format):pvb::rgba_format(format);
            VkDeviceImageMemoryRequirements q{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS}; q.pCreateInfo=&shaped;
            VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; pvbGetDeviceImageMemoryRequirements(d->handle,&q,&requirements);
            VkImage image{}; check(pvbCreateImage(d->handle,&shaped,nullptr,&image)==VK_SUCCESS && last_created==expected,"Shape matrix creates the literal expected backing");
            check(requirements.memoryRequirements.size==uint64_t(expected)*1024,"Shape no-image query agrees with actual creation");
            d->native_etc2=false; // Views must use stored backing, not reselect it.
            VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image=image; v.format=pair; v.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,12,0,2}; VkImageView view{};
            const auto expected_view=pvb::is_etc2_backing(expected)?pvb::etc2_format(pair):pvb::rgba_format(pair);
            check(pvbCreateImageView(d->handle,&v,nullptr,&view)==VK_SUCCESS && last_view==expected_view,"Mutable colorspace views preserve immutable shape backing");
            d->native_etc2=true; pvbDestroyImage(d->handle,image,nullptr);
        }
    }
    native_extent=1024; native_mips=11;
    auto invalid=ci; invalid.usage|=VK_IMAGE_USAGE_STORAGE_BIT; VkImage out{};
    check(pvbCreateImage(d->handle,&invalid,nullptr,&out)==VK_ERROR_FORMAT_NOT_SUPPORTED && !out,"Unsupported storage usage is rejected, not silently accepted");
    VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS}; query.pCreateInfo=&invalid;
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; pvbGetDeviceImageMemoryRequirements(d->handle,&query,&req);
    check(!req.memoryRequirements.size,"Unsupported create-without-image query is empty");
    native_failure=true; const auto before=creates;
    check(pvbCreateImage(d->handle,&ci,nullptr,&out)==VK_ERROR_OUT_OF_DEVICE_MEMORY && !out && creates==before+1 && images.empty(),"Native creation failure keeps no owner and does not retry another backing");
    native_failure=false;
    uint64_t bytes=0;
    check(pvb::etc2_output_size(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,1,1,6,bytes) && bytes==96,"Tiny cube mips retain complete native alpha blocks");
    check(!pvb::etc2_output_size(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,UINT32_MAX,UINT32_MAX,UINT32_MAX,bytes),"Oversized encoded output is rejected with checked arithmetic");
    PvbCompressionStats final{}; pvb_get_compression_stats(&final);
    check(!final.live_etc2_images && !final.live_rgba_images && !final.live_etc2_logical_bytes && !final.live_rgba_logical_bytes && creates==destroys+1,"Residency/native creation cleanup is balanced");
    devices.clear();
    // Both optional format-specialized pipelines must be ready before backing
    // selection. Inject failures in either shader or partially returned pipeline.
    for(codec_fault=0;codec_fault<5;++codec_fault) {
        Device codec{}; codec.handle=d->handle; codec.native_etc2=true;
        codec.CreateDescriptorSetLayout=descriptor_layout; codec.CreatePipelineLayout=pipeline_layout; codec.CreateShaderModule=shader_module;
        codec.CreateComputePipelines=compute_pipeline; codec.DestroyShaderModule=destroy_shader; codec.DestroyPipeline=destroy_pipeline_mock;
        codec.DestroyPipelineLayout=destroy_pipeline_layout; codec.DestroyDescriptorSetLayout=destroy_descriptor_layout;
        shader_calls=compute_calls=0;
        check(init_pipeline(codec)==VK_SUCCESS && codec.pipeline && codec.native_etc2==(codec_fault==0),"Optional encoder failure preserves lossless device route before image queries");
        check(!live_shaders && live_pipelines==(codec_fault==0?3u:1u),"Both optional shaders/partial-pipelines roll back to the one lossless pipeline");
        destroy_pipeline(codec);
        check(!live_pipelines,"All specialized codec and lossless pipelines are released");
    }
    std::printf("ETC2_CONTRACT_RESULT: %u passed; %u failed\n",passed,failed); return failed?1:0;
}
