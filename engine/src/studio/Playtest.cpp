#include "skywalker/studio/Playtest.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "../agent/ToolHelpers.h"
#include "skywalker/core/Random.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/studio/Studio.h"

namespace sky::studio {

namespace {

constexpr double kDt = Engine::kFixedDt;
constexpr float kCell = 2.f;  // coverage grid cell (meters)

double round3(double v) { return std::round(v * 1000) / 1000; }
double round1(double v) { return std::round(v * 10) / 10; }

Json vec3Json(Vec3 v) { return Json::array({round3(v.x), round3(v.y), round3(v.z)}); }

enum class EventClass { None, Death, Fail, Damage, Objective, Pickup, Checkpoint };

EventClass classify(const std::string& raw) {
    std::string n = str::lower(raw);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (has("checkpoint")) return EventClass::Checkpoint;
    if (has("death") || has("died") || has("killed") || n == "die" || has("respawn")) return EventClass::Death;
    if (has("game_over") || has("fail") || n == "lose" || n == "lost" || has("timeout")) return EventClass::Fail;
    if (has("damage") || has("hurt") || n == "hit" || has("player_hit")) return EventClass::Damage;
    if (has("objective") || has("goal") || n == "win" || n == "won" || has("victory") || has("level_complete") ||
        n == "complete" || n == "finish" || n == "finished")
        return EventClass::Objective;
    if (has("collect") || has("pickup") || has("picked_up")) return EventClass::Pickup;
    return EventClass::None;
}

/// Closest point of an AABB to p.
Vec3 closestPoint(const Aabb& b, Vec3 p) { return vmax(b.min, vmin(b.max, p)); }

float distToBox(const Aabb& b, Vec3 p) { return length(closestPoint(b, p) - p); }

float distToBoxXZ(const Aabb& b, Vec3 p) {
    Vec3 c = closestPoint(b, p);
    float dx = c.x - p.x, dz = c.z - p.z;
    return std::sqrt(dx * dx + dz * dz);
}

struct Marker {
    EntityId id = kNoEntity;
    std::string name;
    Aabb box;
};

struct Death {
    double t;
    Vec3 pos;
    std::string cause;
};

struct StuckPeriod {
    double start, end;
    Vec3 pos;
};

struct Shot {
    std::string file;
    int run;
    double t;
    std::string reason;
};

/// Everything recorded in one run.
struct RunRecord {
    uint64_t seed = 0;
    std::vector<std::pair<double, Vec3>> trajectory;
    std::vector<Death> deaths;
    int fails = 0, damage = 0, pickups = 0, checkpoints = 0, objectiveEvents = 0;
    std::vector<std::pair<double, std::string>> objectives;  // goals reached (entity or event)
    std::vector<StuckPeriod> stuck;
    double stuckSeconds = 0;
    bool completed = false, quit = false, playerLost = false;
    double timeToGoal = -1, timeToFirstGoal = -1, duration = 0;
    double distance = 0;
    double coverage = 0;
    int scriptErrors = 0;
    std::vector<std::string> errorTexts;
    Json events = Json::array();
    std::vector<double> tickMs;
};

/// Converts a planar direction into the movement keys to hold.
std::set<std::string> keysFor(Vec3 dir, const PlaytestControls& c) {
    std::set<std::string> keys;
    if (length(dir) < 1e-4f) return keys;
    dir = normalize(dir);
    const float t = 0.38f;  // ~sin(22.5°): 8-way movement
    if (dir.x > t) keys.insert(c.right);
    if (dir.x < -t) keys.insert(c.left);
    if (dir.z < -t) keys.insert(c.up);
    if (dir.z > t) keys.insert(c.down);
    return keys;
}

Vec3 rotateY(Vec3 v, float deg) {
    float r = radians(deg), cs = std::cos(r), sn = std::sin(r);
    return {v.x * cs - v.z * sn, 0, v.x * sn + v.z * cs};
}

class Bot {
public:
    Bot(Engine& sim, const PlaytestConfig& cfg, uint64_t seed, EntityId player, std::vector<Marker> goals,
        std::vector<Marker> hazards, Aabb level)
        : sim_(sim), cfg_(cfg), rng_(seed * 2654435761u + 17), player_(player), goals_(std::move(goals)),
          hazards_(std::move(hazards)), level_(level) {
        actor_ = "bot:" + cfg.policy;
        int cx = std::max(1, static_cast<int>(std::ceil((level_.max.x - level_.min.x) / kCell)));
        int cz = std::max(1, static_cast<int>(std::ceil((level_.max.z - level_.min.z) / kCell)));
        gridW_ = std::min(cx, 512);
        gridH_ = std::min(cz, 512);
        visited_.assign(static_cast<size_t>(gridW_) * gridH_, 0);
        reactionTicks_ = std::max(1, static_cast<int>(std::lround(cfg.persona.reactionTime / kDt)));
        if (!cfg.script.isArray()) return;
        for (const auto& e : cfg.script.elements()) script_.push_back(e);
        std::stable_sort(script_.begin(), script_.end(),
                         [](const Json& a, const Json& b) { return a.get("t").asNumber() < b.get("t").asNumber(); });
    }

    std::vector<std::string> warnings;
    std::set<EntityId> reached;  // goals reached (owned by the run loop)

    /// Called before each tick. `pos` is the player's position (if any).
    void act(int tick, Vec3 pos, bool havePlayer, bool stuck) {
        double t = tick * kDt;
        if (havePlayer) markVisited(pos);
        if (cfg_.policy == "scripted") {
            while (scriptIndex_ < script_.size() && script_[scriptIndex_].get("t").asNumber() <= t + 1e-9) {
                Json args = script_[scriptIndex_++];
                args.erase("t");
                send(args);
            }
            return;
        }
        if (!havePlayer) return;
        if (tick < nextDecision_) return;
        nextDecision_ = tick + reactionTicks_;
        Vec3 dir;
        bool jump = false;
        if (cfg_.policy == "random") {
            if (rng_.nextFloat() < 0.55f || length(lastDir_) < 1e-3f) {
                int k = static_cast<int>(rng_.next() % 9);
                lastDir_ = k == 8 ? Vec3{} : rotateY({0, 0, -1}, 45.f * k);
            }
            dir = lastDir_;
            jump = rng_.nextFloat() < 0.12f;
        } else {
            dir = steer(t, pos, stuck, jump);
        }
        apply(keysFor(dir, cfg_.controls), jump);
    }

    void releaseAll() { apply({}, false); }
    bool wantsQuit() const { return quit_; }
    bool moving() const { return !held_.empty(); }
    double coverage() const {
        size_t n = 0;
        for (uint8_t v : visited_) n += v;
        return visited_.empty() ? 0 : static_cast<double>(n) / static_cast<double>(visited_.size());
    }

private:
    void send(const Json& args) {
        ToolResult r = sim_.callTool("sim_input", args, actor_);
        if (r.isError && warnings.size() < 5) {
            warnings.push_back("sim_input rejected " + args.dump() + ": " + (r.content.empty() ? "" : r.content.front().text));
        }
    }

