#include "skywalker/ui/UiStyle.h"

#include <algorithm>
#include <mutex>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/text/TextLayout.h"

namespace sky::ui {

namespace {

// Built-in themes. Every theme styles every widget kind plus a shared set of classes
// (primary, ghost, danger, title, heading, subtitle, muted, small, large, card, hud) and the
// dialogue box parts, so agents can switch the look of a whole UI with one field.
const char* kDarkTheme = R"JSON({
  "vars": {"accent": "#ff8a3d", "accentHi": "#ffa064", "accentLo": "#e2681f", "text": "#e9ebf0", "muted": "#9aa1ae",
           "panel": "#1b1e25ee", "field": "#111318", "line": "#ffffff1c"},
  "rules": {
    "canvas": {"font": "Inter", "fontSize": 18, "color": "$text"},
    "panel": {},
    ".card": {"background": "$panel", "radius": 12, "borderWidth": 1, "borderColor": "$line",
              "shadowColor": "#00000070", "shadowOffset": [0, 10], "shadowBlur": 28, "padding": 16},
    ".hud": {"background": "#0d0f13b0", "radius": 10, "borderWidth": 1, "borderColor": "#ffffff14", "padding": [6, 12]},
    "text": {},
    "image": {"imageFit": "contain"},
    "button": {"background": "#2c313c", "background2": "#232730", "radius": 8, "borderWidth": 1, "borderColor": "#ffffff1f",
               "textAlign": "center", "padding": [8, 18], "shadowColor": "#00000055", "shadowOffset": [0, 2], "shadowBlur": 6,
               "hover": {"background": "#363c49", "background2": "#2c313c", "borderColor": "#ffffff33"},
               "pressed": {"background": "#1f232b", "background2": "#1f232b", "shadowColor": "#00000000"},
               "focus": {"borderColor": "$accent"},
               "disabled": {"opacity": 0.45}},
    ".primary": {"background": "$accentHi", "background2": "$accent", "color": "#1d1207", "bold": true, "borderColor": "#ffc79e70",
                 "hover": {"background": "#ffb27e", "background2": "#ff944f"}, "pressed": {"background": "$accentLo", "background2": "$accentLo"}},
    ".ghost": {"background": "#00000000", "background2": "#00000000", "borderColor": "#ffffff2a", "shadowColor": "#00000000",
               "hover": {"background": "#ffffff12", "background2": "#ffffff12"}},
    ".danger": {"background": "#d0453a", "background2": "#b8352b", "color": "#ffffff", "hover": {"background": "#e05246", "background2": "#c63c31"}},
    ".title": {"fontSize": 56, "bold": true, "letterSpacing": -0.01},
    ".heading": {"fontSize": 30, "bold": true},
    ".subtitle": {"fontSize": 20, "color": "$muted"},
    ".muted": {"color": "$muted"},
    ".small": {"fontSize": 14},
    ".large": {"fontSize": 24},
    ".accent": {"color": "$accent"},
    "toggle": {"accent": "$accent", "track": "#ffffff26", "knob": "#f4f5f8", "padding": [4, 0],
               "hover": {"track": "#ffffff38"}, "disabled": {"opacity": 0.45}},
    "slider": {"accent": "$accent", "track": "#ffffff22", "knob": "#f6f7f9", "trackHeight": 6, "knobSize": 18,
               "hover": {"knob": "#ffffff"}, "disabled": {"opacity": 0.45}},
    "progress": {"accent": "$accent", "track": "#ffffff1c", "trackHeight": 10, "radius": 5},
    "input": {"background": "$field", "radius": 8, "borderWidth": 1, "borderColor": "#ffffff22", "padding": [8, 12],
              "placeholderColor": "#ffffff55", "hover": {"borderColor": "#ffffff38"}, "focus": {"borderColor": "$accent"}},
    "scroll": {"track": "#ffffff30"},
    ".dialogue_box": {"background": "#0c0e12f0", "background2": "#14171df0", "radius": 16, "borderWidth": 1, "borderColor": "#ffffff1f",
                      "shadowColor": "#000000b0", "shadowOffset": [0, 12], "shadowBlur": 40},
    ".dialogue_portrait": {"radius": 12, "borderWidth": 2, "borderColor": "#ffffff2a", "background": "#00000040", "imageFit": "cover"},
    ".dialogue_name": {"fontSize": 22, "bold": true, "color": "$accent"},
    ".dialogue_text": {"fontSize": 22, "lineSpacing": 1.3, "verticalAlign": "top"},
    ".dialogue_choice": {"textAlign": "left", "fontSize": 19, "padding": [8, 14]},
    ".dialogue_hint": {"fontSize": 14, "color": "$muted", "textAlign": "right"}
  }
})JSON";

