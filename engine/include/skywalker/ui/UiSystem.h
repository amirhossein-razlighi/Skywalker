#pragma once
// The UI system: layout, styling, drawing, hit testing and input for ui_canvas / ui entities.
//
//   Scene (ui_canvas + ui entities) --computeLayout--> Layout (rects, resolved styles)
//        --build--> Frame2D UI quads (drawn by Metal and by the CPU rasterizer)
//        --tick(pointer, keys)--> hover/press/focus, widget values, Wander events ("ui:<name>")
//
// Layout is a pure function of the scene, the viewport size and the interaction state, so the
// same scene always produces the same rects (agents read them with ui_inspect).

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/render/Renderer.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/text/TextLayout.h"
#include "skywalker/ui/UiStyle.h"

namespace sky::ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(Vec4 p) const { return {x + p.w, y + p.x, std::max(0.f, w - p.y - p.w), std::max(0.f, h - p.x - p.z)}; }
    Rect scaled(float s) const { return {x * s, y * s, w * s, h * s}; }
    bool operator==(const Rect&) const = default;
};

struct Node {
    EntityId entity = kNoEntity;
    std::string name;
    int canvas = -1;   // index into Layout::canvases
    int parent = -1;   // node index; -1 = directly under the canvas
    std::vector<int> children;
    int depth = 0;
    UIElement el;      // copy of the component at layout time
    std::vector<std::string> classes;
    Style style;       // resolved for the current state
    Style baseStyle;   // resolved without hover/pressed (for transitions)
    StyleState state;
    Rect rect;         // canvas units (logical pixels, y down)
    Rect clip{0, 0, -1, -1};  // canvas units; w < 0 = unclipped
    bool visible = true;
    bool interactable = true;
    float opacity = 1;
    Vec2 contentSize;  // measured content (scroll views)
    float maxScroll = 0;
};

struct CanvasInfo {
    EntityId entity = kNoEntity;
    UICanvas canvas;
    std::shared_ptr<const StyleSheet> sheet;
    float scale = 1;     // output pixels per canvas unit (screen canvases)
    Vec2 size;           // canvas units
    Mat4 model;          // world canvases: canvas units -> world
    std::vector<int> roots;
};

struct Layout {
    int width = 0, height = 0;  // viewport the layout was computed for
    std::vector<CanvasInfo> canvases;  // in draw order (sortOrder)
    std::vector<Node> nodes;           // canvases in order, each depth-first (draw order)
    std::unordered_map<EntityId, int> index;

    const Node* find(EntityId e) const;
    /// The node's rect in output pixels (screen canvases) or canvas units (world canvases).
    Rect screenRect(const Node& n) const;
};

/// Pointer and keyboard input the UI consumes each tick.
struct UiInput {
    float x = -1, y = -1;     // pointer in viewport pixels (negative = no pointer)
    int width = 0, height = 0;  // viewport the pointer coordinates refer to
    bool down = false;
    float wheel = 0;          // scroll lines (positive = up)
    std::vector<std::string> keys;  // keys pressed this tick ("tab", "enter", "space", "up", "backspace", ...)
    std::string text;         // characters typed this tick
};

/// What the UI reports to the engine when widgets are used.
struct UiEvents {
    std::function<void(const std::string& event, EntityId target)> emit;  // Wander events
    std::function<void(EntityId)> click;                                  // `on click` of the element
    std::function<bool(EntityId)> activate;  // return true to consume (dialogue choices)
};

class UiSystem {
public:
    explicit UiSystem(render2d::Assets2D& assets) : assets_(assets) {}

    /// Lays out every canvas for a viewport. `camera` places world canvases (optional).
    Layout computeLayout(const Scene& scene, int width, int height) const;
    /// Appends UI quads for all canvases to frame.render2d (screen canvases at the frame's size).
    void build(const Scene& scene, FrameData& frame, float time);

    /// Processes one fixed tick of input: hover, press, drag, focus, keyboard and wheel.
    void tick(Scene& scene, const UiInput& input, float dt, const ViewCamera* camera, const UiEvents& events);
    /// Activates a widget as if clicked (buttons, toggles, inputs submit). Used by tools and keys.
    void activate(Scene& scene, EntityId element, const UiEvents& events);

    /// Topmost element under a viewport point (any visible element with a look, or interactive),
    /// using a layout for that viewport. `interactiveOnly` walks up to the nearest interactive ancestor.
    EntityId hitTest(const Scene& scene, float x, float y, int width, int height, const ViewCamera* camera, bool interactiveOnly) const;

    /// Typewriter reveal: only the first `chars` characters of an element's text are drawn (-1 = all).
    void setReveal(EntityId e, int chars);
    void clearReveal(EntityId e) { reveal_.erase(e); }
    void reset();

    /// Editor selection: selected elements get an outline.
    void setSelection(std::vector<EntityId> selection) { selection_ = std::move(selection); }
    EntityId hovered() const { return hovered_; }
    EntityId focused() const { return focused_; }
    void setFocus(EntityId e) { focused_ = e; }

    static bool interactiveWidget(const std::string& widget);
    /// Lays out text the way UI elements do (shared with the inspector).
    text::TextLayout layoutText(const std::string& text, const Style& style, float scale, float maxWidth, bool wrap) const;
    render2d::Assets2D& assets() const { return assets_; }

private:
    EntityId hitIn(const Layout& layout, float x, float y, const ViewCamera* camera, bool interactiveOnly) const;
    StyleState stateFor(EntityId e, const UIElement& el) const;

    render2d::Assets2D& assets_;
    EntityId hovered_ = kNoEntity, pressed_ = kNoEntity, focused_ = kNoEntity, dragging_ = kNoEntity;
    bool wasDown_ = false;
    int lastWidth_ = 1920, lastHeight_ = 1080;
    std::unordered_map<EntityId, float> blend_;  // hover/pressed transition 0..1
    std::unordered_map<EntityId, int> reveal_;
    std::vector<EntityId> selection_;
    mutable std::unordered_map<std::string, text::TextLayout> textCache_;
    float time_ = 0;
};

/// Anchor presets: anchorMin, anchorMax, pivot (all y down).
bool anchorPreset(const std::string& name, Vec2& anchorMin, Vec2& anchorMax, Vec2& pivot);
/// Effective content padding of a node (the element's padding, else its style's).
Vec4 contentPadding(const Node& n);

}  // namespace sky::ui
