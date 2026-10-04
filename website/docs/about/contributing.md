# Contributing

Contributions are welcome: engine features, tools, Wander builtins, examples, skills and documentation. This page
summarises how the codebase is organised and what a change needs to land. The repository's
[AGENTS.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/AGENTS.md) is the authoritative guide (it is
written for coding agents and humans alike).

## Build and test

```bash
cmake --preset headless && cmake --build --preset headless
ctest --preset headless --output-on-failure
cmake --preset asan && cmake --build --preset asan && ./build/asan/tests/skywalker_tests
```

Run the whole suite, not only a filter, before you finish. The `headless`, `release`, `debug` and `asan` presets build
with warnings as errors, and the tree is warning-free in every preset. Build the `debug` preset too if you touched the
editor.

## Layout

| Path | What lives there |
|---|---|
| `engine/include/skywalker/**`, `engine/src/**` | The platform-independent engine: core, ECS, scene, Wander, render front end, assets, agent tools and MCP, studio, audio, input, effects, world, DCC |
| `engine/platform/metal` | The Metal backend and shaders |
| `engine/capi` | The stable C API used by the editor |
| `editor/` | The SwiftUI editor (Swift 6, strict concurrency) |
| `tools/cli` | The `skywalker` executable |
| `tests/` | The doctest suite, one file per area |
| `docs/` | Design documents, embedded in the binary as MCP resources |
| `integrations/` | Agent integrations; edit `integrations/skills-src/`, never the generated copies |
| `website/` | This site |

## Conventions

- **C++20**, namespace `sky`, Google-based `.clang-format` at 120 columns. Return `Result<T>` / `Status` across module
  boundaries, not exceptions; error codes are stable `snake_case` with a message and a hint (use `str::closest` for
  did-you-mean). No raw owning pointers.
- **Everything an agent can do is a tool.** Add a tool with `reg.add({name, title, description, category, schema,
  mutating, handler})` in a `*Tools.cpp` file and one registration line in `EngineTools.cpp`. Descriptions are written
  for models: what it does, when to use it, key parameters, an example. Mutations go through
  `engine.edit(actor, label, fn)` so they are undoable and attributed.
- **Components are plain structs with reflection** (`SKY_FIELD`). New components go in a new header included from
  `ecs/Components.h` and register with one line in `scene/Scene.cpp`; add editor order and icon in
  `editor/Sources/Views/DetailsPanel.swift`.
- **Hub files are append-only and minimal** (`Components.h`, `Scene.cpp`, `EngineTools.cpp`, the CMake lists,
  `Engine.h/.cpp`): many people edit them in parallel.
- **Simulation is deterministic.** Fixed 1/60 s ticks, seeded randomness, snapshot and restore on stop; anything
  simulated must reset on stop and replay identically.
- **Dependencies** come from CMake `FetchContent` at pinned tags from official repositories, with permissive licenses
  recorded in `docs/LICENSING.md`.

## When you change what agents see

| You changed | Also do |
|---|---|
| A tool, argument or enum value | Update the skill text in `integrations/skills-src`, run `python3 integrations/generate.py` and commit the generated files. `integrations/check_skills.py` lints every tool name and example call. |
| A tool, component, Wander builtin or CLI flag | Regenerate this site's reference: `python3 website/scripts/dump_data.py --cli build/release/bin/skywalker` then `python3 website/scripts/gen_reference.py`. The test suite fails when the committed reference is stale. |
| A design document | Add it under `docs/`; it is embedded in the binary and served as `skywalker://docs/<NAME>`. |

## Documentation

The site lives in `website/` (MkDocs with Material). Manual pages are hand-written; the reference and the examples
gallery are generated. Every Wander sample must compile with `skywalker check` and every tool call must match the live
schemas: `website/scripts/check_snippets.py` enforces both. See the
[website README](https://github.com/amirhossein-razlighi/Skywalker/blob/main/website/README.md).

## Contributor terms

Skywalker is licensed under the Business Source License 1.1 with a commercial license for larger organizations (see
[License](license.md)). By submitting a contribution you agree that:

1. You wrote it, or otherwise have the right to submit it under these terms.
2. You keep the copyright to your contribution.
3. You grant the Licensor a perpetual, worldwide, non-exclusive, royalty-free, irrevocable license to use, modify,
   sublicense and distribute your contribution as part of Skywalker, under the Business Source License, the commercial
   license and the Change License (Apache 2.0).
4. Your contribution contains no third-party code or assets under licenses incompatible with those terms.

Add a `Signed-off-by:` line to your commits (`git commit -s`) to confirm these terms.
