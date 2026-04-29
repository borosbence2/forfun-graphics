// forfun_matc — Filament-style material compiler
//
// Usage: forfun_matc <input.mat> [-o output.fpkg]
//
// Without -o: parse the .mat and dump its contents (useful for debugging).
// With    -o: compile all variants to SPIR-V and write a binary .fpkg package.

#include "codegen.h"
#include "mat_description.h"
#include "mat_parser.h"
#include "material_package.h"
#include "spirv_compiler.h"

#include <cstdio>
#include <cstring>

// ---- Type conversions: matc:: → mat:: ----

static mat::ShadingModel toMatShadingModel(matc::ShadingModel sm) {
    switch (sm) {
        case matc::ShadingModel::Lit:                return mat::ShadingModel::Lit;
        case matc::ShadingModel::Unlit:              return mat::ShadingModel::Unlit;
        case matc::ShadingModel::Cloth:              return mat::ShadingModel::Cloth;
        case matc::ShadingModel::Subsurface:         return mat::ShadingModel::Subsurface;
        case matc::ShadingModel::SpecularGlossiness: return mat::ShadingModel::SpecularGlossiness;
    }
    return mat::ShadingModel::Lit;
}

static mat::ParameterType toMatParamType(matc::ParamType pt) {
    switch (pt) {
        case matc::ParamType::Float:  return mat::ParameterType::Float;
        case matc::ParamType::Float2: return mat::ParameterType::Float2;
        case matc::ParamType::Float3: return mat::ParameterType::Float3;
        case matc::ParamType::Float4: return mat::ParameterType::Float4;
        case matc::ParamType::Int:    return mat::ParameterType::Int;
        case matc::ParamType::Bool:   return mat::ParameterType::Bool;
    }
    return mat::ParameterType::Float;
}

static mat::SamplerType toMatSamplerType(matc::SamplerType st) {
    switch (st) {
        case matc::SamplerType::Sampler2D:     return mat::SamplerType::Sampler2D;
        case matc::SamplerType::Sampler2DSrgb: return mat::SamplerType::Sampler2DSrgb;
        case matc::SamplerType::SamplerCube:   return mat::SamplerType::SamplerCube;
    }
    return mat::SamplerType::Sampler2D;
}

// ---- Compile to package ----

static int compile(const matc::MaterialDescription& mat, const char* outputPath) {
    // Build the package metadata first (parameters, samplers, sampler bindings).
    // Samplers occupy set=1, bindings 1+ (binding 0 is the parameter UBO).
    mat::PackageBuilder builder;
    builder.setName(mat.name).setShadingModel(toMatShadingModel(mat.shadingModel));

    for (const auto& p : mat.parameters)
        builder.addParameter(p.name, toMatParamType(p.type), p.defaults);

    for (size_t i = 0; i < mat.samplers.size(); ++i)
        builder.addSampler(mat.samplers[i].name,
                           toMatSamplerType(mat.samplers[i].type),
                           1,
                           static_cast<uint32_t>(i + 1));

    // Compile each variant.
    const auto variantKeys = matc::enumerateVariants(mat);
    for (uint32_t key : variantKeys) {
        const std::string vertSrc = matc::generateVertGlsl(mat, key);
        const std::string fragSrc = matc::generateFragGlsl(mat, key);

        std::vector<uint32_t> vertSpv, fragSpv;
        std::string err;

        std::printf("  compiling variant 0x%04x vert ... ", key);
        std::fflush(stdout);
        if (!matc::compileGlsl(vertSrc, "vert", vertSpv, &err)) {
            std::printf("FAILED\n%s\n", err.c_str());
            return 1;
        }
        std::printf("ok (%zu words)\n", vertSpv.size());

        std::printf("  compiling variant 0x%04x frag ... ", key);
        std::fflush(stdout);
        if (!matc::compileGlsl(fragSrc, "frag", fragSpv, &err)) {
            std::printf("FAILED\n%s\n", err.c_str());
            return 1;
        }
        std::printf("ok (%zu words)\n", fragSpv.size());

        builder.addVariant(key, std::move(vertSpv), std::move(fragSpv));
    }

    mat::MaterialPackage pkg = builder.build();
    if (!pkg.saveToFile(outputPath)) {
        std::fprintf(stderr, "forfun_matc: failed to write %s\n", outputPath);
        return 1;
    }
    std::printf("Written: %s  (%zu variant(s), paramUboSize=%u)\n",
                outputPath, pkg.variants.size(), pkg.paramUboSize);
    return 0;
}

// ---- Entry point ----

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: forfun_matc <input.mat> [-o output.fpkg]\n");
        return 1;
    }
    const char* inputPath  = argv[1];
    const char* outputPath = nullptr;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "-o") == 0) outputPath = argv[i + 1];
    }

    matc::MaterialDescription mat;
    std::string err;
    if (!matc::parseMat(inputPath, mat, &err)) {
        std::fprintf(stderr, "forfun_matc: parse error: %s\n", err.c_str());
        return 1;
    }

    if (!outputPath) {
        // Dump mode — parse verification only.
        std::printf("%-16s %s\n", "name:",         mat.name.c_str());
        std::printf("%-16s %s\n", "shadingModel:", toString(mat.shadingModel));
        std::printf("%-16s %s\n", "blendMode:",    toString(mat.blendMode));

        if (!mat.parameters.empty()) {
            std::printf("parameters (%zu):\n", mat.parameters.size());
            for (const auto& p : mat.parameters)
                std::printf("  %-24s  %-8s  default=[%.3f, %.3f, %.3f, %.3f]\n",
                            p.name.c_str(), toString(p.type),
                            p.defaults[0], p.defaults[1],
                            p.defaults[2], p.defaults[3]);
        }
        if (!mat.samplers.empty()) {
            std::printf("samplers (%zu):\n", mat.samplers.size());
            for (const auto& s : mat.samplers)
                std::printf("  %-24s  %s\n", s.name.c_str(), toString(s.type));
        }
        if (!mat.requires_.empty()) {
            std::printf("requires:");
            for (auto a : mat.requires_) std::printf(" %s", toString(a));
            std::printf("\n");
        }
        std::printf("fragmentBlock: %zu chars\n", mat.fragmentBlock.size());
        return 0;
    }

    return compile(mat, outputPath);
}
