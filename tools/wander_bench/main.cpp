// wander_bench: measures Wander execution speed on representative gameplay workloads.
//
//   wander_bench [--ticks N] [--filter NAME] [--native]   (--native: AOT-compiled behaviors)
//
// Every scenario builds a scene of entities running one script and times fixed ticks
// (1/60 s). Results are printed as a table and as JSON lines (for tracking over time).
// The "w1" scenarios only use syntax that Wander 1 understood, so the same numbers can be
// compared against the old tree-walking interpreter (see docs/WANDER.md, "Performance").

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Aot.h"
#include "skywalker/wander/Runtime.h"

#include <filesystem>
#include <unistd.h>

using namespace sky;

namespace {

struct Scenario {
    const char* name;
    const char* description;
    int entities;
    const char* source;
};

const Scenario kScenarios[] = {
    {"w1_compute", "arithmetic loop on locals (500 iterations x 100 entities)", 100, R"(
on tick
  let acc = 0
  let x = 1.5
  repeat 500 times
    acc = acc + x * 2 - 1
    x = x * 0.999 + 0.01
  end
  self.out = acc
end
)"},
    {"w1_gameplay", "typical per-entity logic: vars, movement, branches, timers (5000 entities)", 5000, R"(
behavior Bob
  var phase = 0
  var speed = 2
  var hits = 0
  on tick
    phase = phase + dt * speed
    move self by (0, sin(phase) * 0.01, 0)
    rotate self by (0, 30 * dt, 0)
    if self.position.y > 2 or self.position.y < -2 then
      speed = -speed
      hits = hits + 1
    elif hits > 100 then
      hits = 0
    end
    every 1 seconds
      speed = speed * 1.0
    end
  end
end
)"},
    {"w1_vectors", "vector math loop (100 iterations x 100 entities)", 100, R"(
on tick
  let p = self.position
  let v = (0, 0, 0)
  repeat 100 times
    v = v + normalize(p - (1, 2, 3)) * 0.01
    p = p + v * dt
  end
  self.position = p
end
)"},
    {"w1_builtins", "math builtin calls (300 iterations x 100 entities)", 100, R"(
on tick
  let acc = 0
  let x = 0.25
  repeat 300 times
    acc = acc + abs(sin(x)) + max(x, 0.5) + clamp(x, 0, 1) + floor(x * 10)
    x = x + 0.001
  end
  self.out = acc
end
)"},
    {"overhead", "scheduling cost: an empty on tick handler (5000 entities)", 5000, R"(
on tick
end
)"},
    {"w2_functions", "fn calls, lists and for loops (100 entities)", 100, R"(
fn dist2(a, b)
  let d = a - b
  return d.x * d.x + d.y * d.y + d.z * d.z
end
on tick
  let points = []
  for i in 0..50
    points.push((i, i * 0.5, 0))
  end
  let best = 1e9
  for p in points
    best = min(best, dist2(p, (10, 3, 0)))
  end
  self.out = best
end
)"},
};

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
    int ticks = 120;
    bool native = false;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!std::strcmp(argv[i], "--native")) native = true;
    }
    std::printf("%-14s %10s %12s  %s\n", "scenario", "ms/tick", "entities", "description");
    for (const Scenario& sc : kScenarios) {
        if (!filter.empty() && std::string(sc.name).find(filter) == std::string::npos) continue;
        Scene scene;
        for (int i = 0; i < sc.entities; ++i) {
            EntityId e = scene.create("E" + std::to_string(i));
            (void)scene.setBehaviors(e, Json::array({Json::object({{"name", "Bench"}, {"source", sc.source}})}));
        }
        wander::Runtime rt(scene);
        wander::InputState input;
        rt.compileScripts();
        if (native) {
            // AOT: compile the scenario's program to native code and run it instead of the VM.
            auto prog = scene.get<Behavior>(scene.entities().front())->scripts[0].program;
            wander::AotOptions o;
            o.cacheDir = (std::filesystem::temp_directory_path() / ("wander_bench_aot_" + std::to_string(::getpid()))).string();
            auto r = wander::compileNative(*prog, o);
            if (!r) {
                std::fprintf(stderr, "  [%s] AOT failed: %s\n", sc.name, r.error().message.c_str());
            } else {
                rt.attachNative(prog->hash, r->native);
            }
        }
        for (int i = 0; i < 5; ++i) rt.tick(1.f / 60.f, input);  // warm up
        auto msgs = rt.drainMessages();
        for (const auto& m : msgs) std::fprintf(stderr, "  [%s] %s\n", sc.name, m.text.c_str());
        double t0 = nowMs();
        for (int i = 0; i < ticks; ++i) rt.tick(1.f / 60.f, input);
        double ms = (nowMs() - t0) / ticks;
        std::printf("%-14s %10.3f %12d  %s\n", sc.name, ms, sc.entities, sc.description);
        std::printf("{\"scenario\":\"%s\",\"ms_per_tick\":%.4f,\"entities\":%d}\n", sc.name, ms, sc.entities);
    }
    return 0;
}
