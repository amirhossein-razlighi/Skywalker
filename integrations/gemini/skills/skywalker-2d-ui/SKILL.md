---
name: skywalker-2d-ui
description: "2D games, UI/HUD and dialogue in Skywalker - sprites, tilemaps, orthographic cameras, menus, HUD widgets, dialogue trees. STUB until the 2D/UI workstream is merged; read docs/2D_AND_UI.md and the live tool descriptions."
---

> **STATUS: STUB.** The 2D, UI and dialogue subsystems are under construction by another workstream and are not part of this build of the skill.
> <!-- TODO(lead): after merging the 2D/UI/dialogue workstream, replace this body with: sprite/tilemap/UI/dialogue components and tools (verify against
> `skywalker tools --markdown`), layout rules, anchors, input focus and gamepad navigation, localization, recipes (title screen, HUD with health bar,
> inventory grid, branching dialogue), capture/verification tips, pitfalls. Source of truth: docs/2D_AND_UI.md. -->

## What to do today

1. `engine_info` and `component_schema`: look for sprite, tilemap, ui, canvas, text or dialogue components and matching tools. If they exist, read
   `skywalker://docs/2D_AND_UI` first.
2. Until then, 2D can be approximated with what exists:
   - Orthographic camera: `components.camera:{orthographic:true, orthoSize:6}`.
   - Flat sprites: `mesh:"quad"` with `texture`, `unlit:true` (or `billboard:true`); generated art via `asset_request {kind:"sprite", prompt}` or `texture_generate`.
   - Pixel-art look: `shading:"unlit"`, `tonemap:"none"`, bloom off (see skywalker-look-dev).
   - Input via actions (skywalker-audio); behaviors in Wander (skywalker-wander).
3. Verify layouts with `viewport_capture` at the target aspect ratio (`width`/`height`) and check text legibility at the smallest supported size.

## Rules that will hold

- Design for several aspect ratios; capture at least 16:9 and 4:3 (and a narrow portrait if mobile).
- Keep UI readable: contrast, size, safe margins. Verify by capture, not by reasoning.
- UI must be reachable with keyboard and gamepad as well as the mouse.