    void apply(const std::set<std::string>& keys, bool jump) {
        Json hold = Json::array(), release = Json::array();
        for (const auto& k : keys) {
            if (!held_.count(k)) hold.push(k);
        }
        for (const auto& k : held_) {
            if (!keys.count(k)) release.push(k);
        }
        Json args = Json::object();
        if (hold.size()) args["hold"] = hold;
        if (release.size()) args["release"] = release;
        if (jump) args["press"] = Json::array({cfg_.controls.jump});
        held_ = keys;
        if (!args.members().empty()) send(args);
    }

    void cellOf(Vec3 p, int& x, int& z) const {
        x = std::clamp(static_cast<int>((p.x - level_.min.x) / kCell), 0, gridW_ - 1);
        z = std::clamp(static_cast<int>((p.z - level_.min.z) / kCell), 0, gridH_ - 1);
    }

    void markVisited(Vec3 p) {
        int x, z;
        cellOf(p, x, z);
        visited_[static_cast<size_t>(z) * gridW_ + x] = 1;
    }

    Vec3 cellCenter(int x, int z) const {
        return {level_.min.x + (static_cast<float>(x) + 0.5f) * kCell, 0, level_.min.z + (static_cast<float>(z) + 0.5f) * kCell};
    }

    /// Nearest unvisited cell, with a little randomness so runs differ by seed.
    std::optional<Vec3> explorationTarget(Vec3 pos) {
        float best = 1e30f;
        std::optional<Vec3> out;
        for (int z = 0; z < gridH_; ++z) {
            for (int x = 0; x < gridW_; ++x) {
                if (visited_[static_cast<size_t>(z) * gridW_ + x]) continue;
                Vec3 c = cellCenter(x, z);
                c.y = pos.y;
                float d = distance(c, pos) * (0.8f + 0.4f * rng_.nextFloat());
                bool nearHazard = std::any_of(hazards_.begin(), hazards_.end(), [&](const Marker& h) { return distToBoxXZ(h.box, c) < 1.f; });
                if (nearHazard) d += 50.f;
                if (d < best) {
                    best = d;
                    out = c;
                }
            }
        }
        return out;
    }

    bool blocked(Vec3 pos, Vec3 dir, float reach, float& obstacleTop) {
        std::vector<EntityId> exclude;
        tools::collectSubtree(sim_.scene(), player_, exclude);
        for (const auto& g : goals_) {
            if (sim_.scene().exists(g.id)) tools::collectSubtree(sim_.scene(), g.id, exclude);
        }
        auto hit = sim_.raycast(Ray{pos, normalize(dir)}, exclude);
        if (!hit || hit->distance > reach) return false;
        obstacleTop = pos.y;
        if (auto box = tools::subtreeBounds(sim_.scene(), hit->entity)) obstacleTop = box->max.y;
        return true;
    }

    Vec3 steer(double t, Vec3 pos, bool stuck, bool& jump) {
        const PlaytestPersona& p = cfg_.persona;
        // Detour after getting stuck: a random heading for a moment, plus a jump attempt.
        if (stuck && t >= detourUntil_ && t - lastDetour_ > 1.0) {
            detourDir_ = rotateY({0, 0, -1}, rng_.range(0.f, 360.f));
            detourUntil_ = t + 0.8 + 1.2 * rng_.nextFloat();
            lastDetour_ = t;
            jump = rng_.nextFloat() < 0.4f + 0.5f * p.skill;
        }
        if (t < detourUntil_) return detourDir_;

        // Pick a target: next unreached goal (goal seeker) or unexplored space (explorer,
        // curious detours, or no goals in the level).
        std::optional<Vec3> target;
        const Marker* goal = nullptr;
        if (cfg_.policy == "goal_seeker") {
            float best = 1e30f;
            for (const auto& g : goals_) {
                if (reached.count(g.id) || !sim_.scene().exists(g.id)) continue;
                float d = distToBox(g.box, pos);
                if (d < best) {
                    best = d;
                    goal = &g;
                }
            }
            if (goal) target = goal->box.center();
            if (goal && t >= curiousUntil_ && rng_.nextFloat() < p.curiosity * 0.04f) {
                curiousTarget_ = explorationTarget(pos);
                curiousUntil_ = t + 1.5 + 2.0 * p.curiosity;
            }
            if (goal && t < curiousUntil_ && curiousTarget_) target = curiousTarget_;
        }
        if (!target) {
            if (!exploreTarget_ || distance(*exploreTarget_, Vec3{pos.x, exploreTarget_->y, pos.z}) < kCell * 0.6f || stuck) {
                exploreTarget_ = explorationTarget(pos);
            }
            target = exploreTarget_;
        }
        if (!target) return {};

        // Progress tracking for patience (quit when nothing improves for too long).
        if (goal) {
            float d = distToBox(goal->box, pos);
            if (progressGoal_ != goal->id || d < bestDistance_ - 0.5f) {
                progressGoal_ = goal->id;
                bestDistance_ = d;
                lastProgress_ = t;
            }
            if (t - lastProgress_ > p.patience) quit_ = true;
        }

        Vec3 dir = *target - pos;
        dir.y = 0;
        if (length(dir) < 1e-3f) return {};
        dir = normalize(dir);
        // Hazards push the bot away; skilled players keep a wider berth.
        float radius = 1.2f + 1.6f * p.skill;
        for (const auto& h : hazards_) {
            if (!sim_.scene().exists(h.id)) continue;
            Vec3 c = closestPoint(h.box, pos);
            Vec3 away = pos - c;
            away.y = 0;
            float d = length(away);
            if (d >= radius) continue;
            if (d < 1e-3f) {
                away = rotateY(dir, 90.f);  // standing on it: step sideways
                d = 0.f;
            }
            float w = (radius - d) / radius * (0.6f + 2.4f * p.skill);
            dir = dir + normalize(away) * w;
        }
        if (length(dir) < 1e-3f) dir = rotateY(*target - pos, 90.f);
        dir = normalize(Vec3{dir.x, 0, dir.z});
        // Imprecision: less skill, noisier steering.
        dir = rotateY(dir, rng_.range(-1.f, 1.f) * (1.f - p.skill) * 35.f);
        // Walls: probe ahead and slide around obstacles.
        float top = 0;
        if (blocked(pos, dir, 1.2f, top)) {
            if (top < pos.y + 1.2f && rng_.nextFloat() < 0.3f + 0.6f * p.skill) jump = true;
            for (float a : {45.f, -45.f, 90.f, -90.f, 135.f, -135.f}) {
                Vec3 alt = rotateY(dir, a);
                float t2 = 0;
                if (!blocked(pos, alt, 1.2f, t2)) {
                    dir = alt;
                    break;
                }
            }
        }
        return dir;
    }

