---
name: skywalker-2d-ui
description: "2D games, UI and dialogue in Skywalker - sprites and sprite sheets, atlases, tilemaps from ASCII, 2D lights, parallax, pixel-perfect cameras, world text, UI canvases (HUD, menus, inventory), themes and style sheets, and Yarn-style dialogue scripts. Use for platformers, top-down and pixel-art games, HUDs, menus, settings screens, dialogue boxes, signs and damage numbers."
---

# 2D, text, UI and dialogue

Load skywalker-core first. Everything here is a reflected component plus a tool; the engine doc is the MCP resource `skywalker://docs/2D_AND_UI`.
Captures (`viewport_capture`) include sprites, tiles, world text and UI with real screen boxes, also without a GPU (CPU rasterizer).

## Conventions

- **World 2D** lives on the **XY plane**: +Y up, cameras look down -Z. Painter's order: `sortingLayer` (background < midground < default < foreground < overlay),
  then `order`, then depth (farther first). Sprites depth-test against 3D geometry, so 2.5D works.
- **UI** is in canvas pixels, origin **top-left, y down**: the same space as the boxes `viewport_capture` and `ui_inspect` return. Padding and margin use CSS order
  `[top, right, bottom, left]` and shorthand (`12`, `[8, 16]`).
- Colors are sRGB hex (`"#ff8800"`); shading is linear HDR, so bloom, fog and tonemapping apply to sprites. Flat pixel art:
  `environment_update {tonemap:"none", bloomIntensity:0, taa:false}`.
- Untextured sprites draw as tinted rectangles and a tilemap without a tileset draws one flat color per tile id: **block out the level first, add art later**.

| Component | Use | Key fields |
|---|---|---|
| `sprite` | Textured quad | `texture` (png or `*.atlas.json`), `frame`, `columns`/`rows`, `pivot` ([0.5,0] = feet), `size` or `pixelsPerUnit`, `color`, `flipX`, `sortingLayer`, `order`, `filter:"nearest"` for pixel art, `normalMap`, `emissive`, `billboard`, `lit` |
| `sprite_anim` | Flipbook | `clips` `{"run":{"frames":"4-11"\|"run_*"\|[..], "fps":12, "loop":true, "events":{"3":"footstep"}}}`, `clip`, `playing`, `speed` |
| `tilemap` | Tile layers | `tileset`, `tileSize`, `cellSize`, `layers[{name,data,solid,z,tint,sortingLayer}]`, `solidTiles`, `autotile`, `filter` |
| `light2d` | 2D light | `kind` point/spot/global, `color`, `intensity`, `radius`, `falloff`, `height` (normal maps), `shadows`, `halo`, `flicker` (max 32 per frame) |
| `parallax` | Layer (entity + children) | `factor` (0 = fixed to camera, <1 far, >1 near), `repeatX/Y`, `spacing`, `origin` |
| `camera2d` | On the camera entity (with `camera.orthographic`) | `pixelsPerUnit`, `referenceHeight` (integer upscaling), `pixelSnap`, `zoom`, `follow`, `smoothing`, `deadZone`, `bounds` |
| `text` | World text (signs, damage numbers) | `text` (rich tags `<b> <i> <color=gold> <size=150%> <font=serif> <br>`), `size` (world em), `align`, `maxWidth`, `outline`, `emissive` |
| `ui_canvas` / `ui` | UI root / element | see the UI section; build them with `ui_create`, never by hand |
| `dialogue` | Conversation runner | `script`, `startNode`, `autoStart`, `ui` (default\|none), `typewriter`, `portraits` |

Fonts built in: Inter (`sans`), EB Garamond (`serif`), JetBrains Mono (`mono`), or any project `.ttf`/`.otf`.

## Workflow A: a 2D game

