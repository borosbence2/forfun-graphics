#include "ibl.h"
#include "types.h"

#include "equirect_to_cube.comp.h"
#include "prefilter_env.comp.h"
#include "brdf_lut.comp.h"
#include "sh_project.comp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" float* stbi_loadf(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels);
extern "C" void   stbi_image_free(void* retval_from_stbi_load);

Cubemap bakeEnvironmentCubemap(VkDevice       device,
                               VkQueue        queue,
                               VkCommandPool  pool,
                               VmaAllocator   allocator,
                               const char*    hdrPath,
                               uint32_t       cubeFaceSize) {
    int hdrW = 0, hdrH = 0, hdrC = 0;
    float* hdrPixels = stbi_loadf(hdrPath, &hdrW, &hdrH, &hdrC, 4);
    if (!hdrPixels) {
        std::fprintf(stderr, "Failed to load HDR %s\n", hdrPath);
        std::abort();
    }
    const VkDeviceSize hdrBytes =
        static_cast<VkDeviceSize>(hdrW) * hdrH * 4 * sizeof(float);

    VkImageCreateInfo eqCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    eqCi.imageType   = VK_IMAGE_TYPE_2D;
    eqCi.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
    eqCi.extent      = {static_cast<uint32_t>(hdrW), static_cast<uint32_t>(hdrH), 1};
    eqCi.mipLevels   = 1;
    eqCi.arrayLayers = 1;
    eqCi.samples     = VK_SAMPLE_COUNT_1_BIT;
    eqCi.tiling      = VK_IMAGE_TILING_OPTIMAL;
    eqCi.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo eqAllocCi{};
    eqAllocCi.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage       eqImage = VK_NULL_HANDLE;
    VmaAllocation eqAlloc = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateImage(allocator, &eqCi, &eqAllocCi,
                            &eqImage, &eqAlloc, nullptr));

    VkBufferCreateInfo stagingCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingCi.size  = hdrBytes;
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
    std::memcpy(stagingInfo.pMappedData, hdrPixels, static_cast<size_t>(hdrBytes));
    stbi_image_free(hdrPixels);

    VkImageViewCreateInfo eqViewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    eqViewCi.image            = eqImage;
    eqViewCi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    eqViewCi.format           = VK_FORMAT_R32G32B32A32_SFLOAT;
    eqViewCi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView eqView = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &eqViewCi, nullptr, &eqView));

    VkSamplerCreateInfo samplerCi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerCi.magFilter     = VK_FILTER_LINEAR;
    samplerCi.minFilter     = VK_FILTER_LINEAR;
    samplerCi.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCi.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCi.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCi.maxAnisotropy = 1.0f;
    VkSampler eqSampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &samplerCi, nullptr, &eqSampler));

    Cubemap cube = createCubemap(allocator, device, cubeFaceSize);

    VkDescriptorSetLayoutBinding cBindings[2]{};
    cBindings[0].binding         = 0;
    cBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cBindings[0].descriptorCount = 1;
    cBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    cBindings[1].binding         = 1;
    cBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cBindings[1].descriptorCount = 1;
    cBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo cLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cLayoutCi.bindingCount = 2;
    cLayoutCi.pBindings    = cBindings;
    VkDescriptorSetLayout cSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &cLayoutCi, nullptr, &cSetLayout));

    VkDescriptorPoolSize cPoolSizes[2]{};
    cPoolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cPoolSizes[0].descriptorCount = 1;
    cPoolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cPoolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo cPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cPoolCi.maxSets       = 1;
    cPoolCi.poolSizeCount = 2;
    cPoolCi.pPoolSizes    = cPoolSizes;
    VkDescriptorPool cPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &cPoolCi, nullptr, &cPool));

    VkDescriptorSetAllocateInfo cSetAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cSetAi.descriptorPool     = cPool;
    cSetAi.descriptorSetCount = 1;
    cSetAi.pSetLayouts        = &cSetLayout;
    VkDescriptorSet cSet = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &cSetAi, &cSet));

    VkShaderModule compModule = createShaderModule(device,
        equirect_to_cube_spv, sizeof(equirect_to_cube_spv));

    VkPipelineLayoutCreateInfo cPipeLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cPipeLayoutCi.setLayoutCount = 1;
    cPipeLayoutCi.pSetLayouts    = &cSetLayout;
    VkPipelineLayout cPipeLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &cPipeLayoutCi, nullptr, &cPipeLayout));

    VkComputePipelineCreateInfo cPipeCi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cPipeCi.layout       = cPipeLayout;
    cPipeCi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cPipeCi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cPipeCi.stage.module = compModule;
    cPipeCi.stage.pName  = "main";
    VkPipeline cPipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &cPipeCi, nullptr, &cPipeline));
    vkDestroyShaderModule(device, compModule, nullptr);

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    transitionImage(cmd, eqImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(hdrW), static_cast<uint32_t>(hdrH), 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, eqImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionImage(cmd, eqImage, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    transitionImageRange(cmd, cube.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         0, 1, 0, 6);

    VkDescriptorImageInfo eqInfo{};
    eqInfo.sampler     = eqSampler;
    eqInfo.imageView   = eqView;
    eqInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo cubeInfo{};
    cubeInfo.imageView   = cube.storageView;
    cubeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet cWrites[2]{};
    cWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cWrites[0].dstSet          = cSet;
    cWrites[0].dstBinding      = 0;
    cWrites[0].descriptorCount = 1;
    cWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cWrites[0].pImageInfo      = &eqInfo;
    cWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cWrites[1].dstSet          = cSet;
    cWrites[1].dstBinding      = 1;
    cWrites[1].descriptorCount = 1;
    cWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cWrites[1].pImageInfo      = &cubeInfo;
    vkUpdateDescriptorSets(device, 2, cWrites, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cPipeLayout, 0, 1, &cSet, 0, nullptr);

    const uint32_t groupsXY = (cubeFaceSize + 15u) / 16u;
    vkCmdDispatch(cmd, groupsXY, groupsXY, 6);

    transitionImageRange(cmd, cube.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         0, 1, 0, 6);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbInfo;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);

    vkDestroyPipeline(device, cPipeline, nullptr);
    vkDestroyPipelineLayout(device, cPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, cPool, nullptr);
    vkDestroyDescriptorSetLayout(device, cSetLayout, nullptr);
    vkDestroyImageView(device, eqView, nullptr);
    vkDestroySampler(device, eqSampler, nullptr);
    vmaDestroyImage(allocator, eqImage, eqAlloc);
    vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);

    std::printf("Baked environment cubemap %ux%u from %dx%d HDR equirect\n",
                cubeFaceSize, cubeFaceSize, hdrW, hdrH);
    return cube;
}

