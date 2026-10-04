#include "skywalker/assets/Material.h"

#include "skywalker/render/Renderer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <unordered_map>

namespace sky {

static_assert(std::is_standard_layout_v<MaterialAsset>);

const TypeInfo& MaterialAsset::type() {
    static const TypeInfo info{
        "material",
        "A reusable surface: color, metal/roughness, emission, albedo texture with tiling, optional unlit.",
        {
            SKY_FIELD(MaterialAsset, color, Color, "Base color (multiplied with the texture)"),
            SKY_FIELD_RANGE(MaterialAsset, metallic, Float, "0 = dielectric, 1 = metal", 0.f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, roughness, Float, "0 = mirror, 1 = matte", 0.02f, 1.f),
            SKY_FIELD(MaterialAsset, emissive, Color, "Emitted light; alpha is strength (glows with bloom)"),
            SKY_FIELD(MaterialAsset, texture, String, "Albedo texture path (png/jpg), project-relative"),
            SKY_FIELD_RANGE(MaterialAsset, tilingU, Float, "Texture repeats along U", 0.01f, 1000.f),
            SKY_FIELD_RANGE(MaterialAsset, tilingV, Float, "Texture repeats along V", 0.01f, 1000.f),
            SKY_FIELD(MaterialAsset, unlit, Bool, "Ignore lighting (flat look for 2D / UI / stylized)"),
            SKY_FIELD_ENUM(MaterialAsset, shading,
                           "pbr = physically based, toon = cel bands + crisp highlights, unlit = flat, water = animated waves; "
                           "character models: skin (subsurface scattering, dual-lobe specular, pores, transmission), eye "
                           "(refracted iris, wet cornea, shadowed corners), cloth (sheen and fuzz), hair_card (anisotropic "
                           "strand highlights, dithered or coverage alpha)",
                           "pbr", "toon", "unlit", "water", "skin", "eye", "cloth", "hair_card"),
            SKY_FIELD(MaterialAsset, normalMap, String, "Normal map (png), project-relative"),
            SKY_FIELD(MaterialAsset, ormMap, String, "Occlusion/roughness/metallic map (R/G/B), project-relative"),
            SKY_FIELD(MaterialAsset, emissiveMap, String, "Emission map, multiplied with emissive"),
            SKY_FIELD_RANGE(MaterialAsset, normalStrength, Float, "Normal map strength", 0.f, 4.f),
            SKY_FIELD(MaterialAsset, triplanar, Bool, "World-space texture projection (tiling = repeats per meter)"),
            SKY_FIELD_RANGE(MaterialAsset, clearcoat, Float, "Glossy lacquer layer (car paint, varnish)", 0.f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, subsurface, Float, "Light bleeding through (skin, leaves, wax, snow)", 0.f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, rim, Float, "Stylized rim light", 0.f, 4.f),
            SKY_FIELD_RANGE(MaterialAsset, outline, Float, "Cartoon outline width in pixels", 0.f, 12.f),
            SKY_FIELD(MaterialAsset, outlineColor, Color, "Outline color"),
            SKY_FIELD(MaterialAsset, doubleSided, Bool, "Render both faces"),
            SKY_FIELD_RANGE(MaterialAsset, occlusionStrength, Float, "Ambient occlusion from the ORM map's red channel", 0.f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, alphaCutoff, Float, "Alpha-tested cutout threshold (foliage, sails, fences); 0 = off",
                            0.f, 1.f),
            // skin
            SKY_FIELD(MaterialAsset, scatterColor, Color,
                      "skin: relative distance each color scatters under the surface (red farthest: the warm glow in shadow "
                      "edges and backlit ears)"),
            SKY_FIELD_RANGE(MaterialAsset, scatterRadius, Float, "skin: scattering distance of the reddest light in millimeters (1-4)",
                            0.f, 50.f),
            SKY_FIELD_RANGE(MaterialAsset, lobeMix, Float, "skin: weight of the second, broader specular lobe (0.1-0.2)", 0.f, 1.f),
            SKY_FIELD(MaterialAsset, lobeRoughness, Vec2, "skin: roughness multipliers of the two specular lobes [sharp, broad]"),
            SKY_FIELD_RANGE(MaterialAsset, microNormal, Float, "skin: procedural pore detail strength (fades with distance)", 0.f, 2.f),
            SKY_FIELD_RANGE(MaterialAsset, microNormalTiling, Float, "skin: pore detail repeats per UV unit", 1.f, 4000.f),
            SKY_FIELD_RANGE(MaterialAsset, transmission, Float,
                            "skin: light through thin parts (ears, nostrils, fingers), from the sun's shadow-map thickness", 0.f, 4.f),
            // eye
            SKY_FIELD(MaterialAsset, irisCenter, Vec2, "eye: UV of the iris center in the eye texture"),
            SKY_FIELD(MaterialAsset, irisCenter2, Vec2,
                      "eye: UV of a second iris when both eyes share one texture (each pixel uses the nearer); [-1, -1] = none"),
            SKY_FIELD_RANGE(MaterialAsset, irisRadius, Float, "eye: UV radius of the iris", 0.001f, 0.5f),
            SKY_FIELD_RANGE(MaterialAsset, irisDepth, Float, "eye: parallax depth of the iris under the cornea (UV units)", 0.f, 0.3f),
            SKY_FIELD_RANGE(MaterialAsset, corneaRoughness, Float, "eye: roughness of the wet cornea highlight", 0.01f, 0.5f),
            SKY_FIELD_RANGE(MaterialAsset, eyeShadow, Float, "eye: darkening toward the eye corners (lids and lashes)", 0.f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, limbusDarkening, Float, "eye: dark ring around the iris", 0.f, 1.f),
            // cloth
            SKY_FIELD(MaterialAsset, sheenColor, Color, "cloth: color of the soft sheen at grazing angles (velvet, wool, cotton)"),
            SKY_FIELD_RANGE(MaterialAsset, sheenRoughness, Float, "cloth: 0.3 satin .. 1 felt", 0.07f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, fuzz, Float, "cloth: extra rim of loose fibers", 0.f, 2.f),
            // hair cards
            SKY_FIELD_RANGE(MaterialAsset, hairShift, Float, "hair_card: primary highlight shift along the strand", -1.f, 1.f),
            SKY_FIELD_RANGE(MaterialAsset, hairSpecular, Float, "hair_card: highlight strength", 0.f, 4.f),
            SKY_FIELD_ENUM(MaterialAsset, hairDirection,
                           "hair_card: strand direction in the card's UVs (root to tip); auto = found per pixel from the strand "
                           "pattern of the texture (atlases whose cards run either way)",
                           "auto", "v", "-v", "u", "-u"),
            SKY_FIELD_ENUM(MaterialAsset, alphaMode,
                           "hair_card: dither = stochastic transparency that resolves to soft strands under TAA and stills; "
                           "coverage = alpha to coverage (MSAA)",
                           "dither", "coverage"),
        }};
    return info;
}

Surface toSurface(const MaterialAsset& m) {
    Surface s;
    s.color = m.color;
    s.emissive = m.emissive;
    s.metallic = m.metallic;
    s.roughness = m.roughness;
    s.texture = m.texture;
    s.normalMap = m.normalMap;
    s.ormMap = m.ormMap;
    s.emissiveMap = m.emissiveMap;
    s.tiling = {m.tilingU, m.tilingV};
    s.normalStrength = m.normalStrength;
    s.triplanar = m.triplanar;
    s.shading = m.unlit ? Shading::Unlit : shadingFromString(m.shading);
    s.clearcoat = m.clearcoat;
    s.subsurface = m.subsurface;
    s.rim = m.rim;
    s.outline = m.outline;
    s.outlineColor = m.outlineColor;
    s.doubleSided = m.doubleSided;
    s.occlusionStrength = m.occlusionStrength;
    s.alphaCutoff = m.alphaCutoff;
    // Character models: packed for DrawUniforms.character (colors linear, like the renderer's lin()).
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    switch (s.shading) {
        case Shading::Skin:
            s.model[0] = {lin(m.scatterColor.x), lin(m.scatterColor.y), lin(m.scatterColor.z), m.scatterRadius};
            s.model[1] = {m.lobeRoughness.x, m.lobeRoughness.y, m.lobeMix, m.transmission};
            s.model[2] = {m.microNormal, m.microNormalTiling, 0.f, 0.f};
            break;
        case Shading::Eye:
            s.model[0] = {m.irisCenter.x, m.irisCenter.y, m.irisRadius, m.irisDepth};
            s.model[1] = {m.corneaRoughness, m.eyeShadow, m.limbusDarkening, 0.f};
            s.model[2] = {m.irisCenter2.x, m.irisCenter2.y, 0.f, 0.f};
            break;
        case Shading::Cloth:
            s.model[0] = {lin(m.sheenColor.x), lin(m.sheenColor.y), lin(m.sheenColor.z), m.sheenRoughness};
            s.model[1] = {m.fuzz, 0.f, 0.f, 0.f};
            break;
        case Shading::HairCard: {
            const bool u = m.hairDirection == "u" || m.hairDirection == "-u";
            const bool neg = !m.hairDirection.empty() && m.hairDirection[0] == '-';
            const bool autoDir = m.hairDirection.empty() || m.hairDirection == "auto";
            // z: 0 = along u, 1 = along v, 2 = auto (the shader reads the texture's strand pattern)
            s.model[0] = {m.hairShift, m.hairSpecular, autoDir ? 2.f : (u ? 0.f : 1.f), neg ? -1.f : 1.f};
            s.model[1] = {m.alphaMode == "coverage" ? 1.f : 0.f, 0.f, 0.f, 0.f};
            break;
        }
        default: break;
    }
    return s;
}

const std::vector<std::string>& materialPresets() {
    static const std::vector<std::string> names{"gold", "silver", "copper", "chrome", "brushed_steel", "iron",
                                                "plastic", "rubber", "ceramic", "car_paint", "glass", "water",
                                                "ice", "skin", "wax", "leaves", "snow", "velvet", "neon", "toon",
                                                "toon_metal", "clay", "eye", "cloth", "hair_card"};
    return names;
}

Result<MaterialAsset> materialPreset(const std::string& name) {
    static const std::unordered_map<std::string, const char*> presets{
        {"gold", R"({"color":"#ffc35a","metallic":1,"roughness":0.22})"},
        {"silver", R"({"color":"#f2f0ec","metallic":1,"roughness":0.18})"},
        {"copper", R"({"color":"#f2a07a","metallic":1,"roughness":0.3})"},
        {"chrome", R"({"color":"#f5f6f8","metallic":1,"roughness":0.04})"},
        {"brushed_steel", R"({"color":"#c8ccd2","metallic":1,"roughness":0.38})"},
        {"iron", R"({"color":"#8a8a8f","metallic":0.9,"roughness":0.62})"},
        {"plastic", R"({"color":"#e04a3a","metallic":0,"roughness":0.32})"},
        {"rubber", R"({"color":"#202022","metallic":0,"roughness":0.92})"},
        {"ceramic", R"({"color":"#f4f1ea","metallic":0,"roughness":0.12,"clearcoat":0.6})"},
        {"car_paint", R"({"color":"#b01622","metallic":0.6,"roughness":0.38,"clearcoat":1})"},
        {"glass", R"({"color":"#d8ecff38","metallic":0,"roughness":0.03,"doubleSided":true})"},
        {"water", R"({"color":"#0d3a4a","metallic":0,"roughness":0.04,"shading":"water"})"},
        {"ice", R"({"color":"#cdeaffcc","metallic":0,"roughness":0.08,"subsurface":0.6})"},
        {"skin", R"({"color":"#e7b192","metallic":0,"roughness":0.5,"shading":"skin","scatterColor":"#f2a68d",
            "scatterRadius":2.2,"lobeMix":0.15,"lobeRoughness":[0.75,1.3],"microNormal":0.25,"microNormalTiling":140,
            "transmission":0.6})"},
        {"eye", R"({"color":"#ffffff","metallic":0,"roughness":0.2,"shading":"eye","irisCenter":[0.5,0.5],"irisRadius":0.12,
            "irisDepth":0.05,"corneaRoughness":0.03,"eyeShadow":0.6,"limbusDarkening":0.45})"},
        {"cloth", R"({"color":"#6f6a62","metallic":0,"roughness":0.85,"shading":"cloth","sheenColor":"#d8d2c8",
            "sheenRoughness":0.55,"fuzz":0.4})"},
        {"hair_card", R"({"color":"#3a2a1e","metallic":0,"roughness":0.35,"shading":"hair_card","alphaCutoff":0.05,
            "doubleSided":true,"hairShift":0.08,"hairSpecular":0.8,"hairDirection":"auto","alphaMode":"dither"})"},
        {"wax", R"({"color":"#f1e7cf","metallic":0,"roughness":0.35,"subsurface":0.9})"},
        {"leaves", R"({"color":"#4f8a34","metallic":0,"roughness":0.7,"subsurface":0.6,"doubleSided":true})"},
        {"snow", R"({"color":"#f4f8ff","metallic":0,"roughness":0.75,"subsurface":0.5})"},
        {"velvet", R"({"color":"#6a1a3a","metallic":0,"roughness":0.85,"shading":"cloth","sheenColor":"#e8a0c0",
            "sheenRoughness":0.35,"fuzz":0.6})"},
        {"neon", R"({"color":"#000000","emissive":[0.2,0.9,1,4],"unlit":true})"},
        {"toon", R"({"color":"#ff8a4a","shading":"toon","roughness":0.4,"rim":0.6,"outline":2})"},
        {"toon_metal", R"({"color":"#c8d2e0","shading":"toon","metallic":0.8,"roughness":0.25,"rim":0.8,"outline":2})"},
        {"clay", R"({"color":"#d98a6a","metallic":0,"roughness":0.95})"},
    };
    auto it = presets.find(name);
    if (it == presets.end()) {
        return Error::make("unknown_preset", "no material preset '" + name + "'", "presets: gold, chrome, glass, car_paint, skin, toon, ...");
    }
    return materialFromJson(Json::parse(it->second).value());
}

Json materialToJson(const MaterialAsset& m) {
    Json j = reflect::toJson(&m, MaterialAsset::type());
    Json doc = Json::object({{"format", "skywalker.material"}, {"version", 1}});
    // Fields of the character models are written only for materials of that model (keeps files short).
    static const std::vector<std::pair<std::string, std::vector<std::string>>> groups = {
        {"skin", {"scatterColor", "scatterRadius", "lobeMix", "lobeRoughness", "microNormal", "microNormalTiling", "transmission"}},
        {"eye", {"irisCenter", "irisCenter2", "irisRadius", "irisDepth", "corneaRoughness", "eyeShadow", "limbusDarkening"}},
        {"cloth", {"sheenColor", "sheenRoughness", "fuzz"}},
        {"hair_card", {"hairShift", "hairSpecular", "hairDirection", "alphaMode"}}};
    auto skip = [&](const std::string& key) {
        for (const auto& [model, keys] : groups) {
            if (model != m.shading && std::find(keys.begin(), keys.end(), key) != keys.end()) return true;
        }
        return false;
    };
    for (const auto& [k, v] : j.members()) {
        if (!skip(k)) doc[k] = v;
    }
    return doc;
}

Result<MaterialAsset> materialFromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_material", "material must be a JSON object");
    Json fields = j;
    fields.erase("format");
    fields.erase("version");
    MaterialAsset m;
    if (Status s = reflect::applyJson(&m, MaterialAsset::type(), fields); !s) return s.error();
    return m;
}

Result<MaterialAsset> loadMaterial(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Error::make("io_error", "cannot read material " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = Json::parse(ss.str());
    if (!j) return j.error();
    return materialFromJson(j.value());
}

Status saveMaterial(const std::string& path, const MaterialAsset& m) {
    std::ofstream f(path);
    if (!f) return Error::make("io_error", "cannot write material " + path);
    f << materialToJson(m).dump(2) << "\n";
    return {};
}

}  // namespace sky
