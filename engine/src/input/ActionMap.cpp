#include "skywalker/input/ActionMap.h"

#include <algorithm>
#include <cmath>

#include "skywalker/core/Strings.h"

namespace sky::input {

// --- names ---------------------------------------------------------------------------------------

namespace {

struct PadName {
    const char* name;
    PadButton button;
};

const std::vector<PadName>& padNames() {
    static const std::vector<PadName> names{
        {"south", PadButton::South},
        {"east", PadButton::East},
        {"west", PadButton::West},
        {"north", PadButton::North},
        {"leftShoulder", PadButton::LeftShoulder},
        {"rightShoulder", PadButton::RightShoulder},
        {"leftTrigger", PadButton::LeftTrigger},
        {"rightTrigger", PadButton::RightTrigger},
        {"select", PadButton::Select},
        {"start", PadButton::Start},
        {"leftStickButton", PadButton::LeftStick},
        {"rightStickButton", PadButton::RightStick},
        {"dpadUp", PadButton::DpadUp},
        {"dpadDown", PadButton::DpadDown},
        {"dpadLeft", PadButton::DpadLeft},
        {"dpadRight", PadButton::DpadRight},
        {"guide", PadButton::Guide},
    };
    return names;
}

const std::vector<std::pair<const char*, PadButton>>& padAliases() {
    static const std::vector<std::pair<const char*, PadButton>> a{
        {"a", PadButton::South},          {"cross", PadButton::South},      {"b", PadButton::East},
        {"circle", PadButton::East},      {"x", PadButton::West},           {"square", PadButton::West},
        {"y", PadButton::North},          {"triangle", PadButton::North},   {"lb", PadButton::LeftShoulder},
        {"l1", PadButton::LeftShoulder},  {"rb", PadButton::RightShoulder}, {"r1", PadButton::RightShoulder},
        {"lt", PadButton::LeftTrigger},   {"l2", PadButton::LeftTrigger},   {"rt", PadButton::RightTrigger},
        {"r2", PadButton::RightTrigger},  {"back", PadButton::Select},      {"view", PadButton::Select},
        {"share", PadButton::Select},     {"menu", PadButton::Start},       {"options", PadButton::Start},
        {"l3", PadButton::LeftStick},     {"r3", PadButton::RightStick},    {"up", PadButton::DpadUp},
        {"down", PadButton::DpadDown},    {"left", PadButton::DpadLeft},    {"right", PadButton::DpadRight},
        {"home", PadButton::Guide},
    };
    return a;
}

}  // namespace

const char* toString(PadButton b) {
    for (const auto& p : padNames()) {
        if (p.button == b) return p.name;
    }
    return "?";
}

bool parsePadButton(std::string_view name, PadButton& out) {
    std::string l = str::lower(name);
    for (const auto& p : padNames()) {
        if (l == str::lower(p.name)) {
            out = p.button;
            return true;
        }
    }
    for (const auto& [alias, button] : padAliases()) {
        if (l == alias) {
            out = button;
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& padButtonNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& p : padNames()) n.emplace_back(p.name);
        return n;
    }();
    return names;
}

const std::vector<std::string>& namedKeys() {
    static const std::vector<std::string> keys{
        "space", "enter", "escape", "tab", "backspace", "delete", "left", "right", "up", "down", "shift", "ctrl", "alt",
        "cmd", "home", "end", "pageup", "pagedown", "capslock", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9",
        "f10", "f11", "f12"};
    return keys;
}

std::string canonicalKey(std::string_view name) {
    std::string k = str::lower(str::trim(name));
    static const std::vector<std::pair<const char*, const char*>> aliases{
        {"esc", "escape"},      {"return", "enter"},   {"spacebar", "space"}, {"control", "ctrl"},
        {"option", "alt"},      {"command", "cmd"},    {"meta", "cmd"},       {"del", "delete"},
        {"arrowleft", "left"},  {"arrowright", "right"}, {"arrowup", "up"},   {"arrowdown", "down"},
        {"pgup", "pageup"},     {"pgdn", "pagedown"},  {"lshift", "shift"},   {"rshift", "shift"},
            };
    for (const auto& [from, to] : aliases) {
        if (k == from) return to;
    }
    if (k.empty() && name.find(' ') != std::string_view::npos) return "space";
    return k;
}

bool isKnownKey(std::string_view canonical) {
    if (canonical.empty()) return false;
    // A single printable character (ASCII, or one UTF-8 sequence) names itself.
    size_t cp = 1;
    unsigned char c0 = static_cast<unsigned char>(canonical[0]);
    if (c0 >= 0xF0) cp = 4;
    else if (c0 >= 0xE0) cp = 3;
    else if (c0 >= 0xC0) cp = 2;
    if (canonical.size() == cp) return true;
    const auto& named = namedKeys();
    return std::find(named.begin(), named.end(), std::string(canonical)) != named.end();
}

const char* toString(ActionType t) {
    switch (t) {
        case ActionType::Button: return "button";
        case ActionType::Axis: return "axis";
        case ActionType::Axis2D: return "axis2d";
    }
    return "button";
}

void InputState::endTick() {
    pressed.clear();
    released.clear();
    clicked.clear();
    mousePressed.clear();
    mouseReleased.clear();
    mouseDX = mouseDY = scrollX = scrollY = 0.f;
    for (auto it = virtualActions.begin(); it != virtualActions.end();) {
        if (--it->second.ticks <= 0) it = virtualActions.erase(it);
        else ++it;
    }
}

void InputState::clear() {
    *this = InputState{};
}

// --- sources -------------------------------------------------------------------------------------

Result<Source> parseSource(std::string_view textIn) {
    std::string text = str::trim(textIn);
    size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return Error::make("invalid_binding", "binding '" + text + "' needs a device prefix",
                           "use key:<name>, mouse:<button|delta|scroll|position>, or pad:<button|stick|trigger>");
    }
    std::string device = str::lower(text.substr(0, colon));
    std::string rest = text.substr(colon + 1);
    Source s;
    // Optional .x / .y component suffix.
    std::string base = rest;
    size_t dot = rest.rfind('.');
    if (dot != std::string::npos && dot + 1 < rest.size()) {
        std::string c = str::lower(rest.substr(dot + 1));
        if (c == "x" || c == "y") {
            s.component = c == "x" ? 0 : 1;
            base = rest.substr(0, dot);
        }
    }
    std::string lbase = str::lower(base);

