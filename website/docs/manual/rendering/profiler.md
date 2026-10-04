# Profiler

The profiler tells you where a frame's time goes: GPU milliseconds for every render, compute and blit pass, the same
summed by area (shadows, main, GI, reflections, clouds, post...), and CPU scopes for building and encoding the frame.
It is on by default and available to agents through `perf_stats`, to the editor through the
viewport's stats overlay, and to embedders through the C API. Benchmarks render the current view, any camera or any
quality tier, so you can measure a change before and after.

<figure markdown>
![A mountain temple surrounded by dense forest](../../assets/images/shots/ashen_peaks/establishing.webp){ loading=lazy }
<figcaption>ashen_peaks: thousands of instanced trees over an eroded terrain. Its foliage cost was measured and reduced with the per-pass timeline described on this page.</figcaption>
</figure>

## Concepts

### What is measured

| Source | How | Where it shows up |
|---|---|---|
| GPU passes | Every render, compute and blit pass samples GPU timestamps at its stage boundaries; a frame's samples are resolved when it completes and kept as rolling 60-frame statistics per pass | `profile.passes`, `profile.groups` |
| CPU scopes | `SKY_PROFILE_SCOPE("area.name")` times a block on the calling thread, with rolling 60-sample statistics | `profile.cpu` |
| Frame totals | Command-buffer GPU time, CPU frame time, draw calls, triangles, lights, entities | `perf_stats`, the stats overlay, `sky_frame_stats` |

### perf_stats

`perf_stats` without arguments reports the cost of the last frames and the scene's complexity: GPU and CPU frame
time, draw calls, triangles, lights, terrain nodes, foliage instances (meshes and impostors), entities, behaviors,
assets and effect timings. Arguments turn it into a benchmark:

| Argument | Meaning |
|---|---|
| `frames` | Render this many real-time frames first (temporal AA, no supersampling) and report average, minimum and maximum GPU time. 16.6 ms is 60 fps. |
| `width`, `height` | Benchmark resolution (default 1920×1080) |
| `view` | `"editor"` (default), `"scene"` (the game camera) or a custom camera `{"eye": [...], "target": [...], "fov": 50}` |
| `quality` | `full` (default, as in play mode), `balanced` or `fast`: benchmark an editor viewport tier |
| `passes` | Add the per-pass GPU timeline and CPU scopes as `profile` |

With `frames` above 0 the timeline is reset first, so `profile` covers exactly the benchmark frames.

### The profile

| Key | Contents |
|---|---|
| `profile.passes` | `[{pass, group, ms, avgMs, minMs, maxMs, vertexMs, count, seen}]` in encode order. `ms` is the latest frame; `count` merges repeated encoders (bloom levels, still sub-samples); `seen` is how many frames in the window the pass ran in (the sky bake runs once). |
| `profile.groups` | Milliseconds per frame by area: `shadows`, `main`, `ao`, `ssgi`, `ssr`, `resolve`, `effects`, `volumetrics`, `clouds`, `temporal`, `upscale`, `post`, `foliage`, `particles`, `hair`, `skinning`, `environment`, `2d`, `ui`, `overlays`, `debug` |
| `profile.cpu` | CPU scopes: `frame.build`, `scene.buildFrame`, `world.gather` (terrain and foliage chunks), `particles.gather`, `2d.gather`, `render.encode`, `render.readback` (waits for the GPU), `render.present`, `sim.step` |
| `spanMs`, `sumMs`, `frameGpuMs` | First to last sample; the sum of `groups`; the command buffer's GPU time |
| `droppedPasses` | Passes beyond the 4096-sample buffer (normally 0) |

**Fragment versus vertex time.** A render pass's time is its fragment span. Apple GPUs run the vertex work of later
passes early, overlapped with earlier fragment work, so vertex spans would overlap and count twice; they are reported
separately as `vertexMs` for geometry-heavy passes (`Main`, `Shadow cascades`). Fragment spans run in sequence, so
`sumMs` is close to `frameGpuMs`. Foliage and terrain draw inside `Main` and `Shadow cascades`; their own passes are
the culling compute (`Foliage cull`) and argument clears. MetalFX upscaling encodes its own work and is not sampled.

