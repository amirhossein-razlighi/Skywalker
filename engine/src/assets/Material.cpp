#include "skywalker/assets/Material.h"

#include <fstream>
#include <sstream>
#include <type_traits>

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
        }};
    return info;
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