const char* kLightTheme = R"JSON({
  "extends": "dark",
  "vars": {"accent": "#2f6fed", "accentHi": "#4a84f5", "accentLo": "#2459c7", "text": "#1d2129", "muted": "#6a7282",
           "panel": "#ffffffee", "field": "#ffffff", "line": "#00000018"},
  "rules": {
    ".card": {"shadowColor": "#1d212924"},
    ".hud": {"background": "#ffffffd0", "borderColor": "#00000014"},
    "button": {"background": "#ffffff", "background2": "#f1f3f6", "borderColor": "#00000022", "shadowColor": "#0000001a",
               "hover": {"background": "#f7f8fa", "background2": "#eceff3", "borderColor": "#00000033"},
               "pressed": {"background": "#e6e9ee", "background2": "#e6e9ee"}},
    ".primary": {"color": "#ffffff", "borderColor": "#2459c780"},
    ".ghost": {"borderColor": "#00000026", "hover": {"background": "#0000000c", "background2": "#0000000c"}},
    "toggle": {"track": "#00000026", "knob": "#ffffff"},
    "slider": {"track": "#0000001f", "knob": "#ffffff"},
    "progress": {"track": "#00000014"},
    "input": {"borderColor": "#00000026", "placeholderColor": "#00000055"},
    "scroll": {"track": "#00000030"},
    ".dialogue_box": {"background": "#fbfbfdf2", "background2": "#f1f2f6f2", "borderColor": "#00000014", "shadowColor": "#00000040"},
    ".dialogue_portrait": {"borderColor": "#00000020", "background": "#0000000c"}
  }
})JSON";

const char* kParchmentTheme = R"JSON({
  "extends": "dark",
  "vars": {"accent": "#8e2b1d", "accentHi": "#a8392a", "accentLo": "#741f14", "text": "#2a2017", "muted": "#6d5b47",
           "panel": "#efe2c4f4", "field": "#f8f0dc", "line": "#5a43293a"},
  "rules": {
    "canvas": {"font": "EB Garamond", "fontSize": 22},
    ".card": {"background": "$panel", "background2": "#e6d5b0f4", "radius": 4, "borderWidth": 2, "borderColor": "#6b4f2e66",
              "shadowColor": "#2a1a0a55", "shadowOffset": [0, 6], "shadowBlur": 18},
    ".hud": {"background": "#efe2c4d8", "borderColor": "#6b4f2e55", "radius": 4},
    "button": {"background": "#f3e7cb", "background2": "#e3d0a6", "radius": 3, "borderWidth": 2, "borderColor": "#6b4f2e88",
               "color": "$text", "shadowColor": "#2a1a0a40",
               "hover": {"background": "#f8eed6", "background2": "#ead9b4", "borderColor": "$accent"},
               "pressed": {"background": "#dcc595", "background2": "#dcc595"}},
    ".primary": {"background": "$accentHi", "background2": "$accent", "color": "#f8efdc", "borderColor": "#4a140c",
                 "hover": {"background": "#b84536", "background2": "#9a3022"}},
    ".ghost": {"borderColor": "#6b4f2e66", "hover": {"background": "#6b4f2e14", "background2": "#6b4f2e14"}},
    ".title": {"fontSize": 64, "bold": false, "letterSpacing": 0.02},
    ".heading": {"fontSize": 34, "bold": false},
    "toggle": {"track": "#6b4f2e40", "knob": "#f8efdc"},
    "slider": {"track": "#6b4f2e3a", "knob": "#f8efdc"},
    "progress": {"track": "#6b4f2e2a"},
    "input": {"background": "$field", "borderColor": "#6b4f2e66", "radius": 3, "placeholderColor": "#2a201770"},
    "scroll": {"track": "#6b4f2e55"},
    ".dialogue_box": {"background": "#f1e4c6f6", "background2": "#e3cfa5f6", "radius": 6, "borderWidth": 2, "borderColor": "#6b4f2e88",
                      "shadowColor": "#1a0f0566"},
    ".dialogue_name": {"fontSize": 26, "color": "$accent", "bold": true},
    ".dialogue_text": {"fontSize": 25},
    ".dialogue_portrait": {"radius": 3, "borderColor": "#6b4f2e88", "background": "#6b4f2e22"}
  }
})JSON";

