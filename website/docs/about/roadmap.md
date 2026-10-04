# Roadmap and changelog

## v0.1 "Studio" (pre-release)

The ground-up expansion of the engine into a complete, agent-first toolset.

=== "Agents"

    - 186 tools over MCP, the editor socket and the C API, with schema validation and did-you-mean errors.
    - Thirteen skills, eight studio-role subagents and five workflow commands for Claude Code, Codex, Gemini CLI and
      Cursor, installed with `skywalker setup`.
    - MCP resources (docs, skills, live catalogue, live studio and scene state) and prompts (roles and workflows).
    - A multi-agent studio: roster with roles and permissions, task board, feedback with director decisions and
      measured effects, playtest bots that play, user-defined loops, a headless runner.
    - A Python agent layer (`python/`, `sky-agents`): a typed engine client, agents, shared memory, workflows,
      approvals, evals and tracing, an event log and hosted tools, and `skywalker serve` for a shared headless engine.

=== "Wander 2"

    - A typed compiler and register bytecode VM; functions, lists and maps, coroutines, state machines, optional
      types, modules and in-language tests.
    - A builtin registry shared by every subsystem; a lossless node-graph view.
    - Ahead-of-time compilation to native code and native C++ modules.
    - Cosmetic `on frame` handlers; game pause, process modes and time scale.

=== "Rendering"

    - G-buffer with clustered lighting (1,000+ lights), screen-space GI and reflections, TAA, MetalFX upscaling, a
      velocity buffer and per-object motion blur.
    - Physical atmosphere with volumetric clouds and cloud shadows, light shafts, height fog.
    - Point and spot light shadows from a cached shadow atlas with per-frame budgets.
    - Eroded terrain with splat layers, instanced foliage with LODs, octahedral impostors, GPU-driven culling and a
      triangle budget.
    - Camera post: auto exposure, depth of field, motion blur, looks and `.cube` LUTs.
    - Debug, clay and sketch views; a per-pass GPU profiler; render layers.
    - GPU particles, strand hair and fur, an FFT ocean, GPU fluid fire and smoke.
    - Editor quality tiers, GPU-fault detection and a cross-process GPU job lock.

=== "Engine"

    - Jolt physics and Recast navigation; skeletal animation with state machines, IK and a sequencer.
    - Spatial audio with a mixer and procedural sound and music; input actions with gamepads.
    - 2D sprites, tilemaps and lights; UI layout and SDF text; dialogue.
    - Render interpolation for high-refresh displays.
    - Linked prefabs with per-instance overrides; entity links that survive renames; unique names.

=== "Production"

    - The Blender bridge, with adapters for other design apps.
    - The movie renderer (H.264, HEVC, ProRes, PNG; real motion blur).
    - The standalone player and `skywalker build` into signed macOS apps.

## Next

| Area | Planned work |
|---|---|
| Rendering | Impostor wind sway and hierarchical LODs for buildings; virtual shadow maps; hardware ray-traced reflections and GI on newer Apple GPUs; skin subsurface profiles; decals; virtualized geometry for scanned assets |
| Platforms | A Vulkan backend (Windows, Linux), then D3D12; an iOS player; an editor shell for other platforms |
| World | Cell-based world streaming for very large maps; road and spline tools; a procedural city kit |
| Wander | A debugger (breakpoints, watches, stepping) in the editor; hot reload of native modules |
| Agents | Built-in generator adapters (image, 3D, audio); screenshot diffing for visual review; spending limits per loop |
| Editor | Multi-select editing, nested prefabs and prefab variants, docking layouts, a timeline editor for sequences, a profiler panel |
| Shipping | Developer ID signing and notarization automation, App Store packaging, save games, localization |

The roadmap lives in [docs/ROADMAP.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/ROADMAP.md).

## Changelog

| Version | Highlights |
|---|---|
| 0.1 (current, pre-release) | Everything above, plus game pause, process modes, time scale and render interpolation; a zero-warning build in every preset; the Business Source License 1.1 |
