---
name: skywalker-vfx
description: "Visual effects and hair in Skywalker - fire, smoke, explosions, rain, snow, sparks and magic via fx_create/fx_burst, volumetric fluids, water, GPU particles (millions, depth collisions, sub-emitters, ribbons, mesh particles, flipbooks, force fields), strand hair and fur (groom_create/groom_update), and measuring cost with fx_stats and fx_benchmark. Use for any effect, weather, muzzle flash, impact, trail, character hair or creature fur."
---

# Effects, GPU particles and hair

Load skywalker-core first. Engine docs: `skywalker://docs/HAIR_AND_VFX` and `skywalker://docs/RENDERING`. Start from a preset, tweak a few fields, look, measure.

## Pick the right kind

| Need | Use | Notes |
|---|---|---|
| Hero fire/smoke, explosions you can light a scene with | Volumetric fluids: `volume_fire`, `volume_torch`, `volume_smoke`, `steam_vent`, `explosion_volume` | Ray-marched GPU fluid, lights the scene itself. Cost: keep to one or two. |
| Composites | `campfire`, `torch`, `burning_barrel`, `explosion` (volumetric + embers/shrapnel + light); `sprite_campfire` is the cheap version | Place at the base of the object. |
| Gameplay-countable particles (hit sparks a rule depends on) | CPU particles: `fire`, `embers`, `smoke`, `steam`, `sparks`, `rain`, `snow`, `mist`, `spray`, `dust`, `fireflies`, `magic`, `fireball`, `debris_smoke`, `shrapnel` | `simulation:"cpu"`: deterministic, seeded per emitter, replayable, up to 50k. |
| Large visual-only effects (storms, fireworks, vortices, leaves) | GPU particles: `sparks_shower`, `fireworks`, `rain_heavy`, `waterfall_mist`, `ember_storm`, `magic_vortex`, `dust_storm`, `falling_leaves`, `snow_heavy`, `smoke_column_gpu` and building blocks `sparks_gpu`, `embers_gpu`, `rain_gpu`, `splashes_gpu`, `rockets_gpu`, `firework_burst_gpu`, `mist_gpu`, `droplets_gpu` | `simulation:"gpu"`: compute shaders, up to 4M particles, **not deterministic and invisible to gameplay**. |
| Water | `ocean`, `calm_sea`, `storm`, `lake`, `pool`, `puddle` | Entity y = water level; see skywalker-world-building. |
| Hair and fur | `groom_create` presets `hair_straight hair_wavy hair_curly hair_ponytail hair_short fur_short fur_long`; rigged characters `hair_scalp beard eyebrows fur_dense` | Real strands, GPU-simulated, Marschner shading. |

## Workflow

1. **Create** the closest preset at the right spot and scale (a campfire is ~1 m, a torch small, an explosion 5-15 m): `fx_create {effect, name, position, parent, overrides}`. `overrides` patch the particles/water fields (composites: every emitter).
2. **Look**: `viewport_capture` close up (`eye`/`target` 4-8 m away, `samples:8`). Effects read best on dark or mid-tone backgrounds with `bloomIntensity` 0.4-0.7.
3. **Animate in time**: effects move. `sim_control step` (or just capture; previews animate while editing) and capture at 3-4 times; burst effects look dead in a still, so capture 5-20 ticks after `fx_burst`.
4. **Tune a few fields**, recapture. **Measure** (below) before adding more.
5. Fires light their surroundings by themselves; add a real point light only for stylized pools of light.

```text
fx_create {effect:"campfire", name:"Camp Fire", position:[0,0,0]}
fx_create {effect:"rain", position:[0,12,0], overrides:{floorHeight:0, rate:900}}
fx_create {effect:"sparks_shower", position:[3,1,0]}
fx_burst {entity:"Camp Fire", count:60}
viewport_capture {eye:[4,2,6], target:[0,0.8,0], samples:8, annotate:false, overlays:false}
sim_control {action:"step", ticks:20}
```

In Wander: `burst(n)` or `burst(entity, n)`, `spawn("prefab:...")` for an explosion prefab. Rain and snow cover a box around the emitter: place it ~12 m above the area and set `floorHeight` to the ground. Wind
(`environment_update {windSpeed, windDirection}`) carries smoke, rain, leaves and anything with `wind > 0`.

## The `particles` component (read the live list: `component_schema {component:"particles"}`)

