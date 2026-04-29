#include "vk_helpers.h"

#include <cstring>

VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sizeBytes;
    ci.pCode    = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &mod));
    return mod;
}

Buffer createBufferGPU(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferCi.size        = size;
    bufferCi.usage       = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;

    Buffer b{};
    VK_CHECK(vmaCreateBuffer(allocator, &bufferCi, &allocCi,
                             &b.buffer, &b.allocation, nullptr));
    return b;
}

Buffer createBufferHostMapped(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferCi.size        = size;
    bufferCi.usage       = usage;
    bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;
    allocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    Buffer b{};
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(allocator, &bufferCi, &allocCi,
                             &b.buffer, &b.allocation, &info));
    b.mapped = info.pMappedData;
    return b;
}

void uploadToBuffer(VkDevice        device,
                    VkQueue         queue,
                    VkCommandPool   pool,
                    VmaAllocator    allocator,
                    const void*     src,
                    VkDeviceSize    size,
                    VkBuffer        dst) {
    VkBufferCreateInfo stagingCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingCi.size  = size;
    stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocCi{};
    stagingAllocCi.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer          stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation     stagingAlloc  = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    VK_CHECK(vmaCreateBuffer(allocator, &stagingCi, &stagingAllocCi,
                             &stagingBuffer, &stagingAlloc, &stagingInfo));

    std::memcpy(stagingInfo.pMappedData, src, static_cast<size_t>(size));

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, stagingBuffer, dst, 1, &copy);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbSubmit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbSubmit.commandBuffer = cmd;

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbSubmit;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vmaDestroyBuffer(allocator, stagingBuffer, stagingAlloc);
}

VkSampler createDefaultSampler(VkDevice device) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter     = VK_FILTER_LINEAR;
    ci.minFilter     = VK_FILTER_LINEAR;
    ci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.maxAnisotropy = 1.0f;
    ci.minLod        = 0.0f;
    ci.maxLod        = 0.0f;
    ci.borderColor   = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &ci, nullptr, &sampler));
    return sampler;
}

VkSampler createIblSampler(VkDevice device, float maxLod) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter     = VK_FILTER_LINEAR;
    ci.minFilter     = VK_FILTER_LINEAR;
    ci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxAnisotropy = 1.0f;
    ci.minLod        = 0.0f;
    ci.maxLod        = maxLod;

    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &ci, nullptr, &sampler));
    return sampler;
}

VkSampler createShadowSampler(VkDevice device) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter     = VK_FILTER_NEAREST;
    ci.minFilter     = VK_FILTER_NEAREST;
    ci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    ci.minLod        = 0.0f;
    ci.maxLod        = 0.0f;
    ci.maxAnisotropy = 1.0f;

    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &ci, nullptr, &sampler));
    return sampler;
}

DepthImage createDepthImage(VmaAllocator allocator, VkDevice device,
                            VkExtent2D extent, VkImageUsageFlags usage) {
    VkImageCreateInfo imageCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCi.imageType     = VK_IMAGE_TYPE_2D;
    imageCi.format        = kDepthFormat;
    imageCi.extent        = {extent.width, extent.height, 1};
    imageCi.mipLevels     = 1;
    imageCi.arrayLayers   = 1;
    imageCi.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCi.usage         = usage;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCi{};
    allocCi.usage    = VMA_MEMORY_USAGE_AUTO;
    allocCi.flags    = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    allocCi.priority = 1.0f;

    DepthImage d{};
    VK_CHECK(vmaCreateImage(allocator, &imageCi, &allocCi,
                            &d.image, &d.allocation, nullptr));

    VkImageViewCreateInfo viewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCi.image            = d.image;
    viewCi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format           = kDepthFormat;
    viewCi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &viewCi, nullptr, &d.view));

    return d;
}

RenderTarget createRenderTarget(VmaAllocator allocator, VkDevice device,
                                uint32_t width, uint32_t height, VkFormat format,
                                VkImageUsageFlags extraUsage) {
    RenderTarget target{};
    target.width  = width;
    target.height = height;
    target.format = format;

    VkImageCreateInfo imageCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCi.imageType     = VK_IMAGE_TYPE_2D;
    imageCi.format        = format;
    imageCi.extent        = {width, height, 1};
    imageCi.mipLevels     = 1;
    imageCi.arrayLayers   = 1;
    imageCi.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCi.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(allocator, &imageCi, &allocCi,
                            &target.image, &target.allocation, nullptr));

    VkImageViewCreateInfo viewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCi.image            = target.image;
    viewCi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format           = format;
    viewCi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &viewCi, nullptr, &target.view));

    return target;
}

