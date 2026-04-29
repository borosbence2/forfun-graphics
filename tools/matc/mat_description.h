#pragma once

#include <string>
#include <vector>

namespace matc {

enum class ShadingModel { Lit, Unlit, Cloth, Subsurface, SpecularGlossiness };
enum class BlendMode    { Opaque, Transparent, Add, Masked };
enum class ParamType    { Float, Float2, Float3, Float4, Int, Bool };
enum class SamplerType  { Sampler2D, Sampler2DSrgb, SamplerCube };
enum class VertexAttr   { Position, Normal, Tangent, UV0, UV1, Color };

struct ParameterDef {
    std::string name;
    ParamType   type        = ParamType::Float;
    float       defaults[4] = {};
};

struct SamplerDef {
    std::string name;
    SamplerType type = SamplerType::Sampler2D;
};

// Parsed in-memory representation of a .mat source file.
// This is the compiler's input; MaterialPackage is its output.
struct MaterialDescription {
    std::string               name;
    ShadingModel              shadingModel = ShadingModel::Lit;
    BlendMode                 blendMode    = BlendMode::Opaque;
    std::vector<ParameterDef> parameters;
    std::vector<SamplerDef>   samplers;
    std::vector<VertexAttr>   requires_;    // trailing _ avoids keyword clash
    std::string               vertexBlock;   // optional GLSL user function
    std::string               fragmentBlock; // required — the material() function
};

// Diagnostic string conversions.
inline const char* toString(ShadingModel v) {
    switch (v) {
        case ShadingModel::Lit:                return "lit";
        case ShadingModel::Unlit:              return "unlit";
        case ShadingModel::Cloth:              return "cloth";
        case ShadingModel::Subsurface:         return "subsurface";
        case ShadingModel::SpecularGlossiness: return "specularGlossiness";
    }
    return "unknown";
}

inline const char* toString(BlendMode v) {
    switch (v) {
        case BlendMode::Opaque:      return "opaque";
        case BlendMode::Transparent: return "transparent";
        case BlendMode::Add:         return "add";
        case BlendMode::Masked:      return "masked";
    }
    return "unknown";
}

inline const char* toString(ParamType v) {
    switch (v) {
        case ParamType::Float:  return "float";
        case ParamType::Float2: return "float2";
        case ParamType::Float3: return "float3";
        case ParamType::Float4: return "float4";
        case ParamType::Int:    return "int";
        case ParamType::Bool:   return "bool";
    }
    return "unknown";
}

inline const char* toString(SamplerType v) {
    switch (v) {
        case SamplerType::Sampler2D:     return "sampler2d";
        case SamplerType::Sampler2DSrgb: return "sampler2d_srgb";
        case SamplerType::SamplerCube:   return "samplercube";
    }
    return "unknown";
}

inline const char* toString(VertexAttr v) {
    switch (v) {
        case VertexAttr::Position: return "POSITION";
        case VertexAttr::Normal:   return "NORMAL";
        case VertexAttr::Tangent:  return "TANGENT";
        case VertexAttr::UV0:      return "UV0";
        case VertexAttr::UV1:      return "UV1";
        case VertexAttr::Color:    return "COLOR";
    }
    return "unknown";
}

} // namespace matc
