// forfun-graphics — entry point.
//
// Renderer split into modules:
//   types.h            — shared structs (Vertex, UBO, LightingUBO, etc.)
//                        + constants + VK_CHECK
//   vk_helpers.{h,cpp} — Buffer/Texture/Cubemap/RenderTarget wrappers
//                        + creation / upload / transition / sampler helpers
//   gltf_loader.{h,cpp} — glTF mesh + texture loader
//   ibl.{h,cpp}        — env cubemap + prefilter + BRDF LUT + SH bakes
//   pipelines.{h,cpp}  — graphics pipeline factories (skybox, fullscreen)
//   material.{h,cpp}   — Filament-style material system (variant pipelines)
//
// main.cpp owns GLFW + vk-bootstrap init, ImGui, the render loop, and teardown.

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include "types.h"
#include "vk_helpers.h"
#include "gltf_loader.h"
#include "ibl.h"
#include "pipelines.h"
#include "material_package.h"
#include "material.h"
#include "material_instance.h"
#include "device.h"

#include "post.vert.h"
#include "bloom_extract.frag.h"
#include "bloom_blur.frag.h"
#include "post_composite.frag.h"
#include "skybox.vert.h"
#include "skybox.frag.h"
#include "config.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

namespace Input {
    static bool   g_LeftMouseDown = false;
    static double g_LastMouseX    = 0.0;
    static double g_LastMouseY    = 0.0;
}

struct MaterialUiState {
    float  aoStrength       = 1.0f;
    float  normalStrength   = 1.0f;
    float  lightIntensity   = 3.0f;
    float  iblStrength      = 1.0f;
    float  lightYawDeg      = 31.0f;
    float  lightPitchDeg    = 59.0f;
    float  exposure         = 1.0f;
    float  bloomThreshold   = 1.0f;
    float  bloomStrength    = 0.18f;
    bool   shadowsEnabled   = true;
    bool   shadowsBaked     = false;
    bool   orbitCamera      = true;
    float  camYaw           = 0.0f;
    float  camPitch         = 0.3f;
    float  camDistance      = 6.0f;

    struct PointLightUi {
        float pos[3]    = {0.0f, 1.0f, 0.0f};
        float color[3]  = {1.0f, 0.9f, 0.8f};
        float range     = 3.0f;
        float intensity = 5.0f;
    };
    int          numPointLights = 0;
    PointLightUi pointLights[kMaxPointLights];
};

struct Frame {
    VkCommandPool   pool           = VK_NULL_HANDLE;
    VkCommandBuffer cmd            = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkSemaphore     renderFinished = VK_NULL_HANDLE;
    VkFence         inFlight       = VK_NULL_HANDLE;
    Buffer          ubo;
    Buffer          lightingUbo;
    Buffer          pointLightsUbo;
};

} // namespace

