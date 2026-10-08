// SPDX-License-Identifier: MIT
// Compile the real wrapper with native allocation failures. Independent literal
// offsets and native destruction counts verify rollback and ownership boundaries.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
std::atomic<bool> fail_transfer_host{false};
__attribute__((noinline)) void *operator new(std::size_t size) {
    if(fail_transfer_host.exchange(false)) throw std::bad_alloc();
    auto p=std::malloc(size?size:1); if(!p) throw std::bad_alloc(); return p;
}
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p,std::size_t) noexcept { std::free(p); }
#include "../src/wrapper.cpp"
namespace {
unsigned passed=0,failed=0,image_create=0,image_destroy=0,buffer_create=0,buffer_destroy=0,memory_create=0,memory_destroy=0,barrier_calls=0,copy_calls=0;
bool device_destroyed=false;
int failure_stage=0; VkDeviceSize native_size=0; VkSharingMode native_sharing=VK_SHARING_MODE_EXCLUSIVE; uint32_t native_family_count=0;
VkBufferMemoryBarrier last_barriers[8]{}; uint32_t last_count=0;
void check(bool value,const char *name) { if(value) ++passed; else { ++failed; std::fprintf(stderr,"FAIL: %s\n",name); } }
VKAPI_ATTR void VKAPI_CALL format_props(VkPhysicalDevice,VkFormat,VkFormatProperties *out) { *out={}; out->optimalTilingFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT; }
VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice,const VkImageCreateInfo *info,const VkAllocationCallbacks*,VkImage *out) {
    ++image_create; check(info->format==VK_FORMAT_R8G8B8A8_UNORM,"Native image stores decoded RGBA");
    if(failure_stage==1) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=reinterpret_cast<VkImage>(101); if(failure_stage==5) fail_transfer_host=true; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice,VkImage,const VkAllocationCallbacks*) { ++image_destroy; }
VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice,const VkBufferCreateInfo *info,const VkAllocationCallbacks *allocator,VkBuffer *out) {
    ++buffer_create; check(!allocator,"Canonical storage owns native allocator selection"); native_size=info->size; native_sharing=info->sharingMode; native_family_count=info->queueFamilyIndexCount;
    check(info->usage==(VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT),"Canonical buffer has truthful transfer usages");
    if(failure_stage==2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out=reinterpret_cast<VkBuffer>(102); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice,VkBuffer,const VkAllocationCallbacks*) { check(!device_destroyed,"Internal buffer destroyed before native device"); ++buffer_destroy; }
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice,VkBuffer,VkMemoryRequirements *out) { *out={512,64,1}; }
VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory *out) { ++memory_create; if(failure_stage==3) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *out=reinterpret_cast<VkDeviceMemory>(103); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*) { check(!device_destroyed,"Internal memory freed before native device"); ++memory_destroy; }
VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice,const VkAllocationCallbacks*) { device_destroyed=true; }
VKAPI_ATTR VkResult VKAPI_CALL bind_memory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize) { if(failure_stage==4) return VK_ERROR_OUT_OF_DEVICE_MEMORY; if(failure_stage==6) fail_transfer_host=true; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL pipeline_barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t count,const VkBufferMemoryBarrier *barriers,uint32_t,const VkImageMemoryBarrier*) { ++barrier_calls; last_count=count; for(uint32_t n=0;n<count && n<8;++n) last_barriers[n]=barriers[n]; }
VKAPI_ATTR void VKAPI_CALL copy_buffer(VkCommandBuffer,VkBuffer,VkBuffer,uint32_t,const VkBufferCopy*) { ++copy_calls; }
void counters() { image_create=image_destroy=buffer_create=buffer_destroy=memory_create=memory_destroy=barrier_calls=copy_calls=0; }
}
int main() {
    auto d=std::make_shared<Device>(); auto i=std::make_shared<Instance>(); i->GetPhysicalDeviceFormatProperties=format_props; d->instance=i; d->handle=reinterpret_cast<VkDevice>(1);
    d->CreateImage=create_image; d->DestroyImage=destroy_image; d->CreateBuffer=create_buffer; d->DestroyBuffer=destroy_buffer; d->GetBufferMemoryRequirements=requirements; d->AllocateMemory=allocate_memory; d->FreeMemory=free_memory; d->BindBufferMemory=bind_memory; d->CmdPipelineBarrier=pipeline_barrier; d->CmdCopyBuffer=copy_buffer;
    d->memory.memoryTypeCount=1; d->memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; devices[d->handle]=d;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK; info.extent={7,7,1}; info.mipLevels=3; info.arrayLayers=6; info.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for(failure_stage=1;failure_stage<=6;++failure_stage) {
        counters(); VkImage image{}; const auto result=pvbCreateImage(d->handle,&info,nullptr,&image);
        check(result==(failure_stage>=5?VK_ERROR_OUT_OF_HOST_MEMORY:VK_ERROR_OUT_OF_DEVICE_MEMORY) && !image && images.empty(),"Injected image/store allocation failure returns error and null handle");
        check(image_destroy==(failure_stage==1?0u:1u),"Native image rolled back exactly once");
        check(buffer_destroy==((failure_stage>=3 && failure_stage!=5)?1u:0u),"Created canonical buffer rolled back exactly once");
        check(memory_destroy==((failure_stage==4 || failure_stage==6)?1u:0u),"Allocated canonical memory rolled back exactly once");
        check(!live_block_stores && !live_block_bytes,"Failed creation leaves no canonical stores or bytes");
    }
    failure_stage=0; counters(); VkImage image{};
    info.usage&=~VK_IMAGE_USAGE_TRANSFER_SRC_BIT; check(pvbCreateImage(d->handle,&info,nullptr,&image)==VK_SUCCESS && !buffer_create && !lookup(images,image).blocks,"Sampled/upload-only NFS image allocates no canonical storage");
    pvbDestroyImage(d->handle,image,nullptr); check(image_destroy==1 && !buffer_destroy,"Upload-only image cleanup stays native");
    info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT; uint32_t families[2]={0,1}; info.sharingMode=VK_SHARING_MODE_CONCURRENT; info.queueFamilyIndexCount=2; info.pQueueFamilyIndices=families;
    counters(); check(pvbCreateImage(d->handle,&info,nullptr,&image)==VK_SUCCESS,"Canonical image creation");
    check(native_size==288 && native_sharing==VK_SHARING_MODE_CONCURRENT && native_family_count==2,"Canonical size and queue sharing match image");
    auto tracked=lookup(images,image); check(tracked.blocks->mips[0].offset==0 && tracked.blocks->mips[1].offset==192 && tracked.blocks->mips[2].offset==240,"Independent literal mip offsets include all six faces");
    auto c=std::make_shared<Command>(); c->device=d; c->level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; const auto command=reinterpret_cast<VkCommandBuffer>(2); commands[command]=c;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image=image; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barrier.srcQueueFamilyIndex=0; barrier.dstQueueFamilyIndex=1; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,1,VK_REMAINING_MIP_LEVELS,2,3};
    pvbCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    check(barrier_calls==1 && last_count==2 && last_barriers[0].offset==208 && last_barriers[0].size==24 && last_barriers[1].offset==256 && last_barriers[1].size==24,"Image barriers synchronize only matching canonical mip/layer ranges");
    check(last_barriers[0].srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT && last_barriers[0].dstAccessMask==VK_ACCESS_TRANSFER_READ_BIT && last_barriers[0].srcQueueFamilyIndex==0 && last_barriers[0].dstQueueFamilyIndex==1,"Image ownership/access scopes preserved for canonical buffer");
    reset_command(*c); barrier_calls=0; fail_transfer_host=true;
    pvbCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    check(c->error==VK_ERROR_OUT_OF_HOST_MEMORY && !barrier_calls,"Barrier ownership OOM poisons before native recording"); reset_command(*c);
    // Hold no test-owned copy of the store: only image and command ownership are
    // allowed to keep native resources alive after completed image destruction.
    check(retain_store(*c,tracked),"Command retains canonical source ownership");
    pvbDestroyImage(d->handle,image,nullptr); check(!buffer_destroy && !memory_destroy,"Recorded canonical resources survive source image destruction");
    // The temporary read-only snapshot is also an owner, so drop it explicitly.
    tracked.blocks.reset(); reset_command(*c);
    check(buffer_destroy==1 && memory_destroy==1 && !live_block_stores && !live_block_bytes,"Last command reset destroys store and memory exactly once");
    // Defensive device cleanup also releases private command-owned storage
    // before handing device destruction to the backend.
    check(pvbCreateImage(d->handle,&info,nullptr,&image)==VK_SUCCESS,"Device cleanup fixture image"); check(retain_store(*c,lookup(images,image)),"Device cleanup fixture command retention");
    pvbDestroyImage(d->handle,image,nullptr); d->DestroyDevice=destroy_device; pvbDestroyDevice(d->handle,nullptr);
    check(device_destroyed && !live_block_stores && !live_block_bytes && c->stores.empty() && commands.empty() && images.empty() && devices.empty(),"Native device cleanup follows internal store/command cleanup");
    pvb::BlockMip mip{}; uint64_t end=0;
    check(!pvb::block_mip(VK_FORMAT_BC3_UNORM_BLOCK,{UINT32_MAX,UINT32_MAX,1},0,UINT32_MAX,0,mip,end),"Canonical huge-shape multiplication overflow rejected");
    check(!pvb::block_mip(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,{1,1,1},0,1,UINT64_MAX-3,mip,end),"Canonical total-offset overflow rejected");
    std::printf("TRANSFER_OWNERSHIP_RESULT: %u passed; %u failed\n",passed,failed); return failed?1:0;
}
