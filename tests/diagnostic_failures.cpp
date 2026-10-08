// SPDX-License-Identifier: MIT
// Actual diagnostic function and buffer allocator with Vulkan allocation/queue
// failures. Mock pixels only let the ownership fixture reach phase two; fidelity
// is tested independently by the real GPU probe.
#define main pixel_bc_probe_main
#include "probe.cpp"
#undef main
#include <map>
namespace {
enum class Fault { Pipeline, SecondBuffer, AllocateBuffer, BindBuffer, MapBuffer, FourthBuffer, MapSamplingInput, AllocateImage, BindImage, ImageView, WaitTimeout, WaitLost, ResetCommand, DeferredRecovered, DeferredUnknown };
Fault fault{};
enum class Kind { Buffer, Memory, DescriptorLayout, PipelineLayout, Shader, Pipeline, DescriptorPool, Image, View };
std::map<uintptr_t,Kind> live;
std::map<uintptr_t,std::array<uint8_t,64>> storage;
uint32_t serial=10,buffer_calls=0,allocation_calls=0,map_calls=0,wait_calls=0,idle_calls=0;
unsigned device_destroys=0,command_frees=0,unsafe_destroys=0,assertions=0,test_failures=0;
bool gpu_pending=false;
VkDeviceMemory input_memory{},output_memory{};
VkPipeline bound_pipeline{};
uint32_t pushed_format=0;
void expect(bool value,const char *name) { ++assertions; if(!value) { ++test_failures; std::fprintf(stderr,"DIAGNOSTIC_FIXTURE_FAIL: %s\n",name); } }
template<class T> T create(Kind kind) { auto id=++serial; live.emplace(id,kind); return reinterpret_cast<T>(uintptr_t(id)); }
template<class T> void destroy(T handle,Kind kind) {
    if(gpu_pending) ++unsafe_destroys;
    auto id=reinterpret_cast<uintptr_t>(handle); auto found=live.find(id);
    expect(found!=live.end() && found->second==kind,"Owned handle destroyed exactly once with correct type");
    if(found!=live.end()) live.erase(found);
}
VkResult VKAPI_CALL create_buffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer *out) {
    ++buffer_calls;
    if((fault==Fault::SecondBuffer && buffer_calls==2) || (fault==Fault::FourthBuffer && buffer_calls==4)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=create<VkBuffer>(Kind::Buffer); return VK_SUCCESS;
}
void VKAPI_CALL requirements(VkDevice,VkBuffer,VkMemoryRequirements *out) { *out={64,4,1}; }
VkResult VKAPI_CALL allocate_memory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory *out) {
    ++allocation_calls;
    if((fault==Fault::AllocateBuffer && allocation_calls==1) || (fault==Fault::AllocateImage && allocation_calls==5)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=create<VkDeviceMemory>(Kind::Memory); storage.emplace(reinterpret_cast<uintptr_t>(*out),std::array<uint8_t,64>{}); return VK_SUCCESS;
}
VkResult VKAPI_CALL bind_buffer(VkDevice,VkBuffer,VkDeviceMemory memory,VkDeviceSize) {
    if(fault==Fault::BindBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if(buffer_calls%2) input_memory=memory; else output_memory=memory; return VK_SUCCESS;
}
VkResult VKAPI_CALL map_memory(VkDevice,VkDeviceMemory memory,VkDeviceSize,VkDeviceSize,VkMemoryMapFlags,void **out) {
    ++map_calls;
    if((fault==Fault::MapBuffer && map_calls==1) || (fault==Fault::MapSamplingInput && map_calls==3)) return VK_ERROR_MEMORY_MAP_FAILED;
    *out=storage.at(reinterpret_cast<uintptr_t>(memory)).data(); return VK_SUCCESS;
}
void VKAPI_CALL unmap_memory(VkDevice,VkDeviceMemory) { if(gpu_pending) ++unsafe_destroys; }
void VKAPI_CALL destroy_buffer(VkDevice,VkBuffer handle,const VkAllocationCallbacks*) { destroy(handle,Kind::Buffer); }
void VKAPI_CALL free_memory(VkDevice,VkDeviceMemory memory,const VkAllocationCallbacks*) { destroy(memory,Kind::Memory); storage.erase(reinterpret_cast<uintptr_t>(memory)); }
VkResult VKAPI_CALL create_descriptors(VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout *out) { *out=create<VkDescriptorSetLayout>(Kind::DescriptorLayout); return VK_SUCCESS; }
void VKAPI_CALL destroy_descriptors(VkDevice,VkDescriptorSetLayout handle,const VkAllocationCallbacks*) { destroy(handle,Kind::DescriptorLayout); }
VkResult VKAPI_CALL create_layout(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks*,VkPipelineLayout *out) { *out=create<VkPipelineLayout>(Kind::PipelineLayout); return VK_SUCCESS; }
void VKAPI_CALL destroy_layout(VkDevice,VkPipelineLayout handle,const VkAllocationCallbacks*) { destroy(handle,Kind::PipelineLayout); }
VkResult VKAPI_CALL create_shader(VkDevice,const VkShaderModuleCreateInfo*,const VkAllocationCallbacks*,VkShaderModule *out) { *out=create<VkShaderModule>(Kind::Shader); return VK_SUCCESS; }
void VKAPI_CALL destroy_shader(VkDevice,VkShaderModule handle,const VkAllocationCallbacks*) { destroy(handle,Kind::Shader); }
VkResult VKAPI_CALL create_pipeline(VkDevice,VkPipelineCache,uint32_t,const VkComputePipelineCreateInfo*,const VkAllocationCallbacks*,VkPipeline *out) { if(fault==Fault::Pipeline) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *out=create<VkPipeline>(Kind::Pipeline); return VK_SUCCESS; }
void VKAPI_CALL destroy_pipeline(VkDevice,VkPipeline handle,const VkAllocationCallbacks*) { destroy(handle,Kind::Pipeline); }
VkResult VKAPI_CALL create_pool(VkDevice,const VkDescriptorPoolCreateInfo*,const VkAllocationCallbacks*,VkDescriptorPool *out) { *out=create<VkDescriptorPool>(Kind::DescriptorPool); return VK_SUCCESS; }
void VKAPI_CALL destroy_pool(VkDevice,VkDescriptorPool handle,const VkAllocationCallbacks*) { destroy(handle,Kind::DescriptorPool); }
VkResult VKAPI_CALL allocate_set(VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet *out) { *out=reinterpret_cast<VkDescriptorSet>(uintptr_t(1000)); return VK_SUCCESS; }
void VKAPI_CALL update_sets(VkDevice,uint32_t,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*) {}
VkResult VKAPI_CALL create_image(VkDevice,const VkImageCreateInfo*,const VkAllocationCallbacks*,VkImage *out) { *out=create<VkImage>(Kind::Image); return VK_SUCCESS; }
void VKAPI_CALL image_requirements(VkDevice,VkImage,VkMemoryRequirements *out) { *out={64,4,1}; }
VkResult VKAPI_CALL bind_image(VkDevice,VkImage,VkDeviceMemory,VkDeviceSize) { return fault==Fault::BindImage?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS; }
void VKAPI_CALL destroy_image(VkDevice,VkImage handle,const VkAllocationCallbacks*) { destroy(handle,Kind::Image); }
VkResult VKAPI_CALL create_view(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView *out) { if(fault==Fault::ImageView) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *out=create<VkImageView>(Kind::View); return VK_SUCCESS; }
void VKAPI_CALL destroy_view(VkDevice,VkImageView handle,const VkAllocationCallbacks*) { destroy(handle,Kind::View); }
VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VkResult VKAPI_CALL end(VkCommandBuffer) { return VK_SUCCESS; }
void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t,const VkImageMemoryBarrier*) {}
void VKAPI_CALL bind_pipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline pipeline) { bound_pipeline=pipeline; }
void VKAPI_CALL bind_sets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*) {}
void VKAPI_CALL push(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void *data) { pushed_format=*static_cast<const uint32_t*>(data); }
void VKAPI_CALL dispatch(VkCommandBuffer,uint32_t,uint32_t,uint32_t) {
    auto &input=storage.at(reinterpret_cast<uintptr_t>(input_memory)); auto &output=storage.at(reinterpret_cast<uintptr_t>(output_memory));
    if(bound_pipeline==reinterpret_cast<VkPipeline>(uintptr_t(7))) { for(unsigned n=0;n<32;++n) output[n]=uint8_t(std::max(-127,int(int8_t(input[n])))); }
    else { auto pixels=reference(VkFormat(pushed_format),input.data()); std::memcpy(output.data(),pixels.data(),64); }
}
void VKAPI_CALL copy(VkCommandBuffer,VkBuffer,VkImage,VkImageLayout,uint32_t,const VkBufferImageCopy*) {}
VkResult VKAPI_CALL submit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence) { gpu_pending=true; return VK_SUCCESS; }
VkResult VKAPI_CALL wait(VkDevice,uint32_t,const VkFence*,VkBool32,uint64_t) {
    ++wait_calls;
    if(fault==Fault::WaitLost) { gpu_pending=false; return VK_ERROR_DEVICE_LOST; }
    if(fault==Fault::WaitTimeout || fault==Fault::DeferredRecovered || fault==Fault::DeferredUnknown) return VK_TIMEOUT;
    gpu_pending=false; return VK_SUCCESS;
}
VkResult VKAPI_CALL reset_fence(VkDevice,uint32_t,const VkFence*) { return VK_SUCCESS; }
VkResult VKAPI_CALL reset_command(VkCommandBuffer,VkCommandBufferResetFlags) { return fault==Fault::ResetCommand?VK_ERROR_OUT_OF_HOST_MEMORY:VK_SUCCESS; }
VkResult VKAPI_CALL idle(VkDevice) {
    ++idle_calls;
    if(fault==Fault::DeferredUnknown || (fault==Fault::DeferredRecovered && idle_calls==1)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    gpu_pending=false; return fault==Fault::WaitLost?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
void VKAPI_CALL free_commands(VkDevice,VkCommandPool,uint32_t,const VkCommandBuffer*) { ++command_frees; if(gpu_pending) ++unsafe_destroys; }
void VKAPI_CALL destroy_command_pool(VkDevice,VkCommandPool,const VkAllocationCallbacks*) { if(gpu_pending) ++unsafe_destroys; }
void VKAPI_CALL destroy_device(VkDevice,const VkAllocationCallbacks*) { ++device_destroys; expect(live.empty(),"No diagnostic allocations live at device destruction"); if(gpu_pending) ++unsafe_destroys; }
void init_mock(App &a) {
    a.device=reinterpret_cast<VkDevice>(uintptr_t(1)); a.command=reinterpret_cast<VkCommandBuffer>(uintptr_t(2)); a.pool=reinterpret_cast<VkCommandPool>(uintptr_t(3)); a.fence=VK_NULL_HANDLE;
    // The persistent sample pipeline belongs to the surrounding App, not the
    // diagnostic owner. Leave its App field null to isolate diagnostic counts.
    a.memory.memoryTypeCount=1; a.memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    a.CreateBuffer=create_buffer; a.GetBufferMemoryRequirements=requirements; a.AllocateMemory=allocate_memory; a.BindBufferMemory=bind_buffer; a.MapMemory=map_memory; a.UnmapMemory=unmap_memory; a.DestroyBuffer=destroy_buffer; a.FreeMemory=free_memory;
    a.CreateDescriptorSetLayout=create_descriptors; a.DestroyDescriptorSetLayout=destroy_descriptors; a.CreatePipelineLayout=create_layout; a.DestroyPipelineLayout=destroy_layout; a.CreateShaderModule=create_shader; a.DestroyShaderModule=destroy_shader; a.CreateComputePipelines=create_pipeline; a.DestroyPipeline=destroy_pipeline;
    a.CreateDescriptorPool=create_pool; a.DestroyDescriptorPool=destroy_pool; a.AllocateDescriptorSets=allocate_set; a.UpdateDescriptorSets=update_sets;
    a.CreateImage=create_image; a.GetImageMemoryRequirements=image_requirements; a.BindImageMemory=bind_image; a.DestroyImage=destroy_image; a.CreateImageView=create_view; a.DestroyImageView=destroy_view;
    a.BeginCommandBuffer=begin; a.EndCommandBuffer=end; a.CmdPipelineBarrier=barrier; a.CmdBindPipeline=bind_pipeline; a.CmdBindDescriptorSets=bind_sets; a.CmdPushConstants=push; a.CmdDispatch=dispatch; a.CmdCopyBufferToImage=copy;
    a.QueueSubmit=submit; a.WaitForFences=wait; a.ResetFences=reset_fence; a.ResetCommandBuffer=reset_command; a.DeviceWaitIdle=idle; a.FreeCommandBuffers=free_commands; a.DestroyCommandPool=destroy_command_pool; a.DestroyDevice=destroy_device;
}
}
int main() {
    unsigned cases=0;
    for(auto scenario:{Fault::Pipeline,Fault::SecondBuffer,Fault::AllocateBuffer,Fault::BindBuffer,Fault::MapBuffer,Fault::FourthBuffer,Fault::MapSamplingInput,Fault::AllocateImage,Fault::BindImage,Fault::ImageView,Fault::WaitTimeout,Fault::WaitLost,Fault::ResetCommand,Fault::DeferredRecovered,Fault::DeferredUnknown}) {
        ++cases; fault=scenario; live.clear(); storage.clear(); buffer_calls=allocation_calls=map_calls=wait_calls=idle_calls=0; device_destroys=command_frees=unsafe_destroys=0; gpu_pending=false; failed=0;
        App app; init_mock(app); bool caught=false;
        try { signed_diagnostics(app); } catch(const std::runtime_error &e) { caught=true; std::printf("EXPECTED_DIAGNOSTIC_ERROR: scenario=%u %s\n",unsigned(scenario),e.what()); }
        expect(caught,"Injected diagnostic failure propagates original exception");
        if(scenario==Fault::DeferredRecovered || scenario==Fault::DeferredUnknown) expect(app.diagnostic_deferred && !live.empty(),"Unknown pending work transfers ownership for final idle attempt");
        app.shutdown();
        if(scenario==Fault::DeferredUnknown) { expect(device_destroys==0 && !live.empty() && failed>0,"Unresolved completion retains objects and fails without unsafe device destruction"); }
        else { expect(live.empty() && device_destroys==1,"Every owned diagnostic resource reclaimed before device destruction"); }
        expect(unsafe_destroys==0,"No pending resource or command destroyed before GPU quiescence");
        if(scenario==Fault::WaitTimeout || scenario==Fault::WaitLost || scenario==Fault::ResetCommand || scenario==Fault::DeferredRecovered) expect(command_frees==1,"Failed recorded command ownership released once after quiescence");
        if(scenario==Fault::WaitLost || scenario==Fault::DeferredUnknown || scenario==Fault::DeferredRecovered) expect(failed>0,"Quiescence errors remain reported failures");
        failed=0; // Injected errors were asserted; production main preserves them.
    }
    std::printf("DIAGNOSTIC_OWNERSHIP: %u fault scenarios; %u checks; %u failures\n",cases,assertions,test_failures);
    return test_failures?1:0;
}
