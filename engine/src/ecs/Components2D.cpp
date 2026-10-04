#include "skywalker/ecs/Components2D.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<Sprite>);
static_assert(std::is_standard_layout_v<SpriteAnimator>);
static_assert(std::is_standard_layout_v<Tilemap>);
static_assert(std::is_standard_layout_v<Light2D>);
static_assert(std::is_standard_layout_v<Parallax>);
static_assert(std::is_standard_layout_v<Camera2D>);
static_assert(std::is_standard_layout_v<Text>);
static_assert(std::is_standard_layout_v<UICanvas>);
static_assert(std::is_standard_layout_v<UIElement>);
static_assert(std::is_standard_layout_v<DialogueRunner>);

#define SKY_SORTING_LAYERS "background", "midground", "default", "foreground", "overlay"

const std::vector<std::string>& Sprite::sortingLayers() {
    static const std::vector<std::string> layers{SKY_SORTING_LAYERS};
    return layers;
}

const TypeInfo& Sprite::type() {
    static const TypeInfo info{
        "sprite",
        "A textured quad on the entity's XY plane (2D characters, props, backgrounds, pixel art, 2.5D billboards). "
        "Painter's order: sortingLayer, then order, then depth.",
        {
            SKY_FIELD(Sprite, texture, String, "Image (png/jpg) or atlas (*.atlas.json), project-relative"),
            SKY_FIELD(Sprite, frame, String, "Atlas frame name or frame index (\"3\"); empty = first frame / whole image"),
            SKY_FIELD_RANGE(Sprite, columns, Int, "Grid sheet columns (frames left->right, top->bottom)", 1, 1024),
            SKY_FIELD_RANGE(Sprite, rows, Int, "Grid sheet rows", 1, 1024),
            SKY_FIELD(Sprite, region, Vec4, "Explicit pixel rect [x, y, w, h] in the image (overrides frame)"),
            SKY_FIELD(Sprite, pivot, Vec2, "Origin within the frame: [0.5, 0.5] center, [0.5, 0] feet, [0, 0] bottom-left"),
            SKY_FIELD(Sprite, size, Vec2, "World size [w, h]; 0 = pixels / pixelsPerUnit (one 0 keeps the aspect)"),
            SKY_FIELD_RANGE(Sprite, pixelsPerUnit, Float, "Texture pixels per world unit (pixel art: 16 or 32)", 0.01f, 100000.f),
            SKY_FIELD(Sprite, color, Color, "Tint and opacity"),
            SKY_FIELD(Sprite, flipX, Bool, "Mirror horizontally (face left)"),
            SKY_FIELD(Sprite, flipY, Bool, "Mirror vertically"),
            SKY_FIELD_ENUM(Sprite, sortingLayer, "Draw layer (back to front)", SKY_SORTING_LAYERS),
            SKY_FIELD(Sprite, order, Int, "Order within the layer (higher = in front)"),
            SKY_FIELD_ENUM(Sprite, filter, "Texture sampling: linear (painted art) or nearest (pixel art)", "linear", "nearest"),
            SKY_FIELD(Sprite, normalMap, String, "Normal map for 2D lighting (optional)"),
            SKY_FIELD(Sprite, emissive, Color, "Glow color, alpha = strength (blooms)"),
            SKY_FIELD_ENUM(Sprite, billboard, "Face the camera: none, y (upright, turns around Y) or full", "none", "y", "full"),
            SKY_FIELD(Sprite, lit, Bool, "Lit by 2D lights when the scene has any (else full bright)"),
            SKY_FIELD(Sprite, castShadows, Bool, "Blocks shadow-casting 2D lights"),
            SKY_FIELD_RANGE(Sprite, alphaCutoff, Float, "Hard cutout below this alpha (0 = soft edges)", 0.f, 1.f),
            SKY_FIELD(Sprite, visible, Bool, "Whether the sprite is drawn"),
            SKY_FIELD(Sprite, ySort, Bool, "Top-down depth: among ySort sprites and tile rows of the same layer and order, the one "
                                           "lower on screen (smaller world y at the pivot) draws in front"),
            SKY_FIELD(Sprite, palette, String, "Palette swap: *.palette.json ({\"swap\": {\"#3a7d44\": \"#d8e4ec\"}}) or a 2-row png "
                                               "(row 0 source colors, row 1 targets); recolors the texture (seasons, variants)"),
        }};
    return info;
}

