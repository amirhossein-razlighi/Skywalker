# Roadmap

## v0.1 (pre-release, this branch)

The ground-up expansion described in [PLAN](PLAN.md):

- **Agents first:**
  - 160+ tools over MCP, the editor socket and the C API;
  - 13 skills, subagents and setup for Claude Code, Codex, Gemini CLI and Cursor;
  - a multi-agent studio with roles, board, feedback triage, playtest bots and user-defined loops.
- **Wander 2:**
  - register VM; functions, collections, coroutines, state machines, types, modules and tests;
  - builtin registry, graph view, ahead-of-time C++ and native modules.
- **Rendering:**
  - G-buffer, clustered lighting, screen-space GI and reflections, TAA, MetalFX upscaling;
  - atmosphere with volumetric clouds, terrain with erosion, instanced foliage with LODs and a triangle budget;
  - camera post: exposure, DOF, motion blur, LUTs;
  - debug, clay and sketch views;
  - GPU particles, strand hair, FFT ocean, GPU fluids;
  - editor quality tiers; GPU-fault detection and a cross-process GPU job lock.
- **Engine:**
  - Jolt physics, Recast navigation, skeletal animation and a sequencer, spatial audio, input actions;
  - 2D sprites, tilemaps and lights; UI layout and SDF text; dialogue.
- **Production:**
  - Blender bridge;
  - Movie Render Queue (HEVC/ProRes, motion blur);
  - standalone player and `skywalker build` into signed macOS apps.

## Next

| Area | Planned work |
|---|---|
| Rendering | Foliage impostors / HLOD; virtual shadow maps; hardware ray-traced reflections and GI on M3+; skin subsurface profiles; decals; virtualized geometry for scanned assets |
| Platforms | Vulkan backend (Windows/Linux), then D3D12; iOS player; an editor shell for non-Mac platforms |
| World | World partition / streaming for very large maps; road and spline tools; procedural city kit |
| Wander | Debugger (breakpoints, watch, step) in the editor; hot-reload of native modules |
| Agents | Built-in generator adapters (image, 3D, audio); screenshot diffing for visual review; spending limits per loop |
| Editor | Multi-select editing, prefab overrides, docking layouts, a timeline editor for sequences, profiler panel |
| Shipping | Developer ID signing and notarization automation, App Store packaging, save games, localization |