- **Emission**: `rate`, `burst`, `maxParticles`, `lifetime` (+`lifetimeJitter`), `shape` point|sphere|box|disc|cone (+ `mesh` on GPU via `shapeMesh`) with `shapeSize`, `direction`, `speed`, `spread`, `prewarm`, `seed`, `emitting`.
- **Motion**: `gravity` (negative = buoyant), `drag`, `turbulence` + `turbulenceScale` (curl noise), `wind`, `worldSpace` (leave a trail behind a moving emitter).
- **Look**: `look` glow|flame|smoke|spark|rain|snow|mist|sprite, `colorStart`/`colorEnd`, `sizeStart`/`sizeEnd`, `intensity` (HDR), `softness`, `stretch`, `light` (casts a flickering point light).
- **Collision**: `collide` + `floorHeight` + `bounce` + `splash` (CPU floor plane). GPU adds `depthCollision` (everything visible on screen collides), `colliders` (entity names), `stick`, `friction`.

### GPU particles (`simulation:"gpu"`)

| Feature | Fields |
|---|---|
| Facing | `facing`: `camera`, `velocity` (stretched, `stretch`), `horizontal` (ripples, decals), `ribbon` (trails: `trailLength` seconds, `trailSegments`), `mesh` (lit instanced meshes: `mesh` primitive/asset/`fx:leaf`/`fx:shard`/`fx:pebble`, `roughness`, `metallic`) |
| Curves | `colorGradient` `"#fff6d0@0 #ffa030@0.2 #a01800@0.7 #20000000@1"`, `sizeCurve` `"0@0 1@0.1 0.3@1"`, `opacityCurve`, `hueVariation` |
| Force fields | `field`: `vortex` (`fieldAxis`, `fieldPull`, `fieldLift`), `attractor`, `texture` (`fieldTexture` `.fga` from Houdini/EmberGen + `fieldSize`), with `fieldStrength`, `fieldRadius`, `fieldCenter` |
| Sub-emitters | `subEmitter` (another GPU emitter by name), `subEmitOn` death|collision|both, `subEmitCount`, `subEmitInherit`. One sub-emitter per emitter, chains up to 4 deep |
| Flipbooks | `look:"sprite"`, `texture`, `flipbookColumns`, `flipbookRows`, `flipbookFps` (0 = once per life; frames cross-fade), `intensity` > 1 for emissive explosions |
| Sorting | blended looks are sorted back to front on the GPU (`sort:false` is faster) |

```text
entity_create {name:"Tornado", position:[0,0.3,0], components:{particles:{simulation:"gpu", look:"smoke", facing:"camera", field:"vortex", fieldLift:3, fieldPull:2, rate:4000, maxParticles:200000, lifetime:5}}}
entity_create {name:"Ship Trail", parent:"Ship", components:{particles:{simulation:"gpu", look:"glow", facing:"ribbon", worldSpace:true, trailLength:1.2, trailSegments:24, rate:60, colorGradient:"#aaddff@0 #3366ff@0.5 #00000000@1"}}}
entity_create {name:"Explosion Flipbook", position:[0,1,0], components:{particles:{simulation:"gpu", look:"sprite", texture:"fx/explosion_sheet.png", flipbookColumns:8, flipbookRows:8, flipbookFps:24, burst:1, rate:0, intensity:2, lifetime:1.2, sizeStart:3, sizeEnd:3}}}
```

