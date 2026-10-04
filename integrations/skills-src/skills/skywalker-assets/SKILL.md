---
name: skywalker-assets
description: Find, download, import, generate and credit assets for a Skywalker project - models (.glb/.gltf/.obj/.fbx), textures, HDRI skies, audio - with correct licensing. Use for Poly Haven / Kenney / Quaternius / ambientCG downloads, asset_download, asset_import, procedural textures, prefabs and CREDITS.md.
---

# Assets

The project folder is the asset store. Every file becomes a record with a stable GUID (`.meta` sidecar), tags, a description and
provenance. Search before you fetch: the asset you need may already be in the project.

## Decision order

1. **Reuse**: `asset_list {type, tag, query}` then `asset_info {asset}` (shows which entities use it) and `asset_preview {asset}` to look.
2. **Generate procedurally** (free, deterministic, no licensing): `texture_generate` (PBR sets), `audio_generate` (SFX and music),
   `dcc_generate` (buildings, towers, rocks, stairs... via Blender, see skywalker-dcc), `terrain_create` + `foliage_add`.
3. **Download openly licensed** assets with `asset_download` (the human approves each download).
4. **Ask a generator** (`asset_request {kind, prompt, style, target}` queues it; whoever fulfills it calls `asset_complete`).
   If you have your own image/3D/audio generation tools, make the file in the project and call `asset_complete {id, path}`.
5. Never scrape, never bypass logins/paywalls/rate limits, never use assets whose terms you cannot read.

## Licensing rules (non-negotiable)

- Only: **CC0 / public domain, CC-BY (needs attribution), MIT / similar permissive**, or terms the human explicitly accepted.
- Give the license **exactly as stated on the source page** in `license`, plus `author`, `source_page` and, if the license requires it, `attribution`.
- Unknown or "all rights reserved" is refused. Non-commercial (NC) and no-derivatives (ND) produce warnings: tell the human
  before using them in anything that may ship.
- `asset_download` records license, author, URL and retrieval date in each file's `.meta` and appends to the project's `CREDITS.md`.
  Do not edit credits by hand; do tell the human that CC-BY assets must be credited in shipped builds.

## Good sources

| Source | License | Notes |
|---|---|---|
| Poly Haven (polyhaven.com) | CC0 | HDRIs, PBR textures, scanned models. Public API, no key; send a descriptive `User-Agent`. `GET https://api.polyhaven.com/assets?t=models` lists, `GET https://api.polyhaven.com/files/<id>` lists download URLs per resolution (and the dependency files for glTF, which go in `include`). |
| Kenney (kenney.nl/assets) | CC0 | Stylized kits (FBX/OBJ/GLB). Convert FBX with `dcc_convert`. |
| Quaternius (quaternius.com) | CC0 | Low-poly characters, nature, props. |
| ambientCG (ambientcg.com) | CC0 | PBR materials (zip of maps). |
| OpenGameArt, itch.io, Sketchfab, Freesound | per asset | Read the license on the asset page. Many need a login to download: do not work around that, ask the human to provide the file. |

Typical download:

```text
asset_download {url:"<gltf url from the files API>",
  license:"CC0-1.0", author:"<author from the API>", source_page:"https://polyhaven.com/a/<asset id>",
  include:{"textures/<name>_diff_1k.jpg":"<url from the files API 'include' map>"},   # glTF dependencies a library hosts elsewhere
  folder:"crate", import:true, normalize:false, create_entity:"Crate", position:[0,0,0]}
```

Take every URL from the library's own API or page response, never from memory. A `.zip` pack is extracted with
traversal and zip-bomb guards; multi-file glTF and OBJ+MTL dependencies are fetched automatically.

## Import settings

