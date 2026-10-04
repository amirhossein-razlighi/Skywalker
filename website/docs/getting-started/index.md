# Getting started

Skywalker is a C++20 game engine with a native macOS editor, built so that AI agents can see a scene, act on it
precisely, test what they built and work alongside you through the same interface the editor uses. This section takes
you from a fresh checkout to a playable game and an agent that can build with you.

<figure markdown>
![Tidebreak Isle at golden hour](../assets/images/shots/tidebreak_isle/establishing.webp){ loading=lazy }
<figcaption>Tidebreak Isle, one of the sample projects, rendered by the engine from its own sequence camera.</figcaption>
</figure>

## The path

| Step | You will | Time |
|---|---|---|
| [Install and build](install.md) | Build the engine, the CLI, the player and the editor from source; run the tests | 10–20 min |
| [Your first project](first-project.md) | Understand the project folder, open an example, render it from the command line | 5 min |
| [Editor tour](editor-tour.md) | Learn the editor's panels, viewport controls, play mode and the agent dock | 10 min |
| [Your first game in 15 minutes](first-game.md) | Build *Coin Run*: a character, coins, a score and a camera, then test and package it | 15 min |
| [Connect an AI agent](connect-agent.md) | Wire Claude Code, Codex, Gemini CLI or Cursor to the engine with one command | 5 min |

## Three ways to work

Everything in Skywalker is a **tool**: a named operation with a JSON schema, an LLM-oriented description and a
handler. There are three ways to call the same tools, and they never differ in power.

=== "Editor"

    The SwiftUI editor on macOS: outliner, viewport, details panel, asset browser, studio and the in-editor agent
    crew. Every button you press calls a tool, so every edit is undoable and attributed.

    ```bash
    open build/release/bin/Skywalker.app --args --project "$PWD/examples/hello_sky"
    ```

=== "Command line"

    The `skywalker` binary is the headless engine: it renders, simulates, compiles Wander, runs any tool and packages
    games, with a CPU renderer fallback for machines without a GPU.

    ```bash
    skywalker render examples/hello_sky/scenes/main.sky.json -o shot.png --annotate
    skywalker call scene_overview '{}' --project examples/hello_sky --scene scenes/main.sky.json
    ```

=== "AI agents"

    Any MCP client (Claude Code, Codex, Gemini CLI, Cursor, or your own) drives the engine through `skywalker mcp`.
    Attached to a running editor, you watch the agent's edits appear live and can undo them.

    ```bash
    skywalker setup claude
    ```

!!! agent "For agents"

    An agent that joins a project starts the same way every time: learn the conventions, look at what exists, then
    act and verify.

    ```tool
    engine_info {}
    scene_overview {}
    viewport_capture {"annotate": true}
    ```

## Requirements at a glance

| | |
|---|---|
| Platform | macOS 15 or later on Apple silicon (Metal). The engine core is portable C++20 and builds headless elsewhere with a CPU renderer. |
| Toolchain | Xcode 16 or later (Swift 6), CMake 3.29 or later, Ninja |
| Disk | About 2 GB for a release build with its fetched dependencies |
| Optional | Blender 3.2+ for the DCC bridge; an API key for the in-editor crew or the headless studio runner |

Ready? [Install and build](install.md).
