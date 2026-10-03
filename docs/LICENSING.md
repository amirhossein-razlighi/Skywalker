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

Libraries fetched at build time (CMake FetchContent, pinned tags) and linked into the engine:

| Library | Version | License | Used for |
|---|---|---|---|
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | v5.6.0 | MIT | Rigid bodies, colliders, character controller, joints, physics queries |
| [Recast & Detour](https://github.com/recastnavigation/recastnavigation) | v1.6.0 | zlib | Navigation mesh generation, path finding, crowd steering |