    if (device == "key") {
        std::string k = canonicalKey(rest);
        if (!isKnownKey(k)) {
            std::string guess = str::closest(k, namedKeys(), 3);
            return Error::make("invalid_binding", "unknown key '" + rest + "'",
                               guess.empty() ? "use a character (w, 1, ;) or a name like space, enter, escape, left, shift"
                                             : "did you mean 'key:" + guess + "'?");
        }
        s.type = Source::Type::Key;
        s.name = k;
        s.component = -1;
        s.text = "key:" + k;
        return s;
    }
    if (device == "mouse") {
        s.name = lbase;
        if (lbase == "left" || lbase == "right" || lbase == "middle") {
            s.type = Source::Type::MouseButton;
            s.component = -1;
            s.text = "mouse:" + lbase;
        } else if (lbase == "delta" || lbase == "move") {
            s.type = Source::Type::MouseDelta;
            s.text = "mouse:delta" + std::string(s.component < 0 ? "" : s.component == 0 ? ".x" : ".y");
        } else if (lbase == "scroll" || lbase == "wheel") {
            s.type = Source::Type::MouseScroll;
            s.text = "mouse:scroll" + std::string(s.component < 0 ? "" : s.component == 0 ? ".x" : ".y");
        } else if (lbase == "position" || lbase == "pos" || lbase == "cursor") {
            s.type = Source::Type::MousePosition;
            s.text = "mouse:position" + std::string(s.component < 0 ? "" : s.component == 0 ? ".x" : ".y");
        } else {
            std::vector<std::string> valid{"left", "right", "middle", "delta", "scroll", "position"};
            std::string guess = str::closest(lbase, valid, 3);
            return Error::make("invalid_binding", "unknown mouse source '" + rest + "'",
                               guess.empty() ? "use left, right, middle, delta, scroll or position" : "did you mean 'mouse:" + guess + "'?");
        }
        return s;
    }
    if (device == "pad" || device == "gamepad") {
        if (lbase == "leftstick" || lbase == "rightstick" || lbase == "ls" || lbase == "rs") {
            // A stick without a component is the 2D stick; the click is the L3/R3 button via l3/r3.
            s.type = Source::Type::PadStick;
            s.stick = (lbase == "leftstick" || lbase == "ls") ? 0 : 1;
            s.text = std::string("pad:") + (s.stick == 0 ? "leftStick" : "rightStick") +
                     (s.component < 0 ? "" : s.component == 0 ? ".x" : ".y");
            return s;
        }
        PadButton b;
        if (parsePadButton(base, b)) {
            s.component = -1;
            if ((b == PadButton::LeftTrigger || b == PadButton::RightTrigger)) {
                s.type = Source::Type::PadTrigger;
                s.stick = b == PadButton::LeftTrigger ? 0 : 1;
                s.button = b;
                s.text = std::string("pad:") + toString(b);
                return s;
            }
            s.type = Source::Type::PadButton;
            s.button = b;
            s.text = std::string("pad:") + toString(b);
            return s;
        }
        std::string guess = str::closest(base, padButtonNames(), 3);
        return Error::make("invalid_binding", "unknown gamepad control '" + rest + "'",
                           guess.empty() ? "use south, east, west, north, leftShoulder, rightShoulder, leftTrigger, rightTrigger, "
                                           "select, start, dpadUp.., leftStick[.x|.y], rightStick[.x|.y]"
                                         : "did you mean 'pad:" + guess + "'?");
    }
    return Error::make("invalid_binding", "unknown device '" + device + "' in binding '" + text + "'",
                       "use key:, mouse: or pad:");
}

