// SPDX-License-Identifier: MIT
#include "bc_transfer_layout.h"
#include "bc_decode_spv.h"
#include "bc_etc2_encode_bc1_spv.h"
#include "bc_etc2_encode_bc3_spv.h"
#include "etc2_formats.h"
#include "compression_stats.h"
#include "stats.h"
#include "transfer_stats.h"
#include "perf_stats.h"
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#include "android_hal.h"
#endif
#define EXPORT __attribute__((visibility("default")))
#ifndef PVB_UPLOAD_ARENAS
#define PVB_UPLOAD_ARENAS 1
#endif
#ifndef PVB_COMPACT_HISTORY
#define PVB_COMPACT_HISTORY 1
#endif
#ifndef PVB_ETC2
#define PVB_ETC2 0
#endif
#ifdef __ANDROID__
extern "C" EXPORT hwvulkan_module_t HMI;
#endif
#define GLOBAL_FUNCS(X) \
 X(CreateInstance) X(EnumerateInstanceExtensionProperties) X(EnumerateInstanceLayerProperties)
#define INSTANCE_FUNCS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(EnumeratePhysicalDeviceGroups) \
 X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFeatures2) \
 X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceFormatProperties2) \
 X(GetPhysicalDeviceImageFormatProperties) X(GetPhysicalDeviceImageFormatProperties2) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(EnumerateDeviceExtensionProperties) X(CreateDevice)
#define DEVICE_FUNCS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateImage) X(DestroyImage) X(CreateImageView) \
 X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) \
 X(CmdCopyImage) X(CmdCopyImage2) X(CmdCopyImageToBuffer) X(CmdCopyBuffer) X(CmdCopyBufferToImage) X(CmdPipelineBarrier) X(CmdPipelineBarrier2) X(CmdWaitEvents) X(CmdWaitEvents2) X(CmdSetEvent2) X(CmdDispatch) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(QueueSubmit) X(QueueSubmit2) X(GetDeviceImageMemoryRequirements)
