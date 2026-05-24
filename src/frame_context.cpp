#include "frame_context.h"
#include "types.h"   // VK_CHECK

namespace forfun {

FrameContext createFrameContext(const Device& device) {
    FrameContext fc;

    VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = device.graphicsFamily;
    VK_CHECK(vkCreateCommandPool(device.device, &poolCi, nullptr, &fc.pool));

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = fc.pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device.device, &cbAi, &fc.cmd));

    VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(device.device, &semCi, nullptr, &fc.imageAvailable));
    VK_CHECK(vkCreateSemaphore(device.device, &semCi, nullptr, &fc.renderFinished));

    VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device.device, &fenceCi, nullptr, &fc.inFlight));

    return fc;
}

void destroyFrameContext(const Device& device, FrameContext& fc) {
    if (fc.inFlight)       vkDestroyFence      (device.device, fc.inFlight,       nullptr);
    if (fc.renderFinished) vkDestroySemaphore  (device.device, fc.renderFinished, nullptr);
    if (fc.imageAvailable) vkDestroySemaphore  (device.device, fc.imageAvailable, nullptr);
    if (fc.pool)           vkDestroyCommandPool(device.device, fc.pool,           nullptr);
    fc = {};
}

} // namespace forfun
