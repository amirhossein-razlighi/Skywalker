#pragma once
// 2D, text, UI and dialogue components. Included at the end of Components.h; tables in
// Components2D.cpp. Conventions:
//   * World 2D lives on the XY plane (+Y up, the camera looks down -Z). A sprite's z orders
//     2.5D scenes and drives fog; painter's order is (sortingLayer, order, depth).
//   * UI is in canvas pixels with the origin at the TOP-LEFT and y DOWN (like screenshots and
//     the boxes viewport_capture returns). Paddings and margins use CSS order [top, right, bottom, left].

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// A textured quad in the world: characters, props, backgrounds, pixel art, 2.5D billboards.
struct Sprite {
    std::string texture;              // image (png/jpg) or atlas (*.atlas.json), project-relative
    std::string frame;                // atlas frame name, or frame index ("3"); "" = first frame / whole image
    int columns = 1;                  // grid sheet: the image is columns x rows frames (index left->right, top->bottom)
    int rows = 1;
    Vec4 region{0.f, 0.f, 0.f, 0.f};  // explicit pixel rect [x, y, w, h] (overrides frame when w, h > 0)
    Vec2 pivot{0.5f, 0.5f};           // 0,0 = bottom-left, 1,1 = top-right of the frame
    Vec2 size{0.f, 0.f};              // world size; 0 = frame pixels / pixelsPerUnit (keeps aspect if one is 0)
    float pixelsPerUnit = 100.f;      // texture pixels per world unit (pixel art: 16 or 32)
    Vec4 color{1.f, 1.f, 1.f, 1.f};   // tint and opacity
    bool flipX = false;
    bool flipY = false;
    std::string sortingLayer = "default";  // background | midground | default | foreground | overlay
    int order = 0;                    // order within the layer (higher draws on top)
    std::string filter = "linear";    // linear | nearest (pixel art)
    std::string normalMap;            // tangent-space normal map for 2D lighting
    Vec4 emissive{0.f, 0.f, 0.f, 1.f};  // glow color, alpha = strength (blooms in HDR)
    std::string billboard = "none";   // none | y (turns around Y to face the camera) | full
    bool lit = true;                  // affected by 2D lights (when the scene has any)
    bool castShadows = false;         // blocks 2D lights that cast shadows
    float alphaCutoff = 0.f;          // > 0: hard cutout below this alpha
    bool visible = true;
    bool ySort = false;               // top-down depth: within its layer and order, lower on screen (smaller y) draws in front
    std::string palette;              // palette swap (*.palette.json or a 2-row png strip): recolors the texture

    static const TypeInfo& type();
    static const std::vector<std::string>& sortingLayers();
};

/// Flipbook animation for a sprite: named clips of frames, a current clip, frame events.
struct SpriteAnimator {
    Json clips = Json::object();  // {"run": {"frames": "4-11", "fps": 12, "loop": true, "events": {"3": "footstep"}}}
    std::string clip;             // clip playing now (Wander: play_anim(self, "run"))
    bool playing = true;
    float speed = 1.f;

    // Runtime state (not serialized): owned by the animation system.
    float time_ = 0.f;
    std::string active_;
    int lastFrame_ = -1;
    bool finished_ = false;

    static const TypeInfo& type();
};

/// A grid of tiles from a tileset image, in layers, with collision flags and auto-tiling.
struct Tilemap {
    std::string tileset;            // image (grid of tiles) or *.tileset.json
    int tileSize = 16;              // tile size in tileset pixels
    float cellSize = 1.f;           // world units per cell
    int width = 16;                 // cells
    int height = 16;
    Json layers = Json::array();    // [{"name", "data", "solid", "visible", "z", "tint", "sortingLayer", "order"}]
    Json solidTiles = Json::array();  // tile ids that collide in layers with "solid": "tiles" ([3, "10-20"])
    Json autotile = Json::object();   // {"wall": {"mode": "blob47" | "wang16", "first": 33}} or {"tiles": [...]}
    Vec4 color{1.f, 1.f, 1.f, 1.f};
    std::string sortingLayer = "background";
    int order = 0;
    std::string filter = "nearest";
    bool lit = true;
    bool castShadows = false;       // solid tiles block shadow-casting 2D lights
    std::string palette;            // palette swap for the tileset image (*.palette.json or a 2-row png strip)

