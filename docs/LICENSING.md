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