    Engine& sim_;
    const PlaytestConfig& cfg_;
    Random rng_;
    EntityId player_;
    std::vector<Marker> goals_, hazards_;
    Aabb level_;
    std::string actor_;
    std::vector<Json> script_;
    size_t scriptIndex_ = 0;
    std::set<std::string> held_;
    int reactionTicks_ = 15;
    int nextDecision_ = 0;
    Vec3 lastDir_;
    Vec3 detourDir_;
    double detourUntil_ = -1, lastDetour_ = -10;
    std::optional<Vec3> exploreTarget_, curiousTarget_;
    double curiousUntil_ = -1;
    EntityId progressGoal_ = kNoEntity;
    float bestDistance_ = 1e30f;
    double lastProgress_ = 0;
    bool quit_ = false;
    int gridW_ = 1, gridH_ = 1;
    std::vector<uint8_t> visited_;
};

std::vector<Marker> markersTagged(const Scene& s, std::initializer_list<const char*> tags) {
    std::vector<Marker> out;
    std::set<EntityId> seen;
    for (const char* tag : tags) {
        for (EntityId e : s.findTagged(tag)) {
            if (!seen.insert(e).second || !s.isActive(e)) continue;
            Marker m;
            m.id = e;
            m.name = s.record(e)->name;
            if (auto b = tools::subtreeBounds(s, e)) {
                m.box = *b;
            } else {
                Vec3 c = s.worldMatrix(e).translation();
                m.box = {c - Vec3(0.5f), c + Vec3(0.5f)};
            }
            out.push_back(m);
        }
    }
    return out;
}

Aabb levelBounds(const Scene& s, Vec3 fallbackCenter) {
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    bool any = false;
    for (EntityId e : s.entities()) {
        if (!s.get<MeshRenderer>(e) || !s.isActive(e)) continue;
        Aabb b = s.localBounds(e).transformed(s.worldMatrix(e));
        box.min = vmin(box.min, b.min);
        box.max = vmax(box.max, b.max);
        any = true;
    }
    if (!any) box = {fallbackCenter - Vec3(10.f), fallbackCenter + Vec3(10.f)};
    // Keep the analysis grid sane for open worlds.
    for (float* lo : {&box.min.x, &box.min.z}) *lo = std::max(*lo, (lo == &box.min.x ? fallbackCenter.x : fallbackCenter.z) - 400.f);
    for (float* hi : {&box.max.x, &box.max.z}) *hi = std::min(*hi, (hi == &box.max.x ? fallbackCenter.x : fallbackCenter.z) + 400.f);
    return box;
}

// --- Heatmap ------------------------------------------------------------------------

struct Canvas {
    Image img;
    Aabb area;
    float scale = 1, ox = 0, oy = 0;

    Canvas(int size, Aabb a) : img(size, size), area(a) {
        float w = std::max(1.f, a.max.x - a.min.x), h = std::max(1.f, a.max.z - a.min.z);
        scale = static_cast<float>(size - 8) / std::max(w, h);
        ox = (static_cast<float>(size) - w * scale) * 0.5f;
        oy = (static_cast<float>(size) - h * scale) * 0.5f;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) set(x, y, 0x16, 0x18, 0x1d);
        }
    }
    void toPx(Vec3 p, float& x, float& y) const {
        x = ox + (p.x - area.min.x) * scale;
        y = oy + (p.z - area.min.z) * scale;  // top of the image = -Z ("forward", W)
    }
    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
        uint8_t* p = img.at(x, y);
        p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
    }
    void blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
        uint8_t* p = img.at(x, y);
        p[0] = static_cast<uint8_t>(std::clamp(p[0] * (1 - a) + r * a, 0.f, 255.f));
        p[1] = static_cast<uint8_t>(std::clamp(p[1] * (1 - a) + g * a, 0.f, 255.f));
        p[2] = static_cast<uint8_t>(std::clamp(p[2] * (1 - a) + b * a, 0.f, 255.f));
    }
    void rect(const Aabb& b, uint8_t r, uint8_t g, uint8_t bl, float alpha) {
        float x0, y0, x1, y1;
        toPx(b.min, x0, y0);
        toPx(b.max, x1, y1);
        for (int y = static_cast<int>(std::floor(std::min(y0, y1))); y <= static_cast<int>(std::ceil(std::max(y0, y1))); ++y) {
            for (int x = static_cast<int>(std::floor(std::min(x0, x1))); x <= static_cast<int>(std::ceil(std::max(x0, x1))); ++x) {
                blend(x, y, r, g, bl, alpha);
            }
        }
    }
    void ring(Vec3 p, float radius, uint8_t r, uint8_t g, uint8_t b) {
        float cx, cy;
        toPx(p, cx, cy);
        for (int a = 0; a < 48; ++a) {
            float ang = static_cast<float>(a) / 48.f * 6.2831853f;
            blend(static_cast<int>(cx + std::cos(ang) * radius), static_cast<int>(cy + std::sin(ang) * radius), r, g, b, 1.f);
        }
    }
    void cross(Vec3 p, uint8_t r, uint8_t g, uint8_t b) {
        float cx, cy;
        toPx(p, cx, cy);
        for (int d = -3; d <= 3; ++d) {
            set(static_cast<int>(cx) + d, static_cast<int>(cy) + d, r, g, b);
            set(static_cast<int>(cx) + d, static_cast<int>(cy) - d, r, g, b);
        }
    }
};

/// Inferno-like ramp for heat.
void heatColor(float v, float& r, float& g, float& b) {
    v = std::clamp(v, 0.f, 1.f);
    struct Stop {
        float t, r, g, b;
    };
    static const Stop stops[] = {{0.f, 40, 10, 90}, {0.35f, 150, 30, 120}, {0.65f, 240, 100, 40}, {1.f, 252, 240, 120}};
    for (int i = 0; i < 3; ++i) {
        if (v <= stops[i + 1].t) {
            float f = (v - stops[i].t) / (stops[i + 1].t - stops[i].t);
            r = stops[i].r + (stops[i + 1].r - stops[i].r) * f;
            g = stops[i].g + (stops[i + 1].g - stops[i].g) * f;
            b = stops[i].b + (stops[i + 1].b - stops[i].b) * f;
            return;
        }
    }
    r = 252, g = 240, b = 120;
}

