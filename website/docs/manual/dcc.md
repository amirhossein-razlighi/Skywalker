# DCC bridge

The DCC bridge lets Skywalker and its agents work inside the 3D applications installed on your computer and bring the
results back into the project: model a ruined tower in Blender and place it, convert a downloaded FBX pack, decimate a
photoscan, or iterate on a mesh in a live Blender window while you watch it in the engine. Everything ends up as glTF
(`.glb`) assets with recorded provenance.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/dcc-live-session.webp · A Blender window with the Skywalker tab open in the N panel (Start Bridge, Send Selection to Skywalker) beside the Skywalker editor showing the same lantern model placed in the scene</div>

## Concepts

### Supported apps

| App | Interface used | Status |
|---|---|---|
| **Blender** 3.2+ | `blender --background --python` (headless) and a live add-on | Fully implemented and covered by the test suite (developed against 3.5.1) |
| **Maya** | `mayapy` (`maya.standalone`) | Adapter follows Autodesk's documented interface. **Untested** |
| **Houdini** | `hython` (`hou`) | Adapter follows SideFX's documented interface. **Untested** |
| **3ds Max** (Windows) | `3dsmaxbatch` (`pymxs`) | Adapter follows Autodesk's documented interface. **Untested** |

Blender is the converter between other formats and the engine: a Maya or Houdini script that writes FBX or OBJ still
ends up as an imported asset, because `import: true` converts it through Blender when Blender is installed.

### Detection and configuration

Nothing needs configuring if Blender is in `/Applications`, `Program Files\Blender Foundation`, `/usr/bin`,
`/opt/blender*` or on `PATH`. Otherwise, the bridge looks in this order (the first working install wins; the others are
listed as `alternatives`):

1. Environment variables: `SKY_BLENDER`, `SKY_MAYAPY`, `SKY_HOUDINI_HYTHON`, `SKY_3DSMAX_BATCH`.
2. `~/.skywalker/dcc/paths.json`, for example `{"blender": "/opt/blender-4.2/blender"}` (keys `blender`, `maya`,
   `houdini`, `3dsmax`).
3. Standard install folders per operating system (`/Applications/...` on macOS, `Program Files` on Windows, `/opt`,
   `/usr` and `~` on Linux), plus `$MAYA_LOCATION` and `$HFS`.
4. `PATH`.

When several versions are installed, the newest is used. A broken override is reported in the app's `note` and does not
hide a working install. `dcc_list` with `refresh: true` rescans; the editor shows the same list under
**Settings → Design Apps**.

### Tools

All bridge tools are in the `dcc` category. Tools that run an external program or script are marked open-world, so MCP
clients ask before running them, and the in-editor crew asks by default unless you change the agent's permissions.

| Tool | What it does |
|---|---|
| `dcc_list` | Detected apps, versions, capabilities, running jobs, the live session and the procedural recipes |
| `dcc_run_script` | Run Python headless in an app; returns the log, a structured result and the produced files; optional `import` and `place` |
| `dcc_convert` | FBX, OBJ, DAE, USD(Z), 3DS, PLY, STL, Alembic or `.blend` (a file or a folder) → `.glb` through Blender, then import |
| `dcc_export` | Export collections or objects of a `.blend` to `.glb` (one asset per collection, real units), then import |
| `dcc_edit_asset` | Round-trip a project mesh through Blender (preset ops and/or a script) and re-import it as a new version |
| `dcc_generate` | A procedural modeling recipe or your own script → `.glb` → import and place |
| `dcc_install_addon` | Install and enable the Blender add-on |
| `dcc_session_start`, `dcc_session_stop`, `dcc_session_status` | Start (windowless or with a window), stop or inspect a live Blender session |
| `dcc_session_exec` | Run Python in the live session (REPL-like, persistent state, one undo step in a window) |
| `dcc_session_pull_selection` | Export Blender's selection to the project; the same name again updates the asset in place |
| `dcc_session_send` | Import a project asset into the live session |
| `dcc_receive` | The entry point for the add-on's **Send Selection** button |
| `dcc_cancel` | Cancel a running job (ids are in `dcc_list`) |