// --- evaluation --------------------------------------------------------------------------------------

Vec2 applyRadialDeadzone(Vec2 v, float deadzone) {
    float mag = std::sqrt(v.x * v.x + v.y * v.y);
    if (mag <= deadzone || mag < 1e-6f) return {0.f, 0.f};
    float scaled = std::min(1.f, (mag - deadzone) / std::max(1e-3f, 1.f - deadzone));
    return {v.x / mag * scaled, v.y / mag * scaled};
}

namespace {

float length2(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

/// Raw value of one source: buttons 0/1 in x, vectors in (x, y).
Vec2 sourceValue(const Source& s, const InputState& in, float deadzone, bool axis2d) {
    auto pick = [&](Vec2 v) -> Vec2 {
        if (s.component == 0) return {v.x, 0.f};
        if (s.component == 1) return axis2d ? Vec2{0.f, v.y} : Vec2{v.y, 0.f};
        return v;
    };
    switch (s.type) {
        case Source::Type::Key:
            return {in.held.count(s.name) || in.pressed.count(s.name) ? 1.f : 0.f, 0.f};
        case Source::Type::MouseButton:
            return {in.mouseHeld.count(s.name) || in.mousePressed.count(s.name) ? 1.f : 0.f, 0.f};
        case Source::Type::MouseDelta: return pick({in.mouseDX, in.mouseDY});
        case Source::Type::MouseScroll: return pick({in.scrollX, in.scrollY});
        case Source::Type::MousePosition: return pick({in.mouseX, in.mouseY});
        case Source::Type::PadButton: {
            for (const auto& p : in.pads) {
                if (p.connected && p.button(s.button)) return {1.f, 0.f};
            }
            return {0.f, 0.f};
        }
        case Source::Type::PadStick: {
            Vec2 best{0.f, 0.f};
            for (const auto& p : in.pads) {
                if (!p.connected) continue;
                Vec2 v = applyRadialDeadzone(s.stick == 0 ? Vec2{p.lx, p.ly} : Vec2{p.rx, p.ry}, deadzone);
                if (length2(v) > length2(best)) best = v;
            }
            return pick(best);
        }
        case Source::Type::PadTrigger: {
            float best = 0.f;
            for (const auto& p : in.pads) {
                if (!p.connected) continue;
                float v = s.stick == 0 ? p.lt : p.rt;
                if (p.button(s.button)) v = std::max(v, 1.f);  // digital-only triggers
                best = std::max(best, v);
            }
            if (best <= deadzone) return {0.f, 0.f};
            return {std::min(1.f, (best - deadzone) / std::max(1e-3f, 1.f - deadzone)), 0.f};
        }
        case Source::Type::None: break;
    }
    return {0.f, 0.f};
}

Vec2 bindingValue(const Binding& b, const InputState& in, float actionDeadzone, bool axis2d) {
    const float dz = b.deadzone >= 0.f ? b.deadzone : actionDeadzone;
    Vec2 v{0.f, 0.f};
    switch (b.kind) {
        case Binding::Kind::Single: v = sourceValue(b.source, in, dz, axis2d); break;
        case Binding::Kind::Composite1D:
            v.x = sourceValue(b.positive, in, dz, false).x - sourceValue(b.negative, in, dz, false).x;
            v.x = std::clamp(v.x, -1.f, 1.f);
            break;
        case Binding::Kind::Composite2D: {
            v.x = sourceValue(b.right, in, dz, false).x - sourceValue(b.left, in, dz, false).x;
            v.y = sourceValue(b.up, in, dz, false).x - sourceValue(b.down, in, dz, false).x;
            float len = length2(v);
            if (len > 1.f) v = {v.x / len, v.y / len};  // diagonals are not faster
            break;
        }
    }
    v.x *= b.scale * (b.invertX ? -1.f : 1.f);
    v.y *= b.scale * (b.invertY ? -1.f : 1.f);
    return v;
}

}  // namespace

void ActionMap::evaluate(InputState& in) const {
    std::map<std::string, ActionState> next;
    for (const Action& a : actions_) {
        const bool axis2d = a.type == ActionType::Axis2D;
        Vec2 value{0.f, 0.f};
        float bestMag = -1.f;
        for (const Binding& b : a.bindings) {
            Vec2 v = bindingValue(b, in, a.deadzone, axis2d);
            float mag = a.type == ActionType::Axis ? std::fabs(v.x) : length2(v);
            if (mag > bestMag) {
                bestMag = mag;
                value = v;
            }
        }
        if (auto it = in.virtualActions.find(a.name); it != in.virtualActions.end()) value = it->second.value;

        ActionState st;
        st.vec2 = axis2d;
        switch (a.type) {
            case ActionType::Button: {
                float mag = std::clamp(length2(value), 0.f, 1.f);
                st.x = mag;
                st.held = mag >= a.threshold;
                break;
            }
            case ActionType::Axis:
                st.x = value.x;
                st.held = std::fabs(value.x) > 1e-4f;
                break;
            case ActionType::Axis2D:
                st.x = value.x;
                st.y = value.y;
                st.held = length2(value) > 1e-4f;
                break;
        }
        bool wasHeld = false;
        if (auto prev = in.actions.find(a.name); prev != in.actions.end()) wasHeld = prev->second.held;
        st.pressed = st.held && !wasHeld;
        st.released = !st.held && wasHeld;
        next[a.name] = st;
    }
    in.actions = std::move(next);
}

// --- bindings & JSON ------------------------------------------------------------------------------------

namespace {

struct Named2D {
    const char* name;
    const char *up, *down, *left, *right;
};
struct Named1D {
    const char* name;
    const char *negative, *positive;
};

const std::vector<Named2D>& named2D() {
    static const std::vector<Named2D> n{
        {"wasd", "key:w", "key:s", "key:a", "key:d"},
        {"arrows", "key:up", "key:down", "key:left", "key:right"},
        {"ijkl", "key:i", "key:k", "key:j", "key:l"},
        {"dpad", "pad:dpadUp", "pad:dpadDown", "pad:dpadLeft", "pad:dpadRight"},
    };
    return n;
}
const std::vector<Named1D>& named1D() {
    static const std::vector<Named1D> n{
        {"ad", "key:a", "key:d"},        {"leftright", "key:left", "key:right"}, {"updown", "key:down", "key:up"},
        {"ws", "key:s", "key:w"},        {"qe", "key:q", "key:e"},               {"dpad_x", "pad:dpadLeft", "pad:dpadRight"},
        {"dpad_y", "pad:dpadDown", "pad:dpadUp"},
    };
    return n;
}

std::vector<std::string> compositeNames() {
    std::vector<std::string> n;
    for (const auto& c : named2D()) n.emplace_back(c.name);
    for (const auto& c : named1D()) n.emplace_back(c.name);
    return n;
}

Result<Binding> bindingFromJson(const Json& j, const std::string& where) {
    Binding b;
    auto parseInto = [&](const Json& v, Source& out, const std::string& field) -> Status {
        if (!v.isString()) return Error::make("invalid_binding", where + "." + field + " must be a source string like \"key:w\"");
        auto s = parseSource(v.asString());
        if (!s) return Error{s.error().code, where + "." + field + ": " + s.error().message, s.error().hint};
        out = *s;
        return {};
    };
    if (j.isString()) {
        std::string text = j.asString();
        std::string l = str::lower(str::trim(text));
        for (const auto& c : named2D()) {
            if (l == c.name) {
                b.kind = Binding::Kind::Composite2D;
                b.compositeName = c.name;
                (void)parseInto(c.up, b.up, "up");
                (void)parseInto(c.down, b.down, "down");
                (void)parseInto(c.left, b.left, "left");
                (void)parseInto(c.right, b.right, "right");
                return b;
            }
        }
        for (const auto& c : named1D()) {
            if (l == c.name) {
                b.kind = Binding::Kind::Composite1D;
                b.compositeName = c.name;
                (void)parseInto(c.negative, b.negative, "negative");
                (void)parseInto(c.positive, b.positive, "positive");
                return b;
            }
        }
        if (text.find(':') == std::string::npos) {
            std::string guess = str::closest(l, compositeNames(), 3);
            return Error::make("invalid_binding", where + ": unknown binding '" + text + "'",
                               guess.empty() ? "use a source like \"key:space\" or a composite: wasd, arrows, ijkl, dpad, ad, leftright, qe"
                                             : "did you mean '" + guess + "'?");
        }
        auto s = parseSource(text);
        if (!s) return Error{s.error().code, where + ": " + s.error().message, s.error().hint};
        b.source = *s;
        return b;
    }
    if (!j.isObject()) return Error::make("invalid_binding", where + " must be a string or an object");
    static const std::vector<std::string> keys{"source", "composite", "up", "down", "left", "right", "negative", "positive",
                                               "scale", "invertX", "invertY", "deadzone"};
    for (const auto& [k, v] : j.members()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            std::string guess = str::closest(k, keys, 3);
            return Error::make("invalid_binding", where + ": unknown binding field '" + k + "'",
                               guess.empty() ? "valid: source, composite, up/down/left/right, negative/positive, scale, invertX, invertY, deadzone"
                                             : "did you mean '" + guess + "'?");
        }
    }
    if (const Json* c = j.find("composite")) {
        auto named = bindingFromJson(*c, where + ".composite");
        if (!named) return named.error();
        if (named->kind == Binding::Kind::Single) return Error::make("invalid_binding", where + ".composite must name a composite (wasd, arrows, ad, ...)");
        b = *named;
    } else if (j.contains("up") || j.contains("down") || j.contains("left") || j.contains("right")) {
        b.kind = Binding::Kind::Composite2D;
        for (auto [field, dst] : {std::pair<const char*, Source*>{"up", &b.up}, {"down", &b.down}, {"left", &b.left}, {"right", &b.right}}) {
            if (j.contains(field)) {
                if (Status s = parseInto(j.get(field), *dst, field); !s) return s.error();
            }
        }
    } else if (j.contains("negative") || j.contains("positive")) {
        b.kind = Binding::Kind::Composite1D;
        for (auto [field, dst] : {std::pair<const char*, Source*>{"negative", &b.negative}, {"positive", &b.positive}}) {
            if (j.contains(field)) {
                if (Status s = parseInto(j.get(field), *dst, field); !s) return s.error();
            }
        }
    } else if (j.contains("source")) {
        if (Status s = parseInto(j.get("source"), b.source, "source"); !s) return s.error();
    } else {
        return Error::make("invalid_binding", where + " needs `source`, `composite`, up/down/left/right or negative/positive");
    }
    b.scale = j.get("scale").asFloat(1.f);
    b.invertX = j.get("invertX").asBool(false);
    b.invertY = j.get("invertY").asBool(false);
    if (j.contains("deadzone")) {
        b.deadzone = j.get("deadzone").asFloat(-1.f);
        if (b.deadzone < 0.f || b.deadzone >= 1.f) return Error::make("invalid_binding", where + ".deadzone must be in 0..1");
    }
    return b;
}

}  // namespace

