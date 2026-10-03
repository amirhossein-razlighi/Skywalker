<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/logo/horizontal-dark.svg">
    <img src="assets/brand/logo/horizontal-light.svg" alt="Skywalker" width="520">
  </picture>
</p>

<p align="center"><b>A game engine built from the ground up for AI agents — and the people who work with them.</b></p>

---

Skywalker is a C++20 game engine with a native macOS editor, designed so that AI agents
(Claude, GPT, DeepSeek, local and self-hosted models) can **see** the scene, **act** on it
precisely, **test** what they built, and **collaborate** with you — through the very same
interface the editor uses.

> **Status: v0.0.1** — an early, working foundation. macOS / Apple Silicon (Metal) first;
> the architecture is backend- and OS-agnostic. See [ROADMAP](docs/ROADMAP.md).

## Highlights

- **One tool surface for everyone.** 57 typed, schema-validated tools (scene, entities,
  world/spatial, behaviors, simulation, viewport, lighting, effects, history, assets,
  materials, prefabs, shaders). The editor UI, the
  in-editor agents and external agents over **MCP** all use them. Errors come with
  `did you mean …?` hints. → [docs/TOOLS.md](docs/TOOLS.md)
- **Agents can see.** `viewport_capture` returns a PNG plus every visible entity's screen
  box; `annotate` draws `#id` labels on the image ("set-of-mark" prompting), so a vision
  model can name exactly what it is looking at.
- **ECPS — Entity · Component · Prompt System.** Every behavior is a natural-language
  **intent** paired with deterministic **Wander** code. People and agents edit either side;
  "Weave" turns intent into code and verifies it in simulation. → [docs/WANDER.md](docs/WANDER.md)
- **Wander** is safe by construction: no unbounded loops (every handler terminates),
  seeded randomness (replayable), execution budgets, precise diagnostics.
- **Everything is undoable and attributed.** Transactional history records *who* did each
  change (you, `agent:Nimbus`, `mcp:claude-code`); `batch` makes many edits atomic.
- **Deterministic simulation** with fixed 60 Hz ticks: agents test gameplay with
  `sim_control step` + `sim_input`, then `stop` restores the scene.
- **A studio of agents.** A roster of specialists (directors, producers, designers,
  programmers, artists, audio, writers, playtesters, critics — each with a role and a focus),
  a task board, feedback with the director's verdicts and their measured effect, playtest
  bots that actually play, and user-defined loops (playtest → triage → fix → verify) that run
  in the editor, headless (`skywalker studio run`) or driven by Claude Code / Codex. Agents
  are designed in the **Agent Designer** (model, mission, instructions, per-category
  permissions, memory, usage) and shared as `agents/*.agent.json`. Bring Anthropic, any
  OpenAI-compatible API, or local models (Ollama, LM Studio, vLLM). → [docs/STUDIO.md](docs/STUDIO.md)
- **An asset system agents can use.** Stable GUIDs, tags, descriptions and generator
  provenance in `.meta` sidecars; glTF/GLB, OBJ+MTL, PLY (vertex colors) and STL import;
  `asset_download` fetches openly licensed models from the web (with your approval, license
  tracking and an auto-maintained `CREDITS.md`); materials and prefabs as files;
  rendered previews; "who uses this?" queries; safe renames. → [docs/ASSETS.md](docs/ASSETS.md)
- **Spatial tools.** Triangle-accurate `raycast`, `place_on_surface`, seeded `scatter`
  (forests in one undo step), four-view `viewport_multi`, and `sim_trace` to verify
  gameplay numerically.
- **Realistic or stylized, your call.** Metal/roughness PBR with normal, ORM and emissive
  maps, triplanar projection, clearcoat and subsurface; image-based lighting from the sky,
  4-cascade soft shadows, SSAO, an atmospheric sky with clouds and stars, height fog, bloom
  and AgX/ACES/neutral tonemapping. Or flip a material to `toon` with outlines. Agents can
  generate seamless PBR textures (`texture_generate`) and start from material presets.
  Photographed HDRI skies light the scene. → [docs/RENDERING.md](docs/RENDERING.md)
- **Simulated effects.** Fire, smoke, steam and explosions as a real GPU fluid simulation
  (3D Eulerian solver: combustion, buoyancy, vorticity, pressure projection), ray-marched with
  blackbody flames and lit, self-shadowed smoke. An FFT ocean (JONSWAP wind waves, choppy
  crests, persistent whitecaps, shore surf, caustics, refraction, depth color, screen-space
  reflections) that gameplay can query, so boats ride the waves you see. Volumetric light
  shafts. Deterministic particles for embers, rain with splashes, snow, mist and sparks.
  One `fx_create` call from a preset; `burst()` / `water_height()` in Wander.
- **Generative assets ready.** `asset_request` / `asset_complete` route 3D, texture,
  sprite, audio, music and video requests to generators and record what prompt made each file.
- **Pro editor.** Outliner, details with reflected properties and scrubbable fields,
  move/rotate/scale gizmos with snapping, Metal viewport with PBR, sun shadows, MSAA,
  selection outlines, stats, console, activity feed.

## Quick start

Requirements: macOS 15+, Xcode 16+ command-line tools, CMake ≥ 3.29, Ninja.

```bash
cmake --preset debug
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

Run the editor with the example project:

```bash
open build/debug/bin/Skywalker.app --env SKY_PROJECT="$PWD/examples/hello_sky"
```

### Connect Claude Code (or any MCP client) to the running editor

The editor listens on `~/.skywalker/editor.sock` (user-only permissions). Bridge it:

```bash
claude mcp add skywalker -- "$PWD/build/debug/bin/skywalker" mcp --attach
```

Or run a headless engine with no editor at all:

```bash
claude mcp add skywalker-headless -- "$PWD/build/debug/bin/skywalker" mcp --project examples/hello_sky --scene scenes/main.sky.json
```

More: [docs/AGENTS.md](docs/AGENTS.md).

### Headless CLI

```bash
build/debug/bin/skywalker render examples/hello_sky/scenes/main.sky.json -o shot.png --annotate
```

`skywalker run SCENE --ticks 600` simulates deterministically and prints logs;
`skywalker check file.wander` compiles Wander; `skywalker call TOOL '{json}'` runs one tool.

## Documentation

| Doc | What's inside |
|---|---|
| [ARCHITECTURE](docs/ARCHITECTURE.md) | Layers, threading, data flow, design decisions |
| [WANDER](docs/WANDER.md) | The behavior language and the ECPS model |
| [AGENTS](docs/AGENTS.md) | MCP, the crew, Agent Designer, providers, generative assets |
| [ASSETS](docs/ASSETS.md) | Asset database, glTF, materials, prefabs, spatial tools |
| [RENDERING](docs/RENDERING.md) | PBR + toon surfaces, IBL, cascaded shadows, SSAO, sky, post, recipes |
| [BRAND](docs/BRAND.md) | Logo, app icon, colors, typography |
| [TOOLS](docs/TOOLS.md) | Generated reference of every tool |
| [DEVELOPMENT](docs/DEVELOPMENT.md) | Building, testing, sanitizers, profiling, conventions |
| [ROADMAP](docs/ROADMAP.md) | What's next |
| [LICENSING](docs/LICENSING.md) | Licensing model (draft — please read) |

## License

See [LICENSE](LICENSE) and [docs/LICENSING.md](docs/LICENSING.md). The intended model is
free for individuals and small companies with a commercial license above a size/revenue
threshold; the repository currently still carries the original CC0 file — **see the
licensing doc before publishing.**
