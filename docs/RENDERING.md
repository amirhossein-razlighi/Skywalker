# Rendering

Skywalker's renderer aims for two things at once:
- physically based images that read as real (metal, glass, skin, stone);
- strongly stylized ones (toon shading, outlines, neon, flat 2D).

Every setting is a reflected field, so agents and the editor set looks through the same
tools (`entity_update`, `material_create`, `environment_update`).

## Frame

| # | Pass | Notes |
|---|---|---|
| 0 | Environment | The sky is rendered into a 128² cubemap and GGX-prefiltered into 6 roughness mips. Re-baked only when the sky, sun or fog changes. |
| 1 | Shadows | 4 sun cascades in a 4096² atlas: practical split scheme, bounding-sphere fit, texel snapping (no shimmering), rotated-Poisson PCF. |
| 2 | Scene | 4× MSAA HDR. Draws sky, opaque meshes, toon outlines, transparent meshes back to front, selection outline and grid. Writes **color**, **indirect light** and **depth**. |
| 3 | SSAO | Half resolution, 12 samples, normals reconstructed from depth, 4×4 blur. |
| 4 | Post | Bloom chain, then the composite: AO applied **to indirect light only**, white balance, exposure, tonemap, saturation/contrast, vignette, dithering. |
| 5 | Overlays | Gizmos, drawn in LDR on top. |

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
| Sky | `skyMode` is `gradient` (artist colors `skyTop` / `skyHorizon`) or `atmosphere` (single-scattering Rayleigh + Mie driven by the sun). Also `clouds` (procedural cover), `stars` (fade in as the sky darkens) and `sunSize`. |
| Sun | `sunAzimuth`, `sunElevation`, `sunColor`, `sunIntensity`. Shadows soften with `shadowSoftness`. |
| Ambient | `ambient` scales sky light from the environment map (lower it for interiors, caves and night). `reflections` scales the specular part. |
| Fog | `fogColor`, `fogDensity`. `fogHeight` > 0 pools fog near the ground (graveyards, swamps, valleys). Fog in-scatters warm light toward the sun. |
| AO | `ao` (strength), `aoRadius` (meters). |
| Camera / grade | `exposure`, `tonemap` (`aces`, `agx`, `neutral`, `filmic`, `none`), `temperature`, `tint`, `saturation`, `contrast`, `vignette`, `bloomIntensity`, `bloomThreshold`. |

### Recipes

| Look | Settings |
|---|---|
| Photoreal daylight | `skyMode: atmosphere`, `clouds: 0.4`, `tonemap: agx`, `ao: 1`, PBR materials from `texture_generate` (triplanar). |
| Horror night | Gradient sky near black, `stars: 0.6`, `fogDensity: 0.03`, `fogHeight: 0.4`, low `ambient`, warm point lights that flicker (Wander `self.light.intensity`), `vignette: 0.5`. |
| Cartoon | `shading: toon`, `outline: 2`–`3`, `rim: 0.5`, saturated colors, `tonemap: neutral`, `saturation: 1.2`. |
| Neon / synthwave | Unlit emissive strips (`emissive` strength 2–5), dark glossy floor (`roughness: 0.3`), `bloomIntensity: 0.8`. |
| Pixel-art 2D | `shading: unlit`, orthographic camera, `tonemap: none`, bloom off. |

## Limits and next steps

- 16 punctual lights per frame: directional lights first, then the point and spot lights
  most relevant to the view.
- Point and spot lights don't cast shadows yet.
- No screen-space reflections or GI (planned).
- Transparent surfaces don't refract.
- Metal backend only; a Vulkan port is on the [roadmap](ROADMAP.md).
