# Movie Render Queue

Offline, deterministic rendering of cinematics to video files and image sequences: Skywalker's
equivalent of Unreal's Movie Render Queue and Unity's Recorder. Agents use the `movie_render` tool,
humans use **Game ▸ Render Movie…** (⌥⌘M) or the film button of a Sequencer, and scripts use
`skywalker movie`.

```text
movie_render {sequence: "Intro", output: "renders/intro.mp4", resolution: "1080p", fps: 24, samples: 8, shutter: 0.5}
```

```bash
skywalker movie scenes/main.sky.json --project my_game --sequence Intro \
    -o renders/intro.mp4 --codec hevc -o renders/intro.mov --resolution 1080p --samples 16 --shutter 0.5
```

Every render starts from the scene state and leaves it exactly as it was: the scene is snapshotted
(play), rendered, and restored (stop), whatever happens (finish, error, cancel).

## What to render

| Source | Arguments | Length |
|---|---|---|
| A sequence | `sequence`: the entity with a `sequencer`, or a `*.sequence.json` (a temporary player plays a sequence nobody in the scene uses) | the sequence's length (`start` / `end` / `duration` pick a range) |
| An inline camera move | `camera`: `{keys: [...]}` or `{shots: [...]}` (below) | the end of the last key / shot |
| The scene camera | nothing (or `camera_entity`) | `duration` seconds |

Movie time 0 is the start of the simulation and of the sequence. A range that starts later pre-rolls
the simulation to it (ticks without rendering), so frame 120 of a range render is identical to frame
120 of the full render.

`simulate: true` runs the whole game while rendering (Wander scripts, physics, navigation): boats bob
on the waves, scripted lights flicker, physics props fall. The default (`false`) runs only the
cinematic systems: sequences, animators and particles. Water, sky, clouds, GPU particles, fluids and
hair always follow the movie clock.

### Inline camera paths

Keyframes are splined (Catmull-Rom, ease `auto`) unless a key gives another `ease`:

```json
{"camera": {"keys": [
    {"t": 0,   "eye": [-2.5, 2.7, -27.5], "target": [2, 2.1, -22], "fov": 46},
    {"t": 1.6, "eye": [-6, 4.2, -25],     "target": [6, 2.4, -4]},
    {"t": 4,   "eye": [-11, 9, -9],       "target": [20, 4, 38], "fov": 50, "roll": 0}],
  "aperture": 2.8, "focus_distance": 12}}
```

Shots are the `sequence_camera_shot` kinds (orbit, dolly, crane, track, pan, static, path, flyover)
with the same fields. They play back to back unless they give `t`; every shot start is a cut:

```json
{"camera": {"shots": [
    {"shot": "flyover", "target": "Village", "duration": 6, "distance": 40, "height": 15},
    {"shot": "orbit", "target": "Well", "offset": [0, 1, 0], "duration": 4, "radius": 8, "from": 0, "to": 70}]}}
```

## Image

| Argument | Default | |
|---|---|---|
| `fps` | 24 | frames per second (1-120) |
| `resolution` / `width`, `height` | 1080p | 720p, 1080p, 1440p, 4k; up to 3840x2160 |
| `samples` | 8 | the per-frame sample budget: anti-aliasing, noise-free GI and reflections, motion blur |
| `shutter` | the camera's `motionBlur` (0 for inline paths) | open shutter as a fraction of the frame interval (0.5 = 180-degree film look) |
| `shutter_samples` | one per sample | sub-frames across the shutter |
| `shutter_timing` | `center` | the open interval centered on the frame time, or `open` / `close` there |
| `quality` | `full` | `balanced` / `fast` for quick previews |
| `clay`, `debug_view` | off, `final` | the same move as matte clay or as a pencil sketch (`debug_view: "sketch"`), for "sketch → clay → final" transitions: the frames line up exactly |
| `warmup` | 4 | frames rendered (not written) before the first one so temporal state settles |

