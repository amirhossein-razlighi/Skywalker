// Wander 2 language and runtime: functions, collections, loops, budget, coroutines,
// state machines, events with payloads, modules, types, diagnostics, tests, determinism.

#include <doctest/doctest.h>

#include <map>

#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Compiler.h"
#include "skywalker/wander/Graph.h"
#include "skywalker/wander/Parser.h"
#include "skywalker/wander/Runtime.h"
#include "skywalker/wander/Testing.h"

using namespace sky;
using namespace sky::wander;

namespace {

EntityId withScript(Scene& s, const std::string& name, const std::string& source) {
    EntityId e = s.create(name);
    (void)s.patchComponent(e, "mesh", Json::object());
    (void)s.setBehaviors(e, Json::array({Json::object({{"name", name + "Script"}, {"source", source}})}));
    return e;
}

void run(Runtime& rt, int ticks, const InputState& in = {}) {
    for (int i = 0; i < ticks; ++i) rt.tick(1.f / 60.f, in);
}

const Json& var(Scene& s, EntityId e, const char* name) { return s.record(e)->vars[name]; }

bool hasCode(const CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

std::string errorsOf(Runtime& rt) {
    std::string out;
    for (const auto& m : rt.drainMessages()) {
        if (m.kind != RuntimeMessage::Kind::Log) out += std::to_string(m.line) + ": " + m.text + "\n";
    }
    return out;
}

CompileResult check(const std::string& src) {
    Scene s;
    Runtime rt(s);
    return compile(src, rt.compileOptions());
}

}  // namespace

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

TEST_CASE("wander2: functions, recursion, return types") {
    Scene s;
    EntityId e = withScript(s, "F", R"(
fn fib(n: number) -> number
  if n < 2 then
    return n
  end
  return fib(n - 1) + fib(n - 2)
end

fn greet(name)
  return "hi {name}"
end

behavior F
  var a = 0
  var b = ""
  fn twice(x)
    return x * 2
  end
  on start
    a = twice(fib(10))
    b = greet("sky")
  end
end)");
    Runtime rt(s);
    run(rt, 1);
    CHECK(errorsOf(rt).empty());
    CHECK(var(s, e, "a").asNumber() == 110);
    CHECK(var(s, e, "b").asString() == "hi sky");
}

TEST_CASE("wander2: recursion depth limit gives a precise error") {
    Scene s;
    withScript(s, "R", "fn down(n)\n  return down(n + 1)\nend\non start\n  down(0)\nend");
    Runtime rt(s);
    run(rt, 1);
    auto msgs = rt.drainMessages();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].kind == RuntimeMessage::Kind::Error);
    CHECK(msgs[0].text.find("recursion too deep") != std::string::npos);
    CHECK(msgs[0].line == 2);
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

TEST_CASE("wander2: lists and maps with methods and value semantics") {
    Scene s;
    EntityId e = withScript(s, "C", R"(
behavior C
  var items: list = []
  var stats = {hp: 3, name: "box"}
  var out = {}
  on start
    items.push(3)
    items.push(1)
    items.push(2)
    let copy = items
    copy.push(99)           -- value semantics: items is unchanged
    items.sort()
    stats.hp -= 1
    stats["armor"] = 5
    let total = 0
    for k, v in stats
      if type_of(v) == "number" then total += v end
    end
    out = {
      first: items[0], last: items[-1], len: items.length, copyLen: copy.length,
      popped: copy.pop(), has2: items.contains(2), has7: 7 in items,
      keys: stats.keys().join(","), total: total, idx: items.index_of(3),
      slice: items.slice(1).length, name: stats.name, missing: stats.nothing
    }
  end
end)");
    Runtime rt(s);
    run(rt, 1);
    CHECK(errorsOf(rt).empty());
    Json out = var(s, e, "out");
    CHECK(out["first"].asNumber() == 1);
    CHECK(out["last"].asNumber() == 3);
    CHECK(out["len"].asNumber() == 3);
    CHECK(out["copyLen"].asNumber() == 4);
    CHECK(out["popped"].asNumber() == 99);
    CHECK(out["has2"].asBool());
    CHECK_FALSE(out["has7"].asBool());
    CHECK(out["keys"].asString() == "hp,name,armor");
    CHECK(out["total"].asNumber() == 7);
    CHECK(out["idx"].asNumber() == 2);
    CHECK(out["slice"].asNumber() == 2);
    CHECK(out["name"].asString() == "box");
    CHECK(out["missing"].isNull());
    // The var list itself was mutated in place and persisted.
    CHECK(var(s, e, "items").size() == 3);
}

