// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2008 The Android Open Source Project
// Copyright 2015 The Android Open Source Project
// Modified: reduced to the LP64 Vulkan HAL ABI used by this wrapper.
// Minimal LP64 ABI declarations corresponding to AOSP hardware.h/hwvulkan.h.
// Android HAL interfaces are not part of the NDK. Field widths/order are ABI, not private C++ APIs.
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
static_assert(sizeof(void*)==8,"This wrapper targets ARM64 Android");
constexpr uint32_t HARDWARE_MODULE_TAG=0x48574d54u; // HWMT
constexpr uint32_t HARDWARE_DEVICE_TAG=0x48574454u; // HWDT
constexpr uint16_t HARDWARE_HAL_API_VERSION=0x0100u;
constexpr uint16_t HWVULKAN_MODULE_API_VERSION_0_1=0x0001u;
constexpr uint32_t HWVULKAN_DEVICE_API_VERSION_0_1=0x00010000u;
struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t { int (*open)(const hw_module_t*,const char*,hw_device_t**); };
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version,hal_api_version;
    const char *id,*name,*author;
    hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[25];
};
struct hw_device_t {
    uint32_t tag,version;
    hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(hw_device_t*);
};
struct hwvulkan_module_t { hw_module_t common; };
struct hwvulkan_device_t {
    hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};
static_assert(sizeof(hw_module_t)==248 && offsetof(hw_module_t,methods)==32);
static_assert(sizeof(hw_device_t)==120 && offsetof(hw_device_t,close)==112);
static_assert(sizeof(hwvulkan_device_t)==144 && offsetof(hwvulkan_device_t,GetInstanceProcAddr)==136);