const char* kGlassTheme = R"JSON({
  "extends": "dark",
  "vars": {"accent": "#46d4ff", "accentHi": "#7ae2ff", "accentLo": "#20b6e6", "text": "#e6f7ff", "muted": "#8fb3c4",
           "panel": "#0a1824b8", "field": "#06121bcc", "line": "#7fdcff40"},
  "rules": {
    ".card": {"background": "$panel", "background2": "#0d2233b8", "radius": 6, "borderWidth": 1, "borderColor": "$line",
              "shadowColor": "#00a0ff22", "shadowOffset": [0, 0], "shadowBlur": 30},
    ".hud": {"background": "#06121b99", "borderColor": "#7fdcff33", "radius": 4},
    "button": {"background": "#0f2a3bcc", "background2": "#0a1d2acc", "radius": 4, "borderColor": "#7fdcff55", "letterSpacing": 0.06,
               "textTransform": "uppercase", "fontSize": 16, "shadowColor": "#00000000",
               "hover": {"background": "#16405acc", "background2": "#0f2a3bcc", "borderColor": "$accent"},
               "pressed": {"background": "#0a1d2a", "background2": "#0a1d2a"}},
    ".primary": {"background": "#1fb5e8", "background2": "#1192c2", "color": "#03121b", "borderColor": "$accentHi"},
    ".title": {"letterSpacing": 0.12, "textTransform": "uppercase", "fontSize": 48},
    ".heading": {"letterSpacing": 0.08, "textTransform": "uppercase", "fontSize": 24},
    "toggle": {"track": "#7fdcff33"},
    "slider": {"track": "#7fdcff2a", "knob": "$accentHi"},
    "progress": {"track": "#7fdcff1f", "radius": 2},
    "input": {"background": "$field", "radius": 4, "borderColor": "#7fdcff44"},
    ".dialogue_box": {"background": "#06121bd8", "background2": "#0b2030d8", "radius": 6, "borderColor": "#7fdcff55",
                      "shadowColor": "#00a0ff30", "shadowOffset": [0, 0]},
    ".dialogue_name": {"textTransform": "uppercase", "letterSpacing": 0.1, "fontSize": 18},
    ".dialogue_portrait": {"radius": 4, "borderColor": "#7fdcff66"}
  }
})JSON";