Cubemap bakePrefilteredEnvCubemap(VkDevice          device,
                                  VkQueue           queue,
                                  VkCommandPool     pool,
                                  VmaAllocator      allocator,
                                  const Cubemap&    envCube,
                                  VkSampler         envSampler,
                                  uint32_t          faceSize) {
    const uint32_t mipLevels = static_cast<uint32_t>(std::floor(std::log2(faceSize))) + 1u;
    Cubemap prefiltered = createCubemap(allocator, device, faceSize, mipLevels);

    VkDescriptorSetLayoutBinding cBindings[2]{};
    cBindings[0].binding         = 0;
    cBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cBindings[0].descriptorCount = 1;
    cBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    cBindings[1].binding         = 1;
    cBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cBindings[1].descriptorCount = 1;
    cBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo cLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cLayoutCi.bindingCount = 2;
    cLayoutCi.pBindings    = cBindings;
    VkDescriptorSetLayout cSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &cLayoutCi, nullptr, &cSetLayout));

    VkDescriptorPoolSize cPoolSizes[2]{};
    cPoolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cPoolSizes[0].descriptorCount = mipLevels;
    cPoolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cPoolSizes[1].descriptorCount = mipLevels;

    VkDescriptorPoolCreateInfo cPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cPoolCi.maxSets       = mipLevels;
    cPoolCi.poolSizeCount = 2;
    cPoolCi.pPoolSizes    = cPoolSizes;
    VkDescriptorPool cPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &cPoolCi, nullptr, &cPool));

    std::vector<VkDescriptorSetLayout> layouts(mipLevels, cSetLayout);
    VkDescriptorSetAllocateInfo cSetAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cSetAi.descriptorPool     = cPool;
    cSetAi.descriptorSetCount = mipLevels;
    cSetAi.pSetLayouts        = layouts.data();
    std::vector<VkDescriptorSet> cSets(mipLevels);
    VK_CHECK(vkAllocateDescriptorSets(device, &cSetAi, cSets.data()));

    VkShaderModule compModule = createShaderModule(device,
        prefilter_env_spv, sizeof(prefilter_env_spv));

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(PrefilterPushConstants);

    VkPipelineLayoutCreateInfo cPipeLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cPipeLayoutCi.setLayoutCount         = 1;
    cPipeLayoutCi.pSetLayouts            = &cSetLayout;
    cPipeLayoutCi.pushConstantRangeCount = 1;
    cPipeLayoutCi.pPushConstantRanges    = &pushRange;
    VkPipelineLayout cPipeLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &cPipeLayoutCi, nullptr, &cPipeLayout));

    VkComputePipelineCreateInfo cPipeCi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cPipeCi.layout       = cPipeLayout;
    cPipeCi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cPipeCi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cPipeCi.stage.module = compModule;
    cPipeCi.stage.pName  = "main";
    VkPipeline cPipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &cPipeCi, nullptr, &cPipeline));
    vkDestroyShaderModule(device, compModule, nullptr);

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    transitionImageRange(cmd, prefiltered.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         0, mipLevels, 0, 6);

    VkDescriptorImageInfo envInfo{};
    envInfo.sampler     = envSampler;
    envInfo.imageView   = envCube.sampleView;
    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = prefiltered.storageMipViews[mip];
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = cSets[mip];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &envInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = cSets[mip];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo      = &outInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cPipeline);

    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        const uint32_t mipSize = std::max(1u, faceSize >> mip);
        PrefilterPushConstants push{};
        push.roughness = (mipLevels > 1)
            ? static_cast<float>(mip) / static_cast<float>(mipLevels - 1)
            : 0.0f;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cPipeLayout, 0, 1, &cSets[mip], 0, nullptr);

        vkCmdPushConstants(cmd, cPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), &push);

        const uint32_t groupsXY = (mipSize + 7u) / 8u;
        vkCmdDispatch(cmd, groupsXY, groupsXY, 6);
    }

    transitionImageRange(cmd, prefiltered.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         0, mipLevels, 0, 6);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbInfo;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);

    vkDestroyPipeline(device, cPipeline, nullptr);
    vkDestroyPipelineLayout(device, cPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, cPool, nullptr);
    vkDestroyDescriptorSetLayout(device, cSetLayout, nullptr);

    std::printf("Baked prefiltered env cubemap %ux%u with %u mips\n",
                faceSize, faceSize, mipLevels);
    return prefiltered;
}

