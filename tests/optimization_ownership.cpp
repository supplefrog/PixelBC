// SPDX-License-Identifier: MIT
// Real wrapper implementation; independent native lifetime/state observations.
#include <atomic>
#include <new>
#include <cstdlib>
std::atomic<bool> optimization_host_fault{false};
__attribute__((noinline)) void* operator new(std::size_t n) {
    if(optimization_host_fault.exchange(false)) throw std::bad_alloc();
    auto p=std::malloc(n?n:1); if(!p) throw std::bad_alloc(); return p;
}
__attribute__((noinline)) void operator delete(void *p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void *p,std::size_t) noexcept { std::free(p); }
#include "../src/wrapper.cpp"
#include <set>
namespace {
uint64_t serial=1000; unsigned passed=0,failed=0,operation=0,fault=0,created_buffers=0,created_pools=0;
bool reset_fails=false;
std::set<uint64_t> buffers,memories,pools_alive;
std::unordered_map<VkBuffer,uint64_t> buffer_sizes;
std::unordered_map<VkDescriptorPool,uint32_t> pool_sizes,pool_used;
std::unordered_map<VkDescriptorSet,std::array<VkDescriptorBufferInfo,2>> descriptors_seen;
template<class T> uint64_t number(T h) { return reinterpret_cast<uint64_t>(h); }
template<class T> T handle() { return reinterpret_cast<T>(++serial); }
void check(bool v,const char *name) { if(v) ++passed; else { ++failed; std::fprintf(stderr,"FAIL: %s\n",name); } }
bool fail_native() { return ++operation==fault; }
VKAPI_ATTR VkResult VKAPI_CALL create_buffer(VkDevice,const VkBufferCreateInfo *i,const VkAllocationCallbacks*,VkBuffer *out) { if(fail_native()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *out=handle<VkBuffer>(); buffers.insert(number(*out)); buffer_sizes[*out]=i->size; ++created_buffers; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL requirements(VkDevice,VkBuffer h,VkMemoryRequirements *out) { *out={buffer_sizes.at(h),256,1}; }
VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory *out) { if(fail_native()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *out=handle<VkDeviceMemory>(); memories.insert(number(*out)); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL bind_memory(VkDevice,VkBuffer,VkDeviceMemory,VkDeviceSize) { return fail_native()?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice,VkBuffer h,const VkAllocationCallbacks*) { check(buffers.erase(number(h))==1,"Buffer destroyed exactly once"); }
VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice,VkDeviceMemory h,const VkAllocationCallbacks*) { check(memories.erase(number(h))==1,"Memory destroyed exactly once"); }
VKAPI_ATTR VkResult VKAPI_CALL create_pool(VkDevice,const VkDescriptorPoolCreateInfo *i,const VkAllocationCallbacks*,VkDescriptorPool *out) { if(fail_native()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; *out=handle<VkDescriptorPool>(); pools_alive.insert(number(*out)); pool_sizes[*out]=i->maxSets; pool_used[*out]=0; ++created_pools; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice,VkDescriptorPool h,const VkAllocationCallbacks*) { check(pools_alive.erase(number(h))==1,"Descriptor pool destroyed exactly once"); }
VKAPI_ATTR VkResult VKAPI_CALL allocate_set(VkDevice,const VkDescriptorSetAllocateInfo *i,VkDescriptorSet *out) { if(fail_native()) return VK_ERROR_OUT_OF_DEVICE_MEMORY; check(pool_used[i->descriptorPool]++<pool_sizes[i->descriptorPool],"Descriptor pool capacity respected"); *out=handle<VkDescriptorSet>(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL reset(VkCommandBuffer,VkCommandBufferResetFlags) { return reset_fails?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*) { return reset_fails?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL free_commands(VkDevice,VkCommandPool,uint32_t,const VkCommandBuffer*) {}
VKAPI_ATTR VkResult VKAPI_CALL reset_pool(VkDevice,VkCommandPool,VkCommandPoolResetFlags) { return reset_fails?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL destroy_command_pool(VkDevice,VkCommandPool,const VkAllocationCallbacks*) { check(buffers.empty()&&memories.empty()&&pools_alive.empty(),"Command-pool native destruction follows arena cleanup"); }
VKAPI_ATTR void VKAPI_CALL destroy_device(VkDevice,const VkAllocationCallbacks*) { check(buffers.empty()&&memories.empty()&&pools_alive.empty()&&!live_resources,"Device native destruction follows arena cleanup/accounting"); }
// State oracle: descriptor layout-prefix signatures and stage-specific bytes.
struct State { std::array<VkDescriptorSet,4> sets{}; std::array<uint32_t,4> dynamic{}; std::array<int,4> prefix{}; std::array<std::array<uint8_t,32>,2> push{}; std::array<int,2> push_layout{}; } state;
std::unordered_map<VkPipelineLayout,std::array<int,4>> signatures;
VKAPI_ATTR void VKAPI_CALL bind_sets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet *sets,uint32_t dc,const uint32_t *offsets) {
    const auto sig=signatures.at(layout);
    for(uint32_t s=0;s<4;++s) if(state.sets[s]) {
        const uint32_t through=s<first?first:s;
        bool compatible=true; for(uint32_t k=0;k<=through;++k) compatible &= state.prefix[k]==sig[k];
        if(!compatible) { state.sets[s]=VK_NULL_HANDLE; state.dynamic[s]=0; }
    }
    for(uint32_t s=0;s<count;++s) { state.sets[first+s]=sets[s]; state.dynamic[first+s]=s<dc?offsets[s]:0; }
    state.prefix=sig;
}
VKAPI_ATTR void VKAPI_CALL push_constants(VkCommandBuffer,VkPipelineLayout layout,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void *data) {
    for(uint32_t s=0;s<2;++s) if(stages&(s?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_COMPUTE_BIT)) { std::memcpy(state.push[s].data()+offset,data,size); state.push_layout[s]=signatures.at(layout)[3]; }
}
bool equal(const State &a,const State &b) { return a.sets==b.sets && a.dynamic==b.dynamic && a.prefix==b.prefix && a.push==b.push && a.push_layout==b.push_layout; }
}
int main() {
    auto d=std::make_shared<Device>(); d->handle=handle<VkDevice>();
    d->CreateBuffer=create_buffer; d->GetBufferMemoryRequirements=requirements; d->AllocateMemory=allocate_memory; d->BindBufferMemory=bind_memory; d->DestroyBuffer=destroy_buffer; d->FreeMemory=free_memory;
    d->CreateDescriptorPool=create_pool; d->DestroyDescriptorPool=destroy_pool; d->AllocateDescriptorSets=allocate_set; d->DestroyPipelineLayout=destroy_layout;
    d->CmdBindDescriptorSets=bind_sets; d->CmdPushConstants=push_constants; d->ResetCommandBuffer=reset; d->BeginCommandBuffer=begin; d->FreeCommandBuffers=free_commands;
    d->ResetCommandPool=reset_pool; d->DestroyCommandPool=destroy_command_pool; d->DestroyDevice=destroy_device;
    d->memory.memoryTypeCount=1; d->memory.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; d->properties.limits.minStorageBufferOffsetAlignment=256;
    auto c=std::make_shared<Command>(); c->device=d; c->level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; auto cb=handle<VkCommandBuffer>(); commands[cb]=c; devices[d->handle]=d;
    pvb::CopyLayout size{}; size.input_bytes=24; size.output_bytes=64;
    if(PVB_UPLOAD_ARENAS) {
        Command fresh; fresh.device=d; Resource r; VkDescriptorSet set{}; uint64_t i=0,o=0;
        optimization_host_fault=true;
        check(upload_allocation(fresh,size,r,set,i,o)==VK_ERROR_OUT_OF_HOST_MEMORY && !created_buffers && !created_pools && fresh.arenas.empty(),"Arena metadata OOM precedes native allocation");
    }
    std::vector<std::array<VkDescriptorBufferInfo,2>> slices;
    std::set<uint64_t> sets;
    for(unsigned n=0;n<130;++n) {
        Resource r; VkDescriptorSet set{}; uint64_t in=0,out=0;
        check(upload_allocation(*c,size,r,set,in,out)==VK_SUCCESS,"Upload allocation succeeds");
        check(in%256==0 && out%256==0,"Suballocation meets SSBO alignment");
        check(sets.insert(number(set)).second,"Every recording slot has a distinct descriptor set");
        std::array<VkDescriptorBufferInfo,2> slice{{{r.input,in,size.input_bytes},{r.output,out,size.output_bytes}}};
        for(const auto &old:slices) for(unsigned b=0;b<2;++b) if(old[b].buffer==slice[b].buffer)
            check(old[b].offset+old[b].range<=slice[b].offset || slice[b].offset+slice[b].range<=old[b].offset,"Executable/replayed recording slices never overlap");
        slices.push_back(slice); c->resources.push_back(r); ++live_resources;
    }
    check(created_buffers==(PVB_UPLOAD_ARENAS?6u:260u) && created_pools==(PVB_UPLOAD_ARENAS?3u:130u),"Native allocation reduction measured across 130 regions / descriptor rollover");
    const auto old_buffers=buffers; const auto old_pools=pools_alive;
    reset_fails=true; VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(pvbResetCommandBuffer(cb,0)!=VK_SUCCESS && buffers==old_buffers && pools_alive==old_pools,"Failed reset keeps executable storage");
    check(pvbBeginCommandBuffer(cb,&bi)!=VK_SUCCESS && buffers==old_buffers,"Failed begin keeps executable storage");
    auto other=std::make_shared<Command>(); other->device=d; Resource r; VkDescriptorSet ds{}; uint64_t in=0,out=0;
    check(upload_allocation(*other,size,r,ds,in,out)==VK_SUCCESS && !old_buffers.count(number(r.input)) && !old_buffers.count(number(r.output)),"Concurrent recording has separate storage");
    other->resources.push_back(r); ++live_resources; reset_command(*other); check(buffers==old_buffers,"Other reset cannot invalidate first recording");
    reset_fails=false; check(pvbResetCommandBuffer(cb,0)==VK_SUCCESS && buffers.empty() && memories.empty() && pools_alive.empty() && !live_resources,"Successful reset frees every owner and accounting");
    for(unsigned f=1;f<=8;++f) {
        operation=0; fault=f; Resource resource; VkDescriptorSet set{}; uint64_t i=0,o=0;
        const auto status=upload_allocation(*c,size,resource,set,i,o); destroy_resource(*d,resource); reset_command(*c);
        check(status!=VK_SUCCESS && buffers.empty() && memories.empty() && pools_alive.empty(),"Native fault rollback/reset releases partial arenas");
    }
    fault=0;
    auto add_layout=[&](std::array<int,4> sig) { auto public_handle=handle<VkPipelineLayout>(); auto shadow=std::make_shared<ShadowLayout>(); shadow->device=d; shadow->handle=handle<VkPipelineLayout>(); signatures[public_handle]=signatures[shadow->handle]=sig; layouts[public_handle]=shadow; return public_handle; };
    const auto a=add_layout({1,2,3,7}),b=add_layout({1,9,3,7}),incompatible_push=add_layout({1,2,3,8});
    auto set0=handle<VkDescriptorSet>(),set1=handle<VkDescriptorSet>(); uint32_t dynamic=12,value=0x13131313;
    for(unsigned n=0;n<1000;++n) { dynamic=n*256; value=n; pvbCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,a,0,1,&set0,1,&dynamic); pvbCmdPushConstants(cb,a,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,4,&value); }
    check(c->binds.size()==(PVB_COMPACT_HISTORY?1u:1000u) && c->pushes.size()==(PVB_COMPACT_HISTORY?1u:1000u),"Only consecutive fully overwritten records compact");
    if(PVB_COMPACT_HISTORY) {
        optimization_host_fault=true;
        pvbCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,a,0,1,&set0,1,&dynamic);
        pvbCmdPushConstants(cb,a,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,4,&value);
        check(optimization_host_fault.exchange(false) && c->error==VK_SUCCESS,"Complete identical-range overwrite needs no new host allocation");
    }
    pvbCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,a,1,1,&set1,0,nullptr);
    pvbCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,b,1,1,&set1,0,nullptr);
    pvbCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,a,0,1,&set0,1,&dynamic);
    check(c->binds.size()==(PVB_COMPACT_HISTORY?4u:1003u),"Different descriptor layout/range boundaries retain disturbance history");
    value=0xaabbccdd; pvbCmdPushConstants(cb,a,VK_SHADER_STAGE_VERTEX_BIT,4,4,&value);
    value=0x44332211; pvbCmdPushConstants(cb,a,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&value);
    value=0xeeeeeeee; pvbCmdPushConstants(cb,incompatible_push,VK_SHADER_STAGE_VERTEX_BIT,8,4,&value);
    value=0x76543210; pvbCmdPushConstants(cb,a,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,12,4,&value);
    check(c->pushes.size()==(PVB_COMPACT_HISTORY?5u:1004u),"Different stage, byte and layout boundaries remain distinct");
    const State expected=state; state={}; replay_state(cb,*c);
    check(equal(state,expected),"Replay preserves layout disturbance, dynamic offsets, mixed stages and partial bytes");
    state={}; const auto last_bind=c->binds.back(); bind_sets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,last_bind.layout->handle,last_bind.first,uint32_t(last_bind.sets.size()),last_bind.sets.data(),uint32_t(last_bind.offsets.size()),last_bind.offsets.data());
    const auto last_push=c->pushes.back(); push_constants(cb,last_push.layout->handle,last_push.stages,last_push.offset,uint32_t(last_push.data.size()),last_push.data.data());
    check(!equal(state,expected),"Failing control: naive last-bind/last-push loses stage and byte state");
    pvbFreeCommandBuffers(d->handle,VK_NULL_HANDLE,1,&cb); check(c->resources.empty()&&c->arenas.empty()&&c->binds.empty()&&c->pushes.empty(),"Free clears all new recording state");
    const auto command_pool=handle<VkCommandPool>();
    auto owned_recording=[&](VkCommandPool pool) {
        auto owned=std::make_shared<Command>(); owned->device=d; owned->pool=pool;
        Resource resource; VkDescriptorSet set{}; uint64_t i=0,o=0;
        check(upload_allocation(*owned,size,resource,set,i,o)==VK_SUCCESS,"Cleanup fixture owns upload resources");
        owned->resources.push_back(resource); ++live_resources; commands[handle<VkCommandBuffer>()]=owned; return owned;
    };
    auto pool_owner1=owned_recording(command_pool),pool_owner2=owned_recording(command_pool);
    reset_fails=true; const auto pool_buffers=buffers;
    check(pvbResetCommandPool(d->handle,command_pool,0)!=VK_SUCCESS && buffers==pool_buffers,"Failed pool reset preserves both recordings");
    reset_fails=false; check(pvbResetCommandPool(d->handle,command_pool,0)==VK_SUCCESS && buffers.empty() && !live_resources,"Successful pool reset releases both recordings");
    auto pool_owner3=owned_recording(command_pool);
    pvbDestroyCommandPool(d->handle,command_pool,nullptr);
    check(commands.empty() && pool_owner3->arenas.empty(),"Pool destruction revokes arena recordings");
    auto device_owner=owned_recording(handle<VkCommandPool>());
    pvbDestroyDevice(d->handle,nullptr);
    check(commands.empty() && devices.empty() && device_owner->arenas.empty(),"Device cleanup revokes recording owners");
    layouts.clear(); devices.clear();
    std::printf("OPTIMIZATION_OWNERSHIP_RESULT: %u passed; %u failed\n",passed,failed); return failed?1:0;
}
