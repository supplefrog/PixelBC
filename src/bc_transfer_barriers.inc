// SPDX-License-Identifier: MIT
// Included inside wrapper.cpp's private namespace. The canonical store performs
// exactly the same transfer accesses as the emulated image. Mirror image memory
// dependencies to its mip/layer ranges; keep caller stages, ownership and flags.
template<class Emit> bool block_barriers(Command &c,const Image &image,const VkImageSubresourceRange &range,Emit emit) {
    if(range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT || range.baseMipLevel>=image.mips || range.baseArrayLayer>=image.layers) {
        fail(c,VK_ERROR_UNKNOWN,"Unsupported BC barrier subresources"); return false;
    }
    const auto levels=range.levelCount==VK_REMAINING_MIP_LEVELS?image.mips-range.baseMipLevel:range.levelCount;
    const auto layers=range.layerCount==VK_REMAINING_ARRAY_LAYERS?image.layers-range.baseArrayLayer:range.layerCount;
    if(!levels || levels>image.mips-range.baseMipLevel || !layers || layers>image.layers-range.baseArrayLayer || !retain_store(c,image)) {
        fail(c,VK_ERROR_UNKNOWN,"Unsupported BC barrier range"); return false;
    }
    for(uint32_t mip=range.baseMipLevel;mip<range.baseMipLevel+levels;++mip) {
        const auto &layout=image.blocks->mips[mip];
        emit(image.blocks->buffer,layout.offset+uint64_t(range.baseArrayLayer)*layout.layer_bytes,uint64_t(layers)*layout.layer_bytes);
    }
    return true;
}
bool translated_barriers(Command &c,uint32_t buffer_count,const VkBufferMemoryBarrier *buffers,uint32_t image_count,const VkImageMemoryBarrier *images_in,std::vector<VkBufferMemoryBarrier> &out) {
    if(buffer_count) out.assign(buffers,buffers+buffer_count);
    for(uint32_t n=0;n<image_count;++n) {
        const auto &barrier=images_in[n]; const auto image=lookup(images,barrier.image); if(!image.device) continue;
        if(barrier.pNext || c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY) { fail(c,VK_ERROR_UNKNOWN,"Unsupported BC image barrier extension/secondary buffer"); return false; }
        if(!image.blocks) continue;
        if(!block_barriers(c,image,barrier.subresourceRange,[&](VkBuffer buffer,uint64_t offset,uint64_t size) {
            VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; b.srcAccessMask=barrier.srcAccessMask; b.dstAccessMask=barrier.dstAccessMask;
            b.srcQueueFamilyIndex=barrier.srcQueueFamilyIndex; b.dstQueueFamilyIndex=barrier.dstQueueFamilyIndex; b.buffer=buffer; b.offset=offset; b.size=size; out.push_back(b);
        })) return false;
    }
    return true;
}
VKAPI_ATTR void VKAPI_CALL pvbCmdPipelineBarrier(VkCommandBuffer handle,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t memory_count,const VkMemoryBarrier *memory,uint32_t buffer_count,const VkBufferMemoryBarrier *buffers,uint32_t image_count,const VkImageMemoryBarrier *image_barriers) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    std::vector<VkBufferMemoryBarrier> translated;
    if(translated_barriers(*c,buffer_count,buffers,image_count,image_barriers,translated))
        c->device->CmdPipelineBarrier(handle,src,dst,flags,memory_count,memory,uint32_t(translated.size()),translated.data(),image_count,image_barriers);
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC barrier translation allocation failed"); }
VKAPI_ATTR void VKAPI_CALL pvbCmdWaitEvents(VkCommandBuffer handle,uint32_t event_count,const VkEvent *events,VkPipelineStageFlags src,VkPipelineStageFlags dst,uint32_t memory_count,const VkMemoryBarrier *memory,uint32_t buffer_count,const VkBufferMemoryBarrier *buffers,uint32_t image_count,const VkImageMemoryBarrier *image_barriers) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    std::vector<VkBufferMemoryBarrier> translated;
    if(translated_barriers(*c,buffer_count,buffers,image_count,image_barriers,translated))
        c->device->CmdWaitEvents(handle,event_count,events,src,dst,memory_count,memory,uint32_t(translated.size()),translated.data(),image_count,image_barriers);
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC event barrier translation allocation failed"); }
bool translated_barriers2(Command &c,const VkDependencyInfo &info,std::vector<VkBufferMemoryBarrier2> &out) {
    if(info.bufferMemoryBarrierCount) out.assign(info.pBufferMemoryBarriers,info.pBufferMemoryBarriers+info.bufferMemoryBarrierCount);
    for(uint32_t n=0;n<info.imageMemoryBarrierCount;++n) {
        const auto &barrier=info.pImageMemoryBarriers[n]; const auto image=lookup(images,barrier.image); if(!image.device) continue;
        if(info.pNext || barrier.pNext || c.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY) { fail(c,VK_ERROR_UNKNOWN,"Unsupported BC synchronization2 extension/secondary buffer"); return false; }
        if(!image.blocks) continue;
        if(!block_barriers(c,image,barrier.subresourceRange,[&](VkBuffer buffer,uint64_t offset,uint64_t size) {
            VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}; b.srcStageMask=barrier.srcStageMask; b.srcAccessMask=barrier.srcAccessMask; b.dstStageMask=barrier.dstStageMask; b.dstAccessMask=barrier.dstAccessMask;
            b.srcQueueFamilyIndex=barrier.srcQueueFamilyIndex; b.dstQueueFamilyIndex=barrier.dstQueueFamilyIndex; b.buffer=buffer; b.offset=offset; b.size=size; out.push_back(b);
        })) return false;
    }
    return true;
}
VKAPI_ATTR void VKAPI_CALL pvbCmdPipelineBarrier2(VkCommandBuffer handle,const VkDependencyInfo *info) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    std::vector<VkBufferMemoryBarrier2> translated;
    if(!translated_barriers2(*c,*info,translated)) return;
    auto copy=*info; copy.bufferMemoryBarrierCount=uint32_t(translated.size()); copy.pBufferMemoryBarriers=translated.data();
    auto native=c->device->CmdPipelineBarrier2; if(!native) native=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(c->device->gdpa(c->device->handle,"vkCmdPipelineBarrier2KHR"));
    native(handle,&copy);
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC synchronization2 translation allocation failed"); }
VKAPI_ATTR void VKAPI_CALL pvbCmdSetEvent2(VkCommandBuffer handle,VkEvent event,const VkDependencyInfo *info) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    std::vector<VkBufferMemoryBarrier2> translated;
    if(!translated_barriers2(*c,*info,translated)) return;
    auto copy=*info; copy.bufferMemoryBarrierCount=uint32_t(translated.size()); copy.pBufferMemoryBarriers=translated.data();
    auto native=c->device->CmdSetEvent2; if(!native) native=reinterpret_cast<PFN_vkCmdSetEvent2>(c->device->gdpa(c->device->handle,"vkCmdSetEvent2KHR"));
    native(handle,event,&copy);
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC event2 translation allocation failed"); }
VKAPI_ATTR void VKAPI_CALL pvbCmdWaitEvents2(VkCommandBuffer handle,uint32_t count,const VkEvent *events,const VkDependencyInfo *info) try {
    auto c=lookup(commands,handle); if(!c || c->error!=VK_SUCCESS) return;
    std::vector<VkDependencyInfo> dependencies; if(count) dependencies.assign(info,info+count);
    std::vector<std::vector<VkBufferMemoryBarrier2>> buffers(count);
    for(uint32_t n=0;n<count;++n) {
        if(!translated_barriers2(*c,info[n],buffers[n])) return;
        dependencies[n].bufferMemoryBarrierCount=uint32_t(buffers[n].size()); dependencies[n].pBufferMemoryBarriers=buffers[n].data();
    }
    auto native=c->device->CmdWaitEvents2; if(!native) native=reinterpret_cast<PFN_vkCmdWaitEvents2>(c->device->gdpa(c->device->handle,"vkCmdWaitEvents2KHR"));
    native(handle,count,events,dependencies.data());
} catch(const std::bad_alloc&) { auto c=lookup(commands,handle); if(c) fail(*c,VK_ERROR_OUT_OF_HOST_MEMORY,"BC event2 wait translation allocation failed"); }
