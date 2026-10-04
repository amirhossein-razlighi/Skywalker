# Input

Games ask for **actions** such as `jump` and `move`, not for keys. The project's action map binds each action to keyboard keys, mouse buttons and axes, and gamepad buttons and sticks, so one script works with every device and players can rebind controls. Raw keys (`key("w")`, `on key "space"`) remain available as a lower-level option. Input is part of the deterministic simulation: the same input sequence replays identically, and `sim_input` drives a game exactly like a player.

## Concepts

### Action types

| Type | Value | Wander |
|---|---|---|
| `button` | Held, pressed and released, plus an analog value 0..1 (a trigger pulled halfway is 0.5) | `action("jump")`, `pressed("jump")`, `released("jump")`, `axis("fire")` |
| `axis` | One number, −1..1 | `axis("steer")` |
| `axis2d` | Two numbers: **x right, y forward/up** | `axis("move")` returns a vector `(x, y, 0)` |

Conventions that make values mean the same on every device:

- **y is up or forward** for sticks, `wasd` and `mouse:delta`. Moving the mouse up gives a positive y.
- **Digital composites are normalized**: W and D together give length 1, not 1.41.
- **Mouse deltas and scrolls are per tick**: pixels since the last tick, scaled by the binding. A stick is a rate; the default `look` binding scales the right stick so that full deflection turns about as fast as a quickly moved mouse, and a script can use one `look` value for both.
- **Edges are per tick.** `pressed` is true for exactly one tick, and so is `released`. A key tapped and released between two ticks still produces one `pressed` and one `released`.
- **Radial deadzone.** Sticks report nothing inside the deadzone and are rescaled so the edge still reaches 1. The default is 0.15, per action or per binding.

### The action map: input.json

The map is stored in `input.json` at the project root; defaults apply when the file is missing. `input_map` edits it, or you edit it by hand; changes are picked up within two seconds. A binding is a string or an object:

```json
{
  "version": 1,
  "actions": {
    "jump":  {"type": "button", "bindings": ["key:space", "pad:south"]},
    "move":  {"type": "axis2d", "deadzone": 0.2, "bindings": ["wasd", "arrows", "pad:leftStick"]},
    "look":  {"type": "axis2d", "bindings": [
               {"source": "mouse:delta", "scale": 0.1, "invertY": true},
               {"source": "pad:rightStick", "scale": 3}]},
    "zoom":  {"type": "axis", "bindings": ["mouse:scroll.y", {"negative": "key:q", "positive": "key:e"}]},
    "fire":  {"type": "button", "threshold": 0.4, "bindings": ["mouse:left", "pad:rightTrigger"]}
  }
}
```

When several bindings of an action are active, the strongest wins (the largest magnitude), so a keyboard and a controller never add up beyond 1. With several pads connected, the strongest stick wins.

### Sources

| Source | Value |
|---|---|
| `key:<name>` | A character (`w`, `1`, `;`) or `space`, `enter`, `escape`, `tab`, `backspace`, `delete`, `left`, `right`, `up`, `down`, `shift`, `ctrl`, `alt`, `cmd`, `home`, `end`, `pageup`, `pagedown`, `f1`..`f12`. Aliases such as `esc` and `return` are accepted. |
| `mouse:left`, `mouse:right`, `mouse:middle` | Buttons |
| `mouse:delta`, `.x`, `.y` | Movement since the last tick, pixels, +y up |
| `mouse:scroll`, `.x`, `.y` | Scroll since the last tick |
| `mouse:position`, `.x`, `.y` | Cursor in the viewport, 0..1 from the top-left |
| `pad:south`, `pad:east`, `pad:west`, `pad:north` | Face buttons by position. Aliases `a`, `b`, `x`, `y`, `cross`, `circle`, `square`, `triangle` |
| `pad:leftShoulder`, `pad:rightShoulder`, `pad:select`, `pad:start`, `pad:guide` | Other buttons (aliases `lb`, `rb`, `l1`, `r1`, `back`, `menu`, `options`) |
| `pad:leftStickButton`, `pad:rightStickButton` | Stick clicks (`l3`, `r3`) |
| `pad:dpadUp`, `pad:dpadDown`, `pad:dpadLeft`, `pad:dpadRight` | D-pad |
| `pad:leftStick`, `pad:rightStick`, `.x`, `.y` | Analog sticks |
| `pad:leftTrigger`, `pad:rightTrigger` | Analog 0..1; also usable as buttons |

