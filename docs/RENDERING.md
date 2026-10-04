# Rendering

Skywalker's renderer aims for two things at once:
- physically based images that read as real (metal, glass, skin, stone);
- strongly stylized ones (toon shading, outlines, neon, flat 2D).

Every setting is a reflected field, so agents and the editor set looks through the same
tools (`entity_update`, `material_create`, `environment_update`).

## Frame

Each frame renders one or more sub-samples:
- **Real time:** one jittered sample per frame, with temporal history.
- **Stills and cinematics:** N jittered sub-samples are accumulated, so captures are
  supersampled and GI and reflections come out noise-free (`viewport_capture` `samples`,
  CLI `--samples`).

| # | Pass | Notes |
|---|---|---|
| 0 | Environment | The sky (with volumetric clouds) is rendered into a 128² cubemap and GGX-prefiltered into 6 roughness mips. It is re-baked only when the sky changes. |
| 1 | Shadows | 4 sun cascades in a 4096² atlas. Uses bounding-sphere fit, texel snapping and rotated-Poisson PCF. Terrain, instanced foliage (alpha-tested), hair and mesh particles cast too. Meshes use a coarser LOD. |
| 2 | Clouds | Half resolution: ray-marched volumetric cloud layer, with temporal reprojection in real time. |
| 3 | Scene | 4× MSAA, memoryless (tile memory), with a jittered projection. Draws sky, opaque meshes (automatic LODs), CDLOD terrain, instanced foliage, strand hair, mesh particles, outlines, transparent meshes and the grid. Writes HDR color plus a G-buffer: albedo + material AO, and octahedral normal + roughness + metallic or a "no screen-space lighting" flag. Surfaces use clustered lighting. |
| 4 | SSAO, SSGI, SSR | Half resolution. GI uses cosine-sampled screen rays with a sky fallback. Reflections are GGX-importance-sampled. Both gather the previous anti-aliased frame (light keeps bouncing) and use temporal accumulation in real time. |
| 5 | Lighting resolve | Swaps the sky-probe indirect light of PBR surfaces for GI and reflections (bilateral upsample), and applies SSAO to indirect diffuse. |
| 6 | Effects | Fluid simulation (compute), FFT water, fluid volumes, GPU particles (compute, sorted) and CPU particles over the lit scene. |
| 7 | Volumetric light | Half resolution: shadow-mapped sun shafts and lamp cones through height-falling haze. |
| 8 | Temporal | Applies the volumetric light, then one of: TAA (Catmull-Rom history, YCoCg variance clipping, reactive mask for particles); accumulation of sub-samples; or a pass-through when MetalFX upscales. |
| 9 | Upscale | Used when `renderScale` < 1: camera motion vectors, then the MetalFX temporal scaler, from internal to output resolution. |
| 10 | Camera | Motion blur (camera motion), bokeh depth of field (thin-lens CoC, half-res gather) and auto exposure (center-weighted metering + adaptation). |
| 11 | Post | Bloom chain, then the composite: chromatic aberration, white balance, exposure, tonemap, saturation/contrast, look / 3D LUT, vignette, grain, contrast-adaptive sharpening, dithering. Debug views replace the image. |
| 12 | Overlays | Gizmos, drawn in LDR on top. |

### Lighting

- **Clustered forward lighting:** up to 1024 lights. The view is split into 16×9×24 clusters
  (built on the CPU, tested), and each pixel evaluates only nearby lights. Directional
  lights apply everywhere. The 16 most important lights also light water, particles, fluids
  and volumetric fog.
- **Global illumination** (`gi`, `giDistance`): bounce and emissive light from what is on
  screen, with the sky probe for rays that leave the screen. It works best with temporal
  accumulation (real time) or `samples` of 8 or more (stills).
- **Reflections** (`ssr`): glossy surfaces (wet streets, floors, metal, still water)
  reflect the scene. Rough surfaces fall back to the probe.
- **Cloud shadows:** drifting cloud shadows dim the sun on every surface.

