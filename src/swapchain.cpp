#include "swapchain.h"

#include <VkBootstrap.h>

#include <cstdio>
#include <cstdlib>

namespace forfun {

Swapchain createSwapchain(const Device& device, const SwapchainCreateInfo& info) {
    auto result = vkb::SwapchainBuilder{
            device.physicalDevice, device.device, device.surface, device.graphicsFamily}
        .set_desired_format({info.desiredFormat, info.desiredColorSpace})
        .set_desired_present_mode(info.desiredPresentMode)
        .set_desired_extent(info.desiredExtent.width, info.desiredExtent.height)
        .build();
    if (!result) {
        std::fprintf(stderr, "forfun::createSwapchain: %s\n",
                     result.error().message().c_str());
        std::abort();
    }
    vkb::Swapchain vkbSc = result.value();

    Swapchain sc;
    sc.swapchain   = vkbSc.swapchain;
    sc.extent      = vkbSc.extent;
    sc.imageFormat = vkbSc.image_format;
    sc.images      = vkbSc.get_images().value();
    sc.imageViews  = vkbSc.get_image_views().value();
    return sc;
}

void destroySwapchain(const Device& device, Swapchain& sc) {
    for (VkImageView view : sc.imageViews) {
        if (view) vkDestroyImageView(device.device, view, nullptr);
    }
    if (sc.swapchain) {
        vkDestroySwapchainKHR(device.device, sc.swapchain, nullptr);
    }
    sc = {};
}

} // namespace forfun
