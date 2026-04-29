#include "gltf_loader.h"

#include <tiny_gltf.h>

#include <cstdio>
#include <cstring>
#include <string>

bool loadGltfAsset(const char* path, MeshData& out) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model    model;
    std::string        err, warn;

    const std::string p(path);
    const bool isBinary = p.size() >= 4 && p.compare(p.size() - 4, 4, ".glb") == 0;
    const bool ok = isBinary
        ? loader.LoadBinaryFromFile(&model, &err, &warn, p)
        : loader.LoadASCIIFromFile (&model, &err, &warn, p);

    if (!warn.empty()) std::fprintf(stderr, "tinygltf warning: %s\n", warn.c_str());
    if (!ok) {
        std::fprintf(stderr, "tinygltf error loading %s: %s\n", path, err.c_str());
        return false;
    }
    if (model.meshes.empty() || model.meshes[0].primitives.empty()) {
        std::fprintf(stderr, "glTF has no mesh primitives\n");
        return false;
    }

    const auto& prim = model.meshes[0].primitives[0];

    auto attrPtr = [&](const char* name, size_t& count) -> const float* {
        auto it = prim.attributes.find(name);
        if (it == prim.attributes.end()) { count = 0; return nullptr; }
        const auto& acc  = model.accessors[it->second];
        const auto& view = model.bufferViews[acc.bufferView];
        const auto& buf  = model.buffers[view.buffer];
        count = acc.count;
        return reinterpret_cast<const float*>(
            buf.data.data() + view.byteOffset + acc.byteOffset);
    };

    size_t posCount = 0, normCount = 0, uvCount = 0;
    const float* positions = attrPtr("POSITION",   posCount);
    const float* normals   = attrPtr("NORMAL",     normCount);
    const float* uvs       = attrPtr("TEXCOORD_0", uvCount);
    if (!positions || !normals || posCount != normCount) {
        std::fprintf(stderr, "glTF primitive missing POSITION/NORMAL or count mismatch\n");
        return false;
    }
    if (!uvs || uvCount != posCount) {
        std::fprintf(stderr, "glTF primitive missing TEXCOORD_0 or count mismatch\n");
        return false;
    }

    out.vertices.resize(posCount);
    for (size_t i = 0; i < posCount; ++i) {
        out.vertices[i].pos    = {positions[i*3+0], positions[i*3+1], positions[i*3+2]};
        out.vertices[i].normal = {normals  [i*3+0], normals  [i*3+1], normals  [i*3+2]};
        out.vertices[i].uv     = {uvs      [i*2+0], uvs      [i*2+1]};
    }

    if (prim.indices < 0) {
        std::fprintf(stderr, "glTF primitive has no index buffer (non-indexed unsupported)\n");
        return false;
    }
    const auto& idxAcc  = model.accessors[prim.indices];
    const auto& idxView = model.bufferViews[idxAcc.bufferView];
    const auto& idxBuf  = model.buffers[idxView.buffer];
    const uint8_t* idxData = idxBuf.data.data() + idxView.byteOffset + idxAcc.byteOffset;

    out.indices.resize(idxAcc.count);
    switch (idxAcc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            std::memcpy(out.indices.data(), idxData, idxAcc.count * sizeof(uint32_t));
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            const uint16_t* src = reinterpret_cast<const uint16_t*>(idxData);
            for (size_t i = 0; i < idxAcc.count; ++i) out.indices[i] = src[i];
            break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            for (size_t i = 0; i < idxAcc.count; ++i) out.indices[i] = idxData[i];
            break;
        default:
            std::fprintf(stderr, "Unsupported index component type %d\n",
                         idxAcc.componentType);
            return false;
    }

    if (prim.material < 0) {
        std::fprintf(stderr, "glTF primitive has no material\n");
        return false;
    }
    const auto& mat = model.materials[prim.material];

    auto extractTexture = [&](int texIndex, const char* slotName, ImageData& outImg) -> bool {
        if (texIndex < 0) {
            std::fprintf(stderr, "Material missing %s texture\n", slotName);
            return false;
        }
        const auto& tex = model.textures[texIndex];
        if (tex.source < 0) {
            std::fprintf(stderr, "%s texture has no image source\n", slotName);
            return false;
        }
        const auto& img = model.images[tex.source];
        if (img.width <= 0 || img.height <= 0 || img.image.empty()) {
            std::fprintf(stderr, "%s image has no decoded pixel data\n", slotName);
            return false;
        }
        outImg.width  = static_cast<uint32_t>(img.width);
        outImg.height = static_cast<uint32_t>(img.height);
        outImg.rgba.resize(static_cast<size_t>(img.width) * img.height * 4);
        if (img.component == 4) {
            std::memcpy(outImg.rgba.data(), img.image.data(), outImg.rgba.size());
        } else if (img.component == 3) {
            const size_t pixelCount = static_cast<size_t>(img.width) * img.height;
            for (size_t i = 0; i < pixelCount; ++i) {
                outImg.rgba[i*4+0] = img.image[i*3+0];
                outImg.rgba[i*4+1] = img.image[i*3+1];
                outImg.rgba[i*4+2] = img.image[i*3+2];
                outImg.rgba[i*4+3] = 255;
            }
        } else {
            std::fprintf(stderr, "%s: unsupported channel count %d\n", slotName, img.component);
            return false;
        }
        return true;
    };

    if (!extractTexture(mat.pbrMetallicRoughness.baseColorTexture.index,
                        "baseColor", out.baseColor))               return false;
    if (!extractTexture(mat.pbrMetallicRoughness.metallicRoughnessTexture.index,
                        "metallicRoughness", out.metallicRoughness)) return false;
    if (!extractTexture(mat.normalTexture.index,
                        "normal", out.normal))                     return false;

    std::printf("Loaded %s: %zu vertices, %zu indices, baseColor %ux%u, MR %ux%u, normal %ux%u\n",
                path, out.vertices.size(), out.indices.size(),
                out.baseColor.width,         out.baseColor.height,
                out.metallicRoughness.width, out.metallicRoughness.height,
                out.normal.width,            out.normal.height);
    return true;
}
