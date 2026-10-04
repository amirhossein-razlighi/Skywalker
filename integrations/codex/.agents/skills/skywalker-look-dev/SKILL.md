---
name: skywalker-look-dev
description: "Art-direct how a Skywalker scene looks - lighting, sky and atmosphere, GI/SSR/TAA, volumetric clouds and god rays, fog, auto exposure, tonemapping, color looks and LUTs, camera lens (depth of field, motion blur), materials - and judge renders with samples and debug views. Use for golden hour, overcast, night neon, interiors, \"make it look AAA/cinematic\", or when an image looks flat, washed out, blown out or orange."
---

# Look development

Load skywalker-core first. Look-dev is an **iterate-and-measure** loop: change a few fields, capture, judge against
the rubric below, change again. Do not set 30 fields at once: you cannot tell which one hurt.

## Where the controls live

| Layer | Tool | Fields |
|---|---|---|
| Scene lighting and post | `environment_update` (merge; `environment_get` reads back) | see the table below |
| Lights | `entity_create`/`entity_update` with `components.light` | `kind: directional\|point\|spot, color, intensity (0..1000), range (m), spotAngle`; v2: `temperature` (Kelvin, 0 = off), `innerAngle` (spot hard core), `specular` (0 = no glints), `volumetric` (god-ray strength), `negative` (subtracts light), `attenuation: smooth\|inverse_square` + `size`, `distanceFade` + `fadeBegin`/`fadeLength`, `cullMask` (layers it lights); shadows: `castShadows` (default true), `shadowResolution`, `shadowBias`, `shadowNormalBias`, `shadowMaxDistance`, `shadowMode: cube\|dual_paraboloid` |
| Light shadows | `light_shadows` (one light, a list or `"all"`), `shadow_atlas_info` | budgets in the environment: `localShadowLights` (16), `localShadowUpdates` (24 views/frame), `localShadowAtlas` (4096 px) |
| Render layers | `render_layers` | name layers (`action:"name"`), put meshes on layers (`layers`) and give cameras / lights a `cull_mask`, by name |
| Camera lens | `components.camera` | `fov, aperture (f-stop, 0 = DOF off), focusDistance (m, 0 = autofocus), motionBlur (0..1, 0.5 film-like: camera and moving objects), primary, cullMask` |
| Surfaces | `material_create` / `material_update` / `texture_generate`, or `components.mesh` | `color, metallic, roughness, emissive (alpha = strength, HDR), normalMap, ormMap, clearcoat, subsurface, triplanar, shading: pbr\|toon\|unlit\|water` |
| Judging | `viewport_capture` | `samples, debug_view, overlays:false, annotate:false, view:"scene", quality` |
| Live viewport tier | `viewport_quality` | `fast`, `balanced`, `full` (see below) |

`environment_update {preset: noon|sunset|night|overcast|studio}` applies a preset first, then your overrides. Start
from the closest preset, never from nothing.

| Group | Fields (ranges) |
|---|---|
| Sun | `sunAzimuth, sunElevation (-90..90), sunColor, sunIntensity (0..100), sunSize, shadowSoftness (0..6), shadowDistance` |
| Sky | `skyMode: gradient\|atmosphere\|hdri`; gradient: `skyTop, skyHorizon, ground`; hdri: `hdri (.hdr path), hdriRotation, hdriIntensity, align_sun_to_hdri:true`; `stars (0..1)` |
| Clouds | `clouds (cover 0..1), cloudMode: volumetric\|flat, cloudHeight, cloudThickness, cloudDensity (0.3 wispy..2 stormy), cloudScale, cloudSpeed` |
| Light bounce | `ambient (0..10), reflections (IBL strength), ao, aoRadius, gi (0..1 screen-space bounce + emissive), giDistance, ssr (0..1 glossy reflections)` |
| Air | `fogColor, fogDensity (0..1, exponential), fogHeight (pools near ground), godRays (0..8, 1 natural), haze (0.005 clear..0.1 misty), windSpeed, windDirection` |
| Image | `taa, sharpen, tonemap: aces\|agx\|neutral\|filmic\|none, exposure, autoExposure, exposureCompensation (EV), adaptationSpeed, temperature, tint, saturation, contrast, bloomIntensity, bloomThreshold, vignette, grain, chromaticAberration` |
| Grade | `look: none\|warm\|cool\|teal_orange\|golden_hour\|bleach\|noir\|vivid\|moonlight\|vintage`, `lookStrength (0..1)`, `lut` (project-relative `.cube`) |

