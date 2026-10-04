# AGENTS.md: working ON the Skywalker engine

This file is for coding agents (Claude Code, Codex, Gemini CLI, Cursor, ...) that **change the engine's source**. If you want to *make games with* Skywalker
through its tools, you want the `skywalker-*` skills instead (`integrations/`, `docs/INTEGRATIONS.md`, `skywalker setup`).

Skywalker is an AI-agent-first game engine: C++20 core, Metal renderer (CPU fallback), SwiftUI editor, a Wander behavior language, an MCP tool surface.
Read `docs/ARCHITECTURE.md` first; `docs/PLAN.md` has the intent, `docs/DEVELOPMENT.md` the full build notes.

## Build and test

```bash
cmake --preset headless && cmake --build --preset headless     # engine + CLI + tests, no Swift editor (Linux/CI friendly)
./build/headless/tests/skywalker_tests                          # doctest; filter with -tc="name*"
ctest --preset headless --output-on-failure
cmake --preset asan && cmake --build --preset asan && ./build/asan/tests/skywalker_tests   # run before finishing
cmake --preset debug && cmake --build --preset debug            # only if you touched editor/ (macOS, Xcode 16+)
```

The `headless` and `release` presets build with warnings as errors. Run the **whole** suite, not only your filter, before you stop.

Useful: `./build/headless/bin/skywalker tools --markdown` (tool catalogue), `skywalker call TOOL '{json}' --project DIR` (one tool call),
`skywalker render SCENE -o out.png`, `skywalker mcp --project DIR` (MCP over stdio), `skywalker check FILE.wander`.

## Layout

| Path | What lives there |
|---|---|
| `engine/include/skywalker/**`, `engine/src/**` | Platform-independent engine: core, ecs, scene, wander, render front end, assets, agent (tools, MCP, socket), studio, audio, input, fx, world, dcc |
| `engine/platform/metal` | Metal backend (Objective-C++) and shaders |
| `engine/capi` | Stable C ABI used by the editor |
| `editor/` | SwiftUI editor (Swift 6, strict concurrency) |
| `tools/cli` | The `skywalker` executable: mcp, render, run, check, call, tools, studio, setup |
| `tests/` | doctest suite (one file per area) |
| `docs/` | Documentation; `docs/TOOLS.md` is generated from the registry |
| `integrations/` | Agent integrations. **Edit `integrations/skills-src/`, never the generated copies** |
| `python/` | The Python agent layer (`cd python && uv sync && uv run pytest`); after changing engine tools run `uv run sky-agents gen-tools` |

## Conventions

- C++20, namespace `sky`, `.clang-format` (Google-based, 120 columns). Return `Result<T>` / `Status`, not exceptions, across module boundaries; error codes are stable
  `snake_case` with a message and a hint (use `str::closest` for did-you-mean). No raw owning pointers (RAII, `unique_ptr`, value types).
- **Everything an agent can do is a tool.** A tool is `reg.add({name, title, description, category, schema, mutating, handler})` in a file such as `engine/src/agent/FxTools.cpp`
  (compact example) with one `tools::addXxxTools(engine, reg)` line in `registerEngineTools` (`EngineTools.cpp`). Descriptions are written for LLMs: what it does, when to use it, key
  parameters, an example. Mutations go through `engine.edit(actor, label, fn)` so they are undoable and attributed.
- **Components are plain structs with reflection** (`SKY_FIELD`): JSON I/O, schemas, the editor property grid and Wander access come from the field table. Put new components in a new header
  and `#include` it at the end of `ecs/Components.h`; register with one `kinds_.push_back(makeReflectedKind<X>())` line in `scene/Scene.cpp`; add editor UI order/icon in `editor/Sources/Views/DetailsPanel.swift`.
- **Hub files are append-only and minimal** (`Components.h`, `Scene.cpp`, `EngineTools.cpp`, `engine/CMakeLists.txt`, `tests/CMakeLists.txt`, `Engine.h/.cpp`): many people edit them in parallel, so keep
  changes small and grouped.
- **Simulation is deterministic**: fixed 1/60 s ticks in `Engine::step`, seeded randomness, play snapshot/restore on stop. Anything simulated must reset on stop and replay identically.
- Dependencies: CMake `FetchContent` from the official repo with a pinned tag (see `tests/CMakeLists.txt`); permissive licenses only; record them in `docs/LICENSING.md`.
- Wander builtins are added in `wander/Compiler.cpp` (known functions, arity) and `Runtime.cpp` (implementation) and listed in the reference text.
- Backward compatibility is not a goal before release: redesign when justified, then migrate `examples/`, tests and docs so the repo stays green.

## When you change what agents see

- **Add or rename a tool, argument or enum value** -> the skills must stay true. `python3 integrations/check_skills.py --tools <(skywalker tools --json)` lints every tool name and example call
  in `integrations/skills-src`; the test-suite runs it. Update the skill text, then `python3 integrations/generate.py` (also run by `--check` in the tests) and commit the generated files.
- **Add docs** under `docs/`: they are embedded in the binary and served as MCP resources `skywalker://docs/<NAME>`.
- Do not hand-edit `docs/TOOLS.md` unless you are the one regenerating it.

## Pitfalls specific to this repo

- The repository may live in iCloud-synced `~/Documents`. Files can become "dataless" placeholders and make builds or git fail with timeouts or truncated reads; rehydrate them by reading the files (for example
  `find . -path ./build -prune -o -type f -flags +dataless -print0 | xargs -0 -P 16 -n 8 cat > /dev/null`) and keep scratch files outside the repo.
- macOS has no `timeout`; use `perl -e 'alarm shift; exec @ARGV' 600 cmd...`.
- Never `git stash` bare in a shared checkout; never push; keep commits small with clear messages.
- The Metal renderer is macOS-only; the headless preset falls back to the CPU renderer, so tests must not depend on GPU output.
