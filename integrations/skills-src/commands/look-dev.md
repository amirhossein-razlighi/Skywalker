---
description: Art-direct the current scene to a target look (golden hour, overcast, night neon, interior...) with captured before/after
argument-hint: "<target look or mood, e.g. 'moody overcast forest'>"
---

Art-direct the current Skywalker scene toward: **$ARGUMENTS**

Load `skywalker-look-dev` (and `skywalker-world-building` if terrain or foliage need work). Procedure:

1. `scene_overview`, `environment_get`, then a **before** capture from the game camera: `viewport_capture {view:"scene", samples:16, overlays:false, annotate:false}`. Note what is wrong, using the rubric (exposure, depth, color, light direction, materials, focus).
2. Check the camera and subjects are above ground (`terrain_query` / `raycast`) so you are not judging a broken frame.
3. Start from the closest recipe in the skill (`references/recipes.md`) and apply it with one `environment_update`. Add or adjust key lights and materials only if the capture demands it.
4. Recapture. If something is off, diagnose with `debug_view` (`lighting`, `gi`, `albedo`, `material`, `ao`, `reflections`) and change **one group of fields** per iteration. Up to 5 iterations.
5. Check cost with `perf_stats` if you added lights, volumetrics or foliage.
6. Final: capture at `samples:16-32`; report before vs after, the final `environment_get` values and any light/material changes, and what you would do with more time. Do not save over the scene without the human's OK (`scene_save` to a new path if unsure).
