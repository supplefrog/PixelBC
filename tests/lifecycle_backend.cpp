// SPDX-License-Identifier: MIT
// Real DSO callback fixture: each callback writes while its library is mapped.
#include "android_hal.h"
#include <cstdio>
#include <cstdlib>
namespace {
void record(const char *event) { if(auto path=std::getenv("PVB_LIFECYCLE_LOG")) { if(auto file=std::fopen(path,"a")) { std::fprintf(file,"%s\n",event); std::fclose(file); } } }
int close_hal(hw_device_t*) { record("hal-close"); return 0; }
hwvulkan_device_t hal{};
VkResult wait_result=VK_SUCCESS;
__attribute__((constructor)) void load() { record("load"); hal.common.close=close_hal; }
__attribute__((destructor)) void unload() { record("unload"); }
}
extern "C" {
hw_device_t *fixture_hal() { return &hal.common; }
VkResult VKAPI_CALL fixture_wait(VkDevice) { record("wait"); return wait_result; }
void fixture_wait_error() { wait_result=VK_ERROR_DEVICE_LOST; }
void VKAPI_CALL fixture_pool(VkDevice,VkCommandPool,const VkAllocationCallbacks*) { record("pool"); }
void VKAPI_CALL fixture_device(VkDevice,const VkAllocationCallbacks*) { record("device"); }
void VKAPI_CALL fixture_instance(VkInstance,const VkAllocationCallbacks*) { record("instance"); }
}
