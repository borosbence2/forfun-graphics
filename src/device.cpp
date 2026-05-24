#include "device.h"
#include "types.h"   // VK_CHECK

#include <VkBootstrap.h>

#include <cstdio>
#include <cstdlib>

namespace forfun {

Device createDevice(const DeviceCreateInfo& info) {
    if (!info.createSurface) {
        std::fprintf(stderr, "forfun::createDevice: createSurface callback is required\n");
        std::abort();
    }

    Device dev;

    // ---- Instance ----
    auto instanceResult = vkb::InstanceBuilder{}
        .set_app_name(info.appName)
        .require_api_version(1, 3, 0)
        .request_validation_layers(info.enableValidation)
        .use_default_debug_messenger()
        .build();
    if (!instanceResult) {
        std::fprintf(stderr, "forfun::createDevice: Instance: %s\n",
                     instanceResult.error().message().c_str());
        std::abort();
    }
    vkb::Instance vkbInstance = instanceResult.value();
    dev.instance       = vkbInstance.instance;
    dev.debugMessenger = vkbInstance.debug_messenger;

    // ---- Surface (caller-provided via callback) ----
    dev.surface = info.createSurface(dev.instance);
    if (dev.surface == VK_NULL_HANDLE) {
        std::fprintf(stderr, "forfun::createDevice: createSurface returned VK_NULL_HANDLE\n");
        std::abort();
    }

    // ---- Physical Device + Logical Device ----
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore               = VK_TRUE;
    f12.bufferDeviceAddress             = VK_TRUE;
    f12.descriptorIndexing              = VK_TRUE;
    f12.runtimeDescriptorArray          = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;

    auto physResult = vkb::PhysicalDeviceSelector{vkbInstance}
        .set_minimum_version(1, 3)
        .set_required_features_13(f13)
        .set_required_features_12(f12)
        .set_surface(dev.surface)
        .select();
    if (!physResult) {
        std::fprintf(stderr, "forfun::createDevice: PhysicalDevice: %s\n",
                     physResult.error().message().c_str());
        std::abort();
    }

    auto deviceResult = vkb::DeviceBuilder{physResult.value()}.build();
    if (!deviceResult) {
        std::fprintf(stderr, "forfun::createDevice: Device: %s\n",
                     deviceResult.error().message().c_str());
        std::abort();
    }
    vkb::Device vkbDevice = deviceResult.value();
    dev.physicalDevice = vkbDevice.physical_device;
    dev.device         = vkbDevice.device;
    dev.graphicsQueue  = vkbDevice.get_queue      (vkb::QueueType::graphics).value();
    dev.graphicsFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // ---- VMA allocator ----
    VmaAllocatorCreateInfo allocatorCi{};
    allocatorCi.physicalDevice   = dev.physicalDevice;
    allocatorCi.device           = dev.device;
    allocatorCi.instance         = dev.instance;
    allocatorCi.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorCi.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vmaCreateAllocator(&allocatorCi, &dev.allocator));

    return dev;
}

void destroyDevice(Device& dev) {
    if (dev.allocator) {
        vmaDestroyAllocator(dev.allocator);
    }
    if (dev.device) {
        vkDestroyDevice(dev.device, nullptr);
    }
    if (dev.surface && dev.instance) {
        vkDestroySurfaceKHR(dev.instance, dev.surface, nullptr);
    }
    if (dev.debugMessenger && dev.instance) {
        vkb::destroy_debug_utils_messenger(dev.instance, dev.debugMessenger);
    }
    if (dev.instance) {
        vkDestroyInstance(dev.instance, nullptr);
    }
    dev = {};
}

} // namespace forfun
