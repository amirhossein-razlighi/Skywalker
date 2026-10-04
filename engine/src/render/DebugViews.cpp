#include "skywalker/render/DebugViews.h"

#include "skywalker/core/Strings.h"

namespace sky {

const std::vector<DebugViewInfo>& debugViews() {
    using namespace debugview;
    static const std::vector<DebugViewInfo> kViews = {
        {kFinal, "final", "final", "the finished image (default)"},
        {kAlbedo, "albedo", "gbuffer", "base color without lighting (linear albedo from the G-buffer)"},
        {kNormals, "normals", "gbuffer", "world normals as RGB (x right = red, y up = green, z = blue)"},
        {kMaterial, "material", "gbuffer", "roughness in red, metallic in green (clearcoated paint shows its coat roughness)"},
        {kGi, "gi", "gbuffer", "screen-space bounce light only"},
        {kReflections, "reflections", "gbuffer", "screen-space reflections only"},
        {kAo, "ao", "gbuffer", "ambient occlusion (white = open, black = occluded)"},
        {kDepth, "depth", "gbuffer", "scene depth (near bright, far dark)"},
        {kLighting, "lighting", "gbuffer", "lit color before reflection probes, screen-space GI and reflections"},
        {kSketch, "sketch", "style", "pencil contours and cross-hatching"},
        {kImpostors, "impostors", "final", "final image with foliage meshes tinted green and distant impostors magenta"},
        {kWireframe, "wireframe", "final", "dark flat-shaded surfaces with every mesh triangle edge drawn in cyan (depth-tested)"},
        {kOverdraw, "overdraw", "surface",
         "fragments shaded per pixel with no depth test (meshes, terrain, foliage, impostors): black 0, dark blue 1, "
         "blue 2, cyan 3, green 4, yellow 5-6, orange 7-9, red 10-15, white 16+"},
        {kUnshaded, "unshaded", "surface", "albedo + emission with no lighting, shadows or fog"},
        {kLightingOnly, "lighting_only", "surface",
         "the lighting on a white material (albedo 1): judge light placement, shadows and GI without textures"},
        {kShadowCascades, "shadow_cascades", "surface",
         "sun shadow cascade per pixel: red 0 (nearest), green 1, blue 2, yellow 3, gray = beyond the shadow distance"},
        {kLightComplexity, "light_complexity", "surface",
         "point/spot lights evaluated per pixel (its light cluster): black 0, dark blue 1, blue 2, cyan 3, green 4, "
         "yellow 5-6, orange 7-9, red 10-15, white 16+"},
        {kLod, "lod", "surface",
         "mesh level of detail: green LOD0, yellow 1, orange 2, red 3, magenta 4+; foliage meshes by distance band, "
         "impostors purple, terrain by CDLOD node level"},
        {kEmission, "emission", "surface", "emissive light only (bright = glowing)"},
        {kSpecular, "specular", "surface",
         "specular reflectance F0 scaled by glossiness: dielectrics dark gray, metals their tint, rough surfaces darker"},
        {kUvChecker, "uv_checker", "surface",
         "a checker on UV0 (8x8 cells per 0..1 UV tile, red/green tint = U/V position): stretching, seams and flipped "
         "UVs; terrain shows one cell per texture repeat"},
        {kTexelDensity, "texel_density", "surface",
         "base-color texels per meter: blue < 128, cyan 256, green 512 (target), yellow 1024, red > 2048; gray = "
         "untextured"},
        {kMotion, "motion", "gbuffer",
         "the velocity buffer TAA, MetalFX and motion blur use: hue = direction, strength = speed on a log scale "
         "(faint at 0.25 px, full at 15 px per frame) over a dimmed scene; capture twice (samples 1) while something moves"},
        {kShadowAtlas, "shadow_atlas", "final",
         "the local (point / spot) shadow atlas: 4 quadrants of shadow maps (near white, far dark), one outline per "
         "light view: green re-rendered this frame, blue cached, orange waiting for the update budget (shadow_atlas_info)"},
        {kReflectionProbes, "reflection_probes", "final",
         "which reflection probe lights each pixel: the scene tinted with each probe's color (probe_info debugColor; blends "
         "mix their colors, gray = sky only), every influence volume outlined in its color (dashed where hidden), capture "
         "points as dots"},
        {kVehicles, "vehicles", "final",
         "final image with every vehicle's suspension (gray travel, white-to-orange current length), wheels (green on the "
         "ground, red when sliding, gray airborne), contact points and tire forces at them: blue load, orange drive/brake, "
         "red cornering (0.6 m = a wheel's static load); cyan velocity (0.25 s), yellow center of mass. Captures only "
         "(the live viewport shows the final image)"},
    };
    return kViews;
}

std::vector<std::string> debugViewNames() {
    std::vector<std::string> out;
    for (const auto& v : debugViews()) out.emplace_back(v.name);
    return out;
}

Result<int> debugViewFromName(std::string_view name) {
    if (name.empty()) return debugview::kFinal;
    for (const auto& v : debugViews()) {
        if (name == v.name) return v.id;
    }
    std::vector<std::string> names = debugViewNames();
    std::string near = str::closest(name, names, 4);
    std::string all;
    for (const auto& n : names) all += (all.empty() ? "" : ", ") + n;
    std::string hint = near.empty() ? "valid views: " + all : "did you mean \"" + near + "\"? (valid views: " + all + ")";
    return Error::make("invalid_debug_view", "unknown debug view \"" + std::string(name) + "\"", hint);
}

const char* debugViewName(int id) {
    for (const auto& v : debugViews()) {
        if (v.id == id) return v.name;
    }
    return "final";
}

bool debugViewOverridesSurfaces(int id) {
    for (const auto& v : debugViews()) {
        if (v.id == id) return std::string_view(v.kind) == "surface" || id == debugview::kWireframe;
    }
    return false;
}

std::string debugViewHelp() {
    std::string out;
    for (const auto& v : debugViews()) {
        out += v.name;
        out += ": ";
        out += v.description;
        out += "\n";
    }
    return out;
}

}  // namespace sky