Texture bakeBrdfLut(VkDevice       device,
                    VkQueue        queue,
                    VkCommandPool  pool,
                    VmaAllocator   allocator,
                    uint32_t       size) {
    Texture lut = createStorageTexture2D(allocator, device, size, size,
                                         VK_FORMAT_R16G16_SFLOAT);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo cLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cLayoutCi.bindingCount = 1;
    cLayoutCi.pBindings    = &binding;
    VkDescriptorSetLayout cSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &cLayoutCi, nullptr, &cSetLayout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo cPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cPoolCi.maxSets       = 1;
    cPoolCi.poolSizeCount = 1;
    cPoolCi.pPoolSizes    = &poolSize;
    VkDescriptorPool cPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &cPoolCi, nullptr, &cPool));

    VkDescriptorSetAllocateInfo cSetAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cSetAi.descriptorPool     = cPool;
    cSetAi.descriptorSetCount = 1;
    cSetAi.pSetLayouts        = &cSetLayout;
    VkDescriptorSet cSet = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &cSetAi, &cSet));

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = lut.view;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = cSet;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo      = &outInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkShaderModule compModule = createShaderModule(device,
        brdf_lut_spv, sizeof(brdf_lut_spv));

    VkPipelineLayoutCreateInfo cPipeLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cPipeLayoutCi.setLayoutCount = 1;
    cPipeLayoutCi.pSetLayouts    = &cSetLayout;
    VkPipelineLayout cPipeLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &cPipeLayoutCi, nullptr, &cPipeLayout));

    VkComputePipelineCreateInfo cPipeCi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cPipeCi.layout       = cPipeLayout;
    cPipeCi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cPipeCi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cPipeCi.stage.module = compModule;
    cPipeCi.stage.pName  = "main";
    VkPipeline cPipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &cPipeCi, nullptr, &cPipeline));
    vkDestroyShaderModule(device, compModule, nullptr);

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    transitionImage(cmd, lut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cPipeLayout, 0, 1, &cSet, 0, nullptr);
    const uint32_t groupsXY = (size + 7u) / 8u;
    vkCmdDispatch(cmd, groupsXY, groupsXY, 1);

    transitionImage(cmd, lut.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbInfo;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);

    vkDestroyPipeline(device, cPipeline, nullptr);
    vkDestroyPipelineLayout(device, cPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, cPool, nullptr);
    vkDestroyDescriptorSetLayout(device, cSetLayout, nullptr);

    std::printf("Baked BRDF LUT %ux%u\n", size, size);
    return lut;
}

