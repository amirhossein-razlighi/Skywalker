#pragma once
// Material assets (`*.mat.json`). Reflected like components, so the same validation,
// schema and editor UI apply. A MeshRenderer whose `material` field names a material
// uses it instead of its inline color/metallic/roughness/emissive/texture values.

#include <string>
#include <vector>

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
    std::string shading = "pbr";  // pbr | toon | unlit | water
    std::string normalMap;
    std::string ormMap;
    std::string emissiveMap;
    float normalStrength = 1.f;
    bool triplanar = false;
    float clearcoat = 0.f;
    float subsurface = 0.f;
    float rim = 0.f;
    float outline = 0.f;
    Vec4 outlineColor{0.04f, 0.04f, 0.06f, 1.f};
    bool doubleSided = false;
    float occlusionStrength = 1.f;  // how much the ORM map's red channel darkens indirect light
    float alphaCutoff = 0.f;        // > 0: alpha-tested cutout (foliage, sails, fences)
    // Car paint: the clearcoat's own roughness, and metallic flakes in the base layer (docs/RENDERING.md "Car paint").
    float clearcoatRoughness = 0.06f;
    float flakes = 0.f;           // 0..1: share of the base reflection coming from flakes (sparkle)
    float flakeSize = 0.0015f;    // flake size in meters (object space); smaller than a pixel fades into a sheen

    static const TypeInfo& type();
};

struct Surface;
/// The renderer's view of a material (texture paths stay project-relative).
Surface toSurface(const MaterialAsset& m);
/// Built-in starting points for common materials ("gold", "car_paint", "glass", "toon", ...).
const std::vector<std::string>& materialPresets();
Result<MaterialAsset> materialPreset(const std::string& name);

Json materialToJson(const MaterialAsset& m);
Result<MaterialAsset> materialFromJson(const Json& j);
Result<MaterialAsset> loadMaterial(const std::string& absolutePath);
Status saveMaterial(const std::string& absolutePath, const MaterialAsset& m);

}  // namespace sky
