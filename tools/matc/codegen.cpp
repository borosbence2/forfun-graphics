#include "codegen.h"

#include <string>

namespace matc {

const char* uboGlslType(ParamType type) {
    switch (type) {
        case ParamType::Float:  return "float";
        case ParamType::Float2: return "vec2";
        case ParamType::Float3: return "vec3";
        case ParamType::Float4: return "vec4";
        case ParamType::Int:    return "int";
        case ParamType::Bool:   return "int"; // bool disallowed in std140 UBO
    }
    return "float";
}

uint32_t computeBaseVariantKey(const MaterialDescription& mat) {
    uint32_t key = 0;
    for (auto attr : mat.requires_) {
        switch (attr) {
            case VertexAttr::Normal:  key |= mat::VAR_HAS_VERTEX_NORMAL;  break;
            case VertexAttr::Tangent: key |= mat::VAR_HAS_VERTEX_TANGENT; break;
            case VertexAttr::UV0:     key |= mat::VAR_HAS_VERTEX_UV0;     break;
            default: break;
        }
    }
    for (const auto& s : mat.samplers) {
        if      (s.name == "baseColorMap")         key |= mat::VAR_HAS_BASE_COLOR_MAP;
        else if (s.name == "normalMap")            key |= mat::VAR_HAS_NORMAL_MAP;
        else if (s.name == "metallicRoughnessMap") key |= mat::VAR_HAS_MR_MAP;
    }
    // Phase 4 feature flags only apply to Lit; other shading models bypass them.
    if (mat.shadingModel == ShadingModel::Lit) {
        const std::string& frag = mat.fragmentBlock;
        auto uses = [&](const char* field) {
            return frag.find(field) != std::string::npos;
        };
        if (uses("m.clearcoat") || uses("m.clearcoatRoughness") || uses("m.clearcoatNormal"))
            key |= mat::VAR_HAS_CLEARCOAT;
        if (uses("m.anisotropy"))
            key |= mat::VAR_HAS_ANISOTROPY;
        if (uses("m.sheenColor") || uses("m.sheenRoughness"))
            key |= mat::VAR_HAS_SHEEN;
        if (uses("m.transmission") || uses("m.ior") || uses("m.thickness"))
            key |= mat::VAR_HAS_REFRACTION;
    }
    return key;
}

std::vector<uint32_t> enumerateVariants(const MaterialDescription& mat) {
    return { computeBaseVariantKey(mat), static_cast<uint32_t>(mat::VAR_PASS_DEPTH_ONLY) };
}

// ---- Vertex shader ----

std::string generateVertGlsl(const MaterialDescription& mat, uint32_t variantKey) {
    (void)mat;
    const bool depthOnly = variantKey & mat::VAR_PASS_DEPTH_ONLY;
    const bool hasNormal  = variantKey & mat::VAR_HAS_VERTEX_NORMAL;
    const bool hasTangent = variantKey & mat::VAR_HAS_VERTEX_TANGENT;
    const bool hasUV0     = variantKey & mat::VAR_HAS_VERTEX_UV0;

    std::string s;
    s.reserve(1024);
    s += "#version 450\n\n";

    if (hasNormal)  s += "#define HAS_VERTEX_NORMAL 1\n";
    if (hasTangent) s += "#define HAS_VERTEX_TANGENT 1\n";
    if (hasUV0)     s += "#define HAS_VERTEX_UV0 1\n";
    if (depthOnly)  s += "#define PASS_DEPTH_ONLY 1\n";
    s += "\n";

    s += "layout(location = 0) in vec3 inPosition;\n";
    if (hasNormal)  s += "layout(location = 1) in vec3 inNormal;\n";
    if (hasTangent) s += "layout(location = 2) in vec4 inTangent;\n";
    if (hasUV0)     s += "layout(location = 3) in vec2 inUV0;\n";
    s += "\n";

    s += "layout(push_constant) uniform PushConstants {\n"
         "    mat4 model;\n"
         "} pc;\n\n";

    s += "layout(set = 0, binding = 0) uniform FrameUBO {\n"
         "    mat4 viewProj;\n"
         "    mat4 invViewProj;\n"
         "    mat4 lightViewProj;\n"
         "    vec4 cameraPos;\n"
         "} frame;\n\n";

    if (!depthOnly) {
        s += "layout(location = 0) out vec3 outWorldPos;\n";
        if (hasNormal)  s += "layout(location = 1) out vec3 outNormal;\n";
        if (hasTangent) s += "layout(location = 2) out vec4 outTangent;\n";
        if (hasUV0)     s += "layout(location = 3) out vec2 outUV0;\n";
        s += "\n";
    }

    s += "void main() {\n"
         "    vec4 worldPos = pc.model * vec4(inPosition, 1.0);\n";
    if (depthOnly)
        s += "    gl_Position = frame.lightViewProj * worldPos;\n";
    else
        s += "    gl_Position = frame.viewProj * worldPos;\n";
    if (!depthOnly) {
        s += "    outWorldPos = worldPos.xyz;\n";
        if (hasNormal)
            s += "    outNormal  = normalize(mat3(pc.model) * inNormal);\n";
        if (hasTangent)
            s += "    outTangent = vec4(normalize(mat3(pc.model) * inTangent.xyz), inTangent.w);\n";
        if (hasUV0)
            s += "    outUV0 = inUV0;\n";
    }
    s += "}\n";
    return s;
}

// ---- Fragment shader ----

std::string generateFragGlsl(const MaterialDescription& mat, uint32_t variantKey) {
    if (variantKey & mat::VAR_PASS_DEPTH_ONLY)
        return "#version 450\nvoid main() {}\n";

    const bool hasNormal  = variantKey & mat::VAR_HAS_VERTEX_NORMAL;
    const bool hasTangent = variantKey & mat::VAR_HAS_VERTEX_TANGENT;
    const bool hasUV0     = variantKey & mat::VAR_HAS_VERTEX_UV0;

    const bool isUnlit     = mat.shadingModel == ShadingModel::Unlit;
    const bool isCloth     = mat.shadingModel == ShadingModel::Cloth;
    const bool isSubsurf   = mat.shadingModel == ShadingModel::Subsurface;
    const bool isSpecGloss = mat.shadingModel == ShadingModel::SpecularGlossiness;

    std::string s;
    s.reserve(8192);
    s += "#version 450\n\n";

    // Feature defines
    if (hasNormal)                                s += "#define HAS_VERTEX_NORMAL 1\n";
    if (hasTangent)                               s += "#define HAS_VERTEX_TANGENT 1\n";
    if (hasUV0)                                   s += "#define HAS_VERTEX_UV0 1\n";
    if (variantKey & mat::VAR_HAS_BASE_COLOR_MAP) s += "#define HAS_BASE_COLOR_MAP 1\n";
    if (variantKey & mat::VAR_HAS_NORMAL_MAP)     s += "#define HAS_NORMAL_MAP 1\n";
    if (variantKey & mat::VAR_HAS_MR_MAP)         s += "#define HAS_MR_MAP 1\n";
    if (variantKey & mat::VAR_HAS_CLEARCOAT)      s += "#define HAS_CLEARCOAT 1\n";
    if (variantKey & mat::VAR_HAS_ANISOTROPY)     s += "#define HAS_ANISOTROPY 1\n";
    if (variantKey & mat::VAR_HAS_SHEEN)          s += "#define HAS_SHEEN 1\n";
    if (variantKey & mat::VAR_HAS_REFRACTION)     s += "#define HAS_REFRACTION 1\n";
    if (isUnlit)     s += "#define SHADING_MODEL_UNLIT 1\n";
    if (isCloth)     s += "#define SHADING_MODEL_CLOTH 1\n";
    if (isSubsurf)   s += "#define SHADING_MODEL_SUBSURFACE 1\n";
    if (isSpecGloss) s += "#define SHADING_MODEL_SPECGLOSS 1\n";
    s += "\n";

    // Varyings in
    s += "layout(location = 0) in vec3 inWorldPos;\n";
    if (hasNormal)  s += "layout(location = 1) in vec3 inNormal;\n";
    if (hasTangent) s += "layout(location = 2) in vec4 inTangent;\n";
    if (hasUV0)     s += "layout(location = 3) in vec2 inUV0;\n";
    s += "\nlayout(location = 0) out vec4 outColor;\n\n";

    // b0: FrameUBO (all shading models)
    s += "layout(set = 0, binding = 0) uniform FrameUBO {\n"
         "    mat4 viewProj;\n"
         "    mat4 invViewProj;\n"
         "    mat4 lightViewProj;\n"
         "    vec4 cameraPos;\n"
         "} frame;\n\n";

    // b1-b6: PBR resources (skipped for unlit; descriptorBindingPartiallyBound covers unused slots)
    if (!isUnlit) {
        s += "layout(set = 0, binding = 1) uniform SH_UBO { vec4 coeffs[9]; } _sh;\n";
        s += "layout(set = 0, binding = 2) uniform samplerCube _prefilteredEnvCube;\n";
        s += "layout(set = 0, binding = 3) uniform sampler2D   _brdfLut;\n";
        s += "layout(set = 0, binding = 4) uniform sampler2D   _shadowMap;\n\n";
        s += "#ifdef HAS_REFRACTION\n"
             "layout(set = 0, binding = 6) uniform sampler2D _sceneCapture;\n"
             "#endif\n\n";
        s += "layout(set = 0, binding = 7) uniform _PointLightsUBO {\n"
             "    vec4 positionRange[8];\n"
             "    vec4 colorIntensity[8];\n"
             "    int  count;\n"
             "} _pointLights;\n\n";
        s += "layout(set = 0, binding = 5) uniform LightingUBO {\n"
             "    vec4 lightDir;      // xyz = direction, w = direct intensity\n"
             "    vec4 iblParams;     // x = IBL strength, y = max prefiltered LOD, z = shadows enabled\n"
             "    vec4 surfaceScales; // x = AO scale, y = normal scale\n"
             "} _lighting;\n\n";
    }

    // Set 1: material params UBO
    if (!mat.parameters.empty()) {
        s += "layout(set = 1, binding = 0, std140) uniform _MaterialParams {\n";
        for (const auto& p : mat.parameters) {
            s += "    "; s += uboGlslType(p.type);
            s += " _p_"; s += p.name; s += ";\n";
        }
        s += "} _matParams;\n\n";
    }

    // Set 1: samplers
    for (size_t i = 0; i < mat.samplers.size(); ++i) {
        const auto& smp = mat.samplers[i];
        const char* glslType = (smp.type == SamplerType::SamplerCube) ? "samplerCube" : "sampler2D";
        s += "layout(set = 1, binding = " + std::to_string(i + 1) + ") uniform ";
        s += glslType; s += " "; s += smp.name; s += ";\n";
    }
    if (!mat.samplers.empty()) s += "\n";

    // ---- MaterialInputs struct ----
    if (isUnlit) {
        s += "struct MaterialInputs {\n"
             "    vec4 baseColor;\n"
             "    vec3 emissive;\n"
             "};\n\n";
    } else if (isCloth) {
        s += "struct MaterialInputs {\n"
             "    vec4  baseColor;\n"
             "    float roughness;\n"
             "    vec3  sheenColor;\n"
             "    vec3  normal;\n"
             "    vec3  emissive;\n"
             "    vec3  subsurfaceColor;\n"
             "};\n\n";
    } else if (isSubsurf) {
        s += "struct MaterialInputs {\n"
             "    vec4  baseColor;\n"
             "    float metallic;\n"
             "    float roughness;\n"
             "    float ao;\n"
             "    vec3  normal;\n"
             "    vec3  emissive;\n"
             "    vec3  subsurfaceColor;\n"
             "    float subsurfacePower;\n"
             "    float thickness;\n"
             "};\n\n";
    } else if (isSpecGloss) {
        s += "struct MaterialInputs {\n"
             "    vec4  diffuseFactor;\n"
             "    vec3  specular;\n"
             "    float glossiness;\n"
             "    vec3  normal;\n"
             "    vec3  emissive;\n"
             "};\n\n";
    } else {
        // Lit — full PBR with Phase 4 optional fields
        s += "struct MaterialInputs {\n"
             "    vec4  baseColor;\n"
             "    float metallic;\n"
             "    float roughness;\n"
             "    float ao;\n"
             "    vec3  normal;\n"
             "    vec3  emissive;\n"
             "#ifdef HAS_CLEARCOAT\n"
             "    float clearcoat;\n"
             "    float clearcoatRoughness;\n"
             "    vec3  clearcoatNormal;\n"
             "#endif\n"
             "#ifdef HAS_ANISOTROPY\n"
             "    float anisotropy;\n"
             "#endif\n"
             "#ifdef HAS_SHEEN\n"
             "    vec3  sheenColor;\n"
             "    float sheenRoughness;\n"
             "#endif\n"
             "#ifdef HAS_REFRACTION\n"
             "    float transmission;\n"
             "    float ior;\n"
             "    float thickness;\n"
             "#endif\n"
             "};\n\n";
    }

    s += hasUV0 ? "vec2 getUV0() { return inUV0; }\n\n"
                : "vec2 getUV0() { return vec2(0.0); }\n\n";

    // #define aliases emitted after MaterialInputs to avoid macro expansion
    // of struct member names that share a name with a parameter.
    if (!mat.parameters.empty()) {
        for (const auto& p : mat.parameters)
            s += "#define " + p.name + " _matParams._p_" + p.name + "\n";
        s += "\n";
    }

    // ---- User material function ----
    s += mat.fragmentBlock;
    s += "\n\n";

    // ---- PBR helpers (skipped for unlit) ----
    if (!isUnlit) {
        s += "const float PI = 3.14159265359;\n\n";

        s += "float D_GGX(float NdotH, float a) {\n"
             "    float a2 = a * a;\n"
             "    float f  = (NdotH * a2 - NdotH) * NdotH + 1.0;\n"
             "    return a2 / (PI * f * f);\n"
             "}\n\n";

        s += "float G_SchlickGGX(float NdotX, float k) {\n"
             "    return NdotX / (NdotX * (1.0 - k) + k);\n"
             "}\n\n";

        s += "float G_Smith(float NdotV, float NdotL, float roughness) {\n"
             "    float r = roughness + 1.0;\n"
             "    float k = (r * r) / 8.0;\n"
             "    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);\n"
             "}\n\n";

        s += "vec3 F_Schlick(float cosTheta, vec3 F0) {\n"
             "    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);\n"
             "}\n\n";

        s += "vec3 F_SchlickRoughness(float cosTheta, vec3 F0, float roughness) {\n"
             "    return F0 + (max(vec3(1.0 - roughness), F0) - F0)\n"
             "              * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);\n"
             "}\n\n";

        s += "vec3 evaluateSH(vec3 N) {\n"
             "    vec3 col = _sh.coeffs[0].rgb;\n"
             "    col += _sh.coeffs[1].rgb * N.y + _sh.coeffs[2].rgb * N.z + _sh.coeffs[3].rgb * N.x;\n"
             "    col += _sh.coeffs[4].rgb * (N.x * N.y);\n"
             "    col += _sh.coeffs[5].rgb * (N.y * N.z);\n"
             "    col += _sh.coeffs[6].rgb * (3.0 * N.z * N.z - 1.0);\n"
             "    col += _sh.coeffs[7].rgb * (N.x * N.z);\n"
             "    col += _sh.coeffs[8].rgb * (N.x * N.x - N.y * N.y);\n"
             "    return max(col, vec3(0.0));\n"
             "}\n\n";

        s += "float Fd_Burley(float NdotV, float NdotL, float LdotH, float roughness) {\n"
             "    float f90 = 0.5 + 2.0 * roughness * LdotH * LdotH;\n"
             "    float FD90_L = 1.0 + (f90 - 1.0) * pow(clamp(1.0 - NdotL, 0.0, 1.0), 5.0);\n"
             "    float FD90_V = 1.0 + (f90 - 1.0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);\n"
             "    return FD90_L * FD90_V / PI;\n"
             "}\n\n";

        s += "mat3 derivativeTBN(vec3 worldPos, vec3 N, vec2 uv) {\n"
             "    vec3 dp1  = dFdx(worldPos);\n"
             "    vec3 dp2  = dFdy(worldPos);\n"
             "    vec2 duv1 = dFdx(uv);\n"
             "    vec2 duv2 = dFdy(uv);\n"
             "    vec3 dp2perp = cross(dp2, N);\n"
             "    vec3 dp1perp = cross(N, dp1);\n"
             "    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;\n"
             "    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;\n"
             "    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));\n"
             "    return mat3(T * invmax, B * invmax, N);\n"
             "}\n"
             "vec3 perturbNormal(vec3 worldPos, vec3 N, vec2 uv, vec3 nTS) {\n"
             "    return normalize(derivativeTBN(worldPos, N, uv) * nTS);\n"
             "}\n\n";

        s += "#ifdef HAS_ANISOTROPY\n"
             "float D_GGX_Anisotropic(float NdotH, float TdotH, float BdotH, float at, float ab) {\n"
             "    float f = (TdotH / at) * (TdotH / at) + (BdotH / ab) * (BdotH / ab) + NdotH * NdotH;\n"
             "    return 1.0 / (PI * at * ab * f * f);\n"
             "}\n"
             "float V_SmithGGXCorrelated_Anisotropic(float at, float ab,\n"
             "        float TdotV, float BdotV, float TdotL, float BdotL,\n"
             "        float NdotV, float NdotL) {\n"
             "    float lambdaV = NdotL * length(vec3(at * TdotV, ab * BdotV, NdotV));\n"
             "    float lambdaL = NdotV * length(vec3(at * TdotL, ab * BdotL, NdotL));\n"
             "    return 0.5 / (lambdaV + lambdaL);\n"
             "}\n"
             "#endif\n\n";

        // Cloth uses Charlie/Ashikhmin unconditionally; other models guard under HAS_SHEEN.
        if (isCloth) {
            s += "float D_Charlie(float roughness, float NdotH) {\n"
                 "    float invA  = 1.0 / max(roughness * roughness, 0.0001);\n"
                 "    float cos2h = NdotH * NdotH;\n"
                 "    float sin2h = max(1.0 - cos2h, 0.0078125);\n"
                 "    return (2.0 + invA) * pow(sin2h, invA * 0.5) / (2.0 * PI);\n"
                 "}\n"
                 "float V_Ashikhmin(float NdotL, float NdotV) {\n"
                 "    return 1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV));\n"
                 "}\n\n";
        } else {
            s += "#ifdef HAS_SHEEN\n"
                 "float D_Charlie(float roughness, float NdotH) {\n"
                 "    float invA  = 1.0 / max(roughness * roughness, 0.0001);\n"
                 "    float cos2h = NdotH * NdotH;\n"
                 "    float sin2h = max(1.0 - cos2h, 0.0078125);\n"
                 "    return (2.0 + invA) * pow(sin2h, invA * 0.5) / (2.0 * PI);\n"
                 "}\n"
                 "float V_Ashikhmin(float NdotL, float NdotV) {\n"
                 "    return 1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV));\n"
                 "}\n"
                 "#endif\n\n";
        }

        s += "float sampleShadow(vec3 N, vec3 L) {\n"
             "    vec4 shadowClip  = frame.lightViewProj * vec4(inWorldPos, 1.0);\n"
             "    vec3 shadowNdc   = shadowClip.xyz / max(shadowClip.w, 1e-5);\n"
             "    vec2 shadowUv    = shadowNdc.xy * 0.5 + 0.5;\n"
             "    float shadowDepth = shadowNdc.z;\n"
             "    if (shadowClip.w <= 0.0 || shadowDepth <= 0.0 || shadowDepth >= 1.0) return 1.0;\n"
             "    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 ||\n"
             "        shadowUv.y <= 0.0 || shadowUv.y >= 1.0) return 1.0;\n"
             "    float bias = max(0.0025 * (1.0 - dot(N, L)), 0.0005);\n"
             "    vec2 texelSize = 1.0 / vec2(textureSize(_shadowMap, 0));\n"
             "    float visibility = 0.0;\n"
             "    for (int y = -1; y <= 1; ++y) {\n"
             "        for (int x = -1; x <= 1; ++x) {\n"
             "            float closest = texture(_shadowMap, shadowUv + vec2(x, y) * texelSize).r;\n"
             "            visibility += shadowDepth - bias <= closest ? 1.0 : 0.0;\n"
             "        }\n"
             "    }\n"
             "    return visibility / 9.0;\n"
             "}\n\n";

        s += "vec3 evalPointLight(vec3 worldPos, vec3 N, vec3 V, float NdotV,\n"
             "                    vec3 albedo, vec3 F0, float roughness, float metallic,\n"
             "                    vec4 posRange, vec4 colInt) {\n"
             "    vec3  Lv   = posRange.xyz - worldPos;\n"
             "    float dist = length(Lv);\n"
             "    vec3  L    = Lv / max(dist, 1e-5);\n"
             "    float u    = clamp(dist / max(posRange.w, 0.001), 0.0, 1.0);\n"
             "    float attn = (1.0 - u * u) * (1.0 - u * u) / (dist * dist + 1.0);\n"
             "    vec3  H    = normalize(V + L);\n"
             "    float NdotL = max(dot(N, L), 0.0);\n"
             "    float NdotH = max(dot(N, H), 0.0);\n"
             "    float HdotV = max(dot(H, V), 0.0);\n"
             "    vec3  F = F_Schlick(HdotV, F0);\n"
             "    float D = D_GGX(NdotH, roughness * roughness);\n"
             "    float G = G_Smith(NdotV, NdotL, roughness);\n"
             "    vec3  spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);\n"
             "    vec3  kD   = (1.0 - F) * (1.0 - metallic);\n"
             "    return (kD * albedo / PI + spec) * colInt.rgb * colInt.w * NdotL * attn;\n"
             "}\n\n";
    } // !isUnlit

    // ---- main() ----
    if (isUnlit) {
        s += "void main() {\n"
             "    MaterialInputs m;\n"
             "    m.baseColor = vec4(1.0);\n"
             "    m.emissive  = vec3(0.0);\n"
             "    material(m);\n"
             "    outColor = vec4(m.baseColor.rgb + m.emissive, m.baseColor.a);\n"
             "}\n";

    } else if (isCloth) {
        s += "void main() {\n"
             "    MaterialInputs m;\n"
             "    m.baseColor       = vec4(1.0);\n"
             "    m.roughness       = 1.0;\n"
             "    m.sheenColor      = vec3(0.0);\n"
             "    m.normal          = vec3(0.0, 0.0, 1.0);\n"
             "    m.emissive        = vec3(0.0);\n"
             "    m.subsurfaceColor = vec3(0.0);\n"
             "    material(m);\n\n"
             "    m.normal.xy *= _lighting.surfaceScales.y;\n"
             "    m.normal = normalize(m.normal);\n\n"
             "    vec3  albedo    = m.baseColor.rgb;\n"
             "    float roughness = clamp(m.roughness, 0.04, 1.0);\n\n";
        if (hasNormal)
            s += "    vec3 Nv = normalize(inNormal);\n";
        else
            s += "    vec3 Nv = vec3(0.0, 1.0, 0.0);\n";
        s += "    mat3 _tbn = derivativeTBN(inWorldPos, Nv, getUV0());\n"
             "    vec3 N = normalize(_tbn * m.normal);\n"
             "    vec3 V = normalize(frame.cameraPos.xyz - inWorldPos);\n"
             "    float NdotV = max(dot(N, V), 0.0);\n\n"
             "    vec3  L            = normalize(_lighting.lightDir.xyz);\n"
             "    float lightIntensity = _lighting.lightDir.w;\n"
             "    vec3  H     = normalize(V + L);\n"
             "    float NdotL = max(dot(N, L), 0.0);\n"
             "    float NdotH = max(dot(N, H), 0.0);\n\n"
             "    float shadow = _lighting.iblParams.z > 0.5 ? sampleShadow(N, L) : 1.0;\n\n"
             "    float sr = max(roughness, 0.0001);\n"
             "    float Ds = D_Charlie(sr, NdotH);\n"
             "    float Vs = V_Ashikhmin(NdotL, NdotV);\n"
             "    vec3  sheenDirect = Ds * Vs * m.sheenColor * lightIntensity * NdotL * shadow;\n\n"
             "    float sheenMax    = max(max(m.sheenColor.r, m.sheenColor.g), m.sheenColor.b);\n"
             "    vec3  diffuseDirect = (1.0 - sheenMax) * albedo * NdotL * lightIntensity * shadow / PI;\n\n"
             "    float wrapNdotL  = (NdotL + 0.5) / 2.25;\n"
             "    vec3  subsurface = wrapNdotL * m.subsurfaceColor * lightIntensity * shadow;\n\n"
             "    float iblStrength = _lighting.iblParams.x;\n"
             "    vec3 R = reflect(-V, N);\n"
             "    vec3 irradiance  = evaluateSH(N);\n"
             "    vec3 diffuseIBL  = (1.0 - sheenMax) * albedo * irradiance * iblStrength;\n"
             "    vec3 sheenIBL    = textureLod(_prefilteredEnvCube, R, sr * _lighting.iblParams.y).rgb\n"
             "                       * m.sheenColor * iblStrength;\n\n"
             "    vec3 color = diffuseDirect + sheenDirect + subsurface + diffuseIBL + sheenIBL + m.emissive;\n"
             "    for (int _i = 0; _i < _pointLights.count; ++_i)\n"
             "        color += evalPointLight(inWorldPos, N, V, NdotV, albedo, vec3(0.04), roughness, 0.0,\n"
             "                               _pointLights.positionRange[_i], _pointLights.colorIntensity[_i]);\n"
             "    outColor = vec4(color, 1.0);\n"
             "}\n";

    } else if (isSubsurf) {
        s += "void main() {\n"
             "    MaterialInputs m;\n"
             "    m.baseColor       = vec4(1.0);\n"
             "    m.metallic        = 0.0;\n"
             "    m.roughness       = 1.0;\n"
             "    m.ao              = 1.0;\n"
             "    m.normal          = vec3(0.0, 0.0, 1.0);\n"
             "    m.emissive        = vec3(0.0);\n"
             "    m.subsurfaceColor = vec3(0.3);\n"
             "    m.subsurfacePower = 1.0;\n"
             "    m.thickness       = 0.0;\n"
             "    material(m);\n\n"
             "    m.ao *= _lighting.surfaceScales.x;\n"
             "    m.normal.xy *= _lighting.surfaceScales.y;\n"
             "    m.normal = normalize(m.normal);\n\n"
             "    vec3  albedo    = m.baseColor.rgb;\n"
             "    float metallic  = clamp(m.metallic,  0.0, 1.0);\n"
             "    float roughness = clamp(m.roughness, 0.04, 1.0);\n"
             "    float ao        = clamp(m.ao,        0.0, 1.0);\n\n";
        if (hasNormal)
            s += "    vec3 Nv = normalize(inNormal);\n";
        else
            s += "    vec3 Nv = vec3(0.0, 1.0, 0.0);\n";
        s += "    mat3 _tbn = derivativeTBN(inWorldPos, Nv, getUV0());\n"
             "    vec3 N = normalize(_tbn * m.normal);\n"
             "    vec3 V = normalize(frame.cameraPos.xyz - inWorldPos);\n"
             "    float NdotV = max(dot(N, V), 0.0);\n\n"
             "    vec3 F0 = mix(vec3(0.04), albedo, metallic);\n\n"
             "    vec3  L            = normalize(_lighting.lightDir.xyz);\n"
             "    float lightIntensity = _lighting.lightDir.w;\n"
             "    vec3  H     = normalize(V + L);\n"
             "    float NdotL = max(dot(N, L), 0.0);\n"
             "    float NdotH = max(dot(N, H), 0.0);\n"
             "    float HdotV = max(dot(H, V), 0.0);\n\n"
             "    vec3  F = F_Schlick(HdotV, F0);\n"
             "    float D = D_GGX(NdotH, roughness * roughness);\n"
             "    float G = G_Smith(NdotV, NdotL, roughness);\n"
             "    vec3  specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);\n"
             "    vec3  kD_dir   = (1.0 - F) * (1.0 - metallic);\n"
             "    vec3  diffuse  = kD_dir * albedo * Fd_Burley(NdotV, NdotL, HdotV, roughness);\n\n"
             "    float shadow = _lighting.iblParams.z > 0.5 ? sampleShadow(N, L) : 1.0;\n"
             "    vec3  direct = (diffuse + specular) * lightIntensity * NdotL * shadow;\n\n"
             "    float wrappedL   = clamp(NdotL * (1.0 - m.thickness) + m.thickness * 0.5, 0.0, 1.0);\n"
             "    vec3  subsurface = pow(wrappedL, max(m.subsurfacePower, 0.001))\n"
             "                       * m.subsurfaceColor * albedo * lightIntensity * shadow;\n\n"
             "    float iblStrength = _lighting.iblParams.x;\n"
             "    vec3 F_ind  = F_SchlickRoughness(NdotV, F0, roughness);\n"
             "    vec3 kD_ind = (1.0 - F_ind) * (1.0 - metallic);\n"
             "    vec3 irradiance = evaluateSH(N);\n"
             "    vec3 diffuseIBL = kD_ind * albedo * irradiance * iblStrength;\n\n"
             "    vec3 R = reflect(-V, N);\n"
             "    vec3 prefilteredColor = textureLod(_prefilteredEnvCube, R,\n"
             "        roughness * _lighting.iblParams.y).rgb;\n"
             "    vec2 brdf    = texture(_brdfLut, vec2(NdotV, roughness)).rg;\n"
             "    vec3 FssEss  = F_ind * brdf.x + brdf.y;\n"
             "    float Ess    = brdf.x + brdf.y;\n"
             "    vec3 Favg    = F0 + (1.0 - F0) / 21.0;\n"
             "    vec3 Fms     = FssEss * Favg * (1.0 - Ess) / (1.0 - Favg * (1.0 - Ess));\n"
             "    vec3 specularIBL = prefilteredColor * (FssEss + Fms) * iblStrength;\n\n"
             "    vec3 ambient = (diffuseIBL + specularIBL) * ao;\n"
             "    vec3 color   = direct + subsurface + ambient + m.emissive;\n"
             "    for (int _i = 0; _i < _pointLights.count; ++_i)\n"
             "        color += evalPointLight(inWorldPos, N, V, NdotV, albedo, F0, roughness, metallic,\n"
             "                               _pointLights.positionRange[_i], _pointLights.colorIntensity[_i]);\n"
             "    outColor = vec4(color, 1.0);\n"
             "}\n";

    } else if (isSpecGloss) {
        s += "void main() {\n"
             "    MaterialInputs m;\n"
             "    m.diffuseFactor = vec4(1.0);\n"
             "    m.specular      = vec3(0.04);\n"
             "    m.glossiness    = 0.5;\n"
             "    m.normal        = vec3(0.0, 0.0, 1.0);\n"
             "    m.emissive      = vec3(0.0);\n"
             "    material(m);\n\n"
             "    m.normal.xy *= _lighting.surfaceScales.y;\n"
             "    m.normal = normalize(m.normal);\n\n"
             "    float roughness = clamp(1.0 - m.glossiness, 0.04, 1.0);\n"
             "    vec3  F0        = m.specular;\n"
             "    float maxSpec   = max(max(m.specular.r, m.specular.g), m.specular.b);\n"
             "    float metallic  = clamp((maxSpec - 0.04) / 0.96, 0.0, 1.0);\n"
             "    vec3  albedo    = m.diffuseFactor.rgb * (1.0 - metallic);\n"
             "    float ao        = 1.0;\n\n";
        if (hasNormal)
            s += "    vec3 Nv = normalize(inNormal);\n";
        else
            s += "    vec3 Nv = vec3(0.0, 1.0, 0.0);\n";
        s += "    mat3 _tbn = derivativeTBN(inWorldPos, Nv, getUV0());\n"
             "    vec3 N = normalize(_tbn * m.normal);\n"
             "    vec3 V = normalize(frame.cameraPos.xyz - inWorldPos);\n"
             "    float NdotV = max(dot(N, V), 0.0);\n\n"
             "    vec3  L            = normalize(_lighting.lightDir.xyz);\n"
             "    float lightIntensity = _lighting.lightDir.w;\n"
             "    vec3  H     = normalize(V + L);\n"
             "    float NdotL = max(dot(N, L), 0.0);\n"
             "    float NdotH = max(dot(N, H), 0.0);\n"
             "    float HdotV = max(dot(H, V), 0.0);\n\n"
             "    vec3  Fdir   = F_Schlick(HdotV, F0);\n"
             "    float D = D_GGX(NdotH, roughness * roughness);\n"
             "    float G = G_Smith(NdotV, NdotL, roughness);\n"
             "    vec3  specular = (D * G) * Fdir / max(4.0 * NdotV * NdotL, 1e-4);\n"
             "    vec3  kD_dir   = (1.0 - Fdir) * (1.0 - metallic);\n"
             "    vec3  diffuse  = kD_dir * albedo * Fd_Burley(NdotV, NdotL, HdotV, roughness);\n\n"
             "    float shadow = _lighting.iblParams.z > 0.5 ? sampleShadow(N, L) : 1.0;\n"
             "    vec3  direct = (diffuse + specular) * lightIntensity * NdotL * shadow;\n\n"
             "    float iblStrength = _lighting.iblParams.x;\n"
             "    vec3 F_ind  = F_SchlickRoughness(NdotV, F0, roughness);\n"
             "    vec3 kD_ind = (1.0 - F_ind) * (1.0 - metallic);\n"
             "    vec3 irradiance = evaluateSH(N);\n"
             "    vec3 diffuseIBL = kD_ind * albedo * irradiance * iblStrength;\n\n"
             "    vec3 R = reflect(-V, N);\n"
             "    vec3 prefilteredColor = textureLod(_prefilteredEnvCube, R,\n"
             "        roughness * _lighting.iblParams.y).rgb;\n"
             "    vec2 brdf    = texture(_brdfLut, vec2(NdotV, roughness)).rg;\n"
             "    vec3 FssEss  = F_ind * brdf.x + brdf.y;\n"
             "    float Ess    = brdf.x + brdf.y;\n"
             "    vec3 Favg    = F0 + (1.0 - F0) / 21.0;\n"
             "    vec3 Fms     = FssEss * Favg * (1.0 - Ess) / (1.0 - Favg * (1.0 - Ess));\n"
             "    vec3 specularIBL = prefilteredColor * (FssEss + Fms) * iblStrength;\n\n"
             "    vec3 ambient = (diffuseIBL + specularIBL) * ao;\n"
             "    vec3 color   = direct + ambient + m.emissive;\n"
             "    for (int _i = 0; _i < _pointLights.count; ++_i)\n"
             "        color += evalPointLight(inWorldPos, N, V, NdotV, albedo, F0, roughness, metallic,\n"
             "                               _pointLights.positionRange[_i], _pointLights.colorIntensity[_i]);\n"
             "    outColor = vec4(color, 1.0);\n"
             "}\n";

    } else {
        // Lit — full PBR with Phase 4 extensions
        s += "void main() {\n"
             "    MaterialInputs m;\n"
             "    m.baseColor = vec4(1.0);\n"
             "    m.metallic  = 0.0;\n"
             "    m.roughness = 1.0;\n"
             "    m.ao        = 1.0;\n"
             "    m.normal    = vec3(0.0, 0.0, 1.0);\n"
             "    m.emissive  = vec3(0.0);\n"
             "#ifdef HAS_CLEARCOAT\n"
             "    m.clearcoat          = 0.0;\n"
             "    m.clearcoatRoughness = 0.5;\n"
             "    m.clearcoatNormal    = vec3(0.0, 0.0, 1.0);\n"
             "#endif\n"
             "#ifdef HAS_ANISOTROPY\n"
             "    m.anisotropy = 0.0;\n"
             "#endif\n"
             "#ifdef HAS_SHEEN\n"
             "    m.sheenColor     = vec3(0.0);\n"
             "    m.sheenRoughness = 0.5;\n"
             "#endif\n"
             "#ifdef HAS_REFRACTION\n"
             "    m.transmission = 0.0;\n"
             "    m.ior          = 1.5;\n"
             "    m.thickness    = 0.0;\n"
             "#endif\n"
             "    material(m);\n\n"
             "    m.ao *= _lighting.surfaceScales.x;\n"
             "    m.normal.xy *= _lighting.surfaceScales.y;\n"
             "    m.normal = normalize(m.normal);\n\n"
             "    vec3  albedo    = m.baseColor.rgb;\n"
             "    float metallic  = clamp(m.metallic,  0.0, 1.0);\n"
             "    float roughness = clamp(m.roughness, 0.04, 1.0);\n"
             "    float ao        = clamp(m.ao,        0.0, 1.0);\n\n";

        if (hasNormal)
            s += "    vec3 Nv = normalize(inNormal);\n";
        else
            s += "    vec3 Nv = vec3(0.0, 1.0, 0.0);\n";

        s += "    mat3 _tbn = derivativeTBN(inWorldPos, Nv, getUV0());\n"
             "    vec3 N = normalize(_tbn * m.normal);\n"
             "    vec3 V = normalize(frame.cameraPos.xyz - inWorldPos);\n"
             "    float NdotV = max(dot(N, V), 0.0);\n\n"
             "    vec3 F0 = mix(vec3(0.04), albedo, metallic);\n\n"
             "    vec3  L            = normalize(_lighting.lightDir.xyz);\n"
             "    float lightIntensity = _lighting.lightDir.w;\n"
             "    vec3  H     = normalize(V + L);\n"
             "    float NdotL = max(dot(N, L), 0.0);\n"
             "    float NdotH = max(dot(N, H), 0.0);\n"
             "    float HdotV = max(dot(H, V), 0.0);\n\n"
             "    vec3 F = F_Schlick(HdotV, F0);\n"
             "#ifdef HAS_ANISOTROPY\n"
             "    float at = max(roughness * roughness * (1.0 + m.anisotropy), 0.0001);\n"
             "    float ab = max(roughness * roughness * (1.0 - m.anisotropy), 0.0001);\n"
             "    vec3 T = _tbn[0]; vec3 B = _tbn[1];\n"
             "    float D = D_GGX_Anisotropic(NdotH, dot(T, H), dot(B, H), at, ab);\n"
             "    float Vis = V_SmithGGXCorrelated_Anisotropic(at, ab,\n"
             "        dot(T, V), dot(B, V), dot(T, L), dot(B, L), NdotV, NdotL);\n"
             "    vec3 specular = D * Vis * F;\n"
             "#else\n"
             "    float D = D_GGX(NdotH, roughness * roughness);\n"
             "    float G = G_Smith(NdotV, NdotL, roughness);\n"
             "    vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);\n"
             "#endif\n\n"
             "    vec3 kD_dir  = (1.0 - F) * (1.0 - metallic);\n"
             "    vec3 diffuse = kD_dir * albedo * Fd_Burley(NdotV, NdotL, HdotV, roughness);\n\n"
             "    float shadow = _lighting.iblParams.z > 0.5 ? sampleShadow(N, L) : 1.0;\n"
             "    vec3 direct = (diffuse + specular) * lightIntensity * NdotL * shadow;\n\n"
             "    float iblStrength = _lighting.iblParams.x;\n"
             "    vec3 F_ind  = F_SchlickRoughness(NdotV, F0, roughness);\n"
             "    vec3 kD_ind = (1.0 - F_ind) * (1.0 - metallic);\n"
             "    vec3 irradiance = evaluateSH(N);\n"
             "    vec3 diffuseIBL = kD_ind * albedo * irradiance * iblStrength;\n\n"
             "    vec3 R = reflect(-V, N);\n"
             "    vec3 prefilteredColor = textureLod(_prefilteredEnvCube, R,\n"
             "        roughness * _lighting.iblParams.y).rgb;\n"
             "    vec2 brdf    = texture(_brdfLut, vec2(NdotV, roughness)).rg;\n"
             "    vec3 FssEss  = F_ind * brdf.x + brdf.y;\n"
             "    float Ess    = brdf.x + brdf.y;\n"
             "    vec3 Favg    = F0 + (1.0 - F0) / 21.0;\n"
             "    vec3 Fms     = FssEss * Favg * (1.0 - Ess) / (1.0 - Favg * (1.0 - Ess));\n"
             "    vec3 specularIBL = prefilteredColor * (FssEss + Fms) * iblStrength;\n\n"
             "    vec3 ambient = (diffuseIBL + specularIBL) * ao;\n"
             "    vec3 color   = direct + ambient + m.emissive;\n\n"
             "#ifdef HAS_CLEARCOAT\n"
             "    float ccRoughness = clamp(m.clearcoatRoughness, 0.04, 1.0);\n"
             "    vec3  Ncc      = normalize(_tbn * m.clearcoatNormal);\n"
             "    float NdotH_cc = max(dot(Ncc, H), 0.0);\n"
             "    float NdotL_cc = max(dot(Ncc, L), 0.0);\n"
             "    float NdotV_cc = max(dot(Ncc, V), 0.0);\n"
             "    float D_cc = D_GGX(NdotH_cc, ccRoughness * ccRoughness);\n"
             "    float G_cc = G_Smith(NdotV_cc, NdotL_cc, ccRoughness);\n"
             "    float F_cc = 0.04 + 0.96 * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);\n"
             "    float Fc   = F_cc * m.clearcoat;\n"
             "    vec3  cc_direct = vec3((D_cc * G_cc * F_cc) / max(4.0 * NdotV_cc * NdotL_cc, 1e-4))\n"
             "                      * lightIntensity * NdotL_cc * shadow;\n"
             "    vec3  Rcc    = reflect(-V, Ncc);\n"
             "    vec3  prefCC = textureLod(_prefilteredEnvCube, Rcc, ccRoughness * _lighting.iblParams.y).rgb;\n"
             "    vec2  brdfCC = texture(_brdfLut, vec2(NdotV_cc, ccRoughness)).rg;\n"
             "    vec3  cc_IBL = prefCC * (0.04 * brdfCC.x + brdfCC.y) * iblStrength;\n"
             "    color = color * (1.0 - Fc) + (cc_direct + cc_IBL * Fc);\n"
             "#endif\n\n"
             "#ifdef HAS_SHEEN\n"
             "    float sr = max(m.sheenRoughness, 0.0001);\n"
             "    float Ds = D_Charlie(sr, NdotH);\n"
             "    float Vs = V_Ashikhmin(NdotL, NdotV);\n"
             "    vec3  sheenDirect = Ds * Vs * m.sheenColor * lightIntensity * NdotL * shadow;\n"
             "    vec3  sheenIBL = textureLod(_prefilteredEnvCube, R, sr * _lighting.iblParams.y).rgb\n"
             "                     * m.sheenColor * iblStrength;\n"
             "    float sheenScale = 1.0 - max(max(m.sheenColor.r, m.sheenColor.g), m.sheenColor.b) * ao;\n"
             "    color = color * sheenScale + sheenDirect + sheenIBL;\n"
             "#endif\n\n"
             "#ifdef HAS_REFRACTION\n"
             "    vec2 screenUV  = gl_FragCoord.xy / vec2(textureSize(_sceneCapture, 0));\n"
             "    vec2 refrUV    = clamp(screenUV + N.xy * (m.ior - 1.0) * m.thickness * 0.1,\n"
             "                          vec2(0.0), vec2(1.0));\n"
             "    vec3 refrColor = texture(_sceneCapture, refrUV).rgb * albedo;\n"
             "    color = mix(color, refrColor, clamp(m.transmission, 0.0, 1.0));\n"
             "#endif\n\n"
             "    for (int _i = 0; _i < _pointLights.count; ++_i)\n"
             "        color += evalPointLight(inWorldPos, N, V, NdotV, albedo, F0, roughness, metallic,\n"
             "                               _pointLights.positionRange[_i], _pointLights.colorIntensity[_i]);\n"
             "    outColor = vec4(color, 1.0);\n"
             "}\n";
    }

    return s;
}

} // namespace matc
