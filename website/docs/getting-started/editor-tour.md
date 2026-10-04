# Editor tour

The editor is a dense, dark, native macOS app laid out like a professional content-creation tool: an outliner on the
left, the viewport in the middle with a dock below it, the details panel on the right, and a status bar. Every panel
calls the same tools agents use, so anything you do can be undone, replayed or scripted, and anything an agent does
shows up in front of you.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/overview.webp · The whole window with a project open: outliner, viewport, dock (Assets tab) and details panel</div>

## Layout

| Area | What it does |
|---|---|
| **Toolbar** | Transform tools (select, move, rotate, scale), gizmo space (world or local), snapping, play controls, the crew's presence avatars, undo and redo, and toggles for the three side panels |
| **Outliner** | The entity hierarchy: drag to re-parent, toggle entities on and off, see which entities have behaviors, add entities, right-click to save an entity as a prefab |
| **Viewport** | The live Metal view with gizmos, selection outlines, the stats overlay, a quality-tier picker and the debug-view menu |
| **Dock** | Tabs: **Assets** (asset browser), **Console** (logs and errors), **Activity** (who changed what), **Agents** (the in-editor crew), **Studio** (roster, board, feedback, loops, messages) |
| **Details** | The property grid for the selected entity: every component field with its range and documentation, asset pickers, behaviors with intent, code and graph |
| **Status bar** | Play state, the selection, the latest activity and who made it, how many agents are working, the external-agent socket ("MCP listening", click to toggle) and the active renderer |

Panels resize by dragging their edges; the layout and the selected dock tab are remembered.

## Viewport controls

| Action | Mouse | Trackpad |
|---|---|---|
| Orbit | Drag on empty space, right-drag, or ++option++-drag | Two-finger scroll |
| Pan | Middle-drag | ++shift++ + two-finger scroll |
| Zoom | Scroll wheel | Pinch |
| Select | Click; ++shift++ or ++cmd++ click adds or removes | Click |
| Move by dragging | Drag a selected object with the move tool | |
| Frame selection (or everything) | ++f++ | ++f++ |

| Key | Action |
|---|---|
| ++q++ ++w++ ++e++ ++r++ | Select, move, rotate, scale tool |
| ++ctrl++ while dragging a gizmo | Snap (default 0.5 m and 15°; the toolbar toggle snaps always) |
| ++cmd+d++ | Duplicate the selection |
| ++backspace++ | Delete the selection (one undo step) |
| ++escape++ | Clear the selection |
| ++cmd+z++ / ++shift+cmd+z++ | Undo / redo (any edit, by anyone) |
| ++cmd+0++ | Frame everything |

## Play mode

| Command | Shortcut | Tool |
|---|---|---|
| Play | ++cmd+p++ | `sim_control {"action": "play"}` |
| Pause (editor pause) | ++shift+cmd+p++ | `sim_control {"action": "pause"}` |
| Step one tick | toolbar | `sim_control {"action": "step", "ticks": 1}` |
| Stop and restore | ++cmd+period++ | `sim_control {"action": "stop"}` |

While playing, the viewport forwards the keyboard, mouse and connected gamepads to the game (input actions from
`input.json`). Stop restores the exact pre-play snapshot, so you can experiment freely. The editor's pause freezes
everything; the game's own pause (`pause_game()`) is a different thing that keeps menus running: see
[Simulation and time](../manual/simulation.md).

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/play-mode.webp · The editor while playing: the toolbar's play controls highlighted, the game's HUD in the viewport</div>

## The details panel and behaviors

Select an entity to see its components. Each field shows its documentation on hover and validates ranges; asset fields
(material, texture, mesh) take a drag from the asset browser or a pick from the field's menu.

Behaviors appear at the bottom as cards: the **intent** in plain language, then the implementation with a
**Code | Graph** switch. Code is Wander with diagnostics as you type; Graph is the same behavior as a node graph, and
edits in either view regenerate the other. **Weave** asks the crew's gameplay programmer to write the behavior from its
intent, with a test for every rule.

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/details-behavior.webp · The details panel with a light component and a behavior card in Code view</div>

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/behavior-graph.webp · The same behavior in Graph view: entry node, exec wires, data nodes with inline pin values</div>

## Assets

The **Assets** tab lists every file the engine understands with type filters, search over paths, tags and
descriptions, and lazily rendered thumbnails. Drag a mesh or prefab into the viewport to place it on the surface under
the cursor; drag a material or texture onto an object to apply it. Right-click a model and choose **Open in Blender**
to edit it live through the [DCC bridge](../manual/dcc.md).

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/assets-dock.webp · The Assets dock with type filters and counts, thumbnails, and the inspector showing an asset's license, provenance and usage</div>

## Agents and the Studio

The **Agents** tab is the in-editor crew ("Cloudlings"): chat with an agent, watch its tool calls stream, approve or
decline actions when its autonomy is *Ask*. Avatars in the toolbar pulse while an agent works, the **Activity** tab
records every edit with its author (`user`, `agent:Nimbus`, `mcp:claude-code`), and every edit is undoable.

The **Studio** tab holds the studio shared by the crew, the headless runner and external agents: a roster, a Kanban
board, feedback with the director's verdicts and measured effects, loops and message threads. See
[Studio and crews](../manual/studio.md).

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/agents-chat.webp · The Agents tab: a conversation with a crew member, tool calls inline, an approval prompt</div>

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/studio-board.webp · The Studio tab, Board view with tasks across backlog, doing and review</div>

## Stats, quality and debug views

The viewport's stats overlay shows frame rate and CPU and GPU time; click its frame line for the per-pass GPU timings.
While editing, the viewport renders in one of three quality tiers (**fast** by default, **balanced**, **full**) so
heavy worlds stay responsive; play mode, captures and movies always render full quality. The debug-view menu switches
the viewport to wireframe, overdraw, LOD, light complexity, G-buffer channels and more; see
[Debug views](../manual/rendering/debug-views.md).

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/stats-overlay.webp · The viewport stats overlay expanded to the per-pass GPU list</div>

## Menus worth knowing

| Menu | Command | Shortcut |
|---|---|---|
| File | Open Project… | ++cmd+o++ |
| File | Save Scene | ++cmd+s++ |
| Game | Play / Pause / Stop | ++cmd+p++ / ++shift+cmd+p++ / ++cmd+period++ |
| Game | Render Movie… | ++option+cmd+m++ |
| Game | Frame All | ++cmd+0++ |

!!! agent "For agents"

    External agents can attach to the running editor and co-edit live: their edits appear in the Activity feed as
    `mcp:<client>`, and you can undo them. Enable the socket from the status bar (or Settings › External Agents), then
    point the agent at it:

    ```bash
    skywalker mcp --attach
    ```

    Inside that session, `selection_get` tells the agent what you selected ("this", "these"), `selection_set` and
    `camera_set` show you what it means, and `viewport_debug_view` switches the view you both see.

    ```tool
    selection_get {}
    camera_set {"frame": "Tower"}
    viewport_debug_view {"view": "wireframe"}
    ```

Next: build [your first game in 15 minutes](first-game.md).
