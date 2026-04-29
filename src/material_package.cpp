#include "material_package.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <vector>

namespace mat {

// ---- On-disk record types (POD, fixed-size) ----

namespace {

#pragma pack(push, 1)

struct Header {
    char     magic[4];
    uint32_t version;
    uint32_t flags;
    uint32_t shadingModel;
    uint32_t paramUboSize;
    uint32_t nameLen;
    uint32_t numParameters;
    uint32_t numSamplers;
    uint32_t numVariants;
};
static_assert(sizeof(Header) == 36, "Header must be 36 bytes");

struct ParameterRecord {
    char     name[64];
    uint32_t type;
    uint32_t uboOffset;
    float    defaults[4];
};
static_assert(sizeof(ParameterRecord) == 88, "ParameterRecord must be 88 bytes");

struct SamplerRecord {
    char     name[64];
    uint32_t set;
    uint32_t binding;
    uint32_t type;
};
static_assert(sizeof(SamplerRecord) == 76, "SamplerRecord must be 76 bytes");

struct VariantRecord {
    uint32_t variantKey;
    uint32_t _pad0;          // align next u32 pair to 8 — nothing semantic
    uint32_t vertSpirvOffset;
    uint32_t vertSpirvSize;
    uint32_t fragSpirvOffset;
    uint32_t fragSpirvSize;
};
static_assert(sizeof(VariantRecord) == 24, "VariantRecord must be 24 bytes");

#pragma pack(pop)

void copyName(char (&dst)[64], const std::string& src) {
    std::memset(dst, 0, sizeof(dst));
    const size_t n = std::min(src.size(), sizeof(dst) - 1);
    std::memcpy(dst, src.data(), n);
}

} // anonymous

// ---- Lookup helpers ----

const ParameterDesc* MaterialPackage::findParameter(const std::string& n) const {
    for (const auto& p : parameters) if (p.name == n) return &p;
    return nullptr;
}

const SamplerDesc* MaterialPackage::findSampler(const std::string& n) const {
    for (const auto& s : samplers) if (s.name == n) return &s;
    return nullptr;
}

const VariantDesc* MaterialPackage::findVariant(uint32_t key) const {
    for (const auto& v : variants) if (v.variantKey == key) return &v;
    return nullptr;
}

// ---- Serialization ----

bool MaterialPackage::saveToFile(const char* path) const {
    if (name.size() > 0xFFFFFFFFu) {
        std::fprintf(stderr, "MaterialPackage::saveToFile: name too long\n");
        return false;
    }

    Header header{};
    std::memcpy(header.magic, kMagic, 4);
    header.version       = kVersion;
    header.flags         = 0;
    header.shadingModel  = static_cast<uint32_t>(shadingModel);
    header.paramUboSize  = paramUboSize;
    header.nameLen       = static_cast<uint32_t>(name.size());
    header.numParameters = static_cast<uint32_t>(parameters.size());
    header.numSamplers   = static_cast<uint32_t>(samplers.size());
    header.numVariants   = static_cast<uint32_t>(variants.size());

    // Compute the SPIR-V pool offset (start byte in the file).
    const uint32_t headerEnd     = sizeof(Header);
    const uint32_t nameEnd       = headerEnd + header.nameLen;
    const uint32_t paramTableEnd = nameEnd       + header.numParameters * sizeof(ParameterRecord);
    const uint32_t samplerTableEnd = paramTableEnd + header.numSamplers   * sizeof(SamplerRecord);
    const uint32_t variantTableEnd = samplerTableEnd + header.numVariants  * sizeof(VariantRecord);
    const uint32_t poolStart       = variantTableEnd;

    // Build variant records with absolute file offsets into the pool.
    std::vector<VariantRecord> variantRecords(variants.size());
    std::vector<uint32_t>      pool;        // SPIR-V words appended sequentially
    uint32_t                   poolBytes = 0;

    auto appendBlob = [&](const std::vector<uint32_t>& spv,
                          uint32_t& outOffset, uint32_t& outSize) {
        outOffset = poolStart + poolBytes;
        outSize   = static_cast<uint32_t>(spv.size() * sizeof(uint32_t));
        pool.insert(pool.end(), spv.begin(), spv.end());
        poolBytes += outSize;
    };

    for (size_t i = 0; i < variants.size(); ++i) {
        VariantRecord& r = variantRecords[i];
        r.variantKey = variants[i].variantKey;
        r._pad0      = 0;
        appendBlob(variants[i].vertSpirv, r.vertSpirvOffset, r.vertSpirvSize);
        appendBlob(variants[i].fragSpirv, r.fragSpirvOffset, r.fragSpirvSize);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "MaterialPackage::saveToFile: cannot open %s\n", path);
        return false;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(Header));
    out.write(name.data(), name.size());

