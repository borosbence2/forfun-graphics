#pragma once

// Vulkan swapchain + its images + image views, bundled. Created via
// vk-bootstrap internally; consumers get raw Vulkan handles only.
//
// No resize support yet. The desiredExtent is treated as a hard request;
// destroy + recreate manually if the window changes size.

#include "device.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace forfun {

struct SwapchainCreateInfo {
    VkExtent2D       desiredExtent      = {0, 0};
    VkFormat         desiredFormat      = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR  desiredColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR desiredPresentMode = VK_PRESENT_MODE_FIFO_KHR;
};

struct Swapchain {
    VkSwapchainKHR           swapchain   = VK_NULL_HANDLE;
    VkExtent2D               extent      = {0, 0};
    VkFormat                 imageFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage>     images;       // not owned (owned by VkSwapchainKHR)
    std::vector<VkImageView> imageViews;   // owned (destroyed by destroySwapchain)
};

// Aborts on failure (matches the project's VK_CHECK style).
Swapchain createSwapchain(const Device& device, const SwapchainCreateInfo& info);

// Destroys the image views and the swapchain object. Does NOT touch the
// Device. After this call, `sc` is reset to a default-constructed Swapchain.
void destroySwapchain(const Device& device, Swapchain& sc);

} // namespace forfun
