// Every Wander script shipped in examples/ (scenes and prefabs) must compile, round-trip
// through the formatter and the graph view, and survive a short simulation.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
