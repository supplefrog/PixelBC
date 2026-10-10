// SPDX-License-Identifier: MIT
#include <vulkan/vulkan.h>
#include "bc_transfer_layout.h"
#include "stats.h"
#include "transfer_stats.h"
#include "sample_spv.h"
#include "bc_decode_spv.h"
#include "android_hal.h"
#define BCDEC_IMPLEMENTATION
#include "reference_bcdec.h"
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
namespace {
uint32_t passed=0,failed=0,validation_errors=0,max_difference=0,alias_tests=0,alias_skipped=0;
void stage(const char *name) { std::printf("STAGE: %s\n",name); std::fflush(stdout); }
void check(bool condition,const char *name) { if(condition) ++passed; else { ++failed; std::fprintf(stderr,"FAIL: %s\n",name); } }
void require(VkResult result,const char *where) { if(result!=VK_SUCCESS) throw std::runtime_error(std::string(where)+": "+std::to_string(result)); }
#define I_FUNCS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceFormatProperties) X(GetPhysicalDeviceFormatProperties2) X(GetPhysicalDeviceImageFormatProperties) X(GetPhysicalDeviceImageFormatProperties2) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(EnumerateDeviceExtensionProperties)
#define D_FUNCS(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdCopyImage) X(CmdCopyImage2) X(CmdCopyImageToBuffer) X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdCopyBufferToImage) X(CmdCopyBufferToImage2) X(CmdDispatch) X(QueueSubmit) X(QueueSubmit2) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(DeviceWaitIdle) X(GetDeviceImageMemoryRequirements) X(CreateSemaphore) X(DestroySemaphore) X(CreateEvent) X(DestroyEvent) X(CmdPipelineBarrier2) X(CmdSetEvent) X(CmdWaitEvents) X(CmdSetEvent2) X(CmdWaitEvents2)
struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void *mapped{}; };
struct DiagnosticHandles {
    Buffer input{},output{};
    Buffer compressed{};
    VkCommandBuffer transfer_command{};
    VkSemaphore transfer_semaphore{};
    VkEvent transfer_event{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    std::array<VkImage,2> transfer_images{};
    std::array<VkDeviceMemory,2> transfer_memory{};
    std::array<VkImageView,2> transfer_views{};
};
struct App {
    void *library=nullptr;
    PFN_vkGetInstanceProcAddr gipa{}; PFN_vkGetDeviceProcAddr gdpa{};
    PvbGetStats stats{};
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; uint32_t family=0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{};
    VkDescriptorSetLayout descriptors{}; VkPipelineLayout layout{}; VkPipeline pipeline{}; VkSampler sampler{};
    VkDebugUtilsMessengerEXT messenger{}; PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger{};
    hw_device_t *hal=nullptr;
    bool shutting_down=false;
    bool submission_pending=false;
    bool diagnostic_deferred=false;
    DiagnosticHandles deferred_diagnostic{};
#define DECLARE(n) PFN_vk##n n{};
    I_FUNCS(DECLARE) D_FUNCS(DECLARE)
#undef DECLARE
    App()=default;
    App(const App&)=delete;
    App& operator=(const App&)=delete;
    ~App() { shutdown(); }
    void shutdown() noexcept {
        if(shutting_down || (!device && !instance && !hal && !library)) return;
        shutting_down=true;
        stage("shutdown begin");
        auto d=std::exchange(device,VK_NULL_HANDLE);
        if(d) {
            if(DeviceWaitIdle) {
                stage("before vkDeviceWaitIdle"); const auto idle=DeviceWaitIdle(d);
                check(idle==VK_SUCCESS,"DeviceWaitIdle during teardown");
                if(idle==VK_SUCCESS || idle==VK_ERROR_DEVICE_LOST) submission_pending=false;
                stage("after vkDeviceWaitIdle");
            }
            if(submission_pending) {
                check(false,"Pending diagnostic GPU work could not be quiesced for teardown");
                stage("teardown deferred to OS process exit: GPU completion unknown; retaining Vulkan objects, HAL and loader reference");
                // No legal explicit destruction is possible while GPU use is
                // unknown. Return failure naturally; process exit reclaims it.
                instance=VK_NULL_HANDLE; messenger=VK_NULL_HANDLE; hal=nullptr; library=nullptr;
                shutting_down=false; return;
            }
            if(diagnostic_deferred) {
                if(command) { auto owned=std::exchange(command,VK_NULL_HANDLE); FreeCommandBuffers(d,pool,1,&owned); }
                destroy_diagnostic(deferred_diagnostic,d); diagnostic_deferred=false;
            }
            auto child=[&](auto &handle,auto destroy,const char *before,const char *after) {
                if(!handle) return;
                stage(before); auto owned=std::exchange(handle,VK_NULL_HANDLE);
                if(destroy) destroy(d,owned,nullptr); else check(false,"Missing owned Vulkan child teardown callback");
                stage(after);
            };
            child(pool,DestroyCommandPool,"before vkDestroyCommandPool","after vkDestroyCommandPool");
            child(fence,DestroyFence,"before vkDestroyFence","after vkDestroyFence");
            child(sampler,DestroySampler,"before vkDestroySampler","after vkDestroySampler");
            child(pipeline,DestroyPipeline,"before vkDestroyPipeline","after vkDestroyPipeline");
            child(layout,DestroyPipelineLayout,"before vkDestroyPipelineLayout","after vkDestroyPipelineLayout");
            child(descriptors,DestroyDescriptorSetLayout,"before vkDestroyDescriptorSetLayout","after vkDestroyDescriptorSetLayout");
            if(DestroyDevice) { stage("before vkDestroyDevice"); DestroyDevice(d,nullptr); stage("after vkDestroyDevice"); }
            else check(false,"Missing mandatory device teardown callback");
        }
        if(messenger && destroy_messenger) { stage("before Vulkan debug messenger destruction"); destroy_messenger(instance,std::exchange(messenger,VK_NULL_HANDLE),nullptr); stage("after Vulkan debug messenger destruction"); }
        auto i=std::exchange(instance,VK_NULL_HANDLE);
        if(i) { if(DestroyInstance) { stage("before vkDestroyInstance"); DestroyInstance(i,nullptr); stage("after vkDestroyInstance"); } else check(false,"Missing mandatory instance teardown callback"); }
        auto h=std::exchange(hal,nullptr);
        if(h) { stage("before wrapper HAL close"); if(h->close) check(h->close(h)==0,"Wrapper HAL close during teardown"); else check(false,"Missing owned HAL close callback"); stage("after wrapper HAL close"); }
        auto lib=std::exchange(library,nullptr);
        if(lib) { stage("before wrapper dlclose"); check(dlclose(lib)==0,"Wrapper dlclose during teardown");
#ifdef __ANDROID__
            stage("after wrapper dlclose (caller reference released; NODELETE mapping retained)");
#else
            stage("after wrapper dlclose");
#endif
        }
        gipa=nullptr; gdpa=nullptr; stats=nullptr; physical=VK_NULL_HANDLE; queue=VK_NULL_HANDLE; command=VK_NULL_HANDLE;
        stage("shutdown complete"); shutting_down=false;
    }
    template<class T> T ip(const char *n) { auto p=reinterpret_cast<T>(gipa(instance,n)); if(!p) throw std::runtime_error(std::string("Missing instance function ")+n); return p; }
    template<class T> T dp(const char *n) { auto p=reinterpret_cast<T>(gdpa(device,n)); if(!p) throw std::runtime_error(std::string("Missing device function ")+n); return p; }
    uint32_t memory_index(uint32_t mask,VkMemoryPropertyFlags flags) {
        for(uint32_t n=0;n<memory.memoryTypeCount;++n) if((mask&(1u<<n)) && (memory.memoryTypes[n].propertyFlags&flags)==flags) return n;
        throw std::runtime_error("No suitable memory type");
    }
    Buffer buffer(VkDeviceSize size,VkBufferUsageFlags usage) {
        Buffer b;
        try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=size; info.usage=usage; info.sharingMode=VK_SHARING_MODE_EXCLUSIVE; require(CreateBuffer(device,&info,nullptr,&b.handle),"CreateBuffer");
        VkMemoryRequirements req{}; GetBufferMemoryRequirements(device,b.handle,&req); VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=req.size; allocate.memoryTypeIndex=memory_index(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        require(AllocateMemory(device,&allocate,nullptr,&b.memory),"AllocateMemory buffer"); require(BindBufferMemory(device,b.handle,b.memory,0),"BindBufferMemory"); require(MapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped),"MapMemory"); return b;
        } catch(...) { destroy(b); throw; }
    }
    void destroy(Buffer &b) noexcept { destroy(b,device); }
    void destroy(Buffer &b,VkDevice d) noexcept {
        auto owned=std::exchange(b,Buffer{});
        if(owned.mapped) UnmapMemory(d,owned.memory);
        if(owned.handle) DestroyBuffer(d,owned.handle,nullptr);
        if(owned.memory) FreeMemory(d,owned.memory,nullptr);
    }
    void destroy_diagnostic(DiagnosticHandles &owned,VkDevice d) noexcept {
        if(owned.transfer_command) { auto command=std::exchange(owned.transfer_command,VK_NULL_HANDLE); FreeCommandBuffers(d,pool,1,&command); }
        if(owned.transfer_semaphore) DestroySemaphore(d,std::exchange(owned.transfer_semaphore,VK_NULL_HANDLE),nullptr);
        if(owned.transfer_event) DestroyEvent(d,std::exchange(owned.transfer_event,VK_NULL_HANDLE),nullptr);
        if(owned.pool) DestroyDescriptorPool(d,std::exchange(owned.pool,VK_NULL_HANDLE),nullptr);
        if(owned.view) DestroyImageView(d,std::exchange(owned.view,VK_NULL_HANDLE),nullptr);
        if(owned.image) DestroyImage(d,std::exchange(owned.image,VK_NULL_HANDLE),nullptr);
        if(owned.memory) FreeMemory(d,std::exchange(owned.memory,VK_NULL_HANDLE),nullptr);
        if(owned.pipeline) DestroyPipeline(d,std::exchange(owned.pipeline,VK_NULL_HANDLE),nullptr);
        if(owned.shader) DestroyShaderModule(d,std::exchange(owned.shader,VK_NULL_HANDLE),nullptr);
        if(owned.layout) DestroyPipelineLayout(d,std::exchange(owned.layout,VK_NULL_HANDLE),nullptr);
        if(owned.descriptors) DestroyDescriptorSetLayout(d,std::exchange(owned.descriptors,VK_NULL_HANDLE),nullptr);
        for(auto &view:owned.transfer_views) if(view) DestroyImageView(d,std::exchange(view,VK_NULL_HANDLE),nullptr);
        for(auto &image:owned.transfer_images) if(image) DestroyImage(d,std::exchange(image,VK_NULL_HANDLE),nullptr);
        for(auto &memory:owned.transfer_memory) if(memory) FreeMemory(d,std::exchange(memory,VK_NULL_HANDLE),nullptr);
        destroy(owned.input,d); destroy(owned.output,d); destroy(owned.compressed,d);
    }
    void begin() { VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; require(BeginCommandBuffer(command,&info),"BeginCommandBuffer"); }
    void submit(bool use2=false) {
        require(EndCommandBuffer(command),"EndCommandBuffer"); submit_ended(use2);
    }
    void submit_ended(bool use2=false) {
        if(use2) { VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; cb.commandBuffer=command; VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; info.commandBufferInfoCount=1; info.pCommandBufferInfos=&cb; require(QueueSubmit2(queue,1,&info,fence),"QueueSubmit2"); }
        else { VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.commandBufferCount=1; info.pCommandBuffers=&command; require(QueueSubmit(queue,1,&info,fence),"QueueSubmit"); }
        submission_pending=true;
        require(WaitForFences(device,1,&fence,VK_TRUE,30000000000ull),"WaitForFences");
        submission_pending=false;
        require(ResetFences(device,1,&fence),"ResetFences");
    }
};
// Diagnostic resources are acquired directly into this owner before any later
// fallible call. It also revokes the recording before freeing referenced handles.
struct DiagnosticResources:DiagnosticHandles {
    App &app;
    bool recorded=false;
    explicit DiagnosticResources(App &a):app(a) {}
    DiagnosticResources(const DiagnosticResources&)=delete;
    DiagnosticResources& operator=(const DiagnosticResources&)=delete;
    ~DiagnosticResources() { clear(); }
    void begin() { recorded=true; app.begin(); }
    void reset(const char *where) { require(app.ResetCommandBuffer(app.command,0),where); recorded=false; }
    void clear() noexcept {
        if(recorded) {
            stage("diagnostic recovery before vkDeviceWaitIdle");
            const auto idle=app.DeviceWaitIdle(app.device);
            if(idle!=VK_SUCCESS) check(false,"DeviceWaitIdle during diagnostic recovery");
            bool safe=!app.submission_pending || idle==VK_SUCCESS || idle==VK_ERROR_DEVICE_LOST;
            if(!safe) {
                const auto waited=app.WaitForFences(app.device,1,&app.fence,VK_TRUE,30000000000ull);
                safe=waited==VK_SUCCESS || waited==VK_ERROR_DEVICE_LOST;
                if(waited!=VK_SUCCESS) check(false,"Fence wait during diagnostic recovery");
            }
            stage("diagnostic recovery after GPU quiescence check");
            if(!safe) {
                stage("diagnostic resources retained for final teardown idle attempt: GPU completion unknown");
                app.deferred_diagnostic=std::exchange(static_cast<DiagnosticHandles&>(*this),DiagnosticHandles{});
                app.diagnostic_deferred=true; recorded=false; return;
            }
            app.submission_pending=false;
            if(app.command) { auto command=std::exchange(app.command,VK_NULL_HANDLE); app.FreeCommandBuffers(app.device,app.pool,1,&command); }
            recorded=false;
        }
        app.destroy_diagnostic(*this,app.device);
    }
};
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { ++validation_errors; std::fprintf(stderr,"VALIDATION: %s\n",data->pMessage); } return VK_FALSE;
}
void init(App &a,const char *path,bool validation,bool enforce_version=true) {
    std::printf("PROBE_VERSION: %s\nLIBRARY_REQUESTED: %s\nCHAR_SIGNED: %d\n",PVB_VERSION,path,char(-1)<0);
    stage("before wrapper dlopen");
    a.library=dlopen(path,RTLD_NOW|RTLD_LOCAL); if(!a.library) throw std::runtime_error(dlerror());
    stage("after wrapper dlopen");
    auto version=reinterpret_cast<PvbGetVersion>(dlsym(a.library,"pvb_get_version"));
    if(!version || (enforce_version && std::strcmp(version(),PVB_VERSION))) throw std::runtime_error("Probe/library version mismatch");
    Dl_info origin{}; if(dladdr(reinterpret_cast<void*>(version),&origin)) std::printf("LIBRARY_LOADED: %s\n",origin.dli_fname);
    std::printf("LIBRARY_VERSION: %s\n",version());
    a.stats=reinterpret_cast<PvbGetStats>(dlsym(a.library,"pvb_get_stats")); if(!a.stats) throw std::runtime_error("Not PixelBC (missing proof counters)");
#ifdef __ANDROID__
    auto module=static_cast<hwvulkan_module_t*>(dlsym(a.library,"HMI"));
    if(!module || module->common.tag!=HARDWARE_MODULE_TAG || std::strcmp(module->common.id,"vulkan") || !module->common.methods || !module->common.methods->open) throw std::runtime_error("Invalid HMI");
    module->common.dso=a.library;
    stage("before wrapper HAL open");
    if(module->common.methods->open(&module->common,"vk0",&a.hal)!=0 || !a.hal) throw std::runtime_error("Wrapper HAL open failed; no fallback");
    stage("after wrapper HAL open");
    check(a.hal->tag==HARDWARE_DEVICE_TAG && a.hal->version==HWVULKAN_DEVICE_API_VERSION_0_1,"Android HAL device ABI");
    a.gipa=reinterpret_cast<hwvulkan_device_t*>(a.hal)->GetInstanceProcAddr;
#else
    a.gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(a.library,"vkGetInstanceProcAddr"));
