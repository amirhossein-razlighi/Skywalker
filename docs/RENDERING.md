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
| 8 | Velocity | The [velocity buffer](#velocity-buffer-motion-vectors): camera reprojection of the depth buffer plus the object motion the scene pass wrote. |
| 9 | Temporal | Applies the volumetric light, then one of: TAA (dilated velocity, Catmull-Rom history, YCoCg variance clipping, reactive mask for particles); accumulation of sub-samples; or a pass-through when MetalFX upscales. |
| 10 | Upscale | Used when `renderScale` < 1: the MetalFX temporal scaler, from internal to output resolution, fed the velocity buffer, the engine's exposure and the GPU-particle reactive mask. Textures get a mip bias of log2(`renderScale`) so they stay sharp. |
| 11 | Camera | Motion blur (camera and object motion, tile-max reconstruction), bokeh depth of field (thin-lens CoC, half-res gather) and auto exposure (center-weighted metering + adaptation). |
| 12 | Post | Bloom chain, then the composite: chromatic aberration, white balance, exposure, tonemap, saturation/contrast, look / 3D LUT, vignette, grain, contrast-adaptive sharpening, dithering. Debug views replace the image. |
| 13 | Overlays | Gizmos, drawn in LDR on top. |

**Frame pacing.** The simulation ticks at 60 Hz; real-time frames on faster displays show the world
between the last two ticks (render interpolation: transforms, skins, CPU particles and the effects
clock at the displayed time). `perf_stats.frameFlow` reports the interpolation alpha and frame pacing
jitter; the previous frame's model matrix per entity (for motion vectors) is in
`Engine::displayHistory()`. See docs/ARCHITECTURE.md "Render interpolation".

### Lighting

- **Clustered forward lighting:** up to 1024 lights (see [Lights](#lights-light-component) for their fields). The view is split into 16×9×24 clusters
  (built on the CPU, tested), and each pixel evaluates only nearby lights. Directional
  lights apply everywhere. The 16 most important lights also light water, particles, fluids
  and volumetric fog.
- **Global illumination** (`gi`, `giDistance`): bounce and emissive light from what is on
  screen, with the sky probe for rays that leave the screen. It works best with temporal
  accumulation (real time) or `samples` of 8 or more (stills).
- **Reflections** (`ssr`): glossy surfaces (wet streets, floors, metal, still water)
  reflect the scene. Rough surfaces fall back to the probe.
- **Cloud shadows:** drifting cloud shadows dim the sun on every surface.
- **Point and spot light shadows:** every lamp casts shadows from a cached shadow atlas
  (see [Point and spot light shadows](#point-and-spot-light-shadows)).

### Sky, atmosphere and clouds

- **Atmosphere** (`skyMode: atmosphere`): Rayleigh and Mie single scattering plus a
  multiple-scattering term, so horizons are bright, not brown.
- **Volumetric clouds** (`clouds` = coverage, `cloudMode`, `cloudHeight`, `cloudThickness`,
  `cloudDensity`, `cloudScale`, `cloudSpeed`): a curved cloud layer shaped by GPU-generated
  Perlin-Worley noise. It drifts with `windDirection`, is lit with Beer–powder,
  multi-scattering and a dual-lobe phase, and appears in reflections. `cloudMode: flat` is
  the cheap painted layer.
- **Clouds below the camera:** when the camera is above or inside the layer (a strategy map,
  a flight), the clouds are composited over the ground seen through them, not only over the
  sky. For map-scale scenes use a low layer and small features, e.g. `cloudHeight: 330`,
  `cloudThickness: 150`, `cloudScale: 0.1`.
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
  (trunk + alpha-cut leaves) share instances. A compute pass culls every instance and picks
  its level of detail; heavy models become octahedral impostors in the distance (see
  [Foliage impostors](#foliage-impostors)).
- **Levels of detail:** meshes of 3,000+ triangles (photoscans) get an automatic LOD chain
  from meshoptimizer: attribute-aware, with a sloppy fallback for card geometry. The level
  is chosen by on-screen error under one pixel (foliage allows ~3 px, per instance). Leaf
  cards keep a readable canopy.

### Camera, grading and looks

- **Lens** (camera component, or `viewport_capture` `aperture` / `focus_distance` / `tilt_shift`):
  `aperture` (f-stop; 0 = everything sharp), `focusDistance` (0 = autofocus on the center)
  and `motionBlur` (shutter fraction; a post-process blur of camera and object motion in real time, a real accumulated shutter in
  movie renders, see [MOVIE_RENDER](MOVIE_RENDER.md)). `tiltShift` (0..1) fakes a tilt-shift lens: a
  sharp band across the middle of the frame with blur growing above and below it, the "toy town"
  miniature look for aerial shots (a real aperture cannot blur a scene 50 m away that much). It
  shares the bokeh gather with `aperture` and combines with it.
- **Exposure:** `autoExposure`, `exposureCompensation` (EV), `adaptationSpeed`. Manual
  `exposure` still multiplies.
- **Looks:** `look` is one of `warm`, `cool`, `teal_orange`, `golden_hour`, `bleach`,
  `noir`, `vivid`, `moonlight` or `vintage`. `lut` takes a `.cube` file (the format
  DaVinci Resolve and Premiere export) and overrides the look. `lookStrength` blends either.
- **Lens character:** `grain`, `chromaticAberration`, `vignette`, `sharpen`.

### Debug and film views (`viewport_capture`, `viewport_debug_view`)

`debug_view` (captures, movie renders) and `viewport_debug_view {view}` (the live editor
viewport, which the human sees too) take one of these views. `viewport_debug_view {list: true}`
returns every view with its color legend. Unknown names fail with a did-you-mean hint.

| View | Shows |
|---|---|
| `albedo`, `normals`, `material`, `gi`, `reflections`, `ao`, `depth`, `lighting` | G-buffer and lighting buffers (`material`: roughness red, metallic green; `lighting`: before screen-space GI/reflections) |
| `unshaded` | albedo + emission, no lights, shadows or fog |
| `lighting_only` | the lighting on a white material: light placement, shadows and GI without textures |
| `emission` | emissive light only |
| `specular` | specular reflectance F0 × glossiness: dielectrics dark gray, metals their tint |
| `wireframe` | dark surfaces with every mesh and terrain triangle edge in cyan (depth-tested) |
| `overdraw` | fragments per pixel, no depth test (meshes, terrain, foliage, impostors): black 0, dark blue 1, blue 2, cyan 3, green 4, yellow 5–6, orange 7–9, red 10–15, white 16+ |
| `lod` | level of detail: green LOD0, yellow 1, orange 2, red 3, magenta 4+; foliage by its distance band's LOD, impostors purple, terrain by CDLOD node level |
| `uv_checker` | an 8×8 checker per UV0 tile tinted by U (red) and V (green): stretching, seams, flips; terrain shows one cell per texture repeat |
| `texel_density` | base-color texels per meter: blue < 128, cyan 256, green 512 (target), yellow 1024, red > 2048; gray = untextured |
| `shadow_cascades` | sun cascade per pixel: red 0 (nearest), green 1, blue 2, yellow 3, gray beyond the shadow distance |
| `light_complexity` | point/spot lights in each pixel's light cluster, same ramp as overdraw |
| `sketch` | pencil contours and hatching |
| `impostors` | the final image with foliage meshes tinted green and impostors magenta |
| `motion` | the velocity buffer over a dimmed gray image: hue = direction, strength = speed on a log scale (faint at 0.25 px, full at 15 px per frame). Capture with `samples: 1` right after something moved (a `sim_control` step) |
| `shadow_atlas` | the point and spot light shadow maps (4 quadrants), outlined per light: green re-rendered this frame, blue cached, orange waiting for the update budget (see [Point and spot light shadows](#point-and-spot-light-shadows)) |

Surface views (`unshaded` … `light_complexity`) replace each lit surface's color in the
shaders (`FrameUniforms.debug`, `shaders/Debug.metal`) and are shown without tonemapping,
so the legend colors are exact; the sky becomes a neutral backdrop. Water, particles and
fluids are not part of them.

`clay: true` renders every surface as matte white clay. Sketch, clay and final make
"sketch to fill" sequences.

### Scene audit (`scene_audit`): the quality gate before filming

`scene_audit` answers "is anything on camera a placeholder?" for any view: a custom `camera` {eye, target, fov}, several
`cameras`, a `camera_entity`, the `scene` or editor view, or a `sequence` at `times` (default: 12 shots spread over it).
It builds the frame the camera would render and rasterizes it on the CPU into a coarse depth buffer (320 px wide by
default, `resolution` up to 1280): every draw (skinned characters in their current pose), terrains and water. Coverage is
therefore occlusion-aware: a ground plane under terrain or water, or a cube behind a wall, does not count. Very heavy
meshes are rasterized from a coarser LOD. No GPU is used, so it runs in tests and headless CI.

| Finding | What it means | Severity |
|---|---|---|
| `primitives` | builtin cube, sphere, plane, cylinder, cone, quad, capsule, torus on screen, each with its entity and coverage % | `primitive_coverage` error when the total exceeds the limit (2%, 0.1% with `strict`, or `max_primitive_coverage`) |
| `primitiveCharacters` | an animator, character controller or nav agent on primitive meshes, or primitive parts arranged like a body (a sphere head resting on a capsule or cylinder torso, or parts named head / body / arm / leg) | error, with a kit character `suggestion` when a kit is mounted (`kit-character` prefabs) |
| `materials` | `default` (the grey default surface, no material) and `untextured` (a flat color: no base-color texture, not emissive, not unlit) | default: error above the limit; untextured: warning (`stylized: true` makes it info) |
| `missing_texture`, `missing_material`, `missing_mesh`, `missing_hdri` | files that do not load (e.g. downloads not fetched) | error |
| `missing_lods` | a visible mesh of 20,000+ triangles without an LOD chain | warning |
| `texelDensity` | base-color texels per meter on large visible surfaces: below 64 (blurry) or above 16,384 | warning for blurry |
| `default_sky` | the default gradient sky covers more than 2% of the image | warning |
| `lightsWithoutShadows` | a point or spot light reaching a hero mesh (3%+ of the image, or an animated character) without `castShadows` | warning |
| `proceduralMeshes` | builtin grass / rock / fern / flowers meshes on screen | warning above 1% |

The result has `pass` (no errors), `warnings` [{severity, code, message, hint, entity}] sorted by severity, and `stats`
(draws, visible draws, rasterized triangles, terrain / water / sky coverage). Several views also report the worst primitive
coverage and the union of primitive characters. Long lists keep their largest 40 entries (`primitivesTotal` says how many
there were). `media/demo/showcase_gate.py` runs it on every shot of a showcase (see DEVELOPMENT.md).

```text
scene_audit {"view": "scene", "strict": true}
scene_audit {"sequence": "sequences/hero.sequence.json", "times": [1.5, 4, 7.25], "strict": true}
scene_audit {"cameras": [{"eye": [0, 1.7, 6], "target": [0, 1.2, 0], "label": "close"}, {"eye": [0, 20, 40], "target": [0, 0, 0]}]}
```

### Performance

- `perf_stats {passes: true}` shows where the frame time goes (see
  [Profiling and debug views](#profiling-and-debug-views)).
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

## Profiling and debug views

**GPU passes.** Every render, compute and blit pass samples GPU timestamps at its stage
boundaries (`MTLCounterSampleBuffer`, `engine/platform/metal/MetalProfiler.mm`). The samples
of a frame are resolved when it completes and kept as rolling 60-frame statistics per pass
label. `perf_stats {passes: true}` returns them as `profile`:

- `passes`: `[{pass, group, ms, avgMs, minMs, maxMs, vertexMs, count, seen}]` in encode
  order. `ms` is the latest frame; `count` merges repeated encoders (bloom levels, still
  sub-samples); `seen` is the number of frames in the window the pass ran in (the sky
  bake runs once).
- `groups`: milliseconds per frame by area: `shadows`, `main`, `ao`, `ssgi`, `ssr`,
  `resolve`, `effects`, `volumetrics`, `clouds`, `temporal`, `upscale`, `post`, `foliage`,
  `particles`, `hair`, `skinning`, `environment`, `2d`, `ui`, `overlays`, `debug`.
- `cpu`: CPU scopes (`SKY_PROFILE_SCOPE`): `frame.build`, `scene.buildFrame`, `world.gather`
  (terrain and foliage chunks), `particles.gather`, `2d.gather`, `render.encode`,
  `render.readback` (waits for the GPU), `render.present`, `sim.step`.
- `spanMs` (first to last sample), `sumMs` (sum of `groups`), `frameGpuMs` (command-buffer
  time), `droppedPasses` (passes beyond the 4096-sample buffer; normally 0).

A render pass's time is its **fragment** span. Apple GPUs run the vertex work of later
passes early, overlapped with earlier fragment work, so vertex spans would overlap and
double count; they are reported separately as `vertexMs` (geometry-heavy passes: `Main`,
`Shadow cascades`). Fragment spans run in sequence, so `sumMs` ≈ `frameGpuMs`. Foliage and
terrain draw inside `Main` and `Shadow cascades`; their own passes are the culling compute
(`Foliage cull`) and the args clear. MetalFX upscaling is not sampled (it encodes its own
work). With `frames > 0` the timeline is reset first, so it covers exactly the benchmark.
In the editor, click the stats overlay's frame line to expand the same list.

**Debug views** are listed in [Debug and film views](#debug-and-film-views-viewport_capture-viewport_debug_view).

### Recipe: find and fix a slow frame

1. `perf_stats {"frames": 30, "passes": true, "view": "scene"}` — read `profile.groups`
   for the expensive area and `profile.passes` for the pass.
2. `main` or `shadows` high: look at `viewport_capture {"debug_view": "overdraw"}` (stacked
   transparent quads, dense grass) and `{"debug_view": "lod"}` (red/magenta near the
   camera is fine; green far away means the mesh has no LOD chain: chains are built
   automatically only for meshes of 3,000+ triangles, and the level is picked by on-screen
   error, so there is no per-mesh bias to tune; decimate the model or turn it into foliage
   with impostors).
3. Many lights: `{"debug_view": "light_complexity"}` — orange/red areas evaluate 7+ lights
   per pixel; shorten `range` or merge lights.
4. `ssgi`/`ssr`/`clouds`/`volumetrics` high: lower `gi`, `ssr`, cloud quality or `godRays`
   with `environment_update`, or benchmark `quality: "balanced"`.
5. Repeat step 1 and compare `profile.groups`.

Textures: `{"debug_view": "texel_density"}` should be mostly green (512 texels/m) near the
camera; `{"debug_view": "uv_checker"}` shows stretched or flipped UVs.

## Shader library and pipeline cache

- **Precompiled library.** When the offline Metal toolchain is installed
  (`xcodebuild -downloadComponent MetalToolchain`), the build compiles the standard shader
  library to `ShaderLibrary.metallib` (CMake option `SKY_PRECOMPILE_SHADERS`, on by
  default), embeds it, and the engine loads it with `newLibraryWithData` instead of
  compiling ~6k lines of MSL at startup. Without the toolchain the build prints a notice
  and the engine compiles the embedded source (the previous behavior).
  `SKY_SHADER_SOURCE=1` forces the source path; `shader_set` hot reloads always compile
  source.
- **Pipeline cache.** Every pipeline state goes through an `MTLBinaryArchive` at
  `~/Library/Caches/Skywalker/shaders/pipelines-<key>.binarchive`. The key hashes the shader
  source, engine version, GPU and OS build, so a changed shader never loads stale binaries;
  older archives are pruned (the 3 newest are kept). A hit skips the GPU back-end compile
  even when the system's own Metal cache was flushed (OS or Xcode update, a new build).
  `SKY_SHADER_CACHE=0` disables it, `SKY_SHADER_CACHE_DIR` moves it. It is off under Metal
  validation (`MTL_DEBUG_LAYER` / `MTL_SHADER_VALIDATION`): loading an archive through the
  GPU validation device crashes inside Metal.
- **Async pipelines.** `MetalPipelineCache::requestRender` / `requestCompute` and
  `compileShaderLibraryAsync` build on a background queue and return a handle to poll
  (`ready()`), so a renderer draws a fallback until a custom pipeline is ready. Surface
  shaders build on this.
- **Measure.** `engine_info` → `shaderCompileMs` and `rendererStartupMs`, and in detail
  `shaders`: `library` (`metallib` or `source`), `libraryMs`,
  `pipelinesMs`, `shaderCompileMs`, renderer `startupMs`, and `pipelineCache` (`hits`,
  `misses`, `loadMs`, `saveMs`, `bytes`, `path`). `SKY_SHADER_NONCE=<text>` appends a
  comment to the library source, which forces a cold compile for benchmarking.

Startup on an M1 Pro (`skywalker call engine_info`, release build, no Metal toolchain):

| Case | Renderer startup | Library compile | Pipelines (86) |
|---|---|---|---|
| Cold: first launch after a shader change | 2464 ms | 1201 ms | 1203 ms |
| Warm: system Metal cache and pipeline archive | 48–51 ms | 3 ms | 4 ms |

macOS keeps its own per-user Metal compiler cache, so warm launches were already fast before
the archive. The archive pays off when that cache misses while the archive hits (after an OS
or Xcode update, a cache purge, or a shipped game whose archive was pre-warmed); recording
it costs one ~0.3 s write on the cold launch. The cold library compile (1.2 s) is what the
precompiled `.metallib` removes.

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

Fire and smoke as a real fluid simulation on a voxel grid, the way offline tools (Blender
Mantaflow, Houdini Pyro) do it, but on the GPU and in real time.

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
| Shores | Over a terrain the waves shoal: their height falls with the depth of a low-passed (~16 m) seabed, keeps ~40% at the waterline so swash still runs up the beach face, and dies ~1 m above it. Hollows behind a beach crest therefore stay dry instead of flooding with each swell, and the shoreline follows the waves rather than the terrain contour. |
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
| Lamp-lit interior | Night sky, low `ambient`, one warm point light per lamp (shadows are on by default), `shadowResolution: 1024` on the hero lamp, `godRays: 0.5` with `haze: 0.02` so the window and door throw light shafts. |
| Rainy neon street | Night `hdri` or gradient sky, `fx_create rain` 12 m up with `floorHeight` at the street, wet materials (`roughness` 0.1–0.2), emissive signs, `mist` at street level. |

## Point and spot light shadows

Every point and spot light casts shadows by default (`light.castShadows`), so lamps, torches
and fires no longer shine through walls, floors and furniture. The sun keeps its own four
cascades; these shadows live in a separate **local shadow atlas**.

| Piece | What it does |
|---|---|
| Atlas | One depth texture of `Environment.localShadowAtlas` px (default 4096, 64 MB; 2048 = 16 MB, 8192 = 256 MB), split into 4 quadrants of 1, 4, 16 and 64 slots (2048 to 256 px for a 4096 atlas). |
| Slots | Each frame the lights that touch the view are ranked by size on screen and brightness. The most important get the biggest slots; a light keeps its slot while its size changes less than 2x. `shadowResolution` (px) overrides the automatic size. |
| Spot lights | One perspective view of the cone. Cones wider than 65 degrees use a cube instead. |
| Point lights | `shadowMode: cube` (default): six 90-degree views in a 3x2 grid of the slot, exact. `dual_paraboloid`: two hemispheres, 3x fewer views to update, but large flat polygons (a floor made of two triangles) are approximate, so use it for small props, not architecture. |
| Casters | Meshes (alpha-tested cut-outs too), terrain, foliage near the camera, hair and mesh particles. A small mesh around the light or within 0.3 m of it (a bulb, a lamp head with its glass, a sign box) does not shadow its own light. |
| Static cache | A slot re-renders only when its light moves or a caster in its range moves, changes mesh or animates. A street of static lamps costs nothing after its first frame. Hair and mesh particles in range re-render the light every frame. |
| Filtering | Rotated Poisson PCF with a new rotation every frame, so temporal anti-aliasing (or the samples of a still) smooths the penumbra. `Environment.shadowSoftness` widens it. |
| Receivers | Lit surfaces, terrain, foliage, impostors and hair; water reflections of lamps; lit particles and smoke; volumetric lamp cones in fog (`godRays`), which are cut by the same shadows. |

### Budgets (the GPU work stays bounded)

- `Environment.localShadowLights` (default 16, max 64): shadowed lights per frame. Lights
  beyond it still light the scene, without shadows. `0` turns local shadows off.
- `Environment.localShadowUpdates` (default 24): views re-rendered per frame (a cube light
  is 6, a spot 1, a paraboloid light 2). When more change at once, the rest keep last
  frame's shadow and update over the next frames. A light that has just appeared lights
  without a shadow until its first render. Stills (`samples` > 1) and movie frames render
  everything they need.
- The `fast` editor tier halves both budgets.

### Seeing it (agents)

- `shadow_atlas_info {view}` renders one real-time frame and reports every shadow-casting
  light: its projection, slot and face resolution, whether it re-rendered or came from the
  cache, and why it has no shadow (`disabled`, `out_of_view`, `beyond_max_distance`,
  `over_light_budget`, `atlas_full`, `pending`). It also reports the atlas memory, the slots
  used per quadrant, the budgets and warnings. With `entity`, it also lists that light's
  casters and the fixtures that are ignored. `invalidate: true` re-renders every cached
  shadow.
- `viewport_capture {debug_view: "shadow_atlas"}` shows the atlas: 4 quadrants with the
  distance from the light (white = near), outlined per light. Green outlines re-rendered
  this frame, blue ones came from the cache, orange ones wait for the update budget.
- `perf_stats` reports `gpu.localShadows`: shadowed lights, cached lights, views rendered,
  views deferred, CPU planning time and the GPU time of the last shadow pass.
- `light_shadows {lights, enabled, resolution, mode, bias, normal_bias, max_distance}`
  sets shadow fields on one light, a list, or `"all"` (optionally one `kind`) as one undo
  step.

### Light fields

| Field | Meaning |
|---|---|
| `castShadows` | Occluders block the light (default true). |
| `shadowBias` | Depth bias in meters (0.02). Raise it if lit surfaces show dark speckles (acne); lower it if shadows detach from their casters. |
| `shadowNormalBias` | Offset along the surface normal in shadow-map texels (1). Raise it against acne on surfaces lit at grazing angles. |
| `shadowResolution` | Slot hint in px: 256 for small lights, 1024 for a hero light, 0 = automatic. |
| `shadowMaxDistance` | Camera distance (m) where this light's shadow fades out, which frees atlas space in big levels. 0 = no limit. |
| `shadowMode` | Point lights: `cube` or `dual_paraboloid`. |

Effect lights opt in: `particles.lightShadows` and `fluid.lightShadows` (a torch in a cave,
a campfire in a room). They are off by default because a flickering emitter moves its light
every frame, so its shadow re-renders every frame.

### Recipe: lamps that respect walls

```
light_shadows {lights: "all", enabled: true}               # the default; turns it back on everywhere
shadow_atlas_info {view: "scene"}                           # who has a shadow, what was over budget
light_shadows {lights: ["Desk Lamp"], resolution: 1024}    # the hero light gets a sharp shadow
light_shadows {lights: "all", kind: "point", max_distance: 40}   # big open levels: distant lamps drop theirs
viewport_capture {debug_view: "shadow_atlas"}               # look at the maps themselves
```

If light still leaks, check `shadow_atlas_info {entity: "Lamp"}`. A wall missing from
`casters` either has `castShadows: false`, is transparent (`color` alpha < 0.5), or is a
small mesh right next to the light (it is then listed under `ignoredFixtures`). Move the
light 0.3 m away from it, or make the wall bigger. If the lamp's own shade blocks it, move
the light out of the shade.

### Cost (M1 Pro, 1280x720, `perf_stats {frames: 90}`, same binary with local shadows on and off)

| Scene | Off | On | Notes |
|---|---|---|---|
| `examples/render_tests/local_shadows/room` (1 cube light) | 4.6 ms | 5.2 ms | The first frame also renders 6 views (about 2 ms); after that the shadow comes from the cache. |
| `examples/hidden_alley` (9 lights, all shadowed) | 13.4 ms | 14.3 ms | |
| `examples/neon_requiem` (593 lights, 16 shadowed, god rays) | 15.5 ms | 17.5 ms | About 1 ms is the volumetric lamp cones (16 lights x 28 steps); the rest is the surfaces. |

Times are average GPU frame times; the static cache keeps the shadow pass itself at 0 ms
while nothing moves.

Reference scenes: `examples/render_tests/local_shadows/scenes/room.sky.json` (a closed room
with one window, a hanging bulb and furniture; nothing outside may be lit except through the
window) and `corridor.sky.json` (a stone corridor lit by a torch fire with `lightShadows`,
pillars, barrels and side rooms behind the walls). `python3 tools/render_checks/local_shadows.py`
renders them and checks for leaks.

## Foliage impostors

Imported trees, bushes and rocks are often 50k–3M triangles each. Drawn as meshes out to
a 1–2 km cull distance they cost billions of triangles. Beyond a per-layer **transition
distance**, instances draw as **octahedral impostors** instead: camera-facing cards that
read a pre-rendered atlas of the model seen from many directions, so a distant forest costs
a few triangles per tree.

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

## Velocity buffer (motion vectors)

Every frame knows how each pixel moved since the previous one. TAA, MetalFX temporal upscaling and motion blur use this, so moving
and animated things stay sharp instead of ghosting or smearing.

- **Object motion** is written by the scene pass into a fourth MSAA attachment: the screen offset between where a surface point is
  now and where it was a frame ago, measured in the previous frame's projection. Static geometry writes 0.
  - Meshes: the previous transform of every draw (`render/MotionHistory.h`, keyed by entity and mesh; a jump over 25 m counts as
    a teleport, not motion).
  - Skinned meshes: the posed vertices are double-buffered, so the vertex shader reads the previous pose too.
  - Foliage: wind sway is evaluated at the previous frame's time as well.
  - Strand hair: the interpolated strands are double-buffered. Mesh particles use their velocity.
- **Camera motion** is added afterwards from the depth buffer (sky pixels reproject by rotation only). Anything that doesn't write
  object motion is therefore treated as static, never as garbage.
- **TAA** reads the velocity of the nearest surface in a 3x3 neighborhood (dilation), so object edges reproject with the object.
- **MetalFX** gets the velocity buffer as its motion texture, the composite's exposure (`autoExposure` included) as its exposure
  texture, and the GPU-particle reactive mask.
- **Motion blur** (`Camera.motionBlur`, the shutter fraction) follows McGuire et al. 2012: the largest motion per ~40 px tile and per
  3x3 tile neighborhood, then a depth-aware gather along it. Moving objects blur over a sharp background; the blur length is capped
  at two tiles. Movie renders keep their real accumulated shutter ([MOVIE_RENDER](MOVIE_RENDER.md)).
- **Observe it:** `viewport_capture {debug_view: "motion"}` and `perf_stats` `gpu.velocity`: `movingDraws`, `trackedDraws`,
  `teleported`, `maxObjectMotionM`, `skinnedWithPreviousPose`, `mipBias`, `motionBlurTilePx`.
- **Check it:** `python3 tools/render_checks/velocity_and_layers.py` (macOS, opt-in) renders a fast textured cube and a swinging
  skinned strip under TAA and compares them to a 16-sample still. On an M1 Pro the mean error around the cube went from 7.7 to 5.2,
  and around the skinned strip from 13.3 to 7.7 (camera-only motion vectors before).
- **Cost** (M1 Pro, 1920x1080, `perf_stats {frames}`): about +0.2 to +0.4 ms per frame (the extra attachment and one full-screen
  pass): hello_sky 5.95 -> 6.35 ms, ashen_peaks 25.9 -> 26.1 ms.

## Render layers

Twenty render layers. A mesh is **on** layers; cameras and lights **see** layers through a cull mask. Bit `i` of a mask
is layer `i + 1`.

| Field | On | Default | Meaning |
|---|---|---|---|
| `layers` | `mesh` | 1 | Layers the mesh is on |
| `cullMask` | `camera` | all (1048575) | Layers the camera draws; meshes outside it are not drawn (and cast no shadow) |
| `cullMask` | `light` | all | Layers the light illuminates |

Terrain, foliage, water, particles and hair have no `layers` field yet and count as layer 1. Decals, reflection probes and render
targets will use the same two field names (`layers` for what something is on, `cullMask` for what it sees or affects).

Name layers in `game.json` and use the names everywhere:

```json
{"render": {"layers": {"1": "world", "2": "hero", "3": "fx", "20": "editor_only"}}}
```

- `render_layers {action: "name", layer: 2, name: "hero"}` writes the name.
- `render_layers {action: "set", entities: ["Hero"], layers: ["world", "hero"]}` puts meshes on layers.
- `render_layers {action: "set", entities: ["RimLight"], cull_mask: "hero"}` gives cameras and lights a mask.
- Masks accept a number (raw bits), a name, `"3"`, `"all"`, `"none"` or a list; misspelled names get a did-you-mean error.
  `mode: "add"` / `"remove"` edits the current mask.
- `render_layers {}` lists the named layers, meshes per layer, cameras and restricted lights, and warns about meshes no camera draws
  and lights that light nothing.
- Wander: `layer_mask("world", "fx")` returns the bits, e.g. `find("Mirror").camera.cullMask = layer_mask("world")`.

### Recipes

| Goal | Setup |
|---|---|
| Rim light only on the hero | Hero on `["world", "hero"]`; the rim light's `cull_mask: "hero"` |
| First-person arms not in the mirror | Arms on layer `"arms"`; the mirror camera's `cull_mask` excludes it |
| Editor helpers hidden in the game | Helpers on `"editor_only"` (20); the game camera's `cull_mask: ["world", "fx"]` |
| A UI-only 3D preview | Preview model on `"preview"`, lit by a light with `cull_mask: "preview"` only |

## Lights (`light` component)

| Field | Meaning |
|---|---|
| `kind`, `color`, `intensity`, `range`, `spotAngle` | Directional, point or spot; color and brightness; falloff distance; cone half-angle |
| `cullMask` | Render layers it lights ([Render layers](#render-layers)) |
| `temperature` | Kelvin (1900 candle, 2700 tungsten, 4000 fluorescent, 5500 noon, 6500 white, 9000 overcast sky) multiplied with `color`; 0 = off |
| `innerAngle` | Spot: half-angle of the full-intensity core in degrees (0 = automatic soft edge); close to `spotAngle` = hard edge |
| `specular` | Highlight strength; 0 = diffuse only (fill lights that shouldn't sparkle) |
| `volumetric` | Strength in volumetric light / god rays |
| `indirect` | Contribution to world-space GI (reserved until GI probes or lightmaps exist) |
| `negative` | Subtracts light: stylized darkening, fake occlusion |
| `attenuation`, `size` | `smooth` (default, soft, reaches 0 at `range`) or `inverse_square` (physical 1/d², peak capped by the emitter radius `size`, still windowed to `range`) |
| `distanceFade`, `fadeBegin`, `fadeLength` | Fade out with distance from the camera; faded-out lights are not sent to the GPU at all |

`perf_stats` reports `gpu.lights`: `total`, `layerMasked`, `negative`, `inverseSquare`. Point and spot light
shadows have their own fields (`castShadows`, `shadowBias`, ...; see
[Point and spot light shadows](#point-and-spot-light-shadows)). Not yet: bake modes (no
lightmaps yet) and caster masks.

## Limits and next steps

- Local light shadows use fixed-radius PCF (no contact-hardening penumbrae yet). Directional
  lights other than the sun don't cast shadows. Foliage casts local shadows only where it is
  on screen, and impostors don't cast them.
- Toon outlines and distant hair cards write no object motion (they reproject with the camera
  only).
- GI and reflections are screen-space: what is off screen comes from the sky probe. There
  are no reflection probes or world-space GI yet.
- Transparent meshes don't refract (water does). Particles and fluid volumes render after
  transparent meshes.
- Fluid volumes don't cast shadows on the scene yet.
- Impostors are static: they don't sway in the wind, and their lighting uses the model's
  mean roughness. Up close they can look slightly brighter than the meshes, which show
  more inner-canopy shadowing.
- Metal backend only; a Vulkan port is on the [roadmap](ROADMAP.md).
