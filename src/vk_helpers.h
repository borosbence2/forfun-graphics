#pragma once

#include "types.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>

struct Buffer {
    VkBuffer      buffer     = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void*         mapped     = nullptr;
};

struct DepthImage {
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

struct Texture {
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint32_t      width      = 0;
    uint32_t      height     = 0;
    uint32_t      mipLevels  = 1;
};

struct RenderTarget {
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat      format     = VK_FORMAT_UNDEFINED;
    uint32_t      width      = 0;
    uint32_t      height     = 0;
};

struct Cubemap {
    VkImage                  image           = VK_NULL_HANDLE;
    VkImageView              sampleView      = VK_NULL_HANDLE;  // VK_IMAGE_VIEW_TYPE_CUBE
    VkImageView              storageView     = VK_NULL_HANDLE;  // base mip storage view
    std::vector<VkImageView> storageMipViews;
    VmaAllocation            allocation      = VK_NULL_HANDLE;
    uint32_t                 faceSize        = 0;
    uint32_t                 mipLevels       = 1;
};

VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, size_t sizeBytes);

Buffer createBufferGPU       (VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);
Buffer createBufferHostMapped(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);

void   uploadToBuffer(VkDevice device, VkQueue queue, VkCommandPool pool,
                      VmaAllocator allocator,
                      const void* src, VkDeviceSize size, VkBuffer dst);

Texture createTextureRGBA8(VkDevice device, VkQueue queue, VkCommandPool pool,
                           VmaAllocator allocator,
                           const uint8_t* pixels, uint32_t width, uint32_t height,
                           VkFormat format);

VkSampler createDefaultSampler(VkDevice device);
VkSampler createIblSampler    (VkDevice device, float maxLod);
VkSampler createShadowSampler (VkDevice device);

DepthImage   createDepthImage   (VmaAllocator allocator, VkDevice device,
                                 VkExtent2D extent, VkImageUsageFlags usage);
RenderTarget createRenderTarget (VmaAllocator allocator, VkDevice device,
                                 uint32_t width, uint32_t height, VkFormat format,
                                 VkImageUsageFlags extraUsage = 0);

Cubemap createCubemap (VmaAllocator allocator, VkDevice device,
                       uint32_t faceSize, uint32_t mipLevels = 1);
void    destroyCubemap(VkDevice device, VmaAllocator allocator, Cubemap& cubemap);

Texture createStorageTexture2D(VmaAllocator allocator, VkDevice device,
                               uint32_t width, uint32_t height, VkFormat format);

void transitionImage(VkCommandBuffer       cmd,
                     VkImage               image,
                     VkImageAspectFlags    aspect,
                     VkImageLayout         oldLayout,
                     VkImageLayout         newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

void transitionImageRange(VkCommandBuffer       cmd,
                          VkImage               image,
                          VkImageAspectFlags    aspect,
                          VkImageLayout         oldLayout,
                          VkImageLayout         newLayout,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                          uint32_t              baseMipLevel,
                          uint32_t              levelCount,
                          uint32_t              baseArrayLayer,
                          uint32_t              layerCount);
