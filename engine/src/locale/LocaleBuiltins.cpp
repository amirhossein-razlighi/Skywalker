// Wander builtins for localization (docs/LOCALIZATION.md): tr, set_locale, locale, locales.

#include "skywalker/engine/Engine.h"
#include "skywalker/locale/Localization.h"
#include "skywalker/ui/World2D.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

loc::Localization& localization(CallContext& c) {
    Engine* engine = c.service<Engine>();
    if (!engine) c.fail(c.def().name + "(): localization is not available here");
    return engine->world2d().localization();
}

void def(BuiltinRegistry& r, const char* name, std::vector<BuiltinParam> params, TypeSet ret, const char* doc, const char* example,
         BuiltinImpl impl) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = ret;
    d.category = "locale";
    d.doc = doc;
    d.example = example;
    d.fn = impl;
    d.owner = "engine";
    r.add(std::move(d));
}

}  // namespace

void registerLocaleBuiltins(BuiltinRegistry& reg) {
    def(reg, "tr", {{"key", kTString}, {"args", kTMap, true}}, kTString,
        "The localized string for a key in the current locale (locale/*.csv or *.po), falling back pt-BR -> pt -> the "
        "source language. `args` fill {placeholders}, plurals ({n, plural, one {# coin} other {# coins}}) and selects; "
        "numbers are formatted for the locale. A missing key returns the key and is reported by locale_check.",
        "self.ui.text = tr(\"hud.coins\", {n: coins})", [](CallContext& c) -> Value {
            Json args = c.argc() > 1 ? toJson(c.arg(1)) : Json::object();
            return Value::string(localization(c).tr(c.string(0), args));
        });
    def(reg, "set_locale", {{"code", kTString}}, kTNone,
        "Switches the game's language from the next frame (a language menu): \"fr\", \"pt-BR\". Texts written as "
        "\"@key\" and #line dialogue lines follow at once. Undone when play stops; save it with game_var to keep it.",
        "on ui \"French\"\n  set_locale(\"fr\")\nend", [](CallContext& c) -> Value {
            Status s = localization(c).setLocale(c.string(0), loc::Localization::Scope::Game);
            if (!s) c.fail("set_locale(): " + s.error().message + (s.error().hint.empty() ? "" : " - " + s.error().hint));
            return {};
        });
    def(reg, "locale", {}, kTString, "The current locale code (\"en\", \"fr\", \"pt-BR\").",
        "if locale() == \"ja\" then self.text.font = \"fonts/NotoSansJP.otf\" end",
        [](CallContext& c) -> Value { return Value::string(localization(c).locale()); });
    def(reg, "locales", {}, kTList, "Locale codes the game has strings for (with the source language), sorted: for a language menu.",
        "for code in locales() log code end", [](CallContext& c) -> Value {
            std::vector<Value> out;
            for (const auto& l : localization(c).available()) out.push_back(Value::string(l));
            return Value::list(std::move(out));
        });
}

}  // namespace sky