    for (const auto& p : parameters) {
        ParameterRecord r{};
        copyName(r.name, p.name);
        r.type      = static_cast<uint32_t>(p.type);
        r.uboOffset = p.uboOffset;
        std::memcpy(r.defaults, p.defaults, sizeof(r.defaults));
        out.write(reinterpret_cast<const char*>(&r), sizeof(r));
    }

    for (const auto& s : samplers) {
        SamplerRecord r{};
        copyName(r.name, s.name);
        r.set     = s.set;
        r.binding = s.binding;
        r.type    = static_cast<uint32_t>(s.type);
        out.write(reinterpret_cast<const char*>(&r), sizeof(r));
    }

    for (const auto& r : variantRecords) {
        out.write(reinterpret_cast<const char*>(&r), sizeof(r));
    }

    out.write(reinterpret_cast<const char*>(pool.data()),
              static_cast<std::streamsize>(pool.size() * sizeof(uint32_t)));

    if (!out) {
        std::fprintf(stderr, "MaterialPackage::saveToFile: write error on %s\n", path);
        return false;
    }
    return true;
}

bool MaterialPackage::loadFromFile(const char* path, MaterialPackage& out) {
    out = {};

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "MaterialPackage::loadFromFile: cannot open %s\n", path);
        return false;
    }
    const std::streamsize fileSize = in.tellg();
    if (fileSize < static_cast<std::streamsize>(sizeof(Header))) {
        std::fprintf(stderr, "MaterialPackage::loadFromFile: %s is too small\n", path);
        return false;
    }
    in.seekg(0);

    std::vector<char> blob(static_cast<size_t>(fileSize));
    in.read(blob.data(), fileSize);
    if (!in) {
        std::fprintf(stderr, "MaterialPackage::loadFromFile: read error on %s\n", path);
        return false;
    }

    auto take = [&](size_t offset, size_t bytes) -> const char* {
        if (offset + bytes > blob.size()) return nullptr;
        return blob.data() + offset;
    };

    const Header* header = reinterpret_cast<const Header*>(blob.data());
    if (std::memcmp(header->magic, kMagic, 4) != 0) {
        std::fprintf(stderr, "MaterialPackage::loadFromFile: %s has wrong magic\n", path);
        return false;
    }
    if (header->version != kVersion) {
        std::fprintf(stderr,
            "MaterialPackage::loadFromFile: %s has version %u, expected %u\n",
            path, header->version, kVersion);
        return false;
    }

    out.shadingModel = static_cast<ShadingModel>(header->shadingModel);
    out.paramUboSize = header->paramUboSize;

    size_t cursor = sizeof(Header);

    // Material name
    {
        const char* p = take(cursor, header->nameLen);
        if (!p) {
            std::fprintf(stderr, "MaterialPackage::loadFromFile: truncated name\n");
            return false;
        }
        out.name.assign(p, header->nameLen);
        cursor += header->nameLen;
    }

    // Parameter table
    out.parameters.resize(header->numParameters);
    for (uint32_t i = 0; i < header->numParameters; ++i) {
        const char* p = take(cursor, sizeof(ParameterRecord));
        if (!p) {
            std::fprintf(stderr, "MaterialPackage::loadFromFile: truncated param table\n");
            return false;
        }
        const auto* r = reinterpret_cast<const ParameterRecord*>(p);
        ParameterDesc& dst = out.parameters[i];
        dst.name      = std::string(r->name);
        dst.type      = static_cast<ParameterType>(r->type);
        dst.uboOffset = r->uboOffset;
        std::memcpy(dst.defaults, r->defaults, sizeof(dst.defaults));
        cursor += sizeof(ParameterRecord);
    }

    // Sampler table
    out.samplers.resize(header->numSamplers);
    for (uint32_t i = 0; i < header->numSamplers; ++i) {
        const char* p = take(cursor, sizeof(SamplerRecord));
        if (!p) {
            std::fprintf(stderr, "MaterialPackage::loadFromFile: truncated sampler table\n");
            return false;
        }
        const auto* r = reinterpret_cast<const SamplerRecord*>(p);
        SamplerDesc& dst = out.samplers[i];
        dst.name    = std::string(r->name);
        dst.set     = r->set;
        dst.binding = r->binding;
        dst.type    = static_cast<SamplerType>(r->type);
        cursor += sizeof(SamplerRecord);
    }

    // Variant table — referenced into SPIR-V pool by absolute file offsets.
    out.variants.resize(header->numVariants);
    for (uint32_t i = 0; i < header->numVariants; ++i) {
        const char* p = take(cursor, sizeof(VariantRecord));
        if (!p) {
            std::fprintf(stderr, "MaterialPackage::loadFromFile: truncated variant table\n");
            return false;
        }
        const auto* r = reinterpret_cast<const VariantRecord*>(p);
        VariantDesc& dst = out.variants[i];
        dst.variantKey = r->variantKey;

        if (r->vertSpirvSize % sizeof(uint32_t) != 0 ||
            r->fragSpirvSize % sizeof(uint32_t) != 0) {
            std::fprintf(stderr,
                "MaterialPackage::loadFromFile: SPIR-V size not 4-byte aligned\n");
            return false;
        }

        const char* vert = take(r->vertSpirvOffset, r->vertSpirvSize);
        const char* frag = take(r->fragSpirvOffset, r->fragSpirvSize);
        if (!vert || !frag) {
            std::fprintf(stderr,
                "MaterialPackage::loadFromFile: SPIR-V blob outside file\n");
            return false;
        }
        dst.vertSpirv.assign(
            reinterpret_cast<const uint32_t*>(vert),
            reinterpret_cast<const uint32_t*>(vert + r->vertSpirvSize));
        dst.fragSpirv.assign(
            reinterpret_cast<const uint32_t*>(frag),
            reinterpret_cast<const uint32_t*>(frag + r->fragSpirvSize));
        cursor += sizeof(VariantRecord);
    }

    return true;
}

