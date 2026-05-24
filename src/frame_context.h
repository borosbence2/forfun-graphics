#pragma once

// Per-frame engine plumbing: command pool + primary command buffer + 2
// semaphores (image-available, render-finished) + 1 in-flight fence.
// Allocate one of these for each frame-in-flight.
//
// The fence is created already-signaled so the very first frame's
// vkWaitForFences returns immediately.

#include "device.h"

#include <vulkan/vulkan.h>

namespace forfun {

struct FrameContext {
    VkCommandPool   pool           = VK_NULL_HANDLE;
    VkCommandBuffer cmd            = VK_NULL_HANDLE;   // owned by `pool`
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlight       = VK_NULL_HANDLE;
};

// Aborts on failure. Uses device.graphicsFamily for the command pool.
FrameContext createFrameContext(const Device& device);

// Destroys the fence, semaphores, and command pool (which frees the command
// buffer). After this call, `fc` is reset to a default-constructed
// FrameContext.
void destroyFrameContext(const Device& device, FrameContext& fc);

} // namespace forfun
