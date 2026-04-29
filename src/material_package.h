#pragma once

// Filament-style material package format.
//
// A single binary blob containing everything needed to instantiate a material
// at runtime: shading model, parameter table, sampler table, and pre-compiled
// SPIR-V for every variant the compiler emitted.
//
// File layout (little-endian, no padding gaps):
//
//   Header                 (36 bytes)
//   Material name          (Header::nameLen bytes, NOT null-terminated)
//   ParameterRecord[]      (Header::numParameters × 88 bytes)
//   SamplerRecord[]        (Header::numSamplers   × 76 bytes)
//   VariantRecord[]        (Header::numVariants   × 24 bytes)
//   SPIR-V blob pool       (variable; addressed by absolute file offsets in
//                           VariantRecord::{vert,frag}SpirvOffset)
//
// Names in records use a fixed-size char[64] buffer, null-terminated. Plenty
// for any reasonable parameter / sampler identifier.

#include <cstdint>
#include <string>
#include <vector>

namespace mat {

inline constexpr char     kMagic[4] = {'F', 'F', 'M', 'T'};
inline constexpr uint32_t kVersion  = 1;
inline constexpr uint32_t kBindingAuto = UINT32_MAX;

// ---- Enums ----

enum class ShadingModel : uint32_t {
    Lit                = 0,
    Unlit              = 1,
    Cloth              = 2,
    Subsurface         = 3,
    SpecularGlossiness = 4,
};

enum class ParameterType : uint32_t {
    Float  = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    Int    = 4,
    Bool   = 5,
};

enum class SamplerType : uint32_t {
    Sampler2D       = 0,
    Sampler2DSrgb   = 1,
    SamplerCube     = 2,
};

// ---- Variant key ----
//
// One bit per material- or pass-level feature flag. The compiler enumerates
// every reachable combination of bits and emits one (vert, frag) SPIR-V pair
// per combination.

enum VariantKey : uint32_t {
    VAR_HAS_VERTEX_NORMAL  = 1u << 0,
    VAR_HAS_VERTEX_TANGENT = 1u << 1,
    VAR_HAS_VERTEX_UV0     = 1u << 2,
    VAR_HAS_BASE_COLOR_MAP = 1u << 3,
    VAR_HAS_NORMAL_MAP     = 1u << 4,
    VAR_HAS_MR_MAP         = 1u << 5,
    VAR_HAS_CLEARCOAT      = 1u << 6,
    VAR_HAS_ANISOTROPY     = 1u << 7,
    VAR_HAS_SHEEN          = 1u << 8,
    VAR_HAS_REFRACTION     = 1u << 9,
    VAR_PASS_DEPTH_ONLY    = 1u << 11,
    VAR_PASS_TRANSPARENT   = 1u << 12,
};

// ---- In-memory descriptors (the editable form) ----

struct ParameterDesc {
    std::string   name;
    ParameterType type        = ParameterType::Float;
    uint32_t      uboOffset   = 0;       // byte offset into the parameter UBO
    float         defaults[4] = {0,0,0,0}; // up to 4 floats; unused slots are 0
};

struct SamplerDesc {
    std::string name;
    uint32_t    set     = 1;   // material instance set (set 0 is camera/lighting)
    uint32_t    binding = 0;
    SamplerType type    = SamplerType::Sampler2D;
};

struct VariantDesc {
    uint32_t              variantKey = 0;
    std::vector<uint32_t> vertSpirv;
    std::vector<uint32_t> fragSpirv;
};

struct MaterialPackage {
    std::string                name;
    ShadingModel               shadingModel = ShadingModel::Lit;
    uint32_t                   paramUboSize = 0;  // in bytes; multiple of 16
    std::vector<ParameterDesc> parameters;
    std::vector<SamplerDesc>   samplers;
    std::vector<VariantDesc>   variants;

    // Lookup helpers; return nullptr if not found.
    const ParameterDesc* findParameter(const std::string& n) const;
    const SamplerDesc*   findSampler  (const std::string& n) const;
    const VariantDesc*   findVariant  (uint32_t key)         const;

    // Returns true if the package is internally consistent.
    // On failure, writes a human-readable explanation to *outError (if non-null).
    bool validate(std::string* outError = nullptr) const;

    // I/O. Returns false on error and writes an explanation to stderr.
    bool        saveToFile  (const char* path) const;
    static bool loadFromFile(const char* path, MaterialPackage& out);
};

// ---- UBO layout helpers (std140 rules) ----

// Selects the compiled variant key for the given pass.
// For the depth/shadow pass returns VAR_PASS_DEPTH_ONLY (if compiled).
// For the color pass returns the first non-depth variant in the package.
// Returns 0 if no suitable variant exists.
uint32_t computeVariantKey(const MaterialPackage& pkg, bool depthOnly);

// Required byte alignment of `type` in a std140 UBO.
uint32_t uboTypeAlign(ParameterType type);

// Bytes consumed by `type` in a std140 UBO, including any trailing padding.
// Float3 returns 16 (not 12) because std140 pads vec3 to a vec4 stride.
uint32_t uboTypeStride(ParameterType type);

// ---- Builder ----
//
// Constructs a MaterialPackage incrementally. UBO offsets are assigned
// automatically following std140 alignment rules as parameters are added.
// Call build() to finalise; it validates and std::aborts on failure.
//
// Intended for use by M8.materials.2 (compiler): parse the material definition,
// call add* for each entity, then call addVariant for each compiled SPIR-V pair.
//
class PackageBuilder {
public:
    PackageBuilder& setName(std::string name);
    PackageBuilder& setShadingModel(ShadingModel sm);

    // Appends a parameter; auto-assigns a std140-aligned UBO offset.
    // Returns the assigned byte offset so the compiler can cross-reference it.
    // Pass up to 4 floats in `defaults`; unused trailing slots are zeroed.
    uint32_t addParameter(std::string name, ParameterType type,
                          const float* defaults = nullptr);

    // Appends a sampler. Pass binding=kBindingAuto to auto-increment per set.
    PackageBuilder& addSampler(std::string name, SamplerType type,
                               uint32_t set     = 1,
                               uint32_t binding = kBindingAuto);

    // Appends a compiled (vert, frag) SPIR-V pair for the given variant key.
    PackageBuilder& addVariant(uint32_t variantKey,
                               std::vector<uint32_t> vertSpirv,
                               std::vector<uint32_t> fragSpirv);

    // Pads paramUboSize to the next multiple of 16, validates, and returns
    // the finished package. Calls std::abort() if validation fails.
    MaterialPackage build();

private:
    MaterialPackage pkg_;
    uint32_t        nextUboOffset_ = 0;
    uint32_t        nextBinding_   = 0;
};

} // namespace mat
