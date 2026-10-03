# Demo video pipelines

## Showcase video — `out/skywalker-showcase.mp4`

Everything in the video comes from the engine, the live editor, or the agents' own tool calls.

| Footage | Source |
|---|---|
| The games | `games/<id>.py`. Each is built by the crew through engine tools only. The kit (`kit.py`) gives each crew member their own MCP connection, so every edit is attributed (`mcp:Cirro`, `mcp:Aurora`, …). Results live in `examples/<id>`. |
| Live builds (timelapse grid) | `record_all.py` launches the editor on each example, starts `record_session.py`, which captures the editor viewport over its MCP socket every 0.25 s, and runs `showcase.py build <id> --attach --pace 1.0 --log edits.jsonl`. The compositor aligns frames and edits by wall clock. |
| Game shots | `showcase.py footage <id>`: the headless engine plays each game's Wander behaviors while cameras follow `shots()`. |
| Feature shots | `render_features.py`: a look-dev scene (presets + `texture_generate`), toon vs PBR on Cloudhopper, a time-of-day sweep on Harvest Fair, texture swatches, a `viewport_multi` capture. |
| Asset previews | `asset_preview` on the games' prefabs. |
| Photoscanned scenes | `smugglers_cove`, `hidden_alley`, `namaqua_canyon`: CC0 models, textures and HDRI skies from Poly Haven, resolved through its public API (`polyhaven.py`) and downloaded by the crew with `asset_download` (license, author and source land in each project's `CREDITS.md`). Downloads are git-ignored and fetched again on the first build. They also use the simulated effects: GPU fluid fire, FFT water, rain particles, volumetric light. |
| Voice | Kokoro-82M (`tts.py narration2.json vo/`), voice `af_heart`. |
| Music | Procedural (`music.py`). |
| Branding | `assets/brand` (icon and wordmark). |

### Rebuild

```bash
export SKY_CLI=$PWD/../../build/release/bin/skywalker
```

```bash
for g in $(python3 showcase.py list); do python3 showcase.py build $g && python3 showcase.py footage $g WORK/footage/$g; done
```

```bash
python3 render_features.py WORK
```

Record the live builds into `WORK/rec/<game>/`. The editor must be the release build, and the screen can be locked: capture goes through MCP, not the screen.

```bash
python3 record_all.py WORK
```

Then generate the voiceover with the Kokoro venv, on a **short path** (espeak-ng truncates long data paths):

```bash
/path/to/kokoro-venv/bin/python tts.py narration2.json WORK/vo
```

Compose (`--preview` writes stills instead of a video; `--only id,id` limits it to some segments):

```bash
python compose2.py WORK WORK/raw.mp4
```

Normalize loudness for social platforms, **forcing 48 kHz**. `loudnorm` silently upsamples to 96–192 kHz, which QuickTime and most apps play as silence:

```bash
ffmpeg -i WORK/raw.mp4 -c:v copy -af "loudnorm=I=-14:TP=-1.5:LRA=11" -ar 48000 -c:a aac -b:a 192k -movflags +faststart out/skywalker-showcase.mp4
```

Look-dev a single game quickly (one still per shot):

```bash
python3 showcase.py build abyss && python3 showcase.py stills abyss /tmp/look
```

## v0.0.1 demo video — `out/skywalker-demo.mp4`

The original Sky Village / Sky Dash video: `compose.py`, `render_footage.py`, `build_sky_dash.py`, `narration.json`. Sky Village was built live by Claude agents over MCP (69 attributed undo steps).
