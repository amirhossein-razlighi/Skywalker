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

## v0.0.2 (in progress)

- **Asset system:**
  - GUID `.meta` sidecars, tags, descriptions and generator provenance;
  - search, usage queries, safe moves, rendered previews.
- **Formats:** glTF 2.0 / GLB import; material assets; prefabs, including Wander `spawn("prefab:…")`.
- **World tools:** `raycast`, `place_on_surface`, `scatter`, `viewport_multi`, `sim_trace`, `perf_stats`. 57 tools in total.
- **Rendering:** PBR maps, triplanar, clearcoat/subsurface, toon + outlines, IBL, 4 shadow cascades, SSAO, atmospheric sky with clouds and stars, height fog, AgX/neutral/filmic tonemapping; procedural PBR textures (`texture_generate`, 21 kinds) and material presets.
- **Import:** PLY (ascii/binary, vertex colors), STL, OBJ + MTL, glTF maps; `asset_download` for licensed web assets (zip packs, multi-file glTF), credits and provenance.
- **Brand:** app icon, glyph, favicon, lockups, `docs/BRAND.md`.
- **Rendering:**
  - HDR pipeline with bloom, ACES, saturation/contrast grading and vignette;
  - texture tiling, unlit materials, frustum culling.
- **Editor:**
  - asset browser with thumbnails and drag-and-drop placement;
  - asset pickers in Details; Save as Prefab.
- **Agents:**
  - Agent Designer: mission, instructions, per-category permissions, memory, usage;
  - project agent files; parallel pipeline stages.

## Next

| Area | Planned work |
|---|---|
| Rendering | GPU instancing; cascaded shadows; IBL environment maps; glTF skinning/animation and multi-material meshes; sprite atlases and a proper 2D renderer; GPU picking ID buffer |
| Platforms | Vulkan backend (Windows/Linux), then D3D12; an editor shell for non-Mac platforms |
| Physics | Collision shapes, rigid bodies and triggers (`on touch "Name"` in Wander), character controller |
| Audio | Audio engine and playback of `audio`/`music` assets; Wander `play "sound"` |
| Wander | Coroutine-style `wait`, modules, debugger (breakpoints, watch), bytecode VM |
| Agents | Built-in generator adapters (image, 3D, audio, music); MCP resources (scene, docs) and prompts; spending limits on top of usage tracking; screenshot diffing for visual review; agent-designed pipelines |
| Editor | Multi-select editing, prefab overrides and nested prefab sync, play-in-editor game window, undo history panel, layouts, keyboard shortcut editor |
| Production | Packaging and export of standalone games; asset pipeline with import settings; plugin API |