#endif
    if(!a.gipa) throw std::runtime_error("Missing wrapper GPA");
    auto create=reinterpret_cast<PFN_vkCreateInstance>(a.gipa(VK_NULL_HANDLE,"vkCreateInstance")); if(!create) throw std::runtime_error("Backend unavailable; no fallback");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="PixelBC synthetic probe"; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo=&app;
    const char *layer="VK_LAYER_KHRONOS_validation",*extension="VK_EXT_debug_utils";
    if(validation) { info.enabledLayerCount=1; info.ppEnabledLayerNames=&layer; info.enabledExtensionCount=1; info.ppEnabledExtensionNames=&extension; }
    require(create(&info,nullptr,&a.instance),"CreateInstance");
    // Resolve destruction first so subsequent dispatch-loading failures unwind safely.
    a.DestroyInstance=a.ip<PFN_vkDestroyInstance>("vkDestroyInstance");
#define LOAD(n) a.n=a.ip<PFN_vk##n>("vk" #n);
    I_FUNCS(LOAD)
#undef LOAD
    a.gdpa=a.ip<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    if(validation) { auto create_debug=a.ip<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT"); a.destroy_messenger=a.ip<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT"); VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT}; debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT; debug.pfnUserCallback=debug_callback; require(create_debug(a.instance,&debug,nullptr,&a.messenger),"CreateDebugMessenger"); }
    uint32_t count=0; require(a.EnumeratePhysicalDevices(a.instance,&count,nullptr),"EnumeratePhysicalDevices count"); if(!count) throw std::runtime_error("No physical devices"); std::vector<VkPhysicalDevice> physicals(count); require(a.EnumeratePhysicalDevices(a.instance,&count,physicals.data()),"EnumeratePhysicalDevices"); a.physical=physicals[0];
    VkPhysicalDeviceProperties props{}; a.GetPhysicalDeviceProperties(a.physical,&props); std::printf("DEVICE: %s api=%u.%u.%u\n",props.deviceName,VK_API_VERSION_MAJOR(props.apiVersion),VK_API_VERSION_MINOR(props.apiVersion),VK_API_VERSION_PATCH(props.apiVersion)); check(std::strstr(props.deviceName,"PixelBC /")!=nullptr,"Wrapper physical identity (no system fallback)");
    VkPhysicalDeviceFeatures features{}; a.GetPhysicalDeviceFeatures(a.physical,&features); check(!features.textureCompressionBC,"Core BC1-7 feature is not advertised");
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext=&f13; a.GetPhysicalDeviceFeatures2(a.physical,&f2); check(!f2.features.textureCompressionBC,"Features2 BC bit");
    check(f13.synchronization2,"Backend synchronization2 available");
    a.GetPhysicalDeviceQueueFamilyProperties(a.physical,&count,nullptr); std::vector<VkQueueFamilyProperties> families(count); a.GetPhysicalDeviceQueueFamilyProperties(a.physical,&count,families.data());
    bool found=false; for(uint32_t n=0;n<count;++n) if(families[n].queueFlags&VK_QUEUE_COMPUTE_BIT) { a.family=n; found=true; break; } if(!found) throw std::runtime_error("No compute queue");
    float priority=1; VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue.queueFamilyIndex=a.family; queue.queueCount=1; queue.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; enabled.synchronization2=VK_TRUE; VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    uint32_t extension_count=0; require(a.EnumerateDeviceExtensionProperties(a.physical,nullptr,&extension_count,nullptr),"Device extension count"); std::vector<VkExtensionProperties> available(extension_count); require(a.EnumerateDeviceExtensionProperties(a.physical,nullptr,&extension_count,available.data()),"Device extensions"); std::vector<const char*> enabled_extensions;
    for(auto name:{"VK_KHR_copy_commands2","VK_KHR_synchronization2","VK_KHR_maintenance4"}) for(const auto &candidate:available) if(!std::strcmp(candidate.extensionName,name)) enabled_extensions.push_back(name);
    device.enabledExtensionCount=uint32_t(enabled_extensions.size()); device.ppEnabledExtensionNames=enabled_extensions.data(); device.queueCreateInfoCount=1; device.pQueueCreateInfos=&queue; device.pNext=&enabled; require(a.CreateDevice(a.physical,&device,nullptr,&a.device),"CreateDevice");
    a.DestroyDevice=a.dp<PFN_vkDestroyDevice>("vkDestroyDevice");
    a.DeviceWaitIdle=a.dp<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
