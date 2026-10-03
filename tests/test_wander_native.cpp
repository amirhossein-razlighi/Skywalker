// Native code for Wander: AOT parity with the VM, native C++ modules, and the Wander tools.
// The compiler-dependent tests are skipped when no C++ compiler is installed.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "skywalker/engine/Engine.h"
#include "skywalker/native/NativeModules.h"
#include "skywalker/wander/Aot.h"
#include "skywalker/wander/Runtime.h"

using namespace sky;
using namespace sky::wander;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("sky_" + tag + "_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

const char* kParityScript = R"(
fn fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

behavior Parity
  var acc = 0
  var steps: list = []
  var hits = 0
  var label = ""
  param speed = 2 in 0..10
  state Run
    on tick
      let x = 0.25
      for i in 0..40
        acc += sin(x) * cos(i) + sqrt(abs(x - i)) - floor(x * 3) % 7
        x = x * 1.01 + 0.003
        if acc > 50 and i % 3 == 0 then
          acc = acc / 2
        elif acc < -50 or i == 39 then
          acc += 1
        end
      end
      let v = (0, 0, 0)
      repeat 20 times
        v = v + normalize(self.position - (1, 2, 3)) * 0.01
      end
      move self by v + (speed * dt, 0, 0)
      rotate self by (0, 30 * dt, 0)
      steps.push(round(acc, 4))
      if steps.length > 5 then steps.remove_at(0) end
      label = "acc {round(acc, 2)} fib {fib(10)}"
      if state_time > 0.5 then go to Rest end
    end
  end
  state Rest
    on enter
      hits += 1
      wait frames 3
      go to Run
    end
  end
  on event "boom" with b
    hits += b.n
    let total = 0
    for k, val in {a: 1, b: 2, c: 3}
      total += val
    end
    acc -= total
  end
end
)";

std::string simulate(bool native, const std::string& cacheDir, std::string* error = nullptr) {
    Scene s;
    s.seed = 3;
    std::vector<EntityId> ids;
    for (int i = 0; i < 4; ++i) {
        EntityId e = s.create("E" + std::to_string(i));
        (void)s.patchComponent(e, "transform", Json::object({{"position", Json::array({i, 0, 0})}}));
        (void)s.setBehaviors(e, Json::array({Json::object({{"name", "P"}, {"source", kParityScript}})}));
        ids.push_back(e);
    }
    Runtime rt(s);
    rt.compileScripts();
    if (native) {
        auto prog = s.get<Behavior>(ids[0])->scripts[0].program;
        REQUIRE(prog);
        AotOptions o;
        o.cacheDir = cacheDir;
        auto r = compileNative(*prog, o);
        if (!r) {
            if (error) *error = r.error().message;
            return {};
        }
        CHECK(r->inlined > 0);
        rt.attachNative(prog->hash, r->native);
        CHECK(rt.nativeProgramCount() == 1);
    }
    std::string trace;
    for (int t = 0; t < 120; ++t) {
        if (t == 30) rt.emitJson("boom", ids[1], Json::object({{"n", 4}}));
        rt.tick(1.f / 60.f, {});
        for (const auto& m : rt.drainMessages()) trace += m.toJson().dump() + "\n";
    }
    for (EntityId e : ids) {
        trace += s.record(e)->vars.dump() + reflect::vec3ToJson(s.get<Transform>(e)->position).dump() +
                 reflect::vec3ToJson(s.get<Transform>(e)->rotation).dump() + rt.currentState(e) + "\n";
    }
    return trace;
}

}  // namespace

TEST_CASE("aot: generated C++ is self-contained and covers every proto") {
    auto r = compile(kParityScript, std::vector<std::string>{"transform", "mesh"});
    REQUIRE(r.ok());
    size_t inlined = 0, delegated = 0;
    std::string code = generateCpp(*r.program, &inlined, &delegated);
    CHECK(code.find("sky_aot_init") != std::string::npos);
    CHECK(code.find("#include \"skywalker") == std::string::npos);  // ABI header embedded
    CHECK(inlined > 20);
    CHECK(delegated > 0);
}

