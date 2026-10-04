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
- **Foliage:** GPU-instanced and wind-animated, generated in chunks around the camera
  (about 256 instances per chunk) and thinned toward the cull distance. Multi-part models
  (trunk + alpha-cut leaves) share instances. A compute pass culls every instance and picks
  its level of detail; heavy models become octahedral impostors in the distance (see
  [Foliage impostors](#foliage-impostors)).
- **Levels of detail:** meshes of 3,000+ triangles (photoscans) get an automatic LOD chain
  from meshoptimizer: attribute-aware, with a sloppy fallback for card geometry. The level
  is chosen by on-screen error under one pixel (foliage allows ~3 px, per instance). Leaf
  cards keep a readable canopy.

### Camera, grading and looks

- **Lens** (camera component, or `viewport_capture` `aperture` / `focus_distance`):
  `aperture` (f-stop; 0 = everything sharp), `focusDistance` (0 = autofocus on the center)
  and `motionBlur` (shutter fraction).
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
- `sketch`: pencil contours and hatching;
- `impostors`: the final image with foliage meshes tinted green and impostors magenta.

`clay: true` renders every surface as matte white clay. Sketch, clay and final make
"sketch to fill" sequences.

### Performance

- `perf_stats` reports GPU frame time, triangles drawn, terrain nodes, foliage instances,
  draw calls and effect timings. `perf_stats {frames: 30}` benchmarks the current view in
  real time; `view: {eye, target, fov}` benchmarks any camera and `quality` an editor tier.
  Foliage stats: `meshInstances`, `impostorInstances`, `foliageTriangles` (+ shadows),
  `foliageModels` (mesh triangles per model, its impostor and cull distances),
  `impostorsBaked` / `impostorsLoaded` / `impostorBakeMs` / `impostorMemoryMB`.
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

## Foliage impostors

Imported trees, bushes and rocks are often 50k–3M triangles each. Drawn as meshes out to
a 1–2 km cull distance they cost billions of triangles. Beyond a per-layer **transition
distance**, instances draw as **octahedral impostors** instead: camera-facing cards that
read a pre-rendered atlas of the model seen from many directions. This is the same
technique as Fortnite / Unreal's impostors and Horizon's distant vegetation.

**Bake** (GPU, lazy, cached):
- Every foliage layer whose model has 300+ triangles gets an impostor (`impostors: false`
  turns it off). A model is one mesh, or every part of a prefab (trunk, branches, leaves).
- Upright vegetation is captured on a **hemi-octahedral** grid of `impostorFrames`² views
  (default 12×12). Each view is an orthographic render of the bounding sphere, using
  `evaluateMaterial`, the same material code as the meshes (textures, ORM, normal maps,
  alpha test). The alpha test runs per MSAA sample, so leaf edges resolve to true coverage.
- Two RGBA8 atlases: albedo + coverage, and model-space normal (octahedral) + depth +
  subsurface. The model's mean roughness is stored alongside.
- CPU post-process (`render/Impostor.h`):
  - un-premultiply, then dilate colors and normals into the empty texels of each frame
    (no dark halos);
  - build a mip chain whose alpha is rescaled per frame so the alpha-tested area stays
    constant. Distant forests keep their density instead of thinning out.
- Cached in `.skywalker/cache/impostors/<key>.skyimp` (zlib).
  - The key hashes the parts: mesh keys, materials, part transforms, the source files'
    size and mtime, the atlas layout and the bake version.
  - Editing a mesh, texture or material rebakes it. Identical models share one atlas.
  - Caches are gitignored. They rebuild on demand.
- Bakes run in short command buffers: batches of views capped by triangle count, and
  watchdog-safe. In the live viewport, about 100 ms of baking or loading runs per frame.
  Until its impostor is ready, a layer draws only its mesh range.
  - `impostor_bake {entity, layer, rebake, preview}` bakes ahead of time and returns the
    atlas.

**Transition distance.** It is set automatically from on-screen size: where one atlas texel
covers about 1.5 screen pixels, about 0.08–0.1 of the screen height for a tree. It scales with
the resolution and lens.
- Override it per layer with `impostorDistance` (m), or `-1` to disable.
- `impostorResolution` (atlas px; auto 512–2048 by size) and `impostorFrames` trade memory
  against sharpness.
- The balanced and fast editor tiers move it to 0.75× and 0.5×.

**Rendering:**
- **GPU-driven culling.** One compute threadgroup per chunk tests every instance against
  the camera and the 4 sun cascades. It sorts instances into 4 distance bands (per-instance
  mesh LOD) and the impostor bin, and writes compact instance lists plus indirect draw
  arguments. The CPU issues indirect draws only for bands a chunk can touch.
  - Arguments are bounds-checked on the GPU and validated on the CPU when each frame
    completes.
  - `SKY_GPU_CULL=0` (or a failed validation) switches to an identical CPU path.
  - A triangle budget coarsens all foliage LODs if a frame would exceed 120M camera
    triangles (30M in safe mode after a GPU fault).
- **Impostor cards:**
  - Each card blends the 3 nearest views with barycentric weights over the view grid, so
    there is no popping between views.
  - Two depth-parallax steps per view put each pixel on the baked surface.
  - Cards write the G-buffer (normals, roughness, subsurface) and real depth (pixel depth
    offset), so sun and clustered lights, shadows, GI, SSAO, reflections and fog treat
    impostors as geometry.
  - A coarse-mip coverage test skips empty card pixels early.
- **Transition:** meshes and impostors crossfade over a band before the transition distance
  with a complementary per-pixel dither that changes every frame. TAA (or still
  accumulation) blends it, so nothing pops.
- **Shadows:**
  - Mesh LODs (one level coarser) near the camera.
  - Sun-facing impostor cards beyond the transition. They write depth reconstructed from
    the baked depth, so canopies self-shadow.
  - The far cascades (from the 3rd; from the 2nd in lower tiers) draw impostors only.
- **Leaf cards:** with impostors in place, alpha-tested parts may use LOD 2 at mid distance
  (LOD 1 without impostors).

**Measured on ashen_peaks** (M1 Pro, 1920×1080, full quality, 30-frame `perf_stats`):

| View | Before | After (full) | After (fast editing tier) |
|---|---|---|---|
| Aerial | 852 ms, 1.35 G triangles | 19 ms | 7 ms (before: 25 ms, most trees culled) |
| Mid-valley | 2022 ms, 3.9 G triangles | 39 ms, 7.7 M triangles | 10 ms |
| Ground | 2002 ms, 3.2 G triangles | 63 ms, 18 M triangles | 19 ms |

The same scene without any foliage costs about 13 ms (aerial) and 17 ms (ground). What
remains at ground level is mostly the jacaranda's 3.5M-triangle mesh near the camera and
dense alpha-tested grass. Assets with game-ready triangle counts (20–100k) fall well
within budget.

## Limits and next steps

- Point and spot lights don't cast shadows yet. The sun casts four cascades; clouds and
  hair cast their own.
- GI and reflections are screen-space: what is off screen comes from the sky probe. There
  are no reflection probes or world-space GI yet.
- Transparent meshes don't refract (water does). Particles and fluid volumes render after
  transparent meshes.
- Fluid volumes don't cast shadows on the scene yet.
- Impostors are static: they don't sway in the wind, and their lighting uses the model's
  mean roughness. Up close they can look slightly brighter than the meshes, which show
  more inner-canopy shadowing.
- Metal backend only; a Vulkan port is on the [roadmap](ROADMAP.md).