Calls over the agent socket are asynchronous: the design app runs on the caller's connection thread while the editor and
other agents keep working. Only the quick, engine-touching steps (checking arguments, importing the produced files,
placing entities) run on the main thread, and placing is one undoable, attributed step. In-process callers (the editor's
own buttons, the CLI, `batch`) get the same result synchronously.

## How scripts run

`dcc_run_script` stages a private job folder, starts the app with an argument vector (never a shell) and sets:

| Variable | Meaning |
|---|---|
| `SKY_PROJECT` | The absolute project folder |
| `SKY_OUT` | The output folder (default `dcc/<name>`). New or changed files here are reported and can be imported. |
| `SKY_INPUT` | The `input` file, if any. Blender opens a `.blend` itself; Maya and Houdini scenes are opened by the bootstrap. |
| `SKY_ARGS` | The `args` as a JSON list |
| `SKY_JOB`, `SKY_APP`, `SKY_TASK` | The job folder, the app id and the task name |

Inside the app, the helper library is already imported as `sky`:

```python
sky.PROJECT, sky.OUT, sky.INPUT, sky.ARGS
sky.out_path("tower.glb")         # a path inside SKY_OUT (creates folders, refuses to escape it)
sky.project_path("assets/a.png")  # a path inside the project (read access to existing assets)
sky.log("text")                   # appears in the tool result's `log`
sky.warn("text")                  # returned in `warnings`
sky.result(vertices=1234)         # structured values returned as `result` (written immediately)
```

Output is captured with a cap (head and tail kept). Runs have a timeout (default 5 minutes, at most 1 hour) and can be
cancelled; the whole process group is stopped (SIGTERM, then SIGKILL). A Python exception or `sys.exit(n)` fails the call,
and the error contains the exception line plus the app's log.

### The Blender toolkit

`from skywalker_dcc import blender as B` gives scripts a set of helpers:

| Function | Purpose |
|---|---|
| `reset_scene()` | An empty scene with the importers enabled |
| `import_file(path, scale=1, axis_forward=None, axis_up=None)` | FBX, OBJ, DAE, USD/USDA/USDC/USDZ, 3DS (Blender 3.6+), PLY, STL, ABC, GLB/GLTF, BLEND → the list of new objects |
| `export_glb(path, objects=None, apply_modifiers=True, animations=False)` | A self-contained GLB, Y-up, textures embedded |
| `export_fbx`, `export_obj` | For round trips to other tools |
| `stats(objects=None)` | Triangles, vertices, materials and `size_m` (x, up, z) |
| `decimate(objs, ratio)`, `bevel(objs, width, segments, angle_deg)`, `merge_by_distance`, `triangulate`, `shade_smooth(objs, angle_deg)`, `smart_uv`, `apply_modifiers`, `join`, `recenter(objs, "bottom" or "center")`, `bake_transforms` | Mesh clean-up |
| `bake_ao(objs, samples, distance, target="vertex" or "texture", size)` | Cycles ambient occlusion into vertex colors (the engine multiplies them into the albedo) or into a texture |
| `make_material(name, color, roughness, metallic, emission, alpha, vertex_colors)` | A Principled BSDF that glTF exports as PBR |
| `apply_ops([{"op": "decimate", "ratio": 0.3}, ...])` | The same presets `dcc_edit_asset` and `dcc_convert` accept |

Operator arguments are filtered against what the running Blender knows, so the helpers keep working across Blender
releases from 3.2 to 4.x.

### Procedural recipes

`dcc_generate` and `skywalker_dcc.procedural` build models from recipes. They are deterministic (same `seed`, same
model), sized in meters, standing on the ground with the origin at the bottom center, with PBR materials and per-face
color variation (stone courses, plaster, moss, soil).