Other sections of `perf_stats` cover specific features: `gpu.lights` (`total`, `layerMasked`, `negative`,
`inverseSquare`), `gpu.velocity` (moving and tracked draws, teleports, mip bias, motion blur tile size), foliage
impostor statistics (`impostorInstances`, `meshInstances`, `impostorsBaked`, `impostorBakeMs`, memory) and
`frameFlow` (render interpolation alpha and frame pacing jitter).

## How to profile a view

=== "Tool call"

    ```tool
    perf_stats {}
    perf_stats {"frames": 30, "passes": true}
    perf_stats {"frames": 30, "passes": true, "view": "scene", "quality": "balanced"}
    perf_stats {"frames": 30, "passes": true, "view": {"eye": [0, 300, -600], "target": [0, 80, 0], "fov": 50}}
    ```

=== "CLI"

    ```bash
    skywalker call perf_stats '{"frames": 30, "passes": true, "view": "scene"}' --project my_game --scene scenes/main.sky.json
    ```

=== "C++"

    ```cpp
    #include "skywalker/core/Profiler.h"

    void updateWeather(float dt) {
        SKY_PROFILE_SCOPE("weather.update");  // shows up in perf_stats -> profile.cpu
        // ... work ...
    }
    ```

In the editor, the viewport's stats overlay shows frames per second and CPU and GPU milliseconds; click its frame line
to expand the same per-pass list. Embedders read the same data as JSON with `sky_frame_stats` from the
[C API](../../reference/capi.md).

<div class="sky-placeholder"><strong>Editor screenshot</strong>assets/editor/stats-overlay.webp · The viewport stats overlay expanded: fps, CPU and GPU ms, and the per-pass GPU list with Main, Shadow cascades, SSGI and Bloom</div>

## Recipe: find and fix a slow frame

1. **Measure.** Benchmark the view players see and read `profile.groups` for the expensive area, then
   `profile.passes` for the pass:

    ```tool
    perf_stats {"frames": 30, "passes": true, "view": "scene"}
    ```

2. **`main` or `shadows` high:** look for overdraw and missing LODs. Stacked transparent quads and dense grass show
   up warm in `overdraw`; in `lod`, red and magenta near the camera are fine, green far away means LODs are missing or
   a bias is off.

    ```tool
    viewport_capture {"debug_view": "overdraw", "samples": 1}
    viewport_capture {"debug_view": "lod", "samples": 1}
    ```

3. **Many lights:** in `light_complexity`, orange and red areas evaluate 7 or more lights per pixel; shorten `range`,
   merge lights or use `distanceFade`.

    ```tool
    viewport_capture {"debug_view": "light_complexity", "samples": 1}
    ```

4. **`ssgi`, `ssr`, `clouds` or `volumetrics` high:** lower `gi`, `ssr`, cloud settings or `godRays`, or try
   `renderScale` 0.67 (MetalFX reconstructs the output) and compare the `balanced` tier.

    ```tool
    environment_update {"gi": 0.7, "godRays": 0.6, "renderScale": 0.67}
    perf_stats {"frames": 30, "passes": true, "view": "scene", "quality": "balanced"}
    ```

5. **Repeat step 1** and compare `profile.groups` with the first run.

For textures, `texel_density` should be mostly green (512 texels per meter)
near the camera, and `uv_checker` shows stretched or flipped UVs (see [Debug views](debug-views.md)).

### Effects, hair and startup

| Question | Tool |
|---|---|
| What do my particles, fluids and hair cost per frame? | `fx_benchmark {"width": 1920, "height": 1080, "frames": 120}` renders frames back to back with effects time advancing and reports GPU time per frame and per effects pass; `serial: false` measures throughput instead |
| How many GPU particles are alive, how big are the grooms? | `fx_stats {}` after rendering a few frames |
| What does one groom cost? | `groom_info {"entity": "Hero"}`: strands, memory, generation time and measured GPU cost (keep a hero head under about 2 ms at 1080p) |
| How long does the renderer take to start? | `engine_info {}`: `shaderCompileMs`, renderer `startupMs`, how the shader library loaded, pipeline cache hits and misses |

### Measured examples

From the engine's own measurements on an M1 Pro at 1920×1080, full quality, 30-frame `perf_stats`:

