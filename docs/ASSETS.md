# Assets

Skywalker treats the project folder as the asset store. Every file the engine understands
becomes an **asset record** with a stable identity and metadata that agents can search. The
same records drive the editor's asset browser and the agent tools.

## Asset records

| Field | Meaning |
|---|---|
| `guid` | Stable id, stored in a `<file>.meta` sidecar. It survives renames and moves (`guid:3f2a…`). |
| `path` | Project-relative path with `/` separators. Scenes reference assets by path. |
| `type` | `mesh`, `texture`, `material`, `prefab`, `scene`, `audio`, `video`, `script`, `agent`. |
| `tags`, `description` | Free-form search metadata. Agents should write them. |
| `source` | Provenance, e.g. `{"kind":"mesh","prompt":"…","by":"agent:Nimbus"}`. `asset_complete` fills it in for generated files. |
| `import` | Importer output, e.g. the material created from a glTF file. |

The database rescans the project every two seconds while editing (and on `asset_refresh`).
The scan skips dot-folders, `build/`, `node_modules/` and `DerivedData/`. You can commit
the `.meta` files to keep GUIDs stable for your team.

Types are recognized by file name:

| Type | Extensions |
|---|---|
| Mesh | `.glb`, `.gltf`, `.obj` (+ `.mtl`), `.ply`, `.stl` |
| Texture | `.png`, `.jpg`, `.jpeg`, `.hdr` (sky panoramas for `skyMode: hdri`) |
| Material | `.mat.json` |
| Prefab | `.prefab.json` |
| Scene | `.sky.json` |
| Agent | `.agent.json` |
| Audio / video | `.wav`, `.mp3`, `.ogg`, `.m4a` / `.mp4`, `.mov` |

## Meshes

| Format | What is imported |
|---|---|
| glTF 2.0 / GLB | Node hierarchy (flattened), normals, UVs, vertex colors and **every material** (base color, metal/roughness, emissive, normal / ORM / emissive maps, alpha mask or blend, double-sided). Models with several materials become *parts* (`asset:model.gltf#<material>`), one entity each, saved together as `<model>.prefab.json`. External textures are referenced in place. |
| OBJ + MTL | Polygons (triangulated), normals, UVs, `v x y z r g b` vertex colors, and the first `.mtl` material (`Kd`, `d`, `Ns`/`Pr` → roughness, `Pm`, `Ke`, `map_Kd`, `map_Bump`/`norm`). |
| PLY | ASCII and binary (both endians): positions, normals, UVs, vertex colors, polygon faces. Point clouds without faces are rejected with a hint. |
| STL | ASCII and binary, flat-shaded facets (CAD and 3D-printing models). |

Import options for `asset_import` and `asset_download`:
- `normalize` (default true) fits the model to 1 m. Use `false` to keep real-world units
  (photoscanned assets from libraries are already in meters). The result reports the model's
  `bounds`, so agents can place it precisely.
- `z_up` rotates Z-up sources (CAD, scans, some exporters) to Y-up.

Both options are stored in the asset's `.meta`, so the model loads the same way every time.

### Downloading models from the web

`asset_download {url, license, author?, source_page?, attribution?, include?}`:
- **Accepts:** a direct model, texture or audio file, or a `.zip` pack.
  - `.zip` packs are extracted with traversal and zip-bomb guards.
  - Multi-file glTF and OBJ+MTL dependencies are fetched automatically; `include`
    (`{"textures/a.jpg": "https://..."}`) adds files a library hosts elsewhere (Poly Haven's
    file API lists them).
- **Saves** into `downloads/<name>/`.
- **Records** the license, author, URL and retrieval date in every file's `.meta`.
- **Credits:** appends a line to the project's `CREDITS.md`.
- **Imports** the model, and optionally places it in the scene.

Rules for agents:
- Only download assets whose license allows the project's use (CC0, CC-BY with attribution, MIT, public domain, or terms the human accepted).
- Never bypass logins, paywalls or rate limits.
- Unknown or "all rights reserved" licenses are refused. Non-commercial and no-derivatives licenses produce warnings.

