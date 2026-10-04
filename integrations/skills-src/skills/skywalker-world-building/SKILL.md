---
name: skywalker-world-building
description: Build outdoor worlds in Skywalker - terrain presets, sculpting and painting, foliage and ground detail, oceans and lakes, and placing assets on real ground with raycast, place_on_surface and scatter. Use when creating or editing landscapes, islands, forests, villages on terrain, or water.
---

# World building

Load skywalker-core first (conventions, loop, batch). This skill is the terrain-to-props pipeline.

## The pipeline (do it in this order)

1. **Terrain** `terrain_create {preset*, size, resolution, seed, water, generator, layers, position, name}`
2. **Measure** `terrain_query {points:[[x,z],...]}` gives world height, slope, normal and dominant layer.
3. **Shape** `terrain_sculpt` (raise/lower/flatten/smooth/noise/set) and `terrain_paint` (material layers).
4. **Cover** `foliage_add` (GPU-instanced grass, flowers, pebbles, rocks, custom meshes) and `fx_create` water.
5. **Place** things with `raycast` / `place_on_surface` / `scatter {surface:"Terrain"}`.
6. **Check** `viewport_multi` for layout, `viewport_capture` for looks, `perf_stats` for cost.

Everything terrain-specific has its own undo: `terrain_undo {entity}` steps back the last sculpt, paint,
generate or layers call. Component field edits use the regular `history`.

## Presets and what they are for

| Preset | Character | Layers (base first) |
|---|---|---|
| `island_beach` | Island with beaches, erosion, seabed; pair with `water:true` | sand, seabed, grass, soil, rock |
| `tropical_coast` | Coastline with cliffs and coves | same as island_beach |
| `alpine` | Peaks, scree, snow above ~260 m | grass, scree, rock, snow |
| `mountain_valley` | Valley between ranges, snow above ~190 m | grass, scree, rock, snow |
| `canyon` | Terraced canyon walls | dust, gravel, cliff |
| `desert_dunes` | Wind-shaped dunes | sand, crust |
| `rolling_hills` | Gentle pasture/farmland | grass, soil, rock |
| `flat` | Level ground for tests and arenas | grass, soil, rock |

`size` is meters (default 512), `resolution` is 257 / 513 (default) / 1025 / 2049 samples per side (higher costs
memory and generation time; 257 is plenty for a 200 m island, 1025 for 1+ km). Each `seed` is a different
landscape. Tune with `generator`: `shape, seed, minHeight, maxHeight, featureSize (m), ridges, warp, erosion,
thermal, terraces, beachWidth, seaLevel` then re-roll in place with `terrain_generate {entity, generator}`.

Terrain heights are **absolute world values** relative to the terrain entity (rolling_hills can sit 19-28 m
above 0). Never guess the ground height: query it.

```text
terrain_create {preset:"island_beach", size:300, resolution:513, seed:11, water:true}
terrain_query  {points:[[0,0],[40,10],[-60,25]]}      # heights, slopes, dominant layer
```

## Sculpt and paint

Strokes are applied in order: `{x, z, radius, strength, mode, target, falloff}`.

```text
terrain_sculpt {entity:"Terrain", strokes:[
  {x:30, z:10, radius:14, strength:2, mode:"flatten", target:4.0},   # building pad at world y=4
  {x:-20, z:0, radius:9, strength:1.5, mode:"lower"},                # cove
  {x:0, z:-30, radius:25, strength:0.6, mode:"noise"}]}              # rougher ground
terrain_paint  {entity:"Terrain", layer:"sand", strokes:[{x:30, z:10, radius:6, strength:1}]}   # layer by name or index
```

Rules of thumb: flatten with a `target` taken from `terrain_query` for pads, roads and plazas; blend the edges
with a bigger-radius `smooth` stroke; strength in meters for raise/lower; paint strength is 0..1. Foliage that is
restricted to a `terrainLayer` follows your paint.

Custom surface textures (downloaded photoscans, generated PBR sets): `terrain_layers {entity, layers:[{name,
texture, normalMap, ormMap | texgen, color, roughness, tiling (meters per repeat), triplanar, heightMin,
heightMax, slopeMin, slopeMax, noise, sharpness}], auto_paint}`. Base layer first; later layers paint over
earlier ones where their rules match. Hand-painted weights are replaced unless `auto_paint:false`. Up to 8 layers.
Generate seamless sets with `texture_generate {kind:"rock", name:"cliff", create_material:false}`.

## Foliage and ground detail

`foliage_add {entity:"Terrain", layers:[...], seed}` grows foliage as a child of the terrain. Presets:
`meadow_grass, tall_grass, dune_grass, flowers, ferns, beach_pebbles, shells, rocks_small, boulders, custom`.
Layer fields: `preset, mesh, color, density (/m2), scaleMin/Max, slopeMin/Max, heightMin/Max (world y),
terrainLayer, wind, cullDistance, castShadows, clumping, alignToNormal, impostorDistance`.
`terrainLayer` takes the terrain layer's name (`"grass"`) or its index; an unknown name fails with a did-you-mean hint.

