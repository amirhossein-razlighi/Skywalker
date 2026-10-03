#pragma once
// Raw input state as fed by the platform (editor, player, agents) and the evaluated
// per-action state that game logic reads. No dependency on the scene or Wander.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/math/Math.h"

namespace sky::input {

/// Gamepad buttons, named by position so they mean the same on Xbox, PlayStation and Switch
/// controllers: south = A / Cross, east = B / Circle, west = X / Square, north = Y / Triangle.
enum class PadButton : uint8_t {
    South, East, West, North,
    LeftShoulder, RightShoulder, LeftTrigger, RightTrigger,
    Select, Start, LeftStick, RightStick,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Guide,
    Count
};

const char* toString(PadButton b);
/// Accepts canonical names and common aliases ("a", "cross", "lb", "l1", "back", ...).
bool parsePadButton(std::string_view name, PadButton& out);
const std::vector<std::string>& padButtonNames();

/// Canonical lower-case key name: "Esc" -> "escape", "Return" -> "enter", "Control" -> "ctrl".
std::string canonicalKey(std::string_view name);
/// Whether a (canonical) key name is a single character or a known named key.
bool isKnownKey(std::string_view canonical);
const std::vector<std::string>& namedKeys();

constexpr int kMaxGamepads = 4;

struct GamepadState {
    bool connected = false;
    std::string name;
    float lx = 0, ly = 0;  // left stick, -1..1, +y = up/forward
    float rx = 0, ry = 0;  // right stick
    float lt = 0, rt = 0;  // analog triggers 0..1
    uint32_t buttons = 0;  // bit i = PadButton i

    bool button(PadButton b) const { return (buttons >> static_cast<unsigned>(b)) & 1u; }
    void setButton(PadButton b, bool down) {
        const uint32_t bit = 1u << static_cast<unsigned>(b);
        buttons = down ? (buttons | bit) : (buttons & ~bit);
    }
};

/// The evaluated state of one action for the current tick.
struct ActionState {
    bool held = false;      // active this tick
    bool pressed = false;   // became active this tick
    bool released = false;  // became inactive this tick
    bool vec2 = false;      // a 2D axis (Wander axis() returns a vector)
    float x = 0, y = 0;     // value: buttons 0..1 in x, axes -1..1 (axis2d uses y too; +y = up/forward)
};

struct InputState {
    // Keyboard. Names are lower case: "w", "space", "left", "enter", "shift", "1", ...
    std::set<std::string> held;
    std::set<std::string> pressed;   // pressed since the last tick
    std::set<std::string> released;  // released since the last tick
    std::vector<uint64_t> clicked;   // entities clicked since the last tick

    // Mouse. Position is normalized to the viewport (0..1, origin top-left); delta is in
    // pixels since the last tick with +x right and +y UP (so it matches stick conventions).
    float mouseX = 0, mouseY = 0;
    float mouseDX = 0, mouseDY = 0;
    float scrollX = 0, scrollY = 0;
    std::set<std::string> mouseHeld;      // "left", "right", "middle"
    std::set<std::string> mousePressed;
    std::set<std::string> mouseReleased;

    std::array<GamepadState, kMaxGamepads> pads;

    // Evaluated by ActionMap::evaluate() at the start of every tick.
    std::map<std::string, ActionState> actions;

    // Virtual input injected by agents / playtest bots (sim_input): active for N more ticks.
    struct VirtualValue {
        Vec2 value{1.f, 0.f};
        int ticks = 1;
    };
    std::map<std::string, VirtualValue> virtualActions;

    // --- Platform feed (editor, player): turns device events into held / pressed / released sets ---------------
    /// A key went down or up; `key` is any spelling canonicalKey() accepts. Repeats while held are ignored.
    void keyEvent(std::string_view key, bool down);
    /// Cursor position normalized to the viewport and movement in points since the last call (+dy DOWN as on screen).
    void mouseMove(float x01, float y01, float dx, float dy);
    /// button: 0 left, 1 right, 2 middle.
    void mouseButton(int button, bool down);
    void scrollBy(float dx, float dy) {
        scrollX += dx;
        scrollY += dy;
    }
    /// Sticks -1..1 (+y up), triggers 0..1, buttons as a PadButton bitmask. `connected` false clears the pad.
    void gamepad(int index, bool connected, std::string_view name, float lx, float ly, float rx, float ry, float lt, float rt,
                 uint32_t buttons);

    /// Clears everything that only lasts one tick and advances the virtual input timers.
    void endTick();
    /// Forgets all input (play stopped).
    void clear();
};

}  // namespace sky::input