TEST_CASE("aot: native code produces exactly the VM's trace") {
    if (findCxxCompiler().empty()) {
        MESSAGE("no C++ compiler: skipping AOT parity");
        return;
    }
    TempDir dir("aot");
    std::string vm = simulate(false, dir.path.string());
    std::string err;
    std::string native = simulate(true, dir.path.string(), &err);
    REQUIRE_MESSAGE(err.empty(), err);
    CHECK(vm.size() > 200);
    CHECK(native == vm);
    // Second build is a cache hit.
    auto prog = compile(kParityScript, std::vector<std::string>{"transform", "mesh"}).program;
    AotOptions o;
    o.cacheDir = dir.path.string();
    // (different program hash only if compile options differ; the call must still succeed)
    auto again = compileNative(*prog, o);
    REQUIRE(again);
}

TEST_CASE("aot: budget errors and runtime errors match the VM") {
    if (findCxxCompiler().empty()) return;
    TempDir dir("aot_err");
    auto run = [&](bool native) {
        Scene s;
        EntityId e = s.create("Inf");
        (void)s.setBehaviors(e, Json::array({Json::object(
                                    {{"name", "I"},
                                     {"source", "var n = 0\non tick\n  n += 1\n  if n == 2 then log [1][4] end\n  while n > 2\n    n += 1\n  end\nend"}})}));
        Runtime rt(s);
        rt.compileScripts();
        if (native) {
            auto prog = s.get<Behavior>(e)->scripts[0].program;
            AotOptions o;
            o.cacheDir = dir.path.string();
            auto r = compileNative(*prog, o);
            REQUIRE(r);
            rt.attachNative(prog->hash, r->native);
        }
        std::string trace;
        for (int i = 0; i < 4; ++i) {
            rt.tick(1.f / 60.f, {});
            for (const auto& m : rt.drainMessages()) trace += m.toJson().dump() + "\n";
        }
        return trace + s.record(e)->vars.dump();
    };
    std::string vm = run(false);
    CHECK(vm.find("out of range") != std::string::npos);
    CHECK(vm.find("budget") != std::string::npos);
    CHECK(run(true) == vm);
}

// ---------------------------------------------------------------------------
// Native modules
// ---------------------------------------------------------------------------

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

const char* kModule = R"CPP(
#include <cmath>
#include <string>
#include "skywalker/native/sdk.h"

static void triple(SkyCall* call, const SkyValue* args, int, SkyValue* result, void*) {
    *result = sky_number(sky_arg_number(call, args, 0) * 3);
}
static void greet(SkyCall* call, const SkyValue* args, int, SkyValue* result, void*) {
    const char* name = sky_sdk_api->string_chars(&args[0]);
    if (!name) { sky_sdk_api->call_fail(call, "greet needs a string"); return; }
    std::string s = std::string("hello ") + name;
    *result = sky_sdk_api->string_new(s.c_str());
}
static void lift(SkyWorld* w, double dt, void*) {
    SkyEntity ids[16];
    size_t n = sky_sdk_api->find_tagged(w, "lift", ids, 16);
    for (size_t i = 0; i < n && i < 16; ++i) {
        float p[3];
        sky_sdk_api->get_position(w, ids[i], p);
        p[1] += static_cast<float>(dt);
        sky_sdk_api->set_position(w, ids[i], p);
        SkyValue v = sky_number(p[1]);
        sky_sdk_api->set_var(w, ids[i], "lifted", &v);
    }
}