    // Runtime cache key (not serialized).
    uint64_t revision_ = 0;

    static const TypeInfo& type();
};

/// A 2D light: point, spot (cone along local +Y) or global (ambient). Lights normal-mapped sprites.
struct Light2D {
    std::string kind = "point";  // point | spot | global
    Vec4 color{1.f, 0.92f, 0.8f, 1.f};
    float intensity = 1.f;
    float radius = 6.f;          // world units
    float falloff = 2.f;         // attenuation exponent: 1 = linear, 2 = soft, 4 = tight
    float innerAngle = 25.f;     // spot (degrees, half-angle)
    float outerAngle = 45.f;
    float height = 1.f;          // height above the sprite plane (normal-map shading)
    bool shadows = false;        // shadow-casting sprites/tiles block it
    float shadowSoftness = 0.4f;
    float halo = 0.f;            // visible glow in the air around the light (additive)
    float flicker = 0.f;         // 0..1 candle/torch flicker (deterministic)
    int bands = 0;               // pixel-art falloff: 0 = smooth, n = n brightness steps blended by ordered (Bayer) dither

    static const TypeInfo& type();
};

/// Parallax for an entity and its children: with an orthographic camera they move at `factor`
/// of the camera's speed (0 = fixed to the camera like a sky, < 1 far, > 1 near foreground).
struct Parallax {
    Vec2 factor{0.5f, 0.5f};
    Vec2 origin{0.f, 0.f};      // camera position where the layer sits at its authored position
    bool repeatX = false;       // tile sprites horizontally to fill the view
    bool repeatY = false;
    float spacing = 0.f;        // repeat period in world units (0 = sprite width/height)

    static const TypeInfo& type();
};

/// 2D camera behavior on a camera entity: pixel-perfect orthographic projection, following a
/// target, bounds.
struct Camera2D {
    float pixelsPerUnit = 16.f;   // texels per world unit of the art
    int referenceHeight = 0;      // game pixels tall (e.g. 180): integer upscaling; 0 = use camera.orthoSize
    bool pixelSnap = true;        // snap the camera to the texel grid (no shimmering)
    float zoom = 1.f;
    EntityLink follow;            // entity to follow (empty = none)
    float smoothing = 0.15f;      // follow lag in seconds (0 = rigid)
    Vec2 deadZone{0.f, 0.f};      // half-size of the box the target moves in freely (world units)
    Vec2 offset{0.f, 0.f};        // framing offset from the target
    Vec4 bounds{0.f, 0.f, 0.f, 0.f};  // [minX, minY, maxX, maxY] the view stays inside (all 0 = none)

    static const TypeInfo& type();
};

/// Text in the world (signs, labels, damage numbers, speech). Rich text, SDF-crisp at any zoom.
struct Text {
    std::string text = "Text";
    std::string font;                  // Inter | EB Garamond | JetBrains Mono | serif | mono | path.ttf
    float size = 0.5f;                 // em size in world units
    Vec4 color{1.f, 1.f, 1.f, 1.f};
    std::string align = "center";      // left | center | right | justify
    std::string valign = "middle";     // top | middle | bottom (relative to the entity)
    float maxWidth = 0.f;              // wrap width in world units (0 = none)
    float lineSpacing = 1.f;
    float outline = 0.f;               // em (0 .. 0.15)
    Vec4 outlineColor{0.f, 0.f, 0.f, 1.f};
    Vec4 shadowColor{0.f, 0.f, 0.f, 0.f};  // alpha 0 = no shadow
    Vec2 shadowOffset{0.04f, -0.04f};
    float emissive = 0.f;              // HDR glow strength (neon signs)
    std::string billboard = "none";    // none | y | full
    std::string sortingLayer = "default";
    int order = 0;
    bool visible = true;