**Sample allocation.** Without a shutter, all `samples` are jittered sub-pixel samples accumulated
inside one render (like a `viewport_capture` still). With a shutter, the budget is spread over time:
`shutter_samples` sub-frames (default: one per sample), each with `samples / shutter_samples` spatial
samples. Sub-frames are stratified over the open shutter and accumulated on the CPU in float. Their
jitter continues the Halton sequence, so a frame covers distinct sub-pixel positions.

## Outputs

`output` (one path) or `outputs` (several, from one render). The extension picks the codec; `codec`
overrides it (for `output`; items of `outputs` may be `{path, codec, bitrate_mbps}`).

| Path | Codec | Notes |
|---|---|---|
| `renders/x.mp4` | H.264 High (`h264`) | 8-bit 4:2:0, plays everywhere; ~0.3 bit/pixel/frame (1080p24 ≈ 15 Mbit/s cap) |
| `renders/x.mp4` + `codec: "hevc"` | HEVC Main10 | 10-bit 4:2:0, ~2/3 of the H.264 bitrate |
| `renders/x.mov` | ProRes 422 HQ (`prores`) | 10-bit 4:2:2 editing master |
| `renders/x.mov` + `codec: "prores4444"` | ProRes 4444 | 12-bit 4:4:4 (no alpha yet) |
| `renders/x/`, `renders/x_####.png`, `renders/x/f_%04d.png` | PNG sequence | absolute frame numbers (`frame_0120.png` is frame 120 at any range) |

Video is encoded natively with AVFoundation (`AVAssetWriter` + VideoToolbox, in
`engine/platform/metal/MovieWriter.mm` behind `skywalker/render/MovieWriter.h`); there is no ffmpeg
in the engine. H.264 is fed 8-bit BGRA, HEVC and ProRes 16-bit RGB so accumulated sub-frames keep
their extra precision. Files are tagged Rec.709 (primaries, transfer and matrix). Builds without
AVFoundation (other platforms, or `-DSKY_WITH_AVFOUNDATION=OFF`) render PNG sequences only and
report `unsupported_codec` for video.

## Long renders

- **Progress**: every finished frame emits a `movie_progress` engine event (frame, frames, percent,
  ms per frame, ETA, outputs); the end emits `movie_finished` with the summary. The editor shows them
  as a progress bar over the viewport (which displays the frames as they render); the CLI prints a
  progress line.
- **Background**: `background: true` returns at once; the engine's update loop then renders one
  sub-frame per editor frame, so the editor stays responsive and agents keep working.
  `movie_render {action: "status"}` reports progress or the last summary.
- **Cancel**: `movie_render {action: "cancel"}`, the editor's Cancel button or Ctrl-C in the CLI stop
  after the current sub-frame. Written PNG frames stay; a video is finished as a valid, shorter file.
- **Resume**: `resume: true` (PNG sequences only) continues at the first missing frame. Frames are
  written to a temporary file and renamed, so an interrupted render never leaves a truncated frame.
  The simulation pre-rolls to the resume point, so resumed frames are identical to an uninterrupted
  render.
- **Summary**: state, frames, size, fps, duration, camera, shutter, sample split, elapsed time,
  average ms per frame and the output paths.

## Sub-frame time

The simulation keeps its fixed 1/60 s tick. A movie render does not use a finer tick: that would make
scripts and physics diverge from the game being recorded (and from every other render of it).
Instead, a sub-frame at movie time τ runs the simulation to the first tick at or after τ and shows
the state between that tick and the previous one:

- **Transforms** of every entity are interpolated between the two tick states (position and scale
  lerp, rotation slerp, short way round). Jumps longer than 25 m in a tick (teleports, respawns) are
  not smeared. Interpolating local transforms keeps hierarchies (wheels on cars, blades on hubs)
  rigid.
- **Sequences** (property, camera-cut and shot tracks) are evaluated exactly at τ on top, so camera
  moves are smooth curves rather than 60 Hz polylines and cuts land exactly. Event and animation
  tracks stay with the simulation.
