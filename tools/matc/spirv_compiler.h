#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace matc {

// Compile a GLSL source string to SPIR-V words via glslangValidator.
// `stage` must be "vert" or "frag".
// Returns false on error; writes glslangValidator diagnostics to *outError.
bool compileGlsl(const std::string& source, const char* stage,
                 std::vector<uint32_t>& outSpirv,
                 std::string* outError = nullptr);

} // namespace matc