### Composites

Composites build an axis from buttons:

- 2D: `wasd`, `arrows`, `ijkl`, `dpad`;
- 1D: `ad`, `leftright`, `updown`, `ws`, `qe`, `dpad_x`, `dpad_y`;
- objects with any button sources: `{"up", "down", "left", "right"}` for 2D and `{"negative", "positive"}` for 1D.

### Options

On a binding object: `scale`, `invertX`, `invertY`, `deadzone`. On an action: `type`, `bindings`, `deadzone` (default 0.15), `threshold` (the analog value that counts as held for buttons, default 0.5) and `description`.

Typos get hints: `key:spcae` answers *did you mean 'key:space'?*, `"wsad"` answers *did you mean 'wasd'?*, and unknown fields and types list the valid ones.

### The default map

| Action | Type | Keyboard and mouse | Gamepad |
|---|---|---|---|
| `move` | axis2d | WASD, arrow keys | Left stick |
| `look` | axis2d | Mouse movement (0.1 per pixel) | Right stick |
| `jump` | button | Space | South |
| `fire` | button | Left mouse button | Right trigger |
| `aim` | button | Right mouse button | Left trigger |
| `interact` | button | E | West |
| `sprint` | button | Shift | Left stick click |
| `pause` | button | Escape | Start |
| `cursor` | axis2d | Mouse position, 0..1 | — |

`input_map {"operation": "reset"}` restores these. Because the map is plain data, a game can define its own set (`build`, `cancel`, `select_unit`, `camera_pan`) and delete the defaults.

## How to read input in Wander

| Builtin or trigger | Meaning |
|---|---|
| `action(name)` | True while the action is active |
| `pressed(name)` | True on the one tick the action became active |
| `released(name)` | True on the one tick it stopped |
| `axis(name)` | A number (button, axis) or a vector `(x, y, 0)` (axis2d) |
| `on action "name"` | Runs on the tick the action is pressed, before `on tick` of the same entity |
| `key(name)`, `key_pressed(name)`, `on key "name"` | Raw keys, held or pressed this tick |
| `cursor_lock(on)` | Hides and captures the mouse cursor for mouse look (the standalone player honors it; resets when the game stops) |
| `quit_game()` | Closes the standalone player (the editor ignores it) |

Unknown action names are runtime errors listing the actions with a did-you-mean suggestion. Scripts never see which device produced an action; to react to a specific key, use `key("space")` or `on key "space"`.

```wander
behavior Player
  intent "Move with the move action, sprint while held, hop on jump."
  param walk_speed = 4 in 0..10 "walking speed (m/s)"
  param sprint_speed = 7 in 0..15 "sprinting speed (m/s)"

  on tick
    let m = axis("move")                     -- WASD, arrows or the left stick
    let speed = walk_speed
    if action("sprint") then
      speed = sprint_speed
    end
    move self by (m.x * speed * dt, 0, -m.y * speed * dt)
  end

  on action "jump"                           -- space or the south button
    move self by (0, 0.5, 0)
  end
end
```

With a physics character, pass the axis to `walk`: `walk(self, (axis("move").x, 0, -axis("move").y))` (see [Physics](physics.md)).

## Gamepads and the mouse

- **Up to four gamepads.** The editor reads connected controllers through Apple's GameController framework while the game plays; the engine sees only positions and buttons, so any supported controller behaves the same.
- **Gamepad state is not saved** with the scene, and a disconnected pad contributes nothing.
- **The mouse reaches the game while it plays:** movement, the three buttons, the wheel and the normalized cursor position. Clicking an entity still fires `on click`.
- **Modifier keys** (`shift`, `ctrl`, `alt`, `cmd`) are keys like any other.

