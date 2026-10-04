# Localization

A game's texts live in string tables under `locale/`, one key per string and one translation per
locale. UI and world texts refer to keys as `"@key"`, scripts call `tr(key, args)`, and dialogue
lines carry a `#line:<id>` tag. The engine picks the locale, falls back sensibly when a string is
missing, formats plurals and numbers per language, and reloads the tables when they change.

- [String tables](#string-tables)
- [Messages: placeholders, plurals, numbers](#messages-placeholders-plurals-numbers)
- [Using keys](#using-keys)
- [The locale](#the-locale)
- [Wander](#wander)
- [Tools](#tools)
- [Workflow for agents](#workflow-for-agents)
- [Fonts and scripts](#fonts-and-scripts)
- [Limits](#limits)

## String tables

Every `*.csv` and `*.po` file under `locale/` (subfolders too) is loaded and merged. Use whichever
fits your translators; both can coexist.

**CSV** (spreadsheet-friendly): a header row `key,<locale>,<locale>...`, then one row per key.
Optional columns: `comment` (context for translators) and `max` (a length budget in characters).
Rows starting with `#` are comments; cells follow RFC 4180 (quote cells with commas, quotes or line
breaks; `""` is a quote). An empty cell means "not translated".

```csv
key,en,fr,pt-BR,comment,max
menu.play,Play,Jouer,Jogar,main menu button,10
hud.coins,"{n, plural, one {# coin} other {# coins}}","{n, plural, one {# pièce} other {# pièces}}",,
hud.welcome,"Welcome, {name}!","Bienvenue, {name} !",,
```

**gettext .po** (translation tools): one catalog per locale, named `fr.po`, `pt_BR.po` or placed in
`locale/fr/`; a `Language:` header wins over the name. Keys are in `msgctxt` (then `msgid` is the
source text, also used as the source-language string) or directly in `msgid`. `msgid_plural` /
`msgstr[n]` become a plural message (`%d` becomes `#`); entries marked `#, fuzzy` are skipped.

```po
msgid ""
msgstr "Language: fr\n"

#. main menu button
msgctxt "menu.play"
msgid "Play"
msgstr "Jouer"

msgctxt "hud.coins"
msgid "%d coin"
msgid_plural "%d coins"
msgstr[0] "%d pièce"
msgstr[1] "%d pièces"
```

Keys are free-form (`[A-Za-z0-9_.:/-]`); dotted groups such as `menu.play`, `hud.coins`,
`dlg.intro.start.001` keep tables readable.

## Messages: placeholders, plurals, numbers

Strings use a subset of the ICU message format:

| Syntax | Result |
|---|---|
| `Hello, {name}!` | Replaced by the argument; numbers are formatted for the locale |
| `{n, number}` / `{n, number, integer}` / `{n, number, percent}` | `1,234.5` (en), `1.234,5` (de), `1 234,5` (fr); rounded; `25 %` |
| `{n, plural, =0 {no coins} one {# coin} other {# coins}}` | Plural category of `n` in the locale; `#` is `n` formatted; `=N` matches exactly; `offset:1` is supported |
| `{n, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}` | English ordinals (other languages use `other`) |
| `{g, select, female {She} male {He} other {They}}` | Picks by a string value |
| `It''s '{not an argument}'` | `''` is an apostrophe; `'{...}'` quotes braces |

Plural categories follow CLDR: English and most European languages have `one` / `other`; French
and Portuguese count 0 and 1 as `one`; Russian, Ukrainian and Polish add `few` / `many`; Czech and
Slovak `few` / `many`; Arabic has `zero`, `one`, `two`, `few`, `many`, `other`; Japanese, Chinese,
Korean, Vietnamese and Thai use only `other`. Every plural or select needs an `other` case.

## Using keys

| Where | How |
|---|---|
| UI text (`ui.text`, `ui.placeholder`) | Write `"@menu.play"`. `{placeholders}` are filled from the element entity's vars (`"@hud.coins"` with var `n` shows `3 coins`). `"@@x"` shows `@x` |
| World text (`text.text`) | Same: `"@sign.shop"` |
| Scripts | `self.ui.text = tr("hud.coins", {n: coins})` |
| Dialogue | Tag a line or a choice: `Guard: Halt! #line:dlg.gate.halt`. The script's own text is the source-language fallback; translations may use `{$var}` like the script. Speakers are shown through an optional key `speaker.<Name>` |

The scene keeps the key: `entity_get` shows `@menu.play`, while `ui_inspect`, captures and the
editor show the localized text. Missing keys show the key itself and are recorded (`locale_check`
lists them under `runtimeMissing`).

## The locale

The locale shown, from first to last choice:

1. `set_locale(code)` in Wander, or `locale_set` while playing: undone when play stops, so play
   sessions replay; store the player's choice in a save game to keep it.
2. `locale_set` while editing: a preview language for the editor, kept until changed.
3. The player's system locale in a shipped game, when `useSystemLocale` is on and the game has strings
   for it (the standalone player passes it in; the editor and tests never do).
4. `game.json` `localization.locale`.
5. The source language.

```json
"localization": {"source": "en", "locale": "", "useSystemLocale": true, "maxLengthRatio": 1.3}
```

Lookups walk the fallback chain: `pt-BR` → `pt` → the source language. A string missing in Brazilian
Portuguese but present in Portuguese shows the Portuguese one; missing in both shows the source.

Tables reload when a file under `locale/` changes (checked at most once a second, immediately after
an asset refresh), so translators can edit a CSV while the game runs.

## Wander

| Function | Notes |
|---|---|
| `tr(key, args?)` | The localized string; `args` is a map for placeholders, plurals and selects |
| `set_locale(code)` | Switches the language (a language menu); an unknown locale is an error with did-you-mean |
| `locale()` | The current locale code |
| `locales()` | Locale codes with strings (and the source language), sorted |

```text
on ui "Language"                    -- a button that cycles through the languages
  let all = locales()
  set_locale(all[(all.index_of(locale()) + 1) % all.length])
  find("Language").ui.text = "@menu.language"
end
on start
  if locale() == "ja" then self.ui.styleOverrides = {font: "fonts/NotoSansJP-Regular.otf"} end
end
```

## Tools

| Tool | Use |
|---|---|
| `locale_list {}` | Current locale and chain, source, every locale with string counts, coverage and files, runtime misses, file problems |
| `locale_set {locale}` | Preview (editing) or switch (playing) the language; `""` returns to the default |
| `locale_check {locale?, budget?}` | Findings: missing translations per locale, keys used but undefined, unused keys, placeholder mismatches, invalid messages, plurals missing forms the language needs, strings over budget (`max` column, or source length × `budget`), characters no font covers |
| `locale_extract {apply?, file?}` | Hard-coded texts in the open scene, scenes, prefabs, dialogue and scripts with a proposed key each; `apply: true` writes `locale/strings.csv`, rewrites the open scene's fields to `@key` (one undoable edit) and tags dialogue lines with `#line:` |
| `locale_pseudo {expand?}` | Writes the pseudo-locale `en-XA` (`[Ƥļáý~~]`: accented, about 30% longer, placeholders intact) and switches to it |

## Workflow for agents

1. **Extract**: `locale_extract {}` to review, then `locale_extract {"apply": true}`. Replace the
   script strings it reports with `tr("key")`.
2. **Translate**: add a column per locale to `locale/strings.csv` (or `.po` catalogs). Keep
   placeholders and plural keywords; translate only the text.
3. **Check**: `locale_check {}` until `missing`, `undefined`, `placeholders`, `syntax` and `plurals` are
   empty for the shipped locales.
4. **Test layout**: `locale_pseudo {}`, then `viewport_capture` and `ui_inspect` each screen: clipped or
   overflowing text and texts that stay plain (still hard-coded) show up. Repeat with the longest real
   locale (`locale_set {"locale": "de"}`) and `locale_check {"budget": 1.3}`.
5. **Ship**: set `game.json` `localization.source` and `useSystemLocale`; `locale_list` confirms coverage.

## Fonts and scripts

The built-in fonts (Inter, EB Garamond, JetBrains Mono) cover Latin, Latin Extended, Greek and
Cyrillic. Text falls back between them glyph by glyph, but **none covers** Chinese, Japanese, Korean,
Arabic, Hebrew, Devanagari or Thai. Those characters render as missing glyphs until the project adds a
font for them. `locale_check` lists them per locale under `glyphs`.

- Add an OpenType/TrueType font that covers the script to the project (for example the Noto Sans
  families, SIL Open Font License: Noto Sans JP / SC / TC / KR, Noto Sans Arabic, Noto Sans Hebrew,
  Noto Sans Devanagari, Noto Sans Thai) and record its license.
- Use it for that locale: a style sheet per locale, or from a script
  (`if locale() == "ja" then self.ui.styleOverrides = {font: "fonts/NotoSansJP-Regular.otf"} end`, or
  `self.text.font = ...` for world text).
- CJK text wraps between any two characters; keep `max` budgets in mind (CJK strings are short but wide).

## Limits

- No bidirectional layout or contextual shaping: Arabic, Hebrew and Persian draw left to right with
  unjoined letters. Ship those locales only with pre-shaped text (presentation forms) and right-aligned
  elements, or not yet.
- Number formatting covers decimal and grouping separators and percent; no currencies, dates or
  locale digits (Arabic text uses Latin digits).
- Plural rules cover the languages listed above; other languages use the English rules.
- Translated dialogue lines use the dialogue's `{$var}` interpolation, not ICU plurals: branch with
  `<<if>>` in the script for plural-sensitive lines.
