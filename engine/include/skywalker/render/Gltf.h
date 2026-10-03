#pragma once
// glTF 2.0 importer (.glb binary and .gltf JSON with embedded/external buffers).
//
// Most 3D-generation models (and every DCC tool) export glTF. The importer flattens the
// default scene's node hierarchy into one mesh (node transforms applied), reads
// POSITION / NORMAL / TEXCOORD_0 / indices, and reports the first material's base color
// and base-color texture (raw PNG/JPEG bytes) so the asset system can create a material.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/MeshData.h"

namespace sky {

struct GltfImport {
    MeshData mesh;
    Vec4 baseColor{1, 1, 1, 1};
    float metallic = 0.f;
    float roughness = 0.6f;
    Vec4 emissive{0, 0, 0, 1};
    std::vector<uint8_t> textureBytes;  // base color texture, if any
    std::string textureMime;            // "image/png" | "image/jpeg"
    size_t primitiveCount = 0;
    size_t materialCount = 0;
};

/// `baseDir` resolves external buffer/image URIs of .gltf files.
Result<GltfImport> parseGltf(const std::vector<uint8_t>& fileBytes, const std::string& baseDir, bool normalize = true);
Result<GltfImport> loadGltf(const std::string& path, bool normalize = true);

}  // namespace sky
