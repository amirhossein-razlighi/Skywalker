#pragma once
// Debug views: the buffer visualizations and shading overrides behind `FrameData::debugView`,
// `viewport_capture {debug_view}`, `viewport_debug_view` and movie renders. One table maps the
// stable names agents use to the ids the backends switch on.
//
// Kinds:
//   gbuffer  - a fullscreen pass shows a G-buffer / lighting buffer (albedo, normals, ...)
//   surface  - every lit surface shades with a debug color instead of its material (unshaded,
//              lod, light_complexity, ...); shown without tonemapping so colors stay exact
//   final    - the final image, tinted or with lines on top (impostors, wireframe)
//   style    - a stylized look (sketch)

#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky {

namespace debugview {
// Ids are stable (scripts, movie presets and the shaders use them). Append only.
constexpr int kFinal = 0;
constexpr int kAlbedo = 1;
constexpr int kNormals = 2;
constexpr int kMaterial = 3;
constexpr int kGi = 4;
constexpr int kReflections = 5;
constexpr int kAo = 6;
constexpr int kDepth = 7;
constexpr int kLighting = 8;
constexpr int kSketch = 9;
constexpr int kImpostors = 10;
constexpr int kWireframe = 11;
constexpr int kOverdraw = 12;
constexpr int kUnshaded = 13;
constexpr int kLightingOnly = 14;
constexpr int kShadowCascades = 15;
constexpr int kLightComplexity = 16;
constexpr int kLod = 17;
constexpr int kEmission = 18;
constexpr int kSpecular = 19;
constexpr int kUvChecker = 20;
constexpr int kTexelDensity = 21;
constexpr int kMotion = 22;
constexpr int kShadowAtlas = 23;
constexpr int kReflectionProbes = 24;
constexpr int kVehicles = 25;  // final image + CPU overlay of vehicle suspension, contacts and forces (captures)
constexpr int kCount = 26;
}  // namespace debugview

struct DebugViewInfo {
    int id = 0;
    const char* name = "";
    const char* kind = "";         // gbuffer | surface | final | style
    const char* description = "";  // for agents: what the colors mean
};

/// Every debug view, ordered by id.
const std::vector<DebugViewInfo>& debugViews();
/// Names in id order ("final", "albedo", ...), e.g. for tool schema enums.
std::vector<std::string> debugViewNames();
/// Id for a name; unknown names fail with `invalid_debug_view` and a did-you-mean hint.
Result<int> debugViewFromName(std::string_view name);
/// Name for an id ("final" for out-of-range ids).
const char* debugViewName(int id);
/// True for views where surfaces shade with debug colors (the shaders' FrameUniforms.debug.x).
bool debugViewOverridesSurfaces(int id);
/// One line per view ("name: description"), for tool descriptions and docs.
std::string debugViewHelp();

}  // namespace sky
