# 2D, text, UI and dialogue

Skywalker draws 2D games (painted metroidvanias, pixel-art farm sims), text in the world, full
user interfaces (HUDs, menus, inventories, map-game panels, documents) and branching dialogue.
Everything is a reflected component and a tool, so agents build it the same way they build 3D.

```
Scene ──▶ render2d::gather2D ─┐                       ┌─▶ Metal: Sprite2D.metal (scene pass, HDR, G-buffer "unlit")
          ui::UiSystem::build ┴─▶ FrameData.render2d ─┤        UI.metal (after the composite, output resolution)
                                                       └─▶ CPU:   Raster2D (headless captures, CI)
```

Conventions:

- **World 2D** lives on the XY plane: +Y up, cameras look down −Z. Painter's order is
  `sortingLayer` (background < midground < default < foreground < overlay), then `order`, then
  depth (farther first), then scene order. Sprites depth-test against 3D geometry (2.5D works).
- **UI** is in canvas pixels with the origin at the **top-left, y down** — the same space as the
  boxes `viewport_capture` and `ui_inspect` return. Paddings and margins use CSS order
  `[top, right, bottom, left]` and CSS shorthand (`12`, `[8, 16]`).
- Colors are authored in sRGB hex (`"#ff8800"`); 2D shading happens in linear HDR, so bloom,
  fog, exposure and tonemapping apply to sprites. For flat pixel art use `tonemap: "none"`,
  `bloomIntensity: 0`, and `taa: false` if you want every texel razor sharp.

## Components

| Component | What it is | Key fields |
|---|---|---|
| `sprite` | Textured quad | `texture` (png / `*.atlas.json`), `frame` (name or index), `columns`/`rows` (grid sheets), `region`, `pivot` ([0.5, 0] = feet), `size` or `pixelsPerUnit`, `color`, `flipX/Y`, `sortingLayer`, `order`, `filter` (nearest for pixel art), `normalMap`, `emissive`, `billboard` (none/y/full for 2.5D), `lit`, `castShadows`, `alphaCutoff` |
| `sprite_anim` | Flipbook | `clips` `{"run": {"frames": "4-11" \| [..] \| "run_*", "fps": 12, "loop": true, "events": {"3": "footstep"}, "texture"?}}`, `clip`, `playing`, `speed`. Frame events reach the entity's behaviors as `on anim "footstep"`, a non-looping clip's end as `on anim "finished"` |
| `tilemap` | Layers of tiles | `tileset` (png or `*.tileset.json`), `tileSize`, `cellSize`, `width`, `height`, `layers` (`[{name, data, solid, z, tint, sortingLayer, order}]`), `solidTiles`, `autotile` (terrains), `sortingLayer`, `filter`, `lit`, `castShadows` |
| `light2d` | 2D light | `kind` point/spot/global (ambient), `color`, `intensity` (HDR), `radius`, `falloff`, `innerAngle`/`outerAngle` (spot along local +Y), `height` (normal maps), `shadows`, `shadowSoftness`, `halo` (glow in the air), `flicker` |
| `parallax` | Parallax layer (entity + children) | `factor` (0 = fixed to camera, <1 far, >1 near), `origin`, `repeatX/Y`, `spacing` |
| `camera2d` | On the camera entity | `pixelsPerUnit`, `referenceHeight` (e.g. 180 → integer upscaling), `pixelSnap`, `zoom`, `follow` + `smoothing` + `deadZone` + `offset`, `bounds` |
| `text` | World text (signs, damage numbers) | `text` (rich), `font`, `size` (world em), `color`, `align`, `valign`, `maxWidth`, `outline`, `shadowColor`, `emissive` (neon), `billboard`, `sortingLayer` |
| `ui_canvas` | UI root | `mode` screen/world, `referenceResolution`, `scaleMode` (scale_with_screen/constant), `match`, `sortOrder`, `theme` (dark/light/parchment/glass/pixel), `styleSheet`, `worldScale` |
| `ui` | UI element | `widget` (panel, image, text, button, toggle, slider, progress, scroll, input, spacer), `anchor` preset or custom `anchorMin/anchorMax/pivot`, `position`, `size`, `margin`, `layout` (row/column/grid), `gap`, `padding`, `align`, `justify`, `columns`, `fit`, `flex`, `text`, `image`, `value`, `style` (classes), `styleOverrides`, `event`, `interactable`, `visible`, `scroll` |
| `dialogue` | Conversation runner | `script` (`*.dialogue`) or `source`, `startNode`, `autoStart`, `ui` (default/none), `typewriter`, `portraits`; state: `running`, `node`, `speaker`, `line`, `choices`, `tags` |

