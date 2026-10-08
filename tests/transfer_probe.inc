// SPDX-License-Identifier: MIT
// Actual GPU transfer checks. Expected compressed bytes are assembled directly
// from the caller's blocks, independently of the wrapper's canonical layout.
PvbTransferStats transfer_stats(App &a) {
    auto get=reinterpret_cast<PvbGetTransferStats>(dlsym(a.library,"pvb_get_transfer_stats"));
    if(!get) throw std::runtime_error("Missing additive transfer diagnostics");
    PvbTransferStats stats{}; get(&stats); return stats;
}
void transfer_image(App &a,VkFormat format,VkImage &image,VkDeviceMemory &memory,bool source=true) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType=VK_IMAGE_TYPE_2D; info.format=format; info.extent={7,7,1}; info.mipLevels=3; info.arrayLayers=6;
    info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(source) info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    require(a.CreateImage(a.device,&info,nullptr,&image),"Transfer image create");
    VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,image,&req);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=req.size; allocation.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0);
    require(a.AllocateMemory(a.device,&allocation,nullptr,&memory),"Transfer image memory"); require(a.BindImageMemory(a.device,image,memory,0),"Transfer image bind");
}
void transfer_barrier(App &a,VkImage image,VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags src,VkAccessFlags dst,bool sync2=false) {
    if(sync2) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask=old_layout==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_2_NONE:VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask=src; b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask=dst;
        b.oldLayout=old_layout; b.newLayout=new_layout; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,VK_REMAINING_MIP_LEVELS,0,VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount=1; info.pImageMemoryBarriers=&b;
        a.dp<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2KHR")(a.command,&info); ++alias_tests;
    } else {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask=src; b.dstAccessMask=dst;
        b.oldLayout=old_layout; b.newLayout=new_layout; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,3,0,6};
        a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
}
void transfer_copy(App &a,VkImage src,VkImage dst,const VkImageCopy *regions,uint32_t count,uint32_t variant) {
    if(!variant) { a.CmdCopyImage(a.command,src,VK_IMAGE_LAYOUT_GENERAL,dst,VK_IMAGE_LAYOUT_GENERAL,count,regions); return; }
    std::vector<VkImageCopy2> copies(count);
    for(uint32_t n=0;n<count;++n) { auto &r=copies[n]; r.sType=VK_STRUCTURE_TYPE_IMAGE_COPY_2; r.srcSubresource=regions[n].srcSubresource; r.srcOffset=regions[n].srcOffset; r.dstSubresource=regions[n].dstSubresource; r.dstOffset=regions[n].dstOffset; r.extent=regions[n].extent; }
    VkCopyImageInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2}; info.srcImage=src; info.srcImageLayout=VK_IMAGE_LAYOUT_GENERAL; info.dstImage=dst; info.dstImageLayout=VK_IMAGE_LAYOUT_GENERAL; info.regionCount=count; info.pRegions=copies.data();
    a.dp<PFN_vkCmdCopyImage2>(variant==2?"vkCmdCopyImage2KHR":"vkCmdCopyImage2")(a.command,&info); if(variant==2) ++alias_tests;
}
void transfer_readback(App &a,VkImage image,VkBuffer buffer,const VkBufferImageCopy &region,uint32_t variant) {
    if(!variant) { a.CmdCopyImageToBuffer(a.command,image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&region); return; }
    VkBufferImageCopy2 r{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2}; r.bufferOffset=region.bufferOffset; r.bufferRowLength=region.bufferRowLength; r.bufferImageHeight=region.bufferImageHeight; r.imageSubresource=region.imageSubresource; r.imageOffset=region.imageOffset; r.imageExtent=region.imageExtent;
    VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2}; info.srcImage=image; info.srcImageLayout=VK_IMAGE_LAYOUT_GENERAL; info.dstBuffer=buffer; info.regionCount=1; info.pRegions=&r;
    a.dp<PFN_vkCmdCopyImageToBuffer2>(variant==2?"vkCmdCopyImageToBuffer2KHR":"vkCmdCopyImageToBuffer2")(a.command,&info); if(variant==2) ++alias_tests;
}
void transfer_host_barrier(App &a,VkBuffer buffer,VkAccessFlags access) {
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; b.srcAccessMask=access; b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.buffer=buffer; b.size=VK_WHOLE_SIZE;
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);
}
void transfer_case(App &a,VkFormat format,bool sample) {
    DiagnosticResources r(a); const auto initial=transfer_stats(a); const uint32_t bs=pvb::block_bytes(format);
    r.input=a.buffer(8192,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.compressed=a.buffer(8192,VK_BUFFER_USAGE_TRANSFER_DST_BIT); r.output=a.buffer(7*7*6*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(r.input.mapped,0xcc,8192); std::memset(r.compressed.mapped,0xa5,8192);
    const auto paired=VkFormat(uint32_t(format)%2?uint32_t(format)+1:uint32_t(format)-1);
    const auto dst_format=pvb::compatible_view(format,paired)?paired:format;
    transfer_image(a,format,r.image,r.memory); transfer_image(a,format,r.transfer_images[0],r.transfer_memory[0]); transfer_image(a,dst_format,r.transfer_images[1],r.transfer_memory[1]);
    auto stats=transfer_stats(a); check(stats.live_block_stores==initial.live_block_stores+3 && stats.live_block_bytes==initial.live_block_bytes+uint64_t(4+1+1)*6*bs*3,"Canonical compressed allocation covers exact mip/layer content");
    std::array<std::vector<uint8_t>,3> expected; std::array<VkBufferImageCopy,3> upload{},read{}; std::array<VkImageCopy,3> full{};
    uint32_t seed=0x639156d1u+uint32_t(format); std::vector<uint8_t> exact(8192,0xa5);
    for(uint32_t mip=0;mip<3;++mip) {
        const uint32_t w=7u>>mip,blocks=(w+3)/4; expected[mip].resize(size_t(blocks)*blocks*6*bs);
        for(auto &b:expected[mip]) b=uint8_t(random_word(seed));
        auto &u=upload[mip]; u.bufferOffset=64+mip*2048; u.bufferRowLength=16; u.bufferImageHeight=12; u.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,6}; u.imageExtent={w,w,1};
        for(uint32_t layer=0;layer<6;++layer) for(uint32_t y=0;y<blocks;++y) std::memcpy(static_cast<uint8_t*>(r.input.mapped)+u.bufferOffset+(layer*3+y)*4*bs,expected[mip].data()+(layer*blocks+y)*blocks*bs,blocks*bs);
        read[mip]=u; read[mip].bufferOffset=128+mip*2048; read[mip].bufferRowLength=12; read[mip].bufferImageHeight=16;
        full[mip].srcSubresource=full[mip].dstSubresource=u.imageSubresource; full[mip].extent=u.imageExtent;
    }
    std::vector<uint8_t> update(bs); for(auto &b:update) b=uint8_t(random_word(seed)); std::memcpy(static_cast<uint8_t*>(r.input.mapped)+7168,update.data(),bs);
    // Initialize both untouched blocks and source bytes, then update only a
    // final edge block on face2. Copy that3x3 region into mip1 on face4.
    VkBufferImageCopy patch{}; patch.bufferOffset=7168; patch.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,2,1}; patch.imageOffset={4,4,0}; patch.imageExtent={3,3,1};
    std::memcpy(expected[0].data()+(2*4+3)*bs,update.data(),bs); std::memcpy(expected[1].data()+4*bs,update.data(),bs);
    for(uint32_t mip=0;mip<3;++mip) { const uint32_t blocks=((7u>>mip)+3)/4; for(uint32_t layer=0;layer<6;++layer) for(uint32_t y=0;y<blocks;++y) std::memcpy(exact.data()+read[mip].bufferOffset+(layer*4+y)*3*bs,expected[mip].data()+(layer*blocks+y)*blocks*bs,blocks*bs); }
    VkDescriptorSet set{};
    if(sample) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=r.transfer_images[1]; view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format=dst_format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6}; require(a.CreateImageView(a.device,&view,nullptr,&r.view),"Transfer sample view");
        VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=2; pool.pPoolSizes=sizes; require(a.CreateDescriptorPool(a.device,&pool,nullptr,&r.pool),"Transfer sample descriptors");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=r.pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&a.descriptors; require(a.AllocateDescriptorSets(a.device,&allocate,&set),"Transfer sample set");
        VkDescriptorImageInfo image{a.sampler,r.view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buffer{r.output.handle,0,7*7*6*4}; VkWriteDescriptorSet writes[2]{};
        for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; }
        writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&image; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&buffer; a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    }
    r.begin(); const bool sync2=uint32_t(format)%2==0;
    for(auto image:{r.image,r.transfer_images[0],r.transfer_images[1]}) transfer_barrier(a,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,sync2);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,3,upload.data());
    transfer_barrier(a,r.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,sync2);
    a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&patch);
    transfer_barrier(a,r.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,sync2);
    transfer_copy(a,r.image,r.transfer_images[0],full.data(),3,uint32_t(format)%3);
    transfer_barrier(a,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,sync2);
    VkImageCopy partial{}; partial.srcSubresource=patch.imageSubresource; partial.srcOffset=patch.imageOffset; partial.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,4,1}; partial.extent=patch.imageExtent;
    transfer_copy(a,r.image,r.transfer_images[0],&partial,1,(uint32_t(format)+1)%3);
    transfer_barrier(a,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,sync2);
    transfer_copy(a,r.transfer_images[0],r.transfer_images[1],full.data(),3,(uint32_t(format)+2)%3);
    transfer_barrier(a,r.transfer_images[1],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_READ_BIT,sync2);
    for(uint32_t mip=0;mip<3;++mip) transfer_readback(a,r.transfer_images[1],r.compressed.handle,read[mip],mip);
    transfer_host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT);
    if(sample) {
        const uint32_t push[8]={7,7,6,0,pvb::is_signed(format)?1u:0u,pvb::rgba_format(dst_format)==VK_FORMAT_R8G8B8A8_SRGB?1u:0u,0,0};
        a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push); a.CmdDispatch(a.command,1,1,6); transfer_host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT);
    }
    a.submit(sync2); check(!std::memcmp(r.compressed.mapped,exact.data(),exact.size()),"BC image-copy chain and edge subregion preserve all compressed bytes and padding");
    if(sample) {
        bool equal=true; auto actual=static_cast<const uint8_t*>(r.output.mapped);
        for(uint32_t layer=0;layer<6;++layer) for(uint32_t y=0;y<7;++y) for(uint32_t x=0;x<7;++x) { const auto rgba=reference(format,expected[0].data()+((layer*2+y/4)*2+x/4)*bs); for(uint32_t channel=0;channel<4;++channel) { const auto got=actual[((layer*7+y)*7+x)*4+channel],want=rgba[((y%4)*4+x%4)*4+channel]; const int delta=pvb::is_signed(format)?std::abs(int(int8_t(got))-int(int8_t(want))):std::abs(int(got)-int(want)); if(delta>1) equal=false; } }
        check(equal,"Copied decoded pixels match independent blocks through UNORM/SRGB/signed sampling");
    }
    std::memset(r.compressed.mapped,0xa5,8192); a.submit_ended(sync2); check(!std::memcmp(r.compressed.mapped,exact.data(),exact.size()),"Image-copy command buffer resubmits with original retained blocks");
    // The source image may be destroyed after completion. Destination readback
    // remains faithful after executable references are reset (no invalid replay).
    a.DestroyImage(a.device,std::exchange(r.image,VK_NULL_HANDLE),nullptr); a.FreeMemory(a.device,std::exchange(r.memory,VK_NULL_HANDLE),nullptr);
    stats=transfer_stats(a); check(stats.live_block_stores==initial.live_block_stores+3,"Completed recording retains source block store until reset");
    r.reset("Transfer chain reset"); stats=transfer_stats(a); check(stats.live_block_stores==initial.live_block_stores+2,"Reset releases destroyed source's retained store");
    r.begin(); for(uint32_t mip=0;mip<3;++mip) transfer_readback(a,r.transfer_images[1],r.compressed.handle,read[mip],mip); transfer_host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT); a.submit();
    check(!std::memcmp(r.compressed.mapped,exact.data(),exact.size()),"Destination block readback survives legal completed-source destruction"); r.reset("Transfer destination readback reset"); r.clear(); stats=transfer_stats(a);
    check(stats.live_block_stores==initial.live_block_stores && stats.live_block_bytes==initial.live_block_bytes,"Transfer fixture releases all block stores and bytes");
}
void transfer_semaphore_test(App &a,bool producer2) {
    DiagnosticResources r(a); const auto initial=transfer_stats(a);
    r.input=a.buffer(512,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.compressed=a.buffer(512,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for(uint32_t n=0;n<192;++n) static_cast<uint8_t*>(r.input.mapped)[n]=uint8_t(n*73+29);
    std::memset(r.compressed.mapped,0xa5,512);
    transfer_image(a,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,r.image,r.memory); transfer_image(a,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,r.transfer_images[0],r.transfer_memory[0]);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; require(a.CreateSemaphore(a.device,&semaphore,nullptr,&r.transfer_semaphore),"Transfer semaphore create");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=a.pool; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    require(a.AllocateCommandBuffers(a.device,&allocation,&r.transfer_command),"Transfer consumer command");
    r.begin(); transfer_barrier(a,r.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,true);
    VkBufferImageCopy upload{}; upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,6}; upload.imageExtent={7,7,1}; a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&upload);
    require(a.EndCommandBuffer(a.command),"Transfer producer end");
    if(producer2) {
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer=a.command;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; signal.semaphore=r.transfer_semaphore; signal.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; info.commandBufferInfoCount=1; info.pCommandBufferInfos=&command; info.signalSemaphoreInfoCount=1; info.pSignalSemaphoreInfos=&signal; require(a.QueueSubmit2(a.queue,1,&info,VK_NULL_HANDLE),"Transfer producer submit2");
    } else {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.commandBufferCount=1; info.pCommandBuffers=&a.command; info.signalSemaphoreCount=1; info.pSignalSemaphores=&r.transfer_semaphore; require(a.QueueSubmit(a.queue,1,&info,VK_NULL_HANDLE),"Transfer producer submit");
    }
    a.submission_pending=true;
    // Keep the fixture's extra command distinct from App.command during any
    // exception recovery. Both are destroyed only after GPU completion.
    std::swap(a.command,r.transfer_command); a.begin(); transfer_barrier(a,r.transfer_images[0],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,true);
    VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource=upload.imageSubresource; copy.extent=upload.imageExtent; transfer_copy(a,r.image,r.transfer_images[0],&copy,1,producer2?0:2);
    transfer_barrier(a,r.transfer_images[0],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,true); transfer_readback(a,r.transfer_images[0],r.compressed.handle,upload,producer2?2:0); transfer_host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT);
    require(a.EndCommandBuffer(a.command),"Transfer consumer end");
    if(producer2) {
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT; VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.commandBufferCount=1; info.pCommandBuffers=&a.command; info.waitSemaphoreCount=1; info.pWaitSemaphores=&r.transfer_semaphore; info.pWaitDstStageMask=&stage; require(a.QueueSubmit(a.queue,1,&info,a.fence),"Transfer consumer submit waits TRANSFER");
    } else {
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer=a.command;
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; wait.semaphore=r.transfer_semaphore; wait.stageMask=VK_PIPELINE_STAGE_2_COPY_BIT;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; info.commandBufferInfoCount=1; info.pCommandBufferInfos=&command; info.waitSemaphoreInfoCount=1; info.pWaitSemaphoreInfos=&wait; require(a.QueueSubmit2(a.queue,1,&info,a.fence),"Transfer consumer submit2 waits COPY");
    }
    require(a.WaitForFences(a.device,1,&a.fence,VK_TRUE,30000000000ull),"Transfer semaphore fence"); a.submission_pending=false; require(a.ResetFences(a.device,1,&a.fence),"Transfer semaphore fence reset");
    check(!std::memcmp(r.input.mapped,r.compressed.mapped,192),"Native semaphore COPY/TRANSFER wait makes retained upload blocks visible to image-copy/readback");
    bool padding=true; for(uint32_t n=192;n<512;++n) if(static_cast<const uint8_t*>(r.compressed.mapped)[n]!=0xa5) padding=false; check(padding,"Semaphore readback writes only original compressed bytes");
    r.reset("Transfer consumer reset"); require(a.ResetCommandBuffer(r.transfer_command,0),"Transfer producer reset"); std::swap(a.command,r.transfer_command); r.clear(); const auto final=transfer_stats(a);
    check(final.live_block_stores==initial.live_block_stores && final.live_block_bytes==initial.live_block_bytes,"Semaphore fixture frees retained command/image stores");
}
void transfer_event_test(App &a,uint32_t variant) {
    DiagnosticResources r(a); const auto initial=transfer_stats(a);
    r.input=a.buffer(192,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); r.compressed=a.buffer(192,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for(uint32_t n=0;n<192;++n) static_cast<uint8_t*>(r.input.mapped)[n]=uint8_t(n*39+17);
    transfer_image(a,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,r.image,r.memory);
    VkEventCreateInfo create{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO}; require(a.CreateEvent(a.device,&create,nullptr,&r.transfer_event),"Transfer event create");
    r.begin(); transfer_barrier(a,r.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy upload{}; upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,6}; upload.imageExtent={7,7,1}; a.CmdCopyBufferToImage(a.command,r.input.handle,r.image,VK_IMAGE_LAYOUT_GENERAL,1,&upload);
    if(!variant) {
        a.CmdSetEvent(a.command,r.transfer_event,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.image=r.image; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
        a.CmdWaitEvents(a.command,1,&r.transfer_event,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,nullptr,0,nullptr,1,&b);
    } else {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.image=r.image; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
        VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount=1; info.pImageMemoryBarriers=&b;
        a.dp<PFN_vkCmdSetEvent2>(variant==2?"vkCmdSetEvent2KHR":"vkCmdSetEvent2")(a.command,r.transfer_event,&info);
        a.dp<PFN_vkCmdWaitEvents2>(variant==2?"vkCmdWaitEvents2KHR":"vkCmdWaitEvents2")(a.command,1,&r.transfer_event,&info); if(variant==2) alias_tests+=2;
    }
    transfer_readback(a,r.image,r.compressed.handle,upload,variant); transfer_host_barrier(a,r.compressed.handle,VK_ACCESS_TRANSFER_WRITE_BIT); a.submit();
    check(!std::memcmp(r.input.mapped,r.compressed.mapped,192),"Legacy/core2/KHR event image dependencies synchronize retained compressed bytes"); r.reset("Transfer event reset"); r.clear(); const auto final=transfer_stats(a);
    check(final.live_block_stores==initial.live_block_stores && final.live_block_bytes==initial.live_block_bytes,"Event fixture releases all canonical storage");
}
void transfer_tests(App &a) {
    stage("retained BC image-copy/readback cases begin");
    for(uint32_t format=131;format<=142;++format) transfer_case(a,VkFormat(format),true);
    for(auto format:{VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK}) if(pvb::is_bc(format)) transfer_case(a,format,false);
    transfer_semaphore_test(a,false); transfer_semaphore_test(a,true);
    for(uint32_t variant=0;variant<3;++variant) transfer_event_test(a,variant);
    stage("retained BC image-copy/readback cases complete");
}
