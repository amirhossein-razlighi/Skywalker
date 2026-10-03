---
name: skywalker-vfx
description: "Visual effects in Skywalker - fire, smoke, explosions, rain, snow, sparks, magic, volumetric fluids and water via fx_create/fx_burst, tuning particle and fluid components, and (stub) hair and GPU particles. Use for any effect, ambience particle, muzzle flash, impact or weather."
---

> **STATUS: effects below are real and current; hair and GPU particles are a STUB.** The hair / GPU particle systems are under construction by another
> workstream. <!-- TODO(lead): after merging, add a section from docs/HAIR_AND_VFX.md: hair/fur components and tools, GPU particle emitters, budgets,
> recipes, verification; verify names against `skywalker tools --markdown`. -->

# Effects

Start from a preset, then tweak. `fx_create {effect*, name, position, parent, overrides}` creates an entity ready to run; `overrides` patches the
component fields (`{"rate":80,"colorStart":"#7af"}`). `fx_burst {entity, count}` emits particles now (explosions, muzzle flashes, impacts; works in edit
preview and play). In Wander: `burst(n)`, or `spawn("prefab:...")` an explosion prefab.

| Kind | Effects | Notes |
|---|---|---|
| Volumetric fire/smoke (GPU fluid, film quality) | `volume_fire`, `volume_torch`, `volume_smoke`, `steam_vent`, `explosion_volume` | Ray-marched, lights the scene itself, ideal for hero fire. |
| Composites | `campfire`, `torch`, `burning_barrel`, `explosion` (volumetric + embers/shrapnel + light); `sprite_campfire` is the cheap version | Place at the base of the object. |
| Particles | `fire`, `embers`, `smoke`, `steam`, `sparks`, `rain`, `snow`, `mist`, `spray`, `dust`, `fireflies`, `magic`, `fireball`, `debris_smoke`, `shrapnel` | Simulated by the engine, deterministic per emitter seed. |
| Water | `ocean`, `calm_sea`, `storm`, `lake`, `pool`, `puddle` | Entity y = water level; see skywalker-world-building. |

Rain and snow cover a box around the emitter: place it about 12 m above the area and set `floorHeight` to the ground. Wind
(`environment_update {windSpeed, windDirection}`) carries smoke, rain and particles with `wind > 0`.

Tuning fields (component `particles`): `rate, burst, shape (point|sphere|box|disc|cone), shapeSize, direction, speed, spread, lifetime, gravity
(negative = buoyant), drag, turbulence, turbulenceScale, collide (die|bounce|splash) with floorHeight, light (casts a flickering point light), look
(glow|flame|smoke|spark|rain|snow|mist), colorStart/colorEnd, sizeStart/sizeEnd, intensity, softness, wind, maxParticles, prewarm, seed`. For `fluid`: `size, resolution, burnRate, vorticity, flameTemperature (K),
turbulence, light, burst`. Always read the live list with `component_schema {component:"particles"}`.

## Workflow

1. `fx_create` the closest preset at the right spot (and scale: a campfire is ~1 m, a torch is small, an explosion is 5-15 m).
2. `viewport_capture` close up (`eye`/`target` ~4-8 m away, `samples:8`). Effects read best against dark or mid-tone backgrounds with `bloomIntensity` 0.4-0.7.
3. Tune a few fields; recapture. For motion, step the simulation (`sim_control step`) and capture at several ticks; effects animate in time.
4. Lights: fires illuminate their surroundings automatically; add a real point light only for stylized pools of light.
5. Cost: `perf_stats` after adding several volumetrics; prefer one volumetric hero fire plus particle sprites elsewhere. Keep gameplay readable: effects should
   communicate hits, pickups and danger without hiding the player.

## Pitfalls

- Burst effects look dead in a still capture: capture 5-20 ticks after the burst.
- Rain emitter at ground level does nothing useful; raise it and set `floorHeight`.
- Too many overlapping soft particles wash out the frame; lower `rate` or `intensity`, and raise `bloomThreshold`.
- Particles and volumes render after transparent meshes (glass in front of fire will not sort perfectly).
