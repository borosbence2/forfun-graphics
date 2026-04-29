#pragma once

#include "vk_helpers.h"

Cubemap bakeEnvironmentCubemap   (VkDevice device, VkQueue queue, VkCommandPool pool,
                                  VmaAllocator allocator, const char* hdrPath,
                                  uint32_t cubeFaceSize = 512);

Cubemap bakePrefilteredEnvCubemap(VkDevice device, VkQueue queue, VkCommandPool pool,
                                  VmaAllocator allocator,
                                  const Cubemap& envCube, VkSampler envSampler,
                                  uint32_t faceSize = 512);

Texture bakeBrdfLut              (VkDevice device, VkQueue queue, VkCommandPool pool,
                                  VmaAllocator allocator,
                                  uint32_t lutSize = 512);

Buffer  bakeSHCoefficients       (VkDevice device, VkQueue queue, VkCommandPool pool,
                                  VmaAllocator allocator,
                                  const Cubemap& envCube, VkSampler envSampler);
