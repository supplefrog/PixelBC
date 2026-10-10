// SPDX-License-Identifier: MIT
// Exercise actual cached Vulkan encoder creation, reuse, rejection and teardown.
#define PVB_ETC2_NO_MAIN
#include "etc2_probe.cpp"

namespace {
Bytes opaque_black(uint32_t side) {
    Bytes bc(size_t((side+3)/4)*((side+3)/4)*16);
    for(size_t n=0;n<bc.size();n+=16) bc[n]=255; // BC3 selector0: genuine255.
    return bc;
}
void source_quality(const Bytes &bc,const Bytes &etc) {
    bool valid=true;
    for(size_t n=0;n<bc.size()/16;++n) {
        const auto source=reference(VK_FORMAT_BC3_UNORM_BLOCK,bc.data()+n*16);
        const auto decoded=etc_reference(VK_FORMAT_BC3_UNORM_BLOCK,etc.data()+n*16);
        for(uint32_t p=0;p<16;++p) {
            for(uint32_t c=0;c<3;++c) valid&=std::abs(int(source[p*4+c])-int(decoded[p*4+c]))<=32;
            valid&=source[p*4+3]==255 && decoded[p*4+3]==255;
        }
    }
    check(valid,"Cache fixture real GPU ETC independently decodes near source black with exact genuine255 alpha");
}
void cache_checks(EncoderApp &a) {
    constexpr auto f=VK_FORMAT_BC3_UNORM_BLOCK;
    const auto small=opaque_black(4),large=opaque_black(16);
    const auto first=encode(a,f,small,4,4,1);source_quality(small,first);
    const auto initial_pipeline=a.encoders.front()->resources.pipeline;
    const auto repeat=encode(a,f,small,4,4,1);
    const auto expanded=encode(a,f,large,16,16,1);source_quality(large,expanded);
    const auto shrunk=encode(a,f,small,4,4,1);
    check(first==repeat && first==shrunk,"Actual same-source GPU bytes survive repeated, growing and shrinking cache calls");
    check(a.encoder_creations==1 && a.encoders.size()==1 && a.encoders.front()->resources.pipeline==initial_pipeline && a.encoder_buffer_growths==4,"Grow/shrink/repeat uses one live encoder pipeline and grows only two buffer pairs");
    auto same=production_encoder(f);
    const auto identical=encode(a,f,small,4,4,1,&same);
    check(identical==first && a.encoder_creations==1,"Equal explicit module bytes reuse the production pipeline regardless of vector address");
    auto changed=same;changed[2]^=0x10000000u; // Legal nonsemantic SPIR-V generator word.
    const auto changed_output=encode(a,f,small,4,4,1,&changed);source_quality(small,changed_output);
    check(changed_output==first && a.encoder_creations==2 && a.encoders.size()==2 && a.encoders[1]->resources.pipeline!=initial_pipeline,"Different exact SPIR-V bytes create a distinct actual pipeline, even with identical semantics");
    const auto creations=a.encoder_creations,growths=a.encoder_buffer_growths,dispatches=a.encoder_dispatches,reuses=a.encoder_reuses;
    auto &resources=a.encoders.front()->resources;
    resources.begin();
    bool rejected=false;
    try { (void)encode(a,f,large,16,16,1); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && resources.recorded && a.encoder_creations==creations && a.encoder_buffer_growths==growths && a.encoder_dispatches==dispatches && a.encoder_reuses==reuses,"Recording guard rejects real command-buffer reuse before descriptors, allocations or dispatch change");
    auto another=changed;another[2]^=0x20000000u;
    rejected=false;
    try { (void)encode(a,f,large,16,16,1,&another); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && resources.recorded && !a.encoders[1]->resources.recorded && a.encoders.size()==2 && a.encoder_creations==creations && a.encoder_buffer_growths==growths && a.encoder_dispatches==dispatches && a.encoder_reuses==reuses,"Recording guard rejects a different new module before another context, descriptors, allocations or dispatch change");
    require(a.EndCommandBuffer(a.command),"Cache fixture pending command end");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&a.command;
    require(a.QueueSubmit(a.queue,1,&submit,a.fence),"Cache fixture real pending submit");a.submission_pending=true;
    rejected=false;
    try { (void)encode(a,f,large,16,16,1,&changed); } catch(const std::runtime_error&) { rejected=true; }
    check(rejected && a.submission_pending && a.encoder_creations==creations && a.encoder_buffer_growths==growths && a.encoder_dispatches==dispatches && a.encoder_reuses==reuses,"Pending-submission guard rejects cache use before Vulkan resource or dispatch change");
    require(a.WaitForFences(a.device,1,&a.fence,VK_TRUE,30000000000ull),"Cache fixture pending wait");a.submission_pending=false;
    require(a.ResetFences(a.device,1,&a.fence),"Cache fixture pending fence reset");resources.reset("Cache fixture pending command reset");
    const auto resumed=encode(a,f,small,4,4,1);
    check(resumed==first && a.encoder_creations==2,"Cache resumes correct real encoding after completion and reset");
}
}
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc!=2 && argc!=3) { std::fprintf(stderr,"Usage: pixel_etc2_encoder_cache explicit-library.so [--validation]\n");return 2; }
    try {
        const bool validation=argc==3 && std::string(argv[2])=="--validation";
        if(argc==3 && !validation) throw std::runtime_error("Unknown cache fixture option");
        EncoderApp a;init(a,argv[1],validation);oracle_anchors();cache_checks(a);
        a.shutdown();check(a.encoders.empty() && !a.device && !a.library,"Explicit cached encoder shutdown destroys contexts before releasing the device/loader");a.shutdown();
        bool unwound=false;
        try {
            EncoderApp exception_app;init(exception_app,argv[1],validation);
            const auto bc=opaque_black(4),etc=encode(exception_app,VK_FORMAT_BC3_UNORM_BLOCK,bc,4,4,1);source_quality(bc,etc);
            stage("cache fixture intentional exception before derived owner unwind");
            throw std::runtime_error("cache fixture intentional unwind");
        } catch(const std::runtime_error &e) { if(std::string(e.what())!="cache fixture intentional unwind") throw;unwound=true; }
        check(unwound,"Actual cached encoder owner completes exception unwinding");
        check(!validation_errors,"Cached encoder reuse and explicit/exception teardown produce no Vulkan validation errors");
        std::printf("ETC_ENCODER_CACHE_FIXTURE {\"passed\":%u,\"failed\":%u,\"validation_errors\":%u,\"real_GPU_dispatches\":true}\n",passed,failed,validation_errors);
        return failed || validation_errors?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"ETC_ENCODER_CACHE_ERROR: %s\n",e.what());return 2; }
}