Cubemap createCubemap(VmaAllocator allocator, VkDevice device,
                      uint32_t faceSize, uint32_t mipLevels) {
    VkImageCreateInfo imageCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCi.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageCi.imageType     = VK_IMAGE_TYPE_2D;
    imageCi.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageCi.extent        = {faceSize, faceSize, 1};
    imageCi.mipLevels     = mipLevels;
    imageCi.arrayLayers   = 6;
    imageCi.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCi.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                          | VK_IMAGE_USAGE_STORAGE_BIT;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;

    Cubemap c{};
    c.faceSize  = faceSize;
    c.mipLevels = mipLevels;
    VK_CHECK(vmaCreateImage(allocator, &imageCi, &allocCi,
                            &c.image, &c.allocation, nullptr));

    VkImageViewCreateInfo cubeViewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    cubeViewCi.image            = c.image;
    cubeViewCi.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeViewCi.format           = VK_FORMAT_R16G16B16A16_SFLOAT;
    cubeViewCi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 6};
    VK_CHECK(vkCreateImageView(device, &cubeViewCi, nullptr, &c.sampleView));

    c.storageMipViews.resize(mipLevels);
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        VkImageViewCreateInfo storageViewCi = cubeViewCi;
        storageViewCi.subresourceRange.baseMipLevel = mip;
        storageViewCi.subresourceRange.levelCount   = 1;
        storageViewCi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        VK_CHECK(vkCreateImageView(device, &storageViewCi, nullptr, &c.storageMipViews[mip]));
    }
    c.storageView = c.storageMipViews[0];

    return c;
}

void destroyCubemap(VkDevice device, VmaAllocator allocator, Cubemap& cubemap) {
    vkDestroyImageView(device, cubemap.sampleView, nullptr);
    for (VkImageView view : cubemap.storageMipViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    vmaDestroyImage(allocator, cubemap.image, cubemap.allocation);
    cubemap = {};
}

Texture createStorageTexture2D(VmaAllocator allocator, VkDevice device,
                               uint32_t width, uint32_t height, VkFormat format) {
    Texture tex{};
    tex.width  = width;
    tex.height = height;

    VkImageCreateInfo imageCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCi.imageType     = VK_IMAGE_TYPE_2D;
    imageCi.format        = format;
    imageCi.extent        = {width, height, 1};
    imageCi.mipLevels     = 1;
    imageCi.arrayLayers   = 1;
    imageCi.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCi.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(allocator, &imageCi, &allocCi,
                            &tex.image, &tex.allocation, nullptr));

    VkImageViewCreateInfo viewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCi.image            = tex.image;
    viewCi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format           = format;
    viewCi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &viewCi, nullptr, &tex.view));

    return tex;
}

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
                          uint32_t              layerCount) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask        = srcStage;
    barrier.srcAccessMask       = srcAccess;
    barrier.dstStageMask        = dstStage;
    barrier.dstAccessMask       = dstAccess;
    barrier.oldLayout           = oldLayout;
    barrier.newLayout           = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = {
        aspect, baseMipLevel, levelCount, baseArrayLayer, layerCount
    };

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void transitionImage(VkCommandBuffer       cmd,
                     VkImage               image,
                     VkImageAspectFlags    aspect,
                     VkImageLayout         oldLayout,
                     VkImageLayout         newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    transitionImageRange(cmd, image, aspect, oldLayout, newLayout,
                         srcStage, srcAccess, dstStage, dstAccess,
                         0, 1, 0, 1);
}

Texture createTextureRGBA8(VkDevice        device,
                           VkQueue         queue,
                           VkCommandPool   pool,
                           VmaAllocator    allocator,
                           const uint8_t*  pixels,
                           uint32_t        width,
                           uint32_t        height,
                           VkFormat        format) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    VkBufferCreateInfo stagingCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingCi.size  = size;
    stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocCi{};
    stagingAllocCi.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                         | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer          stagingBuf  = VK_NULL_HANDLE;
    VmaAllocation     stagingAlloc = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    VK_CHECK(vmaCreateBuffer(allocator, &stagingCi, &stagingAllocCi,
                             &stagingBuf, &stagingAlloc, &stagingInfo));
    std::memcpy(stagingInfo.pMappedData, pixels, static_cast<size_t>(size));

    Texture tex{};
    tex.width  = width;
    tex.height = height;

    VkImageCreateInfo imageCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCi.imageType     = VK_IMAGE_TYPE_2D;
    imageCi.format        = format;
    imageCi.extent        = {width, height, 1};
    imageCi.mipLevels     = 1;
    imageCi.arrayLayers   = 1;
    imageCi.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCi.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAllocCi{};
    imageAllocCi.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(allocator, &imageCi, &imageAllocCi,
                            &tex.image, &tex.allocation, nullptr));

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    transitionImage(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent                 = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionImage(cmd, tex.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbSubmit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbSubmit.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbSubmit;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);

    VkImageViewCreateInfo viewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCi.image            = tex.image;
    viewCi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format           = format;
    viewCi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &viewCi, nullptr, &tex.view));

    return tex;
}
