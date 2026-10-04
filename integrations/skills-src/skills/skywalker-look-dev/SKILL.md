---
name: skywalker-look-dev
description: Art-direct how a Skywalker scene looks - lighting, sky and atmosphere, GI/SSR/TAA, volumetric clouds and god rays, fog, auto exposure, tonemapping, color looks and LUTs, camera lens (depth of field, motion blur), materials - and judge renders with samples and debug views. Use for golden hour, overcast, night neon, interiors, "make it look AAA/cinematic", or when an image looks flat, washed out, blown out or orange.
---

# Look development

Load skywalker-core first. Look-dev is an **iterate-and-measure** loop: change a few fields, capture, judge against
the rubric below, change again. Do not set 30 fields at once: you cannot tell which one hurt.

## Where the controls live

| Layer | Tool | Fields |
|---|---|---|
| Scene lighting and post | `environment_update` (merge; `environment_get` reads back) | see the table below |
| Lights | `entity_create`/`entity_update` with `components.light` | `kind: directional\|point\|spot, color, intensity (0..1000), range (m), spotAngle`; shadows: `castShadows` (default true), `shadowResolution`, `shadowBias`, `shadowNormalBias`, `shadowMaxDistance`, `shadowMode: cube\|dual_paraboloid` |
| Light shadows | `light_shadows` (one light, a list or `"all"`), `shadow_atlas_info` | budgets in the environment: `localShadowLights` (16), `localShadowUpdates` (24 views/frame), `localShadowAtlas` (4096 px) |
| Camera lens | `components.camera` | `fov, aperture (f-stop, 0 = DOF off), focusDistance (m, 0 = autofocus), motionBlur (0..1, 0.5 film-like), primary` |
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
- **TAA ghosting on fast motion**: stills are fine; for motion tests compare with `taa:false`.
- **LUT path wrong**: `lut` must be a project-relative `.cube` file; apply with `lookStrength`.

## Materials for realism

Use presets (`material_create {path, preset}`: gold, silver, copper, chrome, brushed_steel, iron, plastic, rubber, ceramic, car_paint, glass,
water, ice, skin, wax, leaves, snow, velvet, neon, toon, toon_metal, clay) and `texture_generate` (albedo + normal + ORM) with
`triplanar` so nothing stretches on scaled shapes. Real surfaces vary: add `normalMap`/`ormMap`, keep `roughness` between 0.3 and 0.9 for
most dielectrics, `metallic` 0 or 1 (rarely in between), `subsurface` for leaves/skin/wax/snow, `clearcoat` for paint and varnish.

## Verification checklist before reporting done

- Beauty shot at 16+ samples, `quality:"full"` (the default), overlays off, from the camera the game will use (`view:"scene"`).
- At least one `debug_view` (`lighting` or `gi`) if you changed light terms.
- `perf_stats` if you added many lights (16 punctual lights are used per frame; directional first) or volumetrics.
- Report the final `environment_get` values so the look is reproducible.
