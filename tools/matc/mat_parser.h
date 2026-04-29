#pragma once

#include "mat_description.h"
#include <string>

namespace matc {

// Parse a JSON .mat file at `path` into `out`.
// Returns false on error; writes a human-readable message to *outError.
bool parseMat(const char* path, MaterialDescription& out,
              std::string* outError = nullptr);

} // namespace matc