// ---- computeVariantKey() ----

uint32_t computeVariantKey(const MaterialPackage& pkg, bool depthOnly) {
    if (depthOnly) {
        // The compiler always emits an explicit VAR_PASS_DEPTH_ONLY variant.
        return (pkg.findVariant(VAR_PASS_DEPTH_ONLY) != nullptr)
                   ? static_cast<uint32_t>(VAR_PASS_DEPTH_ONLY)
                   : 0u;
    }
    // Return the first non-depth variant that was compiled.
    for (const auto& v : pkg.variants) {
        if (!(v.variantKey & VAR_PASS_DEPTH_ONLY)) return v.variantKey;
    }
    return 0u;
}

// ---- validate() ----

bool MaterialPackage::validate(std::string* outError) const {
    auto fail = [&](std::string msg) -> bool {
        if (outError) *outError = std::move(msg);
        return false;
    };

    if (name.empty())
        return fail("name is empty");
    if (paramUboSize % 16 != 0)
        return fail("paramUboSize must be a multiple of 16 (std140)");

    // Parameters
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto& p = parameters[i];
        if (p.name.empty())
            return fail("parameter[" + std::to_string(i) + "] has empty name");
        if (p.name.size() > 63)
            return fail("parameter '" + p.name + "' name exceeds 63 chars");
        for (size_t j = i + 1; j < parameters.size(); ++j) {
            if (parameters[j].name == p.name)
                return fail("duplicate parameter name '" + p.name + "'");
        }
        const uint32_t align  = uboTypeAlign(p.type);
        const uint32_t stride = uboTypeStride(p.type);
        if (p.uboOffset % align != 0)
            return fail("parameter '" + p.name + "' uboOffset not aligned to "
                        + std::to_string(align));
        if (p.uboOffset + stride > paramUboSize)
            return fail("parameter '" + p.name + "' extends past paramUboSize");
    }

    // UBO overlap: sort by offset, check no two parameters occupy the same bytes.
    if (parameters.size() > 1) {
        std::vector<size_t> order(parameters.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return parameters[a].uboOffset < parameters[b].uboOffset;
        });
        for (size_t i = 0; i + 1 < order.size(); ++i) {
            const auto& a = parameters[order[i]];
            const auto& b = parameters[order[i + 1]];
            if (b.uboOffset < a.uboOffset + uboTypeStride(a.type))
                return fail("parameters '" + a.name + "' and '" + b.name
                            + "' overlap in the UBO");
        }
    }

    // Samplers
    for (size_t i = 0; i < samplers.size(); ++i) {
        const auto& s = samplers[i];
        if (s.name.empty())
            return fail("sampler[" + std::to_string(i) + "] has empty name");
        if (s.name.size() > 63)
            return fail("sampler '" + s.name + "' name exceeds 63 chars");
        for (size_t j = i + 1; j < samplers.size(); ++j) {
            if (samplers[j].name == s.name)
                return fail("duplicate sampler name '" + s.name + "'");
        }
    }

    // Variants
    for (size_t i = 0; i < variants.size(); ++i) {
        for (size_t j = i + 1; j < variants.size(); ++j) {
            if (variants[j].variantKey == variants[i].variantKey)
                return fail("duplicate variant key "
                            + std::to_string(variants[i].variantKey));
        }
    }

    return true;
}