Image drawHeatmap(const Scene& s, int size, const Aabb& level, const std::vector<RunRecord>& runs, const std::vector<Marker>& goals,
                  const std::vector<Marker>& hazards, EntityId player) {
    Aabb area{level.min - Vec3(2.f), level.max + Vec3(2.f)};
    Canvas c(size, area);
    std::set<EntityId> special;
    for (const auto& g : goals) special.insert(g.id);
    for (const auto& h : hazards) special.insert(h.id);
    std::vector<EntityId> playerTree;
    if (player) tools::collectSubtree(s, player, playerTree);
    float levelArea = (level.max.x - level.min.x) * (level.max.z - level.min.z);
    for (EntityId e : s.entities()) {
        if (!s.get<MeshRenderer>(e) || !s.isActive(e) || special.count(e)) continue;
        if (std::find(playerTree.begin(), playerTree.end(), e) != playerTree.end()) continue;
        Aabb b = s.localBounds(e).transformed(s.worldMatrix(e));
        bool ground = (b.max.y - b.min.y) < 0.6f && (b.max.x - b.min.x) * (b.max.z - b.min.z) > levelArea * 0.3f;
        if (ground) c.rect(b, 0x24, 0x28, 0x30, 1.f);
    }
    for (EntityId e : s.entities()) {
        if (!s.get<MeshRenderer>(e) || !s.isActive(e) || special.count(e)) continue;
        if (std::find(playerTree.begin(), playerTree.end(), e) != playerTree.end()) continue;
        Aabb b = s.localBounds(e).transformed(s.worldMatrix(e));
        bool ground = (b.max.y - b.min.y) < 0.6f && (b.max.x - b.min.x) * (b.max.z - b.min.z) > levelArea * 0.3f;
        if (!ground) c.rect(b, 0x4a, 0x50, 0x5c, 0.9f);
    }
    for (const auto& h : hazards) c.rect(h.box, 0xc8, 0x3a, 0x44, 0.85f);
    for (const auto& g : goals) c.rect(g.box, 0x3c, 0xc0, 0x6a, 0.9f);

    // Trajectory density with a small splat.
    std::vector<float> heat(static_cast<size_t>(size) * size, 0.f);
    float peak = 0;
    for (const auto& r : runs) {
        for (const auto& [t, p] : r.trajectory) {
            float px, py;
            c.toPx(p, px, py);
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    int x = static_cast<int>(px) + dx, y = static_cast<int>(py) + dy;
                    if (x < 0 || y < 0 || x >= size || y >= size) continue;
                    float w = std::exp(-(dx * dx + dy * dy) / 2.f);
                    float& h = heat[static_cast<size_t>(y) * size + x];
                    h += w;
                    peak = std::max(peak, h);
                }
            }
        }
    }
    if (peak > 0) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float v = heat[static_cast<size_t>(y) * size + x];
                if (v <= 0) continue;
                float n = std::sqrt(v / peak), r, g, b;
                heatColor(n, r, g, b);
                c.blend(x, y, r, g, b, std::min(1.f, 0.35f + n));
            }
        }
    }
    for (const auto& r : runs) {
        if (!r.trajectory.empty()) c.ring(r.trajectory.front().second, 3.f, 0x6a, 0xd8, 0xff);
        for (const auto& st : r.stuck) c.ring(st.pos, 5.f, 0xff, 0xd2, 0x3f);
        for (const auto& d : r.deaths) c.cross(d.pos, 0xff, 0xff, 0xff);
    }
    return c.img;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::clamp(p * static_cast<double>(v.size() - 1), 0.0, static_cast<double>(v.size() - 1)));
    return v[i];
}

}  // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

Result<PlaytestConfig> PlaytestConfig::fromJson(const Json& j) {
    PlaytestConfig c;
    c.policy = j.get("policy").asString(c.policy);
    static const std::vector<std::string> policies = {"scripted", "random", "explorer", "goal_seeker"};
    if (std::find(policies.begin(), policies.end(), c.policy) == policies.end()) {
        std::string guess = str::closest(c.policy, policies, 4);
        return Error::make("invalid_arguments", "unknown policy '" + c.policy + "'",
                           guess.empty() ? "use scripted, random, explorer or goal_seeker" : "did you mean '" + guess + "'?");
    }
    c.seconds = std::clamp(j.get("seconds").asNumber(c.seconds), 1.0, 600.0);
    c.runs = static_cast<int>(std::clamp<int64_t>(j.get("runs").asInt(c.runs), 1, 20));
    c.seed = static_cast<uint64_t>(std::max<int64_t>(0, j.get("seed").asInt(1)));
    const Json& p = j.get("persona");
    c.persona.reactionTime = std::clamp(p.get("reaction_time").asFloat(c.persona.reactionTime), 0.02f, 3.f);
    c.persona.skill = std::clamp(p.get("skill").asFloat(c.persona.skill), 0.f, 1.f);
    c.persona.curiosity = std::clamp(p.get("curiosity").asFloat(c.persona.curiosity), 0.f, 1.f);
    c.persona.patience = std::clamp(p.get("patience").asFloat(c.persona.patience), 1.f, 600.f);
    const Json& k = j.get("controls");
    c.controls.up = str::lower(k.get("up").asString(c.controls.up));
    c.controls.down = str::lower(k.get("down").asString(c.controls.down));
    c.controls.left = str::lower(k.get("left").asString(c.controls.left));
    c.controls.right = str::lower(k.get("right").asString(c.controls.right));
    c.controls.jump = str::lower(k.get("jump").asString(c.controls.jump));
    if (j.get("script").isArray()) c.script = j.get("script");
    if (c.policy == "scripted" && c.script.size() == 0) {
        return Error::make("invalid_arguments", "the scripted policy needs a script",
                           "script: [{\"t\":0,\"hold\":[\"w\"]},{\"t\":1.5,\"press\":[\"space\"]},{\"t\":3,\"release\":[\"w\"]}]");
    }
    c.player = j.get("player").isNumber() ? j.get("player").dump() : j.get("player").asString();
    c.goalRadius = std::clamp(j.get("goal_radius").asNumber(c.goalRadius), 0.1, 20.0);
    c.screenshots = j.get("screenshots").asBool(true);
    c.shotInterval = std::clamp(j.get("screenshot_interval").asNumber(0), 0.0, 600.0);
    c.maxShots = static_cast<int>(std::clamp<int64_t>(j.get("max_screenshots").asInt(c.maxShots), 0, 40));
    c.heatmapSize = static_cast<int>(std::clamp<int64_t>(j.get("heatmap_size").asInt(c.heatmapSize), 64, 1024));
    c.label = j.get("label").asString();
    return c;
}

