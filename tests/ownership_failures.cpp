// SPDX-License-Identifier: MIT
// Real wrapper code + deterministic native mocks and allocation faults.
#include <atomic>
#include <new>
#include <cstdio>
#include <cstdlib>
std::atomic<bool> fail_next_allocation{false};
__attribute__((noinline)) void* operator new(std::size_t size) {
    if(fail_next_allocation.exchange(false)) throw std::bad_alloc();
    void *p=std::malloc(size?size:1); if(!p) throw std::bad_alloc(); return p;
}
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p,std::size_t) noexcept { std::free(p); }
#include "../src/wrapper.cpp"
namespace {
unsigned creates=0,destroys=0,native_commands=0,native_submissions=0,passed=0,failed=0;
bool fail_shadow=false,fail_map=false;
const VkAllocationCallbacks *public_allocator=nullptr;
unsigned public_destroyed=0,private_destroyed=0;
void check(bool condition,const char *name) { if(condition) ++passed; else { ++failed; std::fprintf(stderr,"FAIL: %s\n",name); } }
VKAPI_ATTR VkResult VKAPI_CALL fake_create(VkDevice,const VkPipelineLayoutCreateInfo*,const VkAllocationCallbacks *allocator,VkPipelineLayout *out) {
    ++creates;
    if(fail_shadow && creates==2) { *out=VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *out=reinterpret_cast<VkPipelineLayout>(100+creates);
    if(creates==1) public_allocator=allocator;
    if(fail_map && creates==2) fail_next_allocation=true;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fake_destroy(VkDevice,VkPipelineLayout handle,const VkAllocationCallbacks *allocator) {
    ++destroys;
    if(handle==reinterpret_cast<VkPipelineLayout>(101)) { ++public_destroyed; check(allocator==public_allocator,"Public destroy uses original allocator"); }
    else { ++private_destroyed; check(allocator==nullptr,"Private destroy uses owned allocator"); }
}
VKAPI_ATTR void VKAPI_CALL fake_bind(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*) { ++native_commands; }
VKAPI_ATTR void VKAPI_CALL fake_push(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void*) { ++native_commands; }
VKAPI_ATTR VkResult VKAPI_CALL fake_end(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL fake_reset(VkCommandBuffer,VkCommandBufferResetFlags) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL fake_submit(VkQueue,uint32_t,const VkSubmitInfo*,VkFence) { ++native_submissions; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL fake_buffer(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer*) { ++native_commands; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
void counts() { creates=destroys=public_destroyed=private_destroyed=native_commands=native_submissions=0; fail_shadow=fail_map=false; }
}
int main() {
    auto d=std::make_shared<Device>(); d->handle=reinterpret_cast<VkDevice>(1); d->CreatePipelineLayout=fake_create; d->DestroyPipelineLayout=fake_destroy; d->CmdBindDescriptorSets=fake_bind; d->CmdPushConstants=fake_push; d->EndCommandBuffer=fake_end; d->ResetCommandBuffer=fake_reset; d->QueueSubmit=fake_submit; d->CreateBuffer=fake_buffer;
    d->properties.limits.maxStorageBufferRange=8192; for(auto &limit:d->properties.limits.maxComputeWorkGroupCount) limit=64;
    devices[d->handle]=d; VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VkPipelineLayout out{}; VkAllocationCallbacks allocator{};
    counts(); fail_shadow=true;
    auto result=pvbCreatePipelineLayout(d->handle,&info,&allocator,&out);
    check(result==VK_ERROR_OUT_OF_DEVICE_MEMORY && creates==2 && destroys==1 && !out,"Native clone failure rolls back public layout");
    counts(); fail_map=true;
    result=pvbCreatePipelineLayout(d->handle,&info,&allocator,&out);
    check(result==VK_ERROR_OUT_OF_HOST_MEMORY && creates==2 && destroys==2 && !out && layouts.empty(),"Post-native map OOM returns error, destroys both layouts and nulls output");
    counts(); fail_next_allocation=true;
    result=pvbCreatePipelineLayout(d->handle,&info,&allocator,&out);
    check(result==VK_ERROR_OUT_OF_HOST_MEMORY && creates==0 && destroys==0 && !out,"Pre-native host OOM returns error without handles");
    counts();
    result=pvbCreatePipelineLayout(d->handle,&info,&allocator,&out);
    check(result==VK_SUCCESS && out && creates==2,"Owned compatible layout created");
    auto c1=std::make_shared<Command>(),c2=std::make_shared<Command>(); c1->device=c2->device=d; c1->level=c2->level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer h1=reinterpret_cast<VkCommandBuffer>(2),h2=reinterpret_cast<VkCommandBuffer>(3); commands[h1]=c1; commands[h2]=c2; VkDescriptorSet set=reinterpret_cast<VkDescriptorSet>(4); uint32_t value=123;
    pvbCmdBindDescriptorSets(h1,VK_PIPELINE_BIND_POINT_COMPUTE,out,0,1,&set,0,nullptr); pvbCmdPushConstants(h1,out,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,4,&value);
    pvbCmdBindDescriptorSets(h2,VK_PIPELINE_BIND_POINT_COMPUTE,out,0,1,&set,0,nullptr); pvbCmdPushConstants(h2,out,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&value);
    check(c1->pushes[0].stages==(VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT),"Mixed stages retained exactly");
    pvbDestroyPipelineLayout(d->handle,out,&allocator); check(destroys==1 && layouts.empty(),"Caller layout destroyed while two recordings retain shadow");
    check(pvbResetCommandBuffer(h1,0)==VK_SUCCESS && destroys==1,"First recording reset retains shared compatible layout");
    check(pvbResetCommandBuffer(h2,0)==VK_SUCCESS && destroys==2 && private_destroyed==1,"Last recording reset destroys compatible layout exactly once");
    counts(); result=pvbCreatePipelineLayout(d->handle,&info,&allocator,&out); check(result==VK_SUCCESS,"Capture fixture layout");
    fail_next_allocation=true; pvbCmdBindDescriptorSets(h1,VK_PIPELINE_BIND_POINT_COMPUTE,out,0,1,&set,0,nullptr); check(c1->error==VK_ERROR_OUT_OF_HOST_MEMORY && c1->binds.empty(),"Descriptor capture OOM poisons recording without partial state");
    check(pvbEndCommandBuffer(h1)==VK_ERROR_OUT_OF_HOST_MEMORY,"End reports host capture failure");
    VkQueue queue=reinterpret_cast<VkQueue>(5); queues[queue]=d; VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&h1; check(pvbQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_ERROR_OUT_OF_HOST_MEMORY && !native_submissions,"Failed capture cannot reach native submit");
    reset_command(*c1); fail_next_allocation=true; pvbCmdPushConstants(h1,out,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&value); check(c1->error==VK_ERROR_OUT_OF_HOST_MEMORY && c1->pushes.empty(),"Push capture OOM poisons recording without partial state");
    reset_command(*c1); native_commands=0; Image image{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,{4,4,1},1,1,d}; VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={4,4,1}; fail_next_allocation=true;
    decode(h1,*c1,VK_NULL_HANDLE,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL,image,region);
    check(c1->error==VK_ERROR_OUT_OF_HOST_MEMORY && !native_commands && c1->resources.empty(),"Upload ownership reserve OOM precedes native allocations/recording");
    reset_command(*c1); auto tracked_image=reinterpret_cast<VkImage>(6); images[tracked_image]=image; VkBufferImageCopy2 r2{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2}; r2.imageSubresource=region.imageSubresource; r2.imageExtent=region.imageExtent; VkCopyBufferToImageInfo2 copy{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2}; copy.dstImage=tracked_image; copy.regionCount=1; copy.pRegions=&r2; fail_next_allocation=true;
    pvbCmdCopyBufferToImage2(h1,&copy); check(c1->error==VK_ERROR_OUT_OF_HOST_MEMORY && !native_commands,"Copy2 region ownership OOM reports recording error");
    reset_command(*c1); pvbDestroyPipelineLayout(d->handle,out,&allocator); commands.clear(); queues.clear(); images.clear(); devices.clear();
    std::printf("OWNERSHIP_RESULT: %u passed; %u failed\n",passed,failed); return failed?1:0;
}