Json Binding::toJson() const {
    const bool plain = scale == 1.f && !invertX && !invertY && deadzone < 0.f;
    if (plain) {
        if (kind == Kind::Single) return source.text;
        if (!compositeName.empty()) return compositeName;
    }
    Json j = Json::object();
    switch (kind) {
        case Kind::Single: j["source"] = source.text; break;
        case Kind::Composite1D:
            if (!compositeName.empty()) {
                j["composite"] = compositeName;
            } else {
                j["negative"] = negative.text;
                j["positive"] = positive.text;
            }
            break;
        case Kind::Composite2D:
            if (!compositeName.empty()) {
                j["composite"] = compositeName;
            } else {
                j["up"] = up.text;
                j["down"] = down.text;
                j["left"] = left.text;
                j["right"] = right.text;
            }
            break;
    }
    if (scale != 1.f) j["scale"] = scale;
    if (invertX) j["invertX"] = true;
    if (invertY) j["invertY"] = true;
    if (deadzone >= 0.f) j["deadzone"] = deadzone;
    return j;
}

Json Action::toJson() const {
    Json arr = Json::array();
    for (const auto& b : bindings) arr.push(b.toJson());
    Json j = Json::object({{"type", toString(type)}, {"bindings", arr}});
    if (deadzone != 0.15f) j["deadzone"] = deadzone;
    if (threshold != 0.5f) j["threshold"] = threshold;
    if (!description.empty()) j["description"] = description;
    return j;
}

