#pragma once
// Input action map: named, device-independent actions bound to keys, mouse and gamepad.
//
// Game logic asks for actions ("jump", "move") instead of keys, so one script works with
// keyboard, mouse and gamepad and players can rebind. The map lives in the project as
// `input.json` (see docs/INPUT.md) and is edited with the `input_map` tool.
//
// Binding sources (strings):
//   key:<name>            key:w key:space key:left key:shift key:1 ...
//   mouse:left|right|middle                      mouse buttons
//   mouse:delta[.x|.y]    movement since the last tick (pixels, +y up); mouse:scroll[.x|.y]
//   mouse:position[.x|.y] cursor position in the viewport, 0..1 (origin top-left)
//   pad:<button>          south east west north leftShoulder rightShoulder leftTrigger rightTrigger
//                         select start leftStickButton rightStickButton (L3/R3) dpadUp dpadDown dpadLeft dpadRight
//   pad:leftStick[.x|.y]  pad:rightStick[.x|.y]  analog sticks (+y up) with radial deadzone
//   pad:leftTrigger / pad:rightTrigger           as a 0..1 value (also usable as buttons)
// Composites: "wasd" "arrows" "ijkl" "dpad" (2D); "ad" "leftright" "updown" "ws" "qe" (1D),
// or objects {up,down,left,right} / {negative,positive} of button sources.

#include <map>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/input/InputState.h"

namespace sky::input {

enum class ActionType { Button, Axis, Axis2D };
const char* toString(ActionType t);

struct Source {
    enum class Type { None, Key, MouseButton, MouseDelta, MouseScroll, MousePosition, PadButton, PadStick, PadTrigger };
    Type type = Type::None;
    std::string name;              // key / mouse button name
    PadButton button = PadButton::South;
    int stick = 0;                 // 0 left, 1 right (PadStick); 0 left, 1 right (PadTrigger)
    int component = -1;            // -1 = both axes, 0 = x, 1 = y
    std::string text;              // canonical text, e.g. "pad:leftStick.x"
};
Result<Source> parseSource(std::string_view text);

struct Binding {
    enum class Kind { Single, Composite1D, Composite2D };
    Kind kind = Kind::Single;
    Source source;                 // Single
    Source negative, positive;     // Composite1D (left/right)
    Source up, down, left, right;  // Composite2D
    std::string compositeName;     // set when written as a named composite ("wasd")
    float scale = 1.f;
    bool invertX = false;
    bool invertY = false;
    float deadzone = -1.f;         // <0 = the action's deadzone

    Json toJson() const;
};

struct Action {
    std::string name;
    ActionType type = ActionType::Button;
    std::vector<Binding> bindings;
    float deadzone = 0.15f;        // radial deadzone for analog gamepad sources
    float threshold = 0.5f;        // analog value at which a Button action counts as held
    std::string description;

    Json toJson() const;
};

class ActionMap {
public:
    /// move, look, jump, fire, aim, interact, sprint, pause, cursor.
    static ActionMap defaults();
    static Result<ActionMap> fromJson(const Json& j);
    Json toJson() const;

    const std::vector<Action>& actions() const { return actions_; }
    const Action* find(std::string_view name) const;
    std::vector<std::string> names() const;
    /// Adds or replaces an action.
    void set(Action action);
    bool remove(std::string_view name);

    /// Computes every action's state for this tick from the raw device state and virtual
    /// input, using the previous values in `state.actions` for press/release edges.
    void evaluate(InputState& state) const;

    /// Parses one action from JSON ({type, bindings, deadzone?, threshold?, description?}).
    static Result<Action> actionFromJson(const std::string& name, const Json& j);

    /// All binding sources and composites, for documentation and error hints.
    static Json bindingCatalog();

private:
    std::vector<Action> actions_;
};

/// Applies the radial deadzone to a stick vector: zero inside, rescaled so the output reaches 1.
Vec2 applyRadialDeadzone(Vec2 v, float deadzone);

}  // namespace sky::input