TEST_CASE("wander2: out-of-range index is a precise runtime error") {
    Scene s;
    withScript(s, "I", "on start\n  let l = [1, 2]\n  log l[5]\nend");
    Runtime rt(s);
    run(rt, 1);
    std::string err = errorsOf(rt);
    INFO(err);
    CHECK(err.find("index 5 is out of range (the list has 2 items)") != std::string::npos);
    CHECK(err.rfind("3:", 0) == 0);
}

// ---------------------------------------------------------------------------
// Loops and the budget
// ---------------------------------------------------------------------------

TEST_CASE("wander2: for, while, repeat, break, continue") {
    Scene s;
    EntityId e = withScript(s, "L", R"(
behavior L
  var r = []
  on start
    let a = 0
    for i in 0..5
      a += i              -- 0+1+2+3+4
    end
    let b = 0
    for i in 1..=3
      b += i
    end
    let c = 0
    for i in 10..0 step -2
      c += 1              -- 10 8 6 4 2
    end
    let d = 0
    while true
      d += 1
      if d >= 7 then break end
    end
    let evens = 0
    for i in 0..10
      if i % 2 == 1 then continue end
      evens += 1
    end
    let rep = 0
    repeat 4 times
      rep += 1
    end
    let chars = ""
    for ch in "abc"
      chars = ch + chars
    end
    let sum = 0
    for i, x in [10, 20]
      sum += i + x
    end
    r = [a, b, c, d, evens, rep, chars, sum]
  end
end)");
    Runtime rt(s);
    run(rt, 1);
    CHECK(errorsOf(rt).empty());
    Json r = var(s, e, "r");
    REQUIRE(r.size() == 8);
    CHECK(r[size_t{0}].asNumber() == 10);
    CHECK(r[size_t{1}].asNumber() == 6);
    CHECK(r[size_t{2}].asNumber() == 5);
    CHECK(r[size_t{3}].asNumber() == 7);
    CHECK(r[size_t{4}].asNumber() == 5);
    CHECK(r[size_t{5}].asNumber() == 4);
    CHECK(r[size_t{6}].asString() == "cba");
    CHECK(r[size_t{7}].asNumber() == 31);
}

TEST_CASE("wander2: infinite loops hit the budget with a precise error and never hang") {
    Scene s;
    EntityId e = withScript(s, "Inf", "var counter = 0\non tick\n  while true\n    counter = counter\n  end\nend");
    Runtime rt(s);
    run(rt, 1);
    auto msgs = rt.drainMessages();
    REQUIRE(!msgs.empty());
    CHECK(msgs[0].kind == RuntimeMessage::Kind::Error);
    CHECK(msgs[0].text.find("execution budget exceeded") != std::string::npos);
    CHECK(msgs[0].line >= 3);
    // Five failures disable the script.
    run(rt, 6);
    CHECK_FALSE(s.get<Behavior>(e)->scripts[0].enabled);

    Scene s2;
    withScript(s2, "Bomb", "fn f(n)\n  return f(n - 1) + f(n - 1) + 1\nend\non start\n  f(60)\nend");
    Runtime rt2(s2);
    run(rt2, 1);
    std::string err = errorsOf(rt2);
    CHECK((err.find("budget") != std::string::npos || err.find("recursion") != std::string::npos));
}

// ---------------------------------------------------------------------------
// Coroutines
// ---------------------------------------------------------------------------