Result<Action> ActionMap::actionFromJson(const std::string& name, const Json& j) {
    if (name.empty()) return Error::make("invalid_action", "an action needs a name");
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
            return Error::make("invalid_action", "action name '" + name + "' may only contain letters, digits, _ - .");
        }
    }
    if (!j.isObject()) return Error::make("invalid_action", "action '" + name + "' must be an object {type, bindings}");
    static const std::vector<std::string> keys{"type", "bindings", "deadzone", "threshold", "description"};
    for (const auto& [k, v] : j.members()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            std::string guess = str::closest(k, keys, 3);
            return Error::make("invalid_action", "action '" + name + "': unknown field '" + k + "'",
                               guess.empty() ? "valid: type, bindings, deadzone, threshold, description" : "did you mean '" + guess + "'?");
        }
    }
    Action a;
    a.name = name;
    std::string type = str::lower(j.get("type").asString("button"));
    if (type == "button") a.type = ActionType::Button;
    else if (type == "axis" || type == "axis1d") a.type = ActionType::Axis;
    else if (type == "axis2d" || type == "vector2" || type == "vec2") a.type = ActionType::Axis2D;
    else return Error::make("invalid_action", "action '" + name + "': type '" + type + "' must be button, axis or axis2d");
    if (const Json* dz = j.find("deadzone")) {
        a.deadzone = dz->asFloat(0.15f);
        if (a.deadzone < 0.f || a.deadzone >= 1.f) return Error::make("invalid_action", "action '" + name + "': deadzone must be in 0..1");
    }
    if (const Json* th = j.find("threshold")) {
        a.threshold = th->asFloat(0.5f);
        if (a.threshold <= 0.f || a.threshold > 1.f) return Error::make("invalid_action", "action '" + name + "': threshold must be in (0..1]");
    }
    a.description = j.get("description").asString();
    const Json& bindings = j.get("bindings");
    if (!bindings.isArray()) return Error::make("invalid_action", "action '" + name + "': bindings must be an array");
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto b = bindingFromJson(bindings[i], "action '" + name + "' binding " + std::to_string(i));
        if (!b) return b.error();
        a.bindings.push_back(*b);
    }
    return a;
}