### Sky, atmosphere and clouds

- **Atmosphere** (`skyMode: atmosphere`): Rayleigh and Mie single scattering plus a
  multiple-scattering term, so horizons are bright, not brown.
- **Volumetric clouds** (`clouds` = coverage, `cloudMode`, `cloudHeight`, `cloudThickness`,
  `cloudDensity`, `cloudScale`, `cloudSpeed`): a curved cloud layer shaped by GPU-generated
  Perlin-Worley noise. It drifts with `windDirection`, is lit with Beer–powder,
  multi-scattering and a dual-lobe phase, and appears in reflections. `cloudMode: flat` is
  the cheap painted layer.
- **Far sea:** the sea blends into the horizon, so the simulated grid never shows its edge.

### Terrain and foliage

See the `terrain_*` and `foliage_add` tools.
- **Terrain:** a heightfield up to 4097² with erosion-based generation and up to 8
  height-blended layers (photoscans or procedural, triplanar for cliffs). It has wet
  shorelines (`waterLevel`, `wetBand`) and a matching physics heightfield. Rendering uses
  CDLOD: a quadtree selects 32×32 patches displaced from the height texture, morphing
  between LODs with no cracks or popping.
- **Your own relief:** `terrain_create {"heightmap": "maps/continent.png"}` builds the
  terrain from a grayscale image (16-bit PNG for smooth slopes, 8-bit PNG or a square
  `.r16`). Values 0..1 map to `generator.minHeight..maxHeight`; row 0 is the −Z edge.
  `detailNoise` (meters) adds fBm detail; `erosion`/`thermal` still apply. The generator
  keeps the image path, so the gitignored `.terrain` cache is rebuilt from it.
- **Map overlay:** the terrain's `overlay` drapes one image over the whole terrain (row 0
  = −Z edge, column 0 = −X edge; alpha masks it) for political maps, region tints,
  borders or a parchment map. `overlayBlend`: `mix` paints over the ground (matte, the
  relief still shades it), `multiply` tints it, `glow` adds the color unlit.
  `overlayOpacity` fades it. Swap the image or the opacity at run time for map modes,
  e.g. `find("Continent").terrain.overlay = "maps/supply.png"` in Wander. List images
  that scripts pick at run time under `include` in `game.json`.
- **Foliage:** GPU-instanced and wind-animated, generated in chunks around the camera
  (about 256 instances per chunk) and thinned toward the cull distance. Multi-part models
  (trunk + alpha-cut leaves) share instances.
- **Levels of detail:** meshes of 3,000+ triangles (photoscans) get an automatic LOD chain
  from meshoptimizer: attribute-aware, with a sloppy fallback for card geometry. The level
  is chosen by on-screen error under one pixel. Leaf cards keep a readable canopy.

### Camera, grading and looks

- **Lens** (camera component, or `viewport_capture` `aperture` / `focus_distance`):
  `aperture` (f-stop; 0 = everything sharp), `focusDistance` (0 = autofocus on the center)
  and `motionBlur` (shutter fraction; a post-process blur in real time, a real accumulated shutter in
  movie renders, see [MOVIE_RENDER](MOVIE_RENDER.md)).
- **Exposure:** `autoExposure`, `exposureCompensation` (EV), `adaptationSpeed`. Manual
  `exposure` still multiplies.
- **Looks:** `look` is one of `warm`, `cool`, `teal_orange`, `golden_hour`, `bleach`,
  `noir`, `vivid`, `moonlight` or `vintage`. `lut` takes a `.cube` file (Resolve /
  Premiere / Unreal format) and overrides the look. `lookStrength` blends either.
- **Lens character:** `grain`, `chromaticAberration`, `vignette`, `sharpen`.

### Debug and film views (`viewport_capture`)

`debug_view` is one of:
- `albedo`, `normals`, `material` (roughness/metallic), `gi`, `reflections`, `ao`,
  `depth`, `lighting` (before GI);
- `sketch`: pencil contours and hatching.

