# Development

## Build

Requires macOS 15+, Xcode 16+ (Swift 6), CMake ≥ 3.29 and Ninja. The offline Metal
toolchain is *not* required: shaders compile at runtime.

| Preset | Purpose |
|---|---|
| `debug` | Day-to-day work: editor, CLI, tests |
| `release` | Optimized build |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer (engine, CLI and tests; no editor) |
| `tsan` | ThreadSanitizer (engine, CLI and tests) |
| `headless` | Engine, CLI and tests only. Use this on Linux or in CI (falls back to the CPU renderer). |

```bash
cmake --preset debug
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

Editor-only (macOS):

```bash
open build/release/bin/Skywalker.app --args --project "$PWD/examples/hello_sky"
```

Use the `release` preset for day-to-day editing: debug builds are several times slower on the CPU.
File › Open Project… (⌘O) switches projects; the editor reopens the last one.

> **Folder-access prompts:** debug builds are ad-hoc signed, and macOS asks again for
> access to `~/Documents` (or Desktop, Downloads) after each rebuild. To avoid it, sign
> with a stable identity: configure with `-DSKY_CODESIGN_IDENTITY="Apple Development"` and
> every build is signed with it; or keep projects outside protected folders.

## Tests

`tests/` uses [doctest](https://github.com/doctest/doctest). The suite covers:

| Area | What is tested |
|---|---|
| Core | JSON (round trips, errors, depth limits, merge patch), strings, math |
| ECS and reflection | Stale handles, swap-and-pop, validation and hints, schemas |
| Scene and history | Hierarchy, save/load identity, undo/redo, rollback, exact ordering (regressions) |
| Wander | Compiler diagnostics and fuzz-ish garbage input; runtime determinism, events, timers, budgets, spawn/destroy |
| Rendering front end | Primitive winding, OBJ parsing, PNG, visibility, picking, cameras, gizmo math |
| Tools and MCP | Every tool's schema, atomic batches (edit and play), attribution, MCP protocol, the socket round trip with the main-thread hop |

```bash
ctest --preset debug --output-on-failure
```

GPU output is never checked in `ctest` (CI has no GPU). Shader math that matters is mirrored in
C++ and unit-tested instead (for example `render/ShadowAtlas.h` for local light shadows). Opt-in
GPU checks under `tools/render_checks/` render reference scenes from `examples/render_tests/` on
macOS and assert image statistics, not pixel goldens:

```bash
python3 tools/render_checks/local_shadows.py   # point/spot light shadows: no light through walls
```

## Memory safety and profiling

| Tool | How |
|---|---|
| AddressSanitizer + UBSan | `cmake --preset asan && cmake --build --preset asan && ./build/asan/tests/skywalker_tests` |
| ThreadSanitizer | `cmake --preset tsan && cmake --build --preset tsan && ./build/tsan/tests/skywalker_tests` |
| Leak check | `leaks --atExit -- ./build/debug/bin/skywalker run examples/hello_sky/scenes/main.sky.json --ticks 600 -o /tmp/x.png` (expected: `0 leaks`) |
| CPU profiling | Instruments → Time Profiler on `Skywalker.app`, or `xcrun xctrace record --template 'Time Profiler' --launch -- ./build/release/bin/skywalker run …` |
| GPU profiling | Instruments → Metal System Trace; or Xcode → Debug → Capture GPU Workload (attach to the editor) |
| Frame stats | The viewport stats overlay (fps, CPU ms per frame, draws) and `sky_frame_stats` in the C API |

At v0.0.1: the test suite runs clean under ASan/UBSan, and `leaks` reports **0 leaks** for
both a 600-tick simulation with a Metal render and the full test suite.

## Conventions

- C++20, `sky::` namespace, `.clang-format` (Google-based, 120 columns).
- Return `Result<T>` / `Status`, not exceptions, across module boundaries. Error codes are
  stable `snake_case` strings with human-readable messages and hints.
- No raw owning pointers. Prefer value types and `unique_ptr`. In ObjC++, use ARC and
  `@autoreleasepool` at entry points.
- Anything an agent might do must be a tool. Tool descriptions are written for models: when
  to use the tool and what it returns.
- Swift: Swift 6 language mode, `@MainActor` stores, `@Observable`, and `Sendable` values
  across awaits.
