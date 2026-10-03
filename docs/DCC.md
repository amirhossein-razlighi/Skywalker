# Design-app bridge (DCC)

Skywalker's agents can work inside the professional design apps on your computer, then bring
the results back into the project: model a ruined tower in Blender and place it, convert a
downloaded FBX pack, decimate a photoscan, iterate on a mesh in a live Blender window while
watching it in the engine.

| App | Interface used | Status |
|---|---|---|
| **Blender** 3.2+ | `blender --background --python` (headless) and a live add-on | Fully implemented and covered by the test-suite (developed against 3.5.1) |
| **Maya** | `mayapy` (`maya.standalone`) | Adapter follows Autodesk's documented interface. **Untested** |
| **Houdini** | `hython` (`hou`) | Adapter follows SideFX's documented interface. **Untested** |
| **3ds Max** (Windows) | `3dsmaxbatch` (`pymxs`) | Adapter follows Autodesk's documented interface. **Untested** |

Everything funnels into glTF (`.glb`): Blender is the converter between the other formats and
the engine, so a Maya or Houdini script that writes FBX/OBJ still ends up as an imported asset
(`import: true` converts it with Blender when Blender is installed).

## Quick start

```
dcc_list                                  # what is installed and what it can do
dcc_generate {recipe: "tower", params: {ruin: 0.6}, name: "RuinedTower", place: {position: [10,0,4]}}
dcc_convert {path: "downloads/kenney_town", recursive: true}
dcc_edit_asset {asset: "downloads/scan/statue.glb", ops: [{op: "decimate", ratio: 0.25}]}
```

Nothing to configure if Blender lives in `/Applications`, `Program Files\Blender Foundation`,
`/usr/bin`, `/opt/blender*` or on `PATH`.

## Detection and configuration

Order of precedence (first working one wins; the others are listed as `alternatives`):

1. environment: `SKY_BLENDER`, `SKY_MAYAPY`, `SKY_HOUDINI_HYTHON`, `SKY_3DSMAX_BATCH`
2. `~/.skywalker/dcc/paths.json`, e.g. `{"blender": "/opt/blender-4.2/blender"}` (keys: `blender`, `maya`, `houdini`, `3dsmax`)
3. standard install folders per OS (macOS `/Applications/...`, Windows `Program Files`, Linux `/opt`, `/usr`, `~`), `$MAYA_LOCATION`, `$HFS`
4. `PATH`

When several versions are installed the newest is used. A broken override is reported in the
app's `note` and does not hide a working install. `dcc_list {refresh: true}` rescans; the
editor shows the same list under Settings → Design Apps.

## Tools

All tools live in the `dcc` category. Anything that runs an external program or script is
`openWorld` (MCP clients ask before running it) and the in-editor crew **asks by default**
(like `network`), unless the human changes the Cloudling's tool access.

| Tool | What it does |
|---|---|
| `dcc_list` | Detected apps, versions, capabilities, running jobs, the live session, procedural recipes |
| `dcc_run_script` | Run Python headless in an app; returns log, structured result, produced files; `import`/`place` |
| `dcc_convert` | FBX/OBJ/DAE/USD(Z)/3DS/PLY/STL/Alembic/.blend (file or folder) → `.glb` via Blender, then import |
| `dcc_export` | Export collections/objects of a `.blend` to `.glb` (collection-per-asset, real units), then import |
| `dcc_edit_asset` | Round-trip a project mesh through Blender (preset ops and/or script), re-import as a new version |
| `dcc_generate` | Procedural modeling recipes or your own script → `.glb` → import/place |
| `dcc_install_addon` | Install and enable the Blender add-on |
| `dcc_session_start` / `_stop` / `_status` | Start (windowless or with a window) / stop / inspect a live Blender session |
| `dcc_session_exec` | Run Python in the live session (REPL-like, persistent state, Ctrl+Z-able in a window) |
| `dcc_session_pull_selection` | Export Blender's selection to the project; same name again updates it in place |
| `dcc_session_send` | Import a project asset into the live session |
| `dcc_receive` | Entry point for the add-on's *Send Selection* button |
| `dcc_cancel` | Cancel a running job (ids are in `dcc_list`) |

