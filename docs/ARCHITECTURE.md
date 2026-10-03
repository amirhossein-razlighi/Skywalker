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
PCG32. Wander has no unbounded loops, and each handler has an evaluation budget. Stop
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
- Lighting: linear-space PBR (GGX), one sun with a 3×3-PCF shadow map, up to 16
  point/spot/directional lights, hemisphere ambient, fog and ACES tonemapping, with 4× MSAA
  into an sRGB target.
- Editor overlays: selection outline (inverted hull) and gizmos.

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
| A Wander function | The arity table in `Compiler.cpp`, the implementation in `Runtime.cpp::call`, and a line in `referenceText()`. |
| A render backend | Implement `Renderer` (render / readback / present / uploadMesh / reloadShaders), then add it to `createRenderer`. |
| An LLM provider | Most need nothing: they already speak the OpenAI-compatible protocol. Otherwise implement `LLMSession` in `Providers.swift`. |