Json PlaytestConfig::toJson() const {
    Json j = Json::object({{"policy", policy},
                           {"seconds", seconds},
                           {"runs", runs},
                           {"seed", static_cast<int64_t>(seed)},
                           {"persona", Json::object({{"reaction_time", round3(persona.reactionTime)},
                                                     {"skill", round3(persona.skill)},
                                                     {"curiosity", round3(persona.curiosity)},
                                                     {"patience", round3(persona.patience)}})},
                           {"controls", Json::object({{"up", controls.up},
                                                      {"down", controls.down},
                                                      {"left", controls.left},
                                                      {"right", controls.right},
                                                      {"jump", controls.jump}})},
                           {"goal_radius", goalRadius}});
    if (!player.empty()) j["player"] = player;
    if (policy == "scripted") j["script"] = script;
    if (!label.empty()) j["label"] = label;
    return j;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

Result<PlaytestResult> runPlaytest(Engine& source, const PlaytestConfig& cfg) {
    // Sandbox: a separate engine on the same project, holding a copy of the scene. The
    // editor's scene, undo history, selection and simulation are never touched.
    EngineConfig sc;
    sc.renderer = cfg.screenshots ? source.config().renderer : RendererBackend::Null;
    sc.projectDir = source.config().projectDir;
    Engine sim(sc);
    Json snapshot = source.scene().toJson();
    std::string sceneName = source.scene().name;

    PlaytestResult result;
    std::vector<RunRecord> runs;
    std::vector<Shot> shots;
    std::vector<std::string> warnings;
    std::vector<Marker> goalsForMap, hazardsForMap;
    Aabb levelForMap;
    EntityId playerForMap = kNoEntity;
    std::vector<double> renderMs;
    const int totalTicks = static_cast<int>(std::lround(cfg.seconds / kDt));

    for (int run = 0; run < cfg.runs; ++run) {
        if (Status s = sim.scene().loadJson(snapshot); !s) return s.error();
        sim.scene().assetBounds = source.scene().assetBounds;
        RunRecord rec;
        rec.seed = cfg.seed + static_cast<uint64_t>(run);
        sim.scene().seed = static_cast<uint64_t>(snapshot.get("seed").asInt(1)) * 1000003u + rec.seed;

        EntityId player = kNoEntity;
        if (!cfg.player.empty()) {
            bool numeric = std::all_of(cfg.player.begin(), cfg.player.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
            auto r = tools::resolve(sim, numeric ? Json(std::stoll(cfg.player)) : Json(cfg.player));
            if (!r) return r.error();
            player = *r;
        } else {
            auto tagged = sim.scene().findTagged("player");
            if (!tagged.empty()) player = tagged.front();
        }
        if (!player && cfg.policy != "scripted") {
            return Error::make("no_player", "no entity tagged \"player\" in the scene",
                               "tag the player entity \"player\" (entity_update {tags:[\"player\"]}) or pass player");
        }
        std::vector<Marker> goals = markersTagged(sim.scene(), {"goal", "objective"});
        std::vector<Marker> hazards = markersTagged(sim.scene(), {"hazard", "enemy", "danger"});
        Vec3 start = player ? sim.scene().worldMatrix(player).translation() : Vec3{};
        Aabb level = levelBounds(sim.scene(), start);
        if (run == 0) {
            goalsForMap = goals;
            hazardsForMap = hazards;
            levelForMap = level;
            playerForMap = player;
            if (goals.empty() && cfg.policy == "goal_seeker") {
                warnings.push_back("no entity tagged \"goal\" or \"objective\": the goal seeker explored instead");
            }
        }

        // Observe every emitted event (deaths, objectives...).
        struct Emitted {
            std::string name;
            EntityId source;
        };
        std::vector<Emitted> emitted;
        sim.runtime().onEmit = [&emitted](const std::string& name, EntityId, EntityId src) { emitted.push_back({name, src}); };

        Bot bot(sim, cfg, rec.seed, player, goals, hazards, level);
        sim.play();
        (void)sim.drainEvents();

        Vec3 prev = start;
        bool havePlayer = player != kNoEntity;
        std::vector<Vec3> window;  // recent positions (2 s) for stuck detection
        const size_t windowTicks = 120;
        bool stuck = false;
        double stuckStart = 0;
        int lastDeathTick = -1000;
        bool shotStuck = false;
        int deathShots = 0;
        double nextPeriodic = cfg.shotInterval > 0 ? cfg.shotInterval : 1e30;

        auto takeShot = [&](double t, const std::string& reason, Vec3 at) {
            if (!cfg.screenshots || static_cast<int>(shots.size()) >= cfg.maxShots) return;
            CaptureOptions o;
            o.width = cfg.shotWidth;
            o.height = cfg.shotHeight;
            o.editorOverlays = false;
            o.hasCustomView = true;
            o.customView.eye = at + Vec3{0, 7, 9};
            o.customView.target = at + Vec3{0, 0.5f, 0};
            auto cap = sim.capture(o);
            if (!cap) return;
            char name[64];
            std::snprintf(name, sizeof(name), "shot_%02zu_r%d_%s.png", shots.size(), run, reason.c_str());
            result.shots.emplace_back(name, std::move(cap->image));
            shots.push_back({name, run, t, reason});
        };

        // Frame cost: render a 960x540 follow-cam frame every 2 s of game time (after one
        // untimed warm-up frame that pays for pipeline compilation). Includes readback.
        auto sampleFrame = [&](Vec3 at, bool timed) {
            if (!cfg.screenshots) return;
            CaptureOptions o;
            o.width = 960;
            o.height = 540;
            o.editorOverlays = false;
            o.hasCustomView = true;
            o.customView.eye = at + Vec3{0, 7, 9};
            o.customView.target = at + Vec3{0, 0.5f, 0};
            auto t0 = std::chrono::steady_clock::now();
            (void)sim.capture(o);
            if (timed) renderMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        };
        if (run == 0) sampleFrame(start, false);

        int tick = 0;
        for (; tick < totalTicks; ++tick) {
            double t = tick * kDt;
            Vec3 pos = havePlayer && sim.scene().exists(player) ? sim.scene().worldMatrix(player).translation() : prev;
            bot.act(tick, pos, havePlayer && sim.scene().exists(player), stuck);
            auto t0 = std::chrono::steady_clock::now();
            sim.step(1);
            rec.tickMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            double now = (tick + 1) * kDt;

            // Logs and errors from the game's scripts.
            for (const auto& e : sim.drainEvents()) {
                if (e.get("type").asString() != "log") continue;
                std::string kind = e.get("kind").asString();
                if (kind == "error" || kind == "compile") {
                    rec.scriptErrors++;
                    if (rec.errorTexts.size() < 5) {
                        rec.errorTexts.push_back(e.get("script").asString() + ":" + std::to_string(e.get("line").asInt()) + " " +
                                                 e.get("text").asString());
                    }
                }
                if (rec.events.size() < 300) {
                    rec.events.push(Json::object({{"t", round3(now)}, {"log", e.get("text")}, {"kind", kind}}));
                }
            }

            bool alive = havePlayer && sim.scene().exists(player);
            Vec3 cur = alive ? sim.scene().worldMatrix(player).translation() : prev;

            // Events emitted this tick.
            for (const auto& ev : emitted) {
                EventClass cls = classify(ev.name);
                if (rec.events.size() < 300) {
                    Json row = Json::object({{"t", round3(now)}, {"event", ev.name}});
                    if (ev.source && sim.scene().exists(ev.source)) row["source"] = sim.scene().record(ev.source)->name;
                    rec.events.push(row);
                }
                switch (cls) {
                    case EventClass::Death: {
                        std::string cause = ev.name;
                        float best = 3.f;
                        for (const auto& h : hazards) {
                            float d = distToBox(h.box, prev);
                            if (d < best) {
                                best = d;
                                cause = h.name;
                            }
                        }
                        if (ev.source && ev.source != player && sim.scene().exists(ev.source)) {
                            bool isHazard = std::any_of(hazards.begin(), hazards.end(), [&](const Marker& h) { return h.id == ev.source; });
                            if (isHazard) cause = sim.scene().record(ev.source)->name;
                        }
                        rec.deaths.push_back({now, prev, cause});
                        lastDeathTick = tick;
                        if (deathShots++ < 2) takeShot(now, "death", prev);
                        break;
                    }
                    case EventClass::Fail: rec.fails++; break;
                    case EventClass::Damage: rec.damage++; break;
                    case EventClass::Objective:
                        rec.objectiveEvents++;
                        rec.objectives.push_back({now, ev.name});
                        if (rec.timeToFirstGoal < 0) rec.timeToFirstGoal = now;
                        break;
                    case EventClass::Pickup: rec.pickups++; break;
                    case EventClass::Checkpoint: rec.checkpoints++; break;
                    case EventClass::None: break;
                }
            }
            emitted.clear();

            if (havePlayer && !alive && !rec.playerLost) {
                rec.playerLost = true;
                if (lastDeathTick < tick - 30) rec.deaths.push_back({now, prev, "player destroyed"});
            }

            // Respawn detection: a sudden jump without a death event is counted as one.
            if (alive) {
                float step = length(cur - prev);
                if (step > 3.f && lastDeathTick < tick - 30) {
                    std::string cause = "respawn";
                    float best = 3.f;
                    for (const auto& h : hazards) {
                        float d = distToBox(h.box, prev);
                        if (d < best) {
                            best = d;
                            cause = h.name;
                        }
                    }
                    rec.deaths.push_back({now, prev, cause});
                    lastDeathTick = tick;
                    window.clear();
                } else if (step <= 3.f) {
                    rec.distance += step;
                }
            }

            // Goals reached by proximity (or collected while the player was close).
            if (alive) {
                for (const auto& g : goals) {
                    if (bot.reached.count(g.id)) continue;
                    bool exists = sim.scene().exists(g.id) && sim.scene().isActive(g.id);
                    Aabb box = g.box;
                    if (exists) {
                        if (auto b = tools::subtreeBounds(sim.scene(), g.id)) box = *b;
                    }
                    float d = distToBox(box, cur);
                    if (d <= cfg.goalRadius || (!exists && d <= cfg.goalRadius + 2.f)) {
                        bot.reached.insert(g.id);
                        rec.objectives.push_back({now, g.name});
                        if (rec.timeToFirstGoal < 0) rec.timeToFirstGoal = now;
                        takeShot(now, "goal", cur);
                    }
                }
            }
            bool done = goals.empty() ? rec.objectiveEvents > 0 : bot.reached.size() == goals.size();
            if (done && !rec.completed) {
                rec.completed = true;
                rec.timeToGoal = now;
            }

            // Stuck: trying to move but going nowhere for two seconds.
            if (alive) {
                window.push_back(cur);
                if (window.size() > windowTicks) window.erase(window.begin());
                bool trying = bot.moving() || cfg.policy == "scripted";
                bool noProgress = window.size() == windowTicks && length(window.back() - window.front()) < 0.3f;
                if (!stuck && trying && noProgress) {
                    stuck = true;
                    stuckStart = now - 2.0;
                    if (!shotStuck) {
                        shotStuck = true;
                        takeShot(now, "stuck", cur);
                    }
                } else if (stuck && window.size() >= 30 && length(window.back() - window[window.size() - 30]) > 0.6f) {
                    stuck = false;
                    rec.stuck.push_back({stuckStart, now, cur});
                    rec.stuckSeconds += now - stuckStart;
                }
            }
            if (alive && t >= 0.25 * static_cast<double>(rec.trajectory.size())) rec.trajectory.push_back({now, cur});
            if ((tick + 1) % 120 == 0) sampleFrame(cur, true);
            if (now >= nextPeriodic) {
                takeShot(now, "periodic", cur);
                nextPeriodic += cfg.shotInterval;
            }
            prev = cur;
            if (rec.completed && cfg.policy == "goal_seeker") break;
            if (bot.wantsQuit()) {
                rec.quit = true;
                takeShot(now, "quit", cur);
                break;
            }
            if (rec.playerLost) break;
        }
        rec.duration = std::min(totalTicks, tick + 1) * kDt;
        if (stuck) {
            rec.stuck.push_back({stuckStart, rec.duration, prev});
            rec.stuckSeconds += rec.duration - stuckStart;
        }
        rec.coverage = bot.coverage();
        bot.releaseAll();
        if (run == cfg.runs - 1 && shots.size() < static_cast<size_t>(cfg.maxShots)) takeShot(rec.duration, "end", prev);
        for (auto& w : bot.warnings) {
            if (std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(w);
        }
        sim.runtime().onEmit = nullptr;
        sim.stop();
        runs.push_back(std::move(rec));
    }

    // --- Aggregate --------------------------------------------------------------------
    int n = static_cast<int>(runs.size());
    double completed = 0, deaths = 0, fails = 0, damage = 0, stuckS = 0, objectives = 0, coverage = 0, distanceSum = 0, quits = 0;
    double ttg = 0, ttgN = 0;
    int errors = 0;
    std::vector<double> allTicks;
    std::map<std::string, int> causes;
    for (const auto& r : runs) {
        completed += r.completed;
        deaths += static_cast<double>(r.deaths.size());
        fails += r.fails;
        damage += r.damage;
        stuckS += r.stuckSeconds;
        objectives += static_cast<double>(r.objectives.size());
        coverage += r.coverage;
        distanceSum += r.distance;
        quits += r.quit;
        errors += r.scriptErrors;
        if (r.completed) {
            ttg += r.timeToGoal;
            ttgN += 1;
        }
        for (const auto& d : r.deaths) causes[d.cause]++;
        allTicks.insert(allTicks.end(), r.tickMs.begin(), r.tickMs.end());
    }
    double avgTick = 0;
    for (double v : allTicks) avgTick += v;
    avgTick = allTicks.empty() ? 0 : avgTick / static_cast<double>(allTicks.size());
    double avgRender = 0;
    for (double v : renderMs) avgRender += v;
    avgRender = renderMs.empty() ? 0 : avgRender / static_cast<double>(renderMs.size());
    double estFps = std::min(1000.0, 1000.0 / std::max(0.05, avgTick + avgRender));

    Json metrics = Json::object({{"runs", n},
                                 {"completion_rate", round3(completed / n)},
                                 {"deaths", round3(deaths / n)},
                                 {"fails", round3(fails / n)},
                                 {"damage", round3(damage / n)},
                                 {"time_to_goal", ttgN > 0 ? Json(round3(ttg / ttgN)) : Json()},
                                 {"objectives", round3(objectives / n)},
                                 {"goals_total", static_cast<int64_t>(goalsForMap.size())},
                                 {"stuck_seconds", round3(stuckS / n)},
                                 {"coverage", round3(coverage / n)},
                                 {"distance", round1(distanceSum / n)},
                                 {"quit_rate", round3(quits / n)},
                                 {"script_errors", errors},
                                 {"avg_tick_ms", round3(avgTick)},
                                 {"p95_tick_ms", round3(percentile(allTicks, 0.95))},
                                 {"est_fps", round1(estFps)}});
    Json perf = Json::object({{"avg_tick_ms", round3(avgTick)},
                              {"p95_tick_ms", round3(percentile(allTicks, 0.95))},
                              {"max_tick_ms", round3(percentile(allTicks, 1.0))},
                              {"render_ms", round3(avgRender)},
                              {"est_fps", round1(estFps)},
                              {"renderer", sim.renderer().info().backend},
                              {"entities", static_cast<int64_t>(sim.scene().size())}});

    Json runsJson = Json::array();
    for (const auto& r : runs) {
        Json ds = Json::array();
        for (const auto& d : r.deaths) ds.push(Json::object({{"t", round3(d.t)}, {"position", vec3Json(d.pos)}, {"cause", d.cause}}));
        Json obj = Json::array();
        for (const auto& [t, what] : r.objectives) obj.push(Json::object({{"t", round3(t)}, {"what", what}}));
        Json st = Json::array();
        for (const auto& s : r.stuck) {
            st.push(Json::object({{"start", round3(s.start)}, {"end", round3(s.end)}, {"position", vec3Json(s.pos)}}));
        }
        Json traj = Json::array();
        for (const auto& [t, p] : r.trajectory) traj.push(Json::array({round3(t), round3(p.x), round3(p.y), round3(p.z)}));
        Json errs = Json::array();
        for (const auto& e : r.errorTexts) errs.push(e);
        runsJson.push(Json::object({{"seed", static_cast<int64_t>(r.seed)},
                                    {"completed", r.completed},
                                    {"time_to_goal", r.timeToGoal >= 0 ? Json(round3(r.timeToGoal)) : Json()},
                                    {"time_to_first_goal", r.timeToFirstGoal >= 0 ? Json(round3(r.timeToFirstGoal)) : Json()},
                                    {"duration", round3(r.duration)},
                                    {"deaths", ds},
                                    {"fails", r.fails},
                                    {"damage", r.damage},
                                    {"pickups", r.pickups},
                                    {"checkpoints", r.checkpoints},
                                    {"objectives", obj},
                                    {"stuck", st},
                                    {"stuck_seconds", round3(r.stuckSeconds)},
                                    {"quit", r.quit},
                                    {"player_lost", r.playerLost},
                                    {"distance", round1(r.distance)},
                                    {"coverage", round3(r.coverage)},
                                    {"script_errors", r.scriptErrors},
                                    {"errors", errs},
                                    {"events", r.events},
                                    {"trajectory", traj}}));
    }
    Json shotsJson = Json::array();
    for (const auto& s : shots) {
        shotsJson.push(Json::object({{"file", s.file}, {"run", s.run}, {"t", round3(s.t)}, {"reason", s.reason}}));
    }
    Json causeJson = Json::object();
    for (const auto& [k, v] : causes) causeJson[k] = v;
    Json warn = Json::array();
    for (const auto& w : warnings) warn.push(w);
    Json goalsJson = Json::array();
    for (const auto& g : goalsForMap) goalsJson.push(g.name);
    Json hazardsJson = Json::array();
    for (const auto& h : hazardsForMap) hazardsJson.push(h.name);

    // One-line summary for people and agents.
    char buf[512];
    std::string ttgText = ttgN > 0 ? " (avg " + std::to_string(static_cast<int>(std::lround(ttg / ttgN))) + " s)" : "";
    std::snprintf(buf, sizeof(buf), "%d %s run%s × %.0f s: %d/%d reached the goal%s, %.1f deaths/run, %.1f s stuck/run, sim %.2f ms/tick",
                  n, cfg.policy.c_str(), n == 1 ? "" : "s", cfg.seconds, static_cast<int>(completed), n, ttgText.c_str(), deaths / n,
                  stuckS / n, avgTick);
    std::string summary = buf;
    if (!causes.empty()) {
        std::string c;
        for (const auto& [k, v] : causes) c += (c.empty() ? "" : ", ") + k + " ×" + std::to_string(v);
        summary += " — deaths: " + c;
    }
    if (errors) summary += " — " + std::to_string(errors) + " script error(s)";
    for (const auto& w : warnings) summary += " — note: " + w;

    result.report = Json::object({{"scene", sceneName},
                                  {"config", cfg.toJson()},
                                  {"summary", summary},
                                  {"metrics", metrics},
                                  {"perf", perf},
                                  {"death_causes", causeJson},
                                  {"goals", goalsJson},
                                  {"hazards", hazardsJson},
                                  {"level_bounds", Json::object({{"min", vec3Json(levelForMap.min)}, {"max", vec3Json(levelForMap.max)}})},
                                  {"runs", runsJson},
                                  {"screenshots", shotsJson},
                                  {"heatmap", "heatmap.png"},
                                  {"warnings", warn}});
    result.report["findings"] = playtestFindings(result.report);
    result.heatmap = drawHeatmap(sim.scene(), cfg.heatmapSize, levelForMap, runs, goalsForMap, hazardsForMap, playerForMap);
    return result;
}

// ---------------------------------------------------------------------------
// Findings
// ---------------------------------------------------------------------------

Json playtestFindings(const Json& report) {
    Json out = Json::array();
    const Json& m = report.get("metrics");
    const Json& cfg = report.get("config");
    std::string policy = cfg.get("policy").asString("bot");
    double runs = std::max<double>(1, m.get("runs").asNumber(1));
    double seconds = cfg.get("seconds").asNumber();
    std::string repro = "playtest_run {policy:\"" + policy + "\", seed:" + cfg.get("seed").dump() + ", runs:" +
                        m.get("runs").dump() + ", seconds:" + cfg.get("seconds").dump() + "}";
    auto evidence = [&](Json extra) {
        Json e = Json::object({{"metrics", m}, {"repro", Json::array({repro})}});
        if (report.contains("id")) e["playtest"] = report.get("id");
        for (const auto& [k, v] : extra.members()) e[k] = v;
        return e;
    };
    // Deaths by cause.
    for (const auto& [cause, count] : report.get("death_causes").members()) {
        double perRun = count.asNumber() / runs;
        Json positions = Json::array();
        for (const auto& r : report.get("runs").elements()) {
            for (const auto& d : r.get("deaths").elements()) {
                if (d.get("cause").asString() == cause && positions.size() < 6) positions.push(d.get("position"));
            }
        }
        std::string sev = perRun >= 2 ? "high" : perRun >= 0.7 ? "medium" : "low";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Players die at %s (%.0f deaths over %.0f runs, %.1f per run)", cause.c_str(),
                      count.asNumber(), runs, perRun);
        out.push(Json::object({{"category", "difficulty"},
                               {"severity", sev},
                               {"summary", buf},
                               {"details", "The " + policy + " bot died here repeatedly. Check whether the danger is readable and "
                                           "avoidable (telegraphing, space to react, checkpoint placement)."},
                               {"target", cause},
                               {"fingerprint", "deaths:" + cause},
                               {"evidence", evidence(Json::object({{"positions", positions}}))}}));
    }
    // Completion.
    double rate = m.get("completion_rate").asNumber();
    if (m.get("goals_total").asInt() > 0 && rate < 1.0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Only %.0f%% of runs reached the goal within %.0f s", rate * 100, seconds);
        out.push(Json::object({{"category", rate == 0 ? "clarity" : "difficulty"},
                               {"severity", rate == 0 ? "high" : rate < 0.5 ? "medium" : "low"},
                               {"summary", buf},
                               {"details", "Look at the heatmap and stuck positions: is the path to the goal visible and reachable?"},
                               {"target", "goal"},
                               {"fingerprint", "completion"},
                               {"evidence", evidence(Json::object())}}));
    }
    // Stuck clusters (3 m cells).
    std::map<std::pair<int, int>, std::pair<double, Json>> cells;
    for (const auto& r : report.get("runs").elements()) {
        for (const auto& s : r.get("stuck").elements()) {
            const Json& p = s.get("position");
            auto key = std::make_pair(static_cast<int>(std::floor(p[size_t{0}].asNumber() / 3)),
                                      static_cast<int>(std::floor(p[size_t{2}].asNumber() / 3)));
            auto& cell = cells[key];
            cell.first += s.get("end").asNumber() - s.get("start").asNumber();
            cell.second = p;
        }
    }
    for (const auto& [key, val] : cells) {
        double perRun = val.first / runs;
        if (perRun < 1.5) continue;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Player gets stuck near (%.0f, %.0f) — %.1f s per run", val.second[size_t{0}].asNumber(),
                      val.second[size_t{2}].asNumber(), perRun);
        out.push(Json::object({{"category", "clarity"},
                               {"severity", perRun > 6 ? "high" : "medium"},
                               {"summary", buf},
                               {"details", "Movement input was held but the player barely moved: a snag in the geometry, a dead "
                                           "end, or an unclear route."},
                               {"target", "position " + val.second.dump()},
                               {"fingerprint", "stuck:" + std::to_string(key.first) + "," + std::to_string(key.second)},
                               {"evidence", evidence(Json::object({{"positions", Json::array({val.second})}}))}}));
    }
    // Script errors.
    if (m.get("script_errors").asInt() > 0) {
        std::string first;
        for (const auto& r : report.get("runs").elements()) {
            if (first.empty() && r.get("errors").size()) first = r.get("errors")[size_t{0}].asString();
        }
        out.push(Json::object({{"category", "bug"},
                               {"severity", "critical"},
                               {"summary", "Runtime script errors during play: " + (first.empty() ? std::string("see logs") : first)},
                               {"details", "Behaviors raised errors while playing; after 5 errors a script is disabled."},
                               {"target", first},
                               {"fingerprint", "script_errors"},
                               {"evidence", evidence(Json::object())}}));
    }
    // Quits.
    if (m.get("quit_rate").asNumber() > 0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "Bot gave up in %.0f%% of runs (no progress for %.0f s)", m.get("quit_rate").asNumber() * 100,
                      cfg.get("persona").get("patience").asNumber());
        out.push(Json::object({{"category", "fun"},
                               {"severity", "medium"},
                               {"summary", buf},
                               {"details", "A player with this patience would have quit: long stretches without visible progress."},
                               {"target", "pacing"},
                               {"fingerprint", "quit"},
                               {"evidence", evidence(Json::object())}}));
    }
    // Frame cost.
    double p95 = m.get("p95_tick_ms").asNumber();
    if (p95 > 8.0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "Simulation is heavy: p95 %.1f ms per tick (budget 8 ms)", p95);
        out.push(Json::object({{"category", "performance"},
                               {"severity", p95 > 16 ? "high" : "medium"},
                               {"summary", buf},
                               {"details", "Behaviors or effects take too long per fixed step; profile with sim_trace."},
                               {"target", "simulation"},
                               {"fingerprint", "perf:tick"},
                               {"evidence", evidence(Json::object())}}));
    }
    return out;
}