| Recipe | Parameters (defaults) |
|---|---|
| `building` | `floors` 2, `width` 8, `depth` 6, `floor_height` 3, `windows_per_side` 3, `roof` gabled, hip or flat, `wall_style` plaster, brick, stone or wood, `door`, `chimney` |
| `tower` | `radius` 3, `height` 12, `sides` 14, `wall_thickness` 0.9, `course_height` 0.5, `battlements`, `ruin` 0–1, `door`, `window_slits`, `rubble` |
| `wall` | `length` 8, `height` 2.5, `thickness` 0.6, `course_height` 0.4, `ruin` |
| `rock` | `radius` 1, `roughness` 0.45, `flatten` 0.75, `detail` 4, `moss` 0–1 |
| `stairs` | `steps` 10, `width` 1.4, `rise` 0.18, `run` 0.3, `landing`, `stringers`, `style` stone or wood |
| `arch` | `width` 3 (clear opening), `height` 4, `depth` 1, `thickness` 0.55, `segments` 11 |
| `fence` | `length` 6, `height` 1.1, `post_spacing` 1.5, `style` picket or rail |
| `column` | `radius` 0.35, `height` 4, `flutes` 12, `base`, `capital` |
| `barrel` | `radius` 0.38, `height` 0.95, `staves` 16, `hoops` 3 |
| `terrain_chunk` | `size` 32, `resolution` 40, `height` 3, `roughness` 0.5, `flat_radius` 0 |

Every recipe also takes `seed`. Inside Blender, `procedural.describe()` lists them. Compose recipes in your own script:

```python
from skywalker_dcc import procedural as P
tower = P.generate("tower", height=14, ruin=0.6, seed=3)
wall = P.generate("wall", length=9, ruin=0.4)
wall.location = (5, 0, 0)              # everything in the scene is exported
```

## Recipes

### Convert a downloaded FBX pack

```tool
asset_download {"url": "https://example.com/packs/town_kit.zip", "license": "CC0-1.0", "author": "Example Studio", "import": false}
dcc_convert {"path": "downloads/town_kit", "recursive": true, "recenter": "bottom", "place": {"position": [0, 0, 0], "spacing": 4}}
```

Every `.fbx`, `.obj`, `.dae` and other supported file in the folder becomes a `.glb` next to its source (same folder
structure, materials and textures embedded), is imported at its real-world size and tagged `dcc` and `blender`, and its
`.meta` records the converter.

| Option | Use |
|---|---|
| `skip_existing: true` | Resume a large batch: files whose `.glb` is newer than the source are skipped. |
| `scale: 0.01` | A pack authored in centimeters that the importer did not already convert. |
| `recenter: "bottom"` | Put each origin at the model's feet. |
| `ops` | Clean up while converting, for example `[{"op": "decimate", "ratio": 0.5}]`. |
| `place` | Put every converted model into the scene in a row along x, `spacing` meters apart. |

### Model a ruined tower and place it

```tool
dcc_generate {"recipe": "tower", "params": {"height": 14, "radius": 3.2, "ruin": 0.6, "seed": 3}, "name": "RuinedTower", "ops": [{"op": "shade_smooth", "angle": 30}], "place": {"position": [12, 0, -4]}}
viewport_capture {"annotate": true}
```

Look at the capture, then change `params` and generate again. For something the recipes cannot do, write the model
yourself with `dcc_run_script`:

```tool
dcc_run_script {"name": "gatehouse", "import": true, "place": {"name": "Gatehouse", "position": [0, 0, 0]}, "script": "import bpy\nfrom skywalker_dcc import blender as B, procedural as P\nB.reset_scene()\ntower = P.generate('tower', height=9, radius=2.2, ruin=0.2, seed=5)\nwall = P.generate('wall', length=6, height=4, thickness=1.0, ruin=0.1)\nwall.location = (3.2, 0, 0)\nB.export_glb(sky.out_path('gatehouse.glb'))\nsky.result(**B.stats())\n"}
```

### Decimate an imported scan

```tool
dcc_edit_asset {"asset": "downloads/scan/statue.glb", "ops": [{"op": "merge_by_distance"}, {"op": "decimate", "ratio": 0.2}, {"op": "shade_smooth", "angle": 35}, {"op": "bake_ao", "target": "vertex", "samples": 24}], "update_references": true}
```

