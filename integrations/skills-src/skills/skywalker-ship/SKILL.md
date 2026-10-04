---
name: skywalker-ship
description: Ship a Skywalker game as a standalone macOS app - game.json settings (title, window, quality, icon, bundle id), game_settings, dry-run packaging checks, game_build, trying the real player with game_run and frame captures, the skywalker build CLI, headless --check validation, signing and notarization notes. Use when the human wants to package, release, test as a player, or hand a game to someone else.
---

# Shipping a game

Load skywalker-core first. Engine doc: `skywalker://docs/SHIPPING`. A project becomes `Name.app`: the player (same engine as the editor, Cocoa window, no editor needed), the scenes, the **referenced** assets, scripts, the compiled native module,
`Info.plist`, an icon and an ad-hoc signature. macOS only (Apple silicon or Intel with Metal, deployment target macOS 15).

`game_build` and `game_run` are open-world tools (they write files and start processes outside the project): the human approves them in MCP clients. Ask up front, with a one-line description of what you will build and where.

## The loop

1. **Save the scene** (`scene_save`): the player runs the saved scene file, not your in-memory edits.
2. **Settings**: `game_settings` (`get` shows effective settings, the resolved start scene and validation problems; `set` merges fields into `game.json`, `null` removes one; an invalid change writes nothing).
3. **Dry run**: `game_build {dry_run:true}` lists exactly which files would ship and which references are broken. Fix `missing` before building.
4. **Look like a player**: `game_run {capture:"shots/player.png"}` renders one frame of the real player pipeline and exits; `game_run {action:"start"}` opens a real window (real input, audio); `status` shows recent output; `stop` ends it.
5. **Build**: `game_build {out:"~/Builds", release:true}`. Read the report: app path, size, file counts, largest files, warnings (broken references, no icon, excluded-but-referenced files, a native module linking non-system libraries, failed signature).
6. **Validate headlessly**: `Game.app/Contents/MacOS/<exe> --check` (see below). Exit 0 means the bundle, scripts and a rendered test frame are fine.

```text
scene_save {path:"scenes/main.sky.json"}
game_settings {operation:"set", settings:{id:"sky_dash", title:"Sky Dash", genre:"2D Platformer", pitch:"Run, hop, grab every coin.", startScene:"scenes/main.sky.json", window:{width:1280, height:720, fullscreen:false, resizable:true, vsync:true}, quality:"high", icon:"art/icon.png", bundleId:"com.acme.skydash", version:"1.0.0", copyright:"(c) 2026 Acme Games"}}
game_settings {operation:"get"}
game_build {dry_run:true}
game_run {capture:"shots/player.png", frames:120}
game_run {action:"start", scene:"scenes/level2.sky.json", width:1280, height:720, quality:"medium", seconds:30}
game_run {action:"status"}
game_run {action:"stop"}
game_build {out:"~/Builds", name:"Sky Dash", release:true, version:"1.0.0", bundle_id:"com.acme.skydash"}
```

## game.json (all fields optional; unknown fields and wrong types are errors with did-you-mean)

| Field | Default | Meaning |
|---|---|---|
| `id`, `title`, `genre`, `mood`, `pitch`, `assets` | | Description for people, agents and the studio; `title` is the window title and app name; `assets` credits the art sources |
| `startScene` | `scenes/main.sky.json`, else the first `*.sky.json` | The scene the game starts in |
| `window.width`, `window.height` | 1280 x 720 | Content size in **points** (Retina renders 2x pixels), clamped to the screen |
| `window.fullscreen`, `resizable`, `vsync` | false, true, true | Native full screen (Ctrl-Cmd-F toggles); `vsync:false` allows tearing for lowest latency |
| `quality` | `high` | `low` (0.67 scale via MetalFX, no GI/SSR, half AO, flat clouds), `medium` (0.85 scale, half GI), `high` (as authored), `ultra` (full resolution) |
| `renderScale` | 0 | Internal resolution 0.33..1 overriding the preset (0 = automatic) |
| `quitOnEscape` | false | Escape closes the game; otherwise the game decides (typically a `pause` action). Cmd-Q always works |
| `pauseOnFocusLoss` | true | Pause simulation and sound while another app is in front |
| `icon` | none | Square PNG in the project, 1024 x 1024 recommended (converted to `.icns`; non-square is center-cropped with a warning) |
| `bundleId` | `dev.skywalker.games.<id>` | Reverse-DNS; use your own for distribution |
| `version`, `copyright` | `1.0.0` | Digits and dots; shown in the About panel |
| `include`, `exclude` | none | Extra files/folders to ship (globs `*` `?`; a folder includes everything below), files to leave out even when referenced (the build warns for each referenced file you excluded) |

`game_build` arguments override the file for one build (`name`, `icon`, `version`, `bundle_id`, `scene`); `release:true` strips the player's symbols; `all_assets:true` ships every runtime file; `sign:false` skips the ad-hoc signature; `build_native:false` skips the native module.