Untextured sprites draw as tinted rectangles and tilemaps without a tileset draw flat colors per
tile id, so levels can be blocked out before any art exists.

## Text

Fonts are signed-distance fields rasterized lazily from TrueType/OpenType files (stb_truetype) —
crisp at any size, zoom or world scale, with outlines, shadows, glows and faux bold computed in the
shader, and OpenType (GPOS) kerning. Built in: **Inter** (`""`, `sans`), **EB Garamond**
(`serif`), **JetBrains Mono** (`mono`); any `*.ttf`/`*.otf` path in the project works too.

Rich text tags: `<b> <i> <u> <s> <color=#f80> <color=gold> <alpha=0.5> <size=24|150%|+4>
<font=serif> <br> <noparse>`. UTF-8 throughout; missing glyphs fall back to the other built-in
fonts. Wrapping prefers spaces, breaks long words, and handles CJK.

**Localized texts.** A `text.text`, `ui.text` or `ui.placeholder` written `"@menu.play"` shows the
string for that key in the current locale (`locale/*.csv` or `*.po`); `{placeholders}` come from
the entity's vars and `"@@"` writes a literal `@`. Scripts use `tr("key", {n: 3})`. See
[LOCALIZATION](LOCALIZATION.md), which also covers fonts for scripts the built-in fonts lack.

## UI layout and styling