Result<ActionMap> ActionMap::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_input_map", "the input map must be an object {actions: {...}}");
    for (const auto& [k, v] : j.members()) {
        if (k != "actions" && k != "version") {
            return Error::make("invalid_input_map", "unknown top-level field '" + k + "'", "the file has `version` and `actions`");
        }
    }
    ActionMap map;
    const Json& actions = j.get("actions");
    if (!actions.isObject()) return Error::make("invalid_input_map", "`actions` must be an object of name -> action");
    for (const auto& [name, value] : actions.members()) {
        auto a = actionFromJson(name, value);
        if (!a) return a.error();
        map.set(std::move(*a));
    }
    return map;
}

Json ActionMap::toJson() const {
    Json actions = Json::object();
    for (const auto& a : actions_) actions[a.name] = a.toJson();
    return Json::object({{"version", 1}, {"actions", actions}});
}

const Action* ActionMap::find(std::string_view name) const {
    for (const auto& a : actions_) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

std::vector<std::string> ActionMap::names() const {
    std::vector<std::string> n;
    for (const auto& a : actions_) n.push_back(a.name);
    return n;
}

void ActionMap::set(Action action) {
    for (auto& a : actions_) {
        if (a.name == action.name) {
            a = std::move(action);
            return;
        }
    }
    actions_.push_back(std::move(action));
}

bool ActionMap::remove(std::string_view name) {
    auto it = std::remove_if(actions_.begin(), actions_.end(), [&](const Action& a) { return a.name == name; });
    bool removed = it != actions_.end();
    actions_.erase(it, actions_.end());
    return removed;
}

ActionMap ActionMap::defaults() {
    static const char* kDefaults = R"JSON({
  "version": 1,
  "actions": {
    "move": {"type": "axis2d", "description": "Walk / steer: x right, y forward", "bindings": ["wasd", "arrows", "pad:leftStick"]},
    "look": {"type": "axis2d", "description": "Camera turn per tick: x right, y up (mouse pixels * 0.1, right stick up to 3/tick)",
             "bindings": [{"source": "mouse:delta", "scale": 0.1}, {"source": "pad:rightStick", "scale": 3}]},
    "jump": {"type": "button", "bindings": ["key:space", "pad:south"]},
    "fire": {"type": "button", "description": "Primary action", "bindings": ["mouse:left", "pad:rightTrigger"]},
    "aim": {"type": "button", "description": "Secondary action / aim down sights", "bindings": ["mouse:right", "pad:leftTrigger"]},
    "interact": {"type": "button", "bindings": ["key:e", "pad:west"]},
    "sprint": {"type": "button", "bindings": ["key:shift", "pad:l3"]},
    "pause": {"type": "button", "bindings": ["key:escape", "pad:start"]},
    "cursor": {"type": "axis2d", "description": "Mouse position in the viewport, 0..1 from the top-left", "bindings": ["mouse:position"]}
  }
})JSON";
    auto json = Json::parse(kDefaults);
    auto map = fromJson(*json);
    return *map;
}

