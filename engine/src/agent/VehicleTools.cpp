// Vehicle tools: build a drivable car from a chassis mesh and wheel children (vehicle_create),
// tune it with validation (vehicle_tune), watch its live telemetry (vehicle_info) and measure the
// handling with an autopilot (vehicle_test_drive). See docs/PHYSICS.md "Vehicles".

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/audio/Synth.h"
#include "skywalker/audio/Wav.h"
#include "skywalker/core/Strings.h"
#include "skywalker/physics/DebugDraw.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/physics/Vehicle.h"
#include "skywalker/render/DebugViews.h"
#include "skywalker/render/MeshData.h"

namespace sky::tools {

namespace {

using namespace schema;

float r3(float x) { return std::round(x * 1000.f) / 1000.f; }
Json v3(Vec3 v) { return Json::array({r3(v.x), r3(v.y), r3(v.z)}); }

/// Mesh bounds of a subtree in the chassis entity's local space; `wheels` selects the wheel visuals
/// (true) or everything else (false).
std::optional<Aabb> chassisBounds(Engine& engine, EntityId vehicle, bool wheels) {
    const Scene& s = engine.scene();
    Mat4 toLocal = s.worldMatrix(vehicle).inverse();
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    bool any = false;
    std::vector<std::pair<EntityId, bool>> stack{{vehicle, false}};
    while (!stack.empty()) {
        auto [e, inWheel] = stack.back();
        stack.pop_back();
        bool wheel = inWheel || (e != vehicle && physics::isWheelVisual(s, vehicle, e));
        if (const MeshRenderer* m = s.get<MeshRenderer>(e); m && wheel == wheels) {
            const MeshData* md = str::startsWith(m->mesh, "asset:") ? engine.cpuMesh(m->mesh) : nullptr;
            Aabb b = (md ? md->bounds : s.localBounds(e)).transformed(s.worldMatrix(e));
            for (int i = 0; i < 8; ++i) {
                Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
                Vec3 p = toLocal.transformPoint(c);
                box.min = vmin(box.min, p);
                box.max = vmax(box.max, p);
                any = true;
            }
        }
        for (EntityId c : s.children(e)) {
            if (s.get<RigidBody>(c)) continue;
            stack.push_back({c, wheel});
        }
    }
    if (!any) return std::nullopt;
    return box;
}

std::vector<EntityId> wheelVisuals(const Scene& s, EntityId vehicle) {
    std::vector<EntityId> out;
    std::vector<EntityId> stack;
    for (EntityId c : s.children(vehicle)) stack.push_back(c);
    while (!stack.empty()) {
        EntityId e = stack.back();
        stack.pop_back();
        if (physics::isWheelVisual(s, vehicle, e)) {
            out.push_back(e);
            continue;
        }
        for (EntityId c : s.children(e)) stack.push_back(c);
    }
    return out;
}

/// Problems the physics world reported about this vehicle (by name).
Json vehicleWarnings(Engine& engine, EntityId e) {
    Json out = Json::array();
    const EntityRecord* r = engine.scene().record(e);
    if (!r) return out;
    for (const auto& w : engine.physics().recentWarnings()) {
        if (w.find("'" + r->name + "'") != std::string::npos) out.push(w);
    }
    return out;
}

physics::PhysicsWorld& liveWorld(Engine& engine) { return engine.physics().queryWorld(); }

Json setupJson(const Vehicle& v) {
    return Json::object({{"preset", v.preset},
                         {"drive", v.drive},
                         {"mass", v.mass},
                         {"maxTorque", v.maxTorque},
                         {"transmission", v.transmission},
                         {"control", v.control},
                         {"assists", Json::object({{"tractionControl", v.tractionControl},
                                                   {"abs", v.abs},
                                                   {"driftAssist", v.driftAssist}})}});
}

/// Semantic checks beyond the field ranges (values are clamped at runtime; agents should know).
Json tuneWarnings(const Vehicle& v) {
    Json w = Json::array();
    if (v.shiftUpRpm >= v.maxRpm) w.push("shiftUpRpm (" + std::to_string(static_cast<int>(v.shiftUpRpm)) + ") must be below maxRpm: clamped");
    if (v.shiftDownRpm >= v.shiftUpRpm) w.push("shiftDownRpm must be below shiftUpRpm: clamped");
    if (v.minRpm >= v.maxRpm) w.push("minRpm must be below maxRpm");
    if (v.suspensionMinLength >= v.suspensionMaxLength) w.push("suspensionMinLength must be below suspensionMaxLength");
    if (v.gearRatios.isArray()) {
        for (const auto& r : v.gearRatios.elements()) {
            if (!r.isNumber() || r.asFloat(0.f) <= 0.f) w.push("gearRatios must be positive numbers (bad entries are skipped)");
        }
    }
    const std::pair<const char*, const Json*> curves[] = {
        {"longitudinalCurve", &v.longitudinalCurve}, {"lateralCurve", &v.lateralCurve}, {"torqueCurve", &v.torqueCurve}};
    for (const auto& [name, c] : curves) {
        if (c->isArray() && c->size() == 1) w.push(std::string(name) + " needs at least 2 points (the default curve is used)");
    }
    if (v.drive == "awd" && (v.frontTorqueSplit <= 0.f || v.frontTorqueSplit >= 1.f)) {
        w.push("frontTorqueSplit 0 or 1 makes the AWD car effectively RWD/FWD");
    }
    if (v.maxTorque <= 0.f) w.push("maxTorque is 0: the car cannot accelerate");
    if (v.antiRollFront > 4000.f || v.antiRollRear > 4000.f) w.push("anti-roll bars above ~4000 can make the body shake at rest");
    return w;
}

Result<std::string> ensureEngineSound(Engine& engine) {
    const std::string rel = "audio/engine_loop.wav";
    const std::string abs = engine.resolvePath(rel);
    std::error_code ec;
    if (std::filesystem::exists(abs, ec)) return rel;
    auto params = audio::sfxPreset("engine_loop", 1);
    if (!params) return params.error();
    auto pcm = audio::renderSfx(*params);
    if (!pcm) return pcm.error();
    std::filesystem::create_directories(std::filesystem::path(abs).parent_path(), ec);
    if (Status s = audio::writeWav(abs, *pcm); !s) return s.error();
    engine.audio().invalidate(rel);
    return rel;
}

physics::TestDriveOptions driveOptions(const Json& a) {
    physics::TestDriveOptions o;
    o.maneuver = a.get("maneuver").asString("all");
    o.track = a.get("track").asString("proving_ground");
    o.speedKmh = a.get("speed").asFloat(0.f);
    o.duration = a.get("duration").asFloat(0.f);
    o.radius = a.get("radius").asFloat(40.f);
    o.coneSpacing = a.get("cone_spacing").asFloat(18.f);
    o.inputs = a.get("inputs");
    o.overrides = a.get("overrides");
    o.trace = a.get("trace").asBool(false);
    return o;
}

std::string summaryText(const Json& summary) {
    std::ostringstream os;
    auto num = [&](const char* key, const char* label, const char* unit) {
        const Json& v = summary.get(key);
        if (v.isNumber()) os << label << " " << v.asFloat(0.f) << unit << "; ";
    };
    num("zeroTo100s", "0-100 km/h", " s");
    num("brakingDistanceM", "braking from 100", " m");
    num("slalomAvgKmh", "slalom avg", " km/h");
    num("skidpadLateralG", "skidpad", " g");
    num("maxLateralG", "max lateral", " g");
    num("topSpeedKmh", "top speed", " km/h");
    std::string s = os.str();
    if (s.size() > 2) s.resize(s.size() - 2);
    return s;
}

ToolResult captureVehicle(Engine& engine, EntityId e, Json result, const std::string& text, int width, int height) {
    CaptureOptions o;
    o.width = width;
    o.height = height;
    o.samples = 1;
    o.editorOverlays = false;
    o.debugView = debugview::kVehicles;
    const Scene& s = engine.scene();
    Mat4 w = s.worldMatrix(e);
    Vec3 c = w.translation();
    Vec3 fwd = normalize(w.transformDir({0.f, 0.f, -1.f}));
    Vec3 right = normalize(w.transformDir({1.f, 0.f, 0.f}));
    o.hasCustomView = true;
    o.customView = engine.camera().toView();
    o.customView.target = c + Vec3{0.f, 0.4f, 0.f};
    o.customView.eye = c + right * 4.5f + fwd * 3.f + Vec3{0.f, 2.2f, 0.f};
    auto cap = engine.capture(o);
    ToolResult r = ToolResult::json(std::move(result), text);
    if (!cap) return r;
    std::vector<uint8_t> png = encodePng(cap->image);
    r.image(str::base64Encode(png.data(), png.size()));
    return r;
}

}  // namespace

void addVehicleTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"vehicle_create", "Create vehicle",
             "Make a drivable car, truck or kart from a chassis entity whose wheel meshes are child entities named wheel* "
             "(wheel_fl, wheel_fr, wheel_rl, wheel_rr; or pass `wheels`). One undoable edit that adds: a dynamic body "
             "(chassis mass), a box collider fitted to the chassis above the wheels, the `vehicle` component from a preset "
             "(sports, hatchback, truck, kart) in a handling style (arcade = traction control, ABS, drift assist, roll "
             "protection; sim = raw), a chase camera on the scene camera, a looping engine sound pitched by rpm, and the "
             "drive input actions (throttle W/Up/right trigger, brake+reverse S/Down/left trigger, steer A-D/arrows/left "
             "stick, handbrake Space/A). Forward is -Z. Wheels are fitted from their meshes and spin, steer and follow the "
             "suspension while playing. Then: vehicle_test_drive for numbers, vehicle_tune to adjust, vehicle_info "
             "capture=true to see suspension and tire forces. Example: {\"entity\": \"Coupe\", \"preset\": \"sports\"}.",
             "physics",
             object({{"entity", schema::entity("The chassis (root of the car model)")},
                     {"preset", enumeration(physics::vehiclePresetNames(), "Vehicle type (default sports)")},
                     {"handling", enumeration(physics::vehicleHandlingNames(), "arcade (assists, default) or sim")},
                     {"wheels", array(schema::entity(), "Wheel entities (children of the chassis); default: children named wheel*")},
                     {"overrides", Json::object({{"type", "object"}, {"description", "vehicle component fields to set on top of the preset"}})},
                     {"chase_camera", boolean("Make the scene camera a chase camera following this vehicle (default true)")},
                     {"engine_sound", boolean("Add a looping engine sound pitched by rpm (default true)")},
                     {"input_actions", boolean("Add the drive input actions to the project's input map if missing (default true)")},
                     {"collider", enumeration({"auto", "keep"}, "auto = box fitted to the chassis above the wheels (default); keep = your own")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 const EntityId e = *id;
                 Scene& s = engine.scene();
                 const std::string preset = a.get("preset").asString("sports");
                 auto patch = physics::vehiclePreset(preset, a.get("handling").asString("arcade"));
                 if (!patch) return ToolResult::error(patch.error());
                 if (a.get("overrides").isObject()) {
                     for (const auto& [k, v] : a.get("overrides").members()) (*patch)[k] = v;
                 }
                 Json wheelEntries = Json::array();
                 for (const auto& w : a.get("wheels").elements()) {
                     auto wid = resolve(engine, w);
                     if (!wid) return ToolResult::error(wid.error());
                     bool under = false;
                     for (const EntityRecord* r = s.record(*wid); r && r->parent; r = s.record(r->parent)) under = under || r->parent == e;
                     if (!under) {
                         return ToolResult::error(Error::make("invalid_arguments", "wheel '" + s.record(*wid)->name + "' is not a child of '" +
                                                                                       s.record(e)->name + "'",
                                                              "parent the wheel meshes under the chassis entity first (entity_update parent)"));
                     }
                     wheelEntries.push(Json::object({{"entity", s.record(*wid)->name}}));
                 }
                 if (wheelEntries.size() > 0) (*patch)["wheels"] = wheelEntries;
                 Json notes = Json::array();
                 Json result = Json::object();
                 Status st = engine.edit(ctx.actor, "Create vehicle", [&]() -> Status {
                     if (Status x = s.patchComponent(e, "vehicle", *patch); !x) return x;
                     if (s.get<CharacterController>(e)) {
                         if (Status x = s.patchComponent(e, "character", Json()); !x) return x;
                     }
                     auto wheels = wheelVisuals(s, e);
                     if (wheels.empty()) {
                         return Error::make("no_wheels", "'" + s.record(e)->name + "' has no wheel children",
                                            "name the wheel meshes wheel_fl, wheel_fr, wheel_rl, wheel_rr (children of the chassis) "
                                            "or pass `wheels`");
                     }
                     if (wheels.size() < 3) notes.push("only " + std::to_string(wheels.size()) + " wheel(s) found: most vehicles need 3+");
                     const Vehicle* v = s.get<Vehicle>(e);
                     Json body = Json::object({{"motion", "dynamic"},
                                               {"mass", v->mass},
                                               {"linearDamping", 0.0},
                                               {"angularDamping", 0.05},
                                               {"friction", 0.4},
                                               {"startAwake", true}});
                     if (Status x = s.patchComponent(e, "body", body); !x) return x;
                     if (a.get("collider").asString("auto") == "auto") {
                         auto chassis = chassisBounds(engine, e, false);
                         auto wheelBox = chassisBounds(engine, e, true);
                         if (!chassis) return Error::make("no_chassis", "the chassis has no mesh (only wheels)", "add the body mesh to the chassis entity or a child");
                         Aabb box = *chassis;
                         float minRadius = wheelBox ? std::max(0.05f, (wheelBox->max.y - wheelBox->min.y) * 0.5f) : 0.3f;
                         Vec3 scale = s.get<Transform>(e) ? s.get<Transform>(e)->scale : Vec3(1.f);
                         float clearance = std::min(0.45f * minRadius, 0.2f) / std::max(std::fabs(scale.y), 1e-3f);
                         if (wheelBox) box.min.y = std::max(box.min.y, wheelBox->min.y + clearance);
                         if (box.max.y - box.min.y < 0.1f) box.min.y = box.max.y - 0.1f;
                         Json col = Json::object({{"shape", "box"}, {"size", v3(box.max - box.min)}, {"offset", v3(box.center())},
                                                  {"isTrigger", false}, {"rotation", Json::array({0, 0, 0})}});
                         if (Status x = s.patchComponent(e, "collider", col); !x) return x;
                         result["collider"] = col;
                     }
                     // Wheel visuals never carry colliders of their own (they would join the chassis).
                     for (EntityId w : wheels) {
                         if (s.get<Collider>(w)) {
                             if (Status x = s.patchComponent(w, "collider", Json()); !x) return x;
                         }
                     }
                     if (a.get("chase_camera").asBool(true)) {
                         EntityId cam = kNoEntity;
                         bool taken = false;
                         for (EntityId c : s.entities()) {
                             if (const ChaseCamera* cc = s.get<ChaseCamera>(c)) {
                                 if (s.resolve(cc->target, c) != e) taken = true;
                                 else cam = c;
                             }
                         }
                         if (cam == kNoEntity && !taken) {
                             for (EntityId c : s.entities()) {
                                 if (const Camera* cm = s.get<Camera>(c); cm && cm->primary && s.isActive(c)) {
                                     cam = c;
                                     break;
                                 }
                             }
                             if (cam == kNoEntity) {
                                 cam = s.create("Chase Camera");
                                 s.add<Transform>(cam);
                                 if (Status x = s.patchComponent(cam, "camera", Json::object({{"primary", true}})); !x) return x;
                             }
                         }
                         if (cam != kNoEntity) {
                             Json cc = physics::chaseCameraPreset(preset);
                             cc["target"] = Json::object({{"id", static_cast<int64_t>(e)}, {"name", s.record(e)->name}});
                             if (Status x = s.patchComponent(cam, "chase_camera", cc); !x) return x;
                             result["camera"] = s.record(cam)->name;
                         } else {
                             notes.push("another vehicle already owns the chase camera; none added");
                         }
                     }
                     if (a.get("engine_sound").asBool(true)) {
                         const AudioSource* existing = s.get<AudioSource>(e);
                         if (!existing || existing->clip.empty()) {
                             auto clip = ensureEngineSound(engine);
                             if (!clip) {
                                 notes.push("engine sound not generated: " + clip.error().message);
                             } else {
                                 Json audio = Json::object({{"clip", *clip}, {"loop", true}, {"playOnStart", true}, {"spatial", true},
                                                            {"volume", 0.6}, {"minDistance", 3}, {"maxDistance", 80}, {"doppler", 0.4}});
                                 if (Status x = s.patchComponent(e, "audio", audio); !x) return x;
                                 result["engineSound"] = *clip;
                             }
                         }
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 if (a.get("input_actions").asBool(true)) {
                     input::ActionMap map = engine.actionMap();
                     Json added = Json::array();
                     for (auto& action : physics::driveActions()) {
                         if (map.find(action.name)) continue;
                         added.push(action.name);
                         map.set(std::move(action));
                     }
                     if (added.size() > 0) {
                         if (Status x = engine.setActionMap(std::move(map)); !x) notes.push("input actions not saved: " + x.error().message);
                         else result["inputActionsAdded"] = added;
                     }
                 }
                 // Fit check: the edit-time world builds the vehicle exactly as play will.
                 physics::PhysicsWorld& world = liveWorld(engine);
                 auto t = world.vehicle(e);
                 Json warnings = vehicleWarnings(engine, e);
                 for (const auto& n : notes.elements()) warnings.push(n);
                 result["entity"] = static_cast<int64_t>(e);
                 result["vehicle"] = setupJson(*s.get<Vehicle>(e));
                 if (t) {
                     Json wheels = Json::array();
                     for (const auto& w : t->wheels) {
                         wheels.push(Json::object({{"name", w.name},
                                                   {"visual", w.visual && s.record(w.visual) ? Json(s.record(w.visual)->name) : Json()},
                                                   {"radius", r3(w.radius)},
                                                   {"width", r3(w.width)},
                                                   {"steers", w.steers},
                                                   {"driven", w.driven},
                                                   {"handbrake", w.handbrake}}));
                     }
                     result["wheels"] = wheels;
                 } else {
                     warnings.push("the vehicle could not be built yet (see the other warnings)");
                 }
                 result["warnings"] = warnings;
                 result["next"] = "vehicle_test_drive {\"entity\": \"" + s.record(e)->name +
                                  "\"} for 0-100, braking, slalom and skidpad numbers; vehicle_info capture=true to see it";
                 std::string text = "'" + s.record(e)->name + "' is now a " + preset + " (" +
                                    std::to_string(t ? t->wheels.size() : 0) + " wheels, " + s.get<Vehicle>(e)->drive + ")";
                 if (warnings.size() > 0) text += "; " + std::to_string(warnings.size()) + " warning(s)";
                 return ToolResult::json(result, text);
             }});

    reg.add({"vehicle_tune", "Tune vehicle",
             "Change a vehicle's handling parameters with validation (one undoable edit; works while playing: the "
             "vehicle is rebuilt keeping its speed). `set` takes vehicle component fields, e.g. {\"maxTorque\": 600, "
             "\"lateralGrip\": 1.6, \"antiRollRear\": 12000, \"driftAssist\": 0.5}; unknown fields fail with a "
             "did-you-mean. `preset` + `handling` re-apply a preset first. `wheels` overrides per axle or wheel "
             "({\"rear\": {\"lateralGrip\": 1.2}, \"front_left\": {...}}): the wheel list is written out from the current "
             "fit the first time. `test` (a maneuver: accel, braking, slalom, skidpad, all) drives before and after the "
             "change and returns both summaries, so you can tune by numbers. Run vehicle_test_drive for details.",
             "physics",
             object({{"entity", schema::entity("The vehicle")},
                     {"set", Json::object({{"type", "object"}, {"description", "vehicle fields to change"}})},
                     {"preset", enumeration(physics::vehiclePresetNames(), "Re-apply a preset (before `set`)")},
                     {"handling", enumeration(physics::vehicleHandlingNames(), "Handling style for `preset` (default arcade)")},
                     {"wheels", Json::object({{"type", "object"},
                                              {"description", "Per-wheel overrides keyed by axle (front, rear) or wheel name "
                                                              "(front_left, rear_right, axle1_left): {lateralGrip, longitudinalGrip, "
                                                              "suspensionFrequency, suspensionDamping, brakeTorque, steer, drive, ...}"}})},
                     {"test", enumeration({"accel", "braking", "slalom", "skidpad", "top_speed", "all"},
                                          "Measure this maneuver before and after the change")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 const EntityId e = *id;
                 Scene& s = engine.scene();
                 if (!s.get<Vehicle>(e)) {
                     return ToolResult::error(Error::make("not_a_vehicle", "'" + s.record(e)->name + "' has no vehicle component",
                                                          "create one with vehicle_create"));
                 }
                 const std::string test = a.get("test").asString();
                 Json before;
                 if (!test.empty()) {
                     physics::TestDriveOptions o;
                     o.maneuver = test;
                     auto r = physics::runTestDrive(engine.physics(), s, e, o);
                     if (!r) return ToolResult::error(r.error());
                     before = r->get("summary");
                 }
                 Json oldJson = s.componentKind("vehicle")->toJson(s, e);
                 Json wheelPatch = a.get("wheels");
                 Status st = engine.edit(ctx.actor, "Tune vehicle", [&]() -> Status {
                     if (a.contains("preset")) {
                         auto p = physics::vehiclePreset(a.get("preset").asString(), a.get("handling").asString("arcade"));
                         if (!p) return p.error();
                         if (Status x = s.patchComponent(e, "vehicle", *p); !x) return x;
                         if (Status x = s.patchComponent(e, "body", Json::object({{"mass", p->get("mass")}})); !x) return x;
                     }
                     if (a.get("set").isObject()) {
                         if (Status x = s.patchComponent(e, "vehicle", a.get("set")); !x) return x;
                         if (a.get("set").contains("mass")) {
                             if (Status x = s.patchComponent(e, "body", Json::object({{"mass", a.get("set").get("mass")}})); !x) return x;
                         }
                     } else if (a.contains("set")) {
                         return Error::make("invalid_arguments", "`set` must be an object of vehicle fields");
                     }
                     if (wheelPatch.isObject() && wheelPatch.size() > 0) {
                         // Materialize the fitted wheels once, then merge the overrides by axle or name.
                         auto t = liveWorld(engine).vehicle(e);
                         if (!t) return Error::make("not_built", "the vehicle has no fitted wheels yet", "check vehicle_info warnings");
                         Vehicle* v = s.get<Vehicle>(e);
                         Json entries = v->wheels.isArray() && v->wheels.size() == t->wheels.size() ? v->wheels : Json::array();
                         Mat4 toLocal = s.worldMatrix(e).inverse();
                         if (entries.size() == 0) {
                             for (const auto& w : t->wheels) {
                                 Json entry = Json::object({{"position", v3(toLocal.transformPoint(w.mount + w.down * w.restLength))},
                                                            {"radius", r3(w.radius)},
                                                            {"width", r3(w.width)}});
                                 if (w.visual && s.record(w.visual)) entry["entity"] = s.record(w.visual)->name;
                                 entries.push(entry);
                             }
                         }
                         std::vector<std::string> names;
                         for (const auto& w : t->wheels) names.push_back(w.name);
                         Json out = Json::array();
                         for (size_t i = 0; i < entries.size(); ++i) {
                             Json entry = entries[i];
                             const std::string& name = t->wheels[i].name;
                             for (const auto& [key, fields] : wheelPatch.members()) {
                                 bool match = key == name || str::startsWith(name, key + "_");
                                 if (!match) continue;
                                 for (const auto& [f, val] : fields.members()) entry[f] = val;
                             }
                             out.push(entry);
                         }
                         for (const auto& [key, fields] : wheelPatch.members()) {
                             bool any = false;
                             for (const auto& n : names) any = any || n == key || str::startsWith(n, key + "_");
                             if (!any) {
                                 std::vector<std::string> keys = names;
                                 keys.push_back("front");
                                 keys.push_back("rear");
                                 std::string guess = str::closest(key, keys, 3);
                                 return Error::make("unknown_wheel", "no wheel or axle named \"" + key + "\"",
                                                    (guess.empty() ? std::string() : "did you mean \"" + guess + "\"? ") + "wheels: " +
                                                        [&] {
                                                            std::string l;
                                                            for (const auto& n : names) l += (l.empty() ? "" : ", ") + n;
                                                            return l;
                                                        }());
                             }
                             if (!fields.isObject()) return Error::make("invalid_arguments", "wheel overrides must be objects");
                         }
                         if (Status x = s.patchComponent(e, "vehicle", Json::object({{"wheels", out}})); !x) return x;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 Json newJson = s.componentKind("vehicle")->toJson(s, e);
                 Json changed = Json::object();
                 for (const auto& [k, v] : newJson.members()) {
                     if (oldJson.get(k) != v) changed[k] = Json::object({{"from", oldJson.get(k)}, {"to", v}});
                 }
                 for (const char* live : {"speed", "rpm", "gear", "wheelsOnGround", "skid", "throttle", "brake", "steer", "handbrake"}) {
                     if (changed.contains(live) && !a.get("set").contains(live)) changed[live] = Json();
                 }
                 Json clean = Json::object();
                 for (const auto& [k, v] : changed.members()) {
                     if (!v.isNull()) clean[k] = v;
                 }
                 Json result = Json::object({{"entity", static_cast<int64_t>(e)}, {"changed", clean}, {"warnings", tuneWarnings(*s.get<Vehicle>(e))}});
                 std::string text = std::to_string(clean.size()) + " vehicle field(s) changed";
                 if (!test.empty()) {
                     physics::TestDriveOptions o;
                     o.maneuver = test;
                     auto r = physics::runTestDrive(engine.physics(), s, e, o);
                     if (!r) return ToolResult::error(r.error());
                     result["before"] = before;
                     result["after"] = r->get("summary");
                     text += "; before: " + summaryText(before) + " | after: " + summaryText(r->get("summary"));
                 }
                 return ToolResult::json(result, text);
             }});

    reg.add({"vehicle_info", "Vehicle telemetry",
             "Live state of vehicles: speed (km/h), rpm, gear, inputs as asked and as applied after assists (steering "
             "smoothing, auto reverse, traction control, ABS, drift assist), drift angle, lateral/longitudinal g, and per "
             "wheel: suspension length/compression, contact point and surface, load (N), drive and cornering forces, slip "
             "ratio and slip angle, skid 0..1. While editing it shows the fitted setup at rest (wheel roles, radii) and build "
             "warnings. capture=true adds an image with the vehicles debug view (suspension rays, contacts, tire force "
             "vectors). Without `entity` it lists every vehicle. Pair with sim_control step / sim_input to watch a maneuver.",
             "physics",
             object({{"entity", schema::entity("A vehicle (default: all)")},
                     {"wheels", boolean("Include per-wheel data (default true)")},
                     {"capture", boolean("Add a picture with the vehicles debug view (default false)")},
                     {"width", integer("Capture width (default 960)")},
                     {"height", integer("Capture height (default 540)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 physics::PhysicsWorld& world = liveWorld(engine);
                 std::vector<EntityId> ids;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     if (!s.get<Vehicle>(*id)) {
                         return ToolResult::error(Error::make("not_a_vehicle", "'" + s.record(*id)->name + "' has no vehicle component",
                                                              "create one with vehicle_create"));
                     }
                     ids.push_back(*id);
                 } else {
                     for (EntityId e : s.entities()) {
                         if (s.get<Vehicle>(e)) ids.push_back(e);
                     }
                 }
                 Json list = Json::array();
                 std::ostringstream os;
                 for (EntityId e : ids) {
                     Json item;
                     if (auto t = world.vehicle(e)) {
                         item = physics::telemetryJson(s, *t, a.get("wheels").asBool(true));
                         os << s.record(e)->name << ": " << std::round(t->speedKmh) << " km/h, " << std::round(t->rpm) << " rpm, gear "
                            << t->gear << ", " << t->wheelsOnGround << "/" << t->wheels.size() << " wheels down";
                         if (t->skid > 0.3f) os << ", sliding";
                         os << "\n";
                     } else {
                         item = Json::object({{"entity", static_cast<int64_t>(e)}, {"name", s.record(e)->name}, {"built", false}});
                         os << s.record(e)->name << ": not simulated (see warnings)\n";
                     }
                     item["setup"] = setupJson(*s.get<Vehicle>(e));
                     item["warnings"] = vehicleWarnings(engine, e);
                     list.push(item);
                 }
                 physics::VehicleStats st = world.vehicleStats();
                 Json result = Json::object({{"vehicles", list},
                                             {"playing", engine.playState() != PlayState::Editing},
                                             {"stats", Json::object({{"vehicles", st.vehicles}, {"wheels", st.wheels},
                                                                     {"lastStepMs", std::round(st.lastStepMs * 1000.0) / 1000.0}})}});
                 std::string text = ids.empty() ? "no vehicles in the scene (vehicle_create makes one)" : os.str();
                 if (a.get("capture").asBool(false) && !ids.empty()) {
                     int w = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(960), 64, 1920));
                     int h = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(540), 64, 1080));
                     return captureVehicle(engine, ids.front(), result, text, w, h);
                 }
                 return ToolResult::json(result, text);
             }});

    reg.add({"vehicle_test_drive", "Test drive",
             "An autopilot drives a copy of the vehicle through standard maneuvers in a private physics world (the scene "
             "is not changed; works while editing) and returns handling metrics to tune by numbers: accel (0-60, 0-100 "
             "km/h, quarter mile, wheelspin, upshifts), braking (distance and g from `speed`, default 100 km/h; ABS "
             "activity, stability), slalom (8 cones every cone_spacing m at `speed`, default 60: completed, cones hit, "
             "average speed, line error), skidpad (circle of `radius` m, speed ramps until the car leaves the line: max "
             "lateral g, and whether it understeers, oversteers or runs out of power), top_speed (`duration` s), custom "
             "(`inputs` keyframes [{t, throttle, brake, steer, handbrake}]), all (accel, braking, slalom, skidpad). "
             "track=proving_ground (default: flat, full grip, comparable between cars) or scene (the level as it is). "
             "`overrides` tries vehicle fields without editing (e.g. {\"lateralGrip\": 1.6}). trace=true adds a sampled "
             "trace. Example: {\"entity\": \"Coupe\", \"maneuver\": \"all\"}.",
             "physics",
             object({{"entity", schema::entity("The vehicle")},
                     {"maneuver", enumeration({"accel", "braking", "slalom", "skidpad", "top_speed", "custom", "all"}, "What to drive (default all)")},
                     {"speed", number("km/h: braking start (100), slalom (60), skidpad start (30)")},
                     {"duration", number("Seconds for top_speed (45) / custom")},
                     {"radius", number("Skidpad radius in m (default 40)")},
                     {"cone_spacing", number("Slalom cone spacing in m (default 18)")},
                     {"inputs", array(Json::object({{"type", "object"}}), "custom: keyframes {t, throttle, brake, steer, handbrake}")},
                     {"overrides", Json::object({{"type", "object"}, {"description", "vehicle fields for this drive only"}})},
                     {"track", enumeration({"proving_ground", "scene"}, "Where to drive (default proving_ground)")},
                     {"trace", boolean("Include a trace sampled every 0.25 s")}},
                    {"entity"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 if (a.get("overrides").isObject()) {
                     Vehicle probe;
                     if (Status s = reflect::applyJson(&probe, Vehicle::type(), a.get("overrides")); !s) return fail(s);
                 }
                 auto r = physics::runTestDrive(engine.physics(), engine.scene(), *id, driveOptions(a));
                 if (!r) return ToolResult::error(r.error());
                 std::string text = engine.scene().record(*id)->name + ": " + summaryText(r->get("summary"));
                 return ToolResult::json(*r, text);
             }});
}

}  // namespace sky::tools
