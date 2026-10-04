# Licensing

Skywalker is **source available** under the **Business Source License 1.1** (`LICENSE`), with
an Additional Use Grant that makes it free for most people, and a commercial license for larger
organizations. Each version converts to the **Apache License 2.0** four years after its release.

> **Not legal advice.** This page explains the license in plain words. If anything here
> differs from `LICENSE`, `LICENSE` wins.

## Who can use it for free

You can use Skywalker for free, including shipping and selling games made with it, if you are:
- an **individual**, or a **company with under US$1M in annual revenue and under US$1M in
  funding raised** over the last twelve months, counting affiliates; or
- a **non-profit**, a **school or university**, or using it for **non-commercial research,
  teaching or personal projects**.

Anyone may also read, modify and build the source, and use it for evaluation and development,
regardless of size. Production use above the thresholds needs a commercial license.

## Who needs a commercial license

- **Organizations above either threshold** (revenue or funding, including affiliates). You keep
  60 days to arrange a license after you cross a threshold.
- **Anyone offering Skywalker itself as a competing engine, tool or hosted service**, whatever
  their size.

Contact the licensor for terms.

## Terms of Use and privacy

Using the software and its AI features is also covered by the [Terms of Use](legal/TERMS.md) and the
[Privacy Notice](legal/PRIVACY.md); the editor asks you to accept them on first launch, and
`skywalker legal` prints them. Skywalker sends no telemetry and nothing to the licensor. Both are
templates awaiting legal review.

## Your games are yours

Games, applications, assets, scripts, scenes and other content you create with Skywalker belong
to you. Under the free grant there are no royalties or fees, and no obligations on your games
beyond shipping the license notices.

`skywalker build` puts the required notices into every app it packages, in
`Contents/Resources/Licenses/`: Skywalker's `LICENSE` and the third-party licenses below.

## Every version becomes open source

Four years after a version is first published, that version is also licensed under the Apache
License 2.0. Newer versions stay under the Business Source License until their own change date.

## Contributing

Contributions are welcome under the terms in `CONTRIBUTING.md`. You keep the copyright to your
contribution, and grant the licensor the right to distribute it under this license, the
commercial license and the Change License.

## Trademark

The license grants no rights to the Skywalker name or logo. Check trademark availability for
"Skywalker" before a public commercial launch: it is a well-known term in entertainment.

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
| [Box2D](https://github.com/erincatto/box2d) | v3.1.1 | MIT | 2D rigid bodies, shapes, chains, joints, sensors, queries and the character2d mover |

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
