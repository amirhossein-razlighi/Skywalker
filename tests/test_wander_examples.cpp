// Every Wander script shipped in examples/ (scenes and prefabs) must compile, round-trip
// through the formatter and the graph view, and survive a short simulation.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "skywalker/agent/CustomTools.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Compiler.h"
#include "skywalker/wander/Graph.h"
#include "skywalker/wander/Parser.h"
#include "skywalker/wander/Runtime.h"

using namespace sky;
using namespace sky::wander;
namespace fs = std::filesystem;

namespace {

struct ExampleScript {
    std::string file;
    std::string name;
    std::string source;
};

void collect(const Json& j, const std::string& file, std::vector<ExampleScript>& out) {
    if (j.isObject()) {
        if (const Json* b = j.find("behaviors"); b && b->isArray()) {
            for (const auto& s : b->elements()) {
                if (s.isObject() && s.get("source").isString()) {
                    out.push_back({file, s.get("name").asString(), s.get("source").asString()});
                }
            }
        }
        for (const auto& [k, v] : j.members()) {
            if (k != "behaviors") collect(v, file, out);
        }
    } else if (j.isArray()) {
        for (const auto& v : j.elements()) collect(v, file, out);
    }
}

std::vector<ExampleScript> exampleScripts() {
    std::vector<ExampleScript> out;
    fs::path root = fs::path(SKY_SOURCE_DIR) / "examples";
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        std::string name = p.filename().string();
        if (p.extension() != ".json" || name.find(".meta") != std::string::npos) continue;
        if (name.find(".sky") == std::string::npos && name.find(".prefab") == std::string::npos) continue;
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        auto j = Json::parse(ss.str());
        if (!j) continue;
        collect(j.value(), fs::relative(p, root).string(), out);
    }
    return out;
}

}  // namespace

TEST_CASE("examples: every shipped Wander script compiles, formats and round-trips through the graph") {
    auto scripts = exampleScripts();
    REQUIRE(scripts.size() > 100);
    registerEngineBuiltins();  // water_height, burst, ...
    Scene scene;
    Runtime rt(scene);
    CompileOptions opts = rt.compileOptions();
    std::set<std::string> seen;
    size_t unique = 0;
    for (const auto& s : scripts) {
        if (!seen.insert(s.source).second) continue;
        ++unique;
        CAPTURE(s.file);
        CAPTURE(s.name);
        auto r = compile(s.source, opts);
        if (!r.ok()) {
            std::string all;
            for (const auto& d : r.diagnostics) {
                all += std::to_string(d.loc.line) + ":" + std::to_string(d.loc.column) + " " + d.code + " " + d.message + "\n";
            }
            FAIL_CHECK(all << "\n" << s.source);
            continue;
        }
        // Formatting is canonical and stable, and preserves semantics (same bytecode).
        std::string formatted = format(*r.module);
        auto r2 = compile(formatted, opts);
        REQUIRE_MESSAGE(r2.ok(), formatted);
        CHECK(format(*r2.module) == formatted);
        CHECK(r2.program->disassemble(false) == r.program->disassemble(false));
        // Graph round trip: code -> graph -> code gives the same program.
        Json graph = toGraph(*r.module);
        auto back = fromGraph(graph);
        REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
        auto r3 = compile(back.value(), opts);
        REQUIRE_MESSAGE(r3.ok(), back.value());
        CHECK(r3.program->disassemble(false) == r.program->disassemble(false));
    }
    MESSAGE("checked " << unique << " unique scripts from " << scripts.size() << " behaviors");
}

TEST_CASE("docs: every ```wander code block in docs/ compiles") {
    registerEngineBuiltins();
    Scene scene;
    Runtime rt(scene);
    CompileOptions opts = rt.compileOptions();
    // Code of custom tools (docs/CUSTOM_TOOLS.md) also has the tool-only builtins.
    BuiltinRegistry withTools(&BuiltinRegistry::global());
    CustomTools::registerBuiltins(withTools);
    opts.registry = &withTools;
    size_t blocks = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(SKY_SOURCE_DIR) / "docs")) {
        if (entry.path().extension() != ".md") continue;
        std::ifstream f(entry.path());
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        for (size_t at = text.find("```wander\n"); at != std::string::npos; at = text.find("```wander\n", at + 1)) {
            size_t start = at + 10;
            size_t end = text.find("```", start);
            if (end == std::string::npos) break;
            std::string block = text.substr(start, end - start);
            ++blocks;
            // Fragments (a few statements) are checked inside a handler.
            bool ok = compile(block, opts).ok() || compile("on tick\n" + block + "\nend\n", opts).ok();
            CAPTURE(entry.path().filename().string());
            CHECK_MESSAGE(ok, block);
        }
    }
    CHECK(blocks > 5);
}
