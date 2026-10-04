# Simulation and time

Pressing play runs the game: behaviors, physics, animation, particles, audio and UI advance in fixed 1/60 s ticks
from a snapshot of the scene, and stopping restores that snapshot. The simulation is deterministic, so the same scene
and the same input replay identically, which is what lets agents step, trace and test a game the way a player would
play it. This page covers play control, determinism, the game's own pause and time scale, the `process` component,
and how real-time frames on fast displays stay smooth without touching the simulation.

<video controls muted loop playsinline preload="none" poster="../../assets/video/berrybrook/windmill.webp">
  <source src="../../assets/video/berrybrook/windmill.mp4" type="video/mp4">
</video>

*The berrybrook windmill: a Wander behavior ("The sails turn slowly in the breeze") turns the sails every tick.*

## Concepts

### Play states

| State | What happens | How you get there |
|---|---|---|
| Editing | Nothing ticks. Edits go to the scene and the history. Particles and some effects preview live. | Start, or `stop` |
| Playing | The engine snapshots the scene, then runs fixed ticks of 1/60 s. | `play` (++cmd+p++ in the editor) |
| Paused (editor) | Nothing ticks, but `step` advances exact ticks. | `pause` (++cmd+shift+p++) |
| Stopped | The pre-play snapshot is restored: positions, vars, spawned and destroyed entities, everything. | `stop` (++cmd+period++) |

`sim_control` drives all of them, and `step` runs N ticks synchronously and returns the logs and errors produced:

```tool
sim_control {"action": "play"}
sim_control {"action": "pause"}
sim_control {"action": "step", "ticks": 60}
sim_control {"action": "status"}
sim_control {"action": "stop"}
```

`step` takes 1 to 36000 ticks (60 = one second) and works while the editor pause is on, which is how agents test
behaviors frame by frame.

### Determinism

The simulation replays exactly. Several rules make that true:

| Rule | Consequence |
|---|---|
| Fixed ticks of 1/60 s in `Engine::step` | Results never depend on frame rate or machine speed. |
| Entities are processed in scene order | The order is part of the scene file and survives undo, redo, save and load. |
| Events are delivered on the next tick, in emission order | No handler sees an event in the same tick it was emitted. |
| Randomness is a PCG32 generator seeded by the scene's `seed` | `random`, `chance`, `pick`, `shuffle` and `random_int` replay. Wander has no access to the wall clock. |
| Every Wander handler run has a budget of 1,000,000 steps | Loops and calls are charged; a runaway loop aborts with an error at its line instead of hanging the engine. |
| Spawns are bounded and deferred | At most 256 `spawn`s per tick and 20,000 entities; spawned behaviors start next tick; `destroy` takes effect at the end of the tick. |
| Pause and time-scale requests apply at tick boundaries | A pause during which nothing runs is invisible to the simulation. |
| Stop restores the pre-play snapshot | Every run starts from the same state. |

Edits made while playing (by you, by an agent, by a tool) are atomic too: if one fails it is rolled back from a
snapshot. They are still undone when play stops.

### What happens in one tick

