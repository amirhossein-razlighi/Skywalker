#pragma once
// Scene audit: a CPU quality gate for what a camera sees (the `scene_audit` tool).
//
// From one FrameData (the frame a camera would render) the audit answers the questions an art
// director asks before filming:
//   * which builtin primitive meshes (cube, sphere, capsule...) are on screen and how much of
//     the image they cover, occlusion included (a coarse CPU depth raster of every draw,
//     terrain, water and instanced foliage), so a ground plane hidden under terrain or a prop
//     inside a hedge does not count;
//   * which "characters" are made of primitives (an animator / character controller / nav
//     agent on primitive meshes, or a sphere-on-capsule arrangement of primitive parts);
//   * default or untextured materials, missing texture files, big meshes without LODs,
//     texel-density outliers, a default sky, lights without shadows on hero meshes.
// The result is JSON with a `pass` verdict and `warnings` that carry fix hints.
//
// Everything is deterministic and runs without a GPU (tests, headless CI).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/render/MeshData.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/scene/Scene.h"

namespace sky::audit {

/// Builtin geometric primitives (the placeholder shapes the audit hunts for).
bool isPrimitiveMesh(std::string_view mesh);
/// Builtin procedural nature meshes (grass, fern, flowers, pebbles, shell, rock): low detail.
bool isProceduralMesh(std::string_view mesh);

/// Where the audit gets geometry and files from (the engine in the tool; fakes in tests).
struct Sources {
    std::function<const MeshData*(const std::string& meshKey)> mesh;
    /// Posed (CPU-skinned) geometry of an animated draw; null = use the rest pose.
    std::function<std::shared_ptr<const MeshData>(EntityId entity, const std::string& meshKey)> posed;
    /// Absolute path of a project-relative path (material and texture checks).
    std::function<std::string(const std::string& path)> resolvePath;
    /// Kit prefabs available for hints ("kit/characters/guard.prefab.json", ...).
    std::vector<std::string> kitCharacters;
    std::vector<std::string> kitProps;
};

struct Options {
    int resolution = 320;  // width of the coverage raster (height follows the aspect)
    bool strict = false;
    /// Total visible primitive coverage (0..1) above which the view fails. < 0 = 0.001 strict, 0.02 otherwise.
    float maxPrimitiveCoverage = -1.f;
    /// Same for default (never-assigned) materials.
    float maxDefaultMaterialCoverage = -1.f;
    /// Ignore draws covering less than this fraction (sub-pixel specks).
    float minCoverage = 0.00002f;
    /// Stylized looks: flat-colored (untextured) materials are reported as info, not warnings.
    bool stylized = false;
};

/// A coarse depth raster of a frame: per pixel the nearest draw index (>= 0), or one of the
/// kSky / kTerrain / kWater / kInstances codes.
struct CoverageBuffer {
    static constexpr int kSky = -1;
    static constexpr int kTerrain = -2;
    static constexpr int kWater = -3;
    static constexpr int kFoliageBase = -16;  // foliage batch i owns pixels as kFoliageBase - i
    int width = 0;
    int height = 0;
    std::vector<float> depth;   // clip z / w (0 near .. 1 far)
    std::vector<int32_t> owner;
    size_t triangles = 0;       // triangles rasterized
    /// Pixels per owner (draw index -> count); sky, terrain and water in the extras.
    std::vector<uint32_t> drawPixels;
    uint32_t skyPixels = 0, terrainPixels = 0, waterPixels = 0;
    /// Pixels per instanced foliage batch (FrameData::instances index).
    std::vector<uint32_t> foliagePixels;
    size_t foliageInstances = 0;  // instances rasterized (nearest first, within a triangle budget)
    /// Draws whose geometry could not be loaded (missing or broken mesh files).
    std::vector<size_t> missing;
    float pixelFraction() const { return width && height ? 1.f / static_cast<float>(width * height) : 0.f; }
};

CoverageBuffer rasterize(const FrameData& frame, const Sources& src, int width);

/// Audits one frame. `label` names the view in the result (e.g. "t=2.5s").
Json auditFrame(const Scene& scene, const FrameData& frame, const Sources& src, const Options& opts,
                const std::string& label = "");

/// Pixel size of a PNG, JPEG or Radiance HDR file from its header (false if unreadable).
bool imageSize(const std::string& absPath, int& width, int& height);

}  // namespace sky::audit