`clay: true` renders every surface as matte white clay. Sketch, clay and final make
"sketch to fill" sequences.

### Performance

- `perf_stats` reports GPU frame time, triangles drawn, terrain nodes, foliage instances,
  draw calls and effect timings. `perf_stats {frames: 30}` benchmarks the current view in
  real time.
- `renderScale` 0.5–0.77 renders fewer pixels and lets MetalFX reconstruct full
  resolution (about 25% faster at 0.67 on an M1 Pro).
- Per-frame data uses a triple-buffered ring with a frames-in-flight semaphore. Static
  geometry, terrain and instance buffers are uploaded once and cached. Terrain and
  instance textures use unified memory on Apple silicon (no staging copies).

## Surfaces (`mesh` component and material assets)

| Field | Meaning |
|---|---|
| `color`, `metallic`, `roughness` | Metal/roughness PBR. GGX specular with height-correlated Smith visibility, Lambert diffuse, split-sum image-based lighting. |
| `emissive` | `[r, g, b, strength]`; strength can exceed 1 (HDR) and glows with bloom. |
| `texture` | Base color (sRGB). |
| `normalMap` | Tangent-space normals (OpenGL convention). The tangent frame comes from screen-space derivatives, so meshes need no tangents. |
| `ormMap` | glTF packing: R = occlusion, G = roughness, B = metallic. Each channel multiplies the scalar field. |
| `emissiveMap` | Multiplies `emissive`. |
| `tiling` | Texture repeats. With `triplanar`, it is repeats per 2 m of world space. |
| `triplanar` | World-space projection with whiteout normal blending. Textures never stretch on scaled cubes, walls or terrain. |
| `clearcoat` | A second glossy lobe on top (car paint, varnish, ceramics). |
| `subsurface` | Wrap diffuse plus back-light transmission (skin, leaves, wax, snow, ice). |
| `rim` | Stylized silhouette light. |
| `shading` | `pbr`, `toon` (banded light, crisp highlight, hemispheric fill) or `unlit`. |
| `outline` / `outlineColor` | Cartoon outline width in pixels: an inverted hull with constant screen-space width. |
| `doubleSided`, `castShadows` | Self-explanatory. |
| Specular anti-aliasing | Roughness is widened where normals vary within a pixel, so there is no sparkle on detailed normal maps. |

Color alpha below 1 makes a surface transparent: glass, water, ghosts, god rays. Such
surfaces are sorted back to front and don't cast shadows.

## Materials and textures for agents

- `material_create {path, preset?}` starts from a built-in preset and lets other fields
  override it. Presets: `gold`, `silver`, `copper`, `chrome`, `brushed_steel`, `iron`,
  `plastic`, `rubber`, `ceramic`, `car_paint`, `glass`, `water`, `ice`, `skin`, `wax`,
  `leaves`, `snow`, `velvet`, `neon`, `toon`, `toon_metal`, `clay`.
- `texture_generate {kind, name}` writes a seamless albedo, normal and ORM set. By default
  it also creates a triplanar material.
  - Kinds: `noise`, `marble`, `wood`, `planks`, `bricks`, `tiles`, `cobblestone`, `grass`,
    `dirt`, `sand`, `rock`, `metal_brushed`, `rust`, `fabric`, `checker`, `stripes`,
    `hexagons`, `scales`, `stylized`.
  - `color1`–`color3`, `scale`, `variation` and `bump` customize it.
  - Provenance (`generator: texgen`, kind, seed) is stored in the asset's `.meta`.
- glTF import extracts base color, normal, metallic-roughness (plus packed occlusion) and
  emissive maps into a material next to the mesh.

## Environment (`environment_update`)