const char* kPixelTheme = R"JSON({
  "extends": "dark",
  "vars": {"accent": "#f4d35e", "accentHi": "#ffe27a", "accentLo": "#d9b43c", "text": "#f6f0e1", "muted": "#a9a2c0",
           "panel": "#2b2a45", "field": "#1b1a2e", "line": "#0d0c18"},
  "rules": {
    "canvas": {"font": "JetBrains Mono", "fontSize": 18},
    ".card": {"background": "$panel", "radius": 0, "borderWidth": 3, "borderColor": "$line", "shadowColor": "#0d0c18",
              "shadowOffset": [6, 6], "shadowBlur": 0},
    ".hud": {"background": "#1b1a2ed8", "radius": 0, "borderWidth": 3, "borderColor": "$line"},
    "button": {"background": "#4a4878", "background2": "#4a4878", "radius": 0, "borderWidth": 3, "borderColor": "$line",
               "shadowColor": "#0d0c18", "shadowOffset": [4, 4], "shadowBlur": 0,
               "hover": {"background": "#5d5a96", "background2": "#5d5a96"},
               "pressed": {"background": "#38365c", "background2": "#38365c", "shadowOffset": [1, 1]}},
    ".primary": {"background": "$accent", "background2": "$accent", "color": "#1b1a2e", "hover": {"background": "$accentHi", "background2": "$accentHi"}},
    ".title": {"fontSize": 48, "bold": true},
    "toggle": {"track": "#1b1a2e", "knob": "$text"},
    "slider": {"track": "#1b1a2e", "knob": "$text", "trackHeight": 8, "knobSize": 16},
    "progress": {"track": "#1b1a2e", "radius": 0, "trackHeight": 12},
    "input": {"background": "$field", "radius": 0, "borderWidth": 3, "borderColor": "$line"},
    ".dialogue_box": {"background": "#1b1a2ef4", "background2": "#1b1a2ef4", "radius": 0, "borderWidth": 4, "borderColor": "$text",
                      "shadowColor": "#0d0c18", "shadowOffset": [6, 6], "shadowBlur": 0},
    ".dialogue_portrait": {"radius": 0, "borderWidth": 3, "borderColor": "$text", "imageFilter": "nearest"},
    "image": {"imageFilter": "nearest"},
    "panel": {"imageFilter": "nearest"},
    ".dialogue_choice": {"radius": 0}
  }
})JSON";

const std::vector<std::string>& stateNames() {
    static const std::vector<std::string> s{"hover", "focus", "checked", "pressed", "disabled"};
    return s;
}

void mergeInto(Json& dst, const Json& src) {
    if (!src.isObject()) return;
    if (!dst.isObject()) dst = Json::object();
    for (const auto& [k, v] : src.members()) {
        if (v.isObject() && dst.get(k).isObject()) {
            Json sub = dst.get(k);
            mergeInto(sub, v);
            dst[k] = std::move(sub);
        } else {
            dst[k] = v;
        }
    }
}

}  // namespace

const std::vector<std::string>& StyleSheet::propertyNames() {
    static const std::vector<std::string> names{
        "background", "background2", "backgroundImage", "slice", "imageTint", "radius", "borderWidth", "borderColor",
        "shadowColor", "shadowOffset", "shadowBlur", "opacity", "padding", "font", "fontSize", "color", "textAlign",
        "verticalAlign", "bold", "italic", "lineSpacing", "letterSpacing", "textTransform", "textOutline",
        "textOutlineColor", "textShadowColor", "textShadowOffset", "accent", "track", "knob", "trackHeight", "knobSize",
        "placeholderColor", "imageFit", "transition", "imageFilter", "sliceScale", "hover", "pressed", "focus", "checked",
        "disabled"};
    return names;
}

Status StyleSheet::validate(const Json& props, const std::string& where) {
    if (props.isNull()) return {};
    if (!props.isObject()) return Error::make("invalid_style", where + " must be an object of style properties");
    const auto& names = propertyNames();
    for (const auto& [k, v] : props.members()) {
        if (std::find(names.begin(), names.end(), k) == names.end()) {
            std::string guess = str::closest(k, names, 3);
            return Error::make("invalid_style", where + ": unknown style property \"" + k + "\"",
                               guess.empty() ? "see docs/2D_AND_UI.md for style properties" : "did you mean \"" + guess + "\"?");
        }
        if (v.isObject()) {
            if (std::find(stateNames().begin(), stateNames().end(), k) == stateNames().end()) {
                return Error::make("invalid_style", where + "." + k + " must be a value, not an object");
            }
            if (Status s = validate(v, where + "." + k); !s) return s;
        }
    }
    return {};
}

