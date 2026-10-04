// Localization (docs/LOCALIZATION.md): CSV and PO tables, CLDR plurals, fallback chains, ICU-style
// messages with locale number formatting, "@key" texts in UI and world text, dialogue line ids, the
// Wander builtins, hot reload, and the locale_* tools (check, extract, pseudo-locale).

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/locale/LocaleAudit.h"
#include "skywalker/locale/Localization.h"
#include "skywalker/ui/World2D.h"

using namespace sky;
namespace fs = std::filesystem;

/// Category names: doctest cannot print the enum (its stringify finds loc::toString by ADL).
std::string cat(loc::Plural p) { return loc::toString(p); }

namespace {

struct Project {
    fs::path dir;
    explicit Project(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-locale-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
    }
    ~Project() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
    std::string read(const std::string& rel) const {
        std::ifstream f(dir / rel, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

std::unique_ptr<Engine> makeEngine(const Project& p) {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Null;
    cfg.projectDir = p.dir.string();
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Main", false);
    return e;
}

EntityId make(Engine& e, const std::string& doc) {
    auto parsed = Json::parse(doc);
    REQUIRE(parsed);
    EntityId id = e.scene().create(parsed.value().get("name").asString());
    Status st = e.scene().applyEntityJson(id, parsed.value());
    INFO((st.ok() ? std::string() : st.error().message));
    REQUIRE(st);
    return id;
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

std::string fmt(const std::string& msg, const std::string& args, const std::string& locale) {
    return loc::formatMessage(msg, Json::parse(args).value(), locale);
}

const std::string kNbsp = "\xC2\xA0", kNarrow = "\xE2\x80\xAF";

const char* kStrings = "key,en,fr,pt,ru,comment,max\n"
                       "menu.play,Play,Jouer,Jogar,Играть,main menu button,8\n"
                       "menu.quit,Quit,Quitter,Sair,,,\n"
                       "hud.coins,\"{n, plural, one {# coin} other {# coins}}\",\"{n, plural, one {# pièce} other {# pièces}}\",,"
                       "\"{n, plural, one {# монета} few {# монеты} many {# монет} other {# монеты}}\",,\n"
                       "hud.welcome,\"Welcome, {name}!\",\"Bienvenue, {name} !\",,,,\n";

}  // namespace

TEST_CASE("locale: CSV tables - quoting, multi-line cells, metadata columns, comments and duplicates") {
    std::string csv = "\xEF\xBB\xBF# exported from a spreadsheet\nkey,en,fr,comment,max\n"
                      "a,\"Hello, world\",\"Bonjour, \"\"monde\"\"\",greeting,12\n"
                      "b,\"Line one\nLine two\",,,\n"
                      "# a comment row\n"
                      "a,again,,,\n";
    loc::StringTable t = loc::parseCsv(csv, "locale/strings.csv");
    REQUIRE(t.entries.count("a"));
    CHECK(t.entries["a"].text["en"] == "Hello, world");
    CHECK(t.entries["a"].text["fr"] == "Bonjour, \"monde\"");
    CHECK(t.entries["a"].comment == "greeting");
    CHECK(t.entries["a"].maxLength == 12);
    CHECK(t.entries["b"].text["en"] == "Line one\nLine two");
    CHECK_FALSE(t.entries["b"].text.count("fr"));  // empty cell = no translation
    CHECK((t.locales == std::set<std::string>{"en", "fr"}));
    REQUIRE(t.problems.size() == 1);
    CHECK(t.problems[0].code == "duplicate_key");
    CHECK(t.problems[0].line == 7);
    CHECK(loc::parseCsv("name,en\nx,y\n", "f.csv").problems.front().code == "invalid_csv");
    CHECK(loc::csvField("a,b") == "\"a,b\"");
    CHECK(loc::csvField("say \"hi\"") == "\"say \"\"hi\"\"\"");
}

TEST_CASE("locale: gettext catalogs - keys in msgctxt or msgid, plurals, escapes, fuzzy entries") {
    std::string po = R"(# French
msgid ""
msgstr ""
"Language: fr\n"
"Plural-Forms: nplurals=2; plural=(n > 1);\n"

#. main menu
msgctxt "menu.play"
msgid "Play"
msgstr "Jouer"

msgid "menu.quit"
msgstr "Quit\"ter\"\n"

msgctxt "hud.coins"
msgid "%d coin"
msgid_plural "%d coins"
msgstr[0] "%d pièce"
msgstr[1] "%d pièces"

#, fuzzy
msgctxt "hud.lives"
msgid "Lives"
msgstr "Vies"
)";
    loc::StringTable t = loc::parsePo(po, "locale/fr.po", "whatever", "en");
    CHECK(t.locales.count("fr"));
    CHECK(t.entries["menu.play"].text["fr"] == "Jouer");
    CHECK(t.entries["menu.play"].text["en"] == "Play");  // msgid is the source text when msgctxt is the key
    CHECK(t.entries["menu.play"].comment == "main menu");
    CHECK(t.entries["menu.quit"].text["fr"] == "Quit\"ter\"\n");
    CHECK(t.entries["hud.coins"].text["fr"] == "{count, plural, one {# pièce} other {# pièces}}");
    CHECK(loc::formatMessage(t.entries["hud.coins"].text["fr"], Json::object({{"count", 2}}), "fr") == "2 pièces");
    CHECK(loc::formatMessage(t.entries["hud.coins"].text["en"], Json::object({{"count", 1}}), "en") == "1 coin");
    CHECK_FALSE(t.entries["hud.lives"].text.count("fr"));  // fuzzy: not used
    bool fuzzy = false;
    for (const auto& p : t.problems) fuzzy = fuzzy || p.code == "fuzzy";
    CHECK(fuzzy);
    // Russian gettext order (one, few, many) maps onto CLDR categories; the locale comes from the file name.
    std::string ru = "msgid \"apples\"\nmsgid_plural \"apples\"\nmsgstr[0] \"%d яблоко\"\nmsgstr[1] \"%d яблока\"\nmsgstr[2] \"%d яблок\"\n";
    loc::StringTable r = loc::parsePo(ru, "locale/ru.po", "ru", "en");
    const std::string& m = r.entries["apples"].text["ru"];
    CHECK(loc::formatMessage(m, Json::object({{"count", 21}}), "ru") == "21 яблоко");
    CHECK(loc::formatMessage(m, Json::object({{"count", 3}}), "ru") == "3 яблока");
    CHECK(loc::formatMessage(m, Json::object({{"count", 11}}), "ru") == "11 яблок");
}

TEST_CASE("locale: CLDR plural rules for en, fr, ru, ar, ja, pl and cs") {
    CHECK(cat(loc::pluralCategory("en", 1)) == "one");
    CHECK(cat(loc::pluralCategory("en", 0)) == "other");
    CHECK(cat(loc::pluralCategory("en", 1.5)) == "other");
    CHECK(cat(loc::pluralCategory("en-GB", 2)) == "other");
    CHECK(cat(loc::pluralCategory("fr", 0)) == "one");
    CHECK(cat(loc::pluralCategory("fr", 1.5)) == "one");
    CHECK(cat(loc::pluralCategory("fr", 2)) == "other");
    CHECK(cat(loc::pluralCategory("ru", 1)) == "one");
    CHECK(cat(loc::pluralCategory("ru", 21)) == "one");
    CHECK(cat(loc::pluralCategory("ru", 3)) == "few");
    CHECK(cat(loc::pluralCategory("ru", 12)) == "many");
    CHECK(cat(loc::pluralCategory("ru", 25)) == "many");
    CHECK(cat(loc::pluralCategory("ru", 1.5)) == "other");
    CHECK(cat(loc::pluralCategory("ar", 0)) == "zero");
    CHECK(cat(loc::pluralCategory("ar", 1)) == "one");
    CHECK(cat(loc::pluralCategory("ar", 2)) == "two");
    CHECK(cat(loc::pluralCategory("ar", 7)) == "few");
    CHECK(cat(loc::pluralCategory("ar", 11)) == "many");
    CHECK(cat(loc::pluralCategory("ar", 100)) == "other");
    CHECK(cat(loc::pluralCategory("ja", 1)) == "other");
    CHECK(cat(loc::pluralCategory("ja", 5)) == "other");
    CHECK(cat(loc::pluralCategory("pl", 22)) == "few");
    CHECK(cat(loc::pluralCategory("pl", 25)) == "many");
    CHECK(cat(loc::pluralCategory("cs", 3)) == "few");
    CHECK(cat(loc::pluralCategory("cs", 2.5)) == "many");
    CHECK(cat(loc::pluralCategory("pt", 0)) == "one");
    CHECK(cat(loc::pluralCategory("pt-PT", 0)) == "other");
    CHECK(cat(loc::ordinalCategory("en", 22)) == "two");
    CHECK(cat(loc::ordinalCategory("en", 13)) == "other");
    CHECK(loc::pluralCategories("ar").size() == 6);
    CHECK(loc::pluralCategories("ja").size() == 1);
    // Messages pick the case for the message's locale.
    const std::string apples = "{n, plural, zero {no apples} one {one apple} two {two apples} few {# apples (few)} many {# apples (many)} other {# apples}}";
    CHECK(fmt(apples, R"({"n": 2})", "ar") == "two apples");
    CHECK(fmt(apples, R"({"n": 2})", "en") == "2 apples");
    CHECK(fmt(apples, R"({"n": 0})", "fr") == "one apple");
    CHECK(fmt("{n, plural, =0 {none} one {# item} other {# items}}", R"({"n": 0})", "en") == "none");
    CHECK(fmt("{n, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}", R"({"n": 23})", "en") == "23rd");
}

TEST_CASE("locale: interpolation, number formatting, select and quoting") {
    CHECK(fmt("Hi {name}, you have {n} points", R"({"name": "Ada", "n": 1234.5})", "en") == "Hi Ada, you have 1,234.5 points");
    CHECK(fmt("{n}", R"({"n": 1234.5})", "de") == "1.234,5");
    CHECK(fmt("{n}", R"({"n": 1234567})", "fr") == "1" + kNarrow + "234" + kNarrow + "567");
    CHECK(fmt("{n}", R"({"n": 1234})", "es") == "1234");  // Spanish groups from 10 000
    CHECK(fmt("{n}", R"({"n": 12345})", "es") == "12.345");
    CHECK(fmt("{n, number, integer}", R"({"n": 2.6})", "en") == "3");
    CHECK(fmt("{r, number, percent}", R"({"r": 0.25})", "en") == "25%");
    CHECK(fmt("{r, number, percent}", R"({"r": 0.25})", "de") == "25" + kNbsp + "%");
    CHECK(fmt("{n, plural, one {# coin} other {# coins}}", R"({"n": 2500})", "en") == "2,500 coins");
    CHECK(fmt("{g, select, female {She} male {He} other {They}} won", R"({"g": "female"})", "en") == "She won");
    CHECK(fmt("{g, select, female {She} male {He} other {They}} won", R"({"g": "x"})", "en") == "They won");
    CHECK(fmt("It''s '{literal}' {n}", R"({"n": 1})", "en") == "It's {literal} 1");
    std::vector<std::string> problems;
    CHECK(loc::formatMessage("Hi {name}", Json::object(), "en", &problems) == "Hi {name}");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("name") != std::string::npos);
    CHECK_FALSE(loc::validateMessage("{n, plural, one {x}}"));  // no other
    CHECK_FALSE(loc::validateMessage("Hi {name"));
    CHECK(loc::validateMessage("{n, plural, one {# x} other {# y}}"));
    CHECK((loc::placeholders("{a} {n, plural, one {{b}} other {#}}") == std::set<std::string>{"a", "b", "n"}));
}

TEST_CASE("locale: fallback chains, settings and the system locale") {
    CHECK(loc::normalizeLocale("pt_BR.UTF-8") == "pt-BR");
    CHECK(loc::normalizeLocale("zh_hant_tw") == "zh-Hant-TW");
    CHECK(loc::normalizeLocale("C") == "");
    CHECK((loc::fallbackChain("pt-BR", "en") == std::vector<std::string>{"pt-BR", "pt", "en"}));
    CHECK((loc::fallbackChain("en-US", "en") == std::vector<std::string>{"en-US", "en"}));

    Project p("chain");
    p.write("locale/strings.csv", kStrings);
    p.write("locale/pt_BR.po", "msgid \"\"\nmsgstr \"Language: pt-BR\\n\"\n\nmsgid \"menu.quit\"\nmsgstr \"Fechar\"\n");
    auto e = makeEngine(p);
    loc::Localization& l = e->world2d().localization();
    CHECK(l.locale() == "en");
    REQUIRE(l.setLocale("pt-BR", loc::Localization::Scope::Preview));
    CHECK((l.chain() == std::vector<std::string>{"pt-BR", "pt", "en"}));
    CHECK(l.tr("menu.quit") == "Fechar");                               // pt-BR
    CHECK(l.tr("menu.play") == "Jogar");                                // pt
    CHECK(l.tr("hud.welcome", Json::object({{"name", "Ana"}})) == "Welcome, Ana!");  // source
    CHECK(l.tr("menu.nope") == "menu.nope");                            // missing: the key, recorded
    REQUIRE(l.misses().size() == 1);
    CHECK(l.misses()[0].locale == "pt-BR");
    Status bad = l.setLocale("fz", loc::Localization::Scope::Preview);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "unknown_locale");
    CHECK(bad.error().hint.find("fr") != std::string::npos);  // did you mean