The original stays untouched. `statue_v2.glb` records `derivedFrom`, `ops` and the version in its `.meta`;
`update_references` re-points scene entities to the new version (prefab instances of multi-material models keep using
the old file), and the result lists `before` and `after` triangle counts. Use `mode: "replace"` to overwrite a `.glb` in
place so every use updates live; a backup goes to `.skywalker/dcc-backups/`.

Preset ops, run in order: `decimate {ratio}`, `bevel {width, segments, angle}`, `smart_uv {angle, margin}`,
`merge_by_distance {distance}`, `triangulate`, `shade_smooth {angle}`, `recenter {where: bottom|center}`,
`apply_modifiers`, `join {name}`, `bake_ao {samples, distance, target: vertex|texture, size}`.

### Work in a live Blender window

```tool
dcc_install_addon {}
dcc_session_start {"headless": false, "open": "props/lantern.glb"}
dcc_session_exec {"code": "bpy.ops.object.modifier_add(type='SUBSURF')"}
dcc_session_pull_selection {"name": "Lantern"}
dcc_session_exec {"code": "bpy.context.object.modifiers['Subdivision'].levels = 2"}
dcc_session_pull_selection {"name": "Lantern"}
```

1. `dcc_install_addon` installs the add-on once. Then either a human clicks **Start Bridge** in Blender's N panel, or
   `dcc_session_start` launches Blender with the bridge running (`headless: false` opens a window).
2. `dcc_session_exec` runs Python in the session. Variables persist between calls; in a window, each call is one undo
   step the human can undo with ++cmd+z++.
3. The first `dcc_session_pull_selection` with a name imports the selection and places it. Calling it again with the
   same name updates the asset in place, and every entity that uses it refreshes live.

People can also click **Send Selection to Skywalker** in Blender's Skywalker tab, or right-click a model in the editor's
Asset Browser and choose **Open in Blender**.

## The Blender add-on

The add-on (`integrations/blender/skywalker_bridge/`) is embedded in the engine. `dcc_install_addon` copies it into
Blender's user scripts folder and enables it without touching your other preferences.

| Part | Details |
|---|---|
| N panel → **Skywalker** | Start Bridge, Stop Bridge, **Send Selection to Skywalker** |
| Preferences | Start the bridge automatically; override the editor socket path |
| Server | Newline-delimited JSON over TCP on `127.0.0.1` (random port). Every request carries a random token from `~/.skywalker/dcc/blender.json` (mode 0600), compared in constant time. Methods: `ping`, `status`, `exec`, `selection`, `export_selection`, `import_file`, `save_copy`, `shutdown`. |
| Threading | Sockets live on helper threads; every request that touches `bpy` runs on Blender's main thread. A request that waits too long is abandoned and never runs late. |
| Sessions | Headless sessions end when the engine exits or stops them. A window you opened stays open when the bridge stops. One session per user. |

## Safety model

- No shell anywhere: an executable plus an argument vector, the `SKY_*` environment, and the output folder as the
  working directory.
- Outputs only inside the project: output folders and inputs are checked, and `sky.out_path` refuses `..`.
- `.blend` files open with auto-run scripts disabled; headless runs use factory settings unless `user_prefs: true`.
- Captured output is capped, every run has a timeout, and cancellation kills the process group.
- The `dcc` category asks the human by default for in-editor agents; MCP clients see the open-world hint on every tool
  that runs code.
- The add-on's `exec` runs arbitrary Python in Blender by design. Starting the bridge is an explicit user action, and only
  processes of the same user can read the token.

## Provenance

Imported assets get `tags: ["dcc", "<app>"]` and a `source` record in their `.meta`: `generator` (for example
`blender 3.5.1`), `tool`, `by` (the actor), `retrieved`, plus tool-specific fields (`recipe` and `params`, `scriptSha`,
`derivedFrom`, `derivedFromGuid`, `ops` and `version`, `blend`, `live`). You can always tell which tool and which agent
made an asset, and from what.

## Maya, Houdini and 3ds Max

Detection, command lines and the helper modules (`skywalker_dcc.maya`, `skywalker_dcc.houdini`,
`skywalker_dcc.max3ds`, with `export_fbx`, `export_obj` and geometry export) follow the vendors' documentation but have
not been run against the real applications. The pieces are covered by tests where possible (detection on fake installs
for macOS, Windows and Linux; argument construction). To try Maya:

```bash
export SKY_MAYAPY=/Applications/Autodesk/maya2025/Maya.app/Contents/bin/mayapy
```

```tool
dcc_run_script {"app": "maya", "script": "from skywalker_dcc import maya as M\nM.export_fbx(sky.out_path('crate.fbx'))", "import": true}
```

`import: true` converts the FBX through Blender. Report problems with the exact app version.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `error [app_not_found]` | Install Blender or set `SKY_BLENDER`, then `dcc_list {"refresh": true}`. |
| `error [dcc_failed]: ... ValueError: ...` | The error line is the script's; the full app log follows in the hint. |
| `error [no_session]` | Call `dcc_session_start`, or `dcc_install_addon` and click **Start Bridge** in Blender. |
| `timeout` on a session call | Blender is busy or showing a modal dialog; keep `exec` calls short. |
| 3DS import fails | Blender 3.5 ships no 3DS importer; 3.6 and later do. |
| The import is 100 times too big or small | Use `scale` (0.01 or 100) on `dcc_convert`, or `normalize: true` to fit 1 m. |

## Pitfalls

- In-process synchronous callers (the CLI, a `batch` containing a `dcc_*` tool, tests) wait for the app to finish.
  Agents on the socket, the editor's crew and its buttons run design apps off the main thread.
- Files a script writes outside `SKY_OUT` are not reported or imported: use `sky.out_path`.
- Windows process execution is not implemented yet (detection and command lines are).
- glTF export embeds images, but procedural node textures are not baked unless you bake them.
- One live session per user. Alembic and skeletal animation and shape keys are not carried through `dcc_edit_asset`.

!!! warning "Licensing of Blender scripts"

    Blender, Maya, Houdini and 3ds Max are not bundled, linked or redistributed: Skywalker starts the copies you already
    have as separate processes, so their licenses do not apply to Skywalker or to games made with it. Python that runs
    inside Blender and imports `bpy` is, by the Blender Foundation's position, subject to the GPL. The files that do so
    (`integrations/blender/skywalker_bridge/*.py` and `integrations/dcc/skywalker_dcc/{blender,procedural,tasks}.py`)
    carry `SPDX-License-Identifier: GPL-3.0-or-later`. They talk to the engine only through processes and sockets, so the
    engine and editor keep the project license. The engine embeds these files as data and writes them out to run them;
    ship their license text with any binary distribution. See [License](../about/license.md).

!!! agent "For agents"

    Check what is installed, prefer recipes and presets, and look at every result:

    ```tool
    dcc_list {}                                                   # installed apps, recipes, jobs, the live session
    dcc_generate {"recipe": "rock", "params": {"radius": 1.5, "moss": 0.4, "seed": 7}, "name": "MossyRock", "place": {"position": [4, 0, 2]}}
    dcc_convert {"path": "downloads/town_kit", "recursive": true, "skip_existing": true}
    dcc_edit_asset {"asset": "downloads/scan/statue.glb", "ops": [{"op": "decimate", "ratio": 0.25}, {"op": "shade_smooth"}]}
    viewport_capture {"annotate": true}                           # verify the placement and scale
    dcc_cancel {"job": 3}                                         # stop a job that runs too long
    ```

    `dcc_run_script` executes code on the user's machine: describe what the script does when you ask for approval.

## Reference

- Tools: [DCC tools](../reference/tools/dcc.md), including [`dcc_list`](../reference/tools/dcc.md#dcc_list),
  [`dcc_run_script`](../reference/tools/dcc.md#dcc_run_script), [`dcc_convert`](../reference/tools/dcc.md#dcc_convert),
  [`dcc_generate`](../reference/tools/dcc.md#dcc_generate),
  [`dcc_edit_asset`](../reference/tools/dcc.md#dcc_edit_asset),
  [`dcc_session_start`](../reference/tools/dcc.md#dcc_session_start)
- [Assets and prefabs](assets.md) for importing, `.meta` sidecars and GUIDs
- Design document: [docs/DCC.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/DCC.md)
