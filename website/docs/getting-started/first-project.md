# Your first project

A Skywalker project is a plain folder: scenes, assets, scripts and settings are human-readable files that diff well
in git and that agents can read. This page explains the layout, opens the starter scene, and shows the three things
you will do most often from the command line: render, simulate and run a tool.

## The project folder

```text
my_game/
  game.json                 title, start scene, window, quality, icon, packaging rules
  input.json                input action map (optional; defaults apply without it)
  audio.json                mixer buses (optional)
  scenes/
    main.sky.json           a scene: environment + entities with components and behaviors
  materials/  *.mat.json    material assets
  prefabs/    *.prefab.json saved entity trees
  textures/  models/  audio/  ...    any asset; each gets a .meta sidecar with a stable GUID
  scripts/    *.wander      shared Wander modules (use "scripts/util")
  native/     *.cpp         optional native C++ modules
  agents/     *.agent.json  the studio roster (optional)
  studio/                   board, feedback, decisions, loops, playtests (optional)
  CREDITS.md                credits for downloaded assets, written automatically
  .skywalker/               caches (shader pipelines, impostors, compiled modules); never commit
```

Everything an agent or the editor creates follows this layout, and `skywalker build` knows which parts ship with a
game and which (studio, agents, native sources, caches) stay behind. See [Shipping](../manual/shipping.md).

## Anatomy of a scene

Scenes are JSON. Each entity has a stable 64-bit id, a name, a parent, tags, vars, components and behaviors. This is
the hero of the starter scene, `examples/hello_sky/scenes/main.sky.json`:

```json
{
  "id": 6,
  "name": "Hero",
  "parent": 0,
  "enabled": true,
  "tags": ["player"],
  "vars": {},
  "components": {
    "transform": {"position": [0, 0.5, 3]},
    "mesh": {"mesh": "capsule", "color": "#2ec4b6", "roughness": 0.4}
  },
  "behaviors": [
    {"name": "Controls",
     "intent": "WASD moves the hero on the ground at 4 m/s. Space makes it jump.",
     "source": "behavior Controls\n  ...\nend\n",
     "enabled": true}
  ]
}
```

Components are reflected structs: the same field table drives this JSON, the schema agents read, the editor's
property grid and Wander access such as `self.mesh.roughness`. A behavior pairs a natural-language **intent** with the
**Wander** code that implements it:

```wander
behavior Controls
  intent "WASD moves the hero on the ground at 4 m/s. Space makes it jump."
  var speed = 4
  var vy = 0
  on tick
    let dir = (0, 0, 0)
    if key("w") then dir = dir + (0, 0, -1) end
    if key("s") then dir = dir + (0, 0, 1) end
    if key("a") then dir = dir + (-1, 0, 0) end
    if key("d") then dir = dir + (1, 0, 0) end
    move self by normalize(dir) * speed * dt
    vy = vy - 20 * dt
    self.position.y = max(0.5, self.position.y + vy * dt)
  end
  on key "space"
    if self.position.y <= 0.51 then
      vy = 7
    end
  end
end
```

## Render, simulate, inspect

=== "Render a still"

    ```bash
    skywalker render examples/hello_sky/scenes/main.sky.json -o hello.png --samples 8
    ```

    `--samples` accumulates jittered sub-samples (anti-aliased, noise-free GI and reflections), `--annotate` labels
    every entity with its id, `--scene-camera` looks through the scene's camera instead of the editor view.

=== "Simulate"

    ```bash
    skywalker run examples/hello_sky/scenes/main.sky.json --ticks 600 -o after.png
    ```

    Runs ten seconds of the game at the fixed 60 Hz tick, prints every `log` and runtime error, and renders the last
    frame. Runs are deterministic: the same scene always produces the same result.

=== "Run a tool"

    ```bash
    skywalker call scene_overview '{}' --project examples/hello_sky --scene scenes/main.sky.json
    skywalker call viewport_capture '{"annotate": true}' --project examples/hello_sky --scene scenes/main.sky.json -o capture.png
    ```

    `skywalker call` loads the project and scene, runs one tool, prints its result and (with `-o`) writes the image
    the tool returned. Every editor action and every agent action is one of these tools.

=== "Compile Wander"

    ```bash
    skywalker check scripts/player.wander --project my_game
    ```

    Compiles a `.wander` file exactly as the engine would and prints diagnostics with line, column, a stable code
    and a did-you-mean hint.

## Open it in the editor

```bash
open build/release/bin/Skywalker.app --args --project "$PWD/examples/hello_sky"
```

Press ++cmd+p++ to play. ++w++ ++a++ ++s++ ++d++ move the hero, ++space++ jumps, and walking into the coin collects
it. ++cmd+period++ stops and restores the scene exactly as it was before play.

## Start your own

Create a folder and open it; the editor starts an empty project with a default scene (a ground plane, a cube and a
camera). Save with ++cmd+s++: the first save asks for a name and suggests `scenes/main.sky.json`, the scene the
editor opens at launch.

```bash
mkdir -p ~/Games/my_game
open build/release/bin/Skywalker.app --args --project ~/Games/my_game
```

!!! agent "For agents"

    Headless agents work on the same folder through the MCP server; edits stay in memory until the agent saves:

    ```tool
    scene_new {"name": "Main"}
    entity_create {"name": "Crate", "mesh": "cube", "position": [0, 0.5, 0], "color": "#c8a46e"}
    scene_save {"path": "scenes/main.sky.json"}
    ```

Next: the [Editor tour](editor-tour.md), or jump straight to [your first game](first-game.md).
