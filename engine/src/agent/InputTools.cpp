// Input tools: the project's input action map, and the action/gamepad/mouse extensions of
// sim_input that let agents and playtest bots drive a game exactly like a player.

#include <algorithm>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/input/ActionMap.h"
#include "skywalker/physics/Vehicle.h"

namespace sky::tools {

namespace {

using namespace schema;

Json stateJson(const wander::InputState& in) {
    Json out = Json::object();
    for (const auto& [name, st] : in.actions) {
        Json j = Json::object({{"held", st.held}, {"pressed", st.pressed}, {"released", st.released}});
        auto r = [](float v) { return std::round(v * 1000.f) / 1000.f; };
        if (st.vec2) j["value"] = Json::array({r(st.x), r(st.y)});
        else j["value"] = r(st.x);
        out[name] = j;
    }
    return out;
}

Error unknownAction(const input::ActionMap& map, const std::string& name) {
    std::string guess = str::closest(name, map.names(), 3);
    std::string list;
    for (const auto& n : map.names()) list += (list.empty() ? "" : ", ") + n;
    return Error::make("not_found", "no input action '" + name + "'",
                       guess.empty() ? "actions: " + list : "did you mean '" + guess + "'? (actions: " + list + ")");
}

bool readVec2(const Json& j, Vec2& out) {
    if (j.isArray() && j.size() == 2 && j[0].isNumber() && j[1].isNumber()) {
        out = {j[0].asFloat(), j[1].asFloat()};
        return true;
    }
    if (j.isObject() && j.get("x").isNumber() && j.get("y").isNumber()) {
        out = {j.get("x").asFloat(), j.get("y").asFloat()};
        return true;
    }
    return false;
}

}  // namespace

Status applySimInput(Engine& engine, const Json& a) {
    wander::InputState& in = engine.input();
    const input::ActionMap& map = engine.actionMap();

    auto holdAction = [&](const Json& item, bool axis) -> Status {
        std::string name;
        Vec2 value{1.f, 0.f};
        int ticks = 1;
        if (item.isString()) {
            name = item.asString();
        } else if (item.isObject()) {
            name = item.get("name").asString(item.get("action").asString());
            ticks = static_cast<int>(item.get("ticks").asInt(1));
            if (item.contains("x") || item.contains("y")) value = {item.get("x").asFloat(0.f), item.get("y").asFloat(0.f)};
            else if (item.contains("value")) value = {item.get("value").asFloat(1.f), 0.f};
        } else {
            return Error::make("invalid_arguments", "each entry must be an action name or {name, ticks, x, y}");
        }
        const input::Action* action = map.find(name);
        if (!action) return unknownAction(map, name);
        if (axis && action->type == input::ActionType::Button) {
            return Error::make("invalid_arguments", "'" + name + "' is a button; use `actions` to press or hold it");
        }
        if (ticks < 1 || ticks > 360000) return Error::make("invalid_arguments", "ticks must be between 1 and 360000 (60 ticks = 1 second)");
        in.virtualActions[name] = {value, ticks};
        return {};
    };
    for (const auto& item : a.get("actions").elements()) {
        if (Status s = holdAction(item, false); !s) return s;
    }
    for (const auto& item : a.get("axes").elements()) {
        if (!item.isObject()) return Error::make("invalid_arguments", "each axes entry must be {name, x, y, ticks}");
        if (Status s = holdAction(item, true); !s) return s;
    }
    for (const auto& n : a.get("release_actions").elements()) in.virtualActions.erase(n.asString());

    if (const Json* g = a.find("gamepad")) {
        if (!g->isObject()) return Error::make("invalid_arguments", "gamepad must be an object {index, leftStick, rightStick, leftTrigger, rightTrigger, buttons}");
        int index = static_cast<int>(g->get("index").asInt(0));
        if (index < 0 || index >= input::kMaxGamepads) return Error::make("invalid_arguments", "gamepad index must be 0..3");
        input::GamepadState& pad = in.pads[static_cast<size_t>(index)];
        pad.connected = g->get("connected").asBool(true);
        if (pad.name.empty()) pad.name = "Simulated gamepad";
        for (const char* key : {"leftStick", "rightStick"}) {
            if (!g->contains(key)) continue;
            Vec2 v;
            if (!readVec2(g->get(key), v)) return Error::make("invalid_arguments", std::string(key) + " must be [x, y] (y up) in -1..1");
            v.x = std::clamp(v.x, -1.f, 1.f);
            v.y = std::clamp(v.y, -1.f, 1.f);
            if (std::string(key) == "leftStick") {
                pad.lx = v.x;
                pad.ly = v.y;
            } else {
                pad.rx = v.x;
                pad.ry = v.y;
            }
        }
        if (g->contains("leftTrigger")) pad.lt = std::clamp(g->get("leftTrigger").asFloat(), 0.f, 1.f);
        if (g->contains("rightTrigger")) pad.rt = std::clamp(g->get("rightTrigger").asFloat(), 0.f, 1.f);
        if (g->contains("buttons")) {
            uint32_t mask = 0;
            for (const auto& b : g->get("buttons").elements()) {
                input::PadButton button;
                if (!input::parsePadButton(b.asString(), button)) {
                    std::string guess = str::closest(b.asString(), input::padButtonNames(), 3);
                    return Error::make("invalid_arguments", "unknown gamepad button '" + b.asString() + "'",
                                       guess.empty() ? "buttons: south east west north leftShoulder rightShoulder select start dpadUp ..."
                                                     : "did you mean '" + guess + "'?");
                }
                mask |= 1u << static_cast<unsigned>(button);
            }
            pad.buttons = mask;  // the list is the full set of held buttons
        }
    }

    if (const Json* m = a.find("mouse")) {
        if (!m->isObject()) return Error::make("invalid_arguments", "mouse must be an object {x, y, dx, dy, scroll, press, hold, release}");
        if (m->contains("x")) in.mouseX = std::clamp(m->get("x").asFloat(), 0.f, 1.f);
        if (m->contains("y")) in.mouseY = std::clamp(m->get("y").asFloat(), 0.f, 1.f);
        in.mouseDX += m->get("dx").asFloat(0.f);
        in.mouseDY += m->get("dy").asFloat(0.f);
        in.scrollY += m->get("scroll").asFloat(0.f);
        auto button = [&](const Json& v) -> Result<std::string> {
            std::string b = str::lower(v.asString());
            if (b != "left" && b != "right" && b != "middle") {
                return Error::make("invalid_arguments", "mouse button '" + b + "' must be left, right or middle");
            }
            return b;
        };
        for (const auto& v : m->get("press").elements()) {
            auto b = button(v);
            if (!b) return b.error();
            in.mousePressed.insert(*b);
            in.mouseHeld.insert(*b);
        }
        for (const auto& v : m->get("hold").elements()) {
            auto b = button(v);
            if (!b) return b.error();
            in.mouseHeld.insert(*b);
        }
        for (const auto& v : m->get("release").elements()) {
            auto b = button(v);
            if (!b) return b.error();
            in.mouseHeld.erase(*b);
            in.mouseReleased.insert(*b);
        }
    }
    return {};
}

void addInputTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"input_map", "Input actions",
             "Read or edit the project's input action map (input.json): named, device-independent actions bound to "
             "keyboard, mouse and gamepad. Scripts use action(\"jump\"), pressed(\"jump\"), axis(\"move\") and "
             "`on action \"jump\"` instead of raw keys, so one game works with keyboard+mouse and controllers and players "
             "can rebind. Defaults exist: move (WASD/arrows/left stick), look (mouse/right stick), jump, fire, aim, "
             "interact, sprint, pause, cursor. operation=get returns the map, the live state of every action and (with "
             "catalog=true) every valid binding source; set_action adds or replaces one action; remove_action deletes it; "
             "reset restores the defaults; set replaces everything; add_preset preset=drive adds the vehicle actions (throttle "
             "W/Up/right trigger, brake S/Down/left trigger, steer A-D/arrows/left stick, handbrake Space/A, shift_up/shift_down "
             "E/Q/shoulders) that vehicles with control \"player\" read. Action types: button (held/pressed/released), axis "
             "(-1..1), axis2d (x right, y forward/up). Binding strings: key:space, mouse:left, mouse:delta, "
             "pad:south, pad:leftStick, pad:rightTrigger; composites \"wasd\", \"arrows\", \"dpad\", \"ad\", \"qe\"; "
             "objects {source, scale, invertY, deadzone}. Example: set_action name=dash type=button "
             "bindings=[\"key:shift\", \"pad:east\"]. Test with sim_input.",
             "sim",
             object({{"operation", enumeration({"get", "set_action", "remove_action", "reset", "set", "add_preset"}, "What to do (default get)")},
                     {"preset", enumeration({"drive"}, "add_preset: a group of actions (drive = vehicle controls)")},
                     {"name", string("Action name (set_action, remove_action)")},
                     {"type", enumeration({"button", "axis", "axis2d"}, "Action type (set_action, default button)")},
                     {"bindings", array(Json::object({{"description", "A source string like \"key:w\", a composite name like \"wasd\", or an "
                                                                       "object {source|up/down/left/right|negative/positive, scale, invertX, invertY, deadzone}"}}),
                                        "Bindings (set_action)")},
                     {"deadzone", number("Radial deadzone for analog gamepad sources, 0..1 (default 0.15)")},
                     {"threshold", number("Analog value at which a button action counts as held (default 0.5)")},
                     {"description", string("What the action is for")},
                     {"map", Json::object({{"type", "object"}, {"description", "Whole map for operation=set: {actions: {name: {type, bindings}}}"}})},
                     {"catalog", boolean("Include the list of valid binding sources (get)")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 std::string op = a.get("operation").asString("get");
                 input::ActionMap map = engine.actionMap();
                 Json changed;
                 if (op == "set_action") {
                     std::string name = a.get("name").asString();
                     Json def = Json::object({{"type", a.get("type").asString("button")}, {"bindings", a.get("bindings")}});
                     for (const char* key : {"deadzone", "threshold", "description"}) {
                         if (a.contains(key)) def[key] = a.get(key);
                     }
                     auto action = input::ActionMap::actionFromJson(name, def);
                     if (!action) return ToolResult::error(action.error());
                     if (action->bindings.empty()) return ToolResult::error(Error::make("invalid_arguments", "an action needs at least one binding"));
                     map.set(std::move(*action));
                     changed = Json::object({{"set", name}});
                 } else if (op == "remove_action") {
                     std::string name = a.get("name").asString();
                     if (!map.remove(name)) return ToolResult::error(unknownAction(map, name));
                     changed = Json::object({{"removed", name}});
                 } else if (op == "reset") {
                     map = input::ActionMap::defaults();
                     changed = Json::object({{"reset", true}});
                 } else if (op == "set") {
                     auto parsed = input::ActionMap::fromJson(a.get("map"));
                     if (!parsed) return ToolResult::error(parsed.error());
                     map = std::move(*parsed);
                     changed = Json::object({{"replaced", true}});
                 } else if (op == "add_preset") {
                     const std::string preset = a.get("preset").asString("drive");
                     if (preset != "drive") {
                         return ToolResult::error(Error::make("invalid_arguments", "unknown action preset '" + preset + "'", "presets: drive"));
                     }
                     Json added = Json::array();
                     for (auto& action : physics::driveActions()) {
                         if (map.find(action.name)) continue;  // keep the project's own bindings
                         added.push(action.name);
                         map.set(std::move(action));
                     }
                     changed = Json::object({{"preset", preset}, {"added", added}});
                 } else if (op != "get") {
                     return ToolResult::error(Error::make("invalid_arguments", "unknown operation '" + op + "'",
                                                          "use get, set_action, remove_action, reset, set or add_preset"));
                 }
                 if (op != "get") {
                     if (Status s = engine.setActionMap(std::move(map)); !s) return fail(s);
                 }
                 Json out = Json::object({{"path", "input.json"}, {"map", engine.actionMap().toJson()}});
                 if (!changed.isNull()) out["changed"] = changed;
                 out["state"] = stateJson(engine.input());
                 if (a.get("catalog").asBool()) out["catalog"] = input::ActionMap::bindingCatalog();
                 std::string summary = op == "get" ? std::to_string(engine.actionMap().actions().size()) + " input actions"
                                                   : "input map updated (" + op + ")";
                 return ToolResult::json(out, summary);
             }});
}

}  // namespace sky::tools