    // game.json picks the locale; a shipped game's system locale wins when it has strings.
    p.write("game.json", R"({"localization": {"source": "en", "locale": "fr"}})");
    l.clearLocale(loc::Localization::Scope::Preview);
    l.refresh(true);
    CHECK(l.locale() == "fr");
    l.setSystemLocale("ru_RU.UTF-8");
    CHECK(l.locale() == "ru-RU");  // chain ru-RU -> ru -> en
    l.setSystemLocale("de_DE.UTF-8");  // no German strings: game.json still decides
    CHECK(l.locale() == "fr");
    p.write("game.json", R"({"localization": {"locale": "fr", "useSystemLocale": false}})");
    l.setSystemLocale("ru_RU.UTF-8");
    l.refresh(true);
    CHECK(l.locale() == "fr");
    CHECK_FALSE(game::GameSettings::fromJson(Json::parse(R"({"localization": {"lokale": "fr"}})").value()));
    CHECK(game::GameSettings::fromJson(Json::parse(R"({"localization": {"locale": "fr"}})").value()));
}

TEST_CASE("locale: \"@key\" texts in UI and world text follow the locale; hot reload") {
    Project p("ui");
    p.write("locale/strings.csv", kStrings);
    auto e = makeEngine(p);
    make(*e, R"({"name": "Menu", "components": {"ui_canvas": {}}})");
    EntityId play = make(*e, R"({"name": "Play", "components": {"ui": {"widget": "button", "text": "@menu.play"}}})");
    REQUIRE(e->scene().setParent(play, e->scene().find("Menu")));
    EntityId coins = make(*e, R"({"name": "Coins", "components": {"ui": {"widget": "text", "text": "@hud.coins"}}, "vars": {"n": 3}})");
    REQUIRE(e->scene().setParent(coins, e->scene().find("Menu")));
    EntityId at = make(*e, R"({"name": "At", "components": {"ui": {"widget": "text", "text": "@@home"}}})");
    REQUIRE(e->scene().setParent(at, e->scene().find("Menu")));
    auto textOf = [&](EntityId id) {
        ui::Layout lay = e->world2d().ui().computeLayout(e->scene(), 1280, 720);
        const ui::Node* n = lay.find(id);
        REQUIRE(n);
        return n->el.text;
    };
    CHECK(textOf(play) == "Play");
    CHECK(textOf(coins) == "3 coins");  // the entity's vars fill the placeholders
    CHECK(textOf(at) == "@home");
    call(*e, "locale_set", R"({"locale": "fr"})");
    CHECK(textOf(play) == "Jouer");
    CHECK(textOf(coins) == "3 pièces");
    CHECK(e->scene().get<UIElement>(play)->text == "@menu.play");  // the scene keeps the key

    // World text: the glyphs drawn are the translation's.
    make(*e, R"({"name": "Sign", "components": {"text": {"text": "@menu.quit"}}})");
    auto glyphs = [&e] {
        CaptureOptions o;
        o.width = 320;
        o.height = 180;
        return e->frame(o).render2d.sprites.size();  // the sign is the only 2D content: one quad per glyph
    };
    const size_t fr = glyphs();  // "Quitter"
    call(*e, "locale_set", R"({"locale": ""})");
    const size_t en = glyphs();  // "Quit"
    CHECK(fr > en);

    // Hot reload: editing the table shows on the next lookup after a refresh.
    std::string changed = kStrings;
    changed.replace(changed.find("Play,Jouer"), 10, "Start,Jouer");
    p.write("locale/strings.csv", changed);
    e->world2d().invalidate((p.dir / "locale/strings.csv").string());
    CHECK(textOf(play) == "Start");
}

TEST_CASE("locale: dialogue lines with #line ids, choices and speakers") {
    Project p("dialogue");
    p.write("locale/dialogue.csv", "key,en,fr\n"
                                   "intro.greet,\"Hello, {$name}.\",\"Bonjour, {$name}.\"\n"
                                   "intro.yes,,Oui\n"
                                   "speaker.Guard,,Garde\n");
    auto e = makeEngine(p);
    // Inline source (a script file works the same way).
    Json talk = Json::object({{"name", "Talk"},
                              {"components", Json::object({{"dialogue", Json::object({{"ui", "none"}, {"typewriter", 0},
                                                                                        {"source", "title: Start\n---\n<<declare $name = \"Ada\">>\n"
                                                                                                   "Guard: Hello, {$name}. #line:intro.greet\n"
                                                                                                   "-> Yes #line:intro.yes\n    Guard: Good.\n-> No\n===\n"}})}})}});
    EntityId runner = make(*e, talk.dump());
    REQUIRE(e->world2d().localization().setLocale("fr", loc::Localization::Scope::Preview));
    e->play();
    e->step(1);
    Status started = e->world2d().startDialogue(e->scene(), runner, "Start", &e->runtime());
    INFO((started ? std::string() : started.error().message));
    REQUIRE(started);
    Json st = e->world2d().dialogueState(e->scene(), runner);
    CHECK(st.get("line").asString() == "Bonjour, Ada.");
    CHECK(st.get("speaker").asString() == "Garde");
    REQUIRE(e->world2d().advanceDialogue(e->scene(), runner, &e->runtime()));
    st = e->world2d().dialogueState(e->scene(), runner);
    REQUIRE(st.get("choices").size() == 2);
    CHECK(st.get("choices")[0].asString() == "Oui");
    CHECK(st.get("choices")[1].asString() == "No");  // untagged: as written
    e->stop();
}

TEST_CASE("locale: Wander tr, set_locale, locale and locales; set_locale is undone when play stops") {
    Project p("wander");
    p.write("locale/strings.csv", kStrings);
    auto e = makeEngine(p);
    EntityId hud = make(*e, R"({"name": "Hud"})");
    REQUIRE(e->scene().setBehaviors(hud, Json::array({Json::object({{"name", "Hud"}, {"source", R"(var label = ""
var lang = ""
var count = 0
on start
  label = tr("hud.coins", {n: 1})
  count = locales().length
  set_locale("ru")
end
on tick
  if frame == 2 then
    lang = locale()
    label = tr("hud.coins", {n: 5})
  end
end)"}})})));
    e->play();
    e->step(4);
    const EntityRecord* r = e->scene().record(hud);
    CHECK(r->vars.get("count").asInt() == 4);  // en, fr, pt, ru
    CHECK(r->vars.get("lang").asString() == "ru");
    CHECK(r->vars.get("label").asString() == "5 монет");
    e->stop();
    CHECK(e->world2d().localization().locale() == "en");
}

TEST_CASE("locale: locale_check finds missing, undefined, unused, placeholder, plural, overlong and glyph problems") {
    Project p("check");
    p.write("locale/strings.csv", "key,en,fr,ru,ja,max\n"
                                  "menu.play,Play,Jouer,Играть,プレイ,\n"
                                  "menu.title,The Long Road,La très très longue route du destin,,,\n"
                                  "hud.score,Score: {score},Score : {points},,,\n"
                                  "hud.coins,\"{n, plural, one {# coin} other {# coins}}\",,\"{n, plural, one {# монета} other {# монет}}\",,\n"
                                  "old.key,Unused,,,,\n"
                                  "hud.short,OK,D'accord,,,2\n");
    auto e = makeEngine(p);
    make(*e, R"({"name": "Menu", "components": {"ui_canvas": {}}})");
    make(*e, R"({"name": "Play", "components": {"ui": {"widget": "button", "text": "@menu.play"}}})");
    make(*e, R"({"name": "Title", "components": {"text": {"text": "@menu.title"}}})");
    make(*e, R"({"name": "Ghost", "components": {"ui": {"widget": "text", "text": "@menu.ghost"}}})");
    make(*e, R"({"name": "Score", "components": {"ui": {"widget": "text", "text": "@hud.score"}}})");
    make(*e, R"({"name": "Ok", "components": {"ui": {"widget": "button", "text": "@hud.short"}}})");
    p.write("scripts/hud.wander", "on tick\n  self.ui.text = tr(\"hud.coins\", {n: 2})\nend\n");

    Json r = call(*e, "locale_check", R"({"budget": 1.5})");
    const Json& s = r.get("summary");
    CHECK(s.get("keys").asInt() == 6);
    // Missing translations per locale.
    std::map<std::string, int64_t> missing;
    for (const auto& m : r.get("missing").elements()) missing[m.get("locale").asString()] = m.get("missing").asInt();
    CHECK(missing["fr"] == 2);  // hud.coins, old.key
    CHECK(missing["ja"] == 5);
    // Used but undefined, and defined but unused.
    REQUIRE(r.get("undefined").size() == 1);
    CHECK(r.get("undefined")[0].get("key").asString() == "menu.ghost");
    REQUIRE(r.get("unused").size() == 1);
    CHECK(r.get("unused")[0].get("key").asString() == "old.key");
    // {score} vs {points}.
    REQUIRE(r.get("placeholders").size() == 1);
    CHECK(r.get("placeholders")[0].get("missing")[0].asString() == "score");
    CHECK(r.get("placeholders")[0].get("extra")[0].asString() == "points");
    // Russian needs few and many.
    REQUIRE(r.get("plurals").size() == 1);
    CHECK(r.get("plurals")[0].get("locale").asString() == "ru");
    // Over budget: the long French title (ratio) and "D'accord" (max 2, also the source "OK" fits).
    std::set<std::string> overlong;
    for (const auto& o : r.get("overlong").elements()) overlong.insert(o.get("key").asString() + "/" + o.get("locale").asString());
    CHECK(overlong.count("menu.title/fr"));
    CHECK(overlong.count("hud.short/fr"));
    CHECK_FALSE(overlong.count("menu.play/fr"));
    // Japanese needs a font with CJK glyphs.
    bool ja = false;
    for (const auto& g : r.get("glyphs").elements()) ja = ja || g.get("locale").asString() == "ja";
    CHECK(ja);
    CHECK(r.get("warnings").isArray());
}

TEST_CASE("locale: locale_extract proposes keys for hard-coded texts and applies them") {
    Project p("extract");
    p.write("story/intro.dialogue", "title: Start\n---\nGuard: Halt! Who goes there?\n-> A friend\n-> Nobody #line:intro.nobody\n===\n");
    p.write("scripts/over.wander", "on event \"dead\"\n  self.ui.text = \"Game Over\"\n  play_sound(\"audio/sad.wav\")\nend\n");
    auto e = makeEngine(p);
    EntityId canvas = make(*e, R"({"name": "Main Menu", "components": {"ui_canvas": {}}})");
    EntityId play = make(*e, R"({"name": "Play", "components": {"ui": {"widget": "button", "text": "Play"}}})");
    EntityId quit = make(*e, R"({"name": "Quit Button", "components": {"ui": {"widget": "button", "text": "Quit"}}})");
    EntityId done = make(*e, R"({"name": "Keyed", "components": {"ui": {"widget": "button", "text": "@menu.keyed"}}})");
    EntityId num = make(*e, R"({"name": "Number", "components": {"ui": {"widget": "text", "text": "42"}}})");
    for (EntityId c : {play, quit, done, num}) REQUIRE(e->scene().setParent(c, canvas));
    EntityId sign = make(*e, R"({"name": "Shop Sign", "components": {"text": {"text": "Potions & Elixirs"}}})");

    Json r = call(*e, "locale_extract", "{}");
    std::map<std::string, std::string> proposed;  // text -> key
    for (const auto& pr : r.get("proposals").elements()) proposed[pr.get("text").asString()] = pr.get("proposedKey").asString();
    CHECK(proposed["Play"] == "main_menu.play");
    CHECK(proposed["Quit"] == "main_menu.quit_button");
    CHECK(proposed["Potions & Elixirs"] == "main.shop_sign");
    CHECK(proposed["Halt! Who goes there?"] == "dlg.intro.start.001");
    CHECK(proposed.count("A friend"));
    CHECK(proposed.count("Game Over"));
    CHECK_FALSE(proposed.count("42"));
    CHECK_FALSE(proposed.count("Nobody"));          // already has a line id
    CHECK_FALSE(proposed.count("audio/sad.wav"));  // a path

    Json applied = call(*e, "locale_extract", R"({"apply": true})");
    CHECK(applied.get("applied").get("fields").asInt() == 3);
    CHECK(applied.get("applied").get("dialogueLines").asInt() == 2);
    CHECK(e->scene().get<UIElement>(play)->text == "@main_menu.play");
    CHECK(e->scene().get<Text>(sign)->text == "@main.shop_sign");
    CHECK(p.read("story/intro.dialogue").find("Halt! Who goes there? #line:dlg.intro.start.001") != std::string::npos);
    CHECK(p.read("locale/strings.csv").find("main_menu.play,Play") != std::string::npos);
    bool scriptHint = false;
    for (const auto& w : applied.get("warnings").elements()) scriptHint = scriptHint || w.asString().find("tr(") != std::string::npos;
    CHECK(scriptHint);
    // The texts still read the same, now through the table; and the extraction is undoable.
    ui::Layout lay = e->world2d().ui().computeLayout(e->scene(), 1280, 720);
    CHECK(lay.find(play)->el.text == "Play");
    call(*e, "history", R"({"action": "undo"})");
    CHECK(e->scene().get<UIElement>(play)->text == "Play");
}

TEST_CASE("locale: locale_pseudo writes an accented, longer pseudo-locale that keeps placeholders") {
    Project p("pseudo");
    p.write("locale/strings.csv", kStrings);
    auto e = makeEngine(p);
    Json r = call(*e, "locale_pseudo", "{}");
    CHECK(r.get("locale").asString() == "en-XA");
    CHECK(r.get("strings").asInt() == 4);
    CHECK(r.get("current").asString() == "en-XA");
    loc::Localization& l = e->world2d().localization();
    const std::string play = l.tr("menu.play");
    CHECK(play.front() == '[');
    CHECK(play.find("Play") == std::string::npos);
    CHECK(loc::visibleLength(play) >= 4 + 2 + 1);  // brackets and padding
    const std::string welcome = l.tr("hud.welcome", Json::object({{"name", "Ada"}}));
    CHECK(welcome.find("Ada") != std::string::npos);  // {name} survived
    CHECK(l.tr("hud.coins", Json::object({{"n", 2}})).find("2 ") != std::string::npos);
    CHECK(loc::pseudoLocalize("<b>Hi</b>", 0.0) == "[<b>Ĥí</b>]");
    call(*e, "locale_set", R"({"locale": "zz"})", false);
    Json list = call(*e, "locale_list", "{}");
    CHECK(list.get("current").asString() == "en-XA");
    CHECK(list.get("locales").size() == 5);
}
