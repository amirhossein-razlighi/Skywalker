// 2D particle tools: pixel-art weather and ambient life from presets, and live emitter stats.

#include <algorithm>
#include <cctype>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render2d/Particles2D.h"

namespace sky::tools {

using namespace schema;

void addParticles2DTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"particles2d_create", "Create 2D particles",
             "Create pixel-art particles for a 2D game from a preset, ready to tweak: rain (streaks over the whole view), "
             "drizzle, snow (drifting flakes), leaves and petals (falling, swaying), fireflies (glowing, twinkling, wandering), "
             "smoke (chimney puffs), ripples (puddle rings while it rains), dust (motes in sunlight), sparkle (water glints). "
             "Weather presets (rain, drizzle, snow, ripples) wrap: their box tiles endlessly so they fill whatever the camera "
             "shows, wherever the entity is. Untextured particles are solid pixel rectangles (`pixelSize` texels); give a "
             "`texture` sheet (with `columns`/`rows`, `frames`, `animate` random|life|loop) for leaves, flakes or ripple rings. "
             "`overrides` patches fields, e.g. {\"rate\": 400, \"color\": \"#9fc4ff\", \"pixelsPerUnit\": 16}. Simulated on fixed "
             "ticks while playing (deterministic, reset on stop). Example: particles2d_create {\"preset\": \"fireflies\", "
             "\"position\": [12, 4, 0], \"overrides\": {\"area\": [14, 6]}}.",
             "render",
             object({{"preset", enumeration(render2d::particles2dPresets(), "Preset")},
                     {"name", string("Entity name (default: the preset's name)")},
                     {"position", vec3("World position (center of the emission box)")},
                     {"parent", entity("Parent entity (e.g. a chimney)")},
                     {"overrides", Json::object({{"type", "object"}, {"description", "particles2d fields to change"}})}},
                    {"preset"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 const std::string preset = a.get("preset").asString();
                 Json fields = render2d::particles2dPreset(preset);
                 if (fields.isNull()) {
                     std::string guess = str::closest(preset, render2d::particles2dPresets());
                     return fail(Error::make("unknown_preset", "no 2D particle preset '" + preset + "'",
                                             guess.empty() ? "presets: rain, drizzle, snow, leaves, petals, fireflies, smoke, ripples, dust, sparkle"
                                                           : "did you mean '" + guess + "'?"));
                 }
                 for (const auto& [k, v] : a.get("overrides").members()) fields[k] = v;
                 std::string name = a.get("name").asString();
                 if (name.empty()) {
                     name = preset;
                     name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                 }
                 EntityId id = kNoEntity;
                 Status st = engine.edit(ctx.actor, "2D particles: " + preset, [&]() -> Status {
                     EntityId parent = kNoEntity;
                     if (a.contains("parent")) {
                         auto p = resolve(engine, a.get("parent"));
                         if (!p) return p.error();
                         parent = *p;
                     }
                     Scene& s = engine.scene();
                     id = s.create(name, parent);
                     if (a.contains("position")) {
                         if (Status r = s.patchComponent(id, "transform", Json::object({{"position", a.get("position")}})); !r) return r;
                     }
                     return s.patchComponent(id, "particles2d", fields);
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"entity", id}, {"name", name}, {"preset", preset}}),
                                         "created 2D " + preset + " #" + std::to_string(id));
             }});

    reg.add({"particles2d_info", "2D particle stats",
             "Live state of particles2d emitters: particles alive, cap, rate, box, wrap, and the bounds the live particles "
             "cover (world units). Use it to check that weather is running while playing, or to find emitters that hit "
             "their maxParticles cap. Without `entity` lists every emitter in the scene.",
             "render", object({{"entity", entity("Emitter (default: all)")}}), false, false, [&engine](const Json& a, ToolContext&) {
                 const Scene& s = engine.scene();
                 std::vector<EntityId> ids;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     if (!s.get<Particles2D>(*id)) return fail(Error::make("no_emitter", "that entity has no particles2d component",
                                                                           "create one with particles2d_create"));
                     ids.push_back(*id);
                 } else {
                     for (EntityId e : s.entities()) {
                         if (s.get<Particles2D>(e)) ids.push_back(e);
                     }
                 }
                 Json list = Json::array();
                 size_t total = 0;
                 for (EntityId e : ids) {
                     const Particles2D& p = *s.get<Particles2D>(e);
                     float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                     for (const auto& q : p.particles_) {
                         x0 = std::min(x0, q.pos.x);
                         y0 = std::min(y0, q.pos.y);
                         x1 = std::max(x1, q.pos.x);
                         y1 = std::max(y1, q.pos.y);
                     }
                     total += p.particles_.size();
                     Json j = Json::object({{"entity", e},
                                            {"name", s.record(e)->name},
                                            {"alive", static_cast<int64_t>(p.particles_.size())},
                                            {"maxParticles", p.maxParticles},
                                            {"atCap", static_cast<int>(p.particles_.size()) >= p.maxParticles},
                                            {"emitting", p.emitting},
                                            {"rate", p.rate},
                                            {"wrap", p.wrap},
                                            {"area", Json::array({p.area.x, p.area.y})},
                                            {"running", p.started_}});
                     if (!p.particles_.empty()) {
                         j["bounds"] = Json::array({x0, y0, x1, y1});
                         if (p.wrap) j["boundsSpace"] = "box-local";
                     }
                     list.push(std::move(j));
                 }
                 return ToolResult::json(Json::object({{"emitters", list}, {"alive", static_cast<int64_t>(total)}}),
                                         std::to_string(ids.size()) + " emitter(s), " + std::to_string(total) + " particles alive");
             }});
}

}  // namespace sky::tools