| Scene and view | GPU frame time |
|---|---|
| ashen_peaks, aerial view, with foliage impostors | 19 ms (7 ms in the `fast` editing tier) |
| ashen_peaks, mid-valley | 39 ms, 7.7 M triangles (10 ms `fast`) |
| ashen_peaks, ground level | 63 ms, 18 M triangles (19 ms `fast`) |
| hello_sky | about 6.35 ms, of which the velocity buffer adds 0.2 to 0.4 ms |

`renderScale` 0.67 makes a frame about 25% faster on the same machine.

## Environment switches

Renderer switches for benchmarking and debugging, set in the environment of the process:

| Variable | Effect |
|---|---|
| `SKY_GPU_PROFILER=0` | No per-pass GPU timestamps |
| `SKY_GPU_CULL=0` | Use the CPU path for foliage culling instead of the GPU compute pass |
| `SKY_SHADER_SOURCE=1` | Compile the embedded shader source even when a precompiled `.metallib` is embedded |
| `SKY_SHADER_METALLIB=<file>` | Load the shader library from this `.metallib` (falls back to source for missing functions) |
| `SKY_SHADER_CACHE=0`, `SKY_SHADER_CACHE_DIR=<dir>` | Disable or move the pipeline binary archive |
| `SKY_SHADER_NONCE=<text>` | Change the library source by a comment, forcing a cold shader compile for startup benchmarks |

`MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` (Metal validation) turn the pipeline archive off automatically.

```bash
SKY_SHADER_NONCE=cold1 skywalker call engine_info '{}' --project my_game
```

## Instruments and Xcode

For deeper work, the platform tools see everything the engine's profiler does and more:

- **CPU:** Instruments' Time Profiler on the editor, or from the command line:

    ```bash
    xcrun xctrace record --template 'Time Profiler' --launch -- ./build/release/bin/skywalker run examples/hello_sky/scenes/main.sky.json --ticks 600
    ```

- **GPU:** Instruments' Metal System Trace, or Xcode's **Debug › Capture GPU Workload** attached to the editor.
- **Memory:** `leaks --atExit -- ./build/debug/bin/skywalker run examples/hello_sky/scenes/main.sky.json --ticks 600 -o out.png`
  should report 0 leaks.

## Pitfalls

- **Benchmark the right tier.** The editor viewport defaults to the `fast` tier while editing; play mode and captures
  are `full`. Pass `quality` explicitly when you compare numbers.
- **Without `frames`, numbers are whatever was rendered last.** Use `frames: 30` for stable averages.
- **Do not add vertex time.** `vertexMs` overlaps other passes; compare fragment times and `sumMs`.
- **Stills are not frames.** A capture with `samples: 16` renders 16 sub-frames; profile real-time cost with
  `perf_stats {frames}`, not with captures.
- **The CPU fallback renderer has no GPU timeline.** Profile on a Mac with Metal.

!!! agent "For agents"

    Measure before and after every heavy change (big scatters, generators, look changes):

    ```tool
    perf_stats {"frames": 30, "passes": true, "view": "scene"}          # baseline: profile.groups, profile.passes
    foliage_add {"entity": "Island", "layers": [{"preset": "meadow_grass"}]}     # the change
    perf_stats {"frames": 30, "passes": true, "view": "scene"}          # compare against the baseline
    viewport_capture {"debug_view": "overdraw", "samples": 1}           # explain a regression in main
    ```

    Report the numbers with their conditions (resolution, view, tier), and stay under 16.6 ms for 60 fps.

## Reference

- Tools: [`perf_stats`](../../reference/tools/render.md#perf_stats), [`fx_benchmark`](../../reference/tools/render.md#fx_benchmark),
  [`fx_stats`](../../reference/tools/render.md#fx_stats), [`groom_info`](../../reference/tools/render.md#groom_info),
  [`engine_info`](../../reference/tools/scene.md#engine_info), [`viewport_capture`](../../reference/tools/view.md#viewport_capture)
- C API: [`sky_frame_stats`](../../reference/capi.md)
- Manual: [Performance](../performance.md), [Debug views](debug-views.md)
- Design: [docs/RENDERING.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/RENDERING.md) (Profiling and debug views; Shader library and pipeline cache),
  [docs/DEVELOPMENT.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/DEVELOPMENT.md)
