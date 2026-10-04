#pragma once
// Render layers and light v2 helpers (docs/RENDERING.md "Render layers" and "Lights").
//
// 20 render layers: MeshRenderer.layers says which layers a mesh is on;
// Camera.cullMask and Light.cullMask say which layers a camera draws / a light illuminates.
// Bit i is layer i + 1. Projects name layers in game.json:
//   "render": {"layers": {"1": "world", "2": "player", "3": "fx", "20": "editor_only"}}
// Tools and Wander accept masks as a number (raw bits), a layer name or number ("player", "2"),
// "all" / "none", or a list of those.
//
// Field names, shared by every component that takes part: `layers` (what something is on) and
// `cullMask` (what something sees or affects). Future decals, reflection probes and render
// targets use the same two names.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky::render {

constexpr int kLayerCount = 20;
constexpr uint32_t kAllLayers = (1u << kLayerCount) - 1u;

/// Layer names from game.json (index 0 = layer 1; empty = unnamed).
struct LayerNames {
    std::array<std::string, kLayerCount> names;
    /// 1-based layer number of `name` (case-insensitive), or 0.
    int find(std::string_view name) const;
    /// Every name in use plus "layerN" aliases (for did-you-mean hints).
    std::vector<std::string> known() const;
    /// game.json "render.layers" object: {"1": "world", ...}.
    static Result<LayerNames> fromJson(const Json& layers);
    Json toJson() const;
};

/// Parses a mask: a number (raw bits), "all", "none", a layer name, a layer number as a string
/// ("3" or "layer3"), or an array of names / layer numbers (1..20). Unknown names fail with a
/// did-you-mean hint listing the project's layers.
Result<uint32_t> parseLayerMask(const Json& value, const LayerNames& names);
/// The layers in `mask` as names where named, else numbers: ["world", 4].
Json describeLayerMask(uint32_t mask, const LayerNames& names);

/// A normalized RGB tint (max channel 1) for a blackbody color temperature in Kelvin
/// (1000..40000; ~6500 K is white). Authored-color space, like Light.color.
Vec3 kelvinToRgb(float kelvin);

/// Distance fade weight of a light (1 = full, 0 = faded out) at `distance` from the camera.
float lightDistanceFade(float distance, float begin, float length);

}  // namespace sky::render