Buffer bakeSHCoefficients(VkDevice       device,
                           VkQueue        queue,
                           VkCommandPool  pool,
                           VmaAllocator   allocator,
                           const Cubemap& envCube,
                           VkSampler      envSampler) {
    const VkDeviceSize bufSize = 9 * sizeof(float) * 4;
    Buffer shBuf = createBufferGPU(allocator, bufSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    VkDescriptorSetLayoutBinding cBindings[2]{};
    cBindings[0].binding         = 0;
    cBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cBindings[0].descriptorCount = 1;
    cBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    cBindings[1].binding         = 1;
    cBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cBindings[1].descriptorCount = 1;
    cBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo cLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cLayoutCi.bindingCount = 2;
    cLayoutCi.pBindings    = cBindings;
    VkDescriptorSetLayout cSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &cLayoutCi, nullptr, &cSetLayout));

    VkDescriptorPoolSize cPoolSizes[2]{};
    cPoolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cPoolSizes[0].descriptorCount = 1;
    cPoolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cPoolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo cPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cPoolCi.maxSets       = 1;
    cPoolCi.poolSizeCount = 2;
    cPoolCi.pPoolSizes    = cPoolSizes;
    VkDescriptorPool cPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &cPoolCi, nullptr, &cPool));

    VkDescriptorSetAllocateInfo cSetAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cSetAi.descriptorPool     = cPool;
    cSetAi.descriptorSetCount = 1;
    cSetAi.pSetLayouts        = &cSetLayout;
    VkDescriptorSet cSet = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &cSetAi, &cSet));

    VkDescriptorImageInfo envInfo{};
    envInfo.sampler     = envSampler;
    envInfo.imageView   = envCube.sampleView;
    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = shBuf.buffer;
    bufInfo.range  = bufSize;

    VkWriteDescriptorSet cWrites[2]{};
    cWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cWrites[0].dstSet          = cSet;
    cWrites[0].dstBinding      = 0;
    cWrites[0].descriptorCount = 1;
    cWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cWrites[0].pImageInfo      = &envInfo;
    cWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cWrites[1].dstSet          = cSet;
    cWrites[1].dstBinding      = 1;
    cWrites[1].descriptorCount = 1;
    cWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cWrites[1].pBufferInfo     = &bufInfo;
    vkUpdateDescriptorSets(device, 2, cWrites, 0, nullptr);

    VkShaderModule compModule = createShaderModule(device,
        sh_project_spv, sizeof(sh_project_spv));

    VkPipelineLayoutCreateInfo cPipeLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cPipeLayoutCi.setLayoutCount = 1;
    cPipeLayoutCi.pSetLayouts    = &cSetLayout;
    VkPipelineLayout cPipeLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &cPipeLayoutCi, nullptr, &cPipeLayout));

    VkComputePipelineCreateInfo cPipeCi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cPipeCi.layout       = cPipeLayout;
    cPipeCi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cPipeCi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cPipeCi.stage.module = compModule;
    cPipeCi.stage.pName  = "main";
    VkPipeline cPipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                      &cPipeCi, nullptr, &cPipeline));
    vkDestroyShaderModule(device, compModule, nullptr);

    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = pool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cPipeLayout, 0, 1, &cSet, 0, nullptr);
    vkCmdDispatch(cmd, 1, 1, 1);

    VkBufferMemoryBarrier2 bufBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bufBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bufBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bufBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    bufBarrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    bufBarrier.buffer        = shBuf.buffer;
    bufBarrier.size          = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers    = &bufBarrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbInfo;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    vkDestroyPipeline(device, cPipeline, nullptr);
    vkDestroyPipelineLayout(device, cPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, cPool, nullptr);
    vkDestroyDescriptorSetLayout(device, cSetLayout, nullptr);

    std::printf("Baked SH irradiance (9 L0-L2 coefficients)\n");
    return shBuf;
}
