# Roadmap

## v0.0.1 (this release)

- C++20 engine core:
  - reflected components and an ECS with stable ids;
  - transactional, attributed undo and redo;
  - deterministic simulation.
- Wander language and the ECPS model (intent + code): compiler diagnostics, safe runtime.
- Metal renderer:
  - PBR shading, sun shadows, MSAA;
  - selection outline and transform gizmos;
  - runtime-compiled, hot-reloadable shaders;
  - a CPU fallback.
- 35 agent tools; an MCP server (stdio plus attach-to-editor socket); C API; headless CLI.
- SwiftUI editor:
  - panels: outliner, details, assets, console, activity, agents, pipelines;
  - gizmos, stats, and a status bar.
- Crew agents: Anthropic and OpenAI-compatible providers, with autonomy levels, approvals,
  delegation, pipelines, and Weave.

## Next

| Area | Planned work |
|---|---|
| Rendering | GPU instancing and frustum culling; cascaded shadows; IBL environment maps; glTF/GLB import; sprite atlases and a proper 2D renderer; GPU picking ID buffer |
| Platforms | Vulkan backend (Windows/Linux), then D3D12; an editor shell for non-Mac platforms |
| Physics | Collision shapes, rigid bodies and triggers (`on touch "Name"` in Wander), character controller |
| Audio | Audio engine and playback of `audio`/`music` assets; Wander `play "sound"` |
| Wander | Coroutine-style `wait`, entity templates/prefabs, modules, debugger (breakpoints, watch), bytecode VM |
| Agents | Built-in generator adapters (image, 3D, audio, music); MCP resources (scene, docs) and prompts; per-agent budgets and cost tracking; screenshot diffing for visual review; agent memory per project |
| Editor | Multi-select editing, prefabs, play-in-editor game window, undo history panel, layouts, keyboard shortcut editor |
| Production | Packaging and export of standalone games; asset pipeline with import settings; plugin API |
