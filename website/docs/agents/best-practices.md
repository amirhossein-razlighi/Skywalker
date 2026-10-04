# Best practices and prompts

What separates a good agent session from a frustrating one is rarely the model: it is whether the agent looks at its
work, verifies numerically, batches its edits and respects the conventions. This page collects the patterns that work
in Skywalker, prompt templates for common jobs, and the mistakes to avoid.

## Patterns

### Orient once, then query narrowly

`engine_info` and `scene_overview` once per session. On big scenes, `scene_query` by name glob, tag, component or
proximity beats re-reading the whole outline; `entity_get` only the entities you change.

```tool
scene_query {"name": "Lamp*", "component": "light", "limit": 50}
```

### Batch, and label it

Three or more edits go in one `batch`: atomic, one undo step, and the failing index is reported if anything is wrong.
Generators (`scatter`, `foliage_add`, `prefab_instantiate`, `entity_duplicate` with `count`) replace loops of single
creates.

```tool
scatter {"prefab": "prefabs/pine.prefab.json", "count": 40, "radius": 12, "min_distance": 1.5, "surface": "Island"}
```

### Measure the ground before placing anything

Terrain heights are absolute world values. Query the ground, then place, then settle.

```tool
terrain_query {"entity": "Island", "points": [[12, -6], [14, -4]]}
place_on_surface {"entities": ["Crate 1", "Crate 2", "Barrel"]}
physics_settle {"entities": ["Crate 1", "Crate 2", "Barrel"]}
```

### Look at every change, judge at quality

Capture after each meaningful change. Explore at `samples: 1` and small sizes; judge looks at 8–16 samples from the
game camera with labels off. Diagnose with debug views instead of guessing: [How agents see](seeing.md).

### Verify behavior numerically

Write behaviors with a `test` per rule and run `wander_test`; then play with `sim_input`, step, and read state
(`entity_get`, `sim_trace`, `logs`). The simulation is deterministic, so a check that passes once passes every time.

```tool
wander_test {"entity": "Coin", "name": "Coin"}
sim_trace {"entities": ["Player"], "properties": ["transform.position"], "ticks": 120, "every": 20, "hold": ["w"]}
```

### Undo instead of patching

When an edit went wrong, `history {"action": "undo"}` and try differently; layering fixes on a broken state wastes
turns and leaves debris.

### Save and report

Headless edits live in memory: `scene_save` before you finish, and report what changed with ids and a final capture.

## Prompt templates

=== "Build a level"

    ```text
    Build a small coastal village on the existing Island terrain: 6 houses with dcc_generate building, a pier,
    lanterns along a path, a campfire. Query the ground before placing. Batch edits. Capture from the scene camera
    at 8 samples when done and list the ids you created. Save the scene.
    ```

=== "Look development"

    ```text
    Make this scene read as golden hour: low warm sun behind the tower, light haze, soft clouds, agx tonemapping.
    Change a few fields at a time, capture at 8 samples after each change, and stop when exposure, contrast and color
    pass the look-dev rubric. Show me before and after.
    ```

=== "Gameplay behavior"

    ```text
    Write a Guard behavior from this intent: "Patrol between two posts; chase the player when within 5 m; lose 1 hp
    per hit; die at 0". Derive spec rules, write one test per rule, run wander_test until green, attach it to Guard 1
    and 2, then play 10 seconds with the player walking toward the guards and report the guards' hp and states.
    ```

=== "Performance"

    ```text
    The valley view is slow. Benchmark perf_stats with frames 30 and passes true from the scene camera, find the
    most expensive group, check overdraw, lod and light_complexity debug views, fix the top cause, and benchmark
    again. Report before/after milliseconds per group.
    ```

=== "Playtest and fix"

    ```text
    Run playtest_run with the goal_seeker policy for 3 runs of 45 seconds. File the findings as feedback, decide
    which to act on with a rationale, fix them, and verify with playtest_compare. Report the metric deltas.
    ```

## Anti-patterns

| Don't | Do instead |
|---|---|
| Claim "it looks great" without a capture | `viewport_capture` from the camera that matters, at the quality you judge |
| Place props at y = 0 on a terrain | `terrain_query` or `raycast` the ground first |
| Edit while the game is playing | Stop, edit, play again (stop restores the pre-play snapshot) |
| Paste the same warm tone into sun, fog, grade and sky | One dominant hue and one accent; cool fog against a warm sun |
| Set thirty environment fields at once | Change a few, capture, judge, repeat |
| Hand-write scene JSON | Use tools: they validate and are undoable |
| Loop single `entity_create` calls | `batch`, `scatter`, `foliage_add`, `entity_duplicate {"count": n}` |
| Retry a declined download or design-app call | Ask the human once, up front, with the reason |
| Call `engine_info` repeatedly or dump `entity_get` on huge entities | Orient once; query narrowly |

## Working as a crew

For larger jobs, split the work by role and let the studio coordinate: a level designer blocks out, an environment
artist dresses and lights, a gameplay programmer writes behaviors with tests, a playtester runs bots, a critic reviews
captures, and a director decides what to act on with measured effects. Each subagent identifies itself with
`as: "<id>"`, so the board, feedback and history say who did what. See [Studio and crews](../manual/studio.md).

```tool
studio_team_template {"template": "indie_trio"}
studio_loop_define {"template": "playtest_fix_verify", "goal": "Players finish the canyon run without unfair deaths"}
studio_loop_start {"loop": "playtest_fix_verify"}
```

!!! agent "For agents"

    The five-step loop on one card: **orient** (`engine_info`, `scene_overview`), **act** (`batch`), **look**
    (`viewport_capture`), **verify** (`wander_test`, `sim_trace`, `playtest_run`, `perf_stats`), **finish**
    (`scene_save`, report) or **undo** (`history`).