// Renders ImGui widgets for all parameters in a MaterialPackage and pushes
// changes into `inst` via setParameter. `buf` is a parallel value buffer
// initialized from package defaults; it is the single source of truth for
// current widget state, so external setParameter calls should also update it.
static void introspectMaterialImGui(
    const mat::MaterialPackage&          pkg,
    std::vector<std::array<float, 4>>&   buf,
    mat::MaterialInstance&               inst)
{
    for (size_t i = 0; i < pkg.parameters.size(); ++i) {
        const auto&  p    = pkg.parameters[i];
        auto&        v    = buf[i];
        const bool   isColor =
            p.name.find("color") != std::string::npos ||
            p.name.find("Color") != std::string::npos;
        bool changed = false;

        switch (p.type) {
        case mat::ParameterType::Float:
            changed = ImGui::DragFloat(p.name.c_str(), &v[0], 0.01f);
            break;
        case mat::ParameterType::Float2:
            changed = ImGui::DragFloat2(p.name.c_str(), v.data(), 0.01f);
            break;
        case mat::ParameterType::Float3:
            changed = isColor ? ImGui::ColorEdit3(p.name.c_str(), v.data())
                              : ImGui::DragFloat3(p.name.c_str(), v.data(), 0.01f);
            break;
        case mat::ParameterType::Float4:
            changed = isColor ? ImGui::ColorEdit4(p.name.c_str(), v.data())
                              : ImGui::DragFloat4(p.name.c_str(), v.data(), 0.01f);
            break;
        case mat::ParameterType::Int: {
            int iv = static_cast<int>(v[0]);
            if (ImGui::DragInt(p.name.c_str(), &iv)) { v[0] = static_cast<float>(iv); changed = true; }
            break;
        }
        case mat::ParameterType::Bool: {
            bool bv = v[0] > 0.5f;
            if (ImGui::Checkbox(p.name.c_str(), &bv)) { v[0] = bv ? 1.0f : 0.0f; changed = true; }
            break;
        }
        }

        if (!changed) continue;
        switch (p.type) {
        case mat::ParameterType::Float:  inst.setParameter(p.name.c_str(), v[0]); break;
        case mat::ParameterType::Float2: inst.setParameter(p.name.c_str(), glm::vec2(v[0], v[1])); break;
        case mat::ParameterType::Float3: inst.setParameter(p.name.c_str(), glm::vec3(v[0], v[1], v[2])); break;
        case mat::ParameterType::Float4: inst.setParameter(p.name.c_str(), glm::vec4(v[0], v[1], v[2], v[3])); break;
        case mat::ParameterType::Int:    inst.setParameter(p.name.c_str(), static_cast<int32_t>(v[0])); break;
        case mat::ParameterType::Bool:   inst.setParameter(p.name.c_str(), v[0] > 0.5f); break;
        }
    }
}

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(kWindowWidth, kWindowHeight, kAppName, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }
    MaterialUiState materialUi{};

    // ---- Device (instance + surface + physical + logical + queues + VMA) ----
    forfun::Device gpu = forfun::createDevice({
        .appName          = kAppName,
        .enableValidation = true,
        .createSurface    = [window](VkInstance inst) {
            VkSurfaceKHR s = VK_NULL_HANDLE;
            VK_CHECK(glfwCreateWindowSurface(inst, window, nullptr, &s));
            return s;
        },
    });
    // Aliases — keep the rest of main.cpp readable without touching every line.
    VkInstance       instance       = gpu.instance;
    VkSurfaceKHR     surface        = gpu.surface;
    VkPhysicalDevice physicalDevice = gpu.physicalDevice;
    VkDevice         device         = gpu.device;
    VkQueue          graphicsQueue  = gpu.graphicsQueue;
    uint32_t         graphicsFamily = gpu.graphicsFamily;
    VmaAllocator     allocator      = gpu.allocator;

    // ---- Swapchain ----
    auto swapResult = vkb::SwapchainBuilder{physicalDevice, device, surface, graphicsFamily}
        .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(kWindowWidth, kWindowHeight)
        .build();
    if (!swapResult) {
        std::fprintf(stderr, "Swapchain: %s\n", swapResult.error().message().c_str());
        return EXIT_FAILURE;
    }
    vkb::Swapchain           vkbSwapchain = swapResult.value();
    std::vector<VkImage>     scImages     = vkbSwapchain.get_images().value();
    std::vector<VkImageView> scViews      = vkbSwapchain.get_image_views().value();

    // ---- Depth + HDR post targets ----
    DepthImage depthImage = createDepthImage(
        allocator, device, vkbSwapchain.extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    DepthImage shadowMap = createDepthImage(
        allocator, device, {kShadowMapSize, kShadowMapSize},
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    RenderTarget hdrScene = createRenderTarget(
        allocator, device, vkbSwapchain.extent.width, vkbSwapchain.extent.height,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    RenderTarget sceneCapture = createRenderTarget(
        allocator, device, vkbSwapchain.extent.width, vkbSwapchain.extent.height,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    RenderTarget bloomA = createRenderTarget(
        allocator, device, vkbSwapchain.extent.width, vkbSwapchain.extent.height,
        VK_FORMAT_R16G16B16A16_SFLOAT);
    RenderTarget bloomB = createRenderTarget(
        allocator, device, vkbSwapchain.extent.width, vkbSwapchain.extent.height,
        VK_FORMAT_R16G16B16A16_SFLOAT);

    // ---- Transient pool for initial GPU uploads ----
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolCi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolCi.queueFamilyIndex = graphicsFamily;
        VK_CHECK(vkCreateCommandPool(device, &poolCi, nullptr, &uploadPool));
    }

    // Pre-transition shadowMap to SHADER_READ_ONLY_OPTIMAL so its descriptor's
    // claimed layout matches the actual layout even if shadows are disabled
    // for the very first frame. The shadow pass discards previous contents
    // (oldLayout = UNDEFINED) so this transition is safely overwritten when
    // shadows are on.
    {
        VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAi.commandPool        = uploadPool;
        cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAi.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &cmd));

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        transitionImage(cmd, shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        transitionImage(cmd, sceneCapture.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbInfo.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos    = &cbInfo;
        VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &si, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(graphicsQueue));
        vkFreeCommandBuffers(device, uploadPool, 1, &cmd);
    }

    // ---- Load helmet mesh from glTF ----
    MeshData helmetMesh;
    if (!loadGltfAsset(cfg::kHelmetModelPath, helmetMesh)) {
        std::fprintf(stderr, "Failed to load asset\n");
        return EXIT_FAILURE;
    }

    Buffer helmetVB = createBufferGPU(allocator,
        sizeof(Vertex) * helmetMesh.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    uploadToBuffer(device, graphicsQueue, uploadPool, allocator,
                   helmetMesh.vertices.data(),
                   sizeof(Vertex) * helmetMesh.vertices.size(), helmetVB.buffer);

    Buffer helmetIB = createBufferGPU(allocator,
        sizeof(uint32_t) * helmetMesh.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    uploadToBuffer(device, graphicsQueue, uploadPool, allocator,
                   helmetMesh.indices.data(),
                   sizeof(uint32_t) * helmetMesh.indices.size(), helmetIB.buffer);

    const uint32_t helmetIndexCount = static_cast<uint32_t>(helmetMesh.indices.size());

    // ---- PBR textures + samplers ----
    // Base color is sRGB (perceptual); MR + normal are linear data textures.
    Texture baseColor = createTextureRGBA8(device, graphicsQueue, uploadPool, allocator,
        helmetMesh.baseColor.rgba.data(), helmetMesh.baseColor.width, helmetMesh.baseColor.height,
        VK_FORMAT_R8G8B8A8_SRGB);
    Texture metalRough = createTextureRGBA8(device, graphicsQueue, uploadPool, allocator,
        helmetMesh.metallicRoughness.rgba.data(),
        helmetMesh.metallicRoughness.width, helmetMesh.metallicRoughness.height,
        VK_FORMAT_R8G8B8A8_UNORM);
    Texture normalMap = createTextureRGBA8(device, graphicsQueue, uploadPool, allocator,
        helmetMesh.normal.rgba.data(), helmetMesh.normal.width, helmetMesh.normal.height,
        VK_FORMAT_R8G8B8A8_UNORM);
    VkSampler defaultSampler = createDefaultSampler(device);
    VkSampler shadowSampler  = createShadowSampler(device);
    VkSampler envBakeSampler = createIblSampler(device, 0.0f);

    // ---- Environment cubemap + IBL precompute ----
    Cubemap envCube  = bakeEnvironmentCubemap(device, graphicsQueue, uploadPool,
                                              allocator, cfg::kEnvHdrPath);
    Cubemap prefilteredEnvCube = bakePrefilteredEnvCubemap(device, graphicsQueue, uploadPool,
                                                           allocator, envCube, envBakeSampler);
    Texture brdfLut = bakeBrdfLut(device, graphicsQueue, uploadPool, allocator);
    Buffer  shBuf   = bakeSHCoefficients(device, graphicsQueue, uploadPool,
                                          allocator, envCube, envBakeSampler);
    VkSampler iblSampler = createIblSampler(device,
        static_cast<float>(prefilteredEnvCube.mipLevels - 1));

    // ---- Skybox descriptor set layout + pool ----
    // b0 = camera UBO (vert), b1 = envCube (frag).
    VkDescriptorSetLayoutBinding skyboxBindings[2]{};
    skyboxBindings[0].binding         = 0;
    skyboxBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    skyboxBindings[0].descriptorCount = 1;
    skyboxBindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    skyboxBindings[1].binding         = 1;
    skyboxBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    skyboxBindings[1].descriptorCount = 1;
    skyboxBindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo skyboxSetLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    skyboxSetLayoutCi.bindingCount = 2;
    skyboxSetLayoutCi.pBindings    = skyboxBindings;
    VkDescriptorSetLayout skyboxSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &skyboxSetLayoutCi, nullptr, &skyboxSetLayout));

    VkDescriptorPoolSize skyboxPoolSizes[2]{};
    skyboxPoolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight};
    skyboxPoolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
    VkDescriptorPoolCreateInfo skyboxPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    skyboxPoolCi.maxSets       = kFramesInFlight;
    skyboxPoolCi.poolSizeCount = 2;
    skyboxPoolCi.pPoolSizes    = skyboxPoolSizes;
    VkDescriptorPool skyboxPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &skyboxPoolCi, nullptr, &skyboxPool));

    VkPipelineLayoutCreateInfo skyboxPipeLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    skyboxPipeLayoutCi.setLayoutCount = 1;
    skyboxPipeLayoutCi.pSetLayouts    = &skyboxSetLayout;
    VkPipelineLayout skyboxPipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &skyboxPipeLayoutCi, nullptr, &skyboxPipelineLayout));

    VkDescriptorPoolSize imguiPoolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
    };

    VkDescriptorPoolCreateInfo imguiDescPoolCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    imguiDescPoolCi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    imguiDescPoolCi.maxSets       = 1000 * static_cast<uint32_t>(std::size(imguiPoolSizes));
    imguiDescPoolCi.poolSizeCount = static_cast<uint32_t>(std::size(imguiPoolSizes));
    imguiDescPoolCi.pPoolSizes    = imguiPoolSizes;
    VkDescriptorPool imguiDescriptorPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &imguiDescPoolCi, nullptr, &imguiDescriptorPool));

    VkShaderModule skyVertModule = createShaderModule(device,
        skybox_vert_spv, sizeof(skybox_vert_spv));
    VkShaderModule skyFragModule = createShaderModule(device,
        skybox_frag_spv, sizeof(skybox_frag_spv));
    VkShaderModule postVertModule = createShaderModule(device,
        post_vert_spv, sizeof(post_vert_spv));
    VkShaderModule bloomExtractFragModule = createShaderModule(device,
        bloom_extract_frag_spv, sizeof(bloom_extract_frag_spv));
    VkShaderModule bloomBlurFragModule = createShaderModule(device,
        bloom_blur_frag_spv, sizeof(bloom_blur_frag_spv));
    VkShaderModule postCompositeFragModule = createShaderModule(device,
        post_composite_frag_spv, sizeof(post_composite_frag_spv));

    VkDescriptorSetLayoutBinding postOneBinding{};
    postOneBinding.binding         = 0;
    postOneBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postOneBinding.descriptorCount = 1;
    postOneBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo postOneSetLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    postOneSetLayoutCi.bindingCount = 1;
    postOneSetLayoutCi.pBindings    = &postOneBinding;
    VkDescriptorSetLayout postOneSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &postOneSetLayoutCi, nullptr, &postOneSetLayout));

    VkDescriptorSetLayoutBinding postTwoBindings[2]{};
    postTwoBindings[0] = postOneBinding;
    postTwoBindings[1] = postOneBinding;
    postTwoBindings[1].binding = 1;
    VkDescriptorSetLayoutCreateInfo postTwoSetLayoutCi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    postTwoSetLayoutCi.bindingCount = 2;
    postTwoSetLayoutCi.pBindings    = postTwoBindings;
    VkDescriptorSetLayout postTwoSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &postTwoSetLayoutCi, nullptr, &postTwoSetLayout));

    VkDescriptorPoolSize postPoolSize{};
    postPoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postPoolSize.descriptorCount = 8;
    VkDescriptorPoolCreateInfo postPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    postPoolCi.maxSets       = 8;
    postPoolCi.poolSizeCount = 1;
    postPoolCi.pPoolSizes    = &postPoolSize;
    VkDescriptorPool postDescriptorPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &postPoolCi, nullptr, &postDescriptorPool));

    VkDescriptorSet extractSet = VK_NULL_HANDLE;
    VkDescriptorSet blurSetA   = VK_NULL_HANDLE;
    VkDescriptorSet blurSetB   = VK_NULL_HANDLE;
    VkDescriptorSet compositeSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsOneAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsOneAi.descriptorPool     = postDescriptorPool;
    dsOneAi.descriptorSetCount = 1;
    dsOneAi.pSetLayouts        = &postOneSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(device, &dsOneAi, &extractSet));
    VK_CHECK(vkAllocateDescriptorSets(device, &dsOneAi, &blurSetA));
    VK_CHECK(vkAllocateDescriptorSets(device, &dsOneAi, &blurSetB));

    VkDescriptorSetAllocateInfo dsTwoAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsTwoAi.descriptorPool     = postDescriptorPool;
    dsTwoAi.descriptorSetCount = 1;
    dsTwoAi.pSetLayouts        = &postTwoSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(device, &dsTwoAi, &compositeSet));

    VkPipelineLayout extractLayout = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout = VK_NULL_HANDLE;
    VkPushConstantRange extractPushRange{};
    extractPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    extractPushRange.size       = sizeof(float);
    VkPipelineLayoutCreateInfo extractLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    extractLayoutCi.setLayoutCount         = 1;
    extractLayoutCi.pSetLayouts            = &postOneSetLayout;
    extractLayoutCi.pushConstantRangeCount = 1;
    extractLayoutCi.pPushConstantRanges    = &extractPushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &extractLayoutCi, nullptr, &extractLayout));

    VkPushConstantRange blurPushRange{};
    blurPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    blurPushRange.size       = sizeof(glm::vec2);
    VkPipelineLayoutCreateInfo blurLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    blurLayoutCi.setLayoutCount         = 1;
    blurLayoutCi.pSetLayouts            = &postOneSetLayout;
    blurLayoutCi.pushConstantRangeCount = 1;
    blurLayoutCi.pPushConstantRanges    = &blurPushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &blurLayoutCi, nullptr, &blurLayout));

    VkPushConstantRange compositePushRange{};
    compositePushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compositePushRange.size       = sizeof(CompositePushData);
    VkPipelineLayoutCreateInfo compositeLayoutCi{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    compositeLayoutCi.setLayoutCount         = 1;
    compositeLayoutCi.pSetLayouts            = &postTwoSetLayout;
    compositeLayoutCi.pushConstantRangeCount = 1;
    compositeLayoutCi.pPushConstantRanges    = &compositePushRange;
    VK_CHECK(vkCreatePipelineLayout(device, &compositeLayoutCi, nullptr, &compositeLayout));

    VkPipeline bloomExtractPipeline = createFullscreenPipeline(
        device, extractLayout, bloomA.format, postVertModule, bloomExtractFragModule);
    VkPipeline bloomBlurPipeline = createFullscreenPipeline(
        device, blurLayout, bloomB.format, postVertModule, bloomBlurFragModule);
    VkPipeline compositePipeline = createFullscreenPipeline(
        device, compositeLayout, vkbSwapchain.image_format, postVertModule, postCompositeFragModule);

    VkPipeline skyboxPipeline = createSkyboxPipeline(
        device, skyboxPipelineLayout, hdrScene.format, kDepthFormat,
        skyVertModule, skyFragModule);

    vkDestroyShaderModule(device, skyVertModule, nullptr);
    vkDestroyShaderModule(device, skyFragModule, nullptr);
    vkDestroyShaderModule(device, postVertModule, nullptr);
    vkDestroyShaderModule(device, bloomExtractFragModule, nullptr);
    vkDestroyShaderModule(device, bloomBlurFragModule, nullptr);
    vkDestroyShaderModule(device, postCompositeFragModule, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    glfwSetWindowUserPointer(window, &materialUi);

    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
        ImGuiIO& imguiIo = ImGui::GetIO();
        if (imguiIo.WantCaptureMouse) return;
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            Input::g_LeftMouseDown = (action == GLFW_PRESS);
            glfwGetCursorPos(w, &Input::g_LastMouseX, &Input::g_LastMouseY);
        }
    });

    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double xpos, double ypos) {
        if (!Input::g_LeftMouseDown) return;
        auto* state = static_cast<MaterialUiState*>(glfwGetWindowUserPointer(w));
        
        double dx = xpos - Input::g_LastMouseX;
        double dy = ypos - Input::g_LastMouseY;
        Input::g_LastMouseX = xpos;
        Input::g_LastMouseY = ypos;

        state->camYaw   -= static_cast<float>(dx) * 0.005f;
        state->camPitch += static_cast<float>(dy) * 0.005f;

        // Clamp pitch to avoid gimbal lock/flipping
        constexpr float kLimit = glm::radians(89.0f);
        state->camPitch = glm::clamp(state->camPitch, -kLimit, kLimit);
    });

    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
        if (ImGui::GetIO().WantCaptureMouse) return;
        auto* state = static_cast<MaterialUiState*>(glfwGetWindowUserPointer(w));
        state->camDistance -= static_cast<float>(yoffset) * 0.25f;
        state->camDistance = glm::max(0.5f, state->camDistance);
    });

    ImGui_ImplGlfw_InitForVulkan(window, true);

    VkPipelineRenderingCreateInfoKHR imguiRenderingInfo{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    imguiRenderingInfo.colorAttachmentCount    = 1;
    imguiRenderingInfo.pColorAttachmentFormats = &vkbSwapchain.image_format;

    ImGui_ImplVulkan_InitInfo imguiInitInfo{};
    imguiInitInfo.Instance = instance;
    imguiInitInfo.PhysicalDevice = physicalDevice;
    imguiInitInfo.Device = device;
    imguiInitInfo.QueueFamily = graphicsFamily;
    imguiInitInfo.Queue = graphicsQueue;
    imguiInitInfo.DescriptorPool = imguiDescriptorPool;
    imguiInitInfo.MinImageCount = static_cast<uint32_t>(scImages.size());
    imguiInitInfo.ImageCount = static_cast<uint32_t>(scImages.size());
    imguiInitInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imguiInitInfo.UseDynamicRendering = true;
    imguiInitInfo.PipelineRenderingCreateInfo = imguiRenderingInfo;
    ImGui_ImplVulkan_Init(&imguiInitInfo);
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        std::fprintf(stderr, "ImGui font upload failed\n");
        return EXIT_FAILURE;
    }

    VkDescriptorImageInfo hdrSceneInfo{};
    hdrSceneInfo.sampler     = iblSampler;
    hdrSceneInfo.imageView   = hdrScene.view;
    hdrSceneInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo bloomAInfo{};
    bloomAInfo.sampler     = iblSampler;
    bloomAInfo.imageView   = bloomA.view;
    bloomAInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo bloomBInfo{};
    bloomBInfo.sampler     = iblSampler;
    bloomBInfo.imageView   = bloomB.view;
    bloomBInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet postWrites[5]{};
    postWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    postWrites[0].dstSet          = extractSet;
    postWrites[0].dstBinding      = 0;
    postWrites[0].descriptorCount = 1;
    postWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postWrites[0].pImageInfo      = &hdrSceneInfo;

    postWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    postWrites[1].dstSet          = blurSetA;
    postWrites[1].dstBinding      = 0;
    postWrites[1].descriptorCount = 1;
    postWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postWrites[1].pImageInfo      = &bloomAInfo;

    postWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    postWrites[2].dstSet          = blurSetB;
    postWrites[2].dstBinding      = 0;
    postWrites[2].descriptorCount = 1;
    postWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postWrites[2].pImageInfo      = &bloomBInfo;

    postWrites[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    postWrites[3].dstSet          = compositeSet;
    postWrites[3].dstBinding      = 0;
    postWrites[3].descriptorCount = 1;
    postWrites[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postWrites[3].pImageInfo      = &hdrSceneInfo;

    postWrites[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    postWrites[4].dstSet          = compositeSet;
    postWrites[4].dstBinding      = 1;
    postWrites[4].descriptorCount = 1;
    postWrites[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    postWrites[4].pImageInfo      = &bloomAInfo;
    vkUpdateDescriptorSets(device, 5, postWrites, 0, nullptr);

    // ---- Per-frame data ----
    std::array<Frame, kFramesInFlight> frames{};
    for (auto& f : frames) {
        VkCommandPoolCreateInfo framePoolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        framePoolCi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        framePoolCi.queueFamilyIndex = graphicsFamily;
        VK_CHECK(vkCreateCommandPool(device, &framePoolCi, nullptr, &f.pool));

        VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAi.commandPool        = f.pool;
        cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAi.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cbAi, &f.cmd));

        VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &semCi, nullptr, &f.imageAvailable));
        VK_CHECK(vkCreateSemaphore(device, &semCi, nullptr, &f.renderFinished));

        VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fenceCi, nullptr, &f.inFlight));

        f.ubo = createBufferHostMapped(allocator, sizeof(UBO),
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        f.pointLightsUbo = createBufferHostMapped(allocator, sizeof(PointLightsUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        f.lightingUbo = createBufferHostMapped(allocator, sizeof(LightingUBO),
                                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    }

    // ---- Skybox descriptor sets (one per frame, sharing camera UBO) ----
    std::array<VkDescriptorSet, kFramesInFlight> skyboxSets{};
    for (size_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = skyboxPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &skyboxSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(device, &ai, &skyboxSets[i]));

        VkDescriptorBufferInfo uboBI{};
        uboBI.buffer = frames[i].ubo.buffer;
        uboBI.range  = sizeof(UBO);

        VkDescriptorImageInfo envII{};
        envII.sampler     = iblSampler;
        envII.imageView   = envCube.sampleView;
        envII.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet ws[2]{};
        ws[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[0].dstSet          = skyboxSets[i];
        ws[0].dstBinding      = 0;
        ws[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ws[0].descriptorCount = 1;
        ws[0].pBufferInfo     = &uboBI;
        ws[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[1].dstSet          = skyboxSets[i];
        ws[1].dstBinding      = 1;
        ws[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[1].descriptorCount = 1;
        ws[1].pImageInfo      = &envII;
        vkUpdateDescriptorSets(device, 2, ws, 0, nullptr);
    }

    // ---- Material system (M8.materials.7) ----
    // Frame set layout for material pipelines.
    // b0 = camera UBO (vert+frag), b1 = SH UBO (frag),
    // b2-b4 = prefiltered + brdfLut + shadowMap (frag), b5 = LightingUBO (frag),
    // b6 = sceneCapture (frag), b7 = PointLightsUBO (frag).
    VkDescriptorSetLayoutBinding matFrameBindings[8]{};
    // b0=camera UBO, b1=SH UBO, b2-b4=prefiltered+brdfLut+shadowMap, b5=LightingUBO, b6=sceneCapture, b7=PointLightsUBO
    matFrameBindings[0].binding         = 0;
    matFrameBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    matFrameBindings[0].descriptorCount = 1;
    matFrameBindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    matFrameBindings[1].binding         = 1;
    matFrameBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    matFrameBindings[1].descriptorCount = 1;
    matFrameBindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t b = 2; b <= 4; ++b) {
        matFrameBindings[b].binding         = b;
        matFrameBindings[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        matFrameBindings[b].descriptorCount = 1;
        matFrameBindings[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    matFrameBindings[5].binding         = 5;
    matFrameBindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    matFrameBindings[5].descriptorCount = 1;
    matFrameBindings[5].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    matFrameBindings[6].binding         = 6;
    matFrameBindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    matFrameBindings[6].descriptorCount = 1;
    matFrameBindings[6].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    matFrameBindings[7].binding         = 7;
    matFrameBindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    matFrameBindings[7].descriptorCount = 1;
    matFrameBindings[7].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo matFrameSLCI{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    matFrameSLCI.bindingCount = 8;
    matFrameSLCI.pBindings    = matFrameBindings;
    VkDescriptorSetLayout matFrameSetLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &matFrameSLCI, nullptr, &matFrameSetLayout));

    VkDescriptorPoolSize matFramePoolSizes[2]{};
    matFramePoolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight * 4};
    matFramePoolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 4};
    VkDescriptorPoolCreateInfo matFramePoolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    matFramePoolCI.maxSets       = kFramesInFlight;
    matFramePoolCI.poolSizeCount = 2;
    matFramePoolCI.pPoolSizes    = matFramePoolSizes;
    VkDescriptorPool matFramePool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &matFramePoolCI, nullptr, &matFramePool));

    std::array<VkDescriptorSet, kFramesInFlight> matFrameSets{};
    for (size_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = matFramePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &matFrameSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(device, &ai, &matFrameSets[i]));

        VkDescriptorBufferInfo cameraBI{};
        cameraBI.buffer = frames[i].ubo.buffer;
        cameraBI.range  = sizeof(UBO);

        VkDescriptorBufferInfo shBI{};
        shBI.buffer = shBuf.buffer;
        shBI.range  = 9 * sizeof(float) * 4;

        VkDescriptorBufferInfo lightingBI{};
        lightingBI.buffer = frames[i].lightingUbo.buffer;
        lightingBI.range  = sizeof(LightingUBO);

        VkDescriptorBufferInfo pointLightsBI{};
        pointLightsBI.buffer = frames[i].pointLightsUbo.buffer;
        pointLightsBI.range  = sizeof(PointLightsUBO);

        const VkImageView iblViews[3] = {
            prefilteredEnvCube.sampleView,
            brdfLut.view,
            shadowMap.view,
        };
        const VkSampler iblSamplers[3] = {iblSampler, iblSampler, shadowSampler};
        VkDescriptorImageInfo iblImageInfos[3]{};
        for (int k = 0; k < 3; ++k) {
            iblImageInfos[k].sampler     = iblSamplers[k];
            iblImageInfos[k].imageView   = iblViews[k];
            iblImageInfos[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkDescriptorImageInfo sceneCaptureII{};
        sceneCaptureII.sampler     = iblSampler;
        sceneCaptureII.imageView   = sceneCapture.view;
        sceneCaptureII.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet ws[8]{};
        ws[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[0].dstSet          = matFrameSets[i];
        ws[0].dstBinding      = 0;
        ws[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ws[0].descriptorCount = 1;
        ws[0].pBufferInfo     = &cameraBI;
        ws[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[1].dstSet          = matFrameSets[i];
        ws[1].dstBinding      = 1;
        ws[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ws[1].descriptorCount = 1;
        ws[1].pBufferInfo     = &shBI;
        for (int k = 0; k < 3; ++k) {
            ws[2 + k].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[2 + k].dstSet          = matFrameSets[i];
            ws[2 + k].dstBinding      = static_cast<uint32_t>(2 + k);
            ws[2 + k].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            ws[2 + k].descriptorCount = 1;
            ws[2 + k].pImageInfo      = &iblImageInfos[k];
        }
        ws[5].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[5].dstSet          = matFrameSets[i];
        ws[5].dstBinding      = 5;
        ws[5].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ws[5].descriptorCount = 1;
        ws[5].pBufferInfo     = &lightingBI;
        ws[6].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[6].dstSet          = matFrameSets[i];
        ws[6].dstBinding      = 6;
        ws[6].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[6].descriptorCount = 1;
        ws[6].pImageInfo      = &sceneCaptureII;
        ws[7].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[7].dstSet          = matFrameSets[i];
        ws[7].dstBinding      = 7;
        ws[7].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ws[7].descriptorCount = 1;
        ws[7].pBufferInfo     = &pointLightsBI;
        vkUpdateDescriptorSets(device, 8, ws, 0, nullptr);
    }

    // ---- Material system — load all packages ----
    const mat::GpuContext matCtx{device, allocator, hdrScene.format, kDepthFormat,
                                 matFrameSetLayout};

    auto loadPkgOrDie = [](const char* path, const char* matPath) -> mat::MaterialPackage {
        mat::MaterialPackage pkg;
        if (!mat::MaterialPackage::loadFromFile(path, pkg)) {
            std::fprintf(stderr,
                "Error: %s not found.\n  Run: forfun_matc %s -o %s\n",
                path, matPath, path);
            std::exit(EXIT_FAILURE);
        }
        return pkg;
    };

    mat::MaterialPackage helmetPkg = loadPkgOrDie(cfg::kHelmetFpkgPath, cfg::kHelmetMatPath);

    mat::Material         helmetMat  = mat::Material::create(helmetPkg, matCtx);
    mat::MaterialInstance helmetInst = mat::MaterialInstance::create(helmetMat);

    helmetInst.setSampler("baseColorMap",         baseColor.view,  defaultSampler);
    helmetInst.setSampler("metallicRoughnessMap", metalRough.view, defaultSampler);
    helmetInst.setSampler("normalMap",            normalMap.view,  defaultSampler);

    // Pre-warm pipeline caches.
    auto prewarm = [](mat::Material& mat, const mat::MaterialPackage& pkg, const char* tag) {
        const uint32_t colorKey = mat::computeVariantKey(pkg, false);
        const uint32_t depthKey = mat::computeVariantKey(pkg, true);
        mat.pipelineFor(colorKey);
        mat.pipelineFor(depthKey);
        std::printf("[mat] %s: colorKey=0x%04x  depthKey=0x%04x\n", tag, colorKey, depthKey);
    };
    prewarm(helmetMat, helmetPkg, "helmet");

    // Per-parameter ImGui buffer (initialised from package defaults).
    std::vector<std::array<float, 4>> helmetParamBuf(helmetPkg.parameters.size());
    for (size_t i = 0; i < helmetPkg.parameters.size(); ++i) {
        const auto& p = helmetPkg.parameters[i];
        helmetParamBuf[i] = {p.defaults[0], p.defaults[1], p.defaults[2], p.defaults[3]};
    }

    // ---- Hot reload state (helmet only) ----
    namespace fs = std::filesystem;
    auto matSrcTime = fs::exists(cfg::kHelmetMatPath)
        ? fs::last_write_time(cfg::kHelmetMatPath)
        : fs::file_time_type{};

    auto reloadHelmetMaterial = [&]() {
        char cmd[512];
        std::snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" -o \"%s\"",
                      MATC_EXE_PATH, cfg::kHelmetMatPath, cfg::kHelmetFpkgPath);
        std::printf("[hot-reload] Compiling: %s\n", cmd);
        int rc = std::system(cmd);
        if (rc != 0) {
            std::fprintf(stderr, "[hot-reload] Compile failed (exit %d)\n", rc);
            return;
        }
        mat::MaterialPackage newPkg;
        if (!mat::MaterialPackage::loadFromFile(cfg::kHelmetFpkgPath, newPkg)) {
            std::fprintf(stderr, "[hot-reload] Failed to load new package\n");
            return;
        }
        vkDeviceWaitIdle(device);

        std::vector<std::pair<std::string, std::array<float, 4>>> saved;
        for (size_t i = 0; i < helmetPkg.parameters.size(); ++i)
            saved.emplace_back(helmetPkg.parameters[i].name, helmetParamBuf[i]);

        helmetInst.destroy();
        helmetMat.destroy();
        helmetPkg = std::move(newPkg);
        helmetMat  = mat::Material::create(helmetPkg, matCtx);
        helmetInst = mat::MaterialInstance::create(helmetMat);
        helmetInst.setSampler("baseColorMap",         baseColor.view,  defaultSampler);
        helmetInst.setSampler("metallicRoughnessMap", metalRough.view, defaultSampler);
        helmetInst.setSampler("normalMap",            normalMap.view,  defaultSampler);

        helmetParamBuf.assign(helmetPkg.parameters.size(), {});
        for (size_t i = 0; i < helmetPkg.parameters.size(); ++i) {
            const auto& p = helmetPkg.parameters[i];
            helmetParamBuf[i] = {p.defaults[0], p.defaults[1], p.defaults[2], p.defaults[3]};
        }
        for (const auto& [name, vals] : saved) {
            for (size_t i = 0; i < helmetPkg.parameters.size(); ++i) {
                if (helmetPkg.parameters[i].name != name) continue;
                helmetParamBuf[i] = vals;
                const auto& p = helmetPkg.parameters[i];
                switch (p.type) {
                case mat::ParameterType::Float:  helmetInst.setParameter(p.name.c_str(), vals[0]); break;
                case mat::ParameterType::Float2: helmetInst.setParameter(p.name.c_str(), glm::vec2(vals[0], vals[1])); break;
                case mat::ParameterType::Float3: helmetInst.setParameter(p.name.c_str(), glm::vec3(vals[0], vals[1], vals[2])); break;
                case mat::ParameterType::Float4: helmetInst.setParameter(p.name.c_str(), glm::vec4(vals[0], vals[1], vals[2], vals[3])); break;
                case mat::ParameterType::Int:    helmetInst.setParameter(p.name.c_str(), static_cast<int32_t>(vals[0])); break;
                case mat::ParameterType::Bool:   helmetInst.setParameter(p.name.c_str(), vals[0] > 0.5f); break;
                }
                break;
            }
        }
        prewarm(helmetMat, helmetPkg, "helmet");
        std::printf("[hot-reload] Material reloaded\n");
    };

    // ---- Render loop ----
    uint32_t frameIndex = 0;
    const double t0 = glfwGetTime();
    const float aspect = static_cast<float>(kWindowWidth) / static_cast<float>(kWindowHeight);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Check if .mat source file changed and trigger a hot reload.
        if (fs::exists(cfg::kHelmetMatPath)) {
            auto t = fs::last_write_time(cfg::kHelmetMatPath);
            if (t != matSrcTime) {
                matSrcTime = t;
                reloadHelmetMaterial();
            }
        }

        Frame& frame = frames[frameIndex];

        VK_CHECK(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &frame.inFlight));

        float activeYaw = materialUi.camYaw;
        if (materialUi.orbitCamera) {
            activeYaw += static_cast<float>(glfwGetTime() - t0) * 0.6f;
        }

        const glm::vec3 camPos(
            materialUi.camDistance * std::cos(materialUi.camPitch) * std::sin(activeYaw),
            materialUi.camDistance * std::sin(materialUi.camPitch),
            materialUi.camDistance * std::cos(materialUi.camPitch) * std::cos(activeYaw)
        );

        glm::mat4 view = glm::lookAt(camPos, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 proj = glm::perspective(glm::radians(50.0f), aspect, 0.1f, 100.0f);
        proj[1][1] *= -1.0f;  // flip Y for Vulkan clip space

        const float lightYaw   = glm::radians(materialUi.lightYawDeg);
        const float lightPitch = glm::radians(materialUi.lightPitchDeg);
        const glm::vec3 lightDir = glm::normalize(glm::vec3(
            std::cos(lightPitch) * std::cos(lightYaw),
            std::sin(lightPitch),
            std::cos(lightPitch) * std::sin(lightYaw)));
        const glm::vec3 lightPos = lightDir * 4.0f;
        glm::mat4 lightView = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0, 1, 0));
        glm::mat4 lightProj = glm::ortho(-3.5f, 3.5f, -3.5f, 3.5f, 0.1f, 10.0f);

        const glm::mat4 viewProj = proj * view;
        const glm::mat4 lightViewProj = lightProj * lightView;

        UBO ubo{};
        ubo.viewProj      = viewProj;
        ubo.invViewProj   = glm::inverse(viewProj);
        ubo.lightViewProj = lightViewProj;
        ubo.cameraPos     = glm::vec4(camPos, 1.0f);
        std::memcpy(frame.ubo.mapped, &ubo, sizeof(UBO));

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scene Controls")) {
            ImGui::Text("FPS: %.1f (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
            ImGui::Separator();
            ImGui::TextUnformatted("Materials");
            if (ImGui::CollapsingHeader("Damaged Helmet")) {
                introspectMaterialImGui(helmetMat.package(), helmetParamBuf, helmetInst);
            }
            ImGui::SliderFloat("AO scale",     &materialUi.aoStrength,     0.0f, 2.0f);
            ImGui::SliderFloat("Normal scale", &materialUi.normalStrength, 0.0f, 2.0f);
            ImGui::Separator();
            ImGui::TextUnformatted("Lighting");
            ImGui::SliderFloat("Direct Light", &materialUi.lightIntensity, 0.0f, 8.0f);
            ImGui::SliderFloat("IBL", &materialUi.iblStrength, 0.0f, 4.0f);
            ImGui::SliderFloat("Light Yaw", &materialUi.lightYawDeg, -180.0f, 180.0f);
            ImGui::SliderFloat("Light Pitch", &materialUi.lightPitchDeg, 5.0f, 85.0f);
            ImGui::Checkbox("Shadows", &materialUi.shadowsEnabled);
            if (materialUi.shadowsEnabled) {
                ImGui::SameLine();
                ImGui::Checkbox("Bake (freeze)", &materialUi.shadowsBaked);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Post");
            ImGui::SliderFloat("Exposure", &materialUi.exposure, 0.25f, 3.0f);
            ImGui::SliderFloat("Bloom Threshold", &materialUi.bloomThreshold, 0.0f, 4.0f);
            ImGui::SliderFloat("Bloom Strength", &materialUi.bloomStrength, 0.0f, 1.0f);
            ImGui::Separator();
            ImGui::Checkbox("Orbit Camera", &materialUi.orbitCamera);
            ImGui::Separator();
            ImGui::Text("Point Lights (%d / %d)", materialUi.numPointLights, kMaxPointLights);
            if (materialUi.numPointLights < kMaxPointLights && ImGui::Button("Add Light"))
                materialUi.numPointLights++;
            for (int _li = 0; _li < materialUi.numPointLights; ++_li) {
                auto& pl = materialUi.pointLights[_li];
                ImGui::PushID(_li);
                char label[16]; std::snprintf(label, sizeof(label), "Light %d", _li);
                if (ImGui::TreeNode(label)) {
                    ImGui::DragFloat3("Position", pl.pos, 0.05f);
                    ImGui::ColorEdit3("Color",    pl.color);
                    ImGui::SliderFloat("Range",     &pl.range,     0.1f, 20.0f);
                    ImGui::SliderFloat("Intensity", &pl.intensity, 0.0f, 50.0f);
                    if (ImGui::Button("Remove")) {
                        for (int _j = _li; _j < materialUi.numPointLights - 1; ++_j)
                            materialUi.pointLights[_j] = materialUi.pointLights[_j + 1];
                        materialUi.numPointLights--;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Button("Reset")) {
                materialUi = MaterialUiState{};
            }
        }
        ImGui::End();
        ImGui::Render();

        LightingUBO lightingData{};
        lightingData.lightDir      = glm::vec4(lightDir, materialUi.lightIntensity);
        lightingData.iblParams     = glm::vec4(
            materialUi.iblStrength,
            static_cast<float>(prefilteredEnvCube.mipLevels - 1),
            materialUi.shadowsEnabled ? 1.0f : 0.0f,
            0.0f);
        lightingData.surfaceScales = glm::vec4(
            materialUi.aoStrength, materialUi.normalStrength, 0.0f, 0.0f);
        std::memcpy(frame.lightingUbo.mapped, &lightingData, sizeof(LightingUBO));

        PointLightsUBO plData{};
        plData.count = materialUi.numPointLights;
        for (int _li = 0; _li < materialUi.numPointLights; ++_li) {
            const auto& pl = materialUi.pointLights[_li];
            plData.positionRange[_li]  = glm::vec4(pl.pos[0], pl.pos[1], pl.pos[2], pl.range);
            plData.colorIntensity[_li] = glm::vec4(pl.color[0], pl.color[1], pl.color[2], pl.intensity);
        }
        std::memcpy(frame.pointLightsUbo.mapped, &plData, sizeof(PointLightsUBO));

        uint32_t imageIndex = 0;
        VK_CHECK(vkAcquireNextImageKHR(device, vkbSwapchain.swapchain, UINT64_MAX,
                                       frame.imageAvailable, VK_NULL_HANDLE, &imageIndex));

        VK_CHECK(vkResetCommandBuffer(frame.cmd, 0));
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(frame.cmd, &beginInfo));

        VkDeviceSize vbOffset = 0;
        if (materialUi.shadowsEnabled && !materialUi.shadowsBaked) {
            transitionImage(frame.cmd, shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                                | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo shadowDepthAttach{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            shadowDepthAttach.imageView               = shadowMap.view;
            shadowDepthAttach.imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            shadowDepthAttach.loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR;
            shadowDepthAttach.storeOp                 = VK_ATTACHMENT_STORE_OP_STORE;
            shadowDepthAttach.clearValue.depthStencil = {1.0f, 0};

            VkRenderingInfo shadowRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            shadowRenderInfo.renderArea       = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
            shadowRenderInfo.layerCount       = 1;
            shadowRenderInfo.pDepthAttachment = &shadowDepthAttach;

            vkCmdBeginRendering(frame.cmd, &shadowRenderInfo);

            VkViewport shadowViewport{};
            shadowViewport.x        = 0.0f;
            shadowViewport.y        = 0.0f;
            shadowViewport.width    = static_cast<float>(kShadowMapSize);
            shadowViewport.height   = static_cast<float>(kShadowMapSize);
            shadowViewport.minDepth = 0.0f;
            shadowViewport.maxDepth = 1.0f;
            vkCmdSetViewport(frame.cmd, 0, 1, &shadowViewport);

            VkRect2D shadowScissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
            vkCmdSetScissor(frame.cmd, 0, 1, &shadowScissor);
            vkCmdSetDepthBias(frame.cmd, 1.25f, 0.0f, 1.75f);

            {
                const uint32_t depthKey = mat::computeVariantKey(helmetPkg, true);
                const glm::mat4 helmetModel(1.0f);
                vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  helmetMat.pipelineFor(depthKey));
                vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        helmetMat.pipelineLayout(), 0, 1,
                                        &matFrameSets[frameIndex], 0, nullptr);
                helmetInst.bind(frame.cmd);
                vkCmdPushConstants(frame.cmd, helmetMat.pipelineLayout(),
                                   VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(glm::mat4), &helmetModel);
                vkCmdBindVertexBuffers(frame.cmd, 0, 1, &helmetVB.buffer, &vbOffset);
                vkCmdBindIndexBuffer(frame.cmd, helmetIB.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(frame.cmd, helmetIndexCount, 1, 0, 0, 0);
            }

            vkCmdEndRendering(frame.cmd);

            transitionImage(frame.cmd, shadowMap.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }

        transitionImage(frame.cmd, hdrScene.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        transitionImage(frame.cmd, depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                            | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo colorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttach.imageView              = hdrScene.view;
        colorAttach.imageLayout            = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttach.loadOp                 = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp                = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.clearValue.color       = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingAttachmentInfo depthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttach.imageView              = depthImage.view;
        depthAttach.imageLayout            = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp                 = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp                = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderInfo.renderArea           = {{0, 0}, vkbSwapchain.extent};
        renderInfo.layerCount           = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments    = &colorAttach;
        renderInfo.pDepthAttachment     = &depthAttach;

        vkCmdBeginRendering(frame.cmd, &renderInfo);

        VkViewport viewport{};
        viewport.x        = 0.0f;
        viewport.y        = 0.0f;
        viewport.width    = static_cast<float>(vkbSwapchain.extent.width);
        viewport.height   = static_cast<float>(vkbSwapchain.extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, vkbSwapchain.extent};
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

        {
            const uint32_t colorKey = mat::computeVariantKey(helmetPkg, false);
            const glm::mat4 helmetModel(1.0f);
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              helmetMat.pipelineFor(colorKey));
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    helmetMat.pipelineLayout(), 0, 1,
                                    &matFrameSets[frameIndex], 0, nullptr);
            helmetInst.bind(frame.cmd);
            vkCmdPushConstants(frame.cmd, helmetMat.pipelineLayout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(glm::mat4), &helmetModel);
            vkCmdBindVertexBuffers(frame.cmd, 0, 1, &helmetVB.buffer, &vbOffset);
            vkCmdBindIndexBuffer(frame.cmd, helmetIB.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.cmd, helmetIndexCount, 1, 0, 0, 0);
        }

        // Skybox last so opaque mesh fragments early-Z reject the fullscreen triangle.
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                skyboxPipelineLayout, 0, 1, &skyboxSets[frameIndex], 0, nullptr);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);

        vkCmdEndRendering(frame.cmd);

        transitionImage(frame.cmd, hdrScene.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        transitionImage(frame.cmd, bloomA.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo bloomExtractAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bloomExtractAttach.imageView        = bloomA.view;
        bloomExtractAttach.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bloomExtractAttach.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
        bloomExtractAttach.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
        bloomExtractAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo bloomExtractInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        bloomExtractInfo.renderArea           = {{0, 0}, vkbSwapchain.extent};
        bloomExtractInfo.layerCount           = 1;
        bloomExtractInfo.colorAttachmentCount = 1;
        bloomExtractInfo.pColorAttachments    = &bloomExtractAttach;

        vkCmdBeginRendering(frame.cmd, &bloomExtractInfo);
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomExtractPipeline);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                extractLayout, 0, 1, &extractSet, 0, nullptr);
        vkCmdPushConstants(frame.cmd, extractLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(float), &materialUi.bloomThreshold);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(frame.cmd);

        transitionImage(frame.cmd, bloomA.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        transitionImage(frame.cmd, bloomB.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo bloomBlurAttachB{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bloomBlurAttachB.imageView        = bloomB.view;
        bloomBlurAttachB.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bloomBlurAttachB.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
        bloomBlurAttachB.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
        bloomBlurAttachB.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo bloomBlurInfoB{VK_STRUCTURE_TYPE_RENDERING_INFO};
        bloomBlurInfoB.renderArea           = {{0, 0}, vkbSwapchain.extent};
        bloomBlurInfoB.layerCount           = 1;
        bloomBlurInfoB.colorAttachmentCount = 1;
        bloomBlurInfoB.pColorAttachments    = &bloomBlurAttachB;

        vkCmdBeginRendering(frame.cmd, &bloomBlurInfoB);
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomBlurPipeline);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                blurLayout, 0, 1, &blurSetA, 0, nullptr);
        const glm::vec2 blurHorizontal(1.0f, 0.0f);
        vkCmdPushConstants(frame.cmd, blurLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(glm::vec2), &blurHorizontal);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(frame.cmd);

        transitionImage(frame.cmd, bloomB.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        transitionImage(frame.cmd, bloomA.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo bloomBlurAttachA{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bloomBlurAttachA.imageView        = bloomA.view;
        bloomBlurAttachA.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bloomBlurAttachA.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
        bloomBlurAttachA.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
        bloomBlurAttachA.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo bloomBlurInfoA{VK_STRUCTURE_TYPE_RENDERING_INFO};
        bloomBlurInfoA.renderArea           = {{0, 0}, vkbSwapchain.extent};
        bloomBlurInfoA.layerCount           = 1;
        bloomBlurInfoA.colorAttachmentCount = 1;
        bloomBlurInfoA.pColorAttachments    = &bloomBlurAttachA;

        vkCmdBeginRendering(frame.cmd, &bloomBlurInfoA);
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomBlurPipeline);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                blurLayout, 0, 1, &blurSetB, 0, nullptr);
        const glm::vec2 blurVertical(0.0f, 1.0f);
        vkCmdPushConstants(frame.cmd, blurLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(glm::vec2), &blurVertical);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(frame.cmd);

        transitionImage(frame.cmd, bloomA.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        transitionImage(frame.cmd, scImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo compositeAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        compositeAttach.imageView        = scViews[imageIndex];
        compositeAttach.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        compositeAttach.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
        compositeAttach.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
        compositeAttach.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderingInfo compositeInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        compositeInfo.renderArea           = {{0, 0}, vkbSwapchain.extent};
        compositeInfo.layerCount           = 1;
        compositeInfo.colorAttachmentCount = 1;
        compositeInfo.pColorAttachments    = &compositeAttach;

        vkCmdBeginRendering(frame.cmd, &compositeInfo);
        vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
        vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                compositeLayout, 0, 1, &compositeSet, 0, nullptr);
        const CompositePushData compositePush{
            materialUi.exposure,
            materialUi.bloomStrength
        };
        vkCmdPushConstants(frame.cmd, compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(CompositePushData), &compositePush);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.cmd);
        vkCmdEndRendering(frame.cmd);

        transitionImage(frame.cmd, scImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

        // Copy hdrScene → sceneCapture for next-frame screen-space refraction (1-frame latency).
        transitionImage(frame.cmd, hdrScene.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
        transitionImage(frame.cmd, sceneCapture.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy sceneCopy{};
        sceneCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        sceneCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        sceneCopy.extent         = {vkbSwapchain.extent.width, vkbSwapchain.extent.height, 1};
        vkCmdCopyImage(frame.cmd,
                       hdrScene.image,      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sceneCapture.image,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &sceneCopy);
        transitionImage(frame.cmd, sceneCapture.image, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        VK_CHECK(vkEndCommandBuffer(frame.cmd));

        VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbInfo.commandBuffer = frame.cmd;

        VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalInfo.semaphore = frame.renderFinished;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount   = 1;
        submit.pWaitSemaphoreInfos      = &waitInfo;
        submit.commandBufferInfoCount   = 1;
        submit.pCommandBufferInfos      = &cbInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos    = &signalInfo;

        VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submit, frame.inFlight));

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores    = &frame.renderFinished;
        presentInfo.swapchainCount     = 1;
        presentInfo.pSwapchains        = &vkbSwapchain.swapchain;
        presentInfo.pImageIndices      = &imageIndex;
        VK_CHECK(vkQueuePresentKHR(graphicsQueue, &presentInfo));

        frameIndex = (frameIndex + 1) % kFramesInFlight;
    }

    // ---- Teardown ----
    vkDeviceWaitIdle(device);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    for (auto& f : frames) {
        vmaDestroyBuffer(allocator, f.pointLightsUbo.buffer, f.pointLightsUbo.allocation);
        vmaDestroyBuffer(allocator, f.lightingUbo.buffer, f.lightingUbo.allocation);
        vmaDestroyBuffer(allocator, f.ubo.buffer, f.ubo.allocation);
        vkDestroyFence(device, f.inFlight, nullptr);
        vkDestroySemaphore(device, f.renderFinished, nullptr);
        vkDestroySemaphore(device, f.imageAvailable, nullptr);
        vkDestroyCommandPool(device, f.pool, nullptr);
    }

    helmetInst.destroy();
    helmetMat.destroy();
    vkDestroyDescriptorPool(device, matFramePool, nullptr);
    vkDestroyDescriptorSetLayout(device, matFrameSetLayout, nullptr);
    vmaDestroyBuffer(allocator, shBuf.buffer, shBuf.allocation);

    vkDestroyDescriptorPool(device, skyboxPool, nullptr);
    vkDestroyDescriptorPool(device, postDescriptorPool, nullptr);
    vkDestroyDescriptorPool(device, imguiDescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, skyboxSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, postOneSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, postTwoSetLayout, nullptr);

    vkDestroyPipeline(device, compositePipeline, nullptr);
    vkDestroyPipeline(device, bloomBlurPipeline, nullptr);
    vkDestroyPipeline(device, bloomExtractPipeline, nullptr);
    vkDestroyPipeline(device, skyboxPipeline, nullptr);
    vkDestroyPipelineLayout(device, skyboxPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, compositeLayout, nullptr);
    vkDestroyPipelineLayout(device, blurLayout, nullptr);
    vkDestroyPipelineLayout(device, extractLayout, nullptr);

    vmaDestroyBuffer(allocator, helmetIB.buffer, helmetIB.allocation);
    vmaDestroyBuffer(allocator, helmetVB.buffer, helmetVB.allocation);

    vkDestroySampler(device, iblSampler, nullptr);
    vkDestroySampler(device, envBakeSampler, nullptr);
    vkDestroySampler(device, shadowSampler, nullptr);

    vkDestroySampler(device, defaultSampler, nullptr);
    for (Texture* t : {&brdfLut, &normalMap, &metalRough, &baseColor}) {
        vkDestroyImageView(device, t->view, nullptr);
        vmaDestroyImage(allocator, t->image, t->allocation);
    }

    for (Cubemap* c : {&prefilteredEnvCube, &envCube}) {
        destroyCubemap(device, allocator, *c);
    }

    vkDestroyCommandPool(device, uploadPool, nullptr);

    vkDestroyImageView(device, depthImage.view, nullptr);
    vmaDestroyImage(allocator, depthImage.image, depthImage.allocation);
    vkDestroyImageView(device, shadowMap.view, nullptr);
    vmaDestroyImage(allocator, shadowMap.image, shadowMap.allocation);
    for (RenderTarget* target : {&sceneCapture, &bloomB, &bloomA, &hdrScene}) {
        vkDestroyImageView(device, target->view, nullptr);
        vmaDestroyImage(allocator, target->image, target->allocation);
    }

    vkbSwapchain.destroy_image_views(scViews);
    vkb::destroy_swapchain(vkbSwapchain);

    // forfun::destroyDevice handles allocator, device, surface, debug
    // messenger, and instance in the right order.
    forfun::destroyDevice(gpu);

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