```text
foliage_add {entity:"Terrain", layers:[
  {preset:"dune_grass", heightMin:1.5},          # above the wet beach
  {preset:"shells", heightMax:1.2},              # only near the waterline
  {preset:"meadow_grass", terrainLayer:"grass"}, # only where grass is painted
  {preset:"custom", mesh:"asset:downloads/pine/pine.glb", density:0.02, scaleMin:0.8, scaleMax:1.4,
   slopeMax:25, castShadows:true, cullDistance:250}]}
```

Dense layers stream in around the camera, so only the visible part costs. Still check `perf_stats` after
large additions, and give heavy meshes (trees) low density plus a `cullDistance`. `density` on the component
(0..4) scales every layer at once.

Forests to the horizon: heavy meshes (imported trees, bushes, rocks) switch to **octahedral impostors** in the
distance automatically (baked once, cached in `.skywalker/cache/impostors/`), so a `cullDistance` of 1-2 km is
affordable. Per layer: `impostorDistance` (m; 0 = automatic from on-screen size, -1 = never), `impostorResolution`
(atlas px), `impostorFrames` (views per side), `impostors:false`. `impostor_bake {entity:"Forest", rebake:true,
preview:true}` bakes ahead of time or after editing a mesh/material; `viewport_capture {debug_view:"impostors"}`
tints meshes green and impostors magenta; `perf_stats` lists the triangles of each model (`foliageModels`).

## Water

`fx_create {effect:"ocean"|"calm_sea"|"storm"|"lake"|"pool"|"puddle", position:[x,y,z], overrides}`. The entity's
**y is the water level**. Ocean is endless (`size:0`); lake/pool/puddle are bodies of `size` meters with a ragged
shore. Terrain's `water:true` creates a sea at sea level plus wet sand. `water_query {points:[[x,z]]}` returns the
live animated surface height and normal (floating boats, piers above waves). Set the terrain's `waterLevel` and
`wetBand` if you add water by hand so the shore blends. Wind (`environment_update {windSpeed, windDirection}`) drives
waves and foliage.

## Placing things on real ground

| Need | Tool |
|---|---|
| Ground height / slope at known x,z | `terrain_query` |
| Ground under an arbitrary point or a pixel | `raycast {origin:[x,200,z], direction:[0,-1,0]}` (default is straight down) or `{x,y,width,height}` |
| Drop props that you placed roughly | `place_on_surface {entities:[...], offset}` (bottom of bounds rests on the geometry below) |
| Many copies (forests, rocks, coins, crowds) | `scatter {source|prefab, count, center, size|radius, min_distance, yaw, scale, surface:"Terrain", seed, group}` |
| A reusable composite | `prefab_create {entity, path}` then `prefab_instantiate {prefab, position, on_surface:true}` |

`scatter` copies are put under a new group entity and the whole call is one undo step. The `source` entity stays
where it was: delete or hide it afterwards. Use `min_distance` (a village needs 6-10, a forest 2-3) and a
`yaw:[0,360]`, `scale:[0.8,1.3]` range for natural variety. Keep `seed` fixed so reruns reproduce.

```text
batch {label:"Village pad", operations:[
  {tool:"terrain_sculpt", args:{entity:"Terrain", strokes:[{x:30,z:10,radius:16,strength:3,mode:"flatten",target:4}]}},
  {tool:"prefab_instantiate", args:{prefab:"prefabs/hut.prefab.json", position:[30,4,10], on_surface:true}},
  {tool:"prefab_instantiate", args:{prefab:"prefabs/hut.prefab.json", position:[38,4,14], yaw:70, on_surface:true}}]}
viewport_multi {}        # top view confirms spacing; side view confirms contact with the ground
```

## Judging the result

- Top view (`viewport_multi`) for layout and spacing; perspective capture at eye height for scale (a person is
  about 1.8 m, a door 2.1 m, a hut 4-6 m wide).
- Silhouettes read best at the horizon: vary the heights, avoid regular spacing, add a focal landmark.
- Floating or sunk props: `raycast` below the prop; compare `point[1]` with its bottom, or call `place_on_surface`.
- If it looks flat or plastic, it is usually lighting and materials, not terrain: skywalker-look-dev.

## Pitfalls

- Camera or props **under** terrain (see the height note above). A fully orange/brown frame means the camera is inside the ground.
- `foliage heightMin/Max` are world y, not relative to the terrain.
- Re-running `terrain_generate` repaints layers and discards hand-painted weights; sculpt and paint last.
- `scatter` needs `surface` (exact name or id of the terrain) to land only on it; verify a few copies with `scene_overview` afterwards.
- High `resolution` + high `size` is slow to generate; start at 257/513 and raise only if silhouettes are faceted.
- The terrain data lives in a `.terrain` file under the project. It is a cache: the generator plus the component's recorded `edits` (every sculpt, paint and repaint) rebuild it when it is missing, so it need not be committed.