SKY_MODULE_EXPORT int sky_module_init(const SkyApi* api, SkyModule* module) {
    SKY_SDK_INIT(api);
    SkyBuiltinDesc a = {"triple", "Three times x.", "test", "triple(2)", 1, 1, "x: number", "number", triple, nullptr};
    SkyBuiltinDesc b = {"greet", "Greets.", "test", "greet(\"sky\")", 1, 1, "name: string", "string", greet, nullptr};
    if (api->register_builtin(module, &a) || api->register_builtin(module, &b)) return -1;
    return api->register_system(module, "lift", lift, nullptr);
}
)CPP";

}  // namespace

TEST_CASE("native modules: build, load, builtins, systems, diagnostics, hot reload") {
    if (findCxxCompiler().empty()) {
        MESSAGE("no C++ compiler: skipping native modules");
        return;
    }
    TempDir dir("native");
    // A template is a valid starting point.
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = dir.path.string();
    Engine engine(cfg);
    (void)engine.newScene("T", false);
    Json t = engine.callTool("native_template", Json::object({{"name", "starter"}}), "test").structured;
    REQUIRE(fs::exists(dir.path / "native" / "starter.cpp"));
    fs::remove(dir.path / "native" / "starter.cpp");

    writeFile(dir.path / "native" / "game.cpp", kModule);
    ToolResult b = engine.callTool("native_build", Json::object(), "test");
    INFO(b.structured.dump(2));
    REQUIRE_FALSE(b.isError);
    Json list = engine.native().list();
    REQUIRE(list.get("loaded").isObject());
    CHECK(list["loaded"]["builtins"].size() == 2);
    CHECK(list["loaded"]["systems"].size() == 1);
    // The builtin is checked and documented like any other.
    Json ref = engine.callTool("wander_reference", Json::object({{"topic", "triple"}}), "test").structured;
    CHECK(ref["entries"].size() == 1);
    Json bad = engine.callTool("wander_check", Json::object({{"source", "on start\n  log triple(\"x\")\nend"}}), "test").structured;
    CHECK_FALSE(bad["ok"].asBool());

    Scene& s = engine.scene();
    EntityId e = s.create("Box");
    (void)s.patchComponent(e, "mesh", Json::object());
    (void)s.setTags(e, {"lift"});
    (void)s.setBehaviors(e, Json::array({Json::object(
                                {{"name", "N"}, {"source", "var t = 0\nvar g = \"\"\non start\n  t = triple(7)\n  g = greet(self.name)\nend"}})}));
    engine.step(30);
    CHECK(s.record(e)->vars["t"].asNumber() == 21);
    CHECK(s.record(e)->vars["g"].asString() == "hello Box");
    CHECK(s.get<Transform>(e)->position.y == doctest::Approx(0.5).epsilon(0.01));
    CHECK(s.record(e)->vars["lifted"].asNumber() == doctest::Approx(0.5).epsilon(0.01));
    engine.stop();

    // A compile error comes back as structured diagnostics; the old module stays loaded.
    writeFile(dir.path / "native" / "game.cpp", std::string(kModule) + "\nint broken( {\n");
    ToolResult err = engine.callTool("native_build", Json::object(), "test");
    CHECK(err.isError);
    REQUIRE(err.structured["diagnostics"].size() > 0);
    CHECK(err.structured["diagnostics"][size_t{0}]["file"].asString() == "native/game.cpp");
    CHECK(err.structured["diagnostics"][size_t{0}]["line"].asInt() > 30);
    CHECK(engine.native().list().get("loaded").isObject());

    // Hot reload at play start: the new code is built and loaded.
    std::string v2 = kModule;
    v2.replace(v2.find("* 3);"), 5, "* 4);");
    writeFile(dir.path / "native" / "game.cpp", v2);
    engine.play();
    engine.step(1);
    CHECK(s.record(e)->vars["t"].asNumber() == 28);
    engine.stop();
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

TEST_CASE("wander tools: behavior_set with spec, behavior_spec, wander_test, graph round trip, inspect") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);
    (void)engine.newScene("T", false);
    EntityId e = engine.scene().create("Guard");
    (void)engine.scene().patchComponent(e, "mesh", Json::object());
    const char* src = R"(