#define LOAD(n) a.n=a.dp<PFN_vk##n>("vk" #n);
    D_FUNCS(LOAD)
#undef LOAD
    a.GetDeviceQueue(a.device,a.family,0,&a.queue); a.GetPhysicalDeviceMemoryProperties(a.physical,&a.memory);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex=a.family; require(a.CreateCommandPool(a.device,&pool,nullptr,&a.pool),"CreateCommandPool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocate.commandPool=a.pool; allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount=1; require(a.AllocateCommandBuffers(a.device,&allocate,&a.command),"AllocateCommandBuffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; require(a.CreateFence(a.device,&fence,nullptr,&a.fence),"CreateFence");
    VkDescriptorSetLayoutBinding binds[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ds.bindingCount=2; ds.pBindings=binds; require(a.CreateDescriptorSetLayout(a.device,&ds,nullptr,&a.descriptors),"CreateDescriptorSetLayout");
    // A range spanning multiple stages catches invalid compute-only replay of caller push constants.
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,32}; VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&a.descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&range; require(a.CreatePipelineLayout(a.device,&pl,nullptr,&a.layout),"CreatePipelineLayout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(sample_spv); sm.pCode=sample_spv; VkShaderModule shader{}; require(a.CreateShaderModule(a.device,&sm,nullptr,&shader),"CreateShaderModule"); VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=shader; cp.stage.pName="main"; cp.layout=a.layout; require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&cp,nullptr,&a.pipeline),"CreateComputePipeline"); a.DestroyShaderModule(a.device,shader,nullptr);
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST; sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sampler.maxLod=32; require(a.CreateSampler(a.device,&sampler,nullptr,&a.sampler),"CreateSampler");
}
uint32_t random_word(uint32_t &seed) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
void write32(uint8_t *out,uint32_t word) { for(uint32_t n=0;n<4;++n) out[n]=uint8_t(word>>(8*n)); }
std::array<uint8_t,16> known_block(VkFormat f,bool transparent) {
    std::array<uint8_t,16> b{};
    if(f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) { b[0]=transparent?0:0; b[1]=transparent?0:0xf8; b[2]=transparent?0xff:0xe0; b[3]=transparent?0xff:0x07; write32(b.data()+4,transparent?0xffffffffu:0xe4e4e4e4u); }
    else if(f<=VK_FORMAT_BC3_SRGB_BLOCK) { if(f<=VK_FORMAT_BC2_SRGB_BLOCK) for(uint32_t n=0;n<8;++n) b[n]=uint8_t(2*n|((2*n+1)<<4)); else { b[0]=255; b[1]=0; uint64_t indices=0; for(uint32_t n=0;n<16;++n) indices|=uint64_t(n%8)<<(3*n); for(uint32_t n=0;n<6;++n) b[2+n]=uint8_t(indices>>(8*n)); } b[8]=0; b[9]=0; b[10]=0xff; b[11]=0xff; write32(b.data()+12,0xe4e4e4e4u); }
    else { const bool sign=pvb::is_signed(f); for(uint32_t ch=0;ch<(f>=VK_FORMAT_BC5_UNORM_BLOCK?2u:1u);++ch) { b[ch*8]=sign?128:255; b[ch*8+1]=sign?127:0; uint64_t indices=0; for(uint32_t n=0;n<16;++n) indices|=uint64_t(n%8)<<(3*n); for(uint32_t n=0;n<6;++n) b[ch*8+2+n]=uint8_t(indices>>(8*n)); } }
    return b;
}
std::array<uint8_t,64> reference(VkFormat f,const uint8_t *compressed) {
    alignas(8) std::array<uint8_t,16> input{}; std::memcpy(input.data(),compressed,pvb::block_bytes(f)); std::array<uint8_t,64> out{};
    if(f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK) { bcdec_bc1(input.data(),out.data(),16); if(f<=VK_FORMAT_BC1_RGB_SRGB_BLOCK) for(uint32_t n=0;n<16;++n) out[4*n+3]=255; }
    else if(f<=VK_FORMAT_BC2_SRGB_BLOCK) bcdec_bc2(input.data(),out.data(),16);
    else if(f<=VK_FORMAT_BC3_SRGB_BLOCK) bcdec_bc3(input.data(),out.data(),16);
    else { for(uint32_t n=0;n<16;++n) out[4*n+3]=pvb::is_signed(f)?127:255; bcdec__bc4_block(input.data(),out.data(),16,4,pvb::is_signed(f)); if(f>=VK_FORMAT_BC5_UNORM_BLOCK) bcdec__bc4_block(input.data()+8,out.data()+1,16,4,pvb::is_signed(f)); }
    return out;
}
void signed_diagnostics(App &a) {
    stage("signed diagnostics begin (direct decode bytes, then independent SNORM sampling)");
    // Execute the exact runtime decoder shader directly into a host-visible SSBO.
    // This bypasses texture storage/sampling, isolating Mali shader output.
    DiagnosticResources resources(a);
    auto &input=resources.input; auto &output=resources.output;
    auto &descriptors=resources.descriptors; auto &layout=resources.layout;
    auto &shader=resources.shader; auto &pipeline=resources.pipeline; auto &pool=resources.pool;
    auto &image=resources.image; auto &memory=resources.memory; auto &view=resources.view;
    input=a.buffer(16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); output=a.buffer(64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorSetLayoutBinding binds[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; ds.bindingCount=2; ds.pBindings=binds;
    require(a.CreateDescriptorSetLayout(a.device,&ds,nullptr,&descriptors),"Direct decoder descriptors");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,24}; VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&range;
    require(a.CreatePipelineLayout(a.device,&pl,nullptr,&layout),"Direct decoder layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(bc_decode_spv); sm.pCode=bc_decode_spv; require(a.CreateShaderModule(a.device,&sm,nullptr,&shader),"Direct decoder shader");
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=shader; cp.stage.pName="main"; cp.layout=layout;
    require(a.CreateComputePipelines(a.device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline),"Direct decoder pipeline"); a.DestroyShaderModule(a.device,std::exchange(shader,VK_NULL_HANDLE),nullptr);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}; VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.maxSets=1; pool_info.poolSizeCount=1; pool_info.pPoolSizes=&size;
    require(a.CreateDescriptorPool(a.device,&pool_info,nullptr,&pool),"Direct decoder pool"); VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&descriptors; VkDescriptorSet set{}; require(a.AllocateDescriptorSets(a.device,&allocate,&set),"Direct decoder set");
    VkDescriptorBufferInfo buffers[2]={{input.handle,0,16},{output.handle,0,64}}; VkWriteDescriptorSet writes[2]{};
    for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&buffers[n]; }
    a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    for(auto format:{VK_FORMAT_BC4_SNORM_BLOCK,VK_FORMAT_BC5_SNORM_BLOCK}) for(uint32_t fixture=0;fixture<4;++fixture) {
        auto block=known_block(format,false);
        if(fixture) { uint32_t seed=0x7410d0abu+uint32_t(format)*101u+(fixture==1?9u:7u)+(fixture==1?7u:5u)+(fixture==1?0u:fixture-1); for(uint32_t n=0;n<pvb::block_bytes(format);n+=4) write32(block.data()+n,random_word(seed)); }
        auto expected=reference(format,block.data());
        if(!fixture) check(int(int8_t(expected[8]))==-76 && int(int8_t(expected[0]))==-127,"Signed CPU reference literal interpolation/endpoints");
        std::memcpy(input.mapped,block.data(),16); std::memset(output.mapped,0xa5,64);
        resources.begin(); uint32_t push[6]={uint32_t(format),4,4,1,1,1}; a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push); a.CmdDispatch(a.command,1,1,1);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=output.handle; host.size=VK_WHOLE_SIZE; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); a.submit();
        auto actual=static_cast<uint8_t*>(output.mapped); bool equal=true; uint32_t difference=0;
        for(size_t n=0;n<64;++n) { auto delta=uint32_t(std::abs(int(int8_t(actual[n]))-int(int8_t(expected[n])))); difference=std::max(difference,delta); if(delta>1) { if(equal) std::fprintf(stderr,"DIRECT_DECODE_MISMATCH: format=%u fixture=%u byte=%zu got=%d expected=%d\n",uint32_t(format),fixture,n,int(int8_t(actual[n])),int(int8_t(expected[n]))); equal=false; } }
        std::printf("DIRECT_DECODE: format=%u fixture=%u max_difference=%u LSB\n",uint32_t(format),fixture,difference); check(equal,"Direct signed decoder bytes match portable reference"); resources.reset("Direct decoder reset");
    }
    resources.clear();

    // Calibrate actual RGBA8_SNORM sampling without any BC decoder/reference.
    // Both encoded -128 and -127 must sample as -1 and reconstruct to -127.
    input=a.buffer(32,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); output=a.buffer(32,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    constexpr int samples[8]={-128,-127,-76,-1,0,3,99,127}; std::array<uint8_t,32> expected{};
    auto raw=static_cast<uint8_t*>(input.mapped);
    for(uint32_t n=0;n<8;++n) for(uint32_t ch=0;ch<4;++ch) { int value=ch==3?127:samples[(n+ch)%8]; raw[n*4+ch]=uint8_t(value); expected[n*4+ch]=uint8_t(std::max(-127,value)); }
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image_info.imageType=VK_IMAGE_TYPE_2D; image_info.format=VK_FORMAT_R8G8B8A8_SNORM; image_info.extent={4,2,1}; image_info.mipLevels=image_info.arrayLayers=1; image_info.samples=VK_SAMPLE_COUNT_1_BIT; image_info.tiling=VK_IMAGE_TILING_OPTIMAL; image_info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(a.CreateImage(a.device,&image_info,nullptr,&image),"SNORM calibration image"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,image,&req); VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; memory_info.allocationSize=req.size; memory_info.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0); require(a.AllocateMemory(a.device,&memory_info,nullptr,&memory),"SNORM calibration memory"); require(a.BindImageMemory(a.device,image,memory,0),"SNORM calibration bind");
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view_info.image=image; view_info.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view_info.format=VK_FORMAT_R8G8B8A8_SNORM; view_info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; require(a.CreateImageView(a.device,&view_info,nullptr,&view),"SNORM calibration view");
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; pool_info.poolSizeCount=2; pool_info.pPoolSizes=sizes; require(a.CreateDescriptorPool(a.device,&pool_info,nullptr,&pool),"SNORM calibration pool"); allocate.descriptorPool=pool; allocate.pSetLayouts=&a.descriptors; require(a.AllocateDescriptorSets(a.device,&allocate,&set),"SNORM calibration descriptors");
    VkDescriptorImageInfo image_descriptor{a.sampler,view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buffer_descriptor{output.handle,0,32}; writes[0].dstSet=writes[1].dstSet=set; writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pBufferInfo=nullptr; writes[0].pImageInfo=&image_descriptor; writes[1].pBufferInfo=&buffer_descriptor; a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    resources.begin(); VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={4,2,1}; a.CmdCopyBufferToImage(a.command,input.handle,image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    uint32_t push[8]={4,2,1,0,1,0,0,0}; a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push); a.CmdDispatch(a.command,1,1,1);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=output.handle; host.size=VK_WHOLE_SIZE; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); a.submit();
    const bool equal=std::memcmp(output.mapped,expected.data(),32)==0;
    std::printf("SNORM_SAMPLING: literal -128/-127 canonicalized to -127; %s\n",equal?"PASS":"FAIL"); check(equal,"Independent literal RGBA8_SNORM sampling/rounding calibration"); resources.reset("SNORM calibration reset");
    resources.clear();
    stage("signed diagnostics complete");
}