1. **Camera.** One orthographic camera with `camera2d`; `referenceHeight` 180 or 270 gives crisp integer upscaling for 16 px art.
2. **Art to frames.** Grid sheet: `sprite_sheet_slice`. Folder of single images: `sprite_atlas_pack`.
3. **Level.** `tilemap_from_ascii` (fastest), then `tilemap_paint` for edits, `tilemap_inspect` to read it back.
4. **Characters.** Entity + `sprite_sheet_slice {entity}` (applies sprite and clips), then set `pivot`/`filter`.
5. **Depth and mood.** `parallax` backgrounds, `light2d` torches, a dark `environment_update` for night.
6. **Behavior.** Wander (`play_anim`, `tile_at`, `set_tile`, `on anim`, `on ui`). See skywalker-wander.
7. **Verify.** Capture, `step`, `sim_trace`, capture again (see the loop at the end).

```text
entity_create {name:"Knight", position:[2,1,0]}
sprite_sheet_slice {image:"art/knight.png", cell:[32,32], entity:"Knight", animations:{idle:{row:0, fps:6}, run:{row:1, fps:12}, attack:{frames:"16-21", fps:14, loop:false, events:{"3":"hit"}}}}
entity_update {entity:"Knight", components:{sprite:{pivot:[0.5,0], filter:"nearest"}, sprite_anim:{clip:"idle", playing:true}}}
sprite_atlas_pack {folder:"art/props", output:"art/props.atlas.json"}     # then sprite {texture:"art/props.atlas.json", frame:"coin_0"}; suggested clips come back in the result
```

`sprite_sheet_slice` without `entity` only writes the atlas (frames named `<image>_0..`); `names` or `prefix` rename them. `sprite_atlas_pack` trims, pads and extrudes so nothing
bleeds; frames keep natural order (`run_2` before `run_10`).

### Tilemaps

```text
tilemap_from_ascii {name:"Level", tileset:"art/tiles.png", tile_size:16, autotile:{ground:{mode:"wang16", first:1}}, legend:{"#":"ground", "=":5, "o":6}, solid:true, map:"....................\n.......o..o.........\n......======........\n....................\n####################"}
tilemap_paint {entity:"Level", action:"fill", rect:[0,3,20,1], tile:"ground"}              # actions: set | fill | flood | clear; cells are [column,row] from the top-left
entity_create {name:"Camera", position:[10,-2,10], components:{camera:{orthographic:true}, camera2d:{pixelsPerUnit:16, referenceHeight:180, follow:"Knight"}}}
entity_create {name:"Torch", position:[6,-2,0], components:{light2d:{kind:"point", color:"#ffb060", intensity:2.5, radius:6, halo:0.6, flicker:0.3}}}
tilemap_inspect {entity:"Level"}      # ASCII per layer, legend with counts, merged collision rectangles
```

- The legend maps a character to a **tile id** (1-based, 0 = empty), a **terrain** (auto-tiled, from `autotile`) or a tile name. `' '` and `.` are empty.
- Auto-tile modes: `blob47` (47 tiles from `first`), `wang16` (16 tiles, bit order N=1 E=2 S=4 W=8), `random` (variants, `weights`), `single`. An
  `invalid_autotile` error means the tileset has too few tiles for the mode (`terrain "ground" uses tile 9 but the tileset has 8 tiles`).
- `solid:true` marks the layer for collision (`"tiles"` = only `solid_tiles`). Collision data is read with `tilemap_inspect`: `solidRects` are `[x, y, w, h]` in world units
  relative to the map's **top-left**, x right, **y up**, `y` is the rect's bottom edge. The physics world does not read tilemaps by itself: for each rect create a static box,
  `position = mapPosition + [x + w/2, y + h/2, 0]`, `scale = [w, h, 1]`, `components:{collider:{shape:"box"}}`, and give bodies `lockPosition:"z"` so they stay on the plane
  (see skywalker-physics).
- From Wander: `tile_at(map, pos)`, `set_tile(map, pos, id|"terrain")`.

