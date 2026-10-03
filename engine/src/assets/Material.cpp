#include "skywalker/assets/Material.h"

#include "skywalker/render/Renderer.h"

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
            SKY_FIELD_ENUM(MaterialAsset, shading, "pbr = physically based, toon = cel bands + crisp highlights, unlit = flat",
                           "pbr", "toon", "unlit"),
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
    return s;
}

const std::vector<std::string>& materialPresets() {
    static const std::vector<std::string> names{"gold", "silver", "copper", "chrome", "brushed_steel", "iron",
                                                "plastic", "rubber", "ceramic", "car_paint", "glass", "water",
                                                "ice", "skin", "wax", "leaves", "snow", "velvet", "neon", "toon",
                                                "toon_metal", "clay"};
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
        {"water", R"({"color":"#2a6a8a99","metallic":0,"roughness":0.05})"},
        {"ice", R"({"color":"#cdeaffcc","metallic":0,"roughness":0.08,"subsurface":0.6})"},
        {"skin", R"({"color":"#e7b192","metallic":0,"roughness":0.55,"subsurface":0.7})"},
        {"wax", R"({"color":"#f1e7cf","metallic":0,"roughness":0.35,"subsurface":0.9})"},
        {"leaves", R"({"color":"#4f8a34","metallic":0,"roughness":0.7,"subsurface":0.6,"doubleSided":true})"},
        {"snow", R"({"color":"#f4f8ff","metallic":0,"roughness":0.75,"subsurface":0.5})"},
        {"velvet", R"({"color":"#6a1a3a","metallic":0,"roughness":0.85,"rim":1.2})"},
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
    for (const auto& [k, v] : j.members()) doc[k] = v;
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