// ---------------------------------------------------------------------------
// Recording (playtest_run and loop stages)
// ---------------------------------------------------------------------------

Result<Json> runAndRecordPlaytest(Engine& engine, const Json& argsIn, const std::string& actor) {
    Studio& studio = engine.studio();
    Json args = argsIn;
    args.erase("as");
    args.erase("include_heatmap");
    // A playtester plays with its own persona unless the call overrides it.
    std::string who = args.get("agent").asString();
    args.erase("agent");
    if (who.empty()) who = studio.memberForActor(actor);
    if (const AgentProfile* p = who.empty() ? nullptr : studio.agent(who)) {
        const Json& knobs = p->playtest;
        if (!args.contains("policy") && knobs.contains("policy")) args["policy"] = knobs.get("policy");
        if (!args.contains("persona")) {
            Json persona = Json::object();
            for (const char* k : {"reaction_time", "skill", "curiosity", "patience"}) {
                if (knobs.contains(k)) persona[k] = knobs.get(k);
            }
            if (!persona.members().empty()) args["persona"] = persona;
        }
    }
    auto cfg = PlaytestConfig::fromJson(args);
    if (!cfg) return cfg.error();
    auto result = runPlaytest(engine, *cfg);
    if (!result) return result.error();

    std::string id = studio.nextPlaytestId();
    std::string dir = studio.playtestDir(id);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Json& report = result->report;
    report["id"] = id;
    report["created_at"] = studio.timestamp();
    report["by"] = who.empty() ? actor : who;
    if (!cfg->label.empty()) report["label"] = cfg->label;
    std::string rel = "studio/playtests/" + id + "/";
    // Findings reference this run and its screenshots.
    Json captures = Json::array();
    for (const auto& s : report.get("screenshots").elements()) captures.push(rel + s.get("file").asString());
    captures.push(rel + "heatmap.png");
    Json findings = playtestFindings(report);
    for (auto& f : findings.elements()) f["evidence"]["captures"] = captures;
    report["findings"] = findings;
    report["files"] = Json::object({{"report", rel + "report.json"}, {"heatmap", rel + "heatmap.png"}});
    if (Status s = writePng(result->heatmap, dir + "/heatmap.png"); !s) return s.error();
    for (const auto& [name, img] : result->shots) (void)writePng(img, dir + "/" + name);
    {
        std::ofstream f(dir + "/report.json");
        f << report.dump(2) << "\n";
        if (!f) return Error::make("io_error", "cannot write " + dir + "/report.json");
    }
    studio.notePlaytest(id, report.get("metrics"), actor, report.get("summary").asString());
    return report;
}

}  // namespace sky::studio