| Group | Fields |
|---|---|
| Sky | `skyMode` is `gradient` (artist colors `skyTop` / `skyHorizon`), `atmosphere` (single-scattering Rayleigh + Mie driven by the sun) or `hdri` (a photographed `.hdr` panorama in `hdri`, with `hdriRotation`, `hdriIntensity`; `environment_update {align_sun_to_hdri: true}` points the sun at the photo's sun). Also `clouds` (procedural cover), `stars` (fade in as the sky darkens) and `sunSize`. |
| Wind | `windSpeed`, `windDirection`: carries smoke, rain, snow and every particle with `wind` > 0. |
| Sun | `sunAzimuth`, `sunElevation`, `sunColor`, `sunIntensity`. Shadows soften with `shadowSoftness`. |
| Ambient | `ambient` scales sky light from the environment map (lower it for interiors, caves and night). `reflections` scales the specular part. |
| Fog | `fogColor`, `fogDensity`. `fogHeight` > 0 pools fog near the ground (graveyards, swamps, valleys). Fog in-scatters warm light toward the sun. |
| AO | `ao` (strength), `aoRadius` (meters). |
| Camera / grade | `exposure`, `tonemap` (`aces`, `agx`, `neutral`, `filmic`, `none`), `temperature`, `tint`, `saturation`, `contrast`, `vignette`, `bloomIntensity`, `bloomThreshold`. |

## Effects

### Particles (`particles` component, `fx_create`)

Particles are simulated by the engine, not the GPU, so they are deterministic in play mode
(seeded per emitter) and agents can count and test them. They preview live while editing.
With `simulation: "gpu"` a particles component runs on compute shaders instead: millions of
particles, depth-buffer collisions, sub-emitters, ribbons, mesh particles and flipbooks (visuals
only) — see [HAIR_AND_VFX.md](HAIR_AND_VFX.md), which also covers strand hair and fur (`groom`).

| | |
|---|---|
| Forces | `gravity` (negative = buoyant hot gas), `drag` toward the moving air, environment `wind`, and `turbulence` / `turbulenceScale`: a divergence-free swirl field (sum of shear waves, i.e. cheap curl noise), so smoke curls instead of jittering. |
| Emission | `rate`, `burst` (once at start; Wander `burst(n)` / `fx_burst` for more), `shape` (`point`, `sphere`, `box`, `disc`, `cone`) with `shapeSize`, `direction`, `speed`, `spread`, `lifetime`, jitters, `prewarm` (start fully developed). |
| Collision | `collide` with a floor plane at `floorHeight`: die, `bounce`, or `splash` into droplets (rain). |
| Light | `light` > 0: the emitter casts a point light whose strength follows the live flame energy, so it flickers with the simulation. |
| Looks | `flame` (procedural fire tongues: tapering, swaying, noise-eroded, heat ramp from yellow core to red tips, partly opaque so overlaps keep their hue), `smoke` / `mist` (lit billowing volumes: sun with shadows and forward scattering, sky light, every point light), `glow` (embers, fireflies, magic), `spark` and `rain` (velocity-stretched streaks; rain glints under lamps), `snow`. All are soft particles that fade where they meet geometry. |

Presets: `fire`, `embers`, `smoke`, `steam`, `sparks`, `rain`, `snow`, `mist`, `spray`, `dust`,
`fireflies`, `magic`, `fireball`, `debris_smoke`, `shrapnel`; composites `campfire`, `torch`,
`burning_barrel`, `explosion`.

### Volumetric fluids (`fluid` component)

Fire and smoke as a real fluid simulation, the way offline tools (Blender Mantaflow,
Houdini Pyro) and Unreal's Niagara Fluids do it — on the GPU, in real time.

| | |
|---|---|
| Solver | Eulerian gas on a 3D grid (`resolution` cells along the longest side of `size`): second-order MacCormack advection with a limiter, combustion (`fuel` burns at `burnRate` into `heat` and `smoke`), buoyancy, vorticity confinement (`vorticity`: licking flames, curling smoke), source `turbulence`, environment wind, then a Jacobi pressure solve and projection (incompressible flow; solid floor, open sides and top). Prewarmed, so a fire starts already burning; `burst` gives explosions. |
| Rendering | Ray marched per pixel with jitter: flame emission from blackbody temperature (`flameTemperature`, Kelvin), smoke with sun light through a shadow march, a forward-scattering phase, sky light, every point light and the fire's own glow. Stops at opaque geometry. |
| Light | `light` > 0 casts a flickering point light from the fire onto the scene. |

Presets: `volume_fire`, `volume_torch`, `volume_smoke`, `steam_vent`, `explosion_volume`.
The `campfire`, `torch`, `burning_barrel` and `explosion` composites use them, with particle
embers or shrapnel on top.

### Water (`water` component, `fx_create` with `ocean`, `calm_sea`, `storm`, `lake`, `pool`)

| | |
|---|---|
| Simulation | Tessendorf FFT ocean: JONSWAP spectrum from `windSpeed` (300 km fetch), directional spreading around `windDirection`, finite-`depth` dispersion. Three 256² cascades (inverse FFTs run in parallel) (tiles of `patchSize`, /4.7, /19) cover swell to ripples without visible tiling. Choppy displacement (`choppiness`), slopes and the displacement Jacobian are computed with inverse FFTs on the CPU each frame. Replayable: a pure function of parameters, seed and time. |
| Surface | Endless (`size: 0`, a camera-centred grid out to the horizon) or a body of `size` meters around the entity with an organic, ragged shore (lakes, pools, puddles); the entity's y is the water level. |
| Shading | Refraction of the scene behind, absorption and in-scattering by depth (`clarity`, `shallowColor`, `deepColor`), animated caustics on everything underwater, screen-space reflections over the sky (sampled straight from the HDRI when there is one), GGX sun glints (`roughness`), light through thin crests, persistent whitecaps that linger and break into lace where waves fold, surf at the waterline (`foam`), reflections of every lamp and fire. |
| Gameplay | `water_query` (tool) and `water_height(x, z)` (Wander) return the real animated height, so boats and buoys ride the waves you see. |

### Recipes

| Look | Settings |
|---|---|
| Photoreal daylight | `skyMode: atmosphere`, `clouds: 0.4`, `tonemap: agx`, `ao: 1`, PBR materials from `texture_generate` (triplanar). |
| Horror night | Gradient sky near black, `stars: 0.6`, `fogDensity: 0.03`, `fogHeight: 0.4`, low `ambient`, warm point lights that flicker (Wander `self.light.intensity`), `vignette: 0.5`. |
| Cartoon | `shading: toon`, `outline: 2`–`3`, `rim: 0.5`, saturated colors, `tonemap: neutral`, `saturation: 1.2`. |
| Neon / synthwave | Unlit emissive strips (`emissive` strength 2–5), dark glossy floor (`roughness: 0.3`), `bloomIntensity: 0.8`. |
| Pixel-art 2D | `shading: unlit`, orthographic camera, `tonemap: none`, bloom off. |
| Golden-hour coast | `skyMode: hdri` with a sunset panorama + `align_sun_to_hdri`, `fx_create calm_sea`, `tonemap: agx`, a `campfire` on the beach. |
| Light shafts | `godRays: 1`, `haze: 0.01`–`0.03`, a low sun behind trees, pillars or canyon walls; lamps get visible cones in misty air. |
| Rainy neon street | Night `hdri` or gradient sky, `fx_create rain` 12 m up with `floorHeight` at the street, wet materials (`roughness` 0.1–0.2), emissive signs, `mist` at street level. |

## Limits and next steps

- Point and spot lights don't cast shadows yet. The sun casts four cascades; clouds and
  hair cast their own.
- GI and reflections are screen-space: what is off screen comes from the sky probe. There
  are no reflection probes or world-space GI yet.
- Transparent meshes don't refract (water does). Particles and fluid volumes render after
  transparent meshes.
- Fluid volumes don't cast shadows on the scene yet.
- Distant forests use mesh LODs. Impostors (billboard captures) are planned.
- Metal backend only; a Vulkan port is on the [roadmap](ROADMAP.md).