behavior Guard
  intent "Loses one hp per hit and reports when it falls."
  param speed = 3 in 0..10 "walk speed"
  var hp = 3
  on event "hit"
    hp -= 1
    if hp <= 0 then go to Down end
  end
  state Up
    on tick
      rotate self by (0, speed * dt, 0)
    end
  end
  state Down
    on enter
      log "down"
    end
  end
  test "loses hp when hit"
    emit "hit" to self
    wait frames 2
    expect hp == 2
  end
  test "falls after three hits"
    repeat 3 times
      emit "hit" to self
      wait frames 1
    end
    wait frames 1
    expect state == "Down"
  end
end)";
    Json spec = Json::parse(R"({"summary":"guard","rules":[{"text":"loses 1 hp per hit","tests":["loses hp when hit"]},
        {"text":"falls at 0 hp","tests":["falls after three hits"]},{"text":"walks at speed","tests":[]}]})").value();
    ToolResult set = engine.callTool("behavior_set", Json::object({{"entity", "Guard"}, {"name", "Guard"}, {"source", src}, {"spec", spec}}), "t");
    INFO(set.structured.dump(2));
    REQUIRE_FALSE(set.isError);
    Json sv = engine.callTool("behavior_spec", Json::object({{"entity", "Guard"}, {"name", "Guard"}}), "t").structured;
    CHECK(sv["coverage"]["rules"].asInt() == 3);
    CHECK(sv["coverage"]["covered"].asInt() == 2);
    CHECK(sv["derived"]["params"][size_t{0}]["max"].asNumber() == 10);
    CHECK(sv["derived"]["states"].size() == 2);

    Json tr = engine.callTool("wander_test", Json::object({{"entity", "Guard"}, {"name", "Guard"}}), "t").structured;
    INFO(tr.dump(2));
    CHECK(tr["passed"].asInt() == 2);
    CHECK(tr["failed"].asInt() == 0);

    // Graph -> code saves the same program and keeps edited node positions.
    ToolResult g = engine.callTool("behavior_graph", Json::object({{"entity", "Guard"}, {"name", "Guard"}}), "t");
    REQUIRE_FALSE(g.isError);
    Json graph = g.structured;
    Json& firstNode = graph["behaviors"].elements()[0]["handlers"].elements()[0]["nodes"].elements()[0];
    firstNode["x"] = 1234.0;
    ToolResult back = engine.callTool("behavior_from_graph", Json::object({{"graph", graph}, {"entity", "Guard"}, {"name", "Guard"}}), "t");
    INFO(back.structured.dump(2));
    REQUIRE_FALSE(back.isError);
    Json again = engine.callTool("behavior_graph", Json::object({{"entity", "Guard"}, {"name", "Guard"}}), "t").structured;
    CHECK(again["behaviors"][size_t{0}]["handlers"][size_t{0}]["nodes"][size_t{0}]["x"].asNumber() == 1234.0);
    CHECK(engine.callTool("wander_test", Json::object({{"entity", "Guard"}, {"name", "Guard"}}), "t").structured["passed"].asInt() == 2);

    engine.runtime().emit("hit", e);
    engine.step(3);
    Json ins = engine.callTool("wander_inspect", Json::object({{"entity", "Guard"}}), "t").structured;
    CHECK(ins["scripts"][size_t{0}]["behaviors"][size_t{0}]["state"].asString() == "Up");
    CHECK(ins["vars"]["hp"].asNumber() == 2);
    engine.stop();

    // Rejected code changes nothing.
    std::string saved = engine.scene().entityToJson(e)["behaviors"][size_t{0}]["source"].asString();
    ToolResult rej = engine.callTool("behavior_set", Json::object({{"entity", "Guard"}, {"name", "Guard"}, {"source", "on tick\n  move\nend"}}), "t");
    CHECK(rej.isError);
    CHECK(engine.scene().entityToJson(e)["behaviors"][size_t{0}]["source"].asString() == saved);
}