## Workflow B: UI (HUD, menus, inventory)

`ui_create` builds a whole tree in one undoable step and returns ids plus computed rects in pixels. Node types: `panel image text button toggle slider progress scroll input spacer`.
Anchors: `top_left top top_right left center right bottom_left bottom bottom_right` pin an element (`position` is an offset, negative = from the right/bottom edge);
`fill`, `top_stretch`, ... stretch it using `margin`. `layout` row/column/grid with `gap`, `padding`, `align`, `justify`, `columns`; `fit` sizes to content; `flex` shares space.

```text
ui_create {canvas:{name:"HUD", theme:"dark"}, elements:[{type:"panel", name:"Vitals", anchor:"top_left", position:[32,28], style:"hud", layout:"column", gap:6, padding:[10,14], children:[{type:"text", text:"HEALTH", style:"small muted"}, {type:"progress", name:"Health", value:1, size:[240,12]}]}, {type:"text", name:"Score", anchor:"top_right", position:[-32,28], text:"0", style:"large"}]}
ui_create {template:"main_menu", canvas:{name:"Menu", theme:"glass"}}        # templates: main_menu hud pause_menu settings inventory document dialogue
ui_inspect {canvas:"Menu"}                                                    # rects, visibility, text, values; add width/height to test another viewport
ui_style {canvas:"Menu", theme:"parchment"}                                   # themes: dark light parchment glass pixel
ui_style {canvas:"Menu", path:"ui/game.uistyle.json", vars:{accent:"#e8a33d"}, rules:{button:{radius:4, hover:{background:"$accent"}}, "#Title":{fontSize:72}}}
ui_style {element:"Play", style:"primary large"}                              # classes replace; css:{...} adds inline overrides
entity_update {entity:"Title", components:{ui:{text:"Ashes of Veyra"}}}
behavior_set {entity:"Menu", name:"Menu", intent:"Play hides the menu", source:"behavior Menu\n  on ui \"Play\"\n    self.enabled = false\n  end\nend"}
sim_control {action:"play"}
ui_interact {element:"Play", action:"click"}              # also set_value (slider/progress), type, scroll, focus, or at:[x,y] for a pixel click
sim_control {action:"step", ticks:2}
```

- Style cascade: theme, then the canvas `styleSheet`, then widget kind, `.class`, `#Name`, then `styleOverrides`; state blocks `hover pressed focus checked disabled`. `ui_style {canvas, list:true}` prints
  the available properties, themes and selectors. Every theme defines `primary ghost danger title heading subtitle muted small large accent card hud` and the `dialogue_*` parts.
- Canvas `scaleMode` `scale_with_screen` keeps layouts proportional (`constant` = 1 px per pixel); test other sizes with `ui_inspect {width:1280, height:720}` and a portrait size.
- Events: activating a button/toggle/slider/input sends the Wander event `ui:<element name>` (and `on click` on the element). Button names default to their text, so `on ui "Play"` works.
  Read and write elements from Wander: `find("Health").ui.value = hp / 100`, `find("Score").ui.text = "Score: " + str(score)`, `find("Menu").enabled = false`.
- Keyboard and gamepad reach UI too (Tab/arrows move focus, Enter/Space activate). Clicks on UI never fall through to the world while playing.
- `ui_interact` while editing records value edits and only reports which events a click would fire; it needs play mode to run the behaviors.

## Workflow C: dialogue

A Yarn-style script (`story/intro.dialogue`): nodes `title: Name` / `---` / lines / `===`; lines `Speaker: text #tag #portrait:name`; choices `-> text` with an indented body;
commands `<<set $x to 1>>`, `<<declare $x = 0>>`, `<<if $x > 1>> ... <<elseif>> ... <<else>> ... <<endif>>`, `<<jump Node>>`, `<<wait 1>>`, `<<stop>>`; any other `<<name args>>`
becomes the Wander event `dialogue:name`. `{$trust}` interpolates; functions `visited("Node")`, `random`, `dice`, `min`, `max`, `round`.