- **Anchors**: `top_left, top, top_right, left, center, right, bottom_left, bottom, bottom_right`
  pin the element (its `position` is an offset, `size` its size); `fill`, `top_stretch`,
  `middle_stretch`, `bottom_stretch`, `left_stretch`, `center_stretch`, `right_stretch` stretch it
  (insets from `margin`; on an axis that does not stretch, the margin insets the element from the edge it is
  anchored to, e.g. a `bottom_stretch` box with margin bottom 40 floats 40 px above the bottom); `custom` uses `anchorMin`/`anchorMax` (fractions of the parent rectangle the element's corners follow) and `pivot` (the point of the element that `position` places).
- **Layouts**: `row`/`column` stack children with `gap`, `padding`, `align` (cross axis:
  start/center/end/stretch — stretch also stretches fit-to-content children, whose text then wraps)
  and `justify` (start/center/end/space_between); `flex` shares leftover space. `grid` fills
  `columns` equal cells. `fit` sizes an element to its text or children.
- **Canvas scaling**: `scale_with_screen` keeps layouts proportional across resolutions (the canvas
  scales from its reference resolution, blending the width and height ratios by `match`); `constant` is 1 px = 1 px.
- **Styles** cascade from the canvas `theme`, then its `styleSheet`, by widget kind, `.class`
  (the element's `style`) and `#Name`, then the element's `styleOverrides`. State blocks `hover`,
  `pressed`, `focus`, `checked`, `disabled` apply while active, with animated transitions.
  Properties: `background`, `background2` (vertical gradient), `backgroundImage`, `slice`
  (9-slice), `imageTint`, `radius`, `borderWidth`, `borderColor`, `shadowColor`, `shadowOffset`,
  `shadowBlur`, `opacity`, `padding`, `font`, `fontSize`, `color`, `textAlign`, `verticalAlign`,
  `bold`, `italic`, `lineSpacing`, `letterSpacing`, `textTransform`, `textOutline`,
  `textOutlineColor`, `textShadowColor`, `textShadowOffset`, `accent`, `track`, `knob`,
  `trackHeight`, `knobSize`, `placeholderColor`, `imageFit`, `transition`. Text properties inherit.
  Every theme defines the classes `primary ghost danger title heading subtitle muted small large
  accent card hud` and the dialogue parts `dialogue_box dialogue_name dialogue_text
  dialogue_choice dialogue_portrait dialogue_hint`.

```json
// ui/game.uistyle.json
{"format": "skywalker.uistyle", "extends": "parchment", "vars": {"accent": "#7a1f12"},
 "rules": {"button": {"radius": 2, "hover": {"borderColor": "$accent"}},
           ".stamp": {"background": "$accent", "color": "#fff3dc", "textTransform": "uppercase"},
           "#Title": {"fontSize": 72}}}
```

**Input**: the mouse (from the platform's input state), keyboard (Tab / arrows move focus,
Enter/Space activate, arrows adjust sliders, typing edits a focused input) and wheel (scroll views).
Activating a button/toggle/slider/input sends the Wander event `ui:<element name>` (plus the
element's `event`, if set) and `on click` to the element itself. While playing, clicks on UI never
reach the world behind it; while editing, clicking an element selects it.

## Wander

```wander
behavior MainMenu
  on ui "Play"                       -- the button named "Play" was clicked
    find("Main Menu").ui.visible = false
    start_dialogue("Intro")
  end
  on ui "Volume"
    log "volume " + str(find("Volume").ui.value)
  end
end

behavior Hero
  on tick
    if key("d") then
      self.sprite.flipX = false
      play_anim(self, "run")
    else
      play_anim(self, "idle")
    end
  end
  on anim "footstep"                 -- a frame event of the run clip
    log "step"
  end
end

behavior Farmer
  on key "e"
    let map = find("Farm")
    if tile_at(map, self.position) == 3 then   -- grass
      set_tile(map, self.position, "soil")     -- auto-tiled terrain
    end
  end
end

behavior Story
  on dialogue "open_gate"            -- the script ran <<open_gate>>
    destroy find("Gate")
  end
  on dialogue "end"
    find("HUD").enabled = true
  end
end
```

Builtins: `play_anim(e, clip, restart?)`, `tile_at(map, pos, layer?)`,
`set_tile(map, pos, id | "terrain", layer?)`, `start_dialogue(node)` / `start_dialogue(e, node)`,
`dialogue_var(name)` / `dialogue_var(name, value)`, `dialogue_choose(i)`, `dialogue_advance()`.
Triggers: `on ui "Name"` (= `on event "ui:Name"`), `on dialogue "start" | "line" | "choice" |
"end" | "<command>"` (= `on event "dialogue:..."`). Any component field is reachable:
`find("Score").ui.text = "Score: " + str(score)`.

## Dialogue scripts

A Yarn Spinner-style language: nodes, speakers, choices with indented bodies, conditions,
variables, jumps and commands.

```
title: Start
---
<<declare $trust = 1>>
<<declare $name = "Envoy">>
Narrator: The council chamber falls silent. #mood:tense
Vale: You're late, {$name}. #portrait:vale_cold
-> I was delayed by the riots.
    <<set $trust += 1>>
    Vale: Riots. Of course.
-> That is none of your concern. <<if $trust > 5>>
    <<jump Confrontation>>
<<if visited("Archive")>>
    Vale: I hear you've been reading old letters.
<<else>>
    Vale: Sit.
<<endif>>
<<lights_dim 0.5>>
===
title: Confrontation
---
Vale: Then we understand each other. #mood:cold
<<stop>>
===
```

Lines are `Speaker: text` with trailing `#tags` (`#portrait:name` shows `portraits/name.png` in
the default dialogue box; `#line:<id>` localizes the line or choice through the string tables, with
the script's text as the source-language fallback, see [LOCALIZATION](LOCALIZATION.md)). `{expr}` interpolates. Commands: `set` (`to = += -= *= /=`),
`declare`, `if/elseif/else/endif`, `jump`, `wait`, `stop`; anything else (`<<shake 2>>`) becomes
the Wander event `dialogue:shake` with the arguments in the runner's vars `dialogue_command`,
`dialogue_args`, `dialogue_arg`. Variables live in the runner entity's vars (`$trust` = var
`trust`). Functions: `visited`, `visited_count`, `random`, `random_range`, `dice`, `round`,
`floor`, `ceil`, `abs`, `min`, `max`. The built-in dialogue box (portrait, name plate, typewriter
text, numbered choices; click / Space / Enter advance, 1-9 pick) is made of ordinary UI entities
named `Dialogue Box`, `Dialogue Portrait`, `Dialogue Speaker`, `Dialogue Line`, `Dialogue Choices`
under the `Dialogue UI` canvas — restyle them with the `dialogue_*` classes or create them in the
editor with `ui_create {"template": "dialogue"}`. Set `ui: "none"` to draw your own from Wander
(`self.dialogue.line`, `.speaker`, `.choices`).

## Tools

| Tool | Use it to |
|---|---|
| `ui_create` | Build a UI from one JSON tree (or a template: main_menu, hud, pause_menu, settings, inventory, document, dialogue); returns ids and computed rects |
| `ui_style` | Switch a canvas theme, write style-sheet rules, set an element's classes or inline css |
| `ui_inspect` | Read computed rects, visibility, values and resolved styles for any viewport size |
| `ui_interact` | Click / set_value / type / scroll / focus an element, or click at a pixel |
| `dialogue_check` | Lint a script (syntax, missing nodes with did-you-mean, unreachable nodes, unset variables) |
| `dialogue_preview` | Simulate a branch path and read the transcript and final variables |
| `dialogue_control` | Start / advance / choose / stop / read a live conversation |
| `locale_list` / `locale_set` | Locales, coverage and the current one; preview or switch the language |
| `locale_check` | Missing / unused / undefined keys, placeholder and plural problems, overlong strings, uncovered glyphs |
| `locale_extract` / `locale_pseudo` | Turn hard-coded texts into keys; a pseudo-locale (accented, 30% longer) for layout tests |
| `sprite_atlas_pack` | Pack a folder of images into an atlas (trimmed, padded, extruded) + suggested clips |
| `sprite_sheet_slice` | Name the frames of a grid sheet, turn rows into clips, apply them to an entity |
| `tilemap_from_ascii` | Create or update a tilemap from an ASCII map and a legend (tile ids, terrains, names) |
| `tilemap_paint` | set / fill / flood / clear cells, with auto-tiling terrains |
| `tilemap_inspect` | ASCII view of every layer with a legend, plus merged collision rectangles |

Captures (`viewport_capture`) include sprites, tiles, world text and UI, with real screen boxes
for each — also on machines without a GPU (the CPU rasterizer draws the same Frame2D).

## Recipes

### A HUD

```json
ui_create {"canvas": {"name": "HUD", "theme": "dark"}, "elements": [
  {"type": "panel", "name": "Vitals", "anchor": "top_left", "position": [32, 28], "style": "hud",
   "layout": "column", "gap": 6, "padding": [10, 14], "children": [
     {"type": "text", "text": "HEALTH", "style": "small muted"},
     {"type": "progress", "name": "Health", "value": 1, "size": [240, 12]}]},
  {"type": "text", "name": "Score", "anchor": "top_right", "position": [-32, 28], "text": "0", "style": "large"}]}
```
then `find("Health").ui.value = hp / 100` and `find("Score").ui.text = str(score)` from Wander.

### A main menu

```json
ui_create {"template": "main_menu", "canvas": {"name": "Menu", "theme": "glass"}}
entity_update {"entity": "Title", "components": {"ui": {"text": "Ashes of Veyra"}}}
behavior_set {"entity": "Menu", "name": "Menu", "source": "behavior Menu\n  on ui \"Play\"\n    find(\"Menu\").enabled = false\n  end\nend"}
```
Verify with `ui_inspect {"canvas": "Menu"}`, then `sim_control play` and `ui_interact {"element": "Play"}`.

### A pause menu

UI canvases run while the game is paused (their `process` mode defaults to `always`, on the real
clock), so a pause menu is a canvas plus three handlers:

```json
ui_create {"template": "pause_menu", "canvas": {"name": "PauseUI", "theme": "glass"}}
entity_update {"entity": "Pause Dim", "components": {"ui": {"visible": false}}}
behavior_set {"entity": "PauseUI", "name": "Pause", "source": "on action \"pause\"\n  if is_paused() then resume_game() else pause_game() end\nend\non pause\n  find(\"Pause Dim\").ui.visible = true\nend\non resume\n  find(\"Pause Dim\").ui.visible = false\nend\non ui \"Resume\"\n  resume_game()\nend\n"}
```
`pause_game()` freezes every `pausable` entity (the default: behaviors, physics, animation,
particles, non-UI sounds) from the next tick. A HUD that should freeze too gets
`process: {mode: "pausable"}`. Verify with `sim_control {"action": "pause_game"}`,
`process_info {"entity": "PauseUI"}` (runs: true) and `ui_interact {"element": "Resume"}` + `sim_control step`.
See docs/ARCHITECTURE.md "Game pause, process modes and time scale".

### A dialogue scene

1. Write `story/intro.dialogue`, run `dialogue_check {"path": "story/intro.dialogue"}`.
2. `dialogue_preview {"path": "story/intro.dialogue", "choices": [0, "refuse"]}` to test branches.
3. `entity_create {"name": "Vale", "components": {"dialogue": {"script": "story/intro.dialogue", "autoStart": true}}}`.
4. Restyle: `ui_style {"canvas": "Dialogue UI", "theme": "parchment"}` (after it exists) or ship a
   style sheet with `.dialogue_box` rules; portraits go in `portraits/<name>.png`.
5. React in Wander with `on dialogue "<command>"` and read `dialogue_var("trust")`.

### A tilemap level from ASCII

```json
tilemap_from_ascii {"name": "Level", "tileset": "art/tiles.png", "tile_size": 16,
  "autotile": {"ground": {"mode": "blob47", "first": 1}},
  "legend": {"#": "ground", "=": 49, "o": 57}, "solid": true,
  "map": "....................\n.......o..o.........\n......======........\n....................\n####################"}
entity_create {"name": "Camera", "position": [10, -2, 10],
  "components": {"camera": {"orthographic": true},
                 "camera2d": {"pixelsPerUnit": 16, "referenceHeight": 180, "follow": "Hero"}}}
entity_create {"name": "Torch", "position": [6, -2, 0],
  "components": {"light2d": {"kind": "point", "color": "#ffb060", "intensity": 2.5, "radius": 6, "halo": 0.6, "flicker": 0.3}}}
```
`tilemap_inspect {"entity": "Level"}` lists the merged collision rectangles (map-local world
units: x right, y up from the top-left corner), ready for physics bodies.

Auto-tiling: `blob47` takes 47 tiles ordered by ascending reduced 8-neighbour mask (N=1, NE=2,
E=4, SE=8, S=16, SW=32, W=64, NW=128; corners count only with both adjacent edges), `wang16`
takes 16 tiles indexed by N=1 | E=2 | S=4 | W=8, `random` mixes variants (with `weights`),
`single` places one tile. Give `first` (consecutive ids) or an explicit `tiles` list.

### Sprite sheets and atlases

```json
sprite_sheet_slice {"image": "art/knight.png", "cell": [32, 32], "entity": "Knight",
  "animations": {"idle": {"row": 0, "fps": 6}, "run": {"row": 1, "fps": 12},
                 "attack": {"frames": "16-21", "fps": 14, "loop": false, "events": {"3": "hit"}}}}
sprite_atlas_pack {"folder": "art/props", "output": "art/props.atlas.json"}
```

## Limits

- 32 2D lights per frame (nearest to the view); 2D shadows are screen-space (casters must be on
  screen) and the CPU rasterizer lights per sprite without normal maps or shadows.
- Text shaping is per code point (kerning, no ligatures or complex scripts such as Arabic/Indic, no
  right-to-left layout).
- The UI blends in linear light on the GPU and in sRGB on the CPU (tiny differences on soft edges).
- World-space canvases are hit-tested through the game camera; focus navigation is order-based.
- The editor shows UI and sprites in the viewport (click selects them); for the exact 2D framing
  (orthographic, pixel-perfect, camera2d bounds) look through the scene camera. A dedicated 2D
  editing camera (pan/zoom on the XY plane) is not included yet.