    static const TypeInfo& type();
};

/// Root of a UI: a screen overlay (HUDs, menus) or a panel in the world (diegetic UI).
struct UICanvas {
    std::string mode = "screen";             // screen | world
    Vec2 referenceResolution{1920.f, 1080.f};
    std::string scaleMode = "scale_with_screen";  // scale_with_screen | constant
    float match = 0.5f;                      // 0 = match width, 1 = match height
    int sortOrder = 0;                       // canvases draw in this order
    std::string theme = "dark";              // built-in look: dark | light | parchment | glass | pixel
    std::string styleSheet;                  // *.uistyle.json overriding the theme
    float worldScale = 0.01f;                // world mode: world units per canvas pixel
    bool interactable = true;

    static const TypeInfo& type();
};

/// One UI element (child of a canvas or of another element): rect, layout, widget and style.
struct UIElement {
    std::string widget = "panel";  // panel | image | text | button | toggle | slider | progress | scroll | input | spacer
    std::string anchor = "top_left";  // preset (see docs) or custom (anchorMin/anchorMax/pivot)
    Vec2 anchorMin{0.f, 0.f};
    Vec2 anchorMax{0.f, 0.f};
    Vec2 pivot{0.f, 0.f};
    Vec2 position{0.f, 0.f};       // offset from the anchor point (pixels, y down)
    Vec2 size{160.f, 40.f};        // pixels (ignored on stretched axes)
    Vec4 margin{0.f, 0.f, 0.f, 0.f};  // stretched axes: insets [top, right, bottom, left]
    std::string layout = "none";   // none | row | column | grid (arranges children)
    float gap = 0.f;
    Vec4 padding{0.f, 0.f, 0.f, 0.f};
    std::string align = "start";   // children on the cross axis: start | center | end | stretch
    std::string justify = "start"; // children on the main axis: start | center | end | space_between
    int columns = 2;               // grid
    std::string fit = "none";      // size to content: none | width | height | both
    float flex = 0.f;              // in a row/column parent: share of the leftover space
    bool ignoreLayout = false;
    std::string text;              // label / content (rich text)
    std::string image;             // texture or atlas ("ui.atlas.json#heart")
    float value = 0.f;             // slider / progress / toggle (0/1)
    float minValue = 0.f;
    float maxValue = 1.f;
    std::string placeholder;       // input
    std::string style;             // style classes from the theme/style sheet ("primary large")
    Json styleOverrides = Json::object();  // inline style ({"background": "#223", "radius": 12})
    std::string event;             // extra event emitted when activated (default: "ui:<name>")
    bool interactable = true;
    bool visible = true;
    float scroll = 0.f;            // scroll views: content offset (pixels)

    static const TypeInfo& type();
};

/// Runs a .dialogue script (Yarn-style nodes, lines, choices, variables, commands) and drives a
/// restylable dialogue UI (portrait, name plate, typewriter text, choices).
struct DialogueRunner {
    std::string script;            // *.dialogue path
    std::string source;            // or the script inline
    std::string startNode = "Start";
    bool autoStart = false;        // start when play begins
    std::string ui = "default";    // default (built-in dialogue box) | none (drive your own UI)
    float typewriter = 40.f;       // characters per second (0 = instant)
    std::string portraits = "portraits";  // folder for #portrait:<name> tags (<name>.png)
    // State, written by the runner (read it from Wander: self.dialogue.speaker ...).
    bool running = false;
    std::string node;
    std::string speaker;
    std::string line;
    Json choices = Json::array();
    Json tags = Json::object();

    static const TypeInfo& type();
};

}  // namespace sky
