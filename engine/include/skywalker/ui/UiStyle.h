#pragma once
// UI styles: themes, style sheets (*.uistyle.json), cascading and states.
//
// A style sheet maps selectors to properties:
//   {"format": "skywalker.uistyle", "extends": "dark", "vars": {"accent": "#e8a33d"},
//    "rules": {"button": {"background": "$accent", "radius": 10, "hover": {"background": "#f0b85a"}},
//              ".danger": {"background": "#c0392b"},
//              "#PlayButton": {"fontSize": 28}}}
// Selectors: "canvas" (root text defaults), a widget kind ("button"), a class (".primary", from the
// element's `style`), or an entity name ("#Title"). Later, more specific rules win:
// kind < class < name < the element's styleOverrides. State blocks (hover, pressed, focus, checked,
// disabled) apply on top while the state is active. Text properties inherit from the parent element.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky::ui {

/// Resolved look of one element in its current state.
struct Style {
    // Box
    Vec4 background{0, 0, 0, 0};
    Vec4 background2{0, 0, 0, 0};  // gradient bottom color (alpha 0 = flat)
    std::string backgroundImage;   // image or atlas frame drawn as the background
    Vec4 slice{0, 0, 0, 0};        // 9-slice borders in image pixels [top, right, bottom, left]
    Vec4 imageTint{1, 1, 1, 1};
    float radius = 0;
    float borderWidth = 0;
    Vec4 borderColor{0, 0, 0, 0};
    Vec4 shadowColor{0, 0, 0, 0};
    Vec2 shadowOffset{0, 4};
    float shadowBlur = 12;
    float opacity = 1;
    Vec4 padding{0, 0, 0, 0};      // content inset when the element has none [top, right, bottom, left]
    // Text (inherited)
    std::string font;
    float fontSize = 18;
    Vec4 color{1, 1, 1, 1};
    std::string textAlign = "left";      // left | center | right | justify
    std::string verticalAlign = "middle";  // top | middle | bottom
    bool bold = false;
    bool italic = false;
    float lineSpacing = 1;
    float letterSpacing = 0;
    std::string textTransform;           // "" | uppercase | lowercase
    float textOutline = 0;
    Vec4 textOutlineColor{0, 0, 0, 1};
    Vec4 textShadowColor{0, 0, 0, 0};
    Vec2 textShadowOffset{0, 1};
    // Widgets
    Vec4 accent{0.95f, 0.55f, 0.2f, 1};  // progress/slider fill, toggle check, caret
    Vec4 track{1, 1, 1, 0.12f};          // slider/progress/scrollbar track
    Vec4 knob{1, 1, 1, 1};
    float trackHeight = 6;
    float knobSize = 18;
    Vec4 placeholderColor{1, 1, 1, 0.4f};
    std::string imageFit = "contain";    // image widgets: stretch | contain | cover
    float transition = 0.1f;             // seconds to blend hover/pressed looks
};

/// The interaction state an element is drawn in.
struct StyleState {
    bool hover = false, pressed = false, focus = false, checked = false, disabled = false;
};

class StyleSheet {
public:
    /// Built-in themes: dark, light, parchment, glass, pixel.
    static const std::vector<std::string>& themeNames();
    static std::shared_ptr<const StyleSheet> theme(const std::string& name);
    /// Parses a style sheet document; `base` is the theme it extends (unless it names one).
    static Result<std::shared_ptr<const StyleSheet>> parse(const Json& doc, std::shared_ptr<const StyleSheet> base);

    /// Property bag for an element (before states), merged in cascade order.
    Json cascade(const std::string& widget, const std::vector<std::string>& classes, const std::string& entityName) const;
    /// Applies a property bag on top of a style (resolving $vars). Unknown keys are ignored.
    void apply(const Json& props, Style& style) const;
    /// Full resolution: cascade + overrides + states, with text inheritance from `parent`.
    Style resolve(const std::string& widget, const std::vector<std::string>& classes, const std::string& entityName,
                  const Json& overrides, const StyleState& state, const Style* parent) const;
    /// Root text defaults (the "canvas" rule).
    Style root() const;

    const Json& vars() const { return vars_; }
    const Json& rules() const { return rules_; }
    std::string name;

    /// Validates a property bag; returns errors with did-you-mean hints for unknown properties.
    static Status validate(const Json& props, const std::string& where);
    static const std::vector<std::string>& propertyNames();

private:
    Json vars_ = Json::object();
    Json rules_ = Json::object();  // selector -> properties (already merged with the base sheet)
};

}  // namespace sky::ui
