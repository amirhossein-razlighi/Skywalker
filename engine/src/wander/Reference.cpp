// The LLM-oriented Wander guide (`wander_reference`). The builtin list is generated from
// the registry so it always matches what the compiler accepts.

#include <map>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/wander/Compiler.h"

namespace sky::wander {

namespace {

const char* kGuide = R"WANDER(Wander 2 — Skywalker's behavior language (ECPS: Entity · Component · Prompt System)

A behavior = an `intent` (plain language, the human source of truth) + deterministic code.
Safe by construction: every handler run has a step budget (loops can't hang the engine),
randomness is seeded (runs replay exactly), and the compiler reports line/column/code/hint.

behavior Guard
  intent "Patrol between two posts; chase the player when close; lose 1 hp per hit."
  param speed = 3 in 0..10 "walk speed (m/s)"   -- tunable: shown as a slider in the editor
  var hp = 3                                    -- per-entity state (persisted, visible to tools)
  var posts: list = []

  on start
    posts = [self.position, self.position + (6, 0, 0)]
  end

  state Patrol                                  -- first state is the initial one
    on tick
      let goal = posts[floor(state_time / 4) % 2]
      move self toward goal at speed
      let p = nearest("player", 5)
      if p then go to Chase end
    end
  end

  state Chase
    on enter
      self.color = #ff4040
    end
    on tick
      let p = nearest("player", 8)
      if not p then go to Patrol end
      move self toward p at speed * 1.5
    end
  end

  on event "damage" with hit                     -- payload map; `other` is the sender
    hp -= hit.get("amount", 1)
    if hp <= 0 then
      emit "guard_down" with {at: self.position}
      destroy self
    end
  end

  on event "alarm"
    wait 0.5                                     -- coroutine: resumes 0.5 s later (per entity)
    log "{self.name} heard the alarm"
  end

  test "loses hp when hit"
    emit "damage" with {amount: 1} to self
    wait frames 2
    expect hp == 2
  end
end

TRIGGERS  on start | on tick | on event "name" (with x) | on key "space" | on click
          inside a state: on enter | on exit | on tick | on event ...  (plus subsystem triggers, see below)
DECLARE   var x = 0 (per entity; `x` or `self.x`)  |  param x = 1 in 0..5 "doc"  |  const MAX = 10
          fn name(a, b: number) -> number ... return a + b end   (recursion ok)  |  use "scripts/combat" (then combat.fn())
STATEMENTS
  let x = v | x = v | x += v (-= *= /=) | set x to v
  if c then .. elif c then .. else .. end     (`then` optional)
  for x in list .. end | for i, x in list | for k, v in map | for i in 0..10 (end excluded; 0..=10 includes) step 2
  while c .. end | repeat n times .. end | break | continue | return v | stop
  every 2 seconds .. end | after 1 seconds .. end      (timers that tick with the handler)
  wait 1.5 | wait frames 3 | wait until hp <= 0         (handlers and tests only, not inside fn)
  go to StateName
  move e by vec | move e toward point at speed | rotate e by (0, 90 * dt, 0) | look e at point
  emit "event" (with payload) (to entity) | destroy e | log value
VALUES    1.5  "text {interpolated}"  true false none  #ff8800  (x, y, z)  [list]  {key: value}
OPERATORS + - * / %   < <= > >= == !=   and or not   x in list/map/string   "a" + 1 joins text
NAMES     self  other (sender / collision partner)  dt  time  frame  pi  state  state_time  data (event payload)
PROPERTIES e.position e.rotation (degrees) e.scale e.color e.name e.id e.enabled e.tags e.parent e.state
          e.<component>.<field> (self.light.intensity, self.particles.rate)   e.<var>
          v.x v.y v.z v.length   c.r c.g c.b c.a   list.length   string.length   map.key
LISTS/MAPS are values (copied on assignment); mutate in place with methods: self.items.push(x)
TESTS     test "name" .. end — run by wander_test in a sandbox: expect cond ("why"), wait (advances the sim),
          emit, press "space" / hold "w" / release "w" / click e, spawn(...). Tests start after the first tick.

IDIOMS
  - Smooth motion: move self toward target at speed  |  self.position = lerp(self.position, goal, 1 - exp(-8 * dt))
  - Frame-rate independent: always multiply per-tick changes by dt.
  - Sequences: on event "open" .. wait 1 .. end — no manual timers.
  - Find things: nearest("enemy", 10), find_all("coin"), find("Door"); check `if x then` before using a maybe-none.
  - Cooldowns: var cooldown = 0 / cooldown -= dt / if cooldown <= 0 then ... cooldown = 1 end
  - Share code across behaviors with modules: use "scripts/util" then util.clamp01(x).
  - Prefer states over boolean flags for modes (Idle/Chase/Flee); `state` reads the current one.
RULES     meters, +Y up, entities face -Z, rotations are Euler degrees (pitch X, yaw Y, roll Z).
          A handler that errors 5 times disables its script. Events arrive next tick, in emission order.
)WANDER";

}  // namespace

std::string referenceText(const BuiltinRegistry& registry) {
    std::ostringstream os;
    os << kGuide << "\nFUNCTIONS (category: signature — what it does)\n";
    std::map<std::string, std::vector<std::shared_ptr<const BuiltinDef>>> byCategory;
    for (const auto& d : registry.all(false)) byCategory[d->category].push_back(d);
    for (const auto& [cat, defs] : byCategory) {
        os << "  [" << cat << "]\n";
        for (const auto& d : defs) os << "    " << d->signature() << " — " << d->doc << "\n";
    }
    auto triggers = registry.triggers();
    if (!triggers.empty()) {
        os << "SUBSYSTEM TRIGGERS\n";
        for (const auto& t : triggers) {
            os << "    on " << t.name << " — " << t.doc;
            if (!t.payload.empty()) os << " data: " << t.payload;
            os << "\n";
        }
    }
    os << "More: wander_reference topic=\"<category or function>\" for details and examples.\n";
    return os.str();
}

Json builtinReference(const BuiltinRegistry& registry, std::string_view filter) {
    Json out = Json::array();
    std::string f = str::lower(filter);
    for (const auto& d : registry.all(false)) {
        if (!f.empty() && str::lower(d->category) != f && str::lower(d->name) != f) continue;
        out.push(d->toJson());
    }
    if (!f.empty()) {
        for (const auto& t : registry.triggers()) {
            if (str::lower(t.name) == f || str::lower(t.category) == f) {
                out.push(Json::object({{"trigger", t.name}, {"doc", t.doc}, {"payload", t.payload}, {"category", t.category}}));
            }
        }
    }
    return out;
}

}  // namespace sky::wander
