# Shipping a game

A Skywalker project becomes a standalone macOS app: `Sky Dash.app` that opens a window, plays the start
scene, reads the keyboard, mouse and gamepads, plays sound and needs no editor, no engine install and no compiler.

- [Quick start](#quick-start)
- [game.json](#gamejson)
- [The player](#the-player)
- [Building an app](#building-an-app)
- [What gets packaged](#what-gets-packaged)
- [Native modules](#native-modules)
- [Agent tools](#agent-tools)
- [Testing without a display](#testing-without-a-display)
- [Signing and distribution](#signing-and-distribution)
- [License notices and privacy](#license-notices-and-privacy)
- [Limitations](#limitations)

## Quick start

```bash
cmake --preset release && cmake --build --preset release          # builds skywalker and skywalker-player
build/release/bin/skywalker build --project examples/sky_dash --out ~/Builds --release
open ~/Builds/"Sky Dash.app"
```

Without building anything, play a project folder directly:

```bash
build/release/bin/skywalker-player examples/sky_dash
```

For agents: `game_settings` (edit `game.json`), `game_build`, `game_run` (try it in the real player).

## game.json

`game.json` sits in the project root. Every field is optional; a project without one still builds (defaults, the
first scene, a generic bundle id). Unknown fields, wrong types and bad values are errors with a "did you mean" hint, so a
typo never silently ships the wrong thing.

```json
{
  "id": "sky_dash",
  "title": "Sky Dash",
  "genre": "2D Platformer",
  "mood": "bright",
  "pitch": "Run, hop, grab every coin.",
  "assets": "Poly Haven (CC0)",

  "startScene": "scenes/main.sky.json",
  "window": { "width": 1280, "height": 720, "fullscreen": false, "resizable": true, "vsync": true },
  "quality": "high",
  "renderScale": 0,
  "quitOnEscape": false,
  "pauseOnFocusLoss": true,

  "icon": "art/icon.png",
  "bundleId": "com.acme.skydash",
  "version": "1.0.0",
  "copyright": "© 2026 Acme Games",

  "include": ["data/**"],
  "exclude": ["audio/drafts/**"]
}
```

| Field | Default | Meaning |
|---|---|---|
| `id`, `title`, `genre`, `mood`, `pitch`, `assets` | | Description for people, agents and the studio. `title` is the window title and the app name. `assets` credits the art sources. |
| `startScene` | `scenes/main.sky.json`, else the first `*.sky.json` | Scene the game starts in. |
| `window.width`, `window.height` | 1280 x 720 | Content size in **points** (a Retina display renders twice the pixels). Clamped to the screen. |
| `window.fullscreen` | false | Start in native macOS full screen (also toggled with Ctrl-Cmd-F). |
| `window.resizable` | true | Whether the window can be resized. |
| `window.vsync` | true | `false` allows tearing for the lowest latency and asks for the display's top refresh rate. |
| `quality` | `high` | `low`, `medium`, `high`, `ultra`. Presets adjust the scene's environment when it loads: `low` renders at 0.67 scale (MetalFX upscaling), turns off GI and screen-space reflections, halves AO, flat clouds; `medium` 0.85 scale and half-strength GI; `high` keeps what the scene authored; `ultra` forces full resolution. |
| `renderScale` | 0 | Internal resolution 0.33..1 overriding the preset (0 = automatic). |
| `quitOnEscape` | false | Escape closes the game. Otherwise the game decides, typically with the `pause` action. Cmd-Q always works. |
| `pauseOnFocusLoss` | true | Pause the simulation and sound while another app is in front. |
| `icon` | none | A square PNG in the project, 1024 x 1024 recommended. Converted to `.icns` (all sizes) with `sips` and `iconutil`. Non-square images are center-cropped with a warning. |
| `bundleId` | `dev.skywalker.games.<id>` | Reverse-DNS identifier. Use your own for distribution. |
| `version` | `1.0.0` | Digits and dots; becomes `CFBundleShortVersionString` and `CFBundleVersion`. |
| `copyright` | none | Shown in the About panel. |
| `include` | none | Extra files or folders to ship (globs `*` and `?`; a folder name includes everything below it). For assets that scripts load by computed names. |
| `exclude` | none | Files to leave out even when referenced. The build warns about each referenced file you excluded. |
| `render` | none | `{"layers": {"1": "world", "2": "hero"}}` names the 20 render layers ([RENDERING](RENDERING.md#render-layers)); tools and Wander `layer_mask()` accept the names. |
| `mounts` | none | `{"kit": "../_kit"}`: shared folders outside the project, addressed as `kit/...` ([ASSETS](ASSETS.md#shared-kits-mounts-in-gamejson)). Names are letters, digits, `_` and `-`. |
| `localization` | source `en` | `{"source": "en", "locale": "fr", "useSystemLocale": true, "maxLengthRatio": 1.3}`: the game's language and whether a shipped game follows the player's system locale ([LOCALIZATION](LOCALIZATION.md)). |

Command-line flags of the player and `skywalker build` override the file.

## The player

`skywalker-player` is the same engine as the editor with a Cocoa window, no SwiftUI. A `CADisplayLink` drives every
display frame: poll gamepads, `Engine::update(dt)` (fixed 1/60 s simulation steps, audio), render, present.

- **Input.** Keys, the mouse (movement, three buttons, wheel, normalized cursor position, clicking entities fires
  `on click`) and up to four gamepads (GameController framework: Xbox, PlayStation, Switch Pro, MFi) feed the same
  `InputState` the editor and agents use, so `input.json` action maps work unchanged ([INPUT](INPUT.md)).
  Held keys are released when the window loses focus, so nothing sticks.
- **Cursor.** A script calls `cursor_lock(true)` for mouse look: the cursor is hidden and captured, movement arrives as
  `look`. It is given back when the app loses focus and when the game stops. `quit_game()` closes the game.
- **Focus.** With `pauseOnFocusLoss` the simulation and audio pause in the background and resume without a catch-up step.
- **Audio.** Everything in [AUDIO](AUDIO.md) works: components, one-shots, music, the mixer (`audio.json`).
- **Display.** Retina-aware drawable, MetalFX upscaling through `renderScale`, native full screen, a standard menu
  (About, Hide, Quit, Full Screen, Minimize).
- **Errors.** A scene that fails to load, a missing game or a renderer that keeps failing shows a dialog with the reason
  (and a "Copy Details" button) instead of vanishing. Crashes write a backtrace to
  `~/Library/Logs/SkywalkerGames/<Game>-crash.log` and then die normally, so macOS still files its report. Script runtime
  errors and `log` output go to stderr.
- **Signals.** SIGTERM and SIGINT quit like Cmd-Q (audio and cursor are restored).

```
skywalker-player [PROJECT_DIR | Game.app] [options]
  --scene FILE        start scene            --windowed | --fullscreen
  --width N --height N                       --no-vsync
  --quality low|medium|high|ultra            --render-scale 0.33..1
  --quit-on-escape    --no-audio             --verbose
  --check [TICKS]     headless validation, prints a JSON report
  --capture-frame PNG [--frames N]           render one frame of the real pipeline to a PNG and quit
  --quit-after SECONDS
  --agent-socket PATH serve MCP on a Unix socket so agents can inspect and drive the running game (development)
```

`--capture-frame` and `--quit-after` run with a fixed 60 Hz step instead of the display link, so they are deterministic
and work without a display attached. The capture is a read-back of the frame the player presented, at the window's pixel
size (2560 x 1440 for the default window on a Retina display).

## Building an app

```
skywalker build --project DIR --out DIR [--name N] [--icon F.png] [--release] [--all-assets]
                [--version X.Y.Z] [--bundle-id ID] [--scene FILE] [--no-sign] [--no-native]
                [--player PATH] [--dry-run] [--json]
```

- `--release` strips the player's symbols (smaller app) and tags the manifest.
- `--dry-run` lists exactly which files would ship and which references are broken; nothing is written.
- `--all-assets` ships every runtime file in the project, not only the referenced ones.
- The output is `DIR/<Name>.app`, built in a hidden staging folder and moved into place only when complete, so a failed
  build never destroys the previous one. A path that exists but is not an app bundle is never overwritten.
- The player comes from `$SKYWALKER_PLAYER`, `skywalker-player` next to the `skywalker` binary, or `--player`. The
  editor app carries one in `Skywalker.app/Contents/MacOS/`.

The report lists the app path, size, file count by type, the largest files and warnings: broken references, no icon,
an excluded file that is still referenced, a native module linking a library players may not have, a failed signature.

Build outside iCloud-synced folders (Documents, Desktop with iCloud Drive): the sync adds extended attributes to files
and breaks the code signature. The build strips them (`xattr -cr`), but later syncing can add them again.

## What gets packaged

Files of game.json `mounts` (a shared kit outside the project, see ASSETS.md) are copied into the package under the
mount name when the game references them; the shipped game.json has no mounts.

```
Sky Dash.app/Contents/
  Info.plist                 identity, version, icon, LSSupportsGameMode, high-resolution capable
  MacOS/Sky_Dash             the player
  Resources/AppIcon.icns
  Resources/build.json       manifest: identity, start scene, every shipped file with its byte size, native module
  Resources/Game/            the project payload (below)
  Frameworks/<module>.dylib  the project's compiled native module, if it has one
  _CodeSignature/
```

**Computed from references.** The build starts from the roots and follows references until nothing new appears:

- Roots: `game.json` (rewritten with the resolved start scene and no build-only fields), `input.json`, `audio.json`,
  `CREDITS.md`, `LICENSE*`, `NOTICE*`, **every scene** (scripts may switch scenes), every `.wander` file, the start scene,
  and `include` globs.
- Followed: every string in scene, prefab, material, controller, sequence, animation, `.meta` and glTF JSON that names a
  file (`"mesh": "asset:models/ship.obj"`, `"texture": "textures/wood.png"`, `guid:` references through the `.meta`
  sidecars, `#part` suffixes), every string literal in Wander code (`play_sound("audio/hit.wav")`,
  `spawn("prefab:prefabs/coin.prefab.json")`, `use "scripts/util.wander"`), including the behaviors embedded in scenes,
  glTF buffers and images, OBJ `mtllib`, MTL maps. Paths resolve from the project root, then from the referring file's folder.
- Each shipped asset takes its `.meta` sidecar along (GUIDs and import settings such as animation libraries).

**Never shipped:** hidden folders (`.git`, `.skywalker` caches), `build/`, `node_modules/`, `studio/` (board, feedback,
playtest reports), `agents/`, the `native/` sources, earlier `*.app` builds, and DCC or editor files
(`.blend`, `.psd`, `.kra`, `.zip`, backups, logs). Not even with `--all-assets`.

**When a script builds a path at run time** (`play_sound("audio/step_" + n + ".wav")`) the build cannot see it. Add
the folder to `include` in `game.json`, or build with `--all-assets`. The sound fails at run time with a clear error if
the file is missing.

## Native modules

If the project has `native/*.cpp` ([Wander native code](WANDER.md)), the build compiles it with the system compiler
(same flags as `native_build`) and ships the dylib in `Contents/Frameworks/`, signed with the app. The player loads
that library and never invokes a compiler, so players do not need Xcode. Libraries the module links must be system
libraries or you must bundle them: the build warns when `otool -L` shows anything else.

## Agent tools

Category `files`. `game_build` and `game_run` are open-world tools: MCP clients and the in-editor crew ask the human first.

| Tool | Use |
|---|---|
| `game_settings` | `get`: effective settings, resolved start scene, validation. `set`: merge fields into `game.json` (`null` removes one); invalid changes write nothing. |
| `game_build` | Build `Name.app` into `out`. `dry_run` lists what would ship. Options: `name`, `icon`, `version`, `bundle_id`, `scene`, `release`, `all_assets`, `sign`, `build_native`. |
| `game_run` | `start` the player on the project as a separate process (saved scene; `scene`, `fullscreen`, `width`, `height`, `quality`, `seconds`); returns the pid and a log file. `status` shows whether it runs and its recent output; `stop` ends it. `capture` writes one frame to a PNG and exits, a screenshot of what a player would see. `app` runs a built `.app`. |

A typical agent loop: `game_settings` to set the title and icon, `game_build {"dry_run":true}` to check for missing
assets, `game_run {"capture":"/tmp/shot.png"}` to look at the result, `game_build {"out":"~/Builds","release":true}`.

## Testing without a display

```
skywalker-player Game.app --check          # or: Game.app/Contents/MacOS/<exe> --check 300
```

Opens the game headlessly (no window, silent audio), verifies a bundle against its manifest (every file present with
the right size, executable bit, `Info.plist`, start scene, native module), plays N ticks (default 120), reports script
compile and runtime errors, renders a 160 x 90 test frame through the real renderer and prints a JSON report. The exit
code is 0 when everything works, 1 otherwise, so it fits CI:

```json
{ "name": "Sky Dash", "bundled": true, "start_scene": "scenes/main.sky.json", "entities": 141,
  "bundle": { "ok": true, "files": 2, "problems": [] }, "ticks": 120, "audio": "null",
  "render": { "backend": "metal", "ok": true, "contrast": 241, "visible_entities": 35 },
  "ok": true, "problems": [] }
```

`render.contrast` is the luminance range of the test frame: 0 means a flat image (nothing visible).

## Signing and distribution

A build is **ad-hoc signed** (`codesign -s -`): it runs on the Mac that built it, but Gatekeeper blocks a copy
that other people download ("cannot be opened because the developer
cannot be verified"). To distribute:

1. Join the Apple Developer Program and create a **Developer ID Application** certificate.
2. Sign the finished bundle with the hardened runtime, the native module first:
   ```bash
   codesign --force --options runtime --timestamp --sign "Developer ID Application: Your Name (TEAMID)" \
            "Game.app/Contents/Frameworks/"*.dylib
   codesign --force --options runtime --timestamp --sign "Developer ID Application: Your Name (TEAMID)" Game.app
   ```
   The hardened runtime enforces library validation. A native module compiled locally is signed by your identity
   like the app, so it loads; third-party dylibs must be signed with the same team.
3. Notarize and staple:
   ```bash
   ditto -c -k --keepParent Game.app Game.zip
   xcrun notarytool submit Game.zip --keychain-profile "notary" --wait
   xcrun stapler staple Game.app
   ```
4. Ship `Game.app` in a `.dmg` or the notarized `.zip`.

Mac App Store builds additionally need the App Sandbox entitlement and a provisioning profile; the player has not been
validated against the sandbox (project files are read from inside the bundle, nothing is written outside it).

## License notices and privacy

Every app `skywalker build` packages carries the license notices in `Contents/Resources/Licenses/`: Skywalker's
`LICENSE` (the runtime is inside the app), the font licenses and `THIRD-PARTY-NOTICES.md`. Keep them in the bundle
([LICENSING](LICENSING.md)).

A shipped game shows no Skywalker terms or consent screens: the game is the developer's product. The player collects and
sends nothing (no telemetry, analytics or network connections of its own; crash logs stay on the player's disk). If your
game adds networking, accounts, analytics, ads or AI features, you are the controller for that data: follow the checklist
in the [Privacy Notice](legal/PRIVACY.md#for-developers-who-ship-games). Agents can read the documents with `legal_info`.

## Limitations

- **macOS only** (Apple silicon or Intel with Metal). The deployment target is macOS 15.
- One scene is loaded per run; there is no scripted scene switch yet, though every scene is packaged.
- Dynamic asset paths need `include` or `--all-assets` (see above).
- AOT-compiled behaviors are not shipped: Wander runs on its bytecode VM in the player. Native modules are shipped.
- No in-game settings menu, save-game API or localization yet; games keep state in scene variables for a session.
- The player does not hot-reload scripts or assets. `--agent-socket` attaches agents for development inspection
  (screenshots, `sim_input`, entity queries) but should not be used in shipped builds.
- Gamepad rumble, keyboard layouts other than by produced character, IME text input and multiple windows or displays
  are not handled.
- `vsync: false` removes the sync with the display but the frame pacing stays link-driven.
