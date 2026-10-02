# Demo video pipeline

Everything in the Skywalker demo video comes from the engine, the editor, or real agents:

| Footage | Source |
|---|---|
| Sky Village | Built **live in the editor** by Claude agents connected over MCP: Cirro (level design), Aurora (lighting) and Stratus (Wander behaviors), working in parallel, with Nimbus (director) laying the foundation. The scene is `examples/sky_village`, with 69 attributed undo steps. |
| Timelapse | `record_session.py` connects to the editor's MCP socket as `recorder` and captures the viewport every 0.5 s, plus which entities exist at each frame. History attribution comes from the engine's history. |
| Engine shots | `render_footage.py` loads the scene headlessly, plays the simulation (the agents' behaviors), and captures frames along camera paths. |
| Sky Dash (2D) | `build_sky_dash.py` builds the level entirely through engine tools and Wander (`examples/sky_dash`). |
| Editor UI | Real screenshots of the running editor. |
| Voice | [Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M), voice `af_heart`, script in `narration.json`. |
| Music | Procedural (`music.py`), royalty-free by construction. |

## Rebuild

```bash
python3 media/demo/render_footage.py VILLAGE_PROJECT DASH_PROJECT WORK/footage
```

```bash
python compose.py WORK out/skywalker-demo.mp4
```

`WORK` must contain `footage/`, `rec_village/` (frames, `events*.jsonl`, `history.json`), `assets/` (avatars, editor screenshots, `behaviors.json`) and `vo/` (one WAV per narration id). Use `--preview` to write stills instead of a video.

Kokoro needs `pip install kokoro soundfile` in a venv on a **short path**: espeak-ng truncates long data paths.
