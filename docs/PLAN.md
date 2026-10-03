# Skywalker v0.1 "Studio": the plan

Goal: match the features that matter most in Unity and Unreal, built agent-first. Every
capability is a tool, every tool works the same from the editor, the in-editor crew, the
headless CLI, Claude Code, Codex, Gemini CLI, and Cursor. The showcase must hold up next to
Assassin's Creed, Cyberpunk 2077, Black Myth: Wukong, Hollow Knight, Stardew-style farm sims,
Hearts of Iron IV, and Suzerain.

## Gap analysis (v0.0.2 → v0.1)

| Area | Unity / Unreal baseline | v0.0.2 | v0.1 target |
|---|---|---|---|
| **3D rendering** | TAA/TSR, SSR, GI (Lumen/APV), clustered lights, instancing, LOD, terrain, foliage, decals, volumetric clouds, full post stack | PBR, CSM, IBL, SSAO, bloom, water SSR, fluids | TAA + motion vectors, GPU instancing + LOD, terrain with splat layers, instanced foliage with wind, general SSR, SSGI, clustered (forward+) lights, decals, volumetric clouds, auto-exposure, DOF, motion blur, grading LUT, grain, CA |
| **2D** | Sprites, atlases, flipbooks, tilemaps, 2D lights, pixel-perfect camera | none | Sprite + SpriteAnimator, atlases, tilemaps, 2D lights with normal maps, parallax layers, pixel-perfect ortho |
| **Text & UI** | TextMeshPro/UMG: SDF text, layout, widgets, data binding | none | SDF fonts (stb_truetype), world text, a UI canvas with anchors, stacks, widgets, 9-slice, input, Wander bindings, and a dialogue system |
| **Animation** | Skinned meshes, clips, blend trees, state machines, IK, timelines | none | glTF skins and clips, GPU skinning, crossfades, 1D blend spaces, animator state machine, look-at IK, sequencer tracks |
| **Physics** | PhysX/Chaos: rigid bodies, colliders, character controller, joints, triggers, queries | mesh raycast only | Jolt: bodies, colliders (box, sphere, capsule, mesh, height field), character controller, triggers, joints, ray/shape casts |
| **Audio** | Spatial sources, mixer, music | none | miniaudio: spatial sources, buses, music, one-shots from Wander |
| **Input** | Action maps, gamepad | keys | Action maps, gamepad (GameController), mouse look |
| **AI/navigation** | NavMesh, agents, behavior trees | none | Recast/Detour navmesh, nav agents, Wander state machines |
| **Scripting** | C#/C++, Blueprints | Wander (tree-walking interpreter) | Wander 2: functions, state machines, coroutines (`wait`), lists, maps, modules; a bytecode VM; C++ AOT; native C++ modules; a node-graph (Blueprint-style) view; and a natural-language → spec → code pipeline |
| **Agents** | (none) | crew in the editor, MCP | Studio: roster of specialists, task board, playtest bots, feedback, director decisions, defined loops, headless runner, editor Studio panel |
| **Tool integrations** | (none) | MCP | Skills and plugins for Claude Code, Codex, Gemini CLI, and Cursor; MCP resources and prompts; `skywalker setup <client>` |
| **DCC** | Live links (Blender, Maya, Houdini) | none | Blender bridge: run scripts, round-trip assets, FBX→glTF conversion, live session add-on; adapters for Maya, 3ds Max, and Houdini where installed |
| **Shipping** | Standalone builds | none | `skywalker build`: a standalone macOS player app (stretch goal) |

## Workstreams

Each workstream lands as its own branch, merged into `claude/skywalker-game-engine-7e797f`.

| # | Workstream | Main files |
|---|---|---|
| R | 3D rendering upgrade | `MetalRenderer.mm`, `Standard.metal`, `Renderer.h`, `FrameBuilder.cpp`, new terrain/foliage modules |
| T | 2D, text, UI, dialogue | new `render2d/`, `ui/` modules, `Sprite2D.metal`, `MetalRenderer2D.mm` |
| A | Animation | `anim/`, skins in `Gltf.cpp`, skinned pipeline |
| P | Physics (Jolt) + navigation (Recast/Detour) | `physics/`, `nav/` |
| S | Audio + input actions/gamepad | `audio/`, `input/` |
| W | Wander 2, VM, C++ AOT, native modules, graph view | `wander/`, `native/` |
| ST | Studio: multi-agent roster, board, feedback, loops, playtests, runner | `studio/`, `StudioTools.cpp`, editor `Studio/` |
| D | DCC bridge | `dcc/`, `integrations/blender/` |
| K | Skills and integrations | `integrations/claude-code`, `integrations/codex`, `integrations/gemini`, `integrations/cursor`, CLI `setup` |
| G | Showcase games | `examples/*`, `media/demo/*` |