TEST_CASE("wander2: wait seconds / frames / until resume deterministically") {
    Scene s;
    EntityId e = withScript(s, "W", R"(
behavior W
  var trace: list = []
  var flag = false
  on start
    trace.push("a{frame}")
    wait 0.5
    trace.push("b{frame}")
    wait frames 3
    trace.push("c{frame}")
    wait until flag
    trace.push("d{frame}")
  end
  on event "go"
    flag = true
  end
end)");
    Runtime rt(s);
    run(rt, 40);
    rt.emit("go", e);
    run(rt, 3);
    CHECK(errorsOf(rt).empty());
    Json l = var(s, e, "trace");
    REQUIRE(l.size() == 4);
    CHECK(l[size_t{0}].asString() == "a0");
    CHECK(l[size_t{1}].asString() == "b30");
    CHECK(l[size_t{2}].asString() == "c33");
    CHECK(l[size_t{3}].asString() == "d41");
}

TEST_CASE("wander2: a waiting tick handler does not restart; events run concurrently") {
    Scene s;
    EntityId e = withScript(s, "T", R"(
behavior T
  var ticks = 0
  var hits = 0
  var done = 0
  on tick
    ticks += 1
    wait 0.25
  end
  on event "hit"
    hits += 1
    wait frames 2
    done += 1
  end
end)");
    Runtime rt(s);
    rt.emit("hit", e);
    rt.emit("hit", e);
    run(rt, 60);
    CHECK(errorsOf(rt).empty());
    // 60 ticks with a 15-tick wait: the handler restarts on the tick its wait finishes.
    CHECK(var(s, e, "ticks").asNumber() == 4);
    CHECK(var(s, e, "hits").asNumber() == 2);
    CHECK(var(s, e, "done").asNumber() == 2);
}

TEST_CASE("wander2: reset (play/stop) clears coroutines, states and timers") {
    Scene s;
    EntityId e = withScript(s, "R", "var n = 0\non start\n  wait 1\n  n = 1\nend");
    Runtime rt(s);
    run(rt, 30);
    rt.reset();
    (void)s.patchVars(e, Json::object({{"n", 0}}));
    run(rt, 30);  // restarted: the first wait has not elapsed yet
    CHECK(var(s, e, "n").asNumber() == 0);
    run(rt, 31);
    CHECK(var(s, e, "n").asNumber() == 1);
}

// ---------------------------------------------------------------------------
// State machines
// ---------------------------------------------------------------------------

TEST_CASE("wander2: state machines: enter/exit order, state-local handlers, state and state_time") {
    Scene s;
    EntityId e = withScript(s, "S", R"(
behavior Door
  var trace: list = []
  var seen = ""
  on tick
    seen = state
  end
  state Closed
    on enter
      trace.push("enter Closed")
    end
    on event "open"
      go to Opening
    end
    on exit
      trace.push("exit Closed")
    end
  end
  state Opening
    on enter
      trace.push("enter Opening")
      wait 10            -- cancelled by the state change
      trace.push("never")
    end
    on tick
      if state_time >= 0.5 then go to Open end
    end
  end
  state Open
    on enter
      trace.push("enter Open")
    end
  end
end)");
    EntityId watcher = withScript(s, "Watcher", "var other_state = \"\"\non tick\n  other_state = find(\"S\").state\nend");
    Runtime rt(s);
    run(rt, 2);
    CHECK(var(s, e, "seen").asString() == "Closed");
    rt.emit("open", e);
    run(rt, 2);
    CHECK(var(s, e, "seen").asString() == "Opening");
    run(rt, 40);
    CHECK(errorsOf(rt).empty());
    Json t = var(s, e, "trace");
    REQUIRE(t.size() == 4);
    CHECK(t[size_t{0}].asString() == "enter Closed");
    CHECK(t[size_t{1}].asString() == "exit Closed");
    CHECK(t[size_t{2}].asString() == "enter Opening");
    CHECK(t[size_t{3}].asString() == "enter Open");
    CHECK(rt.currentState(e) == "Open");
    CHECK(var(s, watcher, "other_state").asString() == "Open");
    Json info = rt.inspect(e);
    CHECK(info[size_t{0}]["behaviors"][size_t{0}]["state"].asString() == "Open");
}

