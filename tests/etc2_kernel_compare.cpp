// SPDX-License-Identifier: MIT
// Explicit real GPU modules, resident BC buffers, independent BC/detex gates.
#define PVB_ETC2_NO_MAIN
#include "etc2_probe.cpp"

namespace {
std::array<uint32_t,2> shader_failures{};
struct CompareOptions {
    uint32_t size=256,warmups=3,repeats=5,rounds=3;
    bool validation=false,quality_only=false,timing_only=false;
};
void timed_quality(VkFormat f,const Bytes &bc,const uint8_t *etc,uint32_t blocks,uint32_t shader,uint32_t pattern,uint32_t round) {
    ErrorMetric rgb,alpha;uint64_t extremes=0;
    for(uint32_t n=0;n<blocks;++n) {
        const auto source=reference(f,bc.data()+size_t(n)*pvb::block_bytes(f));
        const auto decoded=etc_reference(f,etc+size_t(n)*etc_bytes(f));
        for(uint32_t p=0;p<16;++p) {
            for(uint32_t c=0;c<3;++c) if(source[p*4+3]) rgb.add(uint32_t(std::abs(int(source[p*4+c])-int(decoded[p*4+c]))));
            const uint32_t e=uint32_t(std::abs(int(source[p*4+3])-int(decoded[p*4+3])));
            if(e>alpha.maximum && e>16) {
                std::printf("KERNEL_ALPHA_OUTLIER format=%u shader=%s block=%u pixel=%u source=%u decoded=%u BC=",uint32_t(f),shader?"candidate":"weighted",n,p,source[p*4+3],decoded[p*4+3]);
                for(uint32_t k=0;k<pvb::block_bytes(f);++k) std::printf("%02x",bc[size_t(n)*pvb::block_bytes(f)+k]);std::printf("\n");
            }
            alpha.add(e);
            const auto endpoint=etc2_alpha::bc_source(f,bc.data()+size_t(n)*pvb::block_bytes(f),p).endpoint();
            if(endpoint==etc2_alpha::zero) extremes+=decoded[p*4+3]!=0;
            if(endpoint==etc2_alpha::opaque) extremes+=decoded[p*4+3]!=255;
        }
    }
    std::printf("KERNEL_TIMED_QUALITY {\"format\":%u,\"shader\":\"%s\",\"pattern_id\":%u,\"round\":%u,\"extreme_alpha_errors\":%llu,",uint32_t(f),shader?"candidate":"weighted",pattern,round,(unsigned long long)extremes);print_metric(rgb,"visible_rgb");std::printf(",");print_metric(alpha,"alpha");std::printf("}\n");
    check(rgb.rmse()<=32 && alpha.rmse()<=8 && alpha.maximum<=16 && !extremes,"Timed actual kernel output independently passes unchanged source quality gates");
}
struct ResidentWorkload {
    VkFormat format;
    uint32_t pattern;
    Bytes bc;
    size_t output_bytes;
    DiagnosticResources baseline,candidate;
    std::array<VkDescriptorSet,2> sets{};
    ResidentWorkload(EncoderApp &a,VkFormat f,uint32_t p,uint32_t size,const EncoderSelection &weighted,const EncoderSelection &selected)
        :format(f),pattern(p),bc(benchmark_source(f,size/4,size/4,2,p)),output_bytes(size_t(size/4)*(size/4)*2*etc_bytes(f)),baseline(a),candidate(a) {
        const std::array<DiagnosticResources*,2> resources={&baseline,&candidate};
        const std::array<const std::vector<uint32_t>*,2> modules={weighted.select(f),selected.select(f)};
        for(uint32_t n=0;n<2;++n) {
            auto &r=*resources[n];r.input=a.buffer(bc.size()+2*guard,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            r.output=a.buffer(output_bytes+2*guard,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            std::memset(r.input.mapped,0x5a,bc.size()+2*guard);std::memcpy(static_cast<uint8_t*>(r.input.mapped)+guard,bc.data(),bc.size());
            std::memset(r.output.mapped,0xa5,output_bytes+2*guard);storage_pipeline(a,r,sets[n],bc.size(),output_bytes,modules[n]);
        }
    }
};
void timing(EncoderApp &a,const EncoderSelection &weighted,const EncoderSelection &selected,const CompareOptions &o) {
    constexpr uint32_t layers=2;
    const uint32_t br=o.size/4,bh=o.size/4;
    // Prepare ALL workload pipelines and host-coherent mapped source/output
    // buffers before the first timed window. No encoder/upload creation occurs
    // in the recording/submission/timestamp loop below.
    std::vector<std::unique_ptr<ResidentWorkload>> workloads;
    size_t resident_bytes=0;
    for(auto workload:std::array<std::array<uint32_t,2>,4>{{{{133,0}},{{137,0}},{{137,1}},{{137,2}}}}) {
        const auto f=VkFormat(workload[0]);
        check(native_support(a,f),"Kernel comparison backend genuinely supports native ETC2 format contract");
        workloads.push_back(std::make_unique<ResidentWorkload>(a,f,workload[1],o.size,weighted,selected));
        resident_bytes+=2*(workloads.back()->bc.size()+workloads.back()->output_bytes+4*guard);
    }
    QueryOwner query(a);VkPhysicalDeviceProperties properties{};a.GetPhysicalDeviceProperties(a.physical,&properties);
    std::printf("KERNEL_RESIDENT {\"apps\":1,\"devices\":1,\"workloads\":4,\"pipelines\":8,\"timing_mapped_buffer_bytes\":%zu,\"timing_mapped_buffer_upper_bound\":8388608,\"pipeline_creation_and_source_upload_complete\":true}\n",resident_bytes);
    if(!query.pool) std::printf("KERNEL_GPU_TIMESTAMPS: unsupported; GPU kernel times are null; host record/submit/fence-wait times remain separate\n");
    stage("resident paired kernel timing begin");
    for(auto &owned:workloads) {
        auto &w=*owned;const auto f=w.format;const auto pattern=w.pattern;
        const std::array<DiagnosticResources*,2> resources={&w.baseline,&w.candidate};
        std::printf("KERNEL_TEXTURE_PATTERN {\"format\":%u,\"pattern\":\"%s\",\"pattern_id\":%u,\"sparse_rule\":\"one difficult invocation per8x8 block workgroup; size16 has higher density\"}\n",uint32_t(f),texture_pattern_name(f,pattern),pattern);
        std::printf("KERNEL_CONFIG {\"backend\":");json_string(properties.deviceName);
        std::printf(",\"format\":%u,\"shape\":[%u,%u,2],\"blocks\":%u,\"seed\":%u,\"rounds\":%u,\"warmups\":%u,\"repeats\":%u,\"timestamp_supported\":%s,\"timestamp_bits\":%u,\"period_ns\":%.9g,\"validation_requested\":%s,\"scope\":\"resident SSBO kernel; pipeline creation/upload/oracle excluded; software timing is not Mali FPS\"}\n",uint32_t(f),o.size,o.size,br*bh*layers,corpus_seed,o.rounds,o.warmups,o.repeats,query.pool?"true":"false",query.bits,query.period,o.validation?"true":"false");
        const uint32_t push[6]={uint32_t(f),o.size,o.size,layers,br,bh};
        for(uint32_t round=0;round<o.rounds;++round) {
            std::array<std::vector<double>,2> gpu,wall;
            for(uint32_t iteration=0;iteration<o.warmups+o.repeats;++iteration) for(uint32_t pair=0;pair<2;++pair) {
                const uint32_t n=(round+iteration+pair)&1u;auto &r=*resources[n];const auto start=Clock::now();r.begin();
                VkBufferMemoryBarrier barriers[2]{};
                for(auto &b:barriers) { b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.size=VK_WHOLE_SIZE;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; }
                barriers[0].buffer=r.input.handle;barriers[0].srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barriers[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                barriers[1].buffer=r.output.handle;barriers[1].srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_HOST_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;barriers[1].dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
                a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,2,barriers,0,nullptr);
                query.begin();a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);
                a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&w.sets[n],0,nullptr);
                a.CmdPushConstants(a.command,r.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);a.CmdDispatch(a.command,(br+7)/8,(bh+7)/8,layers);query.end();
                host_barrier(a,r.output.handle,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);a.submit();const auto stop=Clock::now();const auto ms=query.ms();
                r.reset("Direct kernel repeat reset");
                if(iteration>=o.warmups) {
                    if(query.pool) gpu[n].push_back(ms);wall[n].push_back(elapsed(start,stop));
                    std::printf("KERNEL_SAMPLE {\"format\":%u,\"round\":%u,\"pattern_id\":%u,\"repeat\":%u,\"pair_position\":%u,\"shader\":\"%s\",\"gpu_kernel_ms\":",uint32_t(f),round,pattern,iteration-o.warmups,pair,n?"candidate":"weighted");
                    if(query.pool) std::printf("%.9g",ms);else std::printf("null");std::printf(",\"record_submit_wait_ms\":%.9g}\n",wall[n].back());
                }
            }
            // Original source BC and independent detex quality checks occur only
            // after all timed repetitions, never inside a timestamp interval.
            for(uint32_t n=0;n<2;++n) {
                const auto failed_before=failed;auto &r=*resources[n];auto *output=static_cast<uint8_t*>(r.output.mapped);auto *input=static_cast<uint8_t*>(r.input.mapped);bool intact=true,input_intact=true;
                for(uint32_t k=0;k<guard;++k) { intact&=output[k]==0xa5 && output[guard+w.output_bytes+k]==0xa5;input_intact&=input[k]==0x5a && input[guard+w.bc.size()+k]==0x5a; }
                check(intact,"Timed kernel output guards intact");check(input_intact && !std::memcmp(input+guard,w.bc.data(),w.bc.size()),"Timed kernel retains source BC bytes and guards");
                timed_quality(f,w.bc,output+guard,br*bh*layers,n,pattern,round);shader_failures[n]+=failed-failed_before;std::sort(gpu[n].begin(),gpu[n].end());std::sort(wall[n].begin(),wall[n].end());
                std::printf("KERNEL_RESULT {\"format\":%u,\"round\":%u,\"pattern_id\":%u,\"shader\":\"%s\",\"kernel_median_ms\":",uint32_t(f),round,pattern,n?"candidate":"weighted");
                if(query.pool) std::printf("%.9g",gpu[n][o.repeats/2]);else std::printf("null");std::printf(",\"record_submit_wait_median_ms\":%.9g}\n",wall[n][o.repeats/2]);
            }
        }
    }
    stage("resident paired kernel timing complete");
}
uint32_t bounded_integer(const char *value,uint32_t low,uint32_t high) {
    char *end=nullptr;const auto number=std::strtoull(value,&end,0);
    if(!*value || !end || *end || number<low || number>high) throw std::runtime_error("Comparison option outside bounds");
    return uint32_t(number);
}
}
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc<4) { std::fprintf(stderr,"Usage: pixel_etc2_kernel_compare explicit-library.so weighted.spv candidate-bc1.spv candidate-bc3.spv [--validation] [--quality-only|--timing-only] [--size 16..512 power-of-two] [--warmup 0..30] [--repeats 1..30] [--rounds 1..6] [--seed uint32]\nLegacy combined candidate SPIR-V is accepted in place of the BC1/BC3 pair and reported explicitly.\n");return 2; }
    try {
        CompareOptions o;const bool separate=argc>4 && std::string(argv[4]).rfind("--",0)!=0;
        const auto weighted_module=load_reference_encoder(argv[2]);const auto candidate_bc1=load_reference_encoder(argv[3]);
        const auto candidate_bc3=separate?load_reference_encoder(argv[4]):std::vector<uint32_t>{};
        const EncoderSelection weighted{&weighted_module,nullptr,nullptr},candidate{separate?nullptr:&candidate_bc1,separate?&candidate_bc1:nullptr,separate?&candidate_bc3:nullptr};
        for(int n=separate?5:4;n<argc;++n) {
            const std::string option=argv[n];
            if(option=="--validation") o.validation=true;
            else if(option=="--quality-only") o.quality_only=true;
            else if(option=="--timing-only") o.timing_only=true;
            else {
                if(n+1>=argc) throw std::runtime_error("Missing comparison option value");const auto *value=argv[++n];
                if(option=="--size") o.size=bounded_integer(value,16,512);
                else if(option=="--warmup") o.warmups=bounded_integer(value,0,30);
                else if(option=="--repeats") o.repeats=bounded_integer(value,1,30);
                else if(option=="--rounds") o.rounds=bounded_integer(value,1,6);
                else if(option=="--seed") corpus_seed=bounded_integer(value,0,UINT32_MAX);
                else throw std::runtime_error("Unknown comparison option");
            }
        }
        if(o.size&(o.size-1)) throw std::runtime_error("Comparison size must be a power of two");
        if(o.quality_only && o.timing_only) throw std::runtime_error("Quality-only and timing-only are mutually exclusive");
        encoder_identity("KERNEL_MODULE","weighted_baseline",argv[2],VK_FORMAT_UNDEFINED,weighted_module);
        encoder_identity("KERNEL_MODULE",separate?"candidate_bc1":"legacy_combined_candidate",argv[3],separate?VK_FORMAT_BC1_RGBA_UNORM_BLOCK:VK_FORMAT_UNDEFINED,candidate_bc1);
        if(separate) {
            encoder_identity("KERNEL_MODULE","candidate_bc3",argv[4],VK_FORMAT_BC3_UNORM_BLOCK,candidate_bc3);
            encoder_identity("KERNEL_MODULE","candidate_bc2_preview_diagnostic","embedded:bc_etc2_encode_spv",VK_FORMAT_BC2_UNORM_BLOCK,production_encoder(VK_FORMAT_BC2_UNORM_BLOCK));
        }
        if(o.validation) {
            constexpr const char *sync="VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT";
            const auto *old=std::getenv("VK_LAYER_ENABLES");std::string enables=old?old:"";
            if(enables.find(sync)==std::string::npos) { if(!enables.empty()) enables+=",";enables+=sync;if(setenv("VK_LAYER_ENABLES",enables.c_str(),1)) throw std::runtime_error("Cannot request synchronization validation"); }
        }
        EncoderApp a;init(a,argv[1],o.validation,false);oracle_anchors();Options quality_options;
        std::printf("KERNEL_COVERAGE {\"full_prequality_executed\":%s,\"timing_executed\":%s,\"timed_source_quality_required\":true,\"bc2_preview_only\":true,\"candidate_separate_modules\":%s}\n",o.timing_only?"false":"true",o.quality_only?"false":"true",separate?"true":"false");
        const auto timing_seed=corpus_seed;
        if(!o.timing_only) for(uint32_t n=0;n<2;++n) {
            const auto failed_before=failed;std::printf("KERNEL_QUALITY_SOURCE: %s\n",n?"candidate":"weighted");
            const auto &modules=n?candidate:weighted;
            for(auto seed:{2658517889u,183927651u}) { corpus_seed=seed;quality(a,quality_options,modules); }
            if(timing_seed!=2658517889u && timing_seed!=183927651u) { corpus_seed=timing_seed;quality(a,quality_options,modules); }
            adversarial_quality(a,quality_options,modules);shader_failures[n]+=failed-failed_before;
            std::printf("KERNEL_SHADER_QUALITY_CHECKS {\"shader\":\"%s\",\"failed\":%u}\n",n?"candidate":"weighted",failed-failed_before);
        }
        corpus_seed=timing_seed;if(!o.quality_only) timing(a,weighted,candidate,o);a.shutdown();
        std::printf("KERNEL_FAILURE_CLASSIFICATION {\"weighted_baseline_failed\":%u,\"candidate_failed\":%u,\"candidate_quality_passed\":%s,\"joint_check_remains_failed\":%s}\n",shader_failures[0],shader_failures[1],shader_failures[1]?"false":"true",failed?"true":"false");
        std::printf("KERNEL_CHECKS {\"passed\":%u,\"failed\":%u,\"validation_errors\":%u,\"quality_only\":%s,\"timing_only\":%s}\n",passed,failed,validation_errors,o.quality_only?"true":"false",o.timing_only?"true":"false");return failed||validation_errors?1:0;
    } catch(const std::exception &e) { std::fprintf(stderr,"KERNEL_ERROR: %s\n",e.what());return 2; }
}
