# Localization

A game's texts live in string tables under `locale/`, one key per string and one translation per locale. UI and world texts refer to keys as `"@key"`, scripts call `tr(key, args)`, and dialogue lines carry a `#line:<id>` tag. The engine picks the locale, falls back sensibly when a string is missing, formats plurals and numbers per language, and reloads the tables when a translator edits them while the game runs.

## String tables

Every `*.csv` and `*.po` file under `locale/` is loaded and merged. CSV suits spreadsheets: a header `key,<locale>,<locale>...`, optional `comment` and `max` (length budget) columns, and an empty cell for "not translated".

```csv
key,en,fr,pt-BR,comment,max
menu.play,Play,Jouer,Jogar,main menu button,10
hud.coins,"{n, plural, one {# coin} other {# coins}}","{n, plural, one {# pièce} other {# pièces}}",,
hud.welcome,"Welcome, {name}!","Bienvenue, {name} !",,
```

gettext catalogs (`fr.po`, `pt_BR.po`, or files in `locale/fr/`) work too: keys go in `msgctxt` (or `msgid`), plural entries become plural messages, and fuzzy entries are skipped.

## Messages

Strings use a subset of the ICU message format: `{name}` placeholders, `{n, number}` and `{n, number, percent}` formatted for the locale, `{n, plural, =0 {no coins} one {# coin} other {# coins}}` with the language's CLDR plural categories, `{n, selectordinal, ...}`, and `{g, select, female {She} male {He} other {They}}`. Every plural or select needs an `other` case.

## Using keys

| Where | How |
|---|---|
| UI text (`ui.text`, `ui.placeholder`) | `"@menu.play"`; placeholders are filled from the element's vars (`"@hud.coins"` with var `n`) |
| World text (`text.text`) | `"@sign.shop"` |
| Scripts | `self.ui.text = tr("hud.coins", {n: coins})` |
| Dialogue | `Guard: Halt! #line:dlg.gate.halt`: the script's own text is the source-language fallback |

The scene keeps the key (`entity_get` shows `@menu.play`), while `ui_inspect`, captures and the editor show the localized text. A missing key shows the key itself and is recorded for `locale_check`.

## The locale

The locale shown, from first to last choice:

1. `set_locale(code)` in Wander, or `locale_set` while playing (undone when play stops; keep the player's choice in a save game).
2. `locale_set` while editing: a preview language, also offered by the language menu in the editor's viewport toolbar.
3. The player's system language in a shipped game, when `useSystemLocale` is on and the game has strings for it. On macOS that is the first language in System Settings > General > Language & Region.
4. `game.json` `localization.locale`, then the source language.

```json
"localization": {"source": "en", "locale": "", "useSystemLocale": true, "maxLengthRatio": 1.3}
```

Lookups walk a fallback chain: `pt-BR`, then `pt`, then the source language.

```wander
on ui "Language"
  let all = locales()
  set_locale(all[(all.index_of(locale()) + 1) % all.length])
end
```

## Workflow for agents

```tool
locale_extract {}
locale_extract {"apply": true}
locale_check {}
locale_pseudo {}
locale_set {"locale": "de"}
locale_check {"budget": 1.3}
locale_list {}
```

1. **Extract** hard-coded texts: review the proposals, then apply them (`locale/strings.csv`, `@key` fields, `#line:` tags). Replace the script strings it reports with `tr("key")`.
2. **Translate**: add a column per locale, keeping placeholders and plural keywords.
3. **Check** until missing translations, undefined keys, placeholder mismatches, syntax errors and missing plural forms are gone for the shipped locales.
4. **Test layout** with the pseudo-locale `en-XA` (accented and about 30% longer), then with the longest real language, using `viewport_capture` and `ui_inspect` on each screen.
5. **Ship** with `localization.source` and `useSystemLocale` set.

## Fonts and limits

The built-in fonts cover Latin, Greek and Cyrillic. Chinese, Japanese, Korean, Arabic, Hebrew, Devanagari and Thai need a font added to the project (for example a Noto Sans family) and selected for that locale; `locale_check` lists characters no font covers. Right-to-left layout and contextual shaping are not supported yet, and number formatting covers separators and percent but not currencies or dates.

## See also

- Tools: [`locale_list`](../reference/tools/render.md#locale_list), [`locale_set`](../reference/tools/render.md#locale_set), [`locale_check`](../reference/tools/render.md#locale_check), [`locale_extract`](../reference/tools/render.md#locale_extract), [`locale_pseudo`](../reference/tools/render.md#locale_pseudo).
- Related pages: [2D and UI](2d-ui.md), [Shipping](shipping.md).
- Design document: [docs/LOCALIZATION.md](https://github.com/amirhossein-razlighi/Skywalker/blob/main/docs/LOCALIZATION.md).