| Option | Meaning |
|---|---|
| `normalize` (default true) | Fit the model into a 1 m cube. Use `false` for assets already in meters (photoscans, kits) to keep real size. The result reports `bounds`. |
| `z_up` | Rotate Z-up sources (CAD, some exporters) to Y-up. |
| `create_entity` / `position` | Place it immediately. |
| `tags`, `description` | Write them (`asset_tag`) so other agents can find the asset. |

Formats: meshes `.glb .gltf .obj(+.mtl) .ply .stl` natively; `.fbx .dae .usd .3ds .abc .blend` through `dcc_convert` (needs Blender).
glTF materials become a material asset; multi-material models become prefab parts. A model's origin and scale vary by source:
after import, capture it next to a 1.8 m reference and fix scale with `transform` (or re-import with `normalize:false`).

## Using assets

- Mesh on an entity: `components:{mesh:{mesh:"asset:downloads/crate/crate.glb"}}`.
- Reuse: `prefab_create {entity, path:"prefabs/crate.prefab.json"}` then `prefab_instantiate` / `scatter {prefab}`.
- Materials: `material_create {path, preset|color|texture|normalMap|ormMap}`, `material_assign {entities, material}`; sky: `environment_update {skyMode:"hdri", hdri:"downloads/.../x.hdr"}`.
- Textures on terrain: `terrain_layers` (see skywalker-world-building). Procedural sets:
  `texture_generate {kind:"cobblestone", name:"street", create_material:true, tiling:2}` (kinds include noise, marble, wood, planks, bricks,
  tiles, cobblestone, grass, dirt, sand, rock, metal_brushed, rust, fabric, scales, stylized, glow, curtain).
- Rename or move with `asset_move {asset, to}` (GUID and scene references follow); never rename files on disk behind the engine's back.
  `asset_refresh` rescans after you add files with other tools.

## Shared kits outside the project (game.json `mounts`)

A folder of assets shared by several projects (characters, props, materials) is mounted under a top-level name:
`game_settings {operation:"set", settings:{mounts:{kit:"../_kit"}}}` (or edit `game.json`). Then `kit/...` paths work
everywhere a project path does: `prefab_instantiate {prefab:"kit/characters/guard.prefab.json"}`, materials, textures,
animation libraries, `asset_list {query:"kit/props/*"}`. The kit's files keep their `.meta` import settings, and
`game_build` copies the mounted files a game uses into the package. Kit prefabs are tagged `kit-character` / `kit-prop`,
which `scene_audit` uses to suggest replacements for placeholder shapes.

## Pinned downloads for a whole project (`media/demo/assetkit.py`)

For many assets, keep them out of version control in a pinned manifest instead of one `asset_download` call each:
`python3 media/demo/assetkit.py add-polyhaven assets.json model wooden_crate_01 --res 2k`, `add-ambientcg`, then `pin`
(downloads and records sha256), `fetch` (parallel, resumable, verified; into the ignored `downloads/` folder), `check`
(every license in the allowlist: CC0, CC-BY, MIT, Apache, OFL) and `credits` (writes CREDITS.md). For a few files out
of a big zip pack, set `"remote": true` with `extract` include globs: only those members are downloaded (`ls URL` lists a pack).

## Checklist after bringing an asset in

1. `asset_preview` or a capture next to a reference-size object: scale, orientation (faces -Z), pivot at the feet, materials present.
2. `asset_tag {asset, tags:[...], description:"..."}` with what it is and where it came from.
3. Triangle count sanity (`perf_stats` after placing many); decimate heavy scans with `dcc_edit_asset` (skywalker-dcc).
4. `CREDITS.md` has the line (CC-BY!), the `.meta` has the license.

## Pitfalls

- Wrong scale from `normalize:true` on assets that were already metric.
- Missing textures: glTF with external images needs `include` entries or the zip; check the preview for a flat gray look.
- A downloaded `.hdr` is a **panorama** for `skyMode:"hdri"`, not a texture for meshes.
- Do not guess URLs: read them from the source's API or page. A failed download is cheaper than a hallucinated asset.