1. Write the file, then **lint**: `dialogue_check {path:"story/intro.dialogue"}` (syntax, missing jump targets with did-you-mean, unreachable nodes, unset variables, custom commands).
2. **Test branches without play mode**: `dialogue_preview {path, choices:[0,"refuse"], vars:{trust:2}}` returns the transcript, taken choices, commands and final variables; when the choices run out it stops at the pending choice.
3. **Attach**: `entity_create {name:"Vale", components:{dialogue:{script:"story/intro.dialogue", autoStart:true}}}`. The default box (portrait from `portraits/<name>.png`, name plate, typewriter text, numbered choices) is made of
   ordinary UI entities under a `Dialogue UI` canvas; restyle with `ui_style` and the `.dialogue_*` classes, or build it with `ui_create {template:"dialogue"}`. `ui:"none"` lets you draw your own from `self.dialogue.line`.
4. **Drive it live**: `dialogue_control {action:"start"|"advance"|"choose"|"stop"|"state", entity, node, choice}`.
5. **React in Wander**: `on dialogue "open_gate"` (a command), `on dialogue "end"`, `dialogue_var("trust")`, `start_dialogue("Intro")`, `dialogue_choose(i)`.

```text
dialogue_check {path:"story/intro.dialogue"}
dialogue_preview {path:"story/intro.dialogue", choices:[0, "refuse"]}
dialogue_control {action:"start", entity:"Vale"}
dialogue_control {action:"advance"}
dialogue_control {action:"choose", choice:1}
```

## Wander for 2D (validate with `wander_check`)

```wander
behavior Hero
  on tick
    if key("d") then
      self.sprite.flipX = false
      play_anim(self, "run")
    else
      play_anim(self, "idle")
    end
  end
  on anim "hit"
    log "hit frame"
  end
end

behavior Farmer
  on key "e"
    let map = find("Farm")
    if tile_at(map, self.position) == 3 then set_tile(map, self.position, "soil") end
  end
end
```

`play_anim(e, clip, restart?)`: a non-looping clip ends with `on anim "finished"`, frame events arrive as `on anim "<name>"`.

## Verification loop

1. `ui_inspect` first: it is exact and cheap. Check nothing overlaps, nothing leaves the canvas, text fits its box (rect height), buttons are at least ~44 px high.
2. `viewport_capture {view:"scene", width:1280, height:720, annotate:false, overlays:false}` and look. Also capture 4:3 (`1024x768`) and, for mobile, portrait (`720x1280`).
3. For motion: `sim_control {action:"play"}`, `sim_input`, `step`, capture at several ticks, `sim_trace` the player position. Judge animation from several frames, never one.
4. Fix, recapture. Report what you saw, not what you set.

## Pitfalls

- `sprite_sheet_slice` frames are numbered left-to-right, top-to-bottom; `row:1` is the **second** row (0-based). `frames:"16-21"` is an inclusive index range.
- Camera in the wrong place: the camera must be on the +Z side looking down -Z, and sprites at z=0 (the example camera uses z=10). A 2D scene seen "from the editor" is a perspective view; look through the scene camera (`view:"scene"`).
- Pixel art blurry or shimmering: `filter:"nearest"`, `pixelSnap`, `referenceHeight`, `taa:false`, `tonemap:"none"`.
- Text too small to read: sizes are in world units for `text`, pixels for `ui`; check at the smallest supported resolution.
- `save_path` and other output folders must already exist.
- UI built with `ui_create` is one undo step; to change one element use `entity_update {components:{ui:{...}}}` or `ui_style`, not a rebuild.
- 2D shadows are screen-space (casters must be on screen); text shaping is per code point (no Arabic/Indic shaping).
- Design for several aspect ratios; UI must be reachable by keyboard and gamepad as well as the mouse.
