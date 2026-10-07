# Development

## Build

Requires macOS 15+, Xcode 16+ (Swift 6), CMake ≥ 3.29 and Ninja. The offline Metal
toolchain is optional: with it (`xcodebuild -downloadComponent MetalToolchain`, then
reconfigure) the shader library is precompiled to a `.metallib` and startup skips the MSL
compile; without it shaders compile at runtime (see RENDERING.md, "Shader library and
pipeline cache").

| Preset | Purpose |
|---|---|
| `debug` | Day-to-day work: editor, CLI, tests |
| `release` | Optimized build |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer (engine, CLI and tests; no editor) |
| `tsan` | ThreadSanitizer (engine, CLI and tests) |
| `headless` | Engine, CLI and tests only. Use this on Linux or in CI (falls back to the CPU renderer). See [Linux](#linux). |

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

## Linux

The `headless` preset builds the engine, the CLI and the test suite on Linux with GCC 13 or Clang 18 and runs every
test on the CPU renderer (not run in CI). Metal,
the editor and AVFoundation video encoding are macOS-only: the tests that need them report a message and pass.

```bash
sudo apt-get install -y ninja-build zlib1g-dev libcurl4-openssl-dev
cmake --preset headless && cmake --build --preset headless
ctest --preset headless --output-on-failure
cmake --preset asan && cmake --build --preset asan && ./build/asan/tests/skywalker_tests   # ASan + UBSan work on Linux too
```

Portability rules learned from the Linux port:

- **File times.** `std::filesystem::file_time_type` has an implementation-defined epoch (1970 in libc++, 2174 in
  libstdc++, so raw counts are negative on Linux). For hot reload and cache keys use `sky::fileModifiedNs()`
  (`core/FileTime.h`): nanoseconds since the Unix epoch, `-1` when the file is missing. Never test the sign of a raw
  `time_since_epoch().count()`.
- **Warnings.** GCC's `-Wshadow` also flags a lambda parameter that shadows an enclosing `if` declaration, and a local
  type that shadows a member type; Clang does not. Both presets build with warnings as errors, so build with GCC
  before pushing engine changes that CI runs on Linux.
- **Case-sensitive file systems.** macOS volumes are usually case-insensitive, Linux ones are not: refer to project
  files with their exact case.
- **No `/proc`, no `timeout` on macOS.** Code and scripts must run on both; prefer portable C++ (`std::filesystem`,
  `std::chrono`) over platform files.

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
| GPU profiling | `perf_stats {"frames": 30, "passes": true}` (per-pass GPU times, CPU scopes); Instruments → Metal System Trace; or Xcode → Debug → Capture GPU Workload (attach to the editor) |
| Frame stats | The viewport stats overlay (fps, CPU/GPU ms, click it for the pass list) and `sky_frame_stats` in the C API |
| CPU scopes | `SKY_PROFILE_SCOPE("area.name")` (`skywalker/core/Profiler.h`) times a block; it shows up in `perf_stats {passes: true}` → `profile.cpu` |

Renderer environment switches (benchmarking and debugging):

| Variable | Effect |
|---|---|
| `SKY_GPU_PROFILER=0` | no per-pass GPU timestamps |
| `SKY_SHADER_SOURCE=1` | compile the embedded MSL source even when a `.metallib` is embedded |
| `SKY_SHADER_METALLIB=<file>` | load the shader library from this `.metallib` (falls back to source when it lacks a function) |
| `SKY_SHADER_CACHE=0` / `SKY_SHADER_CACHE_DIR=<dir>` | disable / move the pipeline binary archive |
| `SKY_SHADER_NONCE=<text>` | change the library source by a comment: a cold shader compile, for startup benchmarks |

`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` turn the pipeline archive off automatically (Metal
crashes loading archives through the validation device).

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

## Showcase builds and the quality gate

Showcase games are built by scripts that drive the engine through its tools (`media/demo/games/*.py`, `media/demo/kit.py`).
Three pieces keep them reproducible and at the quality bar:

- **Pinned assets** (`media/demo/assetkit.py`, see [ASSETS](ASSETS.md#pinned-asset-manifests-media-demo-assetkit-py)):
  every third-party file in an `assets.json` manifest with URL, sha256, license, author and source page; `fetch` brings
  them into the gitignored `downloads/`, `check` refuses licenses outside the allowlist, `credits` writes CREDITS.md.
- **Shared kits**: characters, props and materials used by several showcases live in one kit folder next to them,
  mounted by each project's game.json (`"mounts": {"kit": "../_kit"}`), so prefabs are referenced as
  `kit/characters/....prefab.json` ([ASSETS](ASSETS.md#shared-kits-mounts-in-gamejson)).
- **The gate**, before filming or reporting a showcase done:

```bash
python3 media/demo/showcase_gate.py examples/my_game --preview          # 1280x720, 8 samples: while iterating
python3 media/demo/showcase_gate.py examples/my_game                    # 1920x1080, 16 samples: the final sheet
python3 media/demo/showcase_gate.py examples/my_game --sequences sequences/intro.sequence.json,sequences/chase.sequence.json
```

It spreads 12 shots over the project's sequences (every `*.sequence.json` played by an entity of the scene, or the ones
given), renders each through the sequence's live camera, audits it with `scene_audit` (strict: visible builtin primitives or
default materials above 0.1% of the image, primitive characters and missing files fail), benchmarks the first hero views with
`perf_stats` (GPU ms, `gpuFaults` must be 0), and writes `shots/shot_NN.jpg` (<= 400 KB), `shots/contact_sheet.jpg`
(<= 1.5 MB) and `shots/gate_report.json`. It prints PASS or FAIL and exits non-zero on FAIL. `--stylized` accepts flat-colored
materials for a deliberate stylized look; `--no-strict` uses the 2% limit. `--movie` renders each shot through
`movie_render` instead of a scrubbed still: the sequence pre-rolls to the moment, so particles (smoke, spray) have their history,
and the shutter (`--shutter`, default 0.5) adds motion blur; use it for driving and action sequences. Renders take the GPU lock,
so run one gate at a time.
The gate is a floor, not the bar: look at the sheet as an art director would (composition, light, scale, stretching).
