#pragma once

#include <vulkan/vulkan.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

inline constexpr uint32_t    kWindowWidth    = 1280;
inline constexpr uint32_t    kWindowHeight   = 720;
inline constexpr uint32_t    kShadowMapSize  = 2048;
inline constexpr const char* kAppName        = "forfun-graphics";
inline constexpr uint32_t    kFramesInFlight = 2;
inline constexpr VkFormat    kDepthFormat    = VK_FORMAT_D32_SFLOAT;

#define VK_CHECK(expr)                                                         \
    do {                                                                       \
        VkResult _r = (expr);                                                  \
        if (_r != VK_SUCCESS) {                                                \
            std::fprintf(stderr, "Vulkan error %d at %s:%d (%s)\n",            \
                         _r, __FILE__, __LINE__, #expr);                       \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct ImageData {
    std::vector<uint8_t> rgba;
    uint32_t             width  = 0;
    uint32_t             height = 0;
};

struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    ImageData             baseColor;
    ImageData             metallicRoughness;  // R=AO, G=roughness, B=metallic (glTF ORM packing)
    ImageData             normal;
};

struct UBO {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;   // skybox uses this to reconstruct world dir at far plane
    glm::mat4 lightViewProj;
    glm::vec4 cameraPos;
};

// Push-constant block used by every material pipeline (one mat4 = 64 bytes).
struct ModelPushConstants {
    glm::mat4 model;
};

struct LightingUBO {
    glm::vec4 lightDir;      // xyz = direction (normalized), w = direct intensity
    glm::vec4 iblParams;     // x = IBL strength, y = max prefiltered LOD, z = shadows enabled (0/1)
    glm::vec4 surfaceScales; // x = AO scale, y = normal scale
};

static constexpr int kMaxPointLights = 8;

// std140 layout: two vec4 arrays then an int count.
struct PointLightsUBO {
    glm::vec4 positionRange[kMaxPointLights];  // xyz=world pos, w=range
    glm::vec4 colorIntensity[kMaxPointLights]; // xyz=linear color, w=intensity
    int       count = 0;
    float     _pad[3]{};
};

struct PrefilterPushConstants {
    float roughness = 0.0f;
};

struct CompositePushData {
    float exposure;
    float bloomStrength;
};
