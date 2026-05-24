#pragma once

// Vulkan instance + surface + physical device + logical device + queues + VMA
// allocator, bundled. Created via vk-bootstrap internally; consumers get raw
// Vulkan handles only (no vk-bootstrap types leak in the public API).
//
// Required features: Vulkan 1.3, dynamicRendering + synchronization2 (1.3),
// timelineSemaphore + bufferDeviceAddress + descriptorIndexing +
// runtimeDescriptorArray + descriptorBindingPartiallyBound (1.2). Callers
// don't pick these — they're baked in because the rest of forfun_core
// assumes them.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>

namespace forfun {

struct DeviceCreateInfo {
    const char* appName          = "forfun-app";
    bool        enableValidation = true;

    // Called once after the Vulkan instance is created, BEFORE physical
    // device selection (the selector needs the surface to filter to queue
    // families that support presentation). Implementations typically wrap
    // glfwCreateWindowSurface or the equivalent for their windowing system.
    // The returned surface is owned by the Device and destroyed by
    // destroyDevice.
    std::function<VkSurfaceKHR(VkInstance)> createSurface;
};

struct Device {
    VkInstance               instance        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger  = VK_NULL_HANDLE;
    VkSurfaceKHR             surface         = VK_NULL_HANDLE;
    VkPhysicalDevice         physicalDevice  = VK_NULL_HANDLE;
    VkDevice                 device          = VK_NULL_HANDLE;
    VkQueue                  graphicsQueue   = VK_NULL_HANDLE;
    uint32_t                 graphicsFamily  = 0;
    VmaAllocator             allocator       = VK_NULL_HANDLE;
};

// Aborts on failure (matches the project's existing VK_CHECK style).
Device createDevice(const DeviceCreateInfo& info);

// Destroys allocator, device, surface, debug messenger, and instance in the
// right order. After this call, `dev` is reset to a default-constructed
// Device.
void destroyDevice(Device& dev);

} // namespace forfun
