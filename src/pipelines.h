#pragma once

#include <vulkan/vulkan.h>

VkPipeline createSkyboxPipeline    (VkDevice device, VkPipelineLayout layout,
                                    VkFormat colorFormat, VkFormat depthFormat,
                                    VkShaderModule vert, VkShaderModule frag);

VkPipeline createFullscreenPipeline(VkDevice device, VkPipelineLayout layout,
                                    VkFormat colorFormat,
                                    VkShaderModule vert, VkShaderModule frag);