Calls over the agent socket are asynchronous: the design app runs on the caller's connection
thread while the editor and other agents keep working. Only the quick, engine-touching steps
(argument checks, importing the produced files, placing entities) run on the main thread, and
placing is one undoable, attributed step (`agent:<name> — Place Tower`). Callers that are
in-process (the editor's own buttons, the CLI, `batch`) get the same result synchronously.

## How scripts run

`dcc_run_script` stages a private job folder, starts the app with an argv vector (never a
shell), and sets:

| Variable | Meaning |
|---|---|
| `SKY_PROJECT` | absolute project folder |
| `SKY_OUT` | output folder (default `dcc/<name>`); **new or changed files here are reported** and importable |
| `SKY_INPUT` | the `input` file, if any (Blender opens a `.blend` itself; Maya/Houdini scenes are opened by the bootstrap) |
| `SKY_ARGS` | JSON list of the `args` |
| `SKY_JOB`, `SKY_APP`, `SKY_TASK` | job folder, app id, task name |

Your script sees the helper library as `sky` (already imported):

```python
sky.PROJECT, sky.OUT, sky.INPUT, sky.ARGS
sky.out_path("tower.glb")        # path inside SKY_OUT (creates folders, refuses to escape it)
sky.project_path("assets/a.png") # path inside the project (read access to existing assets)
sky.log("text")                  # shows up in the tool result's `log`
sky.warn("text")                 # returned in `warnings`
sky.result(vertices=1234)        # structured values returned as `result` (written immediately)
```

Output is captured with a cap (head and tail kept), runs have a timeout (default 5 min, max
1 h) and can be cancelled; the whole process group is stopped (SIGTERM, then SIGKILL).
A Python exception or `sys.exit(n)` fails the call and the error contains the exception line
plus the app's log.

### Blender toolkit (`from skywalker_dcc import blender as B`)

| Function | |
|---|---|
| `reset_scene()` | empty scene, importers enabled |
| `import_file(path, scale=1, axis_forward=None, axis_up=None)` | FBX, OBJ, DAE, USD/USDA/USDC/USDZ, 3DS (Blender 3.6+), PLY, STL, ABC, GLB/GLTF, BLEND → list of new objects |
| `export_glb(path, objects=None, apply_modifiers=True, animations=False)` | self-contained GLB, Y-up, textures embedded |
| `export_fbx`, `export_obj` | for round trips to other tools |
| `stats(objects=None)` | triangles, vertices, materials, `size_m` (x, up, z) |
| `decimate(objs, ratio)`, `bevel(objs, width, segments, angle_deg)`, `merge_by_distance`, `triangulate`, `shade_smooth(objs, angle_deg)`, `smart_uv`, `apply_modifiers`, `join`, `recenter(objs, "bottom"\|"center")`, `bake_transforms` | mesh clean-up |
| `bake_ao(objs, samples, distance, target="vertex"\|"texture", size)` | Cycles ambient occlusion into vertex colors (the engine multiplies them into the albedo) or a texture |
| `make_material(name, color, roughness, metallic, emission, alpha, vertex_colors)` | Principled BSDF that glTF exports as PBR |
| `apply_ops([{"op": "decimate", "ratio": 0.3}, ...])` | the same presets `dcc_edit_asset`/`dcc_convert` accept |

Operator arguments are filtered against what the running Blender knows, so the helpers keep
working across Blender releases (3.2 to 4.x).

### Procedural recipes (`dcc_generate`, `skywalker_dcc.procedural`)

Deterministic (same `seed`, same model), in meters, standing on the ground with the origin at the
bottom center, PBR materials plus per-face color variation (stone courses, plaster, moss, soil):

| Recipe | Parameters (defaults) |
|---|---|
| `building` | floors 2, width 8, depth 6, floor_height 3, windows_per_side 3, roof gabled\|hip\|flat, wall_style plaster\|brick\|stone\|wood, door, chimney |
| `tower` | radius 3, height 12, sides 14, wall_thickness 0.9, course_height 0.5, battlements, **ruin 0–1**, door, window_slits, rubble |
| `wall` | length 8, height 2.5, thickness 0.6, course_height 0.4, ruin |
| `rock` | radius 1, roughness 0.45, flatten 0.75, detail 4, moss 0–1 |
| `stairs` | steps 10, width 1.4, rise 0.18, run 0.3, landing, stringers, style stone\|wood |
| `arch` | width 3 (clear opening), height 4, depth 1, thickness 0.55, segments 11 |
| `fence` | length 6, height 1.1, post_spacing 1.5, style picket\|rail |
| `column` | radius 0.35, height 4, flutes 12, base, capital |
| `barrel` | radius 0.38, height 0.95, staves 16, hoops 3 |
| `terrain_chunk` | size 32, resolution 40, height 3, roughness 0.5, flat_radius 0 |

All take `seed`. `procedural.describe()` lists them from inside Blender. In your own script:

```python
from skywalker_dcc import procedural as P
tower = P.generate("tower", height=14, ruin=0.6, seed=3)
wall  = P.generate("wall", length=9, ruin=0.4)
wall.location = (5, 0, 0)              # compose; everything in the scene is exported
```

## Agent recipes

### Convert a downloaded FBX pack and use it

```
asset_download {url: ".../kenney_town_kit.zip", license: "CC0-1.0", import: false, ...}
dcc_convert {path: "downloads/kenney_town_kit", recursive: true}
```

Every `.fbx`/`.obj`/`.dae`/... in the folder becomes a `.glb` next to its source (same folder
structure, materials and textures embedded), is imported with its real-world size and tagged
`dcc, blender`, and its `.meta` records the converter. Re-run with `skip_existing: true` to
resume a large batch; set `scale: 0.01` for a pack authored in centimeters that the importer
did not already convert, `recenter: "bottom"` to put origins at the feet, and `ops` for
cleanup while converting. `place: {position: [0,0,0], spacing: 4}` puts every converted model into
the scene in a row along x (or place chosen ones later with `asset_list` + `entity_create`).

### Model a ruined tower in Blender and place it

```
dcc_generate {recipe: "tower",
              params: {height: 14, radius: 3.2, ruin: 0.6, seed: 3},
              name: "RuinedTower",
              ops: [{op: "shade_smooth", angle: 30}],
              place: {position: [12, 0, -4]}}
viewport_capture {annotate: true}          # look at it, then iterate on the params
```

For something the recipes cannot do, write the model yourself:

```
dcc_run_script {name: "gatehouse", import: true, place: {name: "Gatehouse", position: [0,0,0]},
  script: "
import bpy
from skywalker_dcc import blender as B, procedural as P
B.reset_scene()
tower = P.generate('tower', height=9, radius=2.2, ruin=0.2, seed=5)
wall = P.generate('wall', length=6, height=4, thickness=1.0, ruin=0.1)
wall.location = (3.2, 0, 0)
B.export_glb(sky.out_path('gatehouse.glb'))
sky.result(**B.stats())
"}
```

### Decimate an imported scan

```
dcc_edit_asset {asset: "downloads/scan/statue.glb",
                ops: [{op: "merge_by_distance"}, {op: "decimate", ratio: 0.2}, {op: "shade_smooth", angle: 35},
                      {op: "bake_ao", target: "vertex", samples: 24}],
                update_references: true}
```

The original stays untouched; `statue_v2.glb` records `derivedFrom`, `ops` and the version in its
`.meta`, `update_references` re-points scene entities (prefab instances of multi-material models
keep using the old file), and the result lists `before`/`after` triangle counts. Use
`mode: "replace"` to overwrite a `.glb` in place so everything updates live (a backup goes to
`.skywalker/dcc-backups/`).

### Work in a live Blender window

```
dcc_install_addon {}                       # once; then in Blender: N panel -> Skywalker -> Start Bridge
dcc_session_start {headless: false, open: "props/lantern.glb"}   # or let the human click Start Bridge
dcc_session_exec {code: "bpy.ops.object.modifier_add(type='SUBSURF')"}
dcc_session_pull_selection {name: "Lantern"}      # first time: imports + places
dcc_session_exec {code: "bpy.context.object.modifiers['Subdivision'].levels = 2"}
dcc_session_pull_selection {name: "Lantern"}      # again: updates in place, the entity refreshes live
```

The human can also click **Send Selection to Skywalker** in Blender's Skywalker tab, or right-click a
model in the editor's Asset Browser → **Open in Blender**.

## The Blender add-on

`integrations/blender/skywalker_bridge/` (embedded in the engine; `dcc_install_addon` copies it
into Blender's user scripts folder and enables it without touching your other preferences).

* **N panel → Skywalker**: Start/Stop Bridge, *Send Selection to Skywalker*.
* **Preferences**: start the bridge automatically, override the editor socket path.
* **Server**: newline-delimited JSON over TCP on `127.0.0.1` (random port). Every request carries
  a random token from `~/.skywalker/dcc/blender.json` (mode 0600), compared in constant time.
  Methods: `ping`, `status`, `exec`, `selection`, `export_selection`, `import_file`, `save_copy`, `shutdown`.
* **Threading**: sockets live on helper threads, every request that touches `bpy` runs on Blender's
  main thread (a `bpy.app.timers` callback in the UI, a pump loop in headless sessions). A request
  that waits too long is abandoned and never runs late.
* **Security**: `exec` runs arbitrary Python in Blender, by design; starting the bridge is an explicit
  user action, and only processes of the same user can read the token. One session per user
  (`blender.json` holds the most recent).
* **Headless sessions** (`dcc_session_start`, default) end when the engine exits or stops them; a
  window you opened stays open when the bridge stops.

## Safety model

* no shell anywhere: executable + argv, SKY_* environment, working directory = the output folder;
* outputs only inside the project (`out_dir`/inputs are checked, `sky.out_path` refuses `..`);
* `.blend` files are opened with auto-run scripts disabled; headless runs use factory settings
  unless `user_prefs: true`;
* captured output is capped; every run has a timeout; cancellation kills the process group;
* the `dcc` category asks the human by default for in-editor Cloudlings; MCP clients see
  `openWorldHint` on every tool that runs code.

## Provenance

Imported assets get `tags: ["dcc", "<app>"]` and `source` in their `.meta`:
`generator` (`blender 3.5.1`), `tool`, `by` (the actor), `retrieved`, plus tool-specific fields
(`recipe` + `params`, `scriptSha`, `derivedFrom` + `derivedFromGuid` + `ops` + `version`, `blend`, `live`).

## Maya, Houdini, 3ds Max

Detection, command lines and the helper modules (`skywalker_dcc.maya`, `.houdini`, `.max3ds`:
`export_fbx`, `export_obj`, geometry export) follow the vendors' documentation but have **not**
been run against the real applications. The pieces are covered by tests where possible (detection
on fake installs for macOS/Windows/Linux, argv construction). To try one:

```
export SKY_MAYAPY=/Applications/Autodesk/maya2025/Maya.app/Contents/bin/mayapy
dcc_run_script {app: "maya", script: "from skywalker_dcc import maya as M\nM.export_fbx(sky.out_path('crate.fbx'))", import: true}
```

`import: true` converts the FBX through Blender. Report problems with the exact app version.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `app_not_found` | install Blender or set `SKY_BLENDER`; `dcc_list {refresh: true}` |
| `dcc_failed ... ValueError: ...` | the error line is the script's; the full app log follows in the hint |
| `no_session` | `dcc_session_start`, or `dcc_install_addon` + *Start Bridge* in Blender |
| `timeout` on a session call | Blender is busy or showing a modal dialog; keep `exec` calls short |
| 3DS import fails | Blender 3.5 ships no 3DS importer (3.6+ does) |
| Import is 100x too big/small | `scale` (0.01/100) on `dcc_convert`, or `normalize: true` to fit 1 m |

## Limitations

* Design apps run off the main thread for agents on the socket, the editor's crew and its buttons
  (the C API's `sky_call_tool_begin` / `sky_pending_run` / `sky_pending_finish`, Swift
  `EngineStore.callAsync`). In-process synchronous callers (the CLI, a `batch` containing a `dcc_*`
  tool, tests) wait for the app to finish.
* Files a script writes outside `SKY_OUT` are not reported or imported (use `sky.out_path`).
* Windows process execution is not implemented yet (detection and command lines are).
* Textures: glTF export embeds images; procedural node textures are not baked unless you bake them.
* One live session per user; Alembic/skeletal animation and shape keys are not carried through `dcc_edit_asset`.
