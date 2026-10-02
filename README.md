<p align="center">
  <img src="assets/brand/logo.svg" alt="Skywalker" width="420">
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

- **One tool surface for everyone.** 35 typed, schema-validated tools (scene, entities,
  behaviors, simulation, viewport, lighting, history, assets, shaders). The editor UI, the
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
- **Your crew of Cloudlings.** Specialized agents (director, level designer, gameplay
  programmer, lighting artist, writer, …) with their own model, autonomy level
  (observe / ask / autonomous), delegation, and pipelines. Bring Anthropic, any
  OpenAI-compatible API, or local models (Ollama, LM Studio, vLLM).
- **Generative assets ready.** `asset_request` / `asset_complete` route 3D, texture,
  sprite, audio, music and video requests to generators; OBJ meshes import directly.
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
| [AGENTS](docs/AGENTS.md) | MCP, the crew, providers, generative assets |
| [TOOLS](docs/TOOLS.md) | Generated reference of every tool |
| [DEVELOPMENT](docs/DEVELOPMENT.md) | Building, testing, sanitizers, profiling, conventions |
| [ROADMAP](docs/ROADMAP.md) | What's next |
| [LICENSING](docs/LICENSING.md) | Licensing model (draft — please read) |

## License

See [LICENSE](LICENSE) and [docs/LICENSING.md](docs/LICENSING.md). The intended model is
free for individuals and small companies with a commercial license above a size/revenue
threshold; the repository currently still carries the original CC0 file — **see the
licensing doc before publishing.**