## Starting recipes (validated starting points, then tune)

Details and per-look numbers are in [references/recipes.md](references/recipes.md). Summary:

| Look | Recipe |
|---|---|
| **Golden hour** | `preset:"noon"`, `skyMode:"atmosphere"`, `sunElevation:12-16`, `sunColor:"#ffd9a8"`, `sunIntensity:3`, volumetric `clouds:0.35`, `gi:0.5`, `godRays:0.8`, `haze:0.012`, **cool** `fogColor:"#a9bbd6"` with low `fogDensity:0.0015`, `look:"golden_hour"` at `lookStrength:0.25`, `tonemap:"agx"`, `autoExposure`, `exposureCompensation:-0.3`. Camera low (eye height), sun behind/beside the subject for rim light. |
| **Overcast / moody** | `preset:"overcast"`, `clouds:0.8`, `cloudDensity:1.2`, soft `shadowSoftness:4`, `sunIntensity` low, `ao:1.2`, desaturate (`saturation:0.85`), `look:"bleach"` at 0.2-0.3, `fogDensity:0.006`, `fogHeight:0.5`. |
| **Night neon** | gradient sky near black, `stars`, low `ambient:0.15`, `gi:0.8`, `ssr:0.9` on wet glossy floor (`roughness:0.1-0.2`), emissive strips (`emissive` strength 2-5) plus matching point lights, `bloomIntensity:0.5-0.7` with `bloomThreshold:1.2+`, `look:"teal_orange"` 0.3, `chromaticAberration:0.1`, `grain:0.08`. |
| **Interior** | `ambient:0.1-0.3` (the sky must not light the room), `reflections:0.3`, sun through windows (`godRays` + `haze:0.02`), warm point/spot lights (intensity 8-25 for `range` 8-14), `ao:1.2`, `gi:0.7`, `autoExposure` with `adaptationSpeed` 1-2. |
| **Cinematic close-up** | `fov:28-40`, `aperture:1.8-2.8`, `focusDistance` = distance to the subject (`raycast` it), `motionBlur:0.5` only for moving shots, `vignette:0.25`, `grain:0.06`. |
| **Stylized / toon** | `shading:"toon"` + `outline:2-3` + `rim:0.5`, `tonemap:"neutral"`, `saturation:1.2`, `gi:0`, `ssr:0`. |

## Judging a render (the part that makes it good)

1. **Capture a beauty shot**: `viewport_capture {view:"scene" or eye/target, width:1280, height:720, samples:16, overlays:false, annotate:false}`.
   `samples` is jittered supersampling: 1 is a fast preview (noisy GI, shimmering), 16-32 is final quality. Judge looks
   at 8+ samples only. The editor view also draws a grid and id labels unless you disable `overlays` and `annotate`.
2. **Read it against this rubric**, in order: exposure (not crushed, not clipped; is there detail in the brightest and darkest
   areas?), contrast and depth (foreground, midground, background separated by value and haze?), color (one dominant hue, one
   accent; not monochrome), lighting direction (readable shadows, a rim or key), material read (metal looks like metal, stone has
   roughness variation), focus (is the eye led to the subject?), noise/shimmer (raise `samples`).
3. **Diagnose with debug views** instead of guessing:

| Symptom | Capture | What it tells you |
|---|---|---|
| Too flat, no depth | `debug_view:"normals"`, `"ao"` | Missing geometry detail or `ao` off |
| Everything glows or is washed out | `"lighting"` (before SS-GI/reflections), `"gi"` | Which term adds the light: fog/haze, GI, ambient, bloom |
| Materials look plastic or wrong | `"albedo"`, `"material"` (roughness red, metallic green) | Albedo too bright/saturated; roughness uniform |
| Reflections absent or noisy | `"reflections"` | `ssr` too low, floor too rough, `samples` too low |
| Wrong shape / scale | `"depth"` | Near/far planes, scale errors |
| Is it the light or the material? | `"lighting_only"` (white material), `"unshaded"` (albedo + emission) | Light placement and shadows vs. texture/albedo problems |
| Glossy/metal looks wrong | `"specular"` (F0 x glossiness) | Dielectrics should be dark gray; metals tinted; a green/colored dielectric means a wrong `metallic` or ORM blue channel |
| Shadows blocky or missing in the distance | `"shadow_cascades"` (red, green, blue, yellow, gray = none) | `shadowDistance` too short or too long for the shot |
| Texture blurry or swimming | `"texel_density"` (green = 512 texels/m), `"uv_checker"` | Texture too small for its size on screen, stretched or flipped UVs, wrong `tiling` |
| Smearing / ghosting of moving things | `"motion"` (capture `samples:1` right after a sim `step`) | The velocity buffer: moving meshes, skinned characters, swaying foliage, hair and mesh particles show hue = direction; gray = static. Something that moves but stays gray will ghost under TAA / MetalFX |