Json ActionMap::bindingCatalog() {
    Json keys = Json::array();
    for (const auto& k : namedKeys()) keys.push(k);
    Json pads = Json::array();
    for (const auto& n : padButtonNames()) pads.push(n);
    return Json::object(
        {{"key", "key:<character or name>: a single character (w, 1, ;) or one of the named keys"},
         {"namedKeys", keys},
         {"mouse", Json::array({"mouse:left", "mouse:right", "mouse:middle", "mouse:delta[.x|.y] (pixels since last tick, +y up)",
                                "mouse:scroll[.x|.y]", "mouse:position[.x|.y] (0..1, top-left origin)"})},
         {"gamepad", Json::object({{"buttons", pads},
                                   {"sticks", Json::array({"pad:leftStick[.x|.y]", "pad:rightStick[.x|.y]"})},
                                   {"triggers", Json::array({"pad:leftTrigger", "pad:rightTrigger"})}})},
         {"composites2d", Json::array({"wasd", "arrows", "ijkl", "dpad"})},
         {"composites1d", Json::array({"ad", "leftright", "updown", "ws", "qe", "dpad_x", "dpad_y"})},
         {"bindingOptions", "{source|composite|up/down/left/right|negative/positive, scale, invertX, invertY, deadzone}"}});
}

}  // namespace sky::input