Recipes: grinder sparks on props `sparks_shower` (depth collision on, aim with the child's `direction`); paint splatter `stick:true` + `depthCollision:true`; leaves in wind `falling_leaves` ~6 m up + `windSpeed:5`; particles shaped like a mesh
`shapeMesh:"asset:..."` with low `speed`; magic trail on a moving object: a child emitter with `facing:"ribbon"`, `worldSpace:true`.
Limits: GPU depth collisions only see what is on screen (the previous frame); sprite/volume effects render after transparent meshes (glass in front of fire will not sort perfectly).

## Hair and fur (`groom`)

```text
groom_create {entity:"Head", preset:"hair_wavy", overrides:{melanin:0.5, redness:0.6, strands:30000}}
groom_update {entity:"Head", fields:{curlRadius:0.01, curlFrequency:16, length:0.3}}
groom_info {entity:"Head"}
groom_export {entity:"Head", path:"grooms/head.groom.json"}
viewport_capture {eye:[0.5,1.75,1.1], target:[0,1.65,0], samples:16, annotate:false, overlays:false}
```

- Heads grow hair on the upper back of the mesh (set `maskDirection`/`maskAngle`/`maskSoftness` or a vertex-color mask `maskChannel` for your model); fur presets cover the whole mesh. `target` grows on another entity's mesh. Roots, guides and children: `strands`, `guides`, `segments`, `length` (m), `widthRoot`/`widthTip` (mm).
- **Geometry fields regenerate** the strands (~0.3-0.7 s per 100k): `strands length curlRadius curlFrequency wave frizz clumps clumpStrength direction gravity mask*`. **Color and motion apply instantly**: `melanin` (0.1 platinum, 0.2 blond, 0.5 light brown, 0.85 dark brown, 0.95+ black), `redness`, `dye`, `rootColor`, `tipColor`, `roughness`, `scatter`, `stiffness`, `wind`, `simulate`.
- Simulation: Verlet on the GPU with gravity, environment wind and collisions with sphere/capsule/plane proxies (the scalp's own plus `colliders:"Torso, Neck"`); `simulate:false` keeps the groomed pose. Grooms are generated in **meters on the scaled mesh**, so scaling an entity to head size does not shrink its hair.
- Quick looks: black glossy `melanin:0.97, roughness:0.22`; ginger `melanin:0.5, redness:0.9`; pastel `melanin:0.15, dye:"#ffb0d8"`; buzz cut `hair_short, length:0.006-0.02, simulate:false`; cat fur `fur_short, strands:150000-300000, length:0.015-0.03`.
- **Rigged characters**: on a skinned mesh the roots ride the animated skin (`attach:"auto"`, bound to triangles), strands collide with capsules fitted to the skeleton (`bodyColliders`), `follow` (0..1) sets how much skin motion they take rigidly and `maxSpeed` caps whipping on fast moves. Limit growth with `maskBone:"Head"` plus a region `maskCenter`/`maskRadius` in `maskSpace:"bounds"` (-1..1 of the bone's vertices), `maskMirror` for brows. Check with `viewport_capture {debug_view:"groom_roots"}` while running and `groom_info` (`attach.boundRoots`). Engine doc: `skywalker://docs/CHARACTERS`.
- **Grooms stack**: scalp, beard and brows on one character are three `groom_create` calls (`entity` = the character or its body mesh); each further groom gets its own child entity ("Beard", "Eyebrows", or `name`) growing on the same mesh, returned as `entity`. `groom_info {entity:"Hero"}` lists them all; `replace:true` swaps the mesh's own groom instead.
- Files: `.hair`, `.groom.json`, `.skygroom` for DCC round trips (`groom_export`); load with `source` (`importScale` 0.01 for centimeter files, `importZUp`). Export folders must already exist.
- Level of detail: `lod:auto` draws strands up close and cards far away; real-time views thin the strands by screen size (one 600k budget shared by every groom in view, a floor of 1500 each) and widen the remaining ones so coverage stays solid, while stills (`samples` > 1) draw every strand. Judge hair at `samples:16`+.

## Measure cost (do it before adding more)

```text
fx_stats {}                                                    # after rendering a few frames: GPU ms of frame / particle sim / hair, live particles per emitter, per-groom sizes
fx_benchmark {width:1920, height:1080, frames:120, serial:true}  # real-time frames back to back; per-pass GPU ms (serial:true waits per frame so passes do not overlap)
groom_info {}                                                  # every groom: strands, memory, measured GPU cost
perf_stats {frames:30}                                         # whole-scene benchmark
```

Budget guide (Apple M1 Pro, 1080p): the stage scene costs ~7 ms; 1M additive GPU particles add ~9 ms, 1M lit sorted smoke ~31 ms (fill rate); a 100k-strand groom filling the screen ~21 ms, at medium distance ~10 ms. Keep a hero head under ~2 ms.
Reduce with fewer `strands`/`segments`, lower `maxParticles`, additive `glow` instead of lit `smoke`, `sort:false`, smaller particles, and `renderScale`. Values are 0 on renderers without GPU effects (CPU fallback).

## Verification loop

1. Capture the effect from the gameplay camera distance, not only close up; check readability: effects must communicate hits, pickups and danger without hiding the player.
2. Capture at several ticks (start, peak, fade) and once with `debug_view:"lighting"` if the fire seems to light nothing.
3. `fx_stats` / `fx_benchmark` numbers in the report; for gameplay-relevant effects confirm the CPU path with `sim_trace` or counts.

## Pitfalls

- Rain emitter at ground level does nothing useful; raise it ~12 m and set `floorHeight`.
- Too many overlapping soft particles wash out the frame: lower `rate`/`intensity`, raise `bloomThreshold`.
- GPU particles are visual only: anything a game rule depends on must use the CPU path.
- Burst presets: a still capture right after creation shows nothing; wait ticks.
- Hair appears floating or missing: wrong mask for the mesh (set `maskDirection`), or the entity has no mesh; check `groom_info` counts.
- Changing a geometry field on a huge groom stalls for a moment; tune on 20-30k strands, raise for the final capture.
