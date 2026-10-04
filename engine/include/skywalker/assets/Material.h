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
    // --- Character material models (shading skin / eye / cloth / hair_card; docs/CHARACTERS.md) ---
    // skin
    Vec4 scatterColor{0.92f, 0.38f, 0.26f, 1.f};  // how far each color travels under the skin (relative): red the farthest
    float scatterRadius = 2.2f;      // mm: mean free path of the reddest light (pre-integrated diffusion and transmission)
    float lobeMix = 0.15f;           // weight of the second (broader) specular lobe
    Vec2 lobeRoughness{0.75f, 1.3f}; // roughness multipliers of the two specular lobes
    float microNormal = 0.25f;       // procedural pore / micro detail on the normal (0 = off)
    float microNormalTiling = 140.f; // micro detail repeats per UV unit
    float transmission = 0.6f;       // light through thin parts (ears, nostrils) from the sun's shadow-map thickness
    // eye
    Vec2 irisCenter{0.5f, 0.5f};     // UV of the iris center
    Vec2 irisCenter2{-1.f, -1.f};    // a second eye in the same texture (both eyes on one atlas); negative = none
    float irisRadius = 0.12f;        // UV radius of the iris (limbus)
    float irisDepth = 0.05f;         // UV parallax depth of the iris under the cornea (refraction)
    float corneaRoughness = 0.03f;   // wet cornea highlight
    float eyeShadow = 0.6f;          // darkening toward the eye corners (lids, lashes)
    float limbusDarkening = 0.45f;   // dark ring around the iris
    // cloth
    Vec4 sheenColor{1.f, 1.f, 1.f, 1.f};  // color of the fuzzy sheen at grazing angles
    float sheenRoughness = 0.5f;     // 0.3 satin .. 1 felt
    float fuzz = 0.4f;               // extra soft rim from fibers
    // hair_card
    float hairShift = 0.08f;         // primary highlight shift along the strand (cuticle tilt)
    float hairSpecular = 0.8f;       // highlight strength
    std::string hairDirection = "v"; // strand direction in UV: v, -v, u, -u
    std::string alphaMode = "dither";  // hair cards: dither (stochastic, resolves under TAA) | coverage (alpha to coverage)
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