4. **Change one group, recapture, compare.** Keep what improved. Stop when the rubric passes, not when you run out of ideas.

## Viewport quality tiers (editing speed vs. the final image)

The live editor viewport renders at a **tier** so heavy worlds stay responsive while you edit. `viewport_quality {}` reads it, `viewport_quality {quality:"fast"|"balanced"|"full"}` sets it. Play mode and the game always render `full`.

| Tier | What it does |
|---|---|
| `fast` (editor default) | Internal resolution 0.5 (MetalFX upscales), no screen-space GI, reflections or light shafts, no depth of field or motion blur, shadow distance capped at 150 m, far foliage culled sooner (x0.4), coarser LODs. Volumetric clouds stay on. |
| `balanced` | Internal resolution 0.75, no depth of field or motion blur, everything else as authored |
| `full` | Exactly what the game and captures show |

- **`viewport_capture` and `perf_stats` take a `quality` argument** (`full` default, or `balanced`/`fast`): a capture with no `quality` is always `full`, whatever the editor tier. Pass `quality:"fast"` to preview what the human sees while editing,
  or to benchmark it (`perf_stats {frames:30, quality:"fast"}`). The editor tier itself only changes what the human sees live.
- **Never judge a look from a `fast` or `balanced` capture**: GI, reflections, god rays and DOF are missing, so everything looks flatter and darker. Judge at `quality:"full"` with `samples:16`. Use `fast` captures only for layout, speed and "will it run".
- If the human says "it looks washed out / flat in the viewport" but your full capture looks right, the editor is on `fast`; suggest `viewport_quality {quality:"full"}` when they want to see the final look live (at a frame-rate cost).

## Common mistakes (each seen in practice)

- **Monochrome orange**: warm sun + warm `fogColor` + `golden_hour` look at full strength + `sunset` preset stack on each other. Keep
  fog and sky cool or neutral, grade lightly (`lookStrength` 0.2-0.4).
- **Milky blacks** (night looks like fog): `fogDensity`, `haze`, `ambient` and auto exposure all lift the shadows. Lower them, check
  `debug_view:"lighting"`, and set `exposureCompensation` negative.
- **Blown neon**: point lights at intensity 40 over a big range plus bloom plus `godRays` white out the frame. Start lights around 8-25,
  `bloomThreshold` above 1, measure.
- **Too dark**: `intensity` below ~8 on a point light barely lights walls; raise, or raise `gi`.
- **A lamp went dark, or light still leaks through a wall**: point and spot lights cast shadows, so a lamp placed inside
  its own shade or behind a sign is blocked. Run `shadow_atlas_info {entity: "Lamp"}`: `casters` lists what blocks it,
  `ignoredFixtures` the small meshes within 0.3 m that are skipped, and `reason` says why a light has no shadow
  (`over_light_budget`: raise `localShadowLights` or turn off minor lights with `light_shadows`). Move the light out of
  the shade, or set `castShadows:false` on the blocking mesh. Look at the maps with `debug_view:"shadow_atlas"`.
- **Many lamps, slow frames**: shadows are cached while nothing in range moves; `shadowMaxDistance` (e.g. 40) frees
  distant lamps in big levels, and `particles.lightShadows` / `fluid.lightShadows` stay off unless a fire needs them.
- **Camera inside terrain** gives a flat orange/brown frame: check the camera height against the ground.
- **Judging at `samples:1`**: noise and missing GI will mislead you.
- **Grid and id labels in the "final" shot**: pass `overlays:false, annotate:false`.
- **Over-bloom**: bloom should hint at emissives and the sun, not blur the scene. Keep `bloomIntensity` under 0.8.
- **Fixing light with materials**: if everything is dull, check lighting first (`sunIntensity`, `exposure`, `ambient`), then albedo.
- **TAA ghosting on fast motion**: moving objects carry motion vectors (velocity buffer), so TAA, MetalFX and motion blur follow
  them. If something still smears, check `debug_view:"motion"` and `perf_stats` `gpu.velocity` (`movingDraws`,
  `skinnedWithPreviousPose`). Toon outlines and distant hair cards have no object motion yet.