The tool is **open-world**:
- MCP clients (Claude Code, Codex, …) show it as a network action that needs approval.
- In the editor, the **Network** permission category asks before every download unless you explicitly allow it for an agent.

### Notes on OBJ and glTF

- **glTF and GLB** import supports:
  - embedded, data-URI and external buffers;
  - strided accessors and indexed or non-indexed triangles;
  - the full node hierarchy (TRS or matrix), flattened into one mesh;
  - the first material's base color, metallic/roughness, emissive and base-color texture.
- **Importing a glTF file** also writes `<name>.mat.json` next to it, plus the extracted
  texture (`<name>_albedo.png`). The new entity uses that material.
- **Normalization.** Meshes are scaled to fit a 1 m cube and centered on the ground, so
  output from generation models arrives at a predictable size.
- **CPU copies.** A CPU copy of every mesh is kept, for triangle-accurate `raycast`,
  `place_on_surface` and `scatter`.

## Materials (`*.mat.json`)

```json
{ "format": "skywalker.material", "version": 1,
  "color": "#e0b040", "metallic": 1, "roughness": 0.25,
  "emissive": "#000000", "texture": "", "tilingU": 1, "tilingV": 1, "unlit": false }
```

- `mesh.material` points at a material. When it is set, the material overrides the
  entity's inline color, PBR values, emissive and texture.
- Editing the file updates every user live.
- Emissive colors above the bloom threshold glow.
- `unlit` gives a flat color or texture (2D, UI, stylized looks).

## Prefabs (`*.prefab.json`)

A prefab is a saved entity tree: components, behaviors (intent + Wander), vars, tags and
children. The root's position is stored at zero, so instances can go anywhere.

You can create and place prefabs in several ways:
- **Create:** use `prefab_create`, or right-click an entity in the Outliner and choose
  **Save as Prefab**.
- **Place:** use `prefab_instantiate` (optionally `on_surface`), or drag the prefab from
  the asset browser.
- **Scatter:** `scatter {prefab, count, …}`.
- **At runtime:** from Wander, `spawn("prefab:prefabs/coin.prefab.json", (x, y, z))`.

## Agent tools

| Tool | Use |
|---|---|
| `asset_list` | Search by type, tag or text. Each line shows the path, type, tags, description and generator prompt. |
| `asset_info` | Everything about one asset, including **which entities use it**. |
| `asset_preview` | Renders an isolated thumbnail: a mesh, a material on a sphere, a texture, or a prefab. |
| `asset_import` | Registers a file, imports meshes, optionally creates an entity. |
| `asset_tag` | Sets tags, description and provenance. |
| `asset_move` | Renames or moves an asset. The GUID and `.meta` follow it, and scene references are rewritten. |
| `material_create` / `material_update` / `material_assign` | Create, change and use materials. |
| `prefab_create` / `prefab_instantiate` | Build once, reuse everywhere. |
| `raycast`, `place_on_surface`, `scatter` | Spatial placement on real geometry. |
| `viewport_multi` | Perspective plus top, front and side orthographic views in one image. |

A typical agent flow:
1. `asset_list {query: "tree"}` to find candidates.
2. `asset_preview` to look at them.
3. `scatter {prefab: "prefabs/pine.prefab.json", count: 40, radius: 12, min_distance: 1.5, surface: "Island"}`.
4. `viewport_multi` to check the layout.

## Editor

- **Assets dock:**
  - type filters with counts, search over paths, tags and descriptions;
  - lazily rendered thumbnails (cached until the file changes);
  - an inspector showing provenance and usage.
- **Drag an asset into the viewport:**
  - meshes and prefabs land on the surface under the cursor;
  - materials and textures apply to the object under the cursor.
- **Drag onto a field:** drop on the *Material*, *Texture* or *Mesh* field in Details,
  or use the field's asset menu.
