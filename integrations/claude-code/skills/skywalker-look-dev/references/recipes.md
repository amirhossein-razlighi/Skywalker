# Look recipes

Each block is an `environment_update` call plus the supporting entities. Treat them as the first 80 percent: capture
(`viewport_capture {view:"scene", samples:16, overlays:false, annotate:false}`), judge with the rubric in SKILL.md,
and adjust. The golden-hour and night-neon numbers were tuned by capturing and comparing renders; the others are
sound starting points from the same method.

## Golden hour over land (tuned)

```text
environment_update {preset:"noon", skyMode:"atmosphere", sunElevation:14, sunAzimuth:215, sunColor:"#ffd9a8", sunIntensity:3.2,
  clouds:0.35, cloudMode:"volumetric", gi:0.5, ssr:0.2, taa:true, godRays:0.8, haze:0.012,
  fogColor:"#a9bbd6", fogDensity:0.0015, fogHeight:0.3,
  look:"golden_hour", lookStrength:0.25, autoExposure:true, exposureCompensation:-0.3, tonemap:"agx", ao:1, vignette:0.2}
```

- Set `sunAzimuth` so the sun is behind or beside what the camera looks at (the compass is 0 = +Z): backlit grass glows.
- Place the camera at eye height (1.6-2 m above `terrain_query` ground) with `fov` 35-45 for a photographic feel.
- The grass reads best with `foliage_add` `meadow_grass` + `flowers` and a low sun; check `debug_view:"gi"` if the ground is too dark.
- The `sunset` preset alone with warm `fogColor` and the full `golden_hour` look turns everything orange. Do not stack.

## Overcast / moody (starting point)

```text
environment_update {preset:"overcast", skyMode:"atmosphere", clouds:0.85, cloudMode:"volumetric", cloudDensity:1.3,
  sunIntensity:1.2, shadowSoftness:4, ao:1.2, aoRadius:1.0, gi:0.5, fogColor:"#9aa6b2", fogDensity:0.006, fogHeight:0.5,
  saturation:0.85, look:"bleach", lookStrength:0.25, tonemap:"agx", autoExposure:true}
```

Soft shadows and fog pooling in valleys carry the mood. Add depth with value layering: darker foreground, light haze in the distance.

## Night neon street (tuned)

```text
environment_update {preset:"night", skyMode:"gradient", skyTop:"#05060e", skyHorizon:"#1a1030", stars:0.2, ambient:0.15,
  gi:0.8, ssr:0.9, taa:true, fogColor:"#1a1233", fogDensity:0.006, fogHeight:0.5, godRays:0.8, haze:0.01,
  bloomIntensity:0.6, bloomThreshold:1.2, look:"teal_orange", lookStrength:0.3,
  autoExposure:true, exposureCompensation:-0.4, tonemap:"agx", vignette:0.3, chromaticAberration:0.12, grain:0.08}
entity_create {name:"Street", mesh:"plane", color:"#1a1c22", scale:[40,1,40], components:{mesh:{roughness:0.12}}}   # wet = low roughness
entity_create {name:"NeonPink", mesh:"cube", scale:[0.1,0.3,10], position:[-5.9,4,-2], components:{mesh:{color:"#ff2d95", emissive:"#ff2d95"}}}
entity_create {name:"PinkLight", components:{light:{kind:"point", color:"#ff2d95", intensity:22, range:18}, transform:{position:[-4.5,4,-2]}}}
fx_create {effect:"rain", position:[0,12,0]}        # emitter 12 m up, floorHeight at the street
fx_create {effect:"mist", position:[0,0.5,-4]}
```

- A glowing strip needs **both** an emissive mesh (what you see) and a point light (what it lights).
- Wet look = glossy floor + `ssr`. If reflections are missing, `debug_view:"reflections"`, then lower roughness or raise `ssr`.
- Intensity guide measured here: point lights at 8 barely lit the walls, at 40 blew out the frame; 20-25 over 18 m was right with bloom threshold 1.2.
- Fog and haze were the cause of milky blacks at first; reducing `fogDensity` from 0.012 to 0.006 restored depth.

## Interior (starting point)

```text
environment_update {preset:"studio", ambient:0.2, reflections:0.3, ao:1.3, aoRadius:0.8, gi:0.7, godRays:1, haze:0.02,
  sunElevation:35, sunAzimuth:120, sunIntensity:3, tonemap:"agx", autoExposure:true, adaptationSpeed:1.5}
entity_create {name:"Lamp", components:{light:{kind:"point", color:"#ffb36b", intensity:14, range:10}, transform:{position:[0,2.2,0]}}}
```

Windows are the key light: put the sun so it enters through them and let `godRays` show the shafts. Keep sky-derived `ambient` low so
the room is lit by the sun, bounce (`gi`) and lamps. Use warm lamps (2700 K feel, `#ffb36b`) against cool daylight for contrast.

## Midday clear (starting point)

```text
environment_update {preset:"noon", skyMode:"atmosphere", clouds:0.3, cloudMode:"volumetric", sunElevation:55, sunIntensity:2.6,
  gi:0.4, ao:1, tonemap:"agx", fogColor:"#b8cbe6", fogDensity:0.001, saturation:1.05}
```

High sun is the least flattering light: add clouds for shape and fog for distance, and keep shadows readable with `shadowSoftness` around 2.

## HDRI sky (photographed light)

```text
asset_download {url:"<.hdr url from the Poly Haven files API>", license:"CC0-1.0", author:"<from the API>", source_page:"https://polyhaven.com/a/<id>"}
environment_update {skyMode:"hdri", hdri:"downloads/sunset/sunset_2k.hdr", hdriIntensity:1, hdriRotation:0}
environment_update {align_sun_to_hdri:true}      # sun direction and shadows follow the photo's sun
```

HDRIs give believable reflections for free. Keep `hdriIntensity` near 1 and tune exposure instead.

## Camera lens cheat sheet

| Intent | fov | aperture | Notes |
|---|---|---|---|
| Landscape / establishing | 45-60 | 0 (off) or 11 | Everything sharp |
| Portrait / hero prop | 28-40 | 1.8-2.8 | `focusDistance` = `raycast` distance to the subject |
| Action / chase | 60-75 | 0 | `motionBlur:0.5` |
| Macro / miniature feel | 25 | 1.4 | Very shallow; keep `focusDistance` accurate |

Camera fields live on the camera component (`entity_update {entity:"Cam", components:{camera:{...}}}`). For a one-off look without
touching the scene camera, pass `eye`, `target`, `fov`, `aperture`, `focus_distance` to `viewport_capture`.

## Color grading

`look` is a quick grade; `lookStrength` blends it. For a studio look-up table use `lut:"looks/film.cube"` (a 3D `.cube` file inside the
project), then `lookStrength`. Prefer `temperature`/`tint` for white balance and `saturation`/`contrast` for restraint over heavy looks.
