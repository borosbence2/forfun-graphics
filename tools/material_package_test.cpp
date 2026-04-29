// Round-trip test for MaterialPackage serialization.
//
// Builds a populated package in memory, writes it to disk, reads it back,
// then compares everything field-by-field. Exits 0 on success, 1 on the
// first mismatch.

#include "material_package.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(int argc, char** argv) {
    using namespace mat;

    // ---- Build the source package ----
    MaterialPackage src;
    src.name         = "DamagedHelmet";
    src.shadingModel = ShadingModel::Lit;
    src.paramUboSize = 32;  // 2 × vec4

    src.parameters = {
        {"baseColorFactor", ParameterType::Float4, 0,  {1.0f, 1.0f, 1.0f, 1.0f}},
        {"metallicFactor",  ParameterType::Float,  16, {1.0f, 0.0f, 0.0f, 0.0f}},
        {"roughnessFactor", ParameterType::Float,  20, {1.0f, 0.0f, 0.0f, 0.0f}},
    };

    src.samplers = {
        {"baseColorMap", 1, 0, SamplerType::Sampler2DSrgb},
        {"normalMap",    1, 1, SamplerType::Sampler2D},
    };

    // Stub SPIR-V — distinguishable per variant + stage, never executed.
    src.variants = {
        VariantDesc{
            VAR_HAS_VERTEX_NORMAL | VAR_HAS_VERTEX_UV0 | VAR_HAS_BASE_COLOR_MAP,
            {0x07230203u, 0x00010000u, 0x11110001u, 0x00000000u},          // "vert blob 1"
            {0x07230203u, 0x00010000u, 0x22220001u, 0x00000000u, 0xCAFEBABEu}, // "frag blob 1"
        },
        VariantDesc{
            VAR_PASS_DEPTH_ONLY,
            {0x07230203u, 0x00010000u, 0x11110002u},                       // "vert blob 2"
            {0x07230203u, 0x00010000u, 0x22220002u},                       // "frag blob 2"
        },
    };

    // ---- Save to a temp file ----
    const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
    const std::filesystem::path tmpFile = tmpDir / "forfun_material_roundtrip.fpkg";
    const std::string tmpFileStr = tmpFile.string();

    std::printf("Writing test package to %s ...\n", tmpFileStr.c_str());
    CHECK(src.saveToFile(tmpFileStr.c_str()), "saveToFile failed");

    // ---- Load back ----
    MaterialPackage dst;
    CHECK(MaterialPackage::loadFromFile(tmpFileStr.c_str(), dst),
          "loadFromFile failed");

    // ---- Compare top-level ----
    CHECK(dst.name         == src.name,         "name mismatch");
    CHECK(dst.shadingModel == src.shadingModel, "shadingModel mismatch");
    CHECK(dst.paramUboSize == src.paramUboSize, "paramUboSize mismatch");

    // ---- Parameters ----
    CHECK(dst.parameters.size() == src.parameters.size(), "parameter count mismatch");
    for (size_t i = 0; i < src.parameters.size(); ++i) {
        const auto& a = src.parameters[i];
        const auto& b = dst.parameters[i];
        CHECK(a.name == b.name,           "parameter name mismatch");
        CHECK(a.type == b.type,           "parameter type mismatch");
        CHECK(a.uboOffset == b.uboOffset, "parameter uboOffset mismatch");
        CHECK(std::memcmp(a.defaults, b.defaults, sizeof(a.defaults)) == 0,
              "parameter defaults mismatch");
    }

    // ---- Samplers ----
    CHECK(dst.samplers.size() == src.samplers.size(), "sampler count mismatch");
    for (size_t i = 0; i < src.samplers.size(); ++i) {
        const auto& a = src.samplers[i];
        const auto& b = dst.samplers[i];
        CHECK(a.name    == b.name,    "sampler name mismatch");
        CHECK(a.set     == b.set,     "sampler set mismatch");
        CHECK(a.binding == b.binding, "sampler binding mismatch");
        CHECK(a.type    == b.type,    "sampler type mismatch");
    }

    // ---- Variants + SPIR-V ----
    CHECK(dst.variants.size() == src.variants.size(), "variant count mismatch");
    for (size_t i = 0; i < src.variants.size(); ++i) {
        const auto& a = src.variants[i];
        const auto& b = dst.variants[i];
        CHECK(a.variantKey == b.variantKey, "variant key mismatch");
        CHECK(a.vertSpirv  == b.vertSpirv,  "vert SPIR-V mismatch");
        CHECK(a.fragSpirv  == b.fragSpirv,  "frag SPIR-V mismatch");
    }

    // ---- Lookup helpers ----
    CHECK(dst.findParameter("metallicFactor")  != nullptr, "findParameter(name) missed");
    CHECK(dst.findParameter("doesnotexist")    == nullptr, "findParameter(missing) hit");
    CHECK(dst.findSampler  ("normalMap")       != nullptr, "findSampler(name) missed");
    CHECK(dst.findVariant  (VAR_PASS_DEPTH_ONLY) != nullptr,
          "findVariant(key) missed");

    // ---- Cleanup ----
    std::error_code ec;
    std::filesystem::remove(tmpFile, ec);

    std::printf("MaterialPackage round-trip: OK\n");

    // ---- PackageBuilder: UBO offset assignment ----
    {
        PackageBuilder b;
        b.setName("TestMaterial").setShadingModel(ShadingModel::Lit);

        // float4 at 0, float at 16, float at 20 — no gaps needed.
        uint32_t off0 = b.addParameter("baseColorFactor", ParameterType::Float4);
        uint32_t off1 = b.addParameter("metallicFactor",  ParameterType::Float);
        uint32_t off2 = b.addParameter("roughnessFactor", ParameterType::Float);
        CHECK(off0 == 0,  "baseColorFactor offset should be 0");
        CHECK(off1 == 16, "metallicFactor offset should be 16");
        CHECK(off2 == 20, "roughnessFactor offset should be 20");

        b.addSampler("baseColorMap", SamplerType::Sampler2DSrgb);
        b.addSampler("normalMap",    SamplerType::Sampler2D);
        b.addVariant(VAR_HAS_VERTEX_NORMAL,
                     {0x07230203u, 0x00010000u},
                     {0x07230203u, 0x00010000u});

        MaterialPackage pkg = b.build();

        CHECK(pkg.name              == "TestMaterial",   "builder: name");
        CHECK(pkg.shadingModel      == ShadingModel::Lit,"builder: shadingModel");
        CHECK(pkg.paramUboSize      == 32u,              "builder: paramUboSize");
        CHECK(pkg.parameters.size() == 3,                "builder: parameter count");
        CHECK(pkg.samplers.size()   == 2,                "builder: sampler count");
        CHECK(pkg.variants.size()   == 1,                "builder: variant count");

        CHECK(pkg.samplers[0].binding == 0, "builder: auto-binding 0");
        CHECK(pkg.samplers[1].binding == 1, "builder: auto-binding 1");

        // validate should pass on a well-formed built package
        std::string err;
        CHECK(pkg.validate(&err), ("builder: validate failed: " + err).c_str());
    }
    std::printf("PackageBuilder: OK\n");

    // ---- PackageBuilder: std140 vec3 alignment gap ----
    {
        PackageBuilder b;
        b.setName("AlignTest").setShadingModel(ShadingModel::Unlit);

        uint32_t off0 = b.addParameter("f",  ParameterType::Float);   // 0
        uint32_t off1 = b.addParameter("v3", ParameterType::Float3);  // 16 (gap 4..15)
        uint32_t off2 = b.addParameter("f2", ParameterType::Float);   // 32

        CHECK(off0 == 0,  "align: float at 0");
        CHECK(off1 == 16, "align: float3 must jump to 16 (std140 vec3 align)");
        CHECK(off2 == 32, "align: float after float3 stride of 16");

        MaterialPackage pkg = b.build();
        CHECK(pkg.paramUboSize == 48u, "align: paramUboSize padded to 48");

        std::string err;
        CHECK(pkg.validate(&err), ("align: validate failed: " + err).c_str());
    }
    std::printf("PackageBuilder alignment: OK\n");

    // ---- validate(): catches bad inputs ----
    {
        std::string err;

        // Empty name
        {
            MaterialPackage bad;
            CHECK(!bad.validate(&err), "validate: should reject empty name");
        }

        // paramUboSize not multiple of 16
        {
            MaterialPackage bad;
            bad.name         = "X";
            bad.paramUboSize = 15;
            CHECK(!bad.validate(&err), "validate: should reject non-aligned UBO size");
        }

        // Duplicate parameter name
        {
            MaterialPackage bad;
            bad.name         = "X";
            bad.paramUboSize = 16;
            bad.parameters   = {{"dup", ParameterType::Float, 0, {}},
                                 {"dup", ParameterType::Float, 4, {}}};
            CHECK(!bad.validate(&err), "validate: should reject duplicate param name");
        }

        // UBO overlap: two floats at offset 0
        {
            MaterialPackage bad;
            bad.name         = "X";
            bad.paramUboSize = 16;
            bad.parameters   = {{"a", ParameterType::Float, 0, {}},
                                 {"b", ParameterType::Float, 0, {}}};
            CHECK(!bad.validate(&err), "validate: should reject overlapping UBO params");
        }

        // Parameter extends past paramUboSize
        {
            MaterialPackage bad;
            bad.name         = "X";
            bad.paramUboSize = 0;
            bad.parameters   = {{"a", ParameterType::Float, 0, {}}};
            CHECK(!bad.validate(&err), "validate: should reject param past UBO end");
        }

        // Duplicate sampler name
        {
            MaterialPackage bad;
            bad.name         = "X";
            bad.paramUboSize = 0;
            bad.samplers     = {{"s", 1, 0, SamplerType::Sampler2D},
                                 {"s", 1, 1, SamplerType::Sampler2D}};
            CHECK(!bad.validate(&err), "validate: should reject duplicate sampler name");
        }

        // Duplicate variant key
        {
            MaterialPackage bad;
            bad.name         = "X";
            bad.paramUboSize = 0;
            bad.variants     = {VariantDesc{1u, {}, {}}, VariantDesc{1u, {}, {}}};
            CHECK(!bad.validate(&err), "validate: should reject duplicate variant key");
        }
    }
    std::printf("validate() error cases: OK\n");

    (void)argc; (void)argv;
    return 0;
}
