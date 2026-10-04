# Architecture

Skywalker is organized in strict layers. Lower layers never depend on higher ones.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Editor (Swift / SwiftUI)        CLI `skywalker` (C++)       External agents   │
│  panels · crew · providers       render · run · mcp          Claude Code, …   │
├───────────────┬──────────────────────────────┬───────────────────────────────┤
│ C API         │  MCP session (JSON-RPC 2.0)  │  Unix socket server (attach)   │
│ sky_api.h     │  stdio / socket transports   │  thread per connection         │
├───────────────┴──────────────────────────────┴───────────────────────────────┤
│ Engine facade: tools · edit transactions · play/stop · jobs · events · gizmo  │
├──────────────┬───────────────┬───────────────┬──────────────┬─────────────────┤
│ Scene        │ History       │ Wander        │ Render front │ Agent tools     │
│ ids, hierarchy│ undo/redo,   │ compiler,     │ FrameBuilder,│ ToolRegistry,   │
│ JSON I/O     │ attribution   │ runtime       │ picking, PNG │ schema checks   │
├──────────────┴───────────────┴───────────────┴──────────────┴─────────────────┤
│ Assets: database (.meta GUIDs, tags, provenance) · materials · prefabs · glTF │
├───────────────────────────────────────────────┬───────────────────────────────┤
│ ECS registry · reflection · components        │ Renderer backends: Metal (HDR, │
│                                               │ bloom, ACES), CPU fallback    │
├───────────────────────────────────────────────┴───────────────────────────────┤
│ Core: Json · Result · Log · Strings · Random · Math                            │
└──────────────────────────────────────────────────────────────────────────────┘
```

| Directory | Contents |
|---|---|
| `engine/include/skywalker/**`, `engine/src/**` | Platform-independent engine (C++20) |
| `engine/platform/metal` | Metal backend (Objective-C++, ARC) and shaders |
| `engine/capi` | Stable C ABI (`sky_api.h`) |
| `editor/` | SwiftUI editor (Swift 6, strict concurrency) |
| `tools/cli` | Headless CLI and MCP stdio server |
| `tests/` | doctest unit and integration tests |
| `python/` | The Python agent layer (`skywalker-agents`, [PYTHON_AGENTS](PYTHON_AGENTS.md)): a client of the tool surface, never linked into the engine |

## Key decisions

**C++20 core, Swift editor, C ABI between them.** The engine is portable C++. The macOS
editor uses SwiftUI and AppKit, which are native, accessible and quick to build with. It
talks to the engine through a small C API, which every language can call: Swift, Python,
Rust, C#, JavaScript FFI. Swift's direct C++ interop was considered and rejected. It
couples the editor build to compiler-specific C++ ABI details, and it gives other languages
nothing.

**One command surface.** Every capability is a *tool*: a name, an LLM-oriented
description, a JSON Schema, and a handler. The registry validates arguments
(type/enum/required/unknown-key, with did-you-mean hints) before a handler runs. MCP,
in-editor agents and the editor UI all call the same tools. A human and an agent can
therefore never have different powers, and every UI action is automatically scriptable.

**Reflection drives everything.** A component is a plain struct plus a field table
(`SKY_FIELD(...)`). From that one table come:
- JSON I/O and validation with field-level errors;
- the JSON Schema agents see;
- the editor's property grid;
- Wander property access (`self.light.intensity`).

**Stable entity ids.** The ECS uses (index, generation) handles internally. Everything
public uses 64-bit ids that are never reused, so an agent can hold on to `#12` safely.

**Transactions instead of command classes.** `History` watches the Scene. The first time a
transaction touches an entity, History snapshots it; at commit it snapshots the entity
again. Undo and redo restore those snapshots plus the full entity order. This makes every
edit undoable for free, makes `batch` atomic (rollback on failure), and attributes each
change to an actor. Interactive drags and gizmo moves hold one transaction per gesture.

**Deterministic simulation.** Play runs fixed 1/60 s ticks. Entities are processed in scene
order. Events are delivered on the next tick in emission order. Randomness is a seeded
PCG32. Every Wander handler run has an instruction budget (loops and calls are charged), so
scripts always terminate; Wander compiles to register bytecode (or native code). Stop
restores the pre-play snapshot. Live edits during play are atomic too: they are rolled back
from a snapshot if they fail.

**Rendering: CPU front end, thin GPU back end.**
`Scene → FrameBuilder (CPU) → FrameData → Renderer`.
- Agent-relevant facts (visible entities, screen boxes, picking, gizmo hit tests) are
  computed on the CPU from FrameData. They behave the same on every backend and in tests.
- `Renderer` is a five-method interface. Metal is the first backend; a CPU fallback keeps
  headless runs working without a GPU.
- Shaders compile at runtime from embedded source. They can be hot-reloaded
  (`shader_set`), and compile errors come back as diagnostics.
- Lighting: linear-space PBR (GGX) on a G-buffer with 4× MSAA. Clustered forward lighting:
  the view is split into 16×9×24 clusters on the CPU (`render/LightClusters.h`, unit-tested),
  each listing at most 128 lights, and up to 1024 lights are kept per frame (`FrameData::kMaxLights`,
  directional first, then the point and spot lights nearest to what the camera looks at). The 16
  most important (`kMaxEffectLights`) also light water, particles, fluids and volumetric fog.
- Shadows: the sun has 4 cascades in a 4096² atlas (bounding-sphere fit, texel snapping,
  rotated-Poisson PCF). Point and spot lights share a separate local shadow atlas
  (`render/ShadowAtlas.h`: 4 quadrants of 1, 4, 16 and 64 slots, cube or dual-paraboloid
  point lights, a static cache), bounded per frame by `Environment.localShadowLights`
  (default 16, max 64 shadowed lights) and `localShadowUpdates` (default 24 face renders).
  Image-based sky light from a prefiltered cubemap, screen-space GI and reflections, height fog,
  and a choice of tonemappers (ACES, AgX, neutral, filmic). Details: [RENDERING.md](RENDERING.md).
- Editor overlays: selection outline (inverted hull) and gizmos.

## Game pause, process modes and time scale

The editor's pause (`sim_control pause`, the toolbar) stops everything. A *game* needs its own
pause: the world freezes while the pause menu keeps working. That is `pause_game()` in Wander
(`sim_control pause_game` for agents), and the `process` component decides what keeps running.

| `process.mode` | Runs while the game plays | Runs while the game is paused |
|---|---|---|
| `pausable` (default) | yes | no |
| `when_paused` | no | yes (pause menu logic) |
| `always` | yes | yes (UI canvases by default, music controllers) |
| `disabled` | no | no (frozen, still drawn) |
| `inherit` (the field default) | from the nearest ancestor that sets it | |

Other fields: `priority` (behaviors run in (priority, scene order): lower first; not inherited),
`clock` (`game` follows `time_scale`, `real` ignores it; inherited) and `interpolation`
(`on`/`off`, see below; inherited). A UI canvas without its own setting is `always` on the
`real` clock, so menus work under a pause and in slow motion; give a HUD `mode: pausable` to
freeze it with the game.

**How a tick runs.** `Engine::step` calls `Runtime::prepareTick()` first: pause and time-scale
requests made since the last tick (by scripts or tools) apply here, `on pause` / `on resume` go
out, and the `ProcessGate` (`scene/Process.h`) is refreshed. Every system then asks the gate
`runs(e)` and `scale(e)` (dt multiplier: the time scale, 1 on the real clock, 0 when stopped):

| System | While the game is paused | Time scale |
|---|---|---|
| Wander | stopped instances keep their coroutines, timers and state; they still get `on pause` / `on resume`; events with a handler (and the last contacts) wait and arrive when they run again; input and clicks are dropped | per-instance `dt`, timers, waits |
| Physics + navigation | the world holds, unless the `physics_world` entity runs (`always`) | step `dt * scale`, split into ≤ 1-tick substeps above 1 |
| Animation, sequences | animators and cutscenes hold their pose | per entity |
| Particles (CPU) | emitters hold | per emitter; GPU effects follow the game clock |
| Sprites, 2D cameras, dialogue | hold | per entity |
| UI | a canvas that does not run takes no input (default canvases run) | UI animations use real time |
| Audio | every bus except `ui` pauses (menu clicks still play) | not pitched |
| Native modules | not ticked | `dt * scale` |

`time` in Wander is *game time* (stops while paused, slows with the scale);
`unscaled_time()` / `unscaled_dt()` are real time. Determinism holds: requests apply at tick
boundaries, the gate is a pure function of the scene and the clock, and a pause where nothing
runs is invisible to the simulation (tests/test_process.cpp checks that pausing for N ticks
yields the same world as not pausing, N ticks later, and that the same pause replays exactly).

**Recipe: a pause menu.**

```text
-- on the PauseMenu ui_canvas (canvases run `always`); its child panel "PausePanel" starts hidden
on action "pause"           -- Escape / Start in the default input map
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
```

(Keep the canvas entity itself enabled: a disabled entity runs nothing, not even `on pause`.)

Agents check it with `process_info` (what runs, why, and a warning when a paused game has
nothing that could resume it), `sim_control {action: "pause_game"}` + `step`, and
`ui_interact` / `sim_input` clicks.

**Recipe: bullet time.** `time_scale(0.25)` slows everything on the game clock; the HUD and
menus (real clock) stay at full speed; `time_scale(1)` restores it.

## Render interpolation

The simulation ticks at a fixed 60 Hz. A 120 Hz (ProMotion) or 144 Hz display would show each
tick for two or an uneven number of frames, which reads as stutter. Real-time frames therefore
show the world between the last two ticks (`engine/Interpolation.h`):

- `alpha = accumulator / fixed dt` — how far real time is into the next tick
  (`Engine::interpolationAlpha()`).
- `TransformHistory` keeps every entity's local transform from before the last tick (captured
  in `Engine::step`, never serialized).
- `ScopedInterpolation` writes `lerp/slerp(previous, current, alpha)` into the scene for the
  time it takes to build one frame and restores the tick state afterwards. Everything that
  reads world matrices is smoothed that way — meshes, cameras, lights, sprites, text, world UI,
  particle emitters, hair, bone attachments. Skinned meshes blend joint matrices
  (`AnimationSystem::setDisplayAlpha`), CPU particles move along their velocity, and the effects
  clock (water, sky, GPU particles) uses the displayed time.
- Opt out per subtree with `process.interpolation: off` (pixel-art snapping), and per jump
  with `teleport(e, position)` in Wander or the `sim_teleport` tool. Moves longer than 25 m
  in one tick are never smeared.
- The picture lags the simulation by up to one tick, as in every fixed-step engine.

Simulation, tools and captures never see the in-between values: `viewport_capture` and
`capture()` show the exact tick state unless asked for an `alpha`. The editor viewport and the
standalone player feed real frame time (`Engine::update`) and render with
`renderToSurface`, which uses the current alpha. The movie renderer uses the same history
and scope for its sub-frames (`Movie.cpp`).

**`on frame` handlers** run once per displayed frame, after interpolation, for cosmetic
touches: camera shake, bobbing, UI tweens. They may set `position`, `rotation`, `scale`,
`color` and fields of `transform`, `mesh`, `light`, `camera`, `sprite`, `text`, `ui`,
`light2d`, call pure functions and read-only queries; every write is undone after the frame.
Writing vars, waiting, timers, `go to`, spawning, emitting or anything random is a compile
error (`frame_not_cosmetic`), and a runtime check covers functions they call. So frames,
however many, never change the simulation.

**Verifying it (agents).** `sim_trace` with `display_hz: 120` simulates a display: per frame
it records alpha and the shown vs tick values, and reports `smoothness.stepJitter` (0 = even
motion; about 2 = every other frame repeats a tick) and frame `pacing` (`jitterMs` against
`jitterMsWithoutInterpolation`). `perf_stats.frameFlow` and `process_info.interpolation` show
the same for the live viewport; `viewport_capture` takes `alpha` and `frame_handlers`;
`skywalker-player --capture-frame out.png --display-hz 120` renders through the real player
pipeline at 120 Hz.

**For renderer features (velocity buffer, per-object motion blur).** The previous frame's
model matrix per entity is in `Engine::displayHistory()` once a feature turns tracking on
(`setTrackDisplayHistory(true)`); it is filled after each real-time frame from the draws.
Because transforms are interpolated before `buildFrame`, `DrawItem::model` already is the
displayed matrix, so `prevModel = displayHistory().previous(entity)` (or `model` when absent)
gives smooth per-frame motion vectors.

## Threading model

The engine is single-threaded by design (the "main thread"). The MCP socket server
accepts connections on its own threads, but every tool call is `post()`ed as a job and runs
in `pump()` on the main thread. Jobs are not pumped while an interactive drag transaction is
open, so agent edits are never attributed to the user's gesture. Shutting down, or stopping
the server, first refuses new jobs and fails queued ones, then joins the threads. A
self-pipe wakes `poll()` because `shutdown()` doesn't interrupt `accept()` on macOS. Requests
that time out are marked abandoned, so they can never apply changes later.

## Memory and lifetime

- RAII everywhere: `std::unique_ptr` ownership, value types, `UniqueFd` for descriptors.
  No raw `new`/`delete` except the C API's opaque handle.
- Metal objects are ARC-managed members. Entry points wrap their work in
  `@autoreleasepool`, so headless runs don't accumulate objects.
- `shared_ptr<const wander::Program>` is used only where compiled programs are genuinely
  shared: the cache, plus the runtime keeping a program alive while it executes.
- Verified with AddressSanitizer + UBSan (`SKY_SANITIZE`) and with macOS `leaks` on full
  CLI sessions and the test suite. See [DEVELOPMENT](DEVELOPMENT.md).

## Extending

| To add… | Do this |
|---|---|
| A component | Struct + `type()` table in `Components.{h,cpp}`, then one `makeReflectedKind<T>()` line in `Scene::registerKinds`. JSON, schema, editor UI and Wander access follow automatically. |
| A tool | `reg.add({...})` in `EngineTools.cpp`, using an `edit(actor, label, …)` transaction for mutations. It then appears in MCP, the editor and the docs (`skywalker tools --markdown`). |
| A Wander function | `reg.add(BuiltinDef{...})` in a `registerXxxBuiltins(reg)` called from `registerEngineBuiltins` (`EngineBuiltins.cpp`): name, typed params, doc, category, implementation. The compiler, `wander_reference` and the docs pick it up (see docs/WANDER.md). |
| A render backend | Implement `Renderer` (render / readback / present / uploadMesh / reloadShaders), then add it to `createRenderer`. |
| An LLM provider | Most need nothing: they already speak the OpenAI-compatible protocol. Otherwise implement `LLMSession` in `Providers.swift`. |
