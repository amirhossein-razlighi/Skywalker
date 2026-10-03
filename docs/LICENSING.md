# Licensing (draft — decision needed)

> **Not legal advice.** This page describes the intended model and the options for getting
> there. Have a lawyer review the final license before release.

## Current state

The repository's `LICENSE` file is **CC0 1.0**, a public-domain dedication. It conflicts
with the intended model below: anything published under CC0 can be used by anyone, for
anything, forever, including large companies. **Replace the license before the first
public release.** Code already published under CC0 stays available under CC0.

## Intended model

- Free to use, modify and ship games with, for individuals, hobbyists, education and
  smaller companies.
- A paid commercial license for organizations above a size or revenue threshold.
- Source available, with community contributions welcome.

## Options

| Option | What it is | Fit |
|---|---|---|
| **Custom "community license" with a threshold** (the Unreal / Unity approach) | Free below a set revenue or funding (e.g. under US$1M in annual revenue); a commercial agreement above it. | Matches the intent exactly. Must be drafted carefully: define "revenue" and "affiliates", and decide whether royalties or seats apply. |
| **Business Source License 1.1 (BSL)** | Source available. An "Additional Use Grant" can allow production use below a threshold. Each version converts to an open license after N years. | Well understood, with a time-delayed open-source guarantee. |
| **Functional Source License (FSL)** | Source available; blocks competing commercial use; converts to Apache/MIT after 2 years. | Simple, but prohibits *competing* use rather than setting a size threshold. |
| **Dual license: AGPL-3.0 + commercial** | Open source; companies that can't comply with AGPL buy a commercial license. | OSI-approved, but AGPL obligations can deter game developers. |
| **PolyForm Small Business** | Free for organizations with fewer than 100 people and under US$1M revenue; others need a separate license. | The closest off-the-shelf match to "free below a size/revenue threshold". |

**Recommendation:** start from **PolyForm Small Business 1.0.0** for the engine and editor,
plus a separate commercial license (with optional royalties) for larger organizations. Keep
**games and content created with Skywalker** fully owned by their creators, with no license
obligations on them. That separation matters to studios.

Also decide:
- A contributor agreement (CLA or DCO), so contributions can be dual-licensed.
- Trademark policy for the name "Skywalker". It is a well-known term in entertainment, so
  check trademark availability before branding a product with it.

## Third-party

Dependencies are fetched at configure time from their official repositories at pinned tags, and
must have permissive licenses (MIT, BSD, zlib, Apache-2.0, public domain).

| Library | Version | License | Used for |
|---|---|---|---|
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.22 | Public domain (or MIT-0) | Audio engine: mixing, spatialization, decoding of wav/mp3/flac, output devices (`engine/src/audio`). |
### Design apps (DCC bridge)

Blender, Maya, Houdini and 3ds Max are **not** bundled, linked or redistributed: Skywalker starts
the copies the user already has installed as separate processes (`docs/DCC.md`), so their
licenses (Blender: GPL-3.0-or-later; the others: proprietary) do not apply to Skywalker or to
games made with it. No new third-party dependencies were added.

One licensing consequence to decide before the public release: Python that runs *inside*
Blender and imports `bpy` is, by the Blender Foundation's long-standing position, subject to the
GPL. The files that do so (`integrations/blender/skywalker_bridge/*.py` and
`integrations/dcc/skywalker_dcc/{blender,procedural,tasks}.py`) therefore carry
`SPDX-License-Identifier: GPL-3.0-or-later`. They are small, self-contained scripts that talk to
the engine only through processes and sockets, so the engine and editor keep the project license;
the rest of `integrations/dcc` (which never imports `bpy`) is under the project license. The
engine embeds these files as data and writes them out to run them; ship their license text with
any binary distribution.
Libraries fetched at build time (CMake FetchContent, pinned tags) and linked into the engine:

| Library | Version | License | Used for |
|---|---|---|---|
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | v5.6.0 | MIT | Rigid bodies, colliders, character controller, joints, physics queries |
| [Recast & Detour](https://github.com/recastnavigation/recastnavigation) | v1.6.0 | zlib | Navigation mesh generation, path finding, crowd steering |

### 2D, text and UI (workstream T)

| Library / asset | Version | License | Used for |
|---|---|---|---|
| [stb](https://github.com/nothings/stb) (`stb_truetype`, `stb_image`, `stb_rect_pack`) | commit `2c980bb` (truetype 1.26, image 2.30, rect_pack 1.01) | Public domain (or MIT) | SDF glyph rasterization, image decoding (PNG/JPEG/BMP/TGA), atlas packing |
| [Inter](https://github.com/rsms/inter) (`assets/fonts/Inter.ttf`) | google/fonts `9710da1` | SIL OFL 1.1 (`assets/fonts/Inter-OFL.txt`) | Default UI font (also compiled into the engine with `#embed` as a fallback) |
| [EB Garamond](https://github.com/octaviopardo/EBGaramond12) (`assets/fonts/EBGaramond.ttf`) | google/fonts `9710da1` | SIL OFL 1.1 (`assets/fonts/EBGaramond-OFL.txt`) | Serif font (books, letters, documents) |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) (`assets/fonts/JetBrainsMono.ttf`) | google/fonts `9710da1` | SIL OFL 1.1 (`assets/fonts/JetBrainsMono-OFL.txt`) | Monospace font (terminals, data, pixel-style UIs) |

The fonts come unmodified from the official [google/fonts](https://github.com/google/fonts)
repository (`ofl/inter`, `ofl/ebgaramond`, `ofl/jetbrainsmono`; variable fonts renamed to short
file names). The OFL allows bundling them with games and software, including commercial ones; the
license text must travel with the font files, and the fonts may not be sold on their own.
