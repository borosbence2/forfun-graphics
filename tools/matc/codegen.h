#pragma once

#include "mat_description.h"
#include "material_package.h"   // for VariantKey constants

#include <cstdint>
#include <string>
#include <vector>

namespace matc {

// Returns the base variant key implied by a material's declared features.
uint32_t computeBaseVariantKey(const MaterialDescription& mat);

// Returns all variant keys to compile for this material.
// Currently emits { baseKey, VAR_PASS_DEPTH_ONLY }.
std::vector<uint32_t> enumerateVariants(const MaterialDescription& mat);

// Build complete vertex GLSL for the given variant.
std::string generateVertGlsl(const MaterialDescription& mat, uint32_t variantKey);

// Build complete fragment GLSL for the given variant.
std::string generateFragGlsl(const MaterialDescription& mat, uint32_t variantKey);

// GLSL type string for a parameter as it appears inside a std140 uniform block.
// (Bool → "int"; bool is disallowed by std140 so we use int instead.)
const char* uboGlslType(ParamType type);

} // namespace matc
