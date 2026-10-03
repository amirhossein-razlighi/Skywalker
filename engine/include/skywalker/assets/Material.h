#pragma once
// Material assets (`*.mat.json`). Reflected like components, so the same validation,
// schema and editor UI apply. A MeshRenderer whose `material` field names a material
// uses it instead of its inline color/metallic/roughness/emissive/texture values.

#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct MaterialAsset {
    Vec4 color{0.8f, 0.8f, 0.82f, 1.f};
    float metallic = 0.f;
    float roughness = 0.55f;
    Vec4 emissive{0.f, 0.f, 0.f, 1.f};
    std::string texture;  // albedo texture, project-relative
    float tilingU = 1.f;
    float tilingV = 1.f;
    bool unlit = false;  // flat color/texture, no lighting (UI, 2D, stylized looks)

    static const TypeInfo& type();
};

Json materialToJson(const MaterialAsset& m);
Result<MaterialAsset> materialFromJson(const Json& j);
Result<MaterialAsset> loadMaterial(const std::string& absolutePath);
Status saveMaterial(const std::string& absolutePath, const MaterialAsset& m);

}  // namespace sky