## Conventions for every workstream

- **Hub files are append-only.** New components go in their own header (included from
  `Components.h`) and register in `Scene.cpp` with one line. Tools go in a new
  `*Tools.cpp`, registered with one line in `registerEngineTools`. New sources are
  appended to `engine/CMakeLists.txt` and tests to `tests/CMakeLists.txt`.
- **Dependencies** come from FetchContent with pinned tags from official repositories
  (doctest pattern). Licenses must be permissive (MIT, BSD, zlib, public domain).
- **Determinism.** Simulation runs at fixed 1/60 s steps and replays exactly.
- **Agent ergonomics.** Every capability has a tool with an LLM-oriented description, a
  JSON schema, structured results, and did-you-mean errors. Every edit is undoable and
  attributed.
- **Quality.** Each workstream adds doctest coverage, passes ASan, and documents itself in
  `docs/`.
- **Generated files.** `docs/TOOLS.md` and tool counts are regenerated only at merge.

## Showcase games: original IP only

The reference games are benchmarks for **quality and genre**, never sources to copy. Each
showcase game uses:
- its own title, world, characters, factions, story and UI style;
- no names, logos, characters, maps, UI layouts, music or signature designs from those
  games;
- only assets that are original, procedural, or permissively licensed with credits.

The strategy sample uses a fictional continent, not real-world maps of a specific era.

## High-end rendering track (engine capability; samples opt in)

Not every sample has to be this heavy, but the engine must be capable of shipping-AAA
looks when a game asks for them.

| Feature | Approach |
|---|---|
| Temporal core | Sub-pixel jitter, TAA with reprojection and variance clipping, and N-sample accumulation for stills and cinematics |
| Global illumination | Screen-space GI (bounce and emissive light) with temporal and spatial denoising, falling back to the sky probe |
| Reflections | Screen-space reflections on every glossy surface (GGX-importance-sampled), falling back to probes |
| Hair and fur | Strand-based grooms (guide curves, interpolated children, clumping, noise), GPU-expanded strand ribbons, Marschner-style R/TT/TRT shading, deep-opacity self-shadowing, and wind/physics sway; a card-based LOD |
| GPU particles | Compute-simulated particles in the millions: curl-noise and vector fields, depth-buffer collisions, ribbons/trails, mesh particles, flipbook sheets, lit and shadowed, sorted, and emitting light |
| Skin | Separable screen-space subsurface scattering; eye and teeth shading |
| Terrain and coastlines | Large erosion-sculpted terrains, splat materials with height blending, parallax occlusion, wet sand where waves reach, foam lines, shoreline wave breaking, detail meshes (shells, pebbles, seaweed) |
| Vegetation | GPU-instanced foliage and grass in the 100k+ range, wind animation, translucency, LOD and impostors |
| Atmosphere | Volumetric clouds, aerial perspective, volumetric fog with local lights |
| Cinematic post | Auto exposure, bokeh depth of field, motion blur, LUT grading, film grain, lens effects |

## Efficiency (Apple silicon first, portable design)

| Area | Practice |
|---|---|
| Tile-based GPU | Memoryless MSAA and G-buffer attachments (tile memory only), load/store actions that avoid bandwidth, half-resolution stochastic passes |
| Upscaling | MetalFX temporal upscaling (render scale under 1), with the engine TAA as fallback |
| GPU-driven work | Compute culling plus indirect draws for instanced foliage, grass and hair; GPU particle simulation; mesh shaders where they pay off |
| CPU→GPU traffic | Triple-buffered ring buffers for per-frame uniforms and instance data, with no per-frame allocations; static geometry and instance buffers uploaded once and versioned |
| Shaders and pipelines | Function-constant specialization instead of runtime branching; pipeline/binary-archive caching; runtime compilation kept for agent hot-reload |
| CPU | A job system (P-core and E-core aware) for frame building, culling, particles, ocean, physics, and animation; data-oriented component storage |
| Profiling | GPU timestamps per pass, CPU scopes, and memory stats, exposed through `perf_stats` so agents can measure and optimize their own scenes |