namespace {
void log_error(const char *message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR,"PixelBC","%s",message);
#else
    std::fprintf(stderr,"PixelBC: %s\n",message);
#endif
}
template<class Action> struct Rollback {
    Action action;
    bool active=true;
    ~Rollback() { if(active) action(); }
};
template<class Action> Rollback<Action> rollback(Action action) { return {action}; }
struct Backend {
    void *library = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkEnumerateInstanceVersion version = nullptr;
#define DECLARE(name) PFN_vk##name name = nullptr;
    GLOBAL_FUNCS(DECLARE)
#undef DECLARE
    std::string path;
#ifdef __ANDROID__
    hw_device_t *hal_device = nullptr;
    // Android's built-in Vulkan loader retains its HAL and DSO for process life.
    // Keep our owned stock-driver reference too: Vulkan objects still have their
    // normal explicit destruction, without invoking vendor unload at wrapper exit.
    // This policy contains an unlocalized teardown crash; it is not its diagnosis.
#endif
};
Backend backend;
std::once_flag backend_once;
std::atomic<uint64_t> dispatches{0}, failures{0}, live_resources{0};
std::atomic<uint64_t> live_block_stores{0}, live_block_bytes{0}, block_copy_bytes{0};
std::atomic<uint32_t> accepted_image_formats{0}, rejected_image_formats{0};
std::atomic<uint64_t> backend_recursion{0};
std::atomic<uint64_t> perf_buffers{0},perf_memory{0},perf_pools{0},perf_bytes{0},perf_binds{0},perf_pushes{0};
std::atomic<uint64_t> etc2_dispatches{0},rgba_dispatches{0},cross_backing_regenerations{0};
struct Instance {
    VkInstance handle;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
#define DECLARE(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCS(DECLARE)
#undef DECLARE
};
struct Device {
    VkDevice handle;
    std::shared_ptr<Instance> instance;
    VkPhysicalDevice physical;
    PFN_vkGetDeviceProcAddr gdpa;
#define DECLARE(name) PFN_vk##name name = nullptr;
    DEVICE_FUNCS(DECLARE)
#undef DECLARE
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    std::vector<VkQueueFamilyProperties> families;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline etc2_pipelines[2] = {VK_NULL_HANDLE,VK_NULL_HANDLE};
    bool native_etc2=false;
};
struct BlockStore {
    std::shared_ptr<Device> device;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    std::vector<pvb::BlockMip> mips;
    uint64_t bytes=0;
    ~BlockStore() {
        if(buffer) device->DestroyBuffer(device->handle,buffer,nullptr);
        if(memory) device->FreeMemory(device->handle,memory,nullptr);
        if(bytes) { --live_block_stores; live_block_bytes-=bytes; }
    }
};
struct Image { VkFormat original; VkExtent3D extent; uint32_t mips, layers; std::shared_ptr<Device> device; std::shared_ptr<BlockStore> blocks; VkImageUsageFlags usage=0; VkFormat backing=VK_FORMAT_UNDEFINED; };
struct Pool { std::shared_ptr<Device> device; uint32_t family; };
struct Resource {
    VkBuffer input = VK_NULL_HANDLE, output = VK_NULL_HANDLE;
    VkDeviceMemory input_memory = VK_NULL_HANDLE, output_memory = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    bool borrowed=false;
};
struct Arena {
    Resource backing;
    uint64_t input_size=0,output_size=0,input_used=0,output_used=0;
    uint32_t sets=0;
};
struct ShadowLayout {
    std::shared_ptr<Device> device;
    VkPipelineLayout handle=VK_NULL_HANDLE;
    ~ShadowLayout() { if(handle) device->DestroyPipelineLayout(device->handle,handle,nullptr); }
};
struct DescriptorBind { std::shared_ptr<ShadowLayout> layout; uint32_t first; std::vector<VkDescriptorSet> sets; std::vector<uint32_t> offsets; };
struct Push { std::shared_ptr<ShadowLayout> layout; VkShaderStageFlags stages; uint32_t offset; std::vector<uint8_t> data; };
struct Command {
    std::shared_ptr<Device> device;
    VkCommandPool pool;
    VkCommandBufferLevel level;
    VkResult error = VK_SUCCESS;
    VkPipeline compute = VK_NULL_HANDLE;
    std::vector<DescriptorBind> binds;
    std::vector<Push> pushes;
    std::vector<Resource> resources;
    std::vector<Arena> arenas;
    std::vector<std::shared_ptr<BlockStore>> stores;
};
std::mutex maps_mutex;
std::unordered_map<VkInstance,std::shared_ptr<Instance>> instances;
std::unordered_map<VkPhysicalDevice,std::shared_ptr<Instance>> physicals;
std::unordered_map<VkDevice,std::shared_ptr<Device>> devices;
std::unordered_map<VkQueue,std::shared_ptr<Device>> queues;
std::unordered_map<VkCommandPool,Pool> pools;
std::unordered_map<VkCommandBuffer,std::shared_ptr<Command>> commands;
std::unordered_map<VkImage,Image> images;
std::unordered_map<VkPipelineLayout,std::shared_ptr<ShadowLayout>> layouts;
template<class K,class V> V lookup(const std::unordered_map<K,V> &map,K key) {
    std::lock_guard<std::mutex> lock(maps_mutex);
    auto found=map.find(key); return found==map.end()?V{}:found->second;
}
template<class T> T resolve(PFN_vkGetInstanceProcAddr fn,VkInstance i,const char *name) { return reinterpret_cast<T>(fn(i,name)); }
void destroy_resource(Device &d,Resource &r) {
    if(r.borrowed) { r={}; return; }
    if(r.descriptors) d.DestroyDescriptorPool(d.handle,r.descriptors,nullptr);
    if(r.input) d.DestroyBuffer(d.handle,r.input,nullptr);
    if(r.output) d.DestroyBuffer(d.handle,r.output,nullptr);
    if(r.input_memory) d.FreeMemory(d.handle,r.input_memory,nullptr);
    if(r.output_memory) d.FreeMemory(d.handle,r.output_memory,nullptr);
    r={};
}
void reset_command(Command &c) {
    for(auto &r:c.resources) { destroy_resource(*c.device,r); --live_resources; }
    for(auto &a:c.arenas) destroy_resource(*c.device,a.backing);
    c.arenas.clear();
    c.resources.clear(); c.stores.clear(); c.binds.clear(); c.pushes.clear(); c.compute=VK_NULL_HANDLE; c.error=VK_SUCCESS;
}
void fail(Command &c,VkResult result,const char *message) {
    if(c.error==VK_SUCCESS) { c.error=result; ++failures; log_error(message); }
}
void log_image_request(const VkImageCreateInfo &info,bool accepted) {
    // At most one accepted and one rejected shape per BC format/process. No
    // asset names or texture bytes are inspected or logged.
    const uint32_t bit=1u<<(uint32_t(info.format)-uint32_t(VK_FORMAT_BC1_RGB_UNORM_BLOCK));
    auto &seen=accepted?accepted_image_formats:rejected_image_formats;
    if(seen.fetch_or(bit)&bit) return;
#ifdef __ANDROID__
    __android_log_print(accepted?ANDROID_LOG_INFO:ANDROID_LOG_ERROR,"PixelBC","BC image %s: format=%u extent=%ux%ux%u mips=%u layers=%u usage=0x%x type=%u tiling=%u flags=0x%x samples=0x%x",accepted?"accepted":"unsupported request",uint32_t(info.format),info.extent.width,info.extent.height,info.extent.depth,info.mipLevels,info.arrayLayers,info.usage,uint32_t(info.imageType),uint32_t(info.tiling),info.flags,uint32_t(info.samples));
#else
    std::fprintf(stderr,"PixelBC: BC image %s: format=%u extent=%ux%ux%u mips=%u layers=%u usage=0x%x type=%u tiling=%u flags=0x%x samples=0x%x\n",accepted?"accepted":"unsupported request",uint32_t(info.format),info.extent.width,info.extent.height,info.extent.depth,info.mipLevels,info.arrayLayers,info.usage,uint32_t(info.imageType),uint32_t(info.tiling),info.flags,uint32_t(info.samples));
#endif
}
VkResult reject_image_request(const VkImageCreateInfo &info) { log_image_request(info,false); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
bool blocked_extension(const char *name) {
    static const char *blocked[]={"VK_EXT_host_image_copy","VK_KHR_maintenance5","VK_KHR_maintenance6","VK_KHR_push_descriptor","VK_EXT_descriptor_buffer","VK_EXT_shader_object","VK_NV_device_generated_commands","VK_NV_device_generated_commands_compute","VK_EXT_device_generated_commands"};
    for(auto b:blocked) if(!std::strcmp(name,b)) return true;
    return false;
}
void clear_unsupported_features(void *chain) {
    for(auto p=static_cast<VkBaseOutStructure*>(chain);p;p=p->pNext) {
        switch(p->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
            reinterpret_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(p)->protectedMemory=VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(p)->protectedMemory=VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES: {
            auto f=reinterpret_cast<VkPhysicalDeviceVulkan14Features*>(p);
            f->maintenance5=f->maintenance6=f->hostImageCopy=f->pushDescriptor=VK_FALSE; break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT:
            reinterpret_cast<VkPhysicalDeviceHostImageCopyFeaturesEXT*>(p)->hostImageCopy=VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceMaintenance5FeaturesKHR*>(p)->maintenance5=VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceMaintenance6FeaturesKHR*>(p)->maintenance6=VK_FALSE; break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT: {
            auto f=reinterpret_cast<VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(p);
            f->descriptorBuffer=f->descriptorBufferCaptureReplay=f->descriptorBufferImageLayoutIgnored=f->descriptorBufferPushDescriptors=VK_FALSE; break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
            reinterpret_cast<VkPhysicalDeviceShaderObjectFeaturesEXT*>(p)->shaderObject=VK_FALSE; break;
        default: break;
        }
    }
}
VkFormatFeatureFlags bc_features(Instance &i,VkPhysicalDevice p,VkFormat f) {
    if(!pvb::is_bc(f)) return 0;
    VkFormatProperties backing{}; i.GetPhysicalDeviceFormatProperties(p,pvb::rgba_format(f),&backing);
    const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if((backing.optimalTilingFeatures&required)!=required) return 0;
    return required;
}
bool supported_image(VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags) {
    constexpr VkImageUsageFlags allowed=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    constexpr VkImageCreateFlags allowed_flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT|VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    return type==VK_IMAGE_TYPE_2D && tiling==VK_IMAGE_TILING_OPTIMAL && usage && !(usage&~allowed) && !(flags&~allowed_flags);
}
VkResult image_properties(Instance &i,VkPhysicalDevice p,VkFormat f,VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties *out) {
    if(!pvb::is_bc(f)||!supported_image(type,tiling,usage,flags)||!bc_features(i,p,f)) { *out={}; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    auto r=i.GetPhysicalDeviceImageFormatProperties(p,pvb::rgba_format(f),type,tiling,usage,flags,out);
    if(r==VK_SUCCESS) out->sampleCounts&=VK_SAMPLE_COUNT_1_BIT;
    return r;
}
// Physical capabilities remain the conservative lossless RGBA contract above.
// Optional ETC2 is selected only when every actual create field fits its native
// limits; otherwise creation and no-image memory requirements both choose RGBA.
VkFormat select_backing(Device &d,const VkImageCreateInfo &info) {
    const auto recovery=pvb::rgba_format(info.format);
    // One whole-image experiment, selected before any upload is recorded.
    // Creation and maintenance4 queries share this selector; stored backing
    // continues to govern views, copies and every mip/layer for its lifetime.
    if(PVB_BC1_SHAPE_RGBA &&
       (info.format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK || info.format==VK_FORMAT_BC1_RGBA_SRGB_BLOCK) &&
       ((info.extent.width==512 && info.extent.height==1024) ||
        (info.extent.width==256 && info.extent.height==2048))) return recovery;
    if(!PVB_ETC2 || !d.native_etc2 || !pvb::etc2_eligible(info.format) ||
       !(info.usage&VK_IMAGE_USAGE_SAMPLED_BIT) || !supported_image(info.imageType,info.tiling,info.usage,info.flags) ||
       info.samples!=VK_SAMPLE_COUNT_1_BIT || !info.extent.width || !info.extent.height || info.extent.depth!=1 ||
       !info.mipLevels || !info.arrayLayers) return recovery;
    const auto target=pvb::etc2_format(info.format);
    // Both colorspaces are required so compatible mutable views have the same
    // backing family. BC4/5 signed/unsigned and BC7 never enter this route.
    constexpr auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    const auto paired=VkFormat(pvb::rgba_format(info.format)==VK_FORMAT_R8G8B8A8_SRGB?uint32_t(target)-1u:uint32_t(target)+1u);
    for(auto f:{target,paired}) {
        VkFormatProperties fp{}; d.instance->GetPhysicalDeviceFormatProperties(d.physical,f,&fp);
        if((fp.optimalTilingFeatures&required)!=required) return recovery;
        VkImageFormatProperties ip{};
        if(d.instance->GetPhysicalDeviceImageFormatProperties(d.physical,f,info.imageType,info.tiling,info.usage,info.flags,&ip)!=VK_SUCCESS ||
           info.extent.width>ip.maxExtent.width || info.extent.height>ip.maxExtent.height || info.extent.depth>ip.maxExtent.depth ||
           info.mipLevels>ip.maxMipLevels || info.arrayLayers>ip.maxArrayLayers || !(ip.sampleCounts&info.samples)) return recovery;
    }
    return target;
}
VkResult buffer(Device &d,VkDeviceSize size,VkBufferUsageFlags usage,VkBuffer &b,VkDeviceMemory &mem,const VkImageCreateInfo *sharing=nullptr) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=size; info.usage=usage; info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(sharing) { info.sharingMode=sharing->sharingMode; info.queueFamilyIndexCount=sharing->queueFamilyIndexCount; info.pQueueFamilyIndices=sharing->pQueueFamilyIndices; }
    ++perf_buffers; perf_bytes+=size;
    VkResult r=d.CreateBuffer(d.handle,&info,nullptr,&b); if(r!=VK_SUCCESS) return r;
    VkMemoryRequirements req{}; d.GetBufferMemoryRequirements(d.handle,b,&req);
    uint32_t index=UINT32_MAX;
    for(uint32_t n=0;n<d.memory.memoryTypeCount;++n) if((req.memoryTypeBits&(1u<<n)) && (d.memory.memoryTypes[n].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { index=n; break; }
    if(index==UINT32_MAX) for(uint32_t n=0;n<d.memory.memoryTypeCount;++n) if(req.memoryTypeBits&(1u<<n)) { index=n; break; }
    if(index==UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=req.size; allocation.memoryTypeIndex=index;
    ++perf_memory;
    r=d.AllocateMemory(d.handle,&allocation,nullptr,&mem); if(r!=VK_SUCCESS) return r;
    return d.BindBufferMemory(d.handle,b,mem,0);
}
VkResult init_pipeline(Device &d) {
    VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ds.bindingCount=2; ds.pBindings=bindings;
    VkResult r=d.CreateDescriptorSetLayout(d.handle,&ds,nullptr,&d.descriptors); if(r!=VK_SUCCESS) return r;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,24};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&d.descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&range;
    r=d.CreatePipelineLayout(d.handle,&pl,nullptr,&d.layout); if(r!=VK_SUCCESS) return r;
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(bc_decode_spv); sm.pCode=bc_decode_spv;
    VkShaderModule shader=VK_NULL_HANDLE; r=d.CreateShaderModule(d.handle,&sm,nullptr,&shader); if(r!=VK_SUCCESS) return r;
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=shader; cp.stage.pName="main"; cp.layout=d.layout;
    r=d.CreateComputePipelines(d.handle,VK_NULL_HANDLE,1,&cp,nullptr,&d.pipeline);
    d.DestroyShaderModule(d.handle,shader,nullptr);
    if(r==VK_SUCCESS && d.native_etc2) {
        const uint32_t *code[2]={bc_etc2_encode_bc1_spv,bc_etc2_encode_bc3_spv};
        const size_t bytes[2]={sizeof(bc_etc2_encode_bc1_spv),sizeof(bc_etc2_encode_bc3_spv)};
        for(uint32_t n=0;n<2 && r==VK_SUCCESS;++n) {
            sm.codeSize=bytes[n]; sm.pCode=code[n];
            shader=VK_NULL_HANDLE; r=d.CreateShaderModule(d.handle,&sm,nullptr,&shader);
            if(r==VK_SUCCESS) {
                cp.stage.module=shader; r=d.CreateComputePipelines(d.handle,VK_NULL_HANDLE,1,&cp,nullptr,&d.etc2_pipelines[n]);
                d.DestroyShaderModule(d.handle,shader,nullptr);
            }
        }
        if(r!=VK_SUCCESS) {
            // Native lossless pipeline already exists. An optional codec
            // pipeline failure fixes the device's route to RGBA before any
            // caller image/requirements query; never change an existing image.
            for(auto &pipeline:d.etc2_pipelines) {
                if(pipeline) d.DestroyPipeline(d.handle,pipeline,nullptr);
                pipeline=VK_NULL_HANDLE;
            }
            d.native_etc2=false;
            log_error("Optional ETC2 pipeline unavailable; using lossless RGBA for this device");
            r=VK_SUCCESS;
        }
    }
    return r;
}
void destroy_pipeline(Device &d) {
    for(auto pipeline:d.etc2_pipelines) if(pipeline) d.DestroyPipeline(d.handle,pipeline,nullptr);
    if(d.pipeline) d.DestroyPipeline(d.handle,d.pipeline,nullptr);
    if(d.layout) d.DestroyPipelineLayout(d.handle,d.layout,nullptr);
    if(d.descriptors) d.DestroyDescriptorSetLayout(d.handle,d.descriptors,nullptr);
}
}

extern "C" EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance,const char*);
extern "C" EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice,const char*);
namespace {
void initialize_backend() {
#ifdef __ANDROID__
    backend.path="/vendor/lib64/hw/vulkan.mali.so";
    backend.library=dlopen(backend.path.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!backend.library) { log_error(dlerror()); return; }
    auto module=static_cast<hw_module_t*>(dlsym(backend.library,"HMI"));
    if(!module || module==&HMI.common || module->tag!=HARDWARE_MODULE_TAG || module->module_api_version!=HWVULKAN_MODULE_API_VERSION_0_1 || module->hal_api_version!=HARDWARE_HAL_API_VERSION || !module->id || std::strcmp(module->id,"vulkan") || !module->methods || !module->methods->open) { log_error("Stock Mali HAL module unavailable"); return; }
    module->dso=backend.library;
    hw_device_t *raw=nullptr;
    if(module->methods->open(module,"vk0",&raw)!=0 || !raw) { log_error("Stock Mali HAL open failed"); return; }
    backend.hal_device=raw;
    if(raw->tag!=HARDWARE_DEVICE_TAG || raw->version!=HWVULKAN_DEVICE_API_VERSION_0_1) { log_error("Stock Mali HAL device ABI mismatch"); return; }
    auto hal=reinterpret_cast<hwvulkan_device_t*>(raw);
    if(!hal->GetInstanceProcAddr || !hal->CreateInstance || !hal->EnumerateInstanceExtensionProperties) { log_error("Stock Mali HAL callbacks missing"); return; }
    backend.gipa=hal->GetInstanceProcAddr;
    backend.CreateInstance=hal->CreateInstance;
    backend.EnumerateInstanceExtensionProperties=hal->EnumerateInstanceExtensionProperties;
#else
    const char *path=std::getenv("PVB_TEST_BACKEND");
    backend.path=path?path:"libvulkan.so.1";
    backend.library=dlopen(backend.path.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!backend.library) { log_error(dlerror()); return; }
    backend.gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(backend.library,"vkGetInstanceProcAddr"));
    if(!backend.gipa) backend.gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(backend.library,"vk_icdGetInstanceProcAddr"));
#endif
    if(!backend.gipa || backend.gipa==vkGetInstanceProcAddr) { ++backend_recursion; backend.gipa=nullptr; log_error("Refusing recursive backend"); return; }
#define LOAD(name) if(!backend.name) backend.name=resolve<PFN_vk##name>(backend.gipa,VK_NULL_HANDLE,"vk" #name);
    GLOBAL_FUNCS(LOAD)
#undef LOAD
    backend.version=resolve<PFN_vkEnumerateInstanceVersion>(backend.gipa,VK_NULL_HANDLE,"vkEnumerateInstanceVersion");
#ifdef __ANDROID__
    if(backend.CreateInstance && backend.EnumerateInstanceExtensionProperties)
        __android_log_print(ANDROID_LOG_INFO,"PixelBC","PixelBC %s activated; backend=%s; BC1-5 GPU path; core BC feature disabled; stock HAL/DSO retained for process lifetime",PVB_VERSION,backend.path.c_str());
#endif
}
bool ready() {
    try { std::call_once(backend_once,initialize_backend); }
    catch(const std::bad_alloc&) { log_error("Backend initialization host allocation failed"); return false; }
    return backend.gipa && backend.CreateInstance && backend.EnumerateInstanceExtensionProperties;
}
VKAPI_ATTR VkResult VKAPI_CALL pvbEnumerateInstanceVersion(uint32_t *version) {
    if(!ready()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    VkResult r=VK_SUCCESS; if(backend.version) r=backend.version(version); else *version=VK_API_VERSION_1_0;
    *version=std::min(*version,VK_API_VERSION_1_3); return r;
}
VKAPI_ATTR VkResult VKAPI_CALL pvbEnumerateInstanceExtensionProperties(const char *layer,uint32_t *count,VkExtensionProperties *props) {
    return ready()?backend.EnumerateInstanceExtensionProperties(layer,count,props):VK_ERROR_INCOMPATIBLE_DRIVER;
}
VKAPI_ATTR VkResult VKAPI_CALL pvbEnumerateInstanceLayerProperties(uint32_t *count,VkLayerProperties *props) {
    if(!ready()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if(backend.EnumerateInstanceLayerProperties) return backend.EnumerateInstanceLayerProperties(count,props);
    *count=0; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL pvbCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *allocator,VkInstance *out) try {
    if(!ready()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if(info->pApplicationInfo && info->pApplicationInfo->apiVersion>VK_API_VERSION_1_3) return VK_ERROR_INCOMPATIBLE_DRIVER;
    *out=VK_NULL_HANDLE;
    auto i=std::make_shared<Instance>();
    VkResult r=backend.CreateInstance(info,allocator,out); if(r!=VK_SUCCESS) return r;
    auto guard=rollback([&] { auto destroy=resolve<PFN_vkDestroyInstance>(backend.gipa,*out,"vkDestroyInstance"); if(destroy) destroy(*out,allocator); *out=VK_NULL_HANDLE; });
    i->handle=*out;
#define LOAD(name) i->name=resolve<PFN_vk##name>(backend.gipa,*out,"vk" #name);
    INSTANCE_FUNCS(LOAD)
#undef LOAD
    i->gdpa=resolve<PFN_vkGetDeviceProcAddr>(backend.gipa,*out,"vkGetDeviceProcAddr");
    if(!i->gdpa || !i->DestroyInstance || !i->EnumeratePhysicalDevices) { return VK_ERROR_INCOMPATIBLE_DRIVER; }
    std::lock_guard<std::mutex> lock(maps_mutex); instances[*out]=i; guard.active=false; return r;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR void VKAPI_CALL pvbDestroyInstance(VkInstance handle,const VkAllocationCallbacks *allocator) {
    auto i=lookup(instances,handle); if(!i) return;
    { std::lock_guard<std::mutex> lock(maps_mutex); for(auto it=physicals.begin();it!=physicals.end();) { if(it->second==i) it=physicals.erase(it); else ++it; } instances.erase(handle); }
    i->DestroyInstance(handle,allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL pvbEnumeratePhysicalDevices(VkInstance handle,uint32_t *count,VkPhysicalDevice *out) try {
    auto i=lookup(instances,handle); if(!i) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=i->EnumeratePhysicalDevices(handle,count,out);
    if(out && (r==VK_SUCCESS || r==VK_INCOMPLETE)) { std::lock_guard<std::mutex> lock(maps_mutex); for(uint32_t n=0;n<*count;++n) physicals[out[n]]=i; }
    return r;
} catch(const std::bad_alloc&) { *count=0; return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR VkResult VKAPI_CALL pvbEnumeratePhysicalDeviceGroups(VkInstance handle,uint32_t *count,VkPhysicalDeviceGroupProperties *out) try {
    auto i=lookup(instances,handle); if(!i || !i->EnumeratePhysicalDeviceGroups) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=i->EnumeratePhysicalDeviceGroups(handle,count,out);
    if(out && (r==VK_SUCCESS || r==VK_INCOMPLETE)) { std::lock_guard<std::mutex> lock(maps_mutex); for(uint32_t n=0;n<*count;++n) for(uint32_t p=0;p<out[n].physicalDeviceCount;++p) physicals[out[n].physicalDevices[p]]=i; }
    return r;
} catch(const std::bad_alloc&) { *count=0; return VK_ERROR_OUT_OF_HOST_MEMORY; }
void modify_properties(VkPhysicalDeviceProperties &p) {
    p.apiVersion=std::min(p.apiVersion,VK_API_VERSION_1_3);
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{}; std::snprintf(name,sizeof(name),"PixelBC / " PVB_VERSION " / %.230s",p.deviceName); std::memcpy(p.deviceName,name,sizeof(name));
}
VKAPI_ATTR void VKAPI_CALL pvbGetPhysicalDeviceProperties(VkPhysicalDevice p,VkPhysicalDeviceProperties *out) { auto i=lookup(physicals,p); if(i) { i->GetPhysicalDeviceProperties(p,out); modify_properties(*out); } }
VKAPI_ATTR void VKAPI_CALL pvbGetPhysicalDeviceProperties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2 *out) { auto i=lookup(physicals,p); if(i) { i->GetPhysicalDeviceProperties2(p,out); modify_properties(out->properties); } }
VKAPI_ATTR void VKAPI_CALL pvbGetPhysicalDeviceFeatures(VkPhysicalDevice p,VkPhysicalDeviceFeatures *out) { auto i=lookup(physicals,p); if(i) { i->GetPhysicalDeviceFeatures(p,out); out->textureCompressionBC=VK_FALSE; } }
VKAPI_ATTR void VKAPI_CALL pvbGetPhysicalDeviceFeatures2(VkPhysicalDevice p,VkPhysicalDeviceFeatures2 *out) { auto i=lookup(physicals,p); if(i) { i->GetPhysicalDeviceFeatures2(p,out); out->features.textureCompressionBC=VK_FALSE; clear_unsupported_features(out->pNext); } }
VKAPI_ATTR void VKAPI_CALL pvbGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,VkFormat f,VkFormatProperties *out) {
    auto i=lookup(physicals,p); if(!i) return;
    if(pvb::is_any_bc(f)) { *out={}; out->optimalTilingFeatures=bc_features(*i,p,f); } else i->GetPhysicalDeviceFormatProperties(p,f,out);
}
VKAPI_ATTR void VKAPI_CALL pvbGetPhysicalDeviceFormatProperties2(VkPhysicalDevice p,VkFormat f,VkFormatProperties2 *out) {
    auto i=lookup(physicals,p); if(!i) return;
    if(!pvb::is_any_bc(f)) { i->GetPhysicalDeviceFormatProperties2(p,f,out); return; }
    // Rejecting unsupported extension-specific BC queries is handled by ImageFormatProperties2;
    // clear all normal feature planes, including the 64-bit promoted structure.
    pvbGetPhysicalDeviceFormatProperties(p,f,&out->formatProperties);
    for(auto ext=static_cast<VkBaseOutStructure*>(out->pNext);ext;ext=ext->pNext) if(ext->sType==VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
        auto props=reinterpret_cast<VkFormatProperties3*>(ext); props->linearTilingFeatures=0; props->bufferFeatures=0; props->optimalTilingFeatures=out->formatProperties.optimalTilingFeatures;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL pvbGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice p,VkFormat f,VkImageType type,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties *out) {
    auto i=lookup(physicals,p); if(!i) return VK_ERROR_INITIALIZATION_FAILED;
    return pvb::is_any_bc(f)?image_properties(*i,p,f,type,tiling,usage,flags,out):i->GetPhysicalDeviceImageFormatProperties(p,f,type,tiling,usage,flags,out);
}
VKAPI_ATTR VkResult VKAPI_CALL pvbGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice p,const VkPhysicalDeviceImageFormatInfo2 *info,VkImageFormatProperties2 *out) {
    auto i=lookup(physicals,p); if(!i) return VK_ERROR_INITIALIZATION_FAILED;
    if(!pvb::is_any_bc(info->format)) return i->GetPhysicalDeviceImageFormatProperties2(p,info,out);
    if(info->pNext || out->pNext) { out->imageFormatProperties={}; return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    return image_properties(*i,p,info->format,info->type,info->tiling,info->usage,info->flags,&out->imageFormatProperties);
}
VKAPI_ATTR VkResult VKAPI_CALL pvbEnumerateDeviceExtensionProperties(VkPhysicalDevice p,const char *layer,uint32_t *count,VkExtensionProperties *out) try {
    auto i=lookup(physicals,p); if(!i) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t size=0; VkResult r=i->EnumerateDeviceExtensionProperties(p,layer,&size,nullptr); if(r!=VK_SUCCESS) return r;
    std::vector<VkExtensionProperties> props(size); r=i->EnumerateDeviceExtensionProperties(p,layer,&size,props.data()); if(r!=VK_SUCCESS) return r;
    props.resize(size); props.erase(std::remove_if(props.begin(),props.end(),[](const auto &x){return blocked_extension(x.extensionName);}),props.end());
    if(!out) { *count=uint32_t(props.size()); return VK_SUCCESS; }
    const uint32_t copied=std::min(*count,uint32_t(props.size())); std::copy_n(props.begin(),copied,out); *count=copied; return copied<props.size()?VK_INCOMPLETE:VK_SUCCESS;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR VkResult VKAPI_CALL pvbCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *allocator,VkDevice *out) try {
    *out=VK_NULL_HANDLE;
    auto i=lookup(physicals,physical); if(!i) return VK_ERROR_INITIALIZATION_FAILED;
    if(info->pEnabledFeatures && info->pEnabledFeatures->textureCompressionBC) return VK_ERROR_FEATURE_NOT_PRESENT;
    for(auto ext=static_cast<const VkBaseInStructure*>(info->pNext);ext;ext=ext->pNext)
        if(ext->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && reinterpret_cast<const VkPhysicalDeviceFeatures2*>(ext)->features.textureCompressionBC) return VK_ERROR_FEATURE_NOT_PRESENT;
    for(uint32_t n=0;n<info->enabledExtensionCount;++n) if(blocked_extension(info->ppEnabledExtensionNames[n])) return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto d=std::make_shared<Device>(); d->instance=i; d->physical=physical; d->gdpa=i->gdpa;
    uint32_t family_count=0; i->GetPhysicalDeviceQueueFamilyProperties(physical,&family_count,nullptr); d->families.resize(family_count); i->GetPhysicalDeviceQueueFamilyProperties(physical,&family_count,d->families.data());
    for(uint32_t n=0;n<info->queueCreateInfoCount;++n) if(info->pQueueCreateInfos[n].queueFamilyIndex>=family_count || !(d->families[info->pQueueCreateInfos[n].queueFamilyIndex].queueFlags&VK_QUEUE_COMPUTE_BIT) || info->pQueueCreateInfos[n].flags) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkDeviceCreateInfo native_info=*info;
    VkPhysicalDeviceFeatures native_features{};
    VkPhysicalDeviceFeatures2 native_features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if(PVB_ETC2) {
        i->GetPhysicalDeviceFeatures(physical,&native_features);
        const auto first=static_cast<const VkBaseInStructure*>(info->pNext);
        const VkPhysicalDeviceFeatures2 *existing=nullptr;
        for(auto ext=first;ext;ext=ext->pNext) if(ext->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) existing=reinterpret_cast<const VkPhysicalDeviceFeatures2*>(ext);
        // Enable a genuine internal compression dependency without mutating
        // caller memory. Unknown prefixes ahead of Features2 are never copied
        // by guessed struct size: that unusual chain uses the RGBA recovery.
        if(native_features.textureCompressionETC2 && (!existing || existing==reinterpret_cast<const VkPhysicalDeviceFeatures2*>(first))) {
            d->native_etc2=true;
            if(existing) { native_features2=*existing; native_features2.features.textureCompressionETC2=VK_TRUE; native_info.pNext=&native_features2; }
            else { native_features=info->pEnabledFeatures?*info->pEnabledFeatures:VkPhysicalDeviceFeatures{}; native_features.textureCompressionETC2=VK_TRUE; native_info.pEnabledFeatures=&native_features; }
        }
    }
    VkResult r=i->CreateDevice(physical,&native_info,allocator,out); if(r!=VK_SUCCESS) return r; d->handle=*out;
    auto guard=rollback([&] {
        { std::lock_guard<std::mutex> lock(maps_mutex); for(auto it=queues.begin();it!=queues.end();) { if(it->second==d) it=queues.erase(it); else ++it; } }
        destroy_pipeline(*d); if(d->DestroyDevice) d->DestroyDevice(*out,allocator); *out=VK_NULL_HANDLE;
    });
#define LOAD(name) d->name=reinterpret_cast<PFN_vk##name>(d->gdpa(*out,"vk" #name));
    DEVICE_FUNCS(LOAD)
#undef LOAD
    i->GetPhysicalDeviceMemoryProperties(physical,&d->memory); i->GetPhysicalDeviceProperties(physical,&d->properties);
    if(!d->GetDeviceImageMemoryRequirements) d->GetDeviceImageMemoryRequirements=reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(d->gdpa(*out,"vkGetDeviceImageMemoryRequirementsKHR"));
    r=init_pipeline(*d);
    if(r!=VK_SUCCESS) return r;
    std::lock_guard<std::mutex> lock(maps_mutex);
    // Track queues transactionally while CreateDevice can report allocation failure.
    // The void GetDeviceQueue APIs then perform no host allocation.
    for(uint32_t n=0;n<info->queueCreateInfoCount;++n) for(uint32_t q=0;q<info->pQueueCreateInfos[n].queueCount;++q) {
        VkQueue queue=VK_NULL_HANDLE; d->GetDeviceQueue(*out,info->pQueueCreateInfos[n].queueFamilyIndex,q,&queue); queues[queue]=d;
    }
    devices[*out]=d; guard.active=false; return VK_SUCCESS;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR void VKAPI_CALL pvbDestroyDevice(VkDevice handle,const VkAllocationCallbacks *allocator) {
    auto d=lookup(devices,handle); if(!d) return;
    { std::lock_guard<std::mutex> lock(maps_mutex);
      for(auto it=commands.begin();it!=commands.end();) { if(it->second->device==d) { reset_command(*it->second); it=commands.erase(it); } else ++it; }
      for(auto it=pools.begin();it!=pools.end();) { if(it->second.device==d) it=pools.erase(it); else ++it; }
      for(auto it=queues.begin();it!=queues.end();) { if(it->second==d) it=queues.erase(it); else ++it; }
      for(auto it=images.begin();it!=images.end();) { if(it->second.device==d) it=images.erase(it); else ++it; }
      for(auto it=layouts.begin();it!=layouts.end();) { if(it->second->device==d) it=layouts.erase(it); else ++it; }
      devices.erase(handle);
    }
    destroy_pipeline(*d); d->DestroyDevice(handle,allocator);
}
VKAPI_ATTR void VKAPI_CALL pvbGetDeviceQueue(VkDevice handle,uint32_t family,uint32_t index,VkQueue *out) { auto d=lookup(devices,handle); if(!d) return; d->GetDeviceQueue(handle,family,index,out); }
VKAPI_ATTR void VKAPI_CALL pvbGetDeviceQueue2(VkDevice handle,const VkDeviceQueueInfo2 *info,VkQueue *out) { auto d=lookup(devices,handle); if(!d) return; d->GetDeviceQueue2(handle,info,out); }
VKAPI_ATTR VkResult VKAPI_CALL pvbCreateImage(VkDevice handle,const VkImageCreateInfo *info,const VkAllocationCallbacks *allocator,VkImage *out) try {
    *out=VK_NULL_HANDLE;
    auto d=lookup(devices,handle); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    if(!pvb::is_any_bc(info->format)) return d->CreateImage(handle,info,allocator,out);
    if(!pvb::is_bc(info->format)) return reject_image_request(*info);
    if(!supported_image(info->imageType,info->tiling,info->usage,info->flags)|| info->samples!=VK_SAMPLE_COUNT_1_BIT || info->extent.depth!=1 || !bc_features(*d->instance,d->physical,info->format)) return reject_image_request(*info);
    VkImageCreateInfo copy=*info; copy.format=select_backing(*d,*info);
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; std::vector<VkFormat> view_formats;
    if(info->pNext) {
        const auto p=static_cast<const VkBaseInStructure*>(info->pNext);
        if(p->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO || p->pNext) return reject_image_request(*info);
        list=*reinterpret_cast<const VkImageFormatListCreateInfo*>(p);
        for(uint32_t n=0;n<list.viewFormatCount;++n) { if(!pvb::is_bc(list.pViewFormats[n]) || !pvb::compatible_view(info->format,list.pViewFormats[n])) return reject_image_request(*info); view_formats.push_back(pvb::is_etc2_backing(copy.format)?pvb::etc2_format(list.pViewFormats[n]):pvb::rgba_format(list.pViewFormats[n])); }
        list.pViewFormats=view_formats.data(); copy.pNext=&list;
    }
    VkResult r=d->CreateImage(handle,&copy,allocator,out); if(r!=VK_SUCCESS) return r;
    auto guard=rollback([&] { d->DestroyImage(handle,*out,allocator); *out=VK_NULL_HANDLE; });
    std::shared_ptr<BlockStore> store;
    if(info->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        if(!info->mipLevels || info->mipLevels>32) return reject_image_request(*info);
        store=std::make_shared<BlockStore>(); store->device=d; store->mips.reserve(info->mipLevels);
        uint64_t bytes=0;
        for(uint32_t mip=0;mip<info->mipLevels;++mip) { pvb::BlockMip row{}; if(!pvb::block_mip(info->format,info->extent,mip,info->arrayLayers,bytes,row,bytes)) return VK_ERROR_OUT_OF_DEVICE_MEMORY; store->mips.push_back(row); }
        r=buffer(*d,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,store->buffer,store->memory,info);
        if(r!=VK_SUCCESS) return r;
        store->bytes=bytes; ++live_block_stores; live_block_bytes+=bytes;
    }
    std::lock_guard<std::mutex> lock(maps_mutex); images[*out]={info->format,info->extent,info->mipLevels,info->arrayLayers,d,std::move(store),info->usage,copy.format}; guard.active=false; log_image_request(*info,true);
    // Anonymous per-format shape logging above is bounded; this route is also
    // exposed by the aggregate compression observation ABI.
    static std::atomic<uint32_t> routes{0}; const uint32_t bit=1u<<(uint32_t(info->format)-131u);
    if(!(routes.fetch_or(bit)&bit)) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO,"PixelBC","Backing route: BC format=%u native format=%u lossy_ETC2=%u native_ETC2_enabled=%u",uint32_t(info->format),uint32_t(copy.format),uint32_t(pvb::is_etc2_backing(copy.format)),uint32_t(d->native_etc2));
#else
        std::fprintf(stderr,"PixelBC: Backing route: BC format=%u native format=%u lossy_ETC2=%u native_ETC2_enabled=%u\n",uint32_t(info->format),uint32_t(copy.format),uint32_t(pvb::is_etc2_backing(copy.format)),uint32_t(d->native_etc2));
#endif
    }
    return r;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR void VKAPI_CALL pvbDestroyImage(VkDevice handle,VkImage image,const VkAllocationCallbacks *allocator) { auto d=lookup(devices,handle); if(!d) return; { std::lock_guard<std::mutex> lock(maps_mutex); images.erase(image); } d->DestroyImage(handle,image,allocator); }
VKAPI_ATTR VkResult VKAPI_CALL pvbCreateImageView(VkDevice handle,const VkImageViewCreateInfo *info,const VkAllocationCallbacks *allocator,VkImageView *out) {
    auto d=lookup(devices,handle); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageViewCreateInfo copy=*info; auto image=lookup(images,info->image);
    if(image.device) {
        if(!pvb::is_bc(info->format) || !pvb::compatible_view(image.original,info->format)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        copy.format=pvb::is_etc2_backing(image.backing)?pvb::etc2_format(info->format):pvb::rgba_format(info->format);
    }
    return d->CreateImageView(handle,&copy,allocator,out);
}
VKAPI_ATTR void VKAPI_CALL pvbGetDeviceImageMemoryRequirements(VkDevice handle,const VkDeviceImageMemoryRequirements *info,VkMemoryRequirements2 *out) try {
    auto d=lookup(devices,handle); if(!d || !d->GetDeviceImageMemoryRequirements) return;
    auto copy=*info; auto create=*info->pCreateInfo;
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; std::vector<VkFormat> formats;
    if(pvb::is_any_bc(create.format)) {
        if(!pvb::is_bc(create.format) || !supported_image(create.imageType,create.tiling,create.usage,create.flags) ||
           create.samples!=VK_SAMPLE_COUNT_1_BIT || create.extent.depth!=1 || !bc_features(*d->instance,d->physical,create.format)) { out->memoryRequirements={}; return; }
        const VkFormat original=create.format;
        create.format=select_backing(*d,create); copy.pCreateInfo=&create;
        if(create.pNext) {
            auto ext=static_cast<const VkBaseInStructure*>(create.pNext);
            if(ext->sType!=VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO || ext->pNext) { out->memoryRequirements={}; return; }
            list=*reinterpret_cast<const VkImageFormatListCreateInfo*>(ext);
            for(uint32_t n=0;n<list.viewFormatCount;++n) { if(!pvb::compatible_view(original,list.pViewFormats[n])) { out->memoryRequirements={}; return; } formats.push_back(pvb::is_etc2_backing(create.format)?pvb::etc2_format(list.pViewFormats[n]):pvb::rgba_format(list.pViewFormats[n])); }
            list.pViewFormats=formats.data(); create.pNext=&list;
        }
    }
    d->GetDeviceImageMemoryRequirements(handle,&copy,out);
} catch(const std::bad_alloc&) { out->memoryRequirements={}; log_error("BC memory-query host allocation failed"); }
VKAPI_ATTR VkResult VKAPI_CALL pvbCreateCommandPool(VkDevice handle,const VkCommandPoolCreateInfo *info,const VkAllocationCallbacks *allocator,VkCommandPool *out) try {
    *out=VK_NULL_HANDLE; auto d=lookup(devices,handle); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r=d->CreateCommandPool(handle,info,allocator,out); if(r!=VK_SUCCESS) return r;
    auto guard=rollback([&] { d->DestroyCommandPool(handle,*out,allocator); *out=VK_NULL_HANDLE; });
    std::lock_guard<std::mutex> lock(maps_mutex); pools[*out]={d,info->queueFamilyIndex}; guard.active=false; return r;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
void cleanup_pool(VkCommandPool pool,bool erase) {
    std::lock_guard<std::mutex> lock(maps_mutex);
    for(auto it=commands.begin();it!=commands.end();) if(it->second->pool==pool) { reset_command(*it->second); if(erase) it=commands.erase(it); else ++it; } else ++it;
    if(erase) pools.erase(pool);
}
VKAPI_ATTR void VKAPI_CALL pvbDestroyCommandPool(VkDevice handle,VkCommandPool pool,const VkAllocationCallbacks *allocator) { auto d=lookup(devices,handle); if(!d) return; cleanup_pool(pool,true); d->DestroyCommandPool(handle,pool,allocator); }
VKAPI_ATTR VkResult VKAPI_CALL pvbResetCommandPool(VkDevice handle,VkCommandPool pool,VkCommandPoolResetFlags flags) { auto d=lookup(devices,handle); if(!d) return VK_ERROR_INITIALIZATION_FAILED; VkResult r=d->ResetCommandPool(handle,pool,flags); if(r==VK_SUCCESS) cleanup_pool(pool,false); return r; }
VKAPI_ATTR VkResult VKAPI_CALL pvbAllocateCommandBuffers(VkDevice handle,const VkCommandBufferAllocateInfo *info,VkCommandBuffer *out) try {
    auto d=lookup(devices,handle); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    std::fill_n(out,info->commandBufferCount,VK_NULL_HANDLE);
    VkResult r=d->AllocateCommandBuffers(handle,info,out); if(r!=VK_SUCCESS) return r;
    auto guard=rollback([&] {
        { std::lock_guard<std::mutex> lock(maps_mutex); for(uint32_t n=0;n<info->commandBufferCount;++n) commands.erase(out[n]); }
        d->FreeCommandBuffers(handle,info->commandPool,info->commandBufferCount,out); std::fill_n(out,info->commandBufferCount,VK_NULL_HANDLE);
    });
    std::lock_guard<std::mutex> lock(maps_mutex); for(uint32_t n=0;n<info->commandBufferCount;++n) { auto c=std::make_shared<Command>(); c->device=d; c->pool=info->commandPool; c->level=info->level; commands[out[n]]=c; } guard.active=false; return r;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR void VKAPI_CALL pvbFreeCommandBuffers(VkDevice handle,VkCommandPool pool,uint32_t count,const VkCommandBuffer *buffers) { auto d=lookup(devices,handle); if(!d) return; { std::lock_guard<std::mutex> lock(maps_mutex); for(uint32_t n=0;n<count;++n) { auto it=commands.find(buffers[n]); if(it!=commands.end()) { reset_command(*it->second); commands.erase(it); } } } d->FreeCommandBuffers(handle,pool,count,buffers); }
VKAPI_ATTR VkResult VKAPI_CALL pvbBeginCommandBuffer(VkCommandBuffer handle,const VkCommandBufferBeginInfo *info) { auto c=lookup(commands,handle); if(!c) return VK_ERROR_INITIALIZATION_FAILED; VkResult r=c->device->BeginCommandBuffer(handle,info); if(r==VK_SUCCESS) reset_command(*c); return r; }
VKAPI_ATTR VkResult VKAPI_CALL pvbEndCommandBuffer(VkCommandBuffer handle) { auto c=lookup(commands,handle); if(!c) return VK_ERROR_INITIALIZATION_FAILED; VkResult r=c->device->EndCommandBuffer(handle); return r==VK_SUCCESS?c->error:r; }
VKAPI_ATTR VkResult VKAPI_CALL pvbResetCommandBuffer(VkCommandBuffer handle,VkCommandBufferResetFlags flags) { auto c=lookup(commands,handle); if(!c) return VK_ERROR_INITIALIZATION_FAILED; VkResult r=c->device->ResetCommandBuffer(handle,flags); if(r==VK_SUCCESS) reset_command(*c); return r; }
VKAPI_ATTR VkResult VKAPI_CALL pvbCreatePipelineLayout(VkDevice handle,const VkPipelineLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkPipelineLayout *out) try {
    auto d=lookup(devices,handle); if(!d) return VK_ERROR_INITIALIZATION_FAILED;
    *out=VK_NULL_HANDLE;
    auto shadow=std::make_shared<ShadowLayout>(); shadow->device=d;
    VkResult r=d->CreatePipelineLayout(handle,info,allocator,out); if(r!=VK_SUCCESS) return r;
    auto guard=rollback([&] { d->DestroyPipelineLayout(handle,*out,allocator); *out=VK_NULL_HANDLE; });
    r=d->CreatePipelineLayout(handle,info,nullptr,&shadow->handle);
    if(r!=VK_SUCCESS) return r;
    std::lock_guard<std::mutex> lock(maps_mutex); layouts[*out]=std::move(shadow); guard.active=false; return VK_SUCCESS;
} catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
VKAPI_ATTR void VKAPI_CALL pvbDestroyPipelineLayout(VkDevice handle,VkPipelineLayout layout,const VkAllocationCallbacks *allocator) {
    auto d=lookup(devices,handle); if(!d) return;
    d->DestroyPipelineLayout(handle,layout,allocator);
    std::lock_guard<std::mutex> lock(maps_mutex); layouts.erase(layout);
}
VKAPI_ATTR void VKAPI_CALL pvbCmdBindPipeline(VkCommandBuffer handle,VkPipelineBindPoint point,VkPipeline pipeline) { auto c=lookup(commands,handle); if(!c) return; c->device->CmdBindPipeline(handle,point,pipeline); if(point==VK_PIPELINE_BIND_POINT_COMPUTE) c->compute=pipeline; }
VKAPI_ATTR void VKAPI_CALL pvbCmdBindDescriptorSets(VkCommandBuffer handle,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet *sets,uint32_t dynamic_count,const uint32_t *offsets) try {
    auto c=lookup(commands,handle); if(!c) return; c->device->CmdBindDescriptorSets(handle,point,layout,first,count,sets,dynamic_count,offsets);
    if(PVB_COMPACT_HISTORY && point==VK_PIPELINE_BIND_POINT_COMPUTE && !c->binds.empty()) {
        auto shadow=lookup(layouts,layout); auto &last=c->binds.back();
        // Identical layout and complete set range: the new binding replaces all
        // effects of this last descriptor operation, including dynamic offsets.
        // Never cross a different layout/range: its disturbance must be replayed.
        if(shadow && shadow==last.layout && first==last.first && count==last.sets.size() && dynamic_count==last.offsets.size()) {
            if(count) std::copy_n(sets,count,last.sets.begin());
            if(dynamic_count) std::copy_n(offsets,dynamic_count,last.offsets.begin());
            return;
        }
    }
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE) { DescriptorBind b; b.layout=lookup(layouts,layout); if(!b.layout) { fail(*c,VK_ERROR_UNKNOWN,"Untracked caller pipeline layout"); return; } b.first=first; if(count) b.sets.assign(sets,sets+count); if(dynamic_count) b.offsets.assign(offsets,offsets+dynamic_count); c->binds.push_back(std::move(b)); }
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC command state capture allocation failed"); }
VKAPI_ATTR void VKAPI_CALL pvbCmdPushConstants(VkCommandBuffer handle,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void *data) try {
    auto c=lookup(commands,handle); if(!c) return; c->device->CmdPushConstants(handle,layout,stages,offset,size,data);
    if(PVB_COMPACT_HISTORY && !c->pushes.empty()) {
        auto shadow=lookup(layouts,layout); auto &last=c->pushes.back();
        // Full overwrite of exactly the same stage/byte range only.
        if(shadow && shadow==last.layout && stages==last.stages && offset==last.offset && size==last.data.size()) {
            if(size) std::memcpy(last.data.data(),data,size);
            return;
        }
    }
    { Push p; p.layout=lookup(layouts,layout); if(!p.layout) { fail(*c,VK_ERROR_UNKNOWN,"Untracked caller push layout"); return; } p.stages=stages; p.offset=offset; p.data.assign(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size); c->pushes.push_back(std::move(p)); }
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC command state capture allocation failed"); }
bool retain_store(Command &c,const Image &image) {
    if(!image.blocks) {
        if(!(image.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return true;
        fail(c,VK_ERROR_UNKNOWN,"Missing BC compressed block storage"); return false;
    }
    if(std::find(c.stores.begin(),c.stores.end(),image.blocks)!=c.stores.end()) return true;
    try { c.stores.push_back(image.blocks); return true; }
    catch(const std::bad_alloc&) { fail(c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC block-storage command ownership failed"); return false; }
}
uint64_t block_offset(const Image &image,const VkBufferImageCopy &region,uint32_t layer,uint32_t row) {
    const auto &mip=image.blocks->mips[region.imageSubresource.mipLevel];
    return mip.offset+(uint64_t(region.imageSubresource.baseArrayLayer)+layer)*mip.layer_bytes+
        ((uint64_t(uint32_t(region.imageOffset.y)/4)+row)*mip.blocks_row+uint32_t(region.imageOffset.x)/4)*pvb::block_bytes(image.original);
}
void copy_block_rows(VkCommandBuffer handle,Command &c,const Image &image,VkBuffer buffer,const VkBufferImageCopy &region,const pvb::CopyLayout &sizes,bool upload) {
    const uint64_t bytes=pvb::block_bytes(image.original), row_bytes=((uint64_t(region.imageExtent.width)+3)/4)*bytes;
    const auto &mip=image.blocks->mips[region.imageSubresource.mipLevel];
    const uint32_t rows=uint32_t((uint64_t(region.imageExtent.height)+3)/4);
    const bool packed_rows=row_bytes==uint64_t(mip.blocks_row)*bytes && sizes.blocks_row==mip.blocks_row;
    const bool packed_layers=packed_rows && rows==mip.blocks_height && sizes.blocks_height==mip.blocks_height;
    const uint32_t row_step=packed_rows?rows:1,layer_step=packed_layers?sizes.layers:1;
    for(uint32_t layer=0;layer<sizes.layers;layer+=layer_step) for(uint32_t row=0;row<rows;row+=row_step) {
        const auto linear=region.bufferOffset+(uint64_t(layer)*sizes.blocks_height+row)*sizes.blocks_row*bytes;
        const auto canonical=block_offset(image,region,layer,row);
        VkBufferCopy copy{upload?linear:canonical,upload?canonical:linear,row_bytes*row_step*layer_step};
        c.device->CmdCopyBuffer(handle,upload?buffer:image.blocks->buffer,upload?image.blocks->buffer:buffer,1,&copy);
        block_copy_bytes+=copy.size;
    }
}
#include "bc_transfer_barriers.inc"
void replay_state(VkCommandBuffer handle,Command &c) {
    auto &d=*c.device;
    // Restore stage-specific compute state. Graphics bindings/constants remain untouched.
    if(c.compute) d.CmdBindPipeline(handle,VK_PIPELINE_BIND_POINT_COMPUTE,c.compute);
    perf_binds+=c.binds.size(); perf_pushes+=c.pushes.size();
    for(const auto &b:c.binds) d.CmdBindDescriptorSets(handle,VK_PIPELINE_BIND_POINT_COMPUTE,b.layout->handle,b.first,uint32_t(b.sets.size()),b.sets.data(),uint32_t(b.offsets.size()),b.offsets.data());
    for(const auto &p:c.pushes) d.CmdPushConstants(handle,p.layout->handle,p.stages,p.offset,uint32_t(p.data.size()),p.data.data());
}
VkResult upload_allocation(Command &c,const pvb::CopyLayout &sizes,Resource &resource,VkDescriptorSet &set,uint64_t &input_offset,uint64_t &output_offset) {
    auto &d=*c.device;
    if(PVB_UPLOAD_ARENAS) {
        const uint64_t alignment=std::max<uint64_t>(16,d.properties.limits.minStorageBufferOffsetAlignment);
        const auto aligned=[&](uint64_t n) { return (n+alignment-1)/alignment*alignment; };
        Arena *arena=nullptr; bool created=false;
        for(auto &a:c.arenas) if(a.sets<64 && aligned(a.input_used)+sizes.input_bytes<=a.input_size && aligned(a.output_used)+sizes.output_bytes<=a.output_size) { arena=&a; break; }
        if(!arena) {
            // Reserve ownership before native calls. Arenas are private to this
            // recording; executable/replayed/pending buffers keep every slot.
            try { if(c.arenas.size()==c.arenas.capacity()) c.arenas.reserve(c.arenas.empty()?1:c.arenas.size()*2); }
            catch(const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
            Arena a;
            a.input_size=aligned(std::max<uint64_t>(65536,sizes.input_bytes));
            a.output_size=aligned(std::max<uint64_t>(262144,sizes.output_bytes));
            auto guard=rollback([&] { destroy_resource(d,a.backing); });
            VkResult r=buffer(d,a.input_size,VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,a.backing.input,a.backing.input_memory);
            if(r==VK_SUCCESS) r=buffer(d,a.output_size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,a.backing.output,a.backing.output_memory);
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,128};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=64; pi.poolSizeCount=1; pi.pPoolSizes=&ps;
            if(r==VK_SUCCESS) { ++perf_pools; r=d.CreateDescriptorPool(d.handle,&pi,nullptr,&a.backing.descriptors); }
            if(r!=VK_SUCCESS) return r;
            c.arenas.push_back(a); guard.active=false; arena=&c.arenas.back(); created=true;
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=arena->backing.descriptors; ai.descriptorSetCount=1; ai.pSetLayouts=&d.descriptors;
        const auto r=d.AllocateDescriptorSets(d.handle,&ai,&set);
        if(r!=VK_SUCCESS) {
            if(created) { destroy_resource(d,arena->backing); c.arenas.pop_back(); }
            return r;
        }
        input_offset=aligned(arena->input_used); output_offset=aligned(arena->output_used);
        arena->input_used=input_offset+sizes.input_bytes; arena->output_used=output_offset+sizes.output_bytes; ++arena->sets;
        resource=arena->backing; resource.borrowed=true;
        return VK_SUCCESS;
    }
    VkResult r=buffer(d,sizes.input_bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,resource.input,resource.input_memory);
    if(r==VK_SUCCESS) r=buffer(d,sizes.output_bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,resource.output,resource.output_memory);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}; VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=&ps;
    if(r==VK_SUCCESS) { ++perf_pools; r=d.CreateDescriptorPool(d.handle,&pi,nullptr,&resource.descriptors); }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=resource.descriptors; ai.descriptorSetCount=1; ai.pSetLayouts=&d.descriptors;
    if(r==VK_SUCCESS) r=d.AllocateDescriptorSets(d.handle,&ai,&set);
    return r;
}
void decode(VkCommandBuffer handle,Command &c,VkBuffer source,VkImage image,VkImageLayout layout,const Image &tracked,const VkBufferImageCopy &region) {
    auto &d=*c.device; pvb::CopyLayout sizes{};
    if(!pvb::copy_layout(tracked.original,region,tracked.extent,tracked.mips,tracked.layers,sizes) || (layout!=VK_IMAGE_LAYOUT_GENERAL && layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) || c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY) { fail(c,VK_ERROR_UNKNOWN,"Unsupported BC upload region/layout/secondary buffer"); return; }
    const bool compressed=pvb::is_etc2_backing(tracked.backing);
    const auto codec=d.etc2_pipelines[tracked.original>=VK_FORMAT_BC3_UNORM_BLOCK?1:0];
    const uint32_t dispatch_width=compressed?(region.imageExtent.width+3u)/4u:region.imageExtent.width;
    const uint32_t dispatch_height=compressed?(region.imageExtent.height+3u)/4u:region.imageExtent.height;
    if(compressed && !pvb::etc2_output_size(tracked.backing,region.imageExtent.width,region.imageExtent.height,sizes.layers,sizes.output_bytes)) { fail(c,VK_ERROR_OUT_OF_DEVICE_MEMORY,"ETC2 output size overflow"); return; }
    if(sizes.input_bytes>d.properties.limits.maxStorageBufferRange || sizes.output_bytes>d.properties.limits.maxStorageBufferRange || (dispatch_width+7u)/8u>d.properties.limits.maxComputeWorkGroupCount[0] || (dispatch_height+7u)/8u>d.properties.limits.maxComputeWorkGroupCount[1] || sizes.layers>d.properties.limits.maxComputeWorkGroupCount[2] || (compressed&&!codec)) { fail(c,VK_ERROR_OUT_OF_DEVICE_MEMORY,"BC decode exceeds storage/dispatch limits or native pipeline unavailable"); return; }
    // Reserve ownership before any native allocation or GPU command: append cannot throw later.
    try { if(c.resources.size()==c.resources.capacity()) c.resources.reserve(c.resources.empty()?1:c.resources.size()*2); }
    catch(const std::bad_alloc&) { fail(c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC resource ownership allocation failed"); return; }
    if(!retain_store(c,tracked)) return;
    if(!(tracked.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fail(c,VK_ERROR_UNKNOWN,"BC upload requires transfer-destination usage"); return; }
    Resource resource{};
    VkDescriptorSet set=VK_NULL_HANDLE; uint64_t input_offset=0,output_offset=0;
    VkResult r=upload_allocation(c,sizes,resource,set,input_offset,output_offset);
    if(r!=VK_SUCCESS) { destroy_resource(d,resource); fail(c,r,"BC upload allocation failed"); return; }
    VkDescriptorBufferInfo buffers[2]={{resource.input,input_offset,sizes.input_bytes},{resource.output,output_offset,sizes.output_bytes}}; VkWriteDescriptorSet writes[2]{};
    for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&buffers[n]; }
    d.UpdateDescriptorSets(d.handle,2,writes,0,nullptr);
    // Preserve the caller's compressed bytes, including selector bits that have
    // no unique inverse in the decoded RGBA image. Buffer pitch padding is not
    // image content and is deliberately excluded from the canonical store.
    if(tracked.blocks) copy_block_rows(handle,c,tracked,source,region,sizes,true);
    VkBufferCopy copy{region.bufferOffset,input_offset,sizes.input_bytes}; d.CmdCopyBuffer(handle,source,resource.input,1,&copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.buffer=resource.input; barrier.offset=input_offset; barrier.size=sizes.input_bytes;
    d.CmdPipelineBarrier(handle,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    const uint32_t push[6]={uint32_t(tracked.original),region.imageExtent.width,region.imageExtent.height,sizes.layers,sizes.blocks_row,sizes.blocks_height};
    d.CmdBindPipeline(handle,VK_PIPELINE_BIND_POINT_COMPUTE,compressed?codec:d.pipeline); d.CmdBindDescriptorSets(handle,VK_PIPELINE_BIND_POINT_COMPUTE,d.layout,0,1,&set,0,nullptr); d.CmdPushConstants(handle,d.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
    d.CmdDispatch(handle,(dispatch_width+7u)/8u,(dispatch_height+7u)/8u,sizes.layers);
    if(compressed) ++etc2_dispatches; else ++rgba_dispatches;
#ifdef __ANDROID__
    if(dispatches.fetch_add(1)==0) __android_log_print(ANDROID_LOG_INFO,"PixelBC","First GPU BC decode recorded: format=%u extent=%ux%u layers=%u",uint32_t(tracked.original),region.imageExtent.width,region.imageExtent.height,sizes.layers);
#else
    ++dispatches;
#endif
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barrier.buffer=resource.output; barrier.offset=output_offset; barrier.size=sizes.output_bytes;
    d.CmdPipelineBarrier(handle,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    VkBufferImageCopy destination=region; destination.bufferOffset=output_offset; destination.bufferRowLength=0; destination.bufferImageHeight=0;
    d.CmdCopyBufferToImage(handle,resource.output,image,layout,1,&destination);
    replay_state(handle,c);
    c.resources.push_back(resource); ++live_resources;
}
VKAPI_ATTR void VKAPI_CALL pvbCmdCopyBufferToImage(VkCommandBuffer handle,VkBuffer source,VkImage image,VkImageLayout layout,uint32_t count,const VkBufferImageCopy *regions) {
    auto c=lookup(commands,handle); if(!c) return; auto tracked=lookup(images,image);
    if(!tracked.device) { c->device->CmdCopyBufferToImage(handle,source,image,layout,count,regions); return; }
    if(c->error!=VK_SUCCESS) return;
    for(uint32_t n=0;n<count && c->error==VK_SUCCESS;++n) decode(handle,*c,source,image,layout,tracked,regions[n]);
}
VKAPI_ATTR void VKAPI_CALL pvbCmdCopyBufferToImage2(VkCommandBuffer handle,const VkCopyBufferToImageInfo2 *info) try {
    auto c=lookup(commands,handle); if(!c) return;
    auto tracked=lookup(images,info->dstImage);
    if(!tracked.device) { auto native=reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(c->device->gdpa(c->device->handle,"vkCmdCopyBufferToImage2")); if(!native) native=reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(c->device->gdpa(c->device->handle,"vkCmdCopyBufferToImage2KHR")); native(handle,info); return; }
    if(info->pNext) { fail(*c,VK_ERROR_UNKNOWN,"BC CopyBufferToImage2 extension chain unsupported"); return; }
    std::vector<VkBufferImageCopy> regions; regions.reserve(info->regionCount);
    for(uint32_t n=0;n<info->regionCount;++n) { const auto &r=info->pRegions[n]; if(r.pNext) { fail(*c,VK_ERROR_UNKNOWN,"BC BufferImageCopy2 extension chain unsupported"); return; } regions.push_back({r.bufferOffset,r.bufferRowLength,r.bufferImageHeight,r.imageSubresource,r.imageOffset,r.imageExtent}); }
    pvbCmdCopyBufferToImage(handle,info->srcBuffer,info->dstImage,info->dstImageLayout,uint32_t(regions.size()),regions.data());
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC command state capture allocation failed"); }
VKAPI_ATTR void VKAPI_CALL pvbCmdCopyImageToBuffer(VkCommandBuffer handle,VkImage image,VkImageLayout layout,VkBuffer destination,uint32_t count,const VkBufferImageCopy *regions) {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return; const auto tracked=lookup(images,image);
    if(!tracked.device) { c->device->CmdCopyImageToBuffer(handle,image,layout,destination,count,regions); return; }
    if(c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || !(tracked.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || (layout!=VK_IMAGE_LAYOUT_GENERAL && layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) { fail(*c,VK_ERROR_UNKNOWN,"Unsupported BC readback layout/usage/secondary buffer"); return; }
    for(uint32_t n=0;n<count;++n) {
        pvb::CopyLayout sizes{};
        if(!pvb::copy_layout(tracked.original,regions[n],tracked.extent,tracked.mips,tracked.layers,sizes)) { fail(*c,VK_ERROR_UNKNOWN,"Unsupported BC readback region"); return; }
        if(!retain_store(*c,tracked)) return;
        copy_block_rows(handle,*c,tracked,destination,regions[n],sizes,false);
    }
}
VKAPI_ATTR void VKAPI_CALL pvbCmdCopyImageToBuffer2(VkCommandBuffer handle,const VkCopyImageToBufferInfo2 *info) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    if(!lookup(images,info->srcImage).device) {
        auto native=reinterpret_cast<PFN_vkCmdCopyImageToBuffer2>(c->device->gdpa(c->device->handle,"vkCmdCopyImageToBuffer2"));
        if(!native) native=reinterpret_cast<PFN_vkCmdCopyImageToBuffer2>(c->device->gdpa(c->device->handle,"vkCmdCopyImageToBuffer2KHR"));
        native(handle,info); return;
    }
    if(info->pNext) { fail(*c,VK_ERROR_UNKNOWN,"BC CopyImageToBuffer2 extension chain unsupported"); return; }
    std::vector<VkBufferImageCopy> regions; regions.reserve(info->regionCount);
    for(uint32_t n=0;n<info->regionCount;++n) { const auto &r=info->pRegions[n]; if(r.pNext) { fail(*c,VK_ERROR_UNKNOWN,"BC readback BufferImageCopy2 extension unsupported"); return; } regions.push_back({r.bufferOffset,r.bufferRowLength,r.bufferImageHeight,r.imageSubresource,r.imageOffset,r.imageExtent}); }
    pvbCmdCopyImageToBuffer(handle,info->srcImage,info->srcImageLayout,info->dstBuffer,uint32_t(regions.size()),regions.data());
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC readback2 region allocation failed"); }
VKAPI_ATTR void VKAPI_CALL pvbCmdCopyImage(VkCommandBuffer handle,VkImage source,VkImageLayout source_layout,VkImage destination,VkImageLayout destination_layout,uint32_t count,const VkImageCopy *regions) {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    const auto src=lookup(images,source),dst=lookup(images,destination);
    if(!src.device && !dst.device) { c->device->CmdCopyImage(handle,source,source_layout,destination,destination_layout,count,regions); return; }
    // BC/uncompressed block reinterpretation and differing BC decode families
    // require additional decoding semantics and are outside this bounded path.
    if(!src.device || !dst.device || !pvb::compatible_view(src.original,dst.original) || c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
        !(src.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(dst.usage&VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        (source_layout!=VK_IMAGE_LAYOUT_GENERAL && source_layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
        (destination_layout!=VK_IMAGE_LAYOUT_GENERAL && destination_layout!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)) { fail(*c,VK_ERROR_UNKNOWN,"Unsupported BC image-copy format/layout/usage/secondary buffer"); return; }
    for(uint32_t n=0;n<count;++n) {
        const auto &r=regions[n]; VkBufferImageCopy sr{},dr{};
        sr.imageSubresource=r.srcSubresource; sr.imageOffset=r.srcOffset; sr.imageExtent=r.extent;
        dr.imageSubresource=r.dstSubresource; dr.imageOffset=r.dstOffset; dr.imageExtent=r.extent;
        pvb::CopyLayout sl{},dl{};
        if(r.srcSubresource.layerCount!=r.dstSubresource.layerCount || !pvb::copy_layout(src.original,sr,src.extent,src.mips,src.layers,sl) || !pvb::copy_layout(dst.original,dr,dst.extent,dst.mips,dst.layers,dl)) { fail(*c,VK_ERROR_UNKNOWN,"Unsupported BC image-copy region"); return; }
        if(!retain_store(*c,src) || !retain_store(*c,dst)) return;
        if(pvb::is_etc2_backing(src.backing)!=pvb::is_etc2_backing(dst.backing)) {
            // Native RGBA↔ETC2 image copies would reinterpret incompatible
            // blocks. Regenerate the destination from byte-faithful BC storage
            // via transfer staging, preserving the caller's transfer-stage
            // synchronization semantics and avoiding repeated lossy encoding.
            const auto &sm=src.blocks->mips[r.srcSubresource.mipLevel];
            dr.bufferOffset=block_offset(src,sr,0,0);
            dr.bufferRowLength=sm.blocks_row*4u; dr.bufferImageHeight=sm.blocks_height*4u;
            decode(handle,*c,src.blocks->buffer,destination,destination_layout,dst,dr);
            ++cross_backing_regenerations;
            if(c->error!=VK_SUCCESS) return;
            continue;
        }
        if(dst.blocks) {
            const auto row_bytes=((uint64_t(r.extent.width)+3)/4)*pvb::block_bytes(src.original);
            const auto &sm=src.blocks->mips[r.srcSubresource.mipLevel],&dm=dst.blocks->mips[r.dstSubresource.mipLevel];
            const uint32_t rows=uint32_t((uint64_t(r.extent.height)+3)/4);
            const bool packed_rows=sl.blocks_row==sm.blocks_row && dl.blocks_row==dm.blocks_row;
            const bool packed_layers=packed_rows && rows==sm.blocks_height && rows==dm.blocks_height;
            const uint32_t row_step=packed_rows?rows:1,layer_step=packed_layers?sl.layers:1;
            for(uint32_t layer=0;layer<sl.layers;layer+=layer_step) for(uint32_t row=0;row<rows;row+=row_step) {
                VkBufferCopy copy{block_offset(src,sr,layer,row),block_offset(dst,dr,layer,row),row_bytes*row_step*layer_step};
                c->device->CmdCopyBuffer(handle,src.blocks->buffer,dst.blocks->buffer,1,&copy); block_copy_bytes+=copy.size;
            }
        }
        // Same BC family, including UNORM/SRGB reinterpretation: decoding is
        // identical in byte storage, and image copy preserves those RGBA bytes.
        c->device->CmdCopyImage(handle,source,source_layout,destination,destination_layout,1,&r);
    }
}
VKAPI_ATTR void VKAPI_CALL pvbCmdCopyImage2(VkCommandBuffer handle,const VkCopyImageInfo2 *info) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    if(!lookup(images,info->srcImage).device && !lookup(images,info->dstImage).device) {
        auto native=c->device->CmdCopyImage2; if(!native) native=reinterpret_cast<PFN_vkCmdCopyImage2>(c->device->gdpa(c->device->handle,"vkCmdCopyImage2KHR")); native(handle,info); return;
    }
    if(info->pNext) { fail(*c,VK_ERROR_UNKNOWN,"BC CopyImage2 extension chain unsupported"); return; }
    std::vector<VkImageCopy> regions; regions.reserve(info->regionCount);
    for(uint32_t n=0;n<info->regionCount;++n) { const auto &r=info->pRegions[n]; if(r.pNext) { fail(*c,VK_ERROR_UNKNOWN,"BC ImageCopy2 extension chain unsupported"); return; } regions.push_back({r.srcSubresource,r.srcOffset,r.dstSubresource,r.dstOffset,r.extent}); }
    pvbCmdCopyImage(handle,info->srcImage,info->srcImageLayout,info->dstImage,info->dstImageLayout,uint32_t(regions.size()),regions.data());
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC image-copy2 region allocation failed"); }
VKAPI_ATTR VkResult VKAPI_CALL pvbQueueSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo *info,VkFence fence) { auto d=lookup(queues,queue); if(!d) return VK_ERROR_INITIALIZATION_FAILED; for(uint32_t n=0;n<count;++n) for(uint32_t b=0;b<info[n].commandBufferCount;++b) { auto c=lookup(commands,info[n].pCommandBuffers[b]); if(c && c->error!=VK_SUCCESS) return c->error; } return d->QueueSubmit(queue,count,info,fence); }
VKAPI_ATTR VkResult VKAPI_CALL pvbQueueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2 *info,VkFence fence) { auto d=lookup(queues,queue); if(!d) return VK_ERROR_INITIALIZATION_FAILED; for(uint32_t n=0;n<count;++n) for(uint32_t b=0;b<info[n].commandBufferInfoCount;++b) { auto c=lookup(commands,info[n].pCommandBufferInfos[b].commandBuffer); if(c && c->error!=VK_SUCCESS) return c->error; } auto fn=d->QueueSubmit2; if(!fn) fn=reinterpret_cast<PFN_vkQueueSubmit2>(d->gdpa(d->handle,"vkQueueSubmit2KHR")); return fn(queue,count,info,fence); }
PFN_vkVoidFunction intercept(const char *name) {
#define MATCH(n) if(!std::strcmp(name,"vk" #n)) return reinterpret_cast<PFN_vkVoidFunction>(pvb##n);
    GLOBAL_FUNCS(MATCH)
    MATCH(EnumerateInstanceVersion)
    MATCH(DestroyInstance) MATCH(EnumeratePhysicalDevices) MATCH(EnumeratePhysicalDeviceGroups)
    MATCH(GetPhysicalDeviceProperties) MATCH(GetPhysicalDeviceProperties2) MATCH(GetPhysicalDeviceFeatures) MATCH(GetPhysicalDeviceFeatures2)
    MATCH(GetPhysicalDeviceFormatProperties) MATCH(GetPhysicalDeviceFormatProperties2)
    MATCH(GetPhysicalDeviceImageFormatProperties) MATCH(GetPhysicalDeviceImageFormatProperties2)
    MATCH(EnumerateDeviceExtensionProperties) MATCH(CreateDevice)
    MATCH(DestroyDevice) MATCH(GetDeviceQueue) MATCH(GetDeviceQueue2) MATCH(CreateImage) MATCH(DestroyImage) MATCH(CreateImageView)
    MATCH(CreateCommandPool) MATCH(DestroyCommandPool) MATCH(ResetCommandPool) MATCH(AllocateCommandBuffers) MATCH(FreeCommandBuffers)
    MATCH(BeginCommandBuffer) MATCH(EndCommandBuffer) MATCH(ResetCommandBuffer)
    MATCH(CreatePipelineLayout) MATCH(DestroyPipelineLayout) MATCH(CmdBindPipeline) MATCH(CmdBindDescriptorSets) MATCH(CmdPushConstants) MATCH(CmdCopyImage) MATCH(CmdCopyImage2) MATCH(CmdCopyBufferToImage) MATCH(CmdCopyBufferToImage2) MATCH(CmdCopyImageToBuffer) MATCH(CmdCopyImageToBuffer2)
    MATCH(CmdPipelineBarrier) MATCH(CmdPipelineBarrier2) MATCH(CmdWaitEvents) MATCH(CmdWaitEvents2) MATCH(CmdSetEvent2)
    MATCH(QueueSubmit) MATCH(QueueSubmit2) MATCH(GetDeviceImageMemoryRequirements)
#undef MATCH
    if(!std::strcmp(name,"vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if(!std::strcmp(name,"vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    const std::pair<const char*,PFN_vkVoidFunction> aliases[]={
#define ALIAS(n) {"vk" #n "KHR",reinterpret_cast<PFN_vkVoidFunction>(pvb##n)}
        ALIAS(EnumeratePhysicalDeviceGroups),ALIAS(GetPhysicalDeviceProperties2),ALIAS(GetPhysicalDeviceFeatures2),ALIAS(GetPhysicalDeviceFormatProperties2),ALIAS(GetPhysicalDeviceImageFormatProperties2),ALIAS(CmdCopyImage2),ALIAS(CmdCopyBufferToImage2),ALIAS(CmdCopyImageToBuffer2),ALIAS(CmdPipelineBarrier2),ALIAS(CmdWaitEvents2),ALIAS(CmdSetEvent2),ALIAS(QueueSubmit2),ALIAS(GetDeviceImageMemoryRequirements)
#undef ALIAS
    };
    for(auto &alias:aliases) if(!std::strcmp(name,alias.first)) return alias.second;
    return nullptr;
}
}
extern "C" EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance handle,const char *name) {
    if(!name || !ready()) return nullptr;
    auto native=backend.gipa(handle,name);
    if(!handle) {
        if(!std::strcmp(name,"vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
        if(!std::strcmp(name,"vkCreateInstance")||!std::strcmp(name,"vkEnumerateInstanceVersion")||!std::strcmp(name,"vkEnumerateInstanceExtensionProperties")||!std::strcmp(name,"vkEnumerateInstanceLayerProperties")) return intercept(name);
        return native;
    }
    if(!native) return nullptr;
    auto fn=intercept(name); return fn?fn:native;
}
extern "C" EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice handle,const char *name) {
    if(!handle || !name) return nullptr;
    auto d=lookup(devices,handle); if(!d) return nullptr;
    auto native=d->gdpa(handle,name); if(!native) return nullptr; auto fn=intercept(name); return fn?fn:native;
}
extern "C" EXPORT void pvb_get_stats(PvbStats *out) {
    if(!out) return;
    (void)ready();
    out->version=1; out->decode_dispatches=dispatches.load(); out->recording_failures=failures.load(); out->live_upload_resources=live_resources.load(); out->backend_recursion_rejections=backend_recursion.load(); std::snprintf(out->backend,sizeof(out->backend),"%s",backend.path.c_str());
}
extern "C" EXPORT const char *pvb_get_version() { return PVB_VERSION; }
extern "C" EXPORT void pvb_get_perf_stats(PvbPerfStats *out) {
    if(out) *out={1,perf_buffers.load(),perf_memory.load(),perf_pools.load(),perf_bytes.load(),perf_binds.load(),perf_pushes.load()};
}
extern "C" EXPORT void pvb_get_transfer_stats(PvbTransferStats *out) {
    if(out) *out={1,live_block_stores.load(),live_block_bytes.load(),block_copy_bytes.load()};
}
extern "C" EXPORT void pvb_get_compression_stats(PvbCompressionStats *out) {
    if(!out) return;
    *out={}; out->version=1;
    out->etc2_upload_dispatches=etc2_dispatches.load(); out->rgba_upload_dispatches=rgba_dispatches.load();
    out->cross_backing_regenerations=cross_backing_regenerations.load();
    std::lock_guard<std::mutex> lock(maps_mutex);
    for(const auto &d:devices) out->native_etc2_devices+=d.second->native_etc2;
    for(const auto &entry:images) {
        const auto &im=entry.second; const bool compressed=pvb::is_etc2_backing(im.backing);
        if(compressed) ++out->live_etc2_images; else ++out->live_rgba_images;
        uint64_t bytes=0;
        for(uint32_t mip=0;mip<im.mips;++mip) {
            const uint64_t w=std::max(1u,im.extent.width>>std::min(mip,31u)),h=std::max(1u,im.extent.height>>std::min(mip,31u));
            bytes+=compressed?((w+3)/4)*((h+3)/4)*im.layers*pvb::etc2_block_bytes(im.backing):w*h*im.layers*4;
        }
        if(compressed) out->live_etc2_logical_bytes+=bytes; else out->live_rgba_logical_bytes+=bytes;
    }
}
#ifdef __ANDROID__
namespace {
int hal_close(hw_device_t *device) { delete reinterpret_cast<hwvulkan_device_t*>(device); return 0; }
int hal_open(const hw_module_t *module,const char *id,hw_device_t **out) {
    if(!id || std::strcmp(id,"vk0") || !out || !ready()) return -1;
    auto device=new(std::nothrow) hwvulkan_device_t{}; if(!device) return -1;
    device->common.tag=HARDWARE_DEVICE_TAG; device->common.version=HWVULKAN_DEVICE_API_VERSION_0_1; device->common.module=const_cast<hw_module_t*>(module); device->common.close=hal_close;
    device->EnumerateInstanceExtensionProperties=pvbEnumerateInstanceExtensionProperties; device->CreateInstance=pvbCreateInstance; device->GetInstanceProcAddr=vkGetInstanceProcAddr; *out=&device->common; return 0;
}
hw_module_methods_t hal_methods={hal_open};
}
extern "C" {
EXPORT hwvulkan_module_t HMI={
    {HARDWARE_MODULE_TAG,HWVULKAN_MODULE_API_VERSION_0_1,HARDWARE_HAL_API_VERSION,"vulkan","Pixel BC1-5 stock Mali wrapper","Pixel Vulkan Wrapper contributors",&hal_methods,nullptr,{0}}
};
}
#endif