### First-person mouse look

```wander
behavior Look
  intent "Turn with the look action; the mouse is captured while playing."
  var yaw = 0
  var pitch = 0

  on start
    cursor_lock(true)
  end

  on tick
    let l = axis("look")
    yaw = yaw - l.x
    pitch = clamp(pitch + l.y, -80, 80)
    self.rotation = (pitch, yaw, 0)
  end

  on action "pause"
    cursor_lock(false)
  end
end
```

### A click-to-place cursor

The default `cursor` action gives the normalized mouse position, `(x, y, 0)` from the top-left, and `fire` is the click:

```wander
behavior Placer
  intent "Log where the player clicks, in normalized screen coordinates."
  on tick
    if pressed("fire") then
      let c = axis("cursor")
      log "clicked at " + str(c.x) + ", " + str(c.y)
    end
  end
end
```

## How to test input

`sim_input` drives the game exactly like a player. It applies from the next tick, so follow it with `sim_control` `step`; 60 ticks are one second.

```tool
sim_input {"actions": ["jump"]}
sim_input {"actions": [{"name": "fire", "ticks": 30}]}
sim_input {"axes": [{"name": "move", "x": 0, "y": 1, "ticks": 120}]}
sim_input {"release_actions": ["fire"]}
sim_input {"press": ["space"], "hold": ["d"]}
sim_input {"gamepad": {"leftStick": [0.6, 0.8], "rightTrigger": 1, "buttons": ["south"]}}
sim_input {"mouse": {"x": 0.5, "y": 0.5, "dx": 12, "dy": -4, "hold": ["left"]}}
sim_control {"action": "step", "ticks": 60}
```

- `actions` taps an action (one tick) or holds it for `ticks`; action input overrides devices for its duration.
- `axes` holds an axis action at a value.
- `press`, `hold` and `release` are raw keys; they go through the map, so `hold: ["w"]` also moves `move`.
- `gamepad` and `mouse` set device state, so they test your bindings too. `buttons` is the full set of held buttons and persists until changed; mouse movement is per tick.

`input_map` with operation `get` also returns the **live state of every action** from the last tick (`held`, `pressed`, `released`, `value`): the quickest answer to "what does the game see?".

## Recipes

### Add a dash

```tool
input_map {"operation": "set_action", "name": "dash", "type": "button", "bindings": ["key:shift", "pad:east"], "description": "Short burst forward"}
behavior_set {"entity": "Player", "name": "Dash", "intent": "Dash 3 m forward on the dash action.", "source": "on action \"dash\"\n  move self by forward(self) * 3\nend"}
```

### A bot that checks the player can reach the door

```tool
sim_control {"action": "stop"}
sim_input {"axes": [{"name": "move", "x": 0, "y": 1, "ticks": 180}]}
sim_control {"action": "step", "ticks": 180}
entity_get {"entity": "Player"}
```

Walk, then jump a gap, one call each:

```tool
sim_input {"axes": [{"name": "move", "x": 0, "y": 1, "ticks": 50}]}
sim_control {"action": "step", "ticks": 50}
sim_input {"actions": ["jump"], "axes": [{"name": "move", "x": 0, "y": 1, "ticks": 60}]}
sim_control {"action": "step", "ticks": 60}
```

### Controls for a strategy game

Replace the defaults with your own actions:

```tool
input_map {"operation": "set", "map": {"actions": {"camera_pan": {"type": "axis2d", "bindings": ["wasd", "arrows", "pad:leftStick"]}, "camera_zoom": {"type": "axis", "bindings": ["mouse:scroll.y", "qe"]}, "select": {"type": "button", "bindings": ["mouse:left", "pad:south"]}, "cancel": {"type": "button", "bindings": ["mouse:right", "key:escape", "pad:east"]}, "cursor": {"type": "axis2d", "bindings": ["mouse:position"]}}}}
input_map {"operation": "get", "catalog": true}
```