- **Inline camera paths** are evaluated exactly at τ.
- **The effects clock** (water waves, sky and clouds, GPU particles, fluids, hair) is set to τ.
- **CPU particles** are moved along their velocity from the tick to τ.

Everything is restored right after the sub-frame renders, so the simulation never sees render-time
state. Skeletal poses update at 60 Hz (limitation below).

The transform part is the same code real-time frames use for render interpolation
(`TransformHistory` + `ScopedInterpolation`, `engine/Interpolation.h`; docs/ARCHITECTURE.md
"Render interpolation"), so entities with `process.interpolation: off` snap to ticks in movies too.
Cosmetic `on frame` Wander handlers (camera shake, bobbing) run for every sub-frame with `time` = τ and
are undone after it, like in real-time frames.

## Temporal state, cuts and exposure

Offline frames are rendered with `FrameData::offline`: every render is an independent accumulation
(no TAA history, even with one sample per sub-frame), the post-process camera motion blur is off (the
real shutter replaces it), and auto exposure adapts by the movie's frame time instead of snapping per
frame like a still: it moves continuously at the scene's `adaptationSpeed`, without pumping. The first
frame and every cut (`resetHistory`) re-meter exposure and drop temporal history, so a cut never
ghosts or inherits the previous shot's exposure. Cuts are known in advance from sequences (camera-cut
keys and shot starts where the shown camera or shot actually changes, also each loop wrap) and from
inline shot lists. Sub-frames never straddle a cut: a frame's shutter is clamped to its own shot. In
scene-camera renders, a change of the primary camera counts as a cut.

## Determinism

Renders start from the canonical scene (its saved JSON form, which is also what every play session
restores), seed the simulation the same way, and step it identically, so two renders of the same clip
produce identical frames on the CPU renderer (tested), and range or resumed renders match full ones.
GPU renders are deterministic up to the GPU's own floating point (parallel reductions in exposure
metering and simulations).

## GPU safety

Each sub-frame goes through `Engine::capture`, which holds the machine-wide GPU job lock
(`~/.skywalker/gpu.lock`) for that sub-frame only, so renders from several processes interleave
instead of stacking up, and the renderer commits one command buffer per accumulated sample: no clip,
frame or sub-frame ever becomes a single long GPU job that could trip the system watchdog. If a frame
faults on the GPU (watchdog timeout, page fault), readback reports `gpu_error` and the renderer
switches to safe mode; the movie then renders that whole frame again once with half the spatial
samples, and stops (`failed`, with the error) if it faults again. A faulted frame is never written;
the summary counts retries in `gpu_retries`.

## Performance

Measured on an M1 Pro (examples/smugglers_cove: HDRI sky, FFT ocean, volumetric campfire, embers,
two ships, `simulate: true`), 1920x1080, 24 fps, 4 s, `samples: 8`, `shutter: 0.5` (8 sub-frames per
frame), HEVC + ProRes outputs: about 230 ms per frame, 30-45 s for the clip including loading.
Clay and sketch versions of the same move cost the same.

## Editor

**Game ▸ Render Movie…** (⌥⌘M), or the film button on a Sequencer's scrub bar, opens a compact form:
source (a sequence or the scene camera with a duration), format, output path, size and rate, samples,
motion blur (shutter angle), look (final, clay, sketch) and simulate. It starts a background render;
the viewport shows the frames as they are rendered, with progress, ETA and Cancel above it.

## Limitations

- Skinned characters are posed at 60 Hz: within one shutter interval at 24 fps their pose changes at
  most twice, so very fast limbs blur in two steps.
- Sub-frames are accumulated after tone mapping (like Unreal's MRQ temporal samples), so very bright
  highlights streak a little dimmer than an in-HDR accumulation would.
- No alpha channel (ProRes 4444 is encoded opaque); no audio track.
- The film grain pattern changes per sub-frame and averages out under motion blur.
- A frame retried after a GPU fault is rendered at the simulation state reached so far, which can be
  up to one tick late for its earliest sub-frames.