const TypeInfo& SpriteAnimator::type() {
    static const TypeInfo info{
        "sprite_anim",
        "Flipbook animation for the entity's sprite. clips: {\"run\": {\"frames\": \"4-11\", \"fps\": 12, \"loop\": true, "
        "\"events\": {\"3\": \"footstep\"}}}. frames: index ranges (\"0-3,6\"), lists ([0, 1, 2] or [\"run_0\", ...]) or an "
        "atlas name pattern (\"run_*\"); a clip may switch sheets with \"texture\", \"columns\", \"rows\". Frame events "
        "reach the entity's behaviors as `on anim \"footstep\"`; non-looping clips send `on anim \"finished\"`.",
        {
            SKY_FIELD_JSON(SpriteAnimator, clips, "Clip name -> {frames, fps, loop, events, texture?, columns?, rows?}", R"({"type": "object"})"),
            SKY_FIELD(SpriteAnimator, clip, String, "Clip playing now (Wander: play_anim(self, \"run\"))"),
            SKY_FIELD(SpriteAnimator, playing, Bool, "Advance frames"),
            SKY_FIELD_RANGE(SpriteAnimator, speed, Float, "Playback speed multiplier", 0.f, 100.f),
        }};
    return info;
}

const TypeInfo& Tilemap::type() {
    static const TypeInfo info{
        "tilemap",
        "A grid of tiles in layers. Cell (0, 0) is the top-left at the entity position; cells go right (+x) and down "
        "(-y) in world space. Tile ids: 0 = empty, n = the n-th tile of the tileset (1-based, left->right, top->bottom). "
        "Edit with tilemap_paint / tilemap_from_ascii; layer data is compact text (\"rle:...\" or \"b64z:...\").",
        {
            SKY_FIELD(Tilemap, tileset, String, "Tileset image (grid of tiles) or *.tileset.json"),
            SKY_FIELD_RANGE(Tilemap, tileSize, Int, "Tile size in tileset pixels", 1, 4096),
            SKY_FIELD_RANGE(Tilemap, cellSize, Float, "World units per cell", 0.001f, 1000.f),
            SKY_FIELD_RANGE(Tilemap, width, Int, "Columns", 1, 4096),
            SKY_FIELD_RANGE(Tilemap, height, Int, "Rows", 1, 4096),
            SKY_FIELD_JSON(Tilemap, layers, "[{name, data, solid (true | \"tiles\"), visible, z, tint, sortingLayer, order, ySort}] bottom to top; a ySort "
                           "layer draws row by row, interleaved with ySort sprites (tall tiles: the tileset's sortOffset)",R"({"type": "array", "items": {"type": "object"}})"),
            SKY_FIELD_JSON(Tilemap, solidTiles, "Tile ids that collide in layers with solid: \"tiles\" ([3, \"10-20\"])", R"({"type": "array"})"),
            SKY_FIELD_JSON(Tilemap, autotile, "Terrains for auto-tiling: {\"wall\": {\"mode\": \"blob47\" | \"wang16\", \"first\": 33}} or \"tiles\": [ids]", R"({"type": "object"})"),
            SKY_FIELD(Tilemap, color, Color, "Tint"),
            SKY_FIELD_ENUM(Tilemap, sortingLayer, "Draw layer of all tile layers (unless a layer overrides it)", SKY_SORTING_LAYERS),
            SKY_FIELD(Tilemap, order, Int, "Order within the sorting layer (each tile layer adds its index)"),
            SKY_FIELD_ENUM(Tilemap, filter, "Texture sampling", "nearest", "linear"),
            SKY_FIELD(Tilemap, lit, Bool, "Lit by 2D lights"),
            SKY_FIELD(Tilemap, castShadows, Bool, "Solid tiles block shadow-casting 2D lights"),
            SKY_FIELD(Tilemap, palette, String, "Palette swap for the tileset image (*.palette.json or a 2-row png strip)"),
        }};
    return info;
}