const std::vector<std::string>& StyleSheet::themeNames() {
    static const std::vector<std::string> names{"dark", "light", "parchment", "glass", "pixel"};
    return names;
}

std::shared_ptr<const StyleSheet> StyleSheet::theme(const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const StyleSheet>> cache;
    std::lock_guard lock(mutex);
    std::string key = std::find(themeNames().begin(), themeNames().end(), name) == themeNames().end() ? "dark" : name;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const char* src = key == "light" ? kLightTheme : key == "parchment" ? kParchmentTheme : key == "glass" ? kGlassTheme
                    : key == "pixel" ? kPixelTheme : kDarkTheme;
    std::shared_ptr<const StyleSheet> base;
    if (key != "dark") {
        // The base theme is built without holding the lock recursively: build "dark" directly here.
        auto it = cache.find("dark");
        if (it == cache.end()) {
            auto dark = parse(Json::parse(kDarkTheme).value(), nullptr);
            it = cache.emplace("dark", dark.value()).first;
        }
        base = it->second;
    }
    Json doc = Json::parse(src).value();
    doc.erase("extends");
    auto sheet = parse(doc, base);
    auto ptr = std::const_pointer_cast<StyleSheet>(sheet.value());
    ptr->name = key;
    cache[key] = ptr;
    return ptr;
}

Result<std::shared_ptr<const StyleSheet>> StyleSheet::parse(const Json& doc, std::shared_ptr<const StyleSheet> base) {
    if (!doc.isObject()) return Error::make("invalid_style", "a style sheet must be a JSON object");
    if (const Json* ext = doc.find("extends"); ext && ext->isString() && !ext->asString().empty()) {
        if (std::find(themeNames().begin(), themeNames().end(), ext->asString()) == themeNames().end()) {
            std::string guess = str::closest(ext->asString(), themeNames(), 3);
            return Error::make("invalid_style", "unknown theme \"" + ext->asString() + "\"",
                               guess.empty() ? "themes: dark, light, parchment, glass, pixel" : "did you mean \"" + guess + "\"?");
        }
        base = theme(ext->asString());
    }
    auto sheet = std::make_shared<StyleSheet>();
    if (base) {
        sheet->vars_ = base->vars_;
        sheet->rules_ = base->rules_;
        sheet->name = base->name;
    }
    mergeInto(sheet->vars_, doc.get("vars"));
    Json rules = doc.get("rules").isObject() ? doc.get("rules") : Json::object();
    for (const auto& [k, v] : doc.members()) {  // selectors may also sit at the top level
        if (k != "rules" && k != "vars" && k != "extends" && k != "format" && k != "version" && k != "name") rules[k] = v;
    }
    for (const auto& [selector, props] : rules.members()) {
        if (Status s = validate(props, "style \"" + selector + "\""); !s) return s.error();
        Json merged = sheet->rules_.get(selector);
        mergeInto(merged, props);
        sheet->rules_[selector] = merged;
    }
    return std::shared_ptr<const StyleSheet>(sheet);
}

Json StyleSheet::cascade(const std::string& widget, const std::vector<std::string>& classes, const std::string& entityName) const {
    Json out = Json::object();
    mergeInto(out, rules_.get("*"));
    mergeInto(out, rules_.get(widget));
    for (const auto& c : classes) mergeInto(out, rules_.get("." + c));
    if (!entityName.empty()) mergeInto(out, rules_.get("#" + entityName));
    return out;
}