- **A light that should only touch the hero**: don't fight it with intensity; name a layer and mask the light:
  `render_layers {action:"name", layer:2, name:"hero"}`, `render_layers {action:"set", entities:["Hero"], layers:["world","hero"]}`,
  `render_layers {action:"set", entities:["RimLight"], cull_mask:"hero"}`. Terrain, foliage, water, particles and hair are on layer 1.
- **Warm practicals**: use `temperature` (2700 tungsten, 1900 candle) instead of guessing orange hex colors; it keeps the light's
  brightness and stays physically plausible.
- **LUT path wrong**: `lut` must be a project-relative `.cube` file; apply with `lookStrength`.

## Materials for realism

Use presets (`material_create {path, preset}`: gold, silver, copper, chrome, brushed_steel, iron, plastic, rubber, ceramic, car_paint, glass,
water, ice, skin, wax, leaves, snow, velvet, neon, toon, toon_metal, clay; character models skin, eye, cloth, hair_card: subsurface skin with
`scatterRadius` mm and pores, refracted irises with `irisCenter`/`irisRadius`, cloth sheen, anisotropic hair cards; check with `debug_view:"sss_mask"`) and `texture_generate` (albedo + normal + ORM) with
`triplanar` so nothing stretches on scaled shapes. Real surfaces vary: add `normalMap`/`ormMap`, keep `roughness` between 0.3 and 0.9 for
most dielectrics, `metallic` 0 or 1 (rarely in between), `subsurface` for leaves/skin/wax/snow, `clearcoat` for paint and varnish.

## Quality gate: `scene_audit` (no placeholder shapes on camera)

Showcase-grade shots contain **no builtin primitives** (cube, sphere, capsule, cylinder, plane, cone, quad, torus), no
characters built from primitives, and no default grey or untextured surfaces. `scene_audit` checks exactly that for what a
camera sees (CPU, occlusion-aware: a ground plane hidden under terrain or water does not count):

```text
scene_audit {view:"scene", strict:true}                       # the game camera; strict = fail above 0.1% primitive coverage
scene_audit {camera:{eye:[0,1.7,6], target:[0,1.2,0], fov:40}}  # any view
scene_audit {sequence:"sequences/hero.sequence.json", times:[1,4,8], strict:true}   # film shots (12 spread shots by default)
```

It returns `pass`, `primitives` (entity, mesh, screen coverage %), `primitiveCharacters` (an animator / character controller /
nav agent on primitives, or a sphere head on a capsule body) with a `suggestion` (a kit character prefab when a kit is
mounted), `materials` (`default` / `untextured`), and `warnings` [{severity, code, message, hint, entity}]: missing texture,
mesh or HDRI files, big meshes without LODs, low texel density, a default sky, lights without shadows on hero meshes,
characters that cast no shadow. Fix every `error`, then the warnings that are visible in the image. `stylized:true` accepts flat
colors for a deliberate stylized look. For a whole showcase, `python3 media/demo/showcase_gate.py PROJECT` renders the 12-shot
contact sheet, audits every shot, benchmarks the hero views with `perf_stats` and prints PASS / FAIL.

## Verification checklist before reporting done

- Beauty shot at 16+ samples, `quality:"full"` (the default), overlays off, from the camera the game will use (`view:"scene"`).
- `scene_audit {view:"scene", strict:true}` passes (no visible primitives, primitive characters or default materials).
- At least one `debug_view` (`lighting` or `gi`) if you changed light terms.
- `perf_stats {frames:30, passes:true}` if you added many lights (all lights shade surfaces through clusters; the 16 most important also light
  water, particles and fog) or volumetrics: `profile.groups` shows which area got expensive (`main`, `shadows`, `ssgi`, `clouds`, `volumetrics`,
  `post` ...), `gpu.lights` counts layer-masked, negative and inverse-square lights, and `debug_view:"light_complexity"` shows where lights stack up
  (orange/red = 7+ lights per pixel).
- Report the final `environment_get` values so the look is reproducible.