const TypeInfo& Light2D::type() {
    static const TypeInfo info{
        "light2d",
        "A 2D light: point, spot (cone along the entity's local +Y) or global (ambient for the whole 2D scene). With "
        "any 2D light in the scene, lit sprites are dark except where light falls; normal maps add relief.",
        {
            SKY_FIELD_ENUM(Light2D, kind, "point | spot | global (ambient)", "point", "spot", "global"),
            SKY_FIELD(Light2D, color, Color, "Light color"),
            SKY_FIELD_RANGE(Light2D, intensity, Float, "Brightness (HDR: > 1 blooms)", 0.f, 1000.f),
            SKY_FIELD_RANGE(Light2D, radius, Float, "Reach in world units", 0.01f, 100000.f),
            SKY_FIELD_RANGE(Light2D, falloff, Float, "Attenuation exponent: 1 linear, 2 soft, 4 tight", 0.1f, 16.f),
            SKY_FIELD_RANGE(Light2D, innerAngle, Float, "Spot: full-intensity half-angle (degrees)", 0.f, 180.f),
            SKY_FIELD_RANGE(Light2D, outerAngle, Float, "Spot: cutoff half-angle (degrees)", 0.f, 180.f),
            SKY_FIELD_RANGE(Light2D, height, Float, "Height above the sprite plane (normal-map relief)", 0.f, 100.f),
            SKY_FIELD(Light2D, shadows, Bool, "Cast shadows from castShadows sprites and solid tiles"),
            SKY_FIELD_RANGE(Light2D, shadowSoftness, Float, "Penumbra softness", 0.f, 1.f),
            SKY_FIELD_RANGE(Light2D, halo, Float, "Visible glow in the air around the light", 0.f, 10.f),
            SKY_FIELD_RANGE(Light2D, flicker, Float, "Candle/torch flicker amount", 0.f, 1.f),
            SKY_FIELD_RANGE(Light2D, bands, Int, "Pixel-art falloff: 0 = smooth gradient; n = n stepped rings joined by an ordered "
                                                "dither on the art's texel grid (no 8-bit banding; 4-8 suits pixel art)", 0, 32),
        }};
    return info;
}

const TypeInfo& Parallax::type() {
    static const TypeInfo info{
        "parallax",
        "Parallax scrolling for this entity and its children (2D, orthographic cameras): they move at `factor` of the "
        "camera's motion. 0 = fixed to the camera (sky), 0.1-0.6 = far layers, 1 = normal, > 1 = near foreground.",
        {
            SKY_FIELD(Parallax, factor, Vec2, "Motion relative to the camera per axis"),
            SKY_FIELD(Parallax, origin, Vec2, "Camera position at which the layer is at its authored position"),
            SKY_FIELD(Parallax, repeatX, Bool, "Repeat sprites horizontally to fill the view (endless backgrounds)"),
            SKY_FIELD(Parallax, repeatY, Bool, "Repeat sprites vertically"),
            SKY_FIELD_RANGE(Parallax, spacing, Float, "Repeat period in world units (0 = sprite size)", 0.f, 1e6f),
        }};
    return info;
}

const TypeInfo& Camera2D::type() {
    static const TypeInfo info{
        "camera2d",
        "2D camera behavior (put it on the camera entity): orthographic, pixel-perfect integer scaling, texel "
        "snapping, smooth target following with a dead zone, and level bounds.",
        {
            SKY_FIELD_RANGE(Camera2D, pixelsPerUnit, Float, "Art texels per world unit (match the sprites)", 0.01f, 100000.f),
            SKY_FIELD_RANGE(Camera2D, referenceHeight, Int,
                            "Game pixels tall (e.g. 180 or 270): integer upscaling for crisp pixel art; 0 = camera.orthoSize", 0, 8192),
            SKY_FIELD(Camera2D, pixelSnap, Bool, "Snap the view to the texel grid (no shimmering)"),
            SKY_FIELD_RANGE(Camera2D, zoom, Float, "Zoom factor (> 1 = closer)", 0.01f, 100.f),
            SKY_FIELD_ENTITY(Camera2D, follow, "Entity to follow; empty = none"),
            SKY_FIELD_RANGE(Camera2D, smoothing, Float, "Follow lag in seconds (0 = rigid)", 0.f, 10.f),
            SKY_FIELD(Camera2D, deadZone, Vec2, "Half-size of the box the target moves in without moving the camera"),
            SKY_FIELD(Camera2D, offset, Vec2, "Framing offset from the target"),
            SKY_FIELD(Camera2D, bounds, Vec4, "[minX, minY, maxX, maxY] the view stays inside (all 0 = unbounded)"),
        }};
    return info;
}

