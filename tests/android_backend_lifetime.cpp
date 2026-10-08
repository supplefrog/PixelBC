// SPDX-License-Identifier: MIT
// Compile the actual Android backend path on the host with bounded HAL/DSO mocks.
// An early sentinel checks again after all wrapper globals undergo natural exit.
#include "android_hal.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
unsigned vendor_closes=0,vendor_unloads=0,device_destroys=0,instance_destroys=0;
struct ExitEvidence {
    ~ExitEvidence() {
        if(vendor_closes || vendor_unloads || device_destroys!=1 || instance_destroys!=1) std::abort();
        std::puts("ANDROID_BACKEND_LIFETIME: vendor HAL/DSO retained through normal exit; Vulkan device/instance destroyed once");
    }
} exit_evidence;
void *mock_library_open(const char*,int);
void *mock_symbol(void*,const char*);
[[maybe_unused]] int mock_library_close(void*);
}
#define __ANDROID__ 1
#define dlopen mock_library_open
#define dlsym mock_symbol
#define dlclose mock_library_close
#include "../src/wrapper.cpp"
#undef dlopen
#undef dlsym
#undef dlclose
namespace {
VKAPI_ATTR VkResult VKAPI_CALL mock_create_instance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL mock_extensions(const char*,uint32_t *count,VkExtensionProperties*) { *count=0; return VK_SUCCESS; }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_gipa(VkInstance,const char *name) {
    if(!std::strcmp(name,"vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(mock_create_instance);
    if(!std::strcmp(name,"vkEnumerateInstanceExtensionProperties")) return reinterpret_cast<PFN_vkVoidFunction>(mock_extensions);
    return nullptr;
}
int mock_vendor_close(hw_device_t*) { ++vendor_closes; return 0; }
hwvulkan_device_t vendor_device{};
int mock_hal_open(const hw_module_t*,const char *id,hw_device_t **out) {
    if(std::strcmp(id,"vk0")) return -1;
    vendor_device.common.tag=HARDWARE_DEVICE_TAG; vendor_device.common.version=HWVULKAN_DEVICE_API_VERSION_0_1; vendor_device.common.close=mock_vendor_close;
    vendor_device.CreateInstance=mock_create_instance; vendor_device.EnumerateInstanceExtensionProperties=mock_extensions; vendor_device.GetInstanceProcAddr=mock_gipa; *out=&vendor_device.common; return 0;
}
hw_module_methods_t vendor_methods{mock_hal_open};
hw_module_t vendor_module{HARDWARE_MODULE_TAG,HWVULKAN_MODULE_API_VERSION_0_1,HARDWARE_HAL_API_VERSION,"vulkan","mock vendor","fixture",&vendor_methods,nullptr,{0}};
void *mock_library_open(const char *path,int) { return !std::strcmp(path,"/vendor/lib64/hw/vulkan.mali.so")?reinterpret_cast<void*>(uintptr_t(11)):nullptr; }
void *mock_symbol(void*,const char *name) { return !std::strcmp(name,"HMI")?&vendor_module:nullptr; }
int mock_library_close(void*) { ++vendor_unloads; return 0; }
VKAPI_ATTR void VKAPI_CALL mock_destroy_device(VkDevice,const VkAllocationCallbacks*) { ++device_destroys; }
VKAPI_ATTR void VKAPI_CALL mock_destroy_instance(VkInstance,const VkAllocationCallbacks*) { ++instance_destroys; }
}
int main() {
    if(!ready() || backend.gipa!=mock_gipa || backend_recursion.load()) return 1;
    { Backend temporary; temporary.library=backend.library; temporary.hal_device=backend.hal_device; }
    if(vendor_closes || vendor_unloads) return 1;
    auto instance=std::make_shared<Instance>(); instance->handle=reinterpret_cast<VkInstance>(uintptr_t(1)); instance->DestroyInstance=mock_destroy_instance; instances[instance->handle]=instance;
    auto device=std::make_shared<Device>(); device->handle=reinterpret_cast<VkDevice>(uintptr_t(2)); device->instance=instance; device->DestroyDevice=mock_destroy_device; devices[device->handle]=device;
    pvbDestroyDevice(device->handle,nullptr); pvbDestroyInstance(instance->handle,nullptr);
    return device_destroys==1 && instance_destroys==1 && !vendor_closes && !vendor_unloads?0:1;
}
