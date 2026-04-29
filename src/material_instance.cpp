#include "material_instance.h"
#include "types.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace mat {

// ---- Move constructor / assignment ----

MaterialInstance::MaterialInstance(MaterialInstance&& o) noexcept
    : mat_(o.mat_)
    , ctx_(o.ctx_)
    , paramUbo_(o.paramUbo_)
    , descriptorPool_(std::exchange(o.descriptorPool_, VK_NULL_HANDLE))
    , descriptorSet_(std::exchange(o.descriptorSet_, VK_NULL_HANDLE))
{
    o.paramUbo_ = {};
    o.mat_      = nullptr;
}

MaterialInstance& MaterialInstance::operator=(MaterialInstance&& o) noexcept {
    if (this != &o) {
        destroy();
        mat_            = o.mat_;
        ctx_            = o.ctx_;
        paramUbo_       = o.paramUbo_;
        descriptorPool_ = std::exchange(o.descriptorPool_, VK_NULL_HANDLE);
        descriptorSet_  = std::exchange(o.descriptorSet_,  VK_NULL_HANDLE);
        o.paramUbo_     = {};
        o.mat_          = nullptr;
    }
    return *this;
}

// ---- Factory ----

MaterialInstance MaterialInstance::create(const Material& mat) {
    MaterialInstance mi;
    mi.mat_ = &mat;
    mi.ctx_ = mat.context();

    const MaterialPackage& pkg = mat.package();
    const GpuContext&      ctx = mi.ctx_;

    // Allocate host-mapped param UBO and fill with per-parameter defaults.
    if (pkg.paramUboSize > 0) {
        mi.paramUbo_ = createBufferHostMapped(
            ctx.allocator, pkg.paramUboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        for (const auto& p : pkg.parameters) {
            uint8_t* dst = static_cast<uint8_t*>(mi.paramUbo_.mapped) + p.uboOffset;
            switch (p.type) {
                case ParameterType::Float:
                    std::memcpy(dst, p.defaults, sizeof(float));        break;
                case ParameterType::Float2:
                    std::memcpy(dst, p.defaults, 2 * sizeof(float));   break;
                case ParameterType::Float3:
                    std::memcpy(dst, p.defaults, 3 * sizeof(float));   break;
                case ParameterType::Float4:
                    std::memcpy(dst, p.defaults, 4 * sizeof(float));   break;
                case ParameterType::Int: {
                    int32_t v = static_cast<int32_t>(p.defaults[0]);
                    std::memcpy(dst, &v, sizeof(v));                   break;
                }
                case ParameterType::Bool: {
                    int32_t v = (p.defaults[0] != 0.0f) ? 1 : 0;
                    std::memcpy(dst, &v, sizeof(v));                   break;
                }
            }
        }
    }

    // Build descriptor pool sized exactly for this material's bindings.
    const uint32_t uboCount     = (pkg.paramUboSize > 0) ? 1u : 0u;
    const uint32_t samplerCount = static_cast<uint32_t>(pkg.samplers.size());

    if (uboCount + samplerCount > 0) {
        std::vector<VkDescriptorPoolSize> poolSizes;
        if (uboCount > 0)
            poolSizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         uboCount});
        if (samplerCount > 0)
            poolSizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount});

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes    = poolSizes.data();
        VK_CHECK(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &mi.descriptorPool_));

        VkDescriptorSetLayout layout = mat.materialSetLayout();
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool     = mi.descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &layout;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &allocInfo, &mi.descriptorSet_));

        // Immediately wire the UBO into the descriptor set at binding 0.
        if (uboCount > 0) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = mi.paramUbo_.buffer;
            bufInfo.offset = 0;
            bufInfo.range  = VK_WHOLE_SIZE;

            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet          = mi.descriptorSet_;
            w.dstBinding      = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.descriptorCount = 1;
            w.pBufferInfo     = &bufInfo;
            vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
        }
        // Sampler slots are left uninitialized; caller must invoke setSampler()
        // for every slot before the first draw.
    }

    return mi;
}

// ---- Parameter setters ----

void MaterialInstance::setParameter(const char* name, float v) {
    const ParameterDesc* d = mat_->package().findParameter(name);
    if (!d) return;
    std::memcpy(static_cast<uint8_t*>(paramUbo_.mapped) + d->uboOffset, &v, sizeof(v));
}

void MaterialInstance::setParameter(const char* name, glm::vec2 v) {
    const ParameterDesc* d = mat_->package().findParameter(name);
    if (!d) return;
    std::memcpy(static_cast<uint8_t*>(paramUbo_.mapped) + d->uboOffset, &v, sizeof(v));
}

void MaterialInstance::setParameter(const char* name, glm::vec3 v) {
    const ParameterDesc* d = mat_->package().findParameter(name);
    if (!d) return;
    std::memcpy(static_cast<uint8_t*>(paramUbo_.mapped) + d->uboOffset, &v, sizeof(v));
}

void MaterialInstance::setParameter(const char* name, glm::vec4 v) {
    const ParameterDesc* d = mat_->package().findParameter(name);
    if (!d) return;
    std::memcpy(static_cast<uint8_t*>(paramUbo_.mapped) + d->uboOffset, &v, sizeof(v));
}

void MaterialInstance::setParameter(const char* name, int32_t v) {
    const ParameterDesc* d = mat_->package().findParameter(name);
    if (!d) return;
    std::memcpy(static_cast<uint8_t*>(paramUbo_.mapped) + d->uboOffset, &v, sizeof(v));
}

void MaterialInstance::setParameter(const char* name, bool v) {
    const ParameterDesc* d = mat_->package().findParameter(name);
    if (!d) return;
    int32_t iv = v ? 1 : 0;
    std::memcpy(static_cast<uint8_t*>(paramUbo_.mapped) + d->uboOffset, &iv, sizeof(iv));
}

// ---- Sampler setter ----

void MaterialInstance::setSampler(const char* name, VkImageView view, VkSampler sampler) {
    const SamplerDesc* d = mat_->package().findSampler(name);
    if (!d || descriptorSet_ == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler;
    imgInfo.imageView   = view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = descriptorSet_;
    w.dstBinding      = d->binding;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
}

// ---- Bind ----

void MaterialInstance::bind(VkCommandBuffer cmd) const {
    if (descriptorSet_ == VK_NULL_HANDLE) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mat_->pipelineLayout(),
                            1, 1, &descriptorSet_,
                            0, nullptr);
}

// ---- Teardown ----

void MaterialInstance::destroy() {
    // Destroying the pool implicitly frees the allocated descriptor set.
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx_.device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_  = VK_NULL_HANDLE;
    }
    if (paramUbo_.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(ctx_.allocator, paramUbo_.buffer, paramUbo_.allocation);
        paramUbo_ = {};
    }
}

} // namespace mat
