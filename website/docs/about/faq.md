# FAQ

## General

??? question "What does agent-first mean in practice?"

    Every capability is a tool with a JSON schema and a model-oriented description, shared by the editor, the
    in-editor crew, the CLI and any MCP client. Agents get structured feedback a person never needs (entity ids and
    screen boxes in every capture, G-buffer views, numeric traces, per-pass GPU timings), every edit is undoable and
    attributed, and the deterministic simulation lets agents test gameplay instead of guessing. See the
    [Agent guide](../agents/index.md).

??? question "Do I need an AI agent to use Skywalker?"

    No. The editor is a complete tool on its own, and the CLI renders, simulates, packages and runs any tool without
    a model. Agents are first-class, not mandatory.

??? question "Which platforms are supported?"

    macOS on Apple silicon, with the Metal renderer, the SwiftUI editor and the standalone player. The engine core is
    portable C++20 and builds headless (with a CPU renderer) for CI. A Vulkan backend for Windows and Linux is on the
    [roadmap](roadmap.md).

??? question "Which models work with the in-editor crew and the studio runner?"

    Anthropic models through the Messages API, and any OpenAI-compatible API: OpenAI, DeepSeek, OpenRouter, Groq, or
    local servers such as Ollama, LM Studio, vLLM and llama.cpp. In the editor, keys live in the macOS Keychain; the
    headless runner reads them from the environment.

??? question "Is it free?"

    Free for individuals, for companies under US$1M in annual revenue and under US$1M in funding, and for non-profits,
    education and non-commercial use, including shipping and selling games. Larger organizations need a commercial
    license. Each version becomes Apache-2.0 four years after release. See [License](license.md).

## Building games

??? question "How do I script gameplay?"

    With [Wander](../manual/wander/index.md), a deterministic behavior language with states, coroutines, collections,
    modules and in-language tests, a node-graph view of the same code, and native compilation for hot paths. For
    anything else, write a native C++ module against the module SDK.

??? question "Can I import my own models and characters?"

    Yes: glTF/GLB (with skins and animations), OBJ, PLY and STL import directly; FBX, USD, Alembic, DAE and `.blend`
    convert through the Blender bridge. See [Assets and prefabs](../manual/assets.md) and
    [Animation](../manual/animation.md).

??? question "Can I make 2D games?"

    Yes: sprites with flipbook animation, atlases, tilemaps with auto-tiling, 2D lights with shadows, parallax and a
    pixel-perfect camera, rendered by the same engine as 3D. See [2D and UI](../manual/2d-ui.md).

??? question "How do I ship a game?"

    `skywalker build --project DIR --out ~/Builds --release` packages a standalone macOS app around the player with
    the scenes, referenced assets, scripts and license notices. See [Shipping](../manual/shipping.md).

## Agents

??? question "Will an agent overwrite my work?"

    Every edit is undoable and attributed in the history and the Activity feed, and `batch` edits are atomic. Crew
    members have autonomy levels (*Observe*, *Ask*, *Autonomous*) and per-category permissions; downloads and
    design-app tools ask for approval by default. See [Permissions and approvals](../agents/permissions.md).

??? question "Can several agents work on one project?"

    Yes. The studio is shared by the editor's crew, the headless runner and every MCP client: a roster with roles, a
    board, feedback, decisions and loops. External subagents identify themselves with `as: "<agent id>"`. See
    [Studio and crews](../manual/studio.md).

??? question "Does the agent need the editor to be running?"

    No. `skywalker mcp --auto` attaches to the editor when it runs and otherwise starts a headless engine on the
    project. Headless edits stay in memory until the agent calls `scene_save`.

## Rendering

??? question "Does it need a GPU?"

    The editor and the player use Metal. The CLI falls back to a CPU renderer when Metal is not available, so headless
    captures, tests and CI work anywhere; agent-relevant facts (visible entities, screen boxes, picking) are computed
    on the CPU and identical on every backend.

??? question "Why does my capture look better than the live viewport?"

    The live editor viewport renders in a quality tier (**fast** by default) to stay responsive in heavy worlds;
    captures, play mode and movies render at full quality. Change the tier with `viewport_quality` or the viewport's
    picker.
