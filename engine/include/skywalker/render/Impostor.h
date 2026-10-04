#pragma once
// Octahedral impostors for distant foliage (the HLOD tail of trees, bushes, rocks, grass).
//
// A model (one mesh or every part of a prefab) is captured from a grid of directions laid out
// on a hemi-octahedron (upright vegetation is never seen from below) or a full octahedron. Each
// frame of the atlas stores albedo + coverage and a model-space normal + depth + subsurface, so
// far instances render as camera-facing quads that blend the 3 nearest frames, correct parallax
// with the depth, and write a real G-buffer and depth: lighting, shadows, GI, AO and fog treat
// them like geometry.
//
// This header is the portable part, shared by the CPU front end, the tests and the GPU shaders
// (Terrain.metal mirrors the direction/frame math exactly):
//   * direction encodings, frame directions/bases and 3-frame blend weights;
//   * the transition distance (screen-space texel density, quality tier, per-layer override);
//   * stable cache keys;
//   * the atlas post-process (un-premultiply, dilation, coverage-preserving mips) and the
//     project cache file (.skywalker/cache/impostors/<key>.skyimp).

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/Image.h"

namespace sky {
struct ImpostorModel;
}

namespace sky::impostor {

/// Bumped whenever the bake output changes, so caches from older engines are rebuilt.
constexpr int kBakeVersion = 1;
constexpr int kDefaultFrames = 12;
constexpr int kMinFrames = 4, kMaxFrames = 32;
/// Mip levels kept per atlas: frame tiles are multiples of 16 texels, so levels 0..4 never mix
/// neighbouring frames.
constexpr int kMipLevels = 5;
/// Alpha threshold of the runtime alpha test; mips preserve the coverage at this cutoff.
constexpr float kAlphaCutoff = 0.5f;
/// Transition at this fraction of the texel-matching distance (impostors may be magnified up to
/// ~1.5x: the bounding-sphere frame is looser than the silhouette).
constexpr float kTexelMatch = 0.65f;

// --- Directions ------------------------------------------------------------------------------

/// Octahedral encoding of a unit direction into [-1, 1]^2 (full sphere).
Vec2 octEncode(Vec3 dir);
Vec3 octDecode(Vec2 e);
/// Hemi-octahedral encoding of the upper hemisphere into [-1, 1]^2 (directions below the
/// horizon are clamped onto it).
Vec2 hemiOctEncode(Vec3 dir);
Vec3 hemiOctDecode(Vec2 e);

/// Continuous grid coordinates in [0, frames - 1]^2 of a view direction (model space, pointing
/// from the model center toward the viewer). Frame (x, y) sits at integer coordinates.
Vec2 gridCoord(Vec3 dir, int frames, bool hemi);
/// The capture direction of frame (x, y): from the model center toward the capture camera.
Vec3 frameDirection(int x, int y, int frames, bool hemi);

/// Orthonormal capture basis of a frame. `forward` points toward the viewer; `right` and `up`
/// span the image (u grows along right, v along up). Shaders build the same basis.
struct Basis {
    Vec3 right, up, forward;
};
Basis frameBasis(Vec3 forward);

/// The 3 frames around a view direction (a triangle of the frame grid) with barycentric
/// weights that sum to 1 and vary continuously with the direction (no popping between frames).
struct Blend {
    int x[3] = {}, y[3] = {};
    float w[3] = {};
};
Blend blendFrames(Vec3 viewDir, int frames, bool hemi);

// --- Transition ------------------------------------------------------------------------------

struct TransitionParams {
    float modelRadius = 1.f;     // bounding-sphere radius (m) at the layer's average scale
    int atlasResolution = 1024;  // requested atlas edge (px)
    int frames = kDefaultFrames;
    int screenHeight = 1080;     // pixels of the view
    float fovDeg = 55.f;         // vertical field of view
    int quality = 0;             // FrameData::quality: 0 full, 1 balanced, 2 fast
    float overrideDistance = 0;  // FoliageLayer::impostorDistance (> 0 explicit, < 0 off, 0 auto)
    float cullDistance = 100.f;  // instances beyond this are not drawn at all
};
/// Edge (px) of one frame tile for a requested atlas resolution: a multiple of 16 so the mip
/// chain stays aligned to frame boundaries.
int tileSize(int atlasResolution, int frames);
/// Atlas resolution picked for a model of this world size (bounding diameter, m).
int autoResolution(float worldDiameter);
/// Camera distance (m) where instances switch from the mesh to the impostor: where one atlas
/// texel covers about one screen pixel (scaled by the quality tier), or the override. 0 when
/// impostors would not pay off (beyond the cull distance) or are disabled.
float transitionDistance(const TransitionParams& p);
/// Width (m) of the dithered crossfade band that ends at the transition distance.
float crossfadeWidth(float transitionDistance);
/// Distance multiplier of a viewport quality tier (lower tiers switch to impostors sooner).
float qualityScale(int quality);

// --- Cache keys ------------------------------------------------------------------------------

/// Stable 64-bit FNV-1a hash.
uint64_t hash64(const std::string& s, uint64_t seed = 1469598103934665603ull);
/// Cache key of a model: hex hash of its parts (mesh keys, materials, part transforms), the
/// atlas layout, the bake version and the content stamps of the source files. Identical inputs
/// give the same key on every run and machine.
std::string cacheKey(const ImpostorModel& model);

// --- Atlas -----------------------------------------------------------------------------------

/// One level of an impostor atlas: two RGBA8 images, rows top-down.
///   albedo: sRGB color, a = coverage
///   normal: xy = model-space octahedral normal * 0.5 + 0.5, z = depth toward the viewer
///           (-radius..radius mapped to 0..1), w = subsurface
struct Atlas {
    int size = 0;    // edge in texels (= frames * tile)
    int frames = 0;  // frames per side
    std::vector<uint8_t> albedo, normal;
    int tile() const { return frames > 0 ? size / frames : 0; }
};

/// Turns a raw GPU bake (MSAA-resolved over a black, transparent background, so every channel
/// is premultiplied by coverage) into a filterable atlas: un-premultiplies, then dilates color,
/// normal and depth into the empty texels of each frame so filtering and mips never bleed black
/// (no halos). Coverage stays as baked.
void finalize(Atlas& atlas);
/// Mip chain (level 0 first, up to kMipLevels): alpha-weighted box filter, then alpha rescaled
/// per frame so that the area passing the runtime alpha test matches level 0 (distant forests
/// keep their density instead of thinning out).
std::vector<Atlas> buildMips(const Atlas& level0);
/// Fraction of a frame's texels whose coverage passes the alpha test.
float frameCoverage(const Atlas& atlas, int fx, int fy);

/// Project cache file: header + JSON metadata + zlib-compressed level 0.
Status save(const std::string& path, const Atlas& atlas, const Json& meta);
struct Loaded {
    Atlas atlas;
    Json meta;
};
Result<Loaded> load(const std::string& path);
/// Preview of the albedo atlas over a checkerboard (for agents), at most `maxSize` px wide.
Image preview(const Atlas& atlas, int maxSize = 1024);

}  // namespace sky::impostor