// ---- UBO layout helpers ----

uint32_t uboTypeAlign(ParameterType type) {
    switch (type) {
        case ParameterType::Float:  return 4;
        case ParameterType::Float2: return 8;
        case ParameterType::Float3: return 16;
        case ParameterType::Float4: return 16;
        case ParameterType::Int:    return 4;
        case ParameterType::Bool:   return 4;
    }
    return 4;
}

uint32_t uboTypeStride(ParameterType type) {
    switch (type) {
        case ParameterType::Float:  return 4;
        case ParameterType::Float2: return 8;
        case ParameterType::Float3: return 16;  // std140 vec3 stride
        case ParameterType::Float4: return 16;
        case ParameterType::Int:    return 4;
        case ParameterType::Bool:   return 4;
    }
    return 4;
}

// ---- PackageBuilder ----

PackageBuilder& PackageBuilder::setName(std::string name) {
    pkg_.name = std::move(name);
    return *this;
}

PackageBuilder& PackageBuilder::setShadingModel(ShadingModel sm) {
    pkg_.shadingModel = sm;
    return *this;
}

uint32_t PackageBuilder::addParameter(std::string name, ParameterType type,
                                      const float* defaults) {
    const uint32_t align  = uboTypeAlign(type);
    const uint32_t stride = uboTypeStride(type);

    // Align the cursor up to the type's required alignment.
    nextUboOffset_ = (nextUboOffset_ + align - 1) & ~(align - 1);
    const uint32_t offset = nextUboOffset_;
    nextUboOffset_ += stride;

    ParameterDesc p;
    p.name      = std::move(name);
    p.type      = type;
    p.uboOffset = offset;
    if (defaults)
        std::memcpy(p.defaults, defaults, sizeof(p.defaults));
    pkg_.parameters.push_back(std::move(p));
    return offset;
}

PackageBuilder& PackageBuilder::addSampler(std::string name, SamplerType type,
                                           uint32_t set, uint32_t binding) {
    SamplerDesc s;
    s.name    = std::move(name);
    s.type    = type;
    s.set     = set;
    s.binding = (binding == kBindingAuto) ? nextBinding_++ : binding;
    pkg_.samplers.push_back(std::move(s));
    return *this;
}

PackageBuilder& PackageBuilder::addVariant(uint32_t variantKey,
                                           std::vector<uint32_t> vertSpirv,
                                           std::vector<uint32_t> fragSpirv) {
    VariantDesc v;
    v.variantKey = variantKey;
    v.vertSpirv  = std::move(vertSpirv);
    v.fragSpirv  = std::move(fragSpirv);
    pkg_.variants.push_back(std::move(v));
    return *this;
}

MaterialPackage PackageBuilder::build() {
    // Round paramUboSize up to next multiple of 16 (std140 struct stride rule).
    pkg_.paramUboSize = (nextUboOffset_ + 15u) & ~15u;

    std::string err;
    if (!pkg_.validate(&err)) {
        std::fprintf(stderr, "PackageBuilder::build: validation failed: %s\n",
                     err.c_str());
        std::abort();
    }
    return std::move(pkg_);
}

} // namespace mat