void StyleSheet::apply(const Json& props, Style& s) const {
    auto value = [&](const Json& v) -> const Json& {
        if (v.isString() && !v.asString().empty() && v.asString()[0] == '$') {
            const Json& r = vars_.get(v.asString().substr(1));
            if (!r.isNull()) return r;
        }
        return v;
    };
    auto color = [&](const char* key, Vec4& out) {
        if (const Json* v = props.find(key)) {
            const Json& r = value(*v);
            Vec4 c;
            if ((r.isString() && text::parseColor(r.asString(), c)) || (!r.isString() && reflect::jsonToColor(r, c))) out = c;
        }
    };
    auto number = [&](const char* key, float& out) {
        if (const Json* v = props.find(key); v && value(*v).isNumber()) out = value(*v).asFloat();
    };
    auto boolean = [&](const char* key, bool& out) {
        if (const Json* v = props.find(key); v && value(*v).isBool()) out = value(*v).asBool();
    };
    auto string = [&](const char* key, std::string& out) {
        if (const Json* v = props.find(key); v && value(*v).isString()) out = value(*v).asString();
    };
    auto vec2 = [&](const char* key, Vec2& out) {
        if (const Json* v = props.find(key)) (void)reflect::jsonToVec2(value(*v), out);
    };
    auto vec4 = [&](const char* key, Vec4& out) {
        if (const Json* v = props.find(key)) (void)reflect::jsonToVec4(value(*v), out);
    };
    color("background", s.background);
    if (props.contains("background") && !props.contains("background2")) s.background2 = {0, 0, 0, 0};
    color("background2", s.background2);
    string("backgroundImage", s.backgroundImage);
    vec4("slice", s.slice);
    color("imageTint", s.imageTint);
    number("radius", s.radius);
    number("borderWidth", s.borderWidth);
    color("borderColor", s.borderColor);
    color("shadowColor", s.shadowColor);
    vec2("shadowOffset", s.shadowOffset);
    number("shadowBlur", s.shadowBlur);
    number("opacity", s.opacity);
    vec4("padding", s.padding);
    string("font", s.font);
    number("fontSize", s.fontSize);
    color("color", s.color);
    string("textAlign", s.textAlign);
    string("verticalAlign", s.verticalAlign);
    boolean("bold", s.bold);
    boolean("italic", s.italic);
    number("lineSpacing", s.lineSpacing);
    number("letterSpacing", s.letterSpacing);
    string("textTransform", s.textTransform);
    number("textOutline", s.textOutline);
    color("textOutlineColor", s.textOutlineColor);
    color("textShadowColor", s.textShadowColor);
    vec2("textShadowOffset", s.textShadowOffset);
    color("accent", s.accent);
    color("track", s.track);
    color("knob", s.knob);
    number("trackHeight", s.trackHeight);
    number("knobSize", s.knobSize);
    color("placeholderColor", s.placeholderColor);
    string("imageFit", s.imageFit);
    number("transition", s.transition);
    string("imageFilter", s.imageFilter);
    number("sliceScale", s.sliceScale);
}

Style StyleSheet::root() const {
    Style s;
    apply(rules_.get("canvas"), s);
    return s;
}

Style StyleSheet::resolve(const std::string& widget, const std::vector<std::string>& classes, const std::string& entityName,
                          const Json& overrides, const StyleState& st, const Style* parent) const {
    Style s;
    const Style inherited = parent ? *parent : root();
    s.font = inherited.font;
    s.fontSize = inherited.fontSize;
    s.color = inherited.color;
    s.textAlign = inherited.textAlign;
    s.bold = inherited.bold;
    s.italic = inherited.italic;
    s.lineSpacing = inherited.lineSpacing;
    s.letterSpacing = inherited.letterSpacing;
    s.textTransform = inherited.textTransform;
    s.textOutline = inherited.textOutline;
    s.textOutlineColor = inherited.textOutlineColor;
    s.textShadowColor = inherited.textShadowColor;
    s.textShadowOffset = inherited.textShadowOffset;
    Json props = cascade(widget, classes, entityName);
    mergeInto(props, overrides);
    apply(props, s);
    const bool active[] = {st.hover && !st.disabled, st.focus, st.checked, st.pressed && !st.disabled, st.disabled};
    for (size_t i = 0; i < stateNames().size(); ++i) {
        if (active[i]) apply(props.get(stateNames()[i]), s);
    }
    return s;
}

}  // namespace sky::ui