Every tick, the runtime first applies pause and time-scale requests made since the last tick and sends
`on pause` / `on resume`. It then visits active entities in scene order. For each script it starts new instances
(var initializers, `on start`, the initial state's `on enter`), resumes handlers whose `wait` elapsed, delivers last
tick's events, then physics contacts, key presses, input actions and clicks, and finally runs `on tick`
(behavior-level first, then the current state's). Physics, navigation, animation, particles, sprites, dialogue, UI,
audio and native modules advance in the same tick.

### Two kinds of pause

| | Editor pause | Game pause |
|---|---|---|
| Started by | `sim_control {"action": "pause"}`, the toolbar, ++cmd+shift+p++ | `pause_game()` in Wander, `sim_control {"action": "pause_game"}` |
| What stops | Everything: no ticks at all | Entities whose `process.mode` is `pausable` (the default) |
| What keeps running | Nothing (but `step` works) | UI canvases, entities with mode `always` or `when_paused` |
| Events | None | `on pause` and `on resume` reach every behavior, even stopped ones |
| Purpose | Inspect and debug | A pause menu inside the game |

## The process component

`process` decides how an entity and its children run while the game is paused or slowed down, in which order their
behaviors run, and whether their motion is smoothed on screen. Every field defaults to `inherit`: the value comes from
the nearest ancestor that sets it.

| Field | Values | Meaning |
|---|---|---|
| `mode` | `inherit`, `pausable`, `when_paused`, `always`, `disabled` | When the entity runs. Root default: `pausable`. |
| `priority` | integer | Behaviors run in (priority, scene order): lower first. Not inherited. |
| `clock` | `inherit`, `game`, `real` | `game` follows the time scale; `real` ignores it (menus, HUD). |
| `interpolation` | `inherit`, `on`, `off` | Whether the entity's motion is smoothed between ticks on screen. |

| `mode` | Runs while the game plays | Runs while the game is paused |
|---|---|---|
| `pausable` | yes | no |
| `when_paused` | no | yes (pause menu logic) |
| `always` | yes | yes (UI, music controllers) |
| `disabled` | no | no (frozen, still drawn) |

A UI canvas without its own setting runs `always` on the `real` clock, so menus work during a pause and in slow
motion. Give a HUD `mode: pausable` to freeze it with the game.

### What each system does

| System | While the game is paused | Under a time scale |
|---|---|---|
| Wander | Stopped instances keep their coroutines, timers and state; they still get `on pause` / `on resume`; events they handle wait and arrive when they run again; input and clicks are dropped | `dt`, timers and waits per instance |
| Physics and navigation | The world holds, unless the `physics_world` entity runs `always` | Steps `dt × scale`, split into substeps of at most one tick above 1 |
| Animation, sequences | Animators and cutscenes hold their pose | Per entity |
| Particles (CPU) | Emitters hold | Per emitter; GPU effects follow the game clock |
| Sprites, 2D cameras, dialogue | Hold | Per entity |
| UI | A canvas that does not run takes no input (default canvases run) | UI animations use real time |
| Audio | Every bus except `ui` pauses, so menu clicks still play | Not pitched |
| Native modules | Not ticked | `dt × scale` |

### Time in Wander

| Name | Meaning |
|---|---|
| `dt` | Seconds per tick for this entity: 1/60 scaled by the time scale, or real on the `real` clock |
| `time` | Game time: stops while paused, slows with the time scale |
| `frame` | The tick counter |
| `unscaled_dt()`, `unscaled_time()` | Real time, whatever the pause or scale |
| `time_scale(x)` | Sets the game clock speed (0 to 10) from the next tick and returns it |
| `pause_game()`, `resume_game()`, `is_paused()` | The game pause |

## How to build a pause menu

Put the menu logic on the pause menu's `ui_canvas` entity (canvases run `always`). Its child panel `PausePanel`
starts hidden.

=== "Wander"

    ```wander
    behavior PauseMenu
      intent "Escape toggles the pause menu; the Resume button closes it."
      on action "pause"
        if is_paused() then resume_game() else pause_game() end
      end
      on pause
        find("PausePanel").ui.visible = true
      end
      on resume
        find("PausePanel").ui.visible = false
      end
      on ui "Resume"
        resume_game()
      end
    end
    ```

=== "Tool call"

    ```tool
    sim_control {"action": "play"}
    sim_control {"action": "pause_game"}
    sim_control {"action": "step", "ticks": 30}
    process_info {"entity": "PauseMenu"}
    ui_interact {"element": "Resume"}
    sim_control {"action": "step", "ticks": 2}
    ```

=== "CLI"

    ```bash
    skywalker call process_info '{"entity": "PauseMenu"}' --project my_game --scene scenes/main.sky.json
    ```

The `pause` action is bound to Escape and the gamepad Start button in the default input map. Keep the canvas entity
itself enabled: a disabled entity runs nothing, not even `on pause`.

`process_info` reports the game clock (paused, time scale, game and real time), every entity with a `process`
component or a UI canvas, and, for one entity, its effective settings and which ancestor decided them. It warns when
a paused game has nothing that could resume it.

## Recipe: bullet time

`time_scale(0.25)` slows everything on the game clock; the HUD and menus on the real clock keep full speed. Budget the
effect in real time with `unscaled_dt()`:

```wander
behavior BulletTime
  intent "Holding the focus action slows the game to a quarter speed for at most three real seconds."
  var budget = 3
  on tick
    if action("focus") and budget > 0 then
      time_scale(0.25)
      budget -= unscaled_dt()
    else
      time_scale(1)
      budget = min(3, budget + unscaled_dt() * 0.5)
    end
  end
end
```

Give the controller entity `process.clock: real` so its own `dt` and timers are not slowed by the effect it controls.
Agents can test the same thing without input:

```tool
sim_control {"action": "time_scale", "scale": 0.25}
sim_trace {"entities": ["Hero"], "properties": ["transform.position"], "ticks": 120, "every": 20}
sim_control {"action": "time_scale", "scale": 1}
```

## Render interpolation

The simulation ticks at 60 Hz, but displays refresh at 60, 120 (ProMotion) or 144 Hz. Showing each tick for an uneven
number of frames reads as stutter, so real-time frames show the world **between** the last two ticks:

- `alpha = accumulator / fixed dt` says how far real time is into the next tick.
- The engine keeps every entity's transform from before the last tick and, for the time it takes to build one frame,
  writes the blend of previous and current (lerp for positions, slerp for rotations) into the scene, then restores the
  tick state. Meshes, cameras, lights, sprites, text, world UI, particle emitters, hair and bone attachments are
  smoothed this way; skinned meshes blend joint matrices; CPU particles move along their velocity; water, sky and GPU
  particles use the displayed time.
- The picture lags the simulation by up to one tick, as in every fixed-step design.

The simulation, tools and captures never see these in-between values. `viewport_capture` shows the exact tick state
unless you pass `alpha`. The editor viewport and the standalone player render with the current alpha.

### Opting out

- Per subtree: `process.interpolation: off` shows entities exactly at tick positions (pixel art, snapping
  puzzles).
- Per jump: `teleport(e, position)` in Wander, or the `sim_teleport` tool, moves an entity without smearing it across
  the screen for a frame. Moves longer than 25 m in one tick are never smeared either.
- Globally: `sim_control {"action": "play", "interpolation": false}`.

```wander
behavior Respawn
  intent "Falling below the world sends the player back to the checkpoint without a smear."
  on tick
    if self.position.y < -20 then
      teleport(self, find("Checkpoint"))
    end
  end
end
```

### on frame: cosmetic, display-rate code

`on frame` handlers run once per displayed frame, after interpolation, for touches that look best at display rate:
camera shake, bobbing, flicker, UI tweens. `dt` is the real time since the last frame (scaled by the time scale) and
`time` the displayed game time.

```wander
behavior Bob
  intent "Bob the camera gently at display rate."
  on frame
    self.position = self.position + (0, sin(time * 9) * 0.03, 0)
  end
end
```

They are **cosmetic**: they may set `position`, `rotation`, `scale`, `color` and fields of `transform`, `mesh`,
`light`, `camera`, `sprite`, `text`, `ui` and `light2d`, read anything, and call pure functions and read-only queries.
Every write is undone after the frame. Changing vars, `wait`, timers, `go to`, `move`/`rotate`/`look`, `emit`, `spawn`,
`destroy` and `random` are compile errors (`frame_not_cosmetic`); use `noise` instead of `random`. So the simulation is
identical however many frames are shown.

### Measuring smoothness

`sim_trace` with `display_hz` simulates a real display: each sample records the interpolation alpha and what that
frame shows next to the tick values, and the result reports `smoothness.stepJitter` (0 is perfectly even motion,
about 2 means every other frame repeats a tick) and frame `pacing`.

```tool
sim_trace {"entities": ["Hero"], "properties": ["transform.position"], "ticks": 120, "display_hz": 120, "every": 1}
process_info {}
viewport_capture {"alpha": 0.5, "frame_handlers": true}
```

`perf_stats` reports the live viewport's `frameFlow`, and the standalone player renders through its real pipeline at
a chosen display rate:

```bash
skywalker-player examples/hello_sky --capture-frame out.png --display-hz 120
```

## Recipe: test a behavior numerically

```tool
sim_input {"hold": ["w"]}
sim_trace {"entities": ["Hero"], "properties": ["transform.position", "vars.score"], "ticks": 180, "every": 30}
logs {"limit": 20}
```

`sim_trace` runs the game for N ticks, samples properties (`<component>.<field>`, `vars.<name>`, `name`, `enabled`)
and restores the edit-mode scene afterwards. `sim_input` injects keys, actions, axes, gamepad, mouse, clicks and events
for the next ticks; follow it with `sim_control` `step` or a trace.

## Pitfalls

- **Requests apply next tick.** After `pause_game()` or `time_scale(x)`, the current tick finishes at the old setting;
  `is_paused()` already returns `true`.
- **A paused game needs a way out.** If nothing runs `always` or `when_paused`, nothing can call `resume_game()`.
  `process_info` warns about this.
- **Disabled entities run nothing**, including `on pause`. Hide a menu with `ui.visible`, not by disabling its
  canvas.
- **Interpolation lags by up to one tick.** Logic must read tick state (it always does); never try to read the
  displayed position from a script.
- **Edits during play are temporary.** Stop restores the pre-play snapshot.
- **Five errors disable a script.** A runtime error aborts that handler run and is reported in `logs`; after five
  errors the script is disabled. Read `logs` after every `step`.

!!! agent "For agents"

    Test gameplay the way a player would, and prove it with numbers:

    ```tool
    sim_control {"action": "play"}                       # snapshot and start
    sim_input {"press": ["space"]}                       # act like a player
    sim_control {"action": "step", "ticks": 60}          # advance exactly one second; returns logs and errors
    viewport_capture {"view": "scene", "samples": 1}     # look at the tick state
    sim_trace {"entities": ["Hero"], "properties": ["transform.position"], "ticks": 120}
    process_info {}                                      # pause, time scale, smoothing
    sim_control {"action": "stop"}                       # restore the scene
    ```

    For behaviors with rules, prefer `test` blocks run by `wander_test` (see [Specs and tests](wander/testing.md)).

## Reference

- Tools: [`sim_control`](../reference/tools/sim.md#sim_control), [`sim_input`](../reference/tools/sim.md#sim_input),
  [`sim_trace`](../reference/tools/sim.md#sim_trace), [`process_info`](../reference/tools/sim.md#process_info),
  [`sim_teleport`](../reference/tools/sim.md#sim_teleport), [`logs`](../reference/tools/sim.md#logs),
  [`viewport_capture`](../reference/tools/view.md#viewport_capture)
- Component: [`process`](../reference/components/core.md#process)
- Wander: [`pause_game`](../reference/wander.md#time-pause_game), [`resume_game`](../reference/wander.md#time-resume_game),
  [`is_paused`](../reference/wander.md#time-is_paused), [`time_scale`](../reference/wander.md#time-time_scale),
  [`unscaled_dt`](../reference/wander.md#time-unscaled_dt), [`unscaled_time`](../reference/wander.md#time-unscaled_time),
  [`teleport`](../reference/wander.md#time-teleport)
- Design: [docs/ARCHITECTURE.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/ARCHITECTURE.md)
  (Game pause, process modes and time scale; Render interpolation), [docs/WANDER.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/WANDER.md)
