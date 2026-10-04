# Skywalker documentation site

The manual, agent guide, reference and examples gallery for Skywalker, built with
[MkDocs](https://www.mkdocs.org) and [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/) and deployed
to GitHub Pages by `.github/workflows/docs.yml`.

```text
website/
  mkdocs.yml               site config and navigation
  requirements.txt         pinned toolchain (MkDocs 1.6, Material 9.7)
  docs/                    the pages
    index.md               home
    getting-started/       install, first project, editor tour, first game, connect an agent
    manual/                one page per subsystem (hand-written)
    agents/                the agent guide (hand-written)
    reference/             GENERATED: tools, components, Wander builtins, CLI, C API
    examples/index.md      GENERATED: the examples gallery
    assets/                css, brand, images, video (all media lives here, never in docs/ at the repo root)
  overrides/main.html      theme override (favicon, social meta)
  data/                    engine dumps the reference is generated from, plus curated examples.json, tool_examples.json
  scripts/                 dump_data.py, gen_reference.py, check_snippets.py, make_media.py, tutorial_coin_run.py
```

`docs/*.md` at the repository root stays the engine's design documentation (embedded in the binary as MCP
resources); the site links to it for deep dives and never copies images or pages into it.

## Build locally

```bash
python3 -m venv /tmp/skydocs-venv
/tmp/skydocs-venv/bin/pip install -r website/requirements.txt
/tmp/skydocs-venv/bin/mkdocs build --strict -f website/mkdocs.yml -d /tmp/skydocs-site
/tmp/skydocs-venv/bin/mkdocs serve -f website/mkdocs.yml -a 127.0.0.1:8765      # live preview
```

The build must stay warning-free in `--strict` mode (broken links and anchors are warnings). The toolchain is pinned to
MkDocs 1.6: do not upgrade to MkDocs 2.0, which drops the plugin and theme system Material depends on.

## Add or change a page

1. Create the Markdown file under `website/docs/` and add it to `nav` in `website/mkdocs.yml` (pages that are not in
   the nav fail the strict build).
2. Follow the page shape of the manual: a clear first paragraph, a figure, **Concepts**, **How to …** with code tabs,
   a **Recipe**, **Pitfalls**, a `!!! agent "For agents"` admonition with the tool calls an agent would use, and
   **Reference** links to the generated pages and the deep design document in `docs/`.
3. Samples must be real:
   - tool calls in ```` ```tool ```` fences, one `tool_name {"strict": "json"}` per line;
   - Wander in ```` ```wander ```` fences, compiling on its own (`skywalker check`); fragments go in ```` ```text ````;
   - CLI in ```` ```bash ```` fences with real `skywalker` commands.
4. Run the checks:

   ```bash
   python3 website/scripts/check_snippets.py --cli build/release/bin/skywalker
   /tmp/skydocs-venv/bin/mkdocs build --strict -f website/mkdocs.yml -d /tmp/skydocs-site
   ```

   `check_snippets.py` validates every tool call against the live schemas (reusing `integrations/check_skills.py`),
   compiles every Wander block, checks CLI commands and inline tool names, and rejects local paths, e-mail addresses,
   API keys and names of other engines anywhere in `website/`. Without `--cli` it skips only the Wander compile (that
   is what the Pages workflow runs).

Writing rules: describe Skywalker on its own terms (no comparisons with other engines), document only what exists,
quote measured numbers with their conditions, use placeholder paths (`~/Builds`, `/Users/me/...`), and never launch
the editor to make screenshots for the site from an automated session.

## Regenerate the reference

The reference pages and the examples gallery are generated, never edited by hand. After changing a tool, argument,
enum value, component field, Wander builtin, CLI flag or an example project:

```bash
cmake --build --preset release --target skywalker
python3 website/scripts/dump_data.py --cli build/release/bin/skywalker     # website/data/*.json from the engine
python3 website/scripts/gen_reference.py                                    # website/docs/reference/** + examples/index.md
python3 website/scripts/check_snippets.py --cli build/release/bin/skywalker
```

Commit `website/data/` and the generated pages together. The engine's test suite (`tests/test_website.cpp`) dumps the
data from the freshly built CLI and fails when the committed files are stale (`gen_reference.py --check --fresh DIR`),
so the reference cannot drift. The Pages workflow therefore never needs to build the C++ engine.

Curated inputs: `website/data/examples.json` (gallery order, description, media and extra feature tags per example;
a new folder under `examples/` must be added here) and `website/data/tool_examples.json` (example calls for tools whose
description and docs contain none). Categories and component groups are listed at the top of `gen_reference.py`; a new
tool category or component fails generation until it is added there and to the nav.

## Add media

All media lives in `website/docs/assets/` (keep the total under about 60 MB):

| Kind | Format | Where |
|---|---|---|
| Stills | WebP, 1280 px wide, quality ~82, no metadata | `assets/images/<section>/` |
| Clips | H.264 MP4, ≤ 720p, ≤ 8 s, ≤ 3 MB, no audio, with a WebP poster of the same name | `assets/video/<group>/` |
| Editor screenshots | WebP, captured by a person (the editor is never launched by automation) | `assets/editor/` |

`scripts/make_media.py` rebuilds everything that comes from the engine:

```bash
python3 website/scripts/make_media.py convert                                      # example shots, previews, hair/VFX images
python3 website/scripts/make_media.py render --cli build/release/bin/skywalker     # look-dev, world, VFX, agent views, example stills
python3 website/scripts/tutorial_coin_run.py --cli build/release/bin/skywalker     # runs the first-game tutorial and captures it
```

Renders go through the headless MCP server at 1280x720 with at most 8 samples, one at a time (the CLI holds the
machine-wide GPU lock). To add a render, add a function to `make_media.py` that builds a small scene with tools and
calls `capture(...)`.

Editor screenshot slots are marked on the pages with a dashed placeholder naming the expected file
(`assets/editor/<name>.webp`). To fill one, save the screenshot as WebP and replace the placeholder `<div>` with a
figure.

## Deployment

`.github/workflows/docs.yml` runs on pushes to `main` that touch `website/**`, `docs/**` or the workflow itself, and on
demand. The **build** job installs the pinned toolchain, runs `check_snippets.py` and `gen_reference.py --check`,
builds with `--strict` and uploads the site as an artifact (downloadable from the run while the repository is
private). The **deploy** job publishes it to GitHub Pages; it is skipped, not failed, while the repository is private.

### Going live

When the repository becomes public:

1. **Settings → Pages → Build and deployment → Source: "GitHub Actions".**
2. Re-run the latest **Docs** workflow (Actions → Docs → Run workflow), or push any change under `website/`.

The site is then served at <https://amirhossein-razlighi.github.io/Skywalker/> (`site_url` in `mkdocs.yml`).
