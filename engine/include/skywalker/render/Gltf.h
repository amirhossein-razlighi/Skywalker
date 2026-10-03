#pragma once
// glTF 2.0 importer (.glb binary and .gltf JSON with embedded/external buffers).
//
// Most 3D-generation models (and every DCC tool) export glTF. The importer flattens the
// default scene's node hierarchy (node transforms applied) and reads POSITION / NORMAL /
// TEXCOORD_0 / COLOR_0 / indices plus every material (PBR factors, base color / normal /
// metal-roughness / emissive maps, alpha mode, double-sidedness).
//
// Models with several materials are split into *parts*, one per material: `part` selects
// the triangles of one material (the asset system addresses it as "asset:model.gltf#<material>").
// `fullBounds` always covers the whole model so parts can be normalized consistently.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/MeshData.h"

namespace sky {

struct GltfImport {
    struct ImageData {
        std::vector<uint8_t> bytes;
        std::string mime;
        std::string uri;  // external file, relative to the .gltf (empty for embedded images)
        bool empty() const { return bytes.empty(); }
        const char* extension() const { return mime == "image/jpeg" ? ".jpg" : ".png"; }
    };
    struct Material {
        std::string name;
        Vec4 baseColor{1, 1, 1, 1};
        float metallic = 1.f;
        float roughness = 1.f;
        Vec4 emissive{0, 0, 0, 1};
        int baseColorImage = -1, normalImage = -1, metallicRoughnessImage = -1, emissiveImage = -1;
        float normalScale = 1.f;
        float occlusionStrength = 0.f;  // > 0 when occlusion is packed into the metal-roughness map's R
        std::string alphaMode = "OPAQUE";  // OPAQUE | MASK | BLEND
        float alphaCutoff = 0.5f;
        bool doubleSided = false;
    };

    MeshData mesh;
    Aabb fullBounds;               // whole model, even when one part was selected
    std::vector<Material> materials;
    std::vector<ImageData> images;  // indexed like glTF images
    std::vector<int> parts;         // materials that have geometry, in first-use order (-1 = no material)
    size_t primitiveCount = 0;
    size_t materialCount = 0;
};

constexpr int kGltfAllParts = -2;

/// `baseDir` resolves external buffer/image URIs of .gltf files. `part` = a material index
/// (or -1 for primitives without a material) keeps only that material's triangles.
Result<GltfImport> parseGltf(const std::vector<uint8_t>& fileBytes, const std::string& baseDir, bool normalize = true,
                             int part = kGltfAllParts);
Result<GltfImport> loadGltf(const std::string& path, bool normalize = true, int part = kGltfAllParts);

}  // namespace sky