const TypeInfo& Text::type() {
    static const TypeInfo info{
        "text",
        "Text in the world (signs, labels, damage numbers, speech bubbles). SDF-rendered: crisp at any zoom. Supports "
        "rich text: <b> <i> <u> <s> <color=#f80> <size=150%> <font=serif> <br>.",
        {
            SKY_FIELD(Text, text, String, "The text (rich text tags allowed)"),
            SKY_FIELD(Text, font, String, "Inter (default), EB Garamond (serif), JetBrains Mono (mono), or a .ttf path"),
            SKY_FIELD_RANGE(Text, size, Float, "Em size in world units", 0.001f, 10000.f),
            SKY_FIELD(Text, color, Color, "Text color"),
            SKY_FIELD_ENUM(Text, align, "Horizontal alignment around the entity", "left", "center", "right", "justify"),
            SKY_FIELD_ENUM(Text, valign, "Vertical alignment around the entity", "top", "middle", "bottom"),
            SKY_FIELD_RANGE(Text, maxWidth, Float, "Wrap width in world units (0 = no wrap)", 0.f, 1e6f),
            SKY_FIELD_RANGE(Text, lineSpacing, Float, "Line height multiplier", 0.1f, 10.f),
            SKY_FIELD_RANGE(Text, outline, Float, "Outline width in em", 0.f, 0.15f),
            SKY_FIELD(Text, outlineColor, Color, "Outline color"),
            SKY_FIELD(Text, shadowColor, Color, "Drop shadow color (alpha 0 = none)"),
            SKY_FIELD(Text, shadowOffset, Vec2, "Drop shadow offset in world units"),
            SKY_FIELD_RANGE(Text, emissive, Float, "Glow strength (neon signs bloom)", 0.f, 100.f),
            SKY_FIELD_ENUM(Text, billboard, "Face the camera", "none", "y", "full"),
            SKY_FIELD_ENUM(Text, sortingLayer, "Draw layer", SKY_SORTING_LAYERS),
            SKY_FIELD(Text, order, Int, "Order within the layer"),
            SKY_FIELD(Text, visible, Bool, "Whether the text is drawn"),
        }};
    return info;
}

const TypeInfo& UICanvas::type() {
    static const TypeInfo info{
        "ui_canvas",
        "Root of a user interface. Children with a `ui` component are laid out in canvas pixels (origin top-left, y "
        "down). screen = overlay scaled from referenceResolution; world = a panel in the scene. Build whole UIs with "
        "ui_create; restyle with ui_style.",
        {
            SKY_FIELD_ENUM(UICanvas, mode, "screen overlay or world-space panel", "screen", "world"),
            SKY_FIELD(UICanvas, referenceResolution, Vec2, "Design resolution [w, h] in pixels"),
            SKY_FIELD_ENUM(UICanvas, scaleMode, "scale_with_screen (resolution independent) or constant (1 px = 1 px)",
                           "scale_with_screen", "constant"),
            SKY_FIELD_RANGE(UICanvas, match, Float, "Scaling: 0 = match width, 1 = match height", 0.f, 1.f),
            SKY_FIELD(UICanvas, sortOrder, Int, "Canvases draw in ascending order"),
            SKY_FIELD_ENUM(UICanvas, theme, "Built-in look", "dark", "light", "parchment", "glass", "pixel"),
            SKY_FIELD(UICanvas, styleSheet, String, "Style sheet (*.uistyle.json) layered over the theme"),
            SKY_FIELD_RANGE(UICanvas, worldScale, Float, "World mode: world units per canvas pixel", 0.00001f, 100.f),
            SKY_FIELD(UICanvas, interactable, Bool, "Receives mouse/keyboard input"),
        }};
    return info;
}

