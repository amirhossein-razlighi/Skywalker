# License

Skywalker is **source available** under the **Business Source License 1.1**, with an Additional Use Grant that makes
it free for most people, and a commercial license for larger organizations. Each version converts to the
**Apache License 2.0** four years after it is first published.

Skywalker is developed by **AmirHossein (Amir) Razlighi** (the Licensor).

!!! warning "Not legal advice"

    This page explains the license in plain words. If anything here differs from the
    [LICENSE](https://github.com/amirhossein-razlighi/Skywalker/blob/main/LICENSE) file, the LICENSE file wins.

## Who can use it for free

You can use Skywalker for free, including shipping and selling games made with it, if you are:

- an **individual**, or a **company with under US$1M in annual revenue and under US$1M in funding raised** over the
  last twelve months, counting affiliates; or
- a **non-profit**, a **school or university**, or using it for **non-commercial research, teaching or personal
  projects**.

Anyone may read, modify and build the source, and use it for evaluation and development, regardless of size.

## Who needs a commercial license

- **Organizations above either threshold** (revenue or funding, including affiliates). You keep 60 days to arrange a
  license after you cross a threshold.
- **Anyone offering Skywalker itself as a competing engine, development tool or hosted service**, whatever their size.

Contact the Licensor for commercial terms.

## Your games are yours

Games, applications, assets, scripts, scenes and other content you create with Skywalker belong to you. Under the free
grant there are no royalties or fees, and no obligations on your games beyond shipping the license notices.
`skywalker build` puts the required notices into every app it packages, in `Contents/Resources/Licenses/`: Skywalker's
LICENSE and the third-party licenses below.

## Every version becomes open source

Four years after a version is first published, that version is also licensed under the Apache License 2.0. Newer
versions stay under the Business Source License until their own change date.

## Third-party components

Dependencies are fetched at configure time from their official repositories at pinned versions and must have
permissive licenses.

| Library or asset | Version | License | Used for |
|---|---|---|---|
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | v5.6.0 | MIT | Rigid bodies, colliders, character controller, joints, queries |
| [Recast & Detour](https://github.com/recastnavigation/recastnavigation) | v1.6.0 | zlib | Navigation meshes, path finding, crowd steering |
| [Box2D](https://github.com/erincatto/box2d) | v3.1.1 | MIT | 2D rigid bodies, shapes, joints, sensors, queries, platformer characters |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.22 | Public domain or MIT-0 | Mixing, spatialization, decoding, output devices |
| [stb](https://github.com/nothings/stb) (`stb_truetype`, `stb_image`, `stb_rect_pack`) | pinned commit | Public domain or MIT | SDF glyphs, image decoding, atlas packing |
| [Inter](https://github.com/rsms/inter), [EB Garamond](https://github.com/octaviopardo/EBGaramond12), [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | google/fonts | SIL OFL 1.1 | Built-in UI, serif and monospace fonts |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | v0.22 | MIT | Automatic mesh LOD chains |
| [doctest](https://github.com/doctest/doctest) | v2.4.11 | MIT | Test suite (not shipped) |

The engine also links the system's zlib and libcurl.

Design apps (Blender, Maya, Houdini, 3ds Max) are not bundled, linked or redistributed: Skywalker starts the copies
you already have as separate processes. The small Python scripts that run *inside* Blender and import `bpy` carry
`SPDX-License-Identifier: GPL-3.0-or-later`; they talk to the engine only through processes and sockets.

The full list with notes is in
[docs/LICENSING.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/LICENSING.md).

## Terms of Use and privacy

Using Skywalker and its AI features is also covered by the
[Terms of Use](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/legal/TERMS.md) and the
[Privacy Notice](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/legal/PRIVACY.md). The editor asks you
to accept them on first launch; for headless and CI use, `skywalker legal --accept` records the same acceptance.
Both documents are drafts awaiting legal review.

- **Skywalker sends nothing to its developer:** no telemetry, analytics, crash reports, update checks or accounts.
  Data leaves your computer only when you send it: to the AI provider you configure, or to a website when you approve
  an asset download.
- **Games you ship show no Skywalker terms or consent screens,** and the player collects and sends nothing. If your
  game adds networking, accounts, analytics or AI features, its privacy is yours to handle; the Privacy Notice has a
  checklist for developers who ship games.

```bash
skywalker legal terms          # print the Terms of Use embedded in this binary
skywalker legal privacy        # the Privacy Notice
skywalker legal --status       # versions and whether they were accepted
```

Agents read the same documents with [`legal_info`](../reference/tools/files.md#legal_info).

## Trademark

The license grants no rights to the Skywalker name or logo.

## This documentation

The documentation site's text, images and clips are part of the repository and covered by the same license. The fonts
used by the site are loaded from Google Fonts under the SIL Open Font License.
