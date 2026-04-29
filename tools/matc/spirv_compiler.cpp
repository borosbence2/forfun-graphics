#include "spirv_compiler.h"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// GLSLANG_VALIDATOR_PATH is injected by CMake as a compile definition.
#ifndef GLSLANG_VALIDATOR_PATH
#  error "GLSLANG_VALIDATOR_PATH must be defined by CMake (see target_compile_definitions)"
#endif

namespace matc {

// FNV-64 hash of the GLSL source. Changing any byte (including variant
// #defines) produces a different hash, so no explicit invalidation needed.
// If glslang itself is upgraded, delete mat_cache/ to force a full rebuild.
static uint64_t fnv64(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s)
        h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    return h;
}

static std::filesystem::path cacheDir() {
    return std::filesystem::path("mat_cache");
}

static std::filesystem::path cachePath(uint64_t hash) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return cacheDir() / (std::string(hex) + ".spv");
}

bool compileGlsl(const std::string& source, const char* stage,
                 std::vector<uint32_t>& outSpirv,
                 std::string* outError) {
    namespace fs = std::filesystem;

    auto fail = [&](std::string msg) -> bool {
        if (outError) *outError = std::move(msg);
        return false;
    };

    // ---- Cache lookup ----
    const uint64_t hash = fnv64(source);
    const fs::path spvCache = cachePath(hash);

    if (fs::exists(spvCache)) {
        std::ifstream cf(spvCache, std::ios::binary | std::ios::ate);
        if (cf) {
            const auto sz = static_cast<size_t>(cf.tellg());
            if (sz > 0 && sz % sizeof(uint32_t) == 0) {
                cf.seekg(0);
                outSpirv.resize(sz / sizeof(uint32_t));
                cf.read(reinterpret_cast<char*>(outSpirv.data()),
                        static_cast<std::streamsize>(sz));
                return true;
            }
        }
        // Corrupt cache entry — fall through to recompile.
        fs::remove(spvCache);
    }

    // ---- Compile ----
    static uint32_t counter = 0;
    const std::string base = "forfun_matc_" + std::to_string(++counter);
    const fs::path tmp     = fs::temp_directory_path();
    const fs::path srcFile = tmp / (base + "." + stage);
    const fs::path spvFile = tmp / (base + ".spv");

    {
        std::ofstream f(srcFile);
        if (!f) return fail("cannot write temp file: " + srcFile.string());
        f << source;
    }

    const std::string cmd =
        "\"" GLSLANG_VALIDATOR_PATH "\""
        " -V \"" + srcFile.string() + "\""
        " -o \"" + spvFile.string() + "\"";

#ifdef _WIN32
    FILE* pipe = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) {
        fs::remove(srcFile);
        return fail("failed to spawn glslangValidator");
    }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        output += buf;

#ifdef _WIN32
    const int exitCode = _pclose(pipe);
#else
    const int exitCode = pclose(pipe);
#endif

    fs::remove(srcFile);

    if (exitCode != 0) {
        fs::remove(spvFile);
        return fail("glslangValidator failed for " + std::string(stage) + ":\n" + output);
    }

    std::ifstream sf(spvFile, std::ios::binary | std::ios::ate);
    if (!sf) {
        fs::remove(spvFile);
        return fail("cannot read SPIR-V output: " + spvFile.string());
    }
    const auto sz = static_cast<size_t>(sf.tellg());
    if (sz % sizeof(uint32_t) != 0) {
        fs::remove(spvFile);
        return fail("SPIR-V output size is not 4-byte aligned");
    }
    sf.seekg(0);
    outSpirv.resize(sz / sizeof(uint32_t));
    sf.read(reinterpret_cast<char*>(outSpirv.data()), static_cast<std::streamsize>(sz));
    sf.close();
    fs::remove(spvFile);

    // ---- Write to cache ----
    std::error_code ec;
    fs::create_directories(cacheDir(), ec); // no-op if already exists
    if (!ec) {
        std::ofstream wf(spvCache, std::ios::binary);
        if (wf)
            wf.write(reinterpret_cast<const char*>(outSpirv.data()),
                     static_cast<std::streamsize>(outSpirv.size() * sizeof(uint32_t)));
    }

    return true;
}

} // namespace matc