TEST_CASE("wander2: endless go to loops are stopped") {
    Scene s;
    withScript(s, "Loop", "state A\n  on enter\n    go to B\n  end\nend\nstate B\n  on enter\n    go to A\n  end\nend");
    Runtime rt(s);
    run(rt, 1);
    CHECK(errorsOf(rt).find("too many state changes") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Events, other, payloads
// ---------------------------------------------------------------------------

TEST_CASE("wander2: events carry payloads and the sender as `other`") {
    Scene s;
    EntityId target = withScript(s, "Target", R"(
behavior Target
  var hp = 10
  var from = ""
  on event "damage" with hit
    hp -= hit.amount
    from = other.name
  end
  on event "heal"
    hp += data.get("amount", 1)
  end
end)");
    withScript(s, "Gun", "on start\n  emit \"damage\" with {amount: 3} to find(\"Target\")\n  emit \"heal\" with {amount: 1}\nend");
    Runtime rt(s);
    run(rt, 2);
    CHECK(errorsOf(rt).empty());
    CHECK(var(s, target, "hp").asNumber() == 8);
    CHECK(var(s, target, "from").asString() == "Gun");
    // From outside (tools): JSON payloads.
    rt.emitJson("damage", target, Json::object({{"amount", 5}}));
    run(rt, 1);
    CHECK(var(s, target, "hp").asNumber() == 3);
}

// ---------------------------------------------------------------------------
// Strings, consts, types, diagnostics
// ---------------------------------------------------------------------------

TEST_CASE("wander2: interpolation and consts") {
    Scene s;
    EntityId e = withScript(s, "Txt", R"(
const MAX = 3 * 2
behavior Txt
  const PREFIX = "hp"
  var msg = ""
  on start
    let v = (1, 2.5, 0)
    msg = "{PREFIX}: {MAX - 1}/{MAX} at {v} by {self} \{literal}"
  end
end)");
    Runtime rt(s);
    run(rt, 1);
    CHECK(errorsOf(rt).empty());
    CHECK(var(s, e, "msg").asString() == "hp: 5/6 at (1, 2.5, 0) by Txt {literal}");
}

TEST_CASE("wander2: static type checks report certain mismatches only") {
    CHECK(hasCode(check("on tick\n  let x = \"a\" - 1\nend"), "type_mismatch"));
    CHECK(hasCode(check("on tick\n  let d = distance(\"a\", self)\nend"), "type_mismatch"));
    CHECK(hasCode(check("var hp: number = 3\non tick\n  hp = \"full\"\nend"), "type_mismatch"));
    CHECK(hasCode(check("fn f(x: number) -> number\n  return \"no\"\nend"), "type_mismatch"));
    CHECK(hasCode(check("on tick\n  let v = (1, 2, 3)\n  log v.w\nend"), "unknown_property"));
    CHECK(hasCode(check("on tick\n  let l = [1]\n  l.shove(2)\nend"), "unknown_method"));
    CHECK(hasCode(check("on tick\n  self.light.intensty = 2\nend"), "unknown_field"));
    // Unknown types (any) never error: gradual typing.
    CHECK(check("var target = none\non tick\n  if target then move self toward target at 2 end\nend").ok());
    CHECK(check("on event \"x\"\n  log data.amount + 1\nend").ok());
}

TEST_CASE("wander2: diagnostics have stable codes, positions and did-you-mean hints") {
    auto r = check("var speed = 2\non tick\n  move self by (sped, 0, 0)\nend");
    REQUIRE_FALSE(r.ok());
    const Diagnostic* d = nullptr;
    for (const auto& x : r.diagnostics) {
        if (x.code == "unknown_name") d = &x;
    }
    REQUIRE(d);
    CHECK(d->loc.line == 3);
    CHECK(d->loc.column == 17);
    CHECK(d->hint.find("speed") != std::string::npos);

    CHECK(hasCode(check("on tick\n  log distnace(self, self)\nend"), "unknown_function"));
    CHECK(hasCode(check("on tick\n  break\nend"), "outside_loop"));
    CHECK(hasCode(check("fn f()\n  wait 1\nend"), "wait_outside_handler"));
    CHECK(hasCode(check("state A\nend\non tick\n  go to Aa\nend"), "unknown_state"));
    CHECK(hasCode(check("on tick\n  expect 1 == 1\nend"), "test_only"));
    CHECK(hasCode(check("on tick\n  let x = 1\n  x = 2\n  const y = 3\n  y = 4\nend"), "const_assign"));
    CHECK(hasCode(check("on bump\nend"), "unknown_trigger"));
    CHECK(hasCode(check("var speed = 1\non tick\n  let speed = 2\nend"), "shadows_var"));
    CHECK(hasCode(check("on tick\n  stop\n  log 1\nend"), "unreachable_code"));
    CHECK(hasCode(check("on tick\n  log sin(1, 2)\nend"), "wrong_arity"));
    CHECK(hasCode(check("on tick\n  let state = 1\nend"), "reserved_word"));
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

TEST_CASE("wander2: modules: use, qualified calls, consts, errors") {
    std::map<std::string, std::string> files{
        {"scripts/combat.wander", "use \"scripts/util\"\nconst MAX_HP = 10\nfn damage(hp, n)\n  return util.clamp0(hp - n)\nend"},
        {"scripts/util.wander", "fn clamp0(x)\n  return max(0, x)\nend"},
        {"scripts/a.wander", "use \"scripts/b\"\nfn fa()\n  return 1\nend"},
        {"scripts/b.wander", "use \"scripts/a\"\nfn fb()\n  return 2\nend"}};
    CompileOptions o;
    o.loadModule = [&](const std::string& path) -> Result<std::string> {
        auto it = files.find(path);
        if (it == files.end()) return Error::make("not_found", "no file " + path);
        return it->second;
    };
    auto r = compile("use \"scripts/combat\"\nvar hp = 0\non start\n  hp = combat.damage(combat.MAX_HP, 25)\nend", o);
    REQUIRE(r.ok());
    CHECK(r.program->modules.size() == 2);

    CHECK(hasCode(compile("use \"scripts/missing\"\non start\nend", o), "unknown_module"));
    CHECK(hasCode(compile("use \"scripts/a\"\non start\nend", o), "module_cycle"));
    CHECK(hasCode(compile("use \"scripts/combat\"\non start\n  combat.dmg(1, 2)\nend", o), "unknown_function"));
    CHECK(hasCode(compile("use \"../secret\"\non start\nend", o), "invalid_module_path"));
    // Errors inside a module name the module file.
    files["scripts/bad.wander"] = "fn f()\n  return nope\nend";
    auto bad = compile("use \"scripts/bad\"\non start\nend", o);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.diagnostics.front().file == "scripts/bad.wander");
}

// ---------------------------------------------------------------------------
// Determinism and vars
// ---------------------------------------------------------------------------

TEST_CASE("wander2: runs replay exactly (seeded randomness, coroutines, states)") {
    auto simulate = [] {
        Scene s;
        s.seed = 7;
        EntityId e = withScript(s, "D", R"(
behavior D
  var path: list = []
  state Wander
    on tick
      move self by (random(-1, 1) * dt, 0, random_int(-1, 1) * dt)
      if chance(0.05) then go to Pause end
    end
  end
  state Pause
    on enter
      path.push(round(self.position.x, 3))
      wait random(0.1, 0.5)
      go to Wander
    end
  end
end)");
        Runtime rt(s);
        run(rt, 600);
        return s.record(e)->vars.dump() + reflect::vec3ToJson(s.get<Transform>(e)->position).dump();
    };
    std::string a = simulate();
    CHECK(a == simulate());
    CHECK(a.size() > 20);
}

TEST_CASE("wander2: entity vars are mirrored every tick and outside edits are picked up") {
    Scene s;
    EntityId e = withScript(s, "V", "var n = 0\non tick\n  n += 1\nend");
    EntityId reader = withScript(s, "Reader", "var seen = 0\non tick\n  seen = find(\"V\").n\nend");
    Runtime rt(s);
    run(rt, 3);
    CHECK(var(s, e, "n").asNumber() == 3);
    // An agent edits the var between ticks.
    (void)s.patchVars(e, Json::object({{"n", 100}}));
    run(rt, 1);
    CHECK(var(s, e, "n").asNumber() == 101);
    CHECK(var(s, reader, "seen").asNumber() >= 100);
}

// ---------------------------------------------------------------------------
// In-language tests
// ---------------------------------------------------------------------------

TEST_CASE("wander2: test blocks run in a sandbox with expect, wait, emit and input") {
    const char* src = R"(
behavior Player
  var hp = 3
  var jumps = 0
  on event "damage" with hit
    hp -= hit.get("amount", 1)
  end
  on key "space"
    jumps += 1
  end

  test "loses hp when hit"
    emit "damage" with {amount: 1} to self
    wait frames 2
    expect hp == 2
  end

  test "jumps on space"
    press "space"
    wait frames 1
    expect jumps == 1, "one jump per press"
  end

  test "this one fails"
    expect hp == 99
  end

  test "runtime errors are failures"
    log [1][3]
  end
end)";
    TestReport rep = runTests(src, [] {
        TestOptions o;
        o.behaviorName = "Player";
        return o;
    }());
    REQUIRE(rep.compiled);
    REQUIRE(rep.tests.size() == 4);
    CHECK(rep.tests[0].passed);
    CHECK(rep.tests[1].passed);
    CHECK_FALSE(rep.tests[2].passed);
    REQUIRE(rep.tests[2].failures.size() == 1);
    CHECK(rep.tests[2].failures[0].message == "expected hp == 99");
    CHECK(rep.tests[2].failures[0].detail == "left side was 3, right side was 99");
    CHECK_FALSE(rep.tests[3].passed);
    CHECK(rep.passed == 2);
    CHECK(rep.failed == 2);
    CHECK(rep.toJson()["total"].asInt() == 4);

    TestOptions only;
    only.behaviorName = "Player";
    only.filter = "jump";
    CHECK(runTests(src, only).tests.size() == 1);
}

TEST_CASE("wander2: the reference guide example compiles and its test passes") {
    std::string guide = referenceText();
    size_t start = guide.find("behavior Guard");
    size_t end = guide.find("\nend\n", start);
    REQUIRE(start != std::string::npos);
    std::string example = guide.substr(start, end + 5 - start);
    TestOptions o;
    o.behaviorName = "Guard";
    TestReport rep = runTests(example, o);
    INFO(rep.toJson().dump(2));
    REQUIRE(rep.compiled);
    REQUIRE(rep.tests.size() == 1);
    CHECK(rep.tests[0].passed);
}

// ---------------------------------------------------------------------------
// Builtin registry
// ---------------------------------------------------------------------------

namespace {
Value tripleImpl(CallContext& c) { return Value::number(c.number(0) * 3); }
}

TEST_CASE("wander2: builtins registered at runtime are checked, documented and callable") {
    BuiltinRegistry reg(&BuiltinRegistry::global());
    BuiltinDef d;
    d.name = "triple";
    d.params = {{"x", kTNumber}};
    d.returns = kTNumber;
    d.category = "test";
    d.doc = "Three times x.";
    d.fn = tripleImpl;
    reg.add(d);
    reg.addTrigger({"contact", "Two bodies touched", "{impulse: number}", "physics"});

    Scene s;
    EntityId e = withScript(s, "B", "var t = 0\nvar touched = 0\non start\n  t = triple(4)\nend\non contact\n  touched = data.impulse\nend");
    Runtime rt(s, &reg);
    run(rt, 1);
    CHECK(errorsOf(rt).empty());
    CHECK(var(s, e, "t").asNumber() == 12);
    rt.emitJson("contact", e, Json::object({{"impulse", 2.5}}));
    run(rt, 1);
    CHECK(var(s, e, "touched").asNumber() == 2.5);

    CompileOptions o;
    o.registry = &reg;
    CHECK(hasCode(compile("on start\n  triple(\"x\")\nend", o), "type_mismatch"));
    CHECK(referenceText(reg).find("triple(x: number) -> number") != std::string::npos);
    CHECK(builtinReference(reg, "test").size() == 1);
    // Without the registration, the same script does not compile.
    CHECK(hasCode(compile("on start\n  triple(1)\nend", std::vector<std::string>{}), "unknown_function"));
}

TEST_CASE("wander2: set_parent re-parents, keeps the world transform and is undone on stop") {
    // The compiler points read-only `parent` at the builtin, which really exists.
    auto ro = check("on start\n  self.parent = none\nend");
    CHECK(hasCode(ro, "readonly"));
    CHECK(!hasCode(check("on start\n  set_parent(self, none)\n  set_parent(self, find(\"Hand\"), false)\nend"), "unknown_function"));
    CHECK(check("on start\n  set_parent(self, none)\nend").ok());
    CHECK(!check("on start\n  set_parent(self, 3)\nend").ok());  // a number is not an entity

    Scene s;
    EntityId hand = s.create("Hand");
    s.add<Transform>(hand).position = {2, 1, 0};
    s.get<Transform>(hand)->rotation = {0, 90, 0};
    s.get<Transform>(hand)->scale = {2, 2, 2};
    EntityId item = withScript(s, "Item", R"(
behavior Pickup
  on start
    set_parent(self, find("Hand"))
  end
  on event "drop"
    set_parent(self, none)
  end
  on event "snap"
    set_parent(self, find("Hand"), false)
  end
  on event "loop"
    set_parent(find("Hand"), self)
  end
end)");
    s.add<Transform>(item).position = {5, 0, -1};
    const Vec3 before = s.worldMatrix(item).translation();
    Runtime rt(s);
    run(rt, 1);
    CHECK(errorsOf(rt).empty());
    REQUIRE(s.record(item)->parent == hand);
    Vec3 w = s.worldMatrix(item).translation();
    CHECK(w.x == doctest::Approx(before.x).epsilon(1e-4));
    CHECK(w.y == doctest::Approx(before.y).epsilon(1e-4));
    CHECK(w.z == doctest::Approx(before.z).epsilon(1e-4));
    CHECK(s.get<Transform>(item)->scale.x == doctest::Approx(0.5f).epsilon(1e-4));  // undoes the parent's scale

    rt.emit("drop");
    run(rt, 1);
    CHECK(s.record(item)->parent == kNoEntity);
    w = s.worldMatrix(item).translation();
    CHECK(w.x == doctest::Approx(before.x).epsilon(1e-4));
    CHECK(w.z == doctest::Approx(before.z).epsilon(1e-4));

    rt.emit("snap");
    run(rt, 1);
    REQUIRE(s.record(item)->parent == hand);
    const Vec3 local = s.get<Transform>(item)->position;  // keep_world = false: the local transform is unchanged
    CHECK(local.x == doctest::Approx(before.x).epsilon(1e-4));
    CHECK(local.z == doctest::Approx(before.z).epsilon(1e-4));

    rt.emit("loop");  // Hand under its own child: a runtime error, nothing changes
    run(rt, 1);
    CHECK(errorsOf(rt).find("descendant") != std::string::npos);
    CHECK(s.record(hand)->parent == kNoEntity);

    // Through the engine: a play-time re-parent is undone when play stops.
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine e(cfg);
    (void)e.newScene("Parenting", false);
    auto call = [&](const char* tool, const char* args) {
        ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
        INFO(tool << ": " << (r.content.empty() ? "" : r.content.front().text));
        REQUIRE(!r.isError);
    };
    call("entity_create", R"({"name":"Hand","position":[0,1,0]})");
    call("entity_create", R"({"name":"Item","position":[3,0,0]})");
    call("behavior_set", R"({"entity":"Item","name":"Grab","source":"on start\n  set_parent(self, find(\"Hand\"))\nend"})");
    call("sim_control", R"({"action":"play"})");
    call("sim_control", R"({"action":"step","ticks":2})");
    CHECK(e.scene().record(e.scene().find("Item"))->parent == e.scene().find("Hand"));
    call("sim_control", R"({"action":"stop"})");
    CHECK(e.scene().record(e.scene().find("Item"))->parent == kNoEntity);
}
