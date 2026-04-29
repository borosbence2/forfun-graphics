#include "material.h"
#include "types.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace mat {

// ---- Move constructor / assignment ----

Material::Material(Material&& o) noexcept
    : pkg_(std::move(o.pkg_))
    , ctx_(o.ctx_)
    , materialSetLayout_(std::exchange(o.materialSetLayout_, VK_NULL_HANDLE))
    , pipelineLayout_(std::exchange(o.pipelineLayout_, VK_NULL_HANDLE))
    , pipelineCache_(std::move(o.pipelineCache_))
{}

Material& Material::operator=(Material&& o) noexcept {
    if (this != &o) {
        destroy();
        pkg_               = std::move(o.pkg_);
        ctx_               = o.ctx_;
        materialSetLayout_ = std::exchange(o.materialSetLayout_, VK_NULL_HANDLE);
        pipelineLayout_    = std::exchange(o.pipelineLayout_,    VK_NULL_HANDLE);
        pipelineCache_     = std::move(o.pipelineCache_);
    }
    return *this;
}

// ---- Factory ----

Material Material::create(const MaterialPackage& pkg, const GpuContext& ctx) {
    Material m;
    m.pkg_ = pkg;
    m.ctx_ = ctx;

    // Build descriptor set layout for set 1.
    // Binding 0: uniform buffer (param UBO), if the material has parameters.
    // Binding N: combined image sampler for each sampler, using the binding
    //            stored in the SamplerDesc (assigned by PackageBuilder).
    std::vector<VkDescriptorSetLayoutBinding> bindings;

    if (pkg.paramUboSize > 0) {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }

    for (const auto& s : pkg.samplers) {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = s.binding;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo dslInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    dslInfo.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &dslInfo, nullptr, &m.materialSetLayout_));

    // Pipeline layout: set 0 = frame (camera/lighting), set 1 = material.
    // Push constant: 64-byte model matrix in vertex stage.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(glm::mat4);

    const VkDescriptorSetLayout setLayouts[2] = {ctx.frameSetLayout, m.materialSetLayout_};
    VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount         = 2;
    plInfo.pSetLayouts            = setLayouts;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &plInfo, nullptr, &m.pipelineLayout_));

    return m;
}

// ---- Pipeline lookup / creation ----

VkPipeline Material::pipelineFor(uint32_t variantKey) {
    auto it = pipelineCache_.find(variantKey);
    if (it != pipelineCache_.end()) return it->second;

    const VariantDesc* v = pkg_.findVariant(variantKey);
    if (!v) return VK_NULL_HANDLE;

    VkPipeline pipeline = buildPipeline(*v);
    pipelineCache_.emplace(variantKey, pipeline);
    return pipeline;
}

VkPipeline Material::buildPipeline(const VariantDesc& variant) const {
    const bool depthOnly    = (variant.variantKey & VAR_PASS_DEPTH_ONLY)    != 0;
    const bool transparent  = (variant.variantKey & VAR_PASS_TRANSPARENT)   != 0;

    // Helper to create modules safely. Returns VK_NULL_HANDLE if spirv is empty.
    auto makeModule = [&](const std::vector<uint32_t>& spirv) -> VkShaderModule {
        if (spirv.empty()) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size() * sizeof(uint32_t);
        info.pCode    = spirv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult res = vkCreateShaderModule(ctx_.device, &info, nullptr, &mod);
        if (res != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };

    VkShaderModule vertMod = makeModule(variant.vertSpirv);
    VkShaderModule fragMod = makeModule(variant.fragSpirv);

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    if (vertMod != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo s{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        s.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        s.module = vertMod;
        s.pName  = "main";
        stages.push_back(s);
    }
    if (!depthOnly && fragMod != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo s{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        s.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        s.module = fragMod;
        s.pName  = "main";
        stages.push_back(s);
    }

    // Vertex input.
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    uint32_t attrCount = 0;

    if (depthOnly) {
        binding.stride = sizeof(Vertex);
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrCount = 1;
    } else if (variant.variantKey & VAR_HAS_VERTEX_TANGENT) {
        binding.stride = sizeof(MaterialVertex);
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    static_cast<uint32_t>(offsetof(MaterialVertex, pos))};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    static_cast<uint32_t>(offsetof(MaterialVertex, normal))};
        attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(MaterialVertex, tangent))};
        attrs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,       static_cast<uint32_t>(offsetof(MaterialVertex, uv0))};
        attrCount = 4;
    } else {
        // UV is at shader location 3 (location 2 is reserved for tangent in
        // the tangent-bearing variant, so this layout stays binary-compatible).
        binding.stride = sizeof(Vertex);
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex, normal))};
        attrs[2] = {3, 0, VK_FORMAT_R32G32_SFLOAT,    static_cast<uint32_t>(offsetof(Vertex, uv))};
        attrCount = 3;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = attrCount;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = depthOnly ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;
    if (depthOnly) {
        rasterizer.depthBiasEnable = VK_TRUE;
    }

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = transparent ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp   = depthOnly ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (transparent) {
        blendAttach.blendEnable         = VK_TRUE;
        blendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttach.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttach.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    if (!depthOnly) {
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &blendAttach;
    }

    const VkDynamicState dynStates[3] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = depthOnly ? 3 : 2;
    dynamicState.pDynamicStates    = dynStates;

    VkPipelineRenderingCreateInfo renderingInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount    = depthOnly ? 0 : 1;
    renderingInfo.pColorAttachmentFormats = depthOnly ? nullptr : &ctx_.colorFormat;
    renderingInfo.depthAttachmentFormat   = ctx_.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages             = stages.data();
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisample;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = pipelineLayout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1,
                                       &pipelineInfo, nullptr, &pipeline));

    vkDestroyShaderModule(ctx_.device, vertMod, nullptr);
    vkDestroyShaderModule(ctx_.device, fragMod, nullptr);

    return pipeline;
}

// ---- Teardown ----

void Material::destroy() {
    for (auto& [key, pipeline] : pipelineCache_)
        vkDestroyPipeline(ctx_.device, pipeline, nullptr);
    pipelineCache_.clear();

    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx_.device, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (materialSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(ctx_.device, materialSetLayout_, nullptr);
        materialSetLayout_ = VK_NULL_HANDLE;
    }
}

} // namespace mat