## What gets packaged

- **Computed from references.** Roots: `game.json`, `input.json`, `audio.json`, `CREDITS.md`, `LICENSE*`, `NOTICE*`, every scene (scripts may switch scenes), every `.wander` file, the start scene and `include` globs. The build follows every string in scenes, prefabs, materials,
  controllers, sequences, animations, `.meta` and glTF JSON that names a file, and every string literal in Wander code (`play_sound("audio/hit.wav")`, `spawn("prefab:prefabs/coin.prefab.json")`, `use "scripts/util.wander"`). `.meta` sidecars come along.
- **Never shipped**: hidden folders (`.git`, `.skywalker` caches), `build/`, `studio/`, `agents/`, `native/` sources, earlier `*.app` builds, DCC/editor files (`.blend`, `.psd`, `.kra`, `.zip`, backups, logs). Not even with `all_assets`.
- **Computed paths**: `play_sound("audio/step_" + n + ".wav")` is invisible to the build. Add the folder to `include` (`game_settings set {include:["audio/**"]}`) or build with `all_assets:true`. Otherwise the sound fails at run time.
- **Native modules** (`native/*.cpp`, see skywalker-wander) are compiled and shipped in `Contents/Frameworks/`, signed with the app. They must link only system libraries, or you bundle the rest. **AOT-compiled behaviors are not shipped**; the player runs the VM.

## CLI

```bash
skywalker build --project DIR --out DIR [--name N] [--icon F.png] [--release] [--all-assets] [--version X.Y.Z] [--bundle-id ID] [--scene FILE] [--no-sign] [--no-native] [--player PATH] [--dry-run] [--json]
skywalker-player PROJECT_DIR                                  # play a project folder directly, no build
skywalker-player Game.app --check [TICKS]                     # headless validation: bundle vs manifest, N ticks (default 120), script errors, a 160x90 test render; JSON report, exit 0 = ok
skywalker-player PROJECT_DIR --capture-frame out.png --frames 120   # render one frame of the real pipeline and quit (deterministic fixed 60 Hz step, works without a display)
skywalker-player PROJECT_DIR --quality low --windowed --width 1280 --height 720 --quit-after 10 --verbose
```

The player comes from `$SKYWALKER_PLAYER`, `skywalker-player` next to the `skywalker` binary, or `--player`. `--agent-socket PATH` serves MCP from the running game for development (never ship with it).
Player flags override `game.json`. `render.contrast` in the `--check` report is the luminance range of the test frame: 0 means a flat image (nothing visible).

## Signing and distribution

A build is **ad-hoc signed**: it runs on the Mac that built it, but Gatekeeper blocks downloaded copies. For distribution the **human** needs an Apple Developer ID certificate: sign the dylibs then the bundle with the hardened runtime, notarize with `xcrun notarytool`,
`xcrun stapler staple`, and ship a `.dmg` or notarized `.zip` (commands in `skywalker://docs/SHIPPING`). Do not attempt these steps without the human's credentials, and never type certificates or passwords yourself. Mac App Store builds additionally need the App Sandbox entitlement (not validated).
**Build outside iCloud-synced folders** (Documents, Desktop with iCloud Drive): sync adds extended attributes that break the signature (the build strips them with `xattr -cr`, but syncing can add them back).

## Pre-release checklist

- `game_settings get` reports no problems; title, icon, version, bundle id are the real ones.
- `game_build {dry_run:true}`: `missing` empty, nothing important in `excluded_but_referenced`, dynamic asset folders covered by `include`.
- `game_run {capture:...}` shows the title screen/first frame correctly (look at the PNG); `game_run start` plays a full loop with the keyboard (and a gamepad if supported) and the pause/quit path works.
- `--check` passes on the built `.app`; `CREDITS.md` lists every downloaded asset with its license (skywalker-assets).
- The game works at several window sizes and at `quality:"low"`.

## Pitfalls

- `no_player` from `game_run` / `game_build`: the `skywalker-player` executable was not found. Build it (`cmake --build build/release`, which builds `skywalker` and `skywalker-player`) or set `SKYWALKER_PLAYER`.
- `game_build {dry_run:true}` reports `missing:[{reference, in}]` (for example a `game.json` icon path that does not exist): fix or remove the reference before the real build. Capture paths (`capture`) are project-relative or absolute; open the PNG to look at it.
- The player runs the **saved** scene: forgetting `scene_save` ships/plays stale content.
- One scene is loaded per run (no scripted scene switch yet, although every scene is packaged); no in-game settings menu, save-game API or localization yet; no hot reload in the player.
- Broken references after moving files: fix them (`dry_run` lists them) rather than `all_assets`.
- Do not use `--agent-socket` or `game_run` windows on the human's machine without telling them; `game_run` opens a real window and captures input.
- Gamepad rumble, IME input and multiple windows/displays are not handled.
