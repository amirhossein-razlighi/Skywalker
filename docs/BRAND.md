# Skywalker brand guide

Overview sheet: `assets/brand/brand-sheet.png`. Regenerate everything with `python3 assets/brand/src/build.py`
(needs `rsvg-convert`, `iconutil`, Pillow; `magick` for the .ico).

## Concept
A Cloudling in sunglasses: friendly (cloud, blush, smile) but cool and professional (gold aviators with a
sunset reflection, deep night-sky tile). The sunset in the lenses and the horizon glow at the tile bottom are
the one warm accent against a navy/indigo field, mirroring the editor's blue/violet UI with an orange pop.

## Logo variants
| Variant | File(s) | Use |
|---|---|---|
| App icon (compact, full colour) | `icon/app-icon.svg`, `app-icon-{1024..16}.png`, `icon/AppIcon.icns` | Dock, Finder, App Store, README avatar. Already has Big Sur grid (824 art on 1024) and shadow. |
| App icon, flat (no margin/shadow) | `icon/app-icon-flat.svg/.png` | Social avatars, lockup embeds, places that apply their own mask. |
| Glyph (monochrome) | `icon/glyph-black.svg`, `glyph-white.svg`, `MenuBarTemplate(@2x).png` | Menu bar (template image), toolbar, docs, single-colour print, emboss. Lenses and smile are knocked out, so it works on any background. |
| Favicon (simplified) | `icon/favicon.svg`, `favicon-{16,32,48,180,512}.png`, `favicon.ico` | Browser tabs, 16-64 px UI. No stars/blush/smile so it stays legible. |
| Horizontal lockup | `logo/horizontal-{dark,light}[-notag].svg/.png` | Website header, slides, GitHub social card. `-notag` when under ~400 px wide. |
| Stacked lockup | `logo/stacked-{dark,light}.svg/.png` | Splash, posters, square placements. |
| Wordmark only | `logo/wordmark-{dark,light}.svg/.png` | Footers, where the mark already appears nearby. |

`assets/brand/app-icon.png` and `logo.svg` are kept in sync as legacy paths.

## Clear space and minimum size
- Clear space around any lockup or tile: the height of the cloud's left lens (about 1/8 of the tile width).
- Minimum sizes: app icon 16 px (use favicon art below 64 px), glyph 14 px high, horizontal lockup 120 px wide
  without tagline (tagline only above 400 px), stacked lockup 96 px wide.
- Dark lockup on `#0c0e1e` or any background darker than `#2a2d3a`; light lockup on white to `#f6f7fc`.

## Colour
| Role | Hex | Notes |
|---|---|---|
| Night (icon top) | `#222a68` | Tile gradient start |
| Deep navy | `#131838` | Tile mid |
| Void | `#070919` | Tile end, dark surfaces |
| Cloud white | `#ffffff` -> `#bfcaf7` | Cloud body gradient |
| Sky blue | `#4f8dff` | = editor `Theme.accent` |
| AI violet | `#9a86ff` | = editor `Theme.ai` |
| Sunset | `#ffb873` | Sparkle, lens reflection, horizon glow (accent only) |
| Rose | `#ed6b7a` | Blush, lens reflection |
| Gold frame | `#fff1cf` / `#e2b062` / `#a8702f` | Sunglass frame gradient |
| Ink | `#1b1d2b` | Face features, wordmark on light is `#14172e` |
Editor UI surfaces stay as defined in `editor/Sources/Theme/Theme.swift` (window `#151619`, panel `#1c1d21`,
text `#e3e4e8`). Use sunset orange sparingly; the editor already uses `#ff7a1a` for selection.

## Typography
**Wordmark** is custom-drawn monoline geometry (round caps, 19-unit stroke on a 150-unit ascender): no font
file is embedded, so there are no font licensing constraints. Only the tagline is live text.

**Proposed brand fonts (SIL OFL, free; NOT downloaded, pending your approval):**
| Role | Font | Fallback |
|---|---|---|
| Display / headings / tagline | Sora (or Outfit, Manrope) | Avenir Next, system-ui |
| UI / body (web, docs) | Inter | SF Pro Text, system-ui |
| Code / mono | JetBrains Mono | SF Mono, Menlo |

**Drafts in this repo** are rendered with the system fallback: the tagline in the SVGs is
`font-family: Inter, SF Pro Text, Avenir Next, sans-serif`, and the PNGs rasterised here picked up whichever
was installed (Inter was likely absent, so a system sans). Re-run `build.py` after installing the approved font
to bake the final tagline. SF fonts are licensed for Apple-platform UI only, so do not ship them in brand art.

**Editor UI** keeps the system font (SF Pro; fine inside an Apple app). Scale from `Theme.swift`:
| Token | Spec | Use |
|---|---|---|
| `caps` | 10 pt semibold, +0.6 tracking, UPPERCASE | Section headers |
| `sectionTitle` | 11 pt semibold | Panel/inspector titles |
| `label` | 11 pt regular | Controls, tabs (selected: semibold) |
| `body` | 12 pt regular | Chat, descriptions |
| `mono` | 11.5 pt monospaced | Code, JSON, tool calls |
| `monoSmall` | 10.5 pt monospaced | Logs, inline ids |
Marketing/web scale (proposed): Display 56/60 Sora 700, H1 40/46 Sora 600, H2 28/34 Sora 600, Body 17/26 Inter 400, Small 14/20 Inter 500, Code 14/22 JetBrains Mono 400.

## Do / don't
- Do keep the sunglasses on the logo cloud; crew Cloudlings (`CloudAvatar`) are the bare, colourful, quiet variants.
- Do use the glyph for anything under 32 px on a single-colour surface.
- Don't recolour the cloud, rotate or stretch the mark, re-set the wordmark in a font, add outlines or drop shadows to lockups.
- Don't put the light lockup on busy or dark backgrounds; don't place the tile on a background close to `#131838` without the flat variant's edge highlight.
- Don't use the tagline below 400 px lockup width.

## File index
```
assets/brand/
  brand-sheet.png          contact sheet of all variants
  app-icon.png, logo.svg   legacy paths (synced)
  icon/  app-icon*.svg/png, AppIcon.icns + .iconset, favicon*, glyph-*, MenuBarTemplate*
  logo/  horizontal-*, stacked-*, wordmark-*
  src/   brand.py (geometry), build.py (all outputs + contact sheet)
editor/Resources/AppIcon.icns   installed app icon (CFBundleIconFile = AppIcon)
```
