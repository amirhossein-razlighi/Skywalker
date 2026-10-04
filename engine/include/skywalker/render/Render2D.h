#pragma once
// 2D, text and UI frame data (included by Renderer.h).
//
//   Scene --(render2d::gather2D / ui::UiSystem, CPU)--> Frame2D --(Metal / CPU raster)--> pixels
//
// World-space quads (sprites, tiles, world text glyphs, 2D light halos) are SpriteInstances drawn
// in the scene pass with depth testing, in painter's order (sorting layer, order, depth). Screen
// and world-space UI is a list of UIQuads drawn after post-processing at output resolution.
// Both lists are plain data, so every backend (and the headless CPU rasterizer) draws the same.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/math/Math.h"

namespace sky {

/// A CPU-side texture: font atlas pages and other generated images. Renderers upload it and
/// re-upload whenever `version` changes.
struct TextureImage {
    std::string key;    // stable identity, e.g. "font:Inter#0"
    int width = 0;
    int height = 0;
    int channels = 4;   // 1 = R8 (SDF glyphs, linear), 4 = RGBA8 (sRGB color)
    std::vector<uint8_t> pixels;
    uint64_t version = 0;
};
using TextureImagePtr = std::shared_ptr<const TextureImage>;

/// The texture of a 2D draw: an image file (absolute path once a frame is built) or an in-memory image.
struct TextureRef {
    std::string path;
    TextureImagePtr image;

    bool empty() const { return path.empty() && !image; }
    const std::string& key() const { return image ? image->key : path; }
    bool operator==(const TextureRef& o) const { return path == o.path && image == o.image; }
};

enum class SpriteMode : int {
    Color = 0,  // RGBA texture (or white when untextured)
    Sdf = 1,    // single-channel signed-distance glyph (world text)
    Halo = 2,   // soft radial glow of a 2D light (additive, untextured)
};

/// One world-space quad (sprite, tile, world-text glyph, light halo). 32 floats; must match Sprite2D.metal.
struct SpriteInstance {
    float origin[4];    // xyz = world position of the quad corner at uv (u0, v0); w = fog factor 0..1
    float axisX[4];     // xyz = world edge from uv u0 -> u1;  w = unused
    float axisY[4];     // xyz = world edge from uv v0 -> v1;  w = unused
    float uv[4];        // u0, v0, u1, v1 (texture space, v down)
    float color[4];     // linear rgba tint (straight alpha)
    float emission[4];  // Color: linear rgb * strength (adds light); Sdf: outline rgb, a = outline width (0..0.5)
    float params[4];    // x = SpriteMode, y = lit (0/1), z = normal map strength (0 = none), w = SDF dilation (bold)
    float extra[4];     // x = casts 2D shadows, y = alpha cutoff, zw = unused
};
static_assert(sizeof(SpriteInstance) == 32 * sizeof(float));

/// A run of consecutive SpriteInstances sharing textures and sampling.
struct SpriteBatch {
    TextureRef texture;     // empty = white
    TextureRef normalMap;   // optional (Color mode)
    bool nearest = false;   // point sampling (pixel art)
    bool additive = false;  // halos
    uint32_t first = 0;
    uint32_t count = 0;
};

/// A 2D light (point, spot or global/ambient), in world space.
struct Light2DItem {
    enum class Kind : int { Global = 0, Point = 1, Spot = 2 } kind = Kind::Point;
    Vec3 position;
    Vec3 direction{0, 1, 0};  // spot axis (unit)
    Vec3 color{1, 1, 1};      // linear rgb * intensity
    float radius = 5.f;       // world units
    float falloff = 1.f;      // exponent of the distance attenuation
    float cosInner = 1.f;     // spot cone
    float cosOuter = 0.f;
    float height = 0.5f;      // light height above the sprite plane (normal mapping)
    bool shadows = false;
    float shadowSoftness = 0.5f;
};

/// UI primitive kinds (UI.metal and the CPU rasterizer interpret them identically).
enum class UIQuadKind : int {
    Rect = 0,    // rounded rectangle: vertical gradient color -> color2, border, corner radius
    Image = 1,   // textured quad tinted by color (rounded corners clip the image)
    Glyph = 2,   // SDF glyph: color, outline color2/width, dilation
    Shadow = 3,  // soft drop shadow of a rounded rect (blur radius in params2.x)
};

/// One UI quad in canvas pixels (y down). 32 floats; must match UI.metal.
struct UIQuad {
    float rect[4];         // x, y, w, h
    float uv[4];           // u0, v0, u1, v1
    float color[4];        // sRGB rgba (straight alpha)
    float color2[4];       // Rect: gradient bottom color; Glyph: outline color
    float borderColor[4];  // sRGB rgba
    float params[4];       // x = UIQuadKind, y = corner radius, z = border width, w = opacity
    float params2[4];      // Rect/Shadow: x = blur; Glyph: x = dilation, y = outline width (0..0.5), z = italic shear
    float clip[4];         // x0, y0, x1, y1 clip rectangle in pixels (scroll views); x1 < x0 = no clip
};
static_assert(sizeof(UIQuad) == 32 * sizeof(float));

struct UIBatch {
    TextureRef texture;  // empty = untextured
    bool nearest = false;
    int canvas = 0;      // index into Frame2D::uiCanvases
    uint32_t first = 0;
    uint32_t count = 0;
};

/// A UI canvas: screen overlays map canvas pixels to the output; world canvases live in the scene.
struct UICanvasItem {
    bool world = false;
    Mat4 model;  // world canvases: canvas pixels (y down) -> world
};

/// Where a 2D entity ended up on screen (for agents: captures, picking).
struct ScreenBox {
    uint64_t entity = 0;
    float x = 0, y = 0, w = 0, h = 0;  // pixels, top-left origin
    float depth = 0;                   // distance from the camera (UI: 0)
    int64_t order = 0;                 // draw order (higher = on top)
    bool ui = false;
};

struct Frame2D {
    std::vector<SpriteInstance> sprites;  // painter's order
    std::vector<SpriteBatch> spriteBatches;
    std::vector<Light2DItem> lights;      // point/spot lights (up to kMaxLights are used)
    Vec3 ambient{1, 1, 1};                // linear; sum of global 2D lights
    bool lit = false;                     // any 2D light in the scene: lit sprites use 2D lighting
    std::vector<UIQuad> ui;
    std::vector<UIBatch> uiBatches;
    std::vector<UICanvasItem> uiCanvases;
    std::vector<ScreenBox> boxes;

    static constexpr size_t kMaxLights = 32;
    bool empty() const { return sprites.empty() && ui.empty(); }
};

}  // namespace sky