const TypeInfo& UIElement::type() {
    static const TypeInfo info{
        "ui",
        "A UI element under a ui_canvas. Rect: anchor preset + position (offset, y down) + size, or stretch with "
        "margin. A layout (row/column/grid) arranges children with gap/padding/align/justify; fit sizes to content. "
        "Look comes from the theme/style sheet by widget kind and `style` classes, plus styleOverrides. Activating "
        "a button/toggle/slider sends Wander `on ui \"<name>\"` (and `on click` to the element).",
        {
            SKY_FIELD_ENUM(UIElement, widget, "What it is", "panel", "image", "text", "button", "toggle", "slider", "progress",
                           "scroll", "input", "spacer"),
            SKY_FIELD_ENUM(UIElement, anchor,
                           "Where it attaches in its parent: corners/edges/center, fill, or *_stretch bands; custom uses "
                           "anchorMin/anchorMax/pivot",
                           "top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right",
                           "fill", "top_stretch", "middle_stretch", "bottom_stretch", "left_stretch", "center_stretch",
                           "right_stretch", "custom"),
            SKY_FIELD(UIElement, anchorMin, Vec2, "custom anchor: parent fraction [x, y] (0,0 = top-left)"),
            SKY_FIELD(UIElement, anchorMax, Vec2, "custom anchor: parent fraction [x, y]; differs from anchorMin = stretch"),
            SKY_FIELD(UIElement, pivot, Vec2, "custom anchor: the element's own reference point [x, y] (0,0 = top-left)"),
            SKY_FIELD(UIElement, position, Vec2, "Offset from the anchor in pixels [x, y] (y down)"),
            SKY_FIELD(UIElement, size, Vec2, "Size in pixels [w, h] (ignored on stretched axes)"),
            SKY_FIELD(UIElement, margin, Vec4, "Insets on stretched axes [top, right, bottom, left]"),
            SKY_FIELD_ENUM(UIElement, layout, "Arrange children", "none", "row", "column", "grid"),
            SKY_FIELD_RANGE(UIElement, gap, Float, "Space between children (pixels)", 0.f, 100000.f),
            SKY_FIELD(UIElement, padding, Vec4, "Inner padding [top, right, bottom, left] (CSS shorthand ok: 12 or [8, 16])"),
            SKY_FIELD_ENUM(UIElement, align, "Children on the cross axis", "start", "center", "end", "stretch"),
            SKY_FIELD_ENUM(UIElement, justify, "Children on the main axis", "start", "center", "end", "space_between"),
            SKY_FIELD_RANGE(UIElement, columns, Int, "Grid columns", 1, 256),
            SKY_FIELD_ENUM(UIElement, fit, "Size to content (children or text)", "none", "width", "height", "both"),
            SKY_FIELD_RANGE(UIElement, flex, Float, "Share of leftover space in a row/column (0 = fixed size)", 0.f, 1000.f),
            SKY_FIELD(UIElement, ignoreLayout, Bool, "Placed by its own anchor even inside a layout"),
            SKY_FIELD(UIElement, text, String, "Label or content (rich text)"),
            SKY_FIELD(UIElement, image, String, "Image path or atlas frame (\"ui.atlas.json#heart\")"),
            SKY_FIELD(UIElement, value, Float, "Slider/progress value; toggle: 0 or 1"),
            SKY_FIELD(UIElement, minValue, Float, "Slider/progress minimum"),
            SKY_FIELD(UIElement, maxValue, Float, "Slider/progress maximum"),
            SKY_FIELD(UIElement, placeholder, String, "Input: hint shown when empty"),
            SKY_FIELD(UIElement, style, String, "Style classes, space separated (\"primary large\")"),
            SKY_FIELD_JSON(UIElement, styleOverrides, "Inline style properties ({\"background\": \"#223\", \"radius\": 12})", R"({"type": "object"})"),
            SKY_FIELD(UIElement, event, String, "Extra event sent when activated (in addition to ui:<name>)"),
            SKY_FIELD(UIElement, interactable, Bool, "Receives input (disabled look when false)"),
            SKY_FIELD(UIElement, visible, Bool, "Shown (hidden elements and their children take no space)"),
            SKY_FIELD(UIElement, scroll, Float, "Scroll views: content offset in pixels"),
        }};
    return info;
}

const TypeInfo& DialogueRunner::type() {
    static const TypeInfo info{
        "dialogue",
        "Runs a .dialogue script (Yarn-style: nodes, `Speaker: line #tags`, `-> choices`, <<set>>, <<if>>, <<jump>>, "
        "custom <<commands>> sent to Wander as `on dialogue \"name\"`) with a built-in, restylable dialogue box. "
        "Start it with Wander start_dialogue(\"Node\"); lint with dialogue_check; simulate with dialogue_preview.",
        {
            SKY_FIELD(DialogueRunner, script, String, "Dialogue file (*.dialogue), project-relative"),
            SKY_FIELD(DialogueRunner, source, String, "Inline script (used when script is empty)"),
            SKY_FIELD(DialogueRunner, startNode, String, "Node to start at"),
            SKY_FIELD(DialogueRunner, autoStart, Bool, "Start when play begins"),
            SKY_FIELD_ENUM(DialogueRunner, ui, "default = built-in dialogue box; none = drive your own UI", "default", "none"),
            SKY_FIELD_RANGE(DialogueRunner, typewriter, Float, "Characters per second (0 = instant)", 0.f, 10000.f),
            SKY_FIELD(DialogueRunner, portraits, String, "Folder for #portrait:<name> tags (<name>.png)"),
            SKY_FIELD(DialogueRunner, running, Bool, "State: a conversation is in progress"),
            SKY_FIELD(DialogueRunner, node, String, "State: current node"),
            SKY_FIELD(DialogueRunner, speaker, String, "State: speaker of the current line"),
            SKY_FIELD(DialogueRunner, line, String, "State: current line text"),
            SKY_FIELD_JSON(DialogueRunner, choices, "State: current choices (array of strings)", R"({"type": "array", "items": {"type": "string"}})"),
            SKY_FIELD_JSON(DialogueRunner, tags, "State: tags of the current line ({\"mood\": \"angry\"})", R"({"type": "object"})"),
        }};
    return info;
}

}  // namespace sky
