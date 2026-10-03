// Effects tools: particle effects (fire, smoke, rain, explosions...) and FFT water from
// presets, bursts, and wave-height queries for placing boats and floating things.

#include <algorithm>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/fx/Particles.h"

namespace sky::tools {

namespace {

using namespace schema;

std::vector<std::string> allEffects() {
    std::vector<std::string> out = fx::particlePresets();
    for (const auto& f : fx::fluidPresets()) out.push_back(f);
    for (const auto& c : fx::compositeEffects()) out.push_back(c);
    for (const auto& w : fx::waterPresets()) out.push_back(w);
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

void addFxTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"fx_create", "Create effect",
             "Create an effect or a body of water from a preset, ready to tweak. Volumetric fluid simulations (GPU, "
             "ray-marched, film-quality fire and smoke): volume_fire, volume_torch, volume_smoke, steam_vent, "
             "explosion_volume. Particles: fire (sprite), embers, smoke, steam, sparks, rain, snow, mist, spray, dust, "
             "fireflies, magic, fireball, debris_smoke, shrapnel. GPU particles (millions, depth collisions, sub-emitters, "
             "ribbons, mesh particles; visuals only): sparks_shower, fireworks, rain_heavy, waterfall_mist (composites), "
             "ember_storm, magic_vortex, dust_storm, falling_leaves (~6 m up), snow_heavy (~12 m up), smoke_column_gpu. "
             "Composites (volumetric fire + embers + light): "
             "campfire, torch, burning_barrel, explosion; sprite_campfire is the cheap particle version. Water (FFT ocean "
             "simulation): ocean, calm_sea, storm, lake, pool, puddle — the entity's y is the water level. `overrides` patches "
             "the particles/water fields (e.g. {\"rate\": 80, \"colorStart\": \"#7af\"}). Fires light the scene by "
             "themselves; rain/snow cover a box around the entity (place it ~12 m up), set floorHeight to the ground.",
             "render",
             object({{"effect", enumeration(allEffects(), "Preset")},
                     {"name", string("Entity name (default: the preset's name)")},
                     {"position", vec3("World position")},
                     {"parent", entity("Parent entity")},
                     {"overrides", Json::object({{"type", "object"},
                                                 {"description", "Fields to change on the particles/water component "
                                                                 "(composites: applied to every emitter)"}})}},
                    {"effect"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::string effect = a.get("effect").asString();
                 std::string name = a.get("name").asString();
                 if (name.empty()) {
                     name = effect;
                     std::replace(name.begin(), name.end(), '_', ' ');
                     if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                 }
                 const Json& overrides = a.get("overrides");
                 EntityId root = kNoEntity;
                 Json children = Json::array();
                 Status st = engine.edit(ctx.actor, "Effect: " + effect, [&]() -> Status {
                     EntityId parent = kNoEntity;
                     if (a.contains("parent")) {
                         auto p = resolve(engine, a.get("parent"));
                         if (!p) return p.error();
                         parent = *p;
                     }
                     Scene& s = engine.scene();
                     root = s.create(name, parent);
                     if (a.contains("position")) {
                         if (Status r = s.patchComponent(root, "transform", Json::object({{"position", a.get("position")}})); !r) return r;
                     }
                     auto withOverrides = [&](Json patch) {
                         for (const auto& [k, v] : overrides.members()) patch[k] = v;
                         return patch;
                     };
                     if (contains(fx::waterPresets(), effect)) {
                         return s.patchComponent(root, "water", withOverrides(fx::waterPreset(effect)));
                     }
                     if (contains(fx::fluidPresets(), effect)) {
                         return s.patchComponent(root, "fluid", withOverrides(fx::fluidPreset(effect)));
                     }
                     if (contains(fx::particlePresets(), effect)) {
                         return s.patchComponent(root, "particles", withOverrides(fx::particlePreset(effect)));
                     }
                     if (contains(fx::compositeEffects(), effect)) {
                         const Json parts = fx::compositeEffect(effect);
                         for (const auto& item : parts.elements()) {
                             EntityId c = s.create(item.get("name").asString(), root);
                             if (Status r = s.patchComponent(c, "transform", Json::object({{"position", item.get("position")}})); !r) {
                                 return r;
                             }
                             if (item.contains("fluid")) {
                                 if (Status r = s.patchComponent(c, "fluid", item.get("fluid")); !r) return r;
                             } else if (Status r = s.patchComponent(c, "particles", withOverrides(item.get("particles"))); !r) {
                                 return r;
                             }
                             children.push(Json::object({{"id", c}, {"name", item.get("name")}}));
                         }
                         return {};
                     }
                     std::string guess = str::closest(effect, allEffects());
                     return Error::make("unknown_effect", "no effect '" + effect + "'", guess.empty() ? "" : "did you mean '" + guess + "'?");
                 });
                 if (!st) return fail(st);
                 Json r = Json::object({{"entity", root}, {"name", name}, {"effect", effect}});
                 if (children.size()) r["children"] = children;
                 return ToolResult::json(r, "created " + effect + " #" + std::to_string(root));
             }});

    reg.add({"fx_burst", "Burst particles",
             "Emit particles from an emitter right now (explosions, muzzle flashes, impacts). Works while editing "
             "(preview) and playing. In Wander use burst(n).",
             "render",
             object({{"entity", entity("Entity with a particles component (or a composite whose children have one)")},
                     {"count", integer("Particles to emit (default: the emitter's burst field, else 50)")}},
                    {"entity"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 std::vector<EntityId> targets;
                 collectSubtree(engine.scene(), *id, targets);
                 int fired = 0;
                 for (EntityId e : targets) {
                     const ParticleEmitter* em = engine.scene().get<ParticleEmitter>(e);
                     if (!em) continue;
                     int n = static_cast<int>(a.get("count").asInt(em->burst > 0 ? em->burst : 50));
                     engine.particles().burst(e, n);
                     ++fired;
                 }
                 if (!fired) return ToolResult::error(Error::make("no_emitter", "no particles component on that entity or its children"));
                 return ToolResult::json(Json::object({{"emitters", fired}}), "burst queued on " + std::to_string(fired) + " emitter(s)");
             }});

    reg.add({"water_query", "Water height",
             "Height and normal of the animated water surface (FFT ocean) at points — for floating boats, buoys and "
             "debris, or placing a pier above the waves. Uses the current effects time.",
             "world",
             object({{"points", array(Json::object({{"type", "array"}, {"items", Json::object({{"type", "number"}})}}),
                                      "Points as [x, z] pairs")}},
                    {"points"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Json out = Json::array();
                 for (const auto& p : a.get("points").elements()) {
                     float x = p[size_t{0}].asFloat(), z = p[size_t{1}].asFloat(), h = 0;
                     Vec3 n;
                     if (engine.waterHeight(x, z, h, &n)) {
                         out.push(Json::object({{"x", x}, {"z", z}, {"height", h}, {"normal", reflect::vec3ToJson(n)}}));
                     } else {
                         out.push(Json::object({{"x", x}, {"z", z}, {"water", false}}));
                     }
                 }
                 return ToolResult::json(Json::object({{"points", out}, {"time", engine.effectsTime()}}), "water heights");
             }});
}

}  // namespace sky::tools
