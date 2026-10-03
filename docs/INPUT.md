# Input

Games ask for **actions** ("jump", "move"), not keys. An action map in the project binds
actions to the keyboard, mouse and gamepads, so one script works with all of them and players
can rebind. Raw keys (`key("w")`, `on key "space"`) still work as a lower-level option.

- [Quick start](#quick-start)
- [Action types](#action-types)
- [Bindings](#bindings)
- [The default map](#the-default-map)
- [Wander](#wander)
- [Gamepads and the mouse](#gamepads-and-the-mouse)
- [Testing and playtest bots](#testing-and-playtest-bots)
- [Agent recipes](#agent-recipes)
- [For platform code](#for-platform-code)

## Quick start

```wander
behavior Player
  var speed = 4
  var vy = 0
  on tick
    let m = axis("move")                           -- WASD, arrows or the left stick
    move self by (m.x * speed * dt, 0, -m.y * speed * dt)
    if action("sprint") then speed = 7 else speed = 4 end
  end
  on action "jump"                                 -- space or the A button
    vy = 6
  end
end
```

```
input_map {"operation": "set_action", "name": "dash", "type": "button", "bindings": ["key:shift", "pad:east"]}
```

## Action types

| Type | Value | Wander |
|---|---|---|
| `button` | held / pressed / released, and an analog value 0..1 (a trigger pulled half way is 0.5) | `action("jump")`, `pressed("jump")`, `released("jump")`, `axis("fire")` |
| `axis` | one number, -1..1 | `axis("steer")` |
| `axis2d` | two numbers: **x right, y forward/up** | `axis("move")` returns a vector `(x, y, 0)` |

Conventions that make the numbers mean the same everywhere:

- **y is up/forward** for sticks, `wasd`, `mouse:delta`. Moving the mouse up gives a positive y.
- Digital composites are **normalized**: pressing W and D together gives length 1, not 1.41.
- Mouse deltas and scrolls are **per tick** amounts (pixels since the last tick, scaled by the
  binding). A stick is a *rate*: the default `look` binding scales the right stick so full
  deflection turns about as fast as a quickly moved mouse, so a script can use one `look` value.
- **Edges are per tick.** `pressed` is true for exactly one tick, `released` likewise. A key
  tapped and released between two ticks still produces one `pressed` and one `released`.
- Gamepad sticks use a **radial deadzone**: nothing inside it, rescaled so the edge still
  reaches 1. Default 0.15, per action or per binding.

## Bindings

The map is stored in **`input.json`** at the project root (defaults apply when it is
missing). The `input_map` tool edits it; so can you, and changes are picked up within two
seconds. A binding is a string or an object:

```json
{
  "version": 1,
  "actions": {
    "jump":  { "type": "button", "bindings": ["key:space", "pad:south"] },
    "move":  { "type": "axis2d", "deadzone": 0.2, "bindings": ["wasd", "arrows", "pad:leftStick"] },
    "look":  { "type": "axis2d", "bindings": [
                 { "source": "mouse:delta", "scale": 0.1, "invertY": true },
                 { "source": "pad:rightStick", "scale": 3 } ] },
    "zoom":  { "type": "axis",   "bindings": ["mouse:scroll.y", {"negative": "key:q", "positive": "key:e"}] },
    "fire":  { "type": "button", "threshold": 0.4, "bindings": ["mouse:left", "pad:rightTrigger"] }
  }
}
```

**Sources**

| Source | Value |
|---|---|
| `key:<name>` | A character (`w`, `1`, `;`) or `space enter escape tab backspace delete left right up down shift ctrl alt cmd home end pageup pagedown f1`..`f12`. Aliases like `esc`, `return` are accepted. |
| `mouse:left` `right` `middle` | Button. |
| `mouse:delta[.x\|.y]` | Movement since the last tick, pixels, +y up. |
| `mouse:scroll[.x\|.y]` | Scroll since the last tick. |
| `mouse:position[.x\|.y]` | Cursor in the viewport, 0..1 from the top-left. |
| `pad:south` `east` `west` `north` | Face buttons by position (A/B/X/Y on Xbox, Cross/Circle/Square/Triangle on PlayStation). Aliases `a b x y cross circle square triangle`. |
| `pad:leftShoulder` `rightShoulder` `select` `start` `guide` | Other buttons (`lb rb l1 r1 back menu options`). |
| `pad:leftStickButton` `rightStickButton` | Stick clicks (`l3`, `r3`). |
| `pad:dpadUp` `dpadDown` `dpadLeft` `dpadRight` | D-pad. |
| `pad:leftStick[.x\|.y]` `pad:rightStick[.x\|.y]` | Analog sticks. |
| `pad:leftTrigger` `pad:rightTrigger` | Analog 0..1 (also fine as buttons). |

**Composites** build an axis from buttons: 2D `wasd` `arrows` `ijkl` `dpad`; 1D `ad` `leftright`
`updown` `ws` `qe` `dpad_x` `dpad_y`; or objects `{"up","down","left","right"}` /
`{"negative","positive"}` with any button sources.

**Options** on a binding object: `scale`, `invertX`, `invertY`, `deadzone`. On an action:
`type`, `bindings`, `deadzone` (default 0.15), `threshold` (analog value that counts as "held" for
buttons, default 0.5), `description`.

When several bindings are active the strongest wins (largest magnitude), so a keyboard and
a controller never add up beyond 1. Several connected pads: the strongest stick wins.

Typos get hints: `key:spcae` → *did you mean 'key:space'?*, `"wsad"` → *did you mean 'wasd'?*,
unknown fields and types list the valid ones.

## The default map

| Action | Type | Keyboard / mouse | Gamepad |
|---|---|---|---|
| `move` | axis2d | WASD, arrow keys | left stick |
| `look` | axis2d | mouse movement (0.1 per pixel) | right stick |
| `jump` | button | space | south (A) |
| `fire` | button | left mouse button | right trigger |
| `aim` | button | right mouse button | left trigger |
| `interact` | button | E | west (X) |
| `sprint` | button | shift | left stick click |
| `pause` | button | escape | start |
| `cursor` | axis2d | mouse position, 0..1 | - |

`input_map {"operation":"reset"}` restores these. Because the map is plain data, a game can
define its own set (`build`, `cancel`, `select_unit`, `camera_pan`) and delete the defaults.

## Wander

```
action("jump")      true while the action is active
pressed("jump")     true for the one tick it became active
released("jump")    true for the one tick it stopped
axis("move")        number (button/axis) or vector (x, y, 0) for axis2d
on action "jump"    handler that runs on the tick the action is pressed
```

Unknown names are runtime errors with the list of actions and a "did you mean" suggestion.
`on action` runs in entity order like every handler, before `on tick` of the same entity.
Scripts never see which device was used; to react to a specific key use `key("space")` or
`on key "space"` as before.

## Gamepads and the mouse

- Up to four gamepads. Connected controllers are read through Apple's GameController
  framework (Xbox, PlayStation, Switch Pro, MFi) by the editor while the game plays; the
  engine only sees positions and buttons, so any controller works the same.
- Gamepad state is **not** part of the saved scene and a disconnected pad contributes nothing.
- The mouse reaches the game while it plays: movement, the three buttons, the wheel and the
  normalized cursor position. Clicking an entity still fires `on click`.
- Modifier keys (`shift`, `ctrl`, `alt`, `cmd`) are keys like any other.

## Testing and playtest bots

`sim_input` drives the game exactly like a player. It applies from the next tick, so follow
it with `sim_control step`. 60 ticks are one second.

```
sim_input {"actions": ["jump"]}                                     -- tap: pressed on one tick
sim_input {"actions": [{"name": "fire", "ticks": 30}]}              -- hold half a second
sim_input {"axes": [{"name": "move", "x": 0, "y": 1, "ticks": 120}]} -- walk forward for 2 s
sim_input {"release_actions": ["fire"]}
sim_input {"press": ["space"], "hold": ["d"], "release": ["d"]}     -- raw keys (go through the map)
sim_input {"gamepad": {"leftStick": [0.6, 0.8], "rightTrigger": 1, "buttons": ["south"]}}
sim_input {"mouse": {"x": 0.5, "y": 0.5, "dx": 12, "dy": -4, "hold": ["left"]}}
```

Action input overrides devices for its duration. `gamepad` and `mouse` set *device* state,
so they test your bindings too (`buttons` is the full set of held buttons and persists until
changed; mouse movement is per tick).

`input_map` (operation `get`) also returns the **live state of every action** from the last
tick (`held`, `pressed`, `released`, `value`), the quickest way to answer "what does the game
see?". Everything is deterministic: the same input sequence replays identically.

## Agent recipes

**Add a dash.**
```
input_map {"operation":"set_action","name":"dash","type":"button","bindings":["key:shift","pad:east"]}
behavior_set {"entity":"Player","name":"Dash","source":"on action \"dash\"\n  move self by forward(self) * 3\nend"}
```

**Test that the player can reach the door** (a bot):
```
sim_control {"action":"stop"}
sim_input {"axes":[{"name":"move","x":0,"y":1,"ticks":180}]}
sim_control {"action":"step","ticks":180}
entity_get {"entity":"Player"}          -- position near the door?
```

**Walk, then jump the gap**, in one call each:
```
sim_input {"axes":[{"name":"move","x":0,"y":1,"ticks":50}]}
sim_control {"action":"step","ticks":50}
sim_input {"actions":["jump"],"axes":[{"name":"move","x":0,"y":1,"ticks":60}]}
sim_control {"action":"step","ticks":60}
```

**Mouse look in first person.**
```wander
behavior Look
  var yaw = 0
  var pitch = 0
  on tick
    let l = axis("look")
    yaw = yaw - l.x
    pitch = clamp(pitch + l.y, -80, 80)
    self.rotation = (pitch, yaw, 0)
  end
end
```

**A click-to-place cursor.** Bind `cursor` (default) and read the normalized position
in a UI or strategy game: `axis("cursor")` gives `(x, y, 0)` from the top-left, with
`pressed("fire")` as the click.

**Rebinding menu.** Read `input_map` for the current map, show the sources, and write the
chosen source back with `set_action`. The file is the save game for controls.

## For platform code

Input reaches the engine through the C API (`engine/capi/include/sky_api.h`):

```c
sky_input_key(engine, "w", 1);                       // keys by name
sky_input_mouse_move(engine, x01, y01, dx, dy);      // normalized position + point deltas (+y down)
sky_input_mouse_button(engine, 0, 1);                // 0 left, 1 right, 2 middle
sky_input_scroll(engine, dx, dy);
sky_input_gamepad(engine, index, connected, name, &state);   // SkyGamepad: sticks, triggers, button bits
```

The editor feeds keys, mouse and (through `GamepadBridge`, `editor/Sources/Engine/GamepadInput.swift`)
controllers only while the game plays. At the start of each fixed tick the engine evaluates the
action map once (`ActionMap::evaluate`), then runs Wander, then clears the per-tick values
(`InputState::endTick`). When several ticks run in one frame, the first receives the mouse
movement. A player build would use the same API from its own window and controller code.