### A rebinding menu

Read the current map with `input_map` (operation `get`), show each action's sources, and write the chosen source back with `set_action`. The `input.json` file is the save file for controls.

## For platform code

Platform code feeds input to the engine through the C API in `sky_api.h`. The editor and the standalone player both use these calls:

```c
void sky_input_key(SkyEngine* engine, const char* key, int down); /* "w", "space", "shift", ... */
void sky_input_click(SkyEngine* engine, uint64_t entity);
void sky_input_mouse_move(SkyEngine* engine, float x, float y, float dx, float dy);
void sky_input_mouse_button(SkyEngine* engine, int button, int down);
void sky_input_scroll(SkyEngine* engine, float dx, float dy);
void sky_input_text(SkyEngine* engine, const char* utf8);

typedef struct SkyGamepad {
    float left_x, left_y, right_x, right_y;
    float left_trigger, right_trigger;
    uint32_t buttons;
} SkyGamepad;
void sky_input_gamepad(SkyEngine* engine, int index, int connected, const char* name, const SkyGamepad* state);
```

- `sky_input_mouse_move` takes the cursor position 0..1 from the top-left and the movement in pixels with +y **down** as on screen; the engine flips it. Mouse buttons are 0 left, 1 right, 2 middle.
- `sky_input_text` delivers typed UTF-8 characters to UI text fields with the next tick.
- Call `sky_input_gamepad` once per frame for every connected controller (index 0..3), and once with `connected` = 0 when one leaves. Sticks are −1..1 with +y up, triggers 0..1. `buttons` is a bitmask: bit 0 south, 1 east, 2 west, 3 north, 4 left shoulder, 5 right shoulder, 6 left trigger, 7 right trigger, 8 select, 9 start, 10 left stick click, 11 right stick click, 12–15 d-pad up, down, left, right, 16 guide.

At the start of each fixed tick the engine evaluates the action map once, then runs Wander, then clears the per-tick values. When several ticks run in one frame, the first one receives the mouse movement.

## Pitfalls

- **`sim_input` needs a step.** Injected input applies from the next tick; call `sim_control` `step` afterwards.
- **Per-tick values.** Mouse deltas and scrolls are amounts per tick, not per second; do not multiply them by `dt`.
- **`pressed` lasts one tick.** Read it every tick (in `on tick`) or use `on action`; a handler that only runs now and then misses it.
- **Cursor lock in the editor.** `cursor_lock` is honored by the standalone player and resets when the game stops.
- **Unknown action names are runtime errors.** Keep `input.json` and scripts in sync; `input_map` lists the actions.

!!! agent "For agents"

    Define actions, write scripts against them, then test like a player:

    ```tool
    input_map {"operation": "get"}                                                           # actions and live state
    input_map {"operation": "set_action", "name": "dash", "type": "button", "bindings": ["key:shift", "pad:east"]}
    sim_input {"axes": [{"name": "move", "x": 0, "y": 1, "ticks": 120}]}                    # walk forward 2 s
    sim_control {"action": "step", "ticks": 120}
    sim_trace {"entities": ["Player"], "properties": ["transform.position"], "ticks": 120, "hold": ["w"]}
    ```

    Prefer actions over raw keys in game code: they work with gamepads, and `sim_input` can drive them directly.

## Reference

- Tools: [`input_map`](../reference/tools/sim.md#input_map), [`sim_input`](../reference/tools/sim.md#sim_input), [`sim_control`](../reference/tools/sim.md#sim_control), [`sim_trace`](../reference/tools/sim.md#sim_trace).
- Wander: [input builtins](../reference/wander.md#input).
- C API: [Game input](../reference/capi.md).
- Related pages: [Physics and navigation](physics.md), [2D and UI](2d-ui.md), [Shipping](shipping.md).
- Design document: [docs/INPUT.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/INPUT.md).
