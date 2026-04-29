#pragma once

#include "material.h"
#include "vk_helpers.h"

#include <cstdint>

namespace mat {

// Owns a single live instance of a Material:
//   - A host-mapped UBO filled with the package's default parameter values.
//   - A VkDescriptorSet for material set (set 1), backed by a private pool.
//
// Created via MaterialInstance::create(). Must be destroyed explicitly via destroy().
// The parent Material must outlive all instances derived from it.
// Move-only; copying is deleted.
class MaterialInstance {
public:
    static MaterialInstance create(const Material& mat);

    // Parameter setters — silently ignore unknown names.
    // Writes directly into the host-mapped UBO; visible to the GPU on the next draw.
    void setParameter(const char* name, float v);
    void setParameter(const char* name, glm::vec2 v);
    void setParameter(const char* name, glm::vec3 v);
    void setParameter(const char* name, glm::vec4 v);
    void setParameter(const char* name, int32_t v);
    void setParameter(const char* name, bool v);

    // Writes a combined image/sampler descriptor to the instance's set.
    // Call after create() for every sampler slot before the first draw.
    void setSampler(const char* name, VkImageView view, VkSampler sampler);

    // Binds the instance's descriptor set to set 1 on `cmd`.
    // Call after vkCmdBindPipeline and the frame-set bind (set 0).
    void bind(VkCommandBuffer cmd) const;

    VkDescriptorSet  descriptorSet() const { return descriptorSet_; }
    const Material&  material()      const { return *mat_; }

    void destroy();

    MaterialInstance(MaterialInstance&&) noexcept;
    MaterialInstance& operator=(MaterialInstance&&) noexcept;
    MaterialInstance(const MaterialInstance&)       = delete;
    MaterialInstance& operator=(const MaterialInstance&) = delete;

private:
    MaterialInstance() = default;

    const Material*  mat_            = nullptr;
    GpuContext       ctx_{};
    Buffer           paramUbo_{};
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet  descriptorSet_  = VK_NULL_HANDLE;
};

} // namespace mat