void layout_tests() {
    VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2}; r.imageExtent={7,5,1}; pvb::CopyLayout out{};
    check(pvb::copy_layout(VK_FORMAT_BC3_UNORM_BLOCK,r,{7,5,1},1,2,out) && out.input_bytes==128 && out.output_bytes==280,"7x5 two-layer block sizing");
    r.bufferRowLength=16; r.bufferImageHeight=12; check(pvb::copy_layout(VK_FORMAT_BC3_UNORM_BLOCK,r,{7,5,1},1,2,out) && out.input_bytes==288,"Padded rows/layers exclude trailing padding");
    r.bufferRowLength=UINT32_MAX-3; r.bufferImageHeight=UINT32_MAX-3; r.imageSubresource.layerCount=32; check(!pvb::copy_layout(VK_FORMAT_BC5_UNORM_BLOCK,r,{7,5,1},1,32,out),"Overflowing padded strides rejected");
    r={}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,2,0,1}; r.imageExtent={1,1,1}; check(pvb::copy_layout(VK_FORMAT_BC4_SNORM_BLOCK,r,{7,5,1},3,1,out)&&out.input_bytes==8&&out.output_bytes==4,"1x1 final mip keeps one full block");
    r.bufferOffset=UINT64_MAX-3; check(!pvb::copy_layout(VK_FORMAT_BC4_SNORM_BLOCK,r,{7,5,1},3,1,out),"Offset overflow/misalignment rejected");
    r.bufferOffset=0; r.imageSubresource.layerCount=0; check(!pvb::copy_layout(VK_FORMAT_BC4_SNORM_BLOCK,r,{7,5,1},3,1,out),"Zero layers rejected");
    check(pvb::compatible_view(VK_FORMAT_BC3_UNORM_BLOCK,VK_FORMAT_BC3_SRGB_BLOCK),"Valid same-family SRGB view");
    check(!pvb::compatible_view(VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC4_UNORM_BLOCK)&&!pvb::compatible_view(VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC4_SNORM_BLOCK),"Cross-family/signed reinterpretation rejected");
}
void capability_tests(App &a) {
    constexpr VkFormatFeatureFlags expected=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    for(uint32_t n=131;n<=142;++n) { VkFormatProperties p{}; a.GetPhysicalDeviceFormatProperties(a.physical,VkFormat(n),&p); check(p.optimalTilingFeatures==expected && !p.linearTilingFeatures && !p.bufferFeatures,"Truthful BC optimal-only sampled/upload features"); VkFormatProperties3 ext{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3}; VkFormatProperties2 p2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2}; p2.pNext=&ext; a.GetPhysicalDeviceFormatProperties2(a.physical,VkFormat(n),&p2); check(ext.optimalTilingFeatures==expected && !ext.linearTilingFeatures && !ext.bufferFeatures,"FormatProperties3 matches legacy planes"); }
    VkImageFormatProperties p{}; check(a.GetPhysicalDeviceImageFormatProperties(a.physical,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&p)==VK_SUCCESS,"Supported BC image query");
    check(a.GetPhysicalDeviceImageFormatProperties(a.physical,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_IMAGE_TYPE_3D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,0,&p)==VK_ERROR_FORMAT_NOT_SUPPORTED,"Unsupported BC 3D rejected");
    check(a.GetPhysicalDeviceImageFormatProperties(a.physical,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_STORAGE_BIT,0,&p)==VK_ERROR_FORMAT_NOT_SUPPORTED,"Unsupported BC storage rejected");
    check(a.GetPhysicalDeviceImageFormatProperties(a.physical,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,0,&p)==VK_SUCCESS,"Compressed transfer-source texture query supported");
    for(auto format:{VK_FORMAT_BC6H_UFLOAT_BLOCK,VK_FORMAT_BC6H_SFLOAT_BLOCK}) { VkFormatProperties props{}; a.GetPhysicalDeviceFormatProperties(a.physical,format,&props); check(!props.optimalTilingFeatures && a.GetPhysicalDeviceImageFormatProperties(a.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_SAMPLED_BIT,0,&p)==VK_ERROR_FORMAT_NOT_SUPPORTED,"BC6 explicitly unsupported"); }
    uint32_t count=0; require(a.EnumerateDeviceExtensionProperties(a.physical,nullptr,&count,nullptr),"Extensions count"); std::vector<VkExtensionProperties> ext(count); require(a.EnumerateDeviceExtensionProperties(a.physical,nullptr,&count,ext.data()),"Extensions"); bool blocked=false; for(const auto &x:ext) if(!std::strcmp(x.extensionName,"VK_EXT_descriptor_buffer")||!std::strcmp(x.extensionName,"VK_EXT_shader_object")||!std::strcmp(x.extensionName,"VK_KHR_maintenance6")) blocked=true; check(!blocked,"Unsupported state-changing extensions filtered");
    check(a.gdpa(a.device,"vkDefinitelyNotARealVulkanCommand")==nullptr,"Unknown GPA remains null");
}
void case_test(App &a,VkFormat format,uint32_t width,uint32_t height,uint32_t layers,uint32_t mip,bool padded,bool random,bool transfer_layout,bool copy2,bool partial=false,bool srgb_view=false) {
    const uint32_t fullw=width,fullh=height; const uint32_t w=std::max(1u,width>>mip),h=std::max(1u,height>>mip);
    VkBufferImageCopy region{}; region.bufferOffset=64; region.bufferRowLength=padded?((w+3)/4+2)*4:0; region.bufferImageHeight=padded?((h+3)/4+1)*4:0; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,layers}; region.imageExtent={w,h,1};
    if(partial) { region.imageOffset={4,4,0}; region.imageExtent={w-4,h-4,1}; }
    pvb::CopyLayout sizes{}; if(!pvb::copy_layout(format,region,{fullw,fullh,1},mip+1,layers,sizes)) throw std::runtime_error("Bad test copy layout");
    Buffer input=a.buffer(sizes.input_bytes+64,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); Buffer output=a.buffer(sizes.output_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); Buffer compressed=a.buffer(sizes.input_bytes+64,VK_BUFFER_USAGE_TRANSFER_DST_BIT); std::memset(input.mapped,0xcc,size_t(sizes.input_bytes+64)); std::memset(output.mapped,0xa5,size_t(sizes.output_bytes)); std::memset(compressed.mapped,0xa5,size_t(sizes.input_bytes+64));
    const uint32_t rw=region.imageExtent.width,rh=region.imageExtent.height; const uint32_t br=sizes.blocks_row,bh=sizes.blocks_height,bs=pvb::block_bytes(format); std::vector<uint8_t> expected(size_t(sizes.output_bytes)); uint32_t seed=0x7410d0abu+uint32_t(format)*101u+width+height+mip;
    auto raw=static_cast<uint8_t*>(input.mapped)+64;
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<(rh+3)/4;++y) for(uint32_t x=0;x<(rw+3)/4;++x) {
        uint8_t *b=raw+((uint64_t(l)*bh+y)*br+x)*bs; auto fixed=known_block(format,padded); std::memcpy(b,fixed.data(),bs); if(random) for(uint32_t n=0;n<bs;n+=4) write32(b+n,random_word(seed)); auto decoded=reference(format,b);
        for(uint32_t py=0;py<4;++py) for(uint32_t px=0;px<4;++px) if(x*4+px<rw&&y*4+py<rh) std::memcpy(expected.data()+((uint64_t(l)*rh+y*4+py)*rw+x*4+px)*4,decoded.data()+(py*4+px)*4,4);
    }
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image_info.imageType=VK_IMAGE_TYPE_2D; image_info.format=format; image_info.extent={fullw,fullh,1}; image_info.mipLevels=mip+1; image_info.arrayLayers=layers; image_info.samples=VK_SAMPLE_COUNT_1_BIT; image_info.tiling=VK_IMAGE_TILING_OPTIMAL; image_info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; image_info.sharingMode=VK_SHARING_MODE_EXCLUSIVE; if(layers==6&&width==height) image_info.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkFormat view_formats[2]={format,VkFormat(uint32_t(format)+1)}; VkImageFormatListCreateInfo view_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; view_list.viewFormatCount=2; view_list.pViewFormats=view_formats; if(srgb_view) { image_info.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; image_info.pNext=&view_list; }
    VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS}; query.pCreateInfo=&image_info; VkMemoryRequirements2 before{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; a.GetDeviceImageMemoryRequirements(a.device,&query,&before);
    if(srgb_view) { auto alias=reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(a.gdpa(a.device,"vkGetDeviceImageMemoryRequirementsKHR")); if(alias) { VkMemoryRequirements2 aliased{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2}; alias(a.device,&query,&aliased); check(aliased.memoryRequirements.size==before.memoryRequirements.size && aliased.memoryRequirements.memoryTypeBits==before.memoryRequirements.memoryTypeBits,"Maintenance4 KHR memory query maps view list identically"); ++alias_tests; } else ++alias_skipped; }
    VkImage image{}; require(a.CreateImage(a.device,&image_info,nullptr,&image),"CreateImage BC"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,image,&req); check(before.memoryRequirements.size==req.size && before.memoryRequirements.memoryTypeBits==req.memoryTypeBits,"CreateImage/memory-query target agreement");
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=req.size; allocate.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0); VkDeviceMemory image_memory{}; require(a.AllocateMemory(a.device,&allocate,nullptr,&image_memory),"Allocate image"); require(a.BindImageMemory(a.device,image,image_memory,0),"BindImageMemory");
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view_info.image=image; view_info.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; view_info.format=srgb_view?view_formats[1]:format; view_info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,layers}; VkImageView view{}; require(a.CreateImageView(a.device,&view_info,nullptr,&view),"CreateImageView BC");
    VkDescriptorPoolSize pool_sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool_info.maxSets=1; pool_info.poolSizeCount=2; pool_info.pPoolSizes=pool_sizes; VkDescriptorPool descriptors{}; require(a.CreateDescriptorPool(a.device,&pool_info,nullptr,&descriptors),"DescriptorPool probe"); VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=descriptors; ds.descriptorSetCount=1; ds.pSetLayouts=&a.descriptors; VkDescriptorSet set{}; require(a.AllocateDescriptorSets(a.device,&ds,&set),"DescriptorSet probe");
    const VkImageLayout image_layout=transfer_layout?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo image_descriptor{a.sampler,view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo buffer_descriptor{output.handle,0,sizes.output_bytes}; VkWriteDescriptorSet writes[2]{}; for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=set; writes[n].dstBinding=n; writes[n].descriptorCount=1; } writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&image_descriptor; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&buffer_descriptor; a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    a.begin(); VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=image_layout; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,layers}; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    const bool srgb=pvb::rgba_format(view_info.format)==VK_FORMAT_R8G8B8A8_SRGB; const uint32_t push[8]={rw,rh,layers,0,pvb::is_signed(format)?1u:0u,srgb?1u:0u,uint32_t(region.imageOffset.x),uint32_t(region.imageOffset.y)};
    // Deliberately bind caller compute pipeline/sets/constants before upload; do not rebind afterwards.
    VkPushConstantRange restore_range{VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,32}; VkPipelineLayoutCreateInfo restore_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; restore_info.setLayoutCount=1; restore_info.pSetLayouts=&a.descriptors; restore_info.pushConstantRangeCount=1; restore_info.pPushConstantRanges=&restore_range; VkPipelineLayout caller_layout{}; require(a.CreatePipelineLayout(a.device,&restore_info,nullptr,&caller_layout),"Temporary caller pipeline layout");
    a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,caller_layout,0,1,&set,0,nullptr); a.CmdPushConstants(a.command,caller_layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push);
    // Vulkan permits this layout destruction after recording. The wrapper must retain compatible owned replay state.
    a.DestroyPipelineLayout(a.device,caller_layout,nullptr);
    if(copy2) { VkBufferImageCopy2 r{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2}; r.bufferOffset=region.bufferOffset; r.bufferRowLength=region.bufferRowLength; r.bufferImageHeight=region.bufferImageHeight; r.imageSubresource=region.imageSubresource; r.imageOffset=region.imageOffset; r.imageExtent=region.imageExtent; VkCopyBufferToImageInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2}; info.srcBuffer=input.handle; info.dstImage=image; info.dstImageLayout=image_layout; info.regionCount=1; info.pRegions=&r; auto copy_alias=reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(a.gdpa(a.device,"vkCmdCopyBufferToImage2KHR")); if(format==VK_FORMAT_BC5_SNORM_BLOCK && copy_alias) { copy_alias(a.command,&info); ++alias_tests; } else a.CmdCopyBufferToImage2(a.command,&info); }
    else a.CmdCopyBufferToImage(a.command,input.handle,image,image_layout,1,&region);
    barrier.oldLayout=image_layout; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);

    a.CmdDispatch(a.command,(rw+7)/8,(rh+7)/8,layers);
    // Exercise a sampled-image dependency chain, with no early transfer-read
    // visibility for the canonical store: upload -> shader read -> readback.
    barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    if(copy2) {
        VkBufferImageCopy2 r{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2}; r.bufferOffset=region.bufferOffset; r.bufferRowLength=region.bufferRowLength; r.bufferImageHeight=region.bufferImageHeight; r.imageSubresource=region.imageSubresource; r.imageOffset=region.imageOffset; r.imageExtent=region.imageExtent;
        VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2}; info.srcImage=image; info.srcImageLayout=VK_IMAGE_LAYOUT_GENERAL; info.dstBuffer=compressed.handle; info.regionCount=1; info.pRegions=&r;
        const bool alias=(format==VK_FORMAT_BC5_SNORM_BLOCK); a.dp<PFN_vkCmdCopyImageToBuffer2>(alias?"vkCmdCopyImageToBuffer2KHR":"vkCmdCopyImageToBuffer2")(a.command,&info); if(alias) ++alias_tests;
    } else a.CmdCopyImageToBuffer(a.command,image,VK_IMAGE_LAYOUT_GENERAL,compressed.handle,1,&region);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=output.handle; host.size=VK_WHOLE_SIZE; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); host.buffer=compressed.handle; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); a.submit(copy2);
    auto actual=static_cast<const uint8_t*>(output.mapped); bool equal=true; uint32_t delta=0; for(size_t n=0;n<expected.size();++n) { int aa=pvb::is_signed(format)?int(int8_t(actual[n])):int(actual[n]),ee=pvb::is_signed(format)?int(int8_t(expected[n])):int(expected[n]); const uint32_t difference=uint32_t(std::abs(aa-ee)); delta=std::max(delta,difference); if(difference>1) { if(equal) std::fprintf(stderr,"Mismatch format=%u %ux%u layers=%u mip=%u padded=%d random=%d byte=%zu got=%d expected=%d\n",uint32_t(format),rw,rh,layers,mip,padded,random,n,aa,ee); equal=false; } } max_difference=std::max(max_difference,delta); check(equal,"GPU sampled BC pixels match independent bcdec reference (+/-1 LSB)");
    if(!random&&!padded&&mip==0&&!partial) { bool literal=true; if(format<=134) literal=actual[0]==255&&actual[1]==0&&actual[2]==0&&actual[3]==255; else if(format<=136) literal=actual[0]==0&&actual[1]==0&&actual[2]==0&&actual[3]==0; else if(format<=138) literal=actual[0]==0&&actual[1]==0&&actual[2]==0&&actual[3]==255; else literal=actual[0]==(pvb::is_signed(format)?129u:255u)&&actual[3]==(pvb::is_signed(format)?127u:255u); check(literal,"Independent literal endpoint/alpha fixture"); }
    std::vector<uint8_t> exact(size_t(sizes.input_bytes+64),0xa5);
    for(uint32_t l=0;l<layers;++l) for(uint32_t y=0;y<(rh+3)/4;++y) {
        const size_t offset=64+(size_t(l)*bh+y)*br*bs;
        std::memcpy(exact.data()+offset,static_cast<const uint8_t*>(input.mapped)+offset,size_t((rw+3)/4)*bs);
    }
    check(!std::memcmp(compressed.mapped,exact.data(),exact.size()),"Compressed readback preserves every original block byte and leaves pitch padding untouched");
    PvbStats stats{}; a.stats(&stats); check(stats.live_upload_resources==1,"Upload resources retained after completed submission for replay");
    if(format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK&&!random&&!padded) { std::vector<uint8_t> first_result(actual,actual+sizes.output_bytes); a.submit_ended(false); check(std::memcmp(output.mapped,first_result.data(),size_t(sizes.output_bytes))==0,"Ended command buffer can be resubmitted"); }
    if(copy2) require(a.ResetCommandPool(a.device,a.pool,0),"ResetCommandPool"); else require(a.ResetCommandBuffer(a.command,0),"ResetCommandBuffer"); a.stats(&stats); check(stats.live_upload_resources==0,"Safe reset releases upload resources");
    a.DestroyDescriptorPool(a.device,descriptors,nullptr); a.DestroyImageView(a.device,view,nullptr); a.DestroyImage(a.device,image,nullptr); a.FreeMemory(a.device,image_memory,nullptr); a.destroy(input); a.destroy(output); a.destroy(compressed);
}
void batch_test(App &a) {
    // Match NFS: one call uploads a complete mip chain across all six cube faces.
    Buffer input=a.buffer(400,VK_BUFFER_USAGE_TRANSFER_SRC_BIT), output=a.buffer(2328,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(input.mapped,0,400); std::memset(output.mapped,0xa5,2328);
    std::array<VkBufferImageCopy,4> regions{}; const uint32_t input_offsets[4]={64,256,304,352}; const uint32_t output_offsets[4]={0,1536,2048,2304};
    for(uint32_t mip=0;mip<4;++mip) {
        uint32_t size=8u>>mip,blocks=(size+3)/4; auto &r=regions[mip]; r.bufferOffset=input_offsets[mip]; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,6}; r.imageExtent={size,size,1};
        for(uint32_t b=0;b<blocks*blocks*6;++b) { auto ptr=static_cast<uint8_t*>(input.mapped)+input_offsets[mip]+b*8; ptr[0]=0; ptr[1]=0xf8; }
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK; info.extent={8,8,1}; info.mipLevels=4; info.arrayLayers=6; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image{}; require(a.CreateImage(a.device,&info,nullptr,&image),"Batch image"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,image,&req); VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize=req.size; allocation.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0); VkDeviceMemory memory{}; require(a.AllocateMemory(a.device,&allocation,nullptr,&memory),"Batch image memory"); require(a.BindImageMemory(a.device,image,memory,0),"Batch bind image");
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4}}; VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=4; pool.poolSizeCount=2; pool.pPoolSizes=ps; VkDescriptorPool descriptors{}; require(a.CreateDescriptorPool(a.device,&pool,nullptr,&descriptors),"Batch descriptors");
    std::array<VkImageView,4> views{}; std::array<VkDescriptorSet,4> sets{};
    for(uint32_t mip=0;mip<4;++mip) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; v.image=image; v.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; v.format=info.format; v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,1,0,6}; require(a.CreateImageView(a.device,&v,nullptr,&views[mip]),"Batch view");
        VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=descriptors; ds.descriptorSetCount=1; ds.pSetLayouts=&a.descriptors; require(a.AllocateDescriptorSets(a.device,&ds,&sets[mip]),"Batch set");
        VkDescriptorImageInfo id{a.sampler,views[mip],VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo bd{output.handle,output_offsets[mip],(8u>>mip)*(8u>>mip)*6u*4u}; VkWriteDescriptorSet writes[2]{}; for(uint32_t n=0;n<2;++n) { writes[n].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[n].dstSet=sets[mip]; writes[n].dstBinding=n; writes[n].descriptorCount=1; } writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&id; writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&bd; a.UpdateDescriptorSets(a.device,2,writes,0,nullptr);
    }
    a.begin(); VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.image=image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,4,0,6}; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    uint32_t push[8]={8,8,6,0,0,0,0,0}; a.CmdBindPipeline(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.pipeline); a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&sets[0],0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push);
    a.CmdCopyBufferToImage(a.command,input.handle,image,VK_IMAGE_LAYOUT_GENERAL,4,regions.data());
    barrier.oldLayout=VK_IMAGE_LAYOUT_GENERAL; barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    for(uint32_t mip=0;mip<4;++mip) { if(mip) { push[0]=push[1]=8u>>mip; a.CmdBindDescriptorSets(a.command,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&sets[mip],0,nullptr); a.CmdPushConstants(a.command,a.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push); } a.CmdDispatch(a.command,1,1,6); }
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=output.handle; host.size=VK_WHOLE_SIZE; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); a.submit();
    for(uint32_t mip=0;mip<4;++mip) { auto pixels=static_cast<uint8_t*>(output.mapped)+output_offsets[mip]; bool correct=true; for(uint32_t n=0;n<(8u>>mip)*(8u>>mip)*6u;++n) if(pixels[n*4]!=255 || pixels[n*4+1] || pixels[n*4+2] || pixels[n*4+3]!=255) correct=false; check(correct,"Batched full cube mip literal RGBA output"); }
    PvbStats stats{}; a.stats(&stats); check(stats.live_upload_resources==4,"Batched resources remain owned until reset"); require(a.ResetCommandPool(a.device,a.pool,0),"Batch reset"); a.stats(&stats); check(stats.live_upload_resources==0,"Batched reset releases all four resources");
    a.DestroyDescriptorPool(a.device,descriptors,nullptr); for(auto v:views) a.DestroyImageView(a.device,v,nullptr); a.DestroyImage(a.device,image,nullptr); a.FreeMemory(a.device,memory,nullptr); a.destroy(input); a.destroy(output);
}
void plain_copy_test(App &a) {
    Buffer input=a.buffer(64,VK_BUFFER_USAGE_TRANSFER_SRC_BIT),output=a.buffer(64,VK_BUFFER_USAGE_TRANSFER_DST_BIT); for(uint32_t n=0;n<64;++n) static_cast<uint8_t*>(input.mapped)[n]=uint8_t(n*37u+11u);
    std::array<VkImage,2> images{}; std::array<VkDeviceMemory,2> memory{};
    for(uint32_t n=0;n<2;++n) { VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R8G8B8A8_UNORM; ci.extent={4,4,1}; ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; require(a.CreateImage(a.device,&ci,nullptr,&images[n]),"Plain image"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,images[n],&req); VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=req.size; allocate.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0); require(a.AllocateMemory(a.device,&allocate,nullptr,&memory[n]),"Plain image memory"); require(a.BindImageMemory(a.device,images[n],memory[n],0),"Plain image bind"); }
    for(uint32_t variant=0;variant<2;++variant) {
        a.begin(); std::array<VkImageMemoryBarrier,2> barriers{}; for(uint32_t n=0;n<2;++n) { auto &b=barriers[n]; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=images[n]; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; } a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers.data());
        VkBufferImageCopy upload{}; upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; upload.imageExtent={4,4,1}; a.CmdCopyBufferToImage(a.command,input.handle,images[0],VK_IMAGE_LAYOUT_GENERAL,1,&upload);
        barriers[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL; barriers[0].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,barriers.data());
        VkImageCopy region{}; region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.extent={4,4,1}; if(!variant) a.CmdCopyImage(a.command,images[0],VK_IMAGE_LAYOUT_GENERAL,images[1],VK_IMAGE_LAYOUT_GENERAL,1,&region); else { VkImageCopy2 copy{VK_STRUCTURE_TYPE_IMAGE_COPY_2}; copy.srcSubresource=region.srcSubresource; copy.dstSubresource=region.dstSubresource; copy.extent=region.extent; VkCopyImageInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2}; info.srcImage=images[0]; info.srcImageLayout=VK_IMAGE_LAYOUT_GENERAL; info.dstImage=images[1]; info.dstImageLayout=VK_IMAGE_LAYOUT_GENERAL; info.regionCount=1; info.pRegions=&copy; a.CmdCopyImage2(a.command,&info); }
        barriers[1].oldLayout=VK_IMAGE_LAYOUT_GENERAL; barriers[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barriers[1]); a.CmdCopyImageToBuffer(a.command,images[1],VK_IMAGE_LAYOUT_GENERAL,output.handle,1,&upload); VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; host.buffer=output.handle; host.size=64; host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; a.CmdPipelineBarrier(a.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr); a.submit(); check(!std::memcmp(input.mapped,output.mapped,64),"NonBC image copy/upload/readback passes through unchanged"); require(a.ResetCommandBuffer(a.command,0),"Plain copy reset");
    }
    for(uint32_t n=0;n<2;++n) { a.DestroyImage(a.device,images[n],nullptr); a.FreeMemory(a.device,memory[n],nullptr); } a.destroy(input); a.destroy(output);
}
void rejection_tests(App &a) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_BC1_RGBA_UNORM_BLOCK; info.extent={4,4,1}; info.mipLevels=info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL; info.usage=VK_IMAGE_USAGE_STORAGE_BIT; VkImage image{}; check(a.CreateImage(a.device,&info,nullptr,&image)==VK_ERROR_FORMAT_NOT_SUPPORTED,"Unsupported BC image creation fails");
    VkFormat bad[2]={info.format,VK_FORMAT_BC4_UNORM_BLOCK}; VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO}; list.viewFormatCount=2; list.pViewFormats=bad; info.pNext=&list; info.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; check(a.CreateImage(a.device,&info,nullptr,&image)==VK_ERROR_FORMAT_NOT_SUPPORTED,"Cross-family mutable list rejected");
    info.pNext=nullptr; info.flags=0; require(a.CreateImage(a.device,&info,nullptr,&image),"Rejection test image"); VkMemoryRequirements req{}; a.GetImageMemoryRequirements(a.device,image,&req); VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=req.size; allocate.memoryTypeIndex=a.memory_index(req.memoryTypeBits,0); VkDeviceMemory memory{}; require(a.AllocateMemory(a.device,&allocate,nullptr,&memory),"Reject image memory"); require(a.BindImageMemory(a.device,image,memory,0),"Reject bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_BC4_UNORM_BLOCK; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; VkImageView result{}; check(a.CreateImageView(a.device,&view,nullptr,&result)==VK_ERROR_FORMAT_NOT_SUPPORTED,"Cross-family image view rejected");
    Buffer input=a.buffer(16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT); a.begin(); VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={4,4,1}; region.bufferOffset=1; a.CmdCopyBufferToImage(a.command,input.handle,image,VK_IMAGE_LAYOUT_GENERAL,1,&region); check(a.EndCommandBuffer(a.command)!=VK_SUCCESS,"Invalid upload fails command recording (no fake success)"); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&a.command; check(a.QueueSubmit(a.queue,1,&submit,VK_NULL_HANDLE)!=VK_SUCCESS,"Invalid recorded upload cannot submit"); require(a.ResetCommandBuffer(a.command,0),"Reset failed recording"); VkImageCreateInfo src_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; src_info.imageType=VK_IMAGE_TYPE_2D; src_info.format=VK_FORMAT_R32G32_UINT; src_info.extent={1,1,1}; src_info.mipLevels=src_info.arrayLayers=1; src_info.samples=VK_SAMPLE_COUNT_1_BIT; src_info.tiling=VK_IMAGE_TILING_OPTIMAL; src_info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT; VkImage source{}; require(a.CreateImage(a.device,&src_info,nullptr,&source),"Raw BC block source image"); VkMemoryRequirements src_req{}; a.GetImageMemoryRequirements(a.device,source,&src_req); allocate.allocationSize=src_req.size; allocate.memoryTypeIndex=a.memory_index(src_req.memoryTypeBits,0); VkDeviceMemory src_memory{}; require(a.AllocateMemory(a.device,&allocate,nullptr,&src_memory),"Raw source memory"); require(a.BindImageMemory(a.device,source,src_memory,0),"Raw source bind");
    auto copy_alias=reinterpret_cast<PFN_vkCmdCopyImage2>(a.gdpa(a.device,"vkCmdCopyImage2KHR"));
    if(!copy_alias) ++alias_skipped;
    for(uint32_t variant=0;variant<(copy_alias?3u:2u);++variant) { a.begin(); VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.extent={1,1,1}; if(!variant) a.CmdCopyImage(a.command,source,VK_IMAGE_LAYOUT_GENERAL,image,VK_IMAGE_LAYOUT_GENERAL,1,&copy); else { VkImageCopy2 region2{VK_STRUCTURE_TYPE_IMAGE_COPY_2}; region2.srcSubresource=copy.srcSubresource; region2.dstSubresource=copy.dstSubresource; region2.extent=copy.extent; VkCopyImageInfo2 info2{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2}; info2.srcImage=source; info2.srcImageLayout=VK_IMAGE_LAYOUT_GENERAL; info2.dstImage=image; info2.dstImageLayout=VK_IMAGE_LAYOUT_GENERAL; info2.regionCount=1; info2.pRegions=&region2; if(variant==2) { copy_alias(a.command,&info2); ++alias_tests; } else a.CmdCopyImage2(a.command,&info2); } check(a.EndCommandBuffer(a.command)!=VK_SUCCESS,"BC image-to-image destination rejects legacy/core2 safely"); check(a.QueueSubmit(a.queue,1,&submit,VK_NULL_HANDLE)!=VK_SUCCESS,"Rejected image copy cannot submit"); require(a.ResetCommandBuffer(a.command,0),"Image copy failure reset"); }
    a.DestroyImage(a.device,source,nullptr); a.FreeMemory(a.device,src_memory,nullptr); a.DestroyImage(a.device,image,nullptr); a.FreeMemory(a.device,memory,nullptr); a.destroy(input);
}
#include "transfer_probe.inc"
}
int main(int argc,char **argv) {
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    std::atexit([] { stage("process atexit (normal termination)"); });
    if(argc<2) { std::fprintf(stderr,"Usage: pixel_bc_probe /absolute/path/libvulkan_pixel_bc.so [--validation]\n"); return 2; }
    try {
        layout_tests(); App app; init(app,argv[1],argc>2&&!std::strcmp(argv[2],"--validation")); capability_tests(app); signed_diagnostics(app);
        for(uint32_t f=131;f<=142;++f) { case_test(app,VkFormat(f),4,4,1,0,false,false,false,false); case_test(app,VkFormat(f),7,5,2,0,true,false,false,true); case_test(app,VkFormat(f),9,7,2,0,true,true,true,false); case_test(app,VkFormat(f),7,5,1,1,false,true,false,true); case_test(app,VkFormat(f),7,5,1,2,false,true,true,false); }
        case_test(app,VK_FORMAT_BC3_UNORM_BLOCK,8,8,6,0,true,true,false,true); case_test(app,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,7,7,1,0,false,true,false,false,true); case_test(app,VK_FORMAT_BC3_UNORM_BLOCK,7,5,1,0,false,true,false,false,false,true);
        stage("BC upload cases complete"); batch_test(app); plain_copy_test(app); transfer_tests(app); stage("expected rejection cases begin"); rejection_tests(app); stage("expected rejection cases complete"); PvbStats stats{}; app.stats(&stats); check(stats.decode_dispatches==72+4*(12+(pvb::is_bc(VK_FORMAT_BC7_UNORM_BLOCK)?2:0)),"All original and transfer-fixture uploads actually recorded GPU decode"); check(stats.recording_failures==(app.gdpa(app.device,"vkCmdCopyImage2KHR")?4u:3u),"Rejected upload/image copies counted once each"); check(!stats.backend_recursion_rejections,"No backend recursion"); check(!stats.live_upload_resources,"No live upload resources at finish"); const auto transfers=transfer_stats(app); check(!transfers.live_block_stores && !transfers.live_block_bytes,"No live retained BC stores/bytes after image and command cleanup"); std::printf("TRANSFER_STORES: %llu; bytes: %llu; recorded copy bytes: %llu\n",static_cast<unsigned long long>(transfers.live_block_stores),static_cast<unsigned long long>(transfers.live_block_bytes),static_cast<unsigned long long>(transfers.recorded_block_copy_bytes)); app.shutdown(); stage("main resumed after shutdown"); check(!validation_errors,"No Vulkan validation errors including teardown"); std::printf("VALIDATION_ENABLED: %d\nBACKEND: %s\nGPU_DISPATCHES: %llu\nMAX_REFERENCE_DIFFERENCE: %u LSB\nALIAS_CHECKS: %u; unavailable: %u\nVALIDATION_ERRORS: %u\n",(argc>2&&!std::strcmp(argv[2],"--validation"))?1:0,stats.backend,static_cast<unsigned long long>(stats.decode_dispatches),max_difference,alias_tests,alias_skipped,validation_errors);
    } catch(const std::exception &e) { ++failed; std::fprintf(stderr,"ERROR: %s\n",e.what()); }
    std::printf("RESULT: %u passed; %u failed\n",passed,failed); std::printf("STAGE: main returning status=%d\n",failed?1:0); return failed?1:0;
}
