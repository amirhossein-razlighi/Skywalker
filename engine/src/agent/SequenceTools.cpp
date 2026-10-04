// Sequencer tools: build cinematic sequences (keyed properties, camera shots and cuts,
// events, animation tracks), scrub them with captures, render frames, and play them.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/anim/Sequence.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/Image.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

std::string secs(float t) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g s", std::round(t * 1000.f) / 1000.f);
    return buf;
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && str::lower(s).compare(s.size() - n, n, suffix) == 0;
}

/// A readable, stable reference to an entity: its name when unique, else "#id".
std::string entityRef(const Scene& s, EntityId e) {
    const std::string& name = s.record(e)->name;
    return s.find(name) == e ? name : formatEntityRef(e);
}

struct SeqTarget {
    std::string path;
    EntityId player = kNoEntity;  // an entity whose sequencer plays it (if any)
};

/// "cinematics/intro.sequence.json" or an entity with a sequencer component.
Result<SeqTarget> resolveSequence(Engine& engine, const Json& ref) {
    Scene& s = engine.scene();
    SeqTarget t;
    if (ref.isString() && endsWith(ref.asString(), ".sequence.json")) {
        t.path = ref.asString();
        for (EntityId e : s.entities()) {
            const SequencePlayer* sp = s.get<SequencePlayer>(e);
            if (sp && sp->sequence == t.path) {
                t.player = e;
                break;
            }
        }
        return t;
    }
    auto id = resolve(engine, ref);
    if (!id) return id.error();
    const SequencePlayer* sp = s.get<SequencePlayer>(*id);
    if (!sp || sp->sequence.empty()) {
        return Error::make("no_sequencer", "entity " + formatEntityRef(*id) + " has no sequencer with a sequence",
                           "pass the .sequence.json path, or create one with sequence_create");
    }
    t.path = sp->sequence;
    t.player = *id;
    return t;
}

Result<anim::SequenceDef> loadDef(Engine& engine, const std::string& path) {
    return anim::loadSequence(engine.resolvePath(path));
}

Status saveDef(Engine& engine, const std::string& path, const anim::SequenceDef& def) {
    std::error_code ec;
    fs::create_directories(fs::path(engine.resolvePath(path)).parent_path(), ec);
    if (Status s = anim::saveSequence(engine.resolvePath(path), def); !s) return s;
    engine.animation().invalidate(path);
    (void)engine.assets().registerFile(engine.resolvePath(path));
    return {};
}

/// Checks "component.field" against the reflected components (did-you-mean on both parts).
Status checkProperty(const Scene& s, const std::string& property) {
    size_t dot = property.find('.');
    if (dot == std::string::npos) {
        return Error::make("invalid_property", "property \"" + property + "\" must be component.field",
                           "e.g. transform.position, light.intensity, camera.fov, mesh.color, environment.sunElevation");
    }
    std::string comp = property.substr(0, dot), field = property.substr(dot + 1);
    const TypeInfo* info = nullptr;
    if (comp == "environment") {
        info = &Environment::type();
    } else if (const ComponentKind* k = s.componentKind(comp)) {
        info = k->info;
    }
    if (!info) {
        std::vector<std::string> names = s.componentNames();
        names.push_back("environment");
        std::string g = str::closest(comp, names, 3);
        return Error::make("unknown_component", "no component \"" + comp + "\"", g.empty() ? "" : "did you mean \"" + g + "\"?");
    }
    if (!info->field(field)) {
        std::string g = str::closest(field, info->fieldNames(), 3);
        std::string all;
        for (const auto& f : info->fieldNames()) all += (all.empty() ? "" : ", ") + f;
        return Error::make("unknown_field", comp + " has no field \"" + field + "\"",
                           g.empty() ? "fields: " + all : "did you mean \"" + comp + "." + g + "\"?");
    }
    return {};
}

/// Finds (or appends) the track matching a predicate in a sequence document.
Json& trackFor(Json& doc, const std::function<bool(const Json&)>& match, Json fresh) {
    Json& tracks = doc["tracks"];
    if (!tracks.isArray()) tracks = Json::array();
    for (auto& t : tracks.elements()) {
        if (match(t)) return t;
    }
    fresh["keys"] = Json::array();
    tracks.push(std::move(fresh));
    return tracks.elements().back();
}

/// Inserts a key (replacing one at the same time).
void putKey(Json& track, Json key) {
    Json& keys = track["keys"];
    if (!keys.isArray()) keys = Json::array();
    float t = key.get("t").asFloat();
    for (auto& k : keys.elements()) {
        if (std::fabs(k.get("t").asFloat() - t) < 1e-4f) {
            k = std::move(key);
            return;
        }
    }
    keys.push(std::move(key));
}

Json trackSummary(const anim::Track& t) {
    Json j = Json::object({{"type", anim::toString(t.type)}, {"keys", t.keys.size()}});
    if (!t.entity.empty()) j["entity"] = t.entity;
    if (!t.property.empty()) j["property"] = t.property;
    if (!t.camera.empty()) j["camera"] = t.camera;
    if (!t.keys.empty()) j["span"] = Json::array({t.keys.front().t, t.keys.back().t});
    return j;
}

Json sequenceSummary(const anim::SequenceDef& def, const std::string& path) {
    Json tracks = Json::array();
    for (const auto& t : def.tracks) tracks.push(trackSummary(t));
    return Json::object({{"path", path}, {"name", def.name}, {"length", def.length()}, {"tracks", tracks}});
}

}  // namespace

void addSequenceTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"sequence_create", "Create sequence",
             "Create a cinematic sequence asset (*.sequence.json) and an entity whose `sequencer` plays it when the "
             "simulation starts. Then add content with sequence_key (keyed properties, camera cuts, events, animations) "
             "and sequence_camera_shot (orbit / dolly / crane / track / pan / static / path moves), check frames with "
             "sequence_scrub and play with sequence_play. `tracks` may hold a full track list (see docs/ANIMATION.md). "
             "Example: {\"path\": \"cinematics/intro.sequence.json\", \"duration\": 10}.",
             "animation",
             object({{"path", string("Project-relative path ending in .sequence.json")},
                     {"name", string("Display name (default: file name)")},
                     {"duration", number("Length in seconds (0 = until the last key)")},
                     {"tracks", array(Json::object({{"type", "object"}}), "Initial tracks (optional)")},
                     {"entity", string("Entity to play it: an existing one gets the sequencer, a new name creates it "
                                       "(default: a new entity named after the sequence)")},
                     {"play_on_start", boolean("Start with the simulation (default true)")},
                     {"loop", boolean("Loop (default false)")},
                     {"overwrite", boolean("Replace an existing file")}},
                    {"path"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::string path = a.get("path").asString();
                 if (!endsWith(path, ".sequence.json")) {
                     return ToolResult::error(Error::make("invalid_path", "sequence paths end in .sequence.json", "e.g. cinematics/intro.sequence.json"));
                 }
                 if (fs::exists(engine.resolvePath(path)) && !a.get("overwrite").asBool()) {
                     return ToolResult::error(Error::make("exists", path + " already exists", "add keys with sequence_key, or pass overwrite: true"));
                 }
                 std::string name = a.get("name").asString(fs::path(path).stem().stem().string());
                 Json doc = Json::object({{"format", "skywalker.sequence"}, {"version", 1}, {"name", name},
                                          {"duration", a.get("duration").asFloat(0.f)}, {"tracks", a.contains("tracks") ? a.get("tracks") : Json::array()}});
                 auto def = anim::SequenceDef::fromJson(doc);
                 if (!def) return ToolResult::error(def.error());
                 for (const auto& t : def->tracks) {
                     if (t.type == anim::TrackType::Property) {
                         if (Status s = checkProperty(engine.scene(), t.property); !s) return fail(s);
                     }
                 }
                 if (Status s = saveDef(engine, path, *def); !s) return fail(s);
                 Scene& s = engine.scene();
                 EntityId player = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Sequence " + name, [&]() -> Status {
                     std::string ent = a.get("entity").asString(name);
                     player = s.find(ent);
                     if (!player) player = s.create(ent);
                     Json c = Json::object({{"sequence", path}});
                     if (a.contains("play_on_start")) c["playOnStart"] = a.get("play_on_start");
                     if (a.contains("loop")) c["loop"] = a.get("loop");
                     return s.patchComponent(player, "sequencer", c);
                 });
                 if (!st) return fail(st);
                 Json r = sequenceSummary(*def, path);
                 r["entity"] = player;
                 return ToolResult::json(r, "sequence " + path + " played by " + formatEntityRef(player));
             }});

    reg.add({"sequence_key", "Key sequence",
             "Add or replace keys in a sequence, many at once. `keys`: property keys {entity, property, t, value, ease} "
             "animating any component field (\"transform.position\", \"transform.rotation\", \"light.intensity\", "
             "\"light.color\", \"camera.fov\", \"mesh.color\", \"mesh.emissive\", \"particles.rate\", or "
             "\"environment.sunElevation\" without entity). ease: linear (default), step, smooth, ease_in, ease_out, auto "
             "(smooth spline through neighbours — best for camera paths) or [x1,y1,x2,y2] cubic-bezier. `camera_cuts`: "
             "[{t, camera}] which camera is live. `events`: [{t, event, target?}] Wander events. `animations`: [{entity, t, "
             "play, fade?, loop?, params?}] drive animators. A key at an existing time replaces it; replace=true clears "
             "touched tracks first. Example: {\"sequence\": \"cinematics/intro.sequence.json\", \"keys\": [{\"entity\": \"Sun\", "
             "\"property\": \"light.intensity\", \"t\": 0, \"value\": 0}, {\"entity\": \"Sun\", \"property\": "
             "\"light.intensity\", \"t\": 3, \"value\": 4, \"ease\": \"smooth\"}]}.",
             "animation",
             object({{"sequence", any("Sequence path (.sequence.json) or an entity with a sequencer")},
                     {"keys", array(Json::object({{"type", "object"}}), "Property keys {entity, property, t, value, ease}")},
                     {"camera_cuts", array(Json::object({{"type", "object"}}), "Camera cuts {t, camera}")},
                     {"events", array(Json::object({{"type", "object"}}), "Events {t, event, target}")},
                     {"animations", array(Json::object({{"type", "object"}}), "Animation keys {entity, t, play, fade, loop, params}")},
                     {"replace", boolean("Clear every touched track before adding (default false: merge)")},
                     {"remove", array(Json::object({{"type", "object"}}), "Tracks to delete: {entity, property} or {type}")},
                     {"duration", number("Set the sequence length (seconds)")}},
                    {"sequence"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 auto target = resolveSequence(engine, a.get("sequence"));
                 if (!target) return ToolResult::error(target.error());
                 auto def = loadDef(engine, target->path);
                 if (!def) return ToolResult::error(def.error());
                 Json doc = def->toJson();
                 Scene& s = engine.scene();
                 const bool replace = a.get("replace").asBool(false);
                 std::vector<const Json*> cleared;
                 auto touch = [&](Json& track) {
                     if (replace && std::find(cleared.begin(), cleared.end(), &track) == cleared.end()) {
                         track["keys"] = Json::array();
                         cleared.push_back(&track);
                     }
                 };
                 auto ent = [&](const Json& ref) -> Result<std::string> {
                     auto id = resolve(engine, ref);
                     if (!id) return id.error();
                     return entityRef(s, *id);
                 };
                 for (const auto& r : a.get("remove").elements()) {
                     Json& tracks = doc["tracks"];
                     std::string type = r.get("type").asString();
                     std::string re = r.get("entity").asString(), rp = r.get("property").asString();
                     auto& arr = tracks.elements();
                     arr.erase(std::remove_if(arr.begin(), arr.end(),
                                              [&](const Json& t) {
                                                  if (!type.empty() && t.get("type").asString() != type) return false;
                                                  if (!re.empty() && t.get("entity").asString() != re) return false;
                                                  if (!rp.empty() && t.get("property").asString() != rp) return false;
                                                  return !type.empty() || !re.empty() || !rp.empty();
                                              }),
                               arr.end());
                 }
                 size_t added = 0;
                 for (const auto& k : a.get("keys").elements()) {
                     std::string property = k.get("property").asString();
                     if (Status st = checkProperty(s, property); !st) return fail(st);
                     std::string entityName;
                     if (!str::startsWith(property, "environment.")) {
                         auto e = ent(k.get("entity"));
                         if (!e) return ToolResult::error(e.error());
                         entityName = *e;
                     }
                     Json& track = trackFor(doc, [&](const Json& t) {
                         return t.get("type").asString("property") == "property" && t.get("entity").asString() == entityName &&
                                t.get("property").asString() == property;
                     }, Json::object({{"type", "property"}, {"entity", entityName}, {"property", property}}));
                     touch(track);
                     Json key = Json::object({{"t", k.get("t")}, {"value", k.contains("value") ? k.get("value") : k.get("v")}});
                     if (k.contains("ease")) key["ease"] = k.get("ease");
                     putKey(track, std::move(key));
                     ++added;
                 }
                 for (const auto& c : a.get("camera_cuts").elements()) {
                     auto cam = resolve(engine, c.get("camera"));
                     if (!cam) return ToolResult::error(cam.error());
                     if (!s.get<Camera>(*cam)) {
                         return ToolResult::error(Error::make("not_a_camera", formatEntityRef(*cam) + " has no camera component",
                                                              "add one with entity_update {\"components\": {\"camera\": {\"primary\": false}}}"));
                     }
                     Json& track = trackFor(doc, [](const Json& t) { return t.get("type").asString() == "camera"; },
                                            Json::object({{"type", "camera"}}));
                     touch(track);
                     putKey(track, Json::object({{"t", c.get("t")}, {"camera", entityRef(s, *cam)}}));
                     ++added;
                 }
                 for (const auto& ev : a.get("events").elements()) {
                     Json& track = trackFor(doc, [](const Json& t) { return t.get("type").asString() == "event"; },
                                            Json::object({{"type", "event"}}));
                     touch(track);
                     Json key = Json::object({{"t", ev.get("t")}, {"event", ev.get("event")}});
                     if (ev.contains("target")) {
                         auto e = ent(ev.get("target"));
                         if (!e) return ToolResult::error(e.error());
                         key["target"] = *e;
                     }
                     putKey(track, std::move(key));
                     ++added;
                 }
                 for (const auto& an : a.get("animations").elements()) {
                     auto id = resolve(engine, an.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     EntityId ae = engine.animation().animatorFor(*id);
                     if (!ae) return ToolResult::error(Error::make("no_animator", formatEntityRef(*id) + " has no animator"));
                     std::string what = an.get("play").asString();
                     if (!what.empty()) {
                         auto lib = engine.animation().libraryOf(ae);
                         if (!lib) return ToolResult::error(lib.error());
                         const Animator* comp = s.get<Animator>(ae);
                         bool known = (*lib)->clip(what) != nullptr;
                         if (!known && !comp->controller.empty()) {
                             if (auto c = engine.animation().controller(comp->controller)) {
                                 for (const auto& L : (*c)->layers) known = known || L.stateIndex(what) >= 0;
                             }
                         }
                         if (!known) {
                             std::string g = str::closest(what, (*lib)->clipNames(), 3);
                             return ToolResult::error(Error::make("not_found", "no state or clip \"" + what + "\"",
                                                                  g.empty() ? "animation_list shows the clips" : "did you mean \"" + g + "\"?"));
                         }
                     }
                     std::string entityName = entityRef(s, ae);
                     Json& track = trackFor(doc, [&](const Json& t) {
                         return t.get("type").asString() == "animation" && t.get("entity").asString() == entityName;
                     }, Json::object({{"type", "animation"}, {"entity", entityName}}));
                     touch(track);
                     Json key = Json::object({{"t", an.get("t")}});
                     for (const char* f : {"play", "fade", "loop", "layer", "params"}) {
                         if (an.contains(f)) key[f] = an.get(f);
                     }
                     putKey(track, std::move(key));
                     ++added;
                 }
                 if (a.contains("duration")) doc["duration"] = std::max(0.f, a.get("duration").asFloat());
                 auto parsed = anim::SequenceDef::fromJson(doc);  // validates every key
                 if (!parsed) return ToolResult::error(parsed.error());
                 if (Status st = saveDef(engine, target->path, *parsed); !st) return fail(st);
                 Json r = sequenceSummary(*parsed, target->path);
                 r["added"] = added;
                 return ToolResult::json(r, std::to_string(added) + " key(s) in " + target->path);
             }});

    reg.add({"sequence_camera_shot", "Camera shot",
             "Add a procedural camera move to a sequence — the quickest way to direct cinematics. Shots are evaluated "
             "live each tick, so they follow moving targets. Kinds: orbit (around target; radius, height, from/to "
             "degrees), dolly (distance [far, near] along angle, or from/to points), crane (height [low, high]), track "
             "(side-on, slides from/to meters while following the target), pan (fixed position, look from/to: entities, "
             "points or yaw degrees), static (position looking at target), path (Catmull-Rom through points, looking at "
             "target or along the path), flyover (straight aerial pass over the target from the angle side: distance = "
             "half the pass length, height). fov: number or [from, to] (zoom), roll in degrees, ease (default smooth). "
             "cut=true (default) also switches to this camera at start. Missing cameras are created. Example: "
             "{\"sequence\": \"Intro\", \"camera\": \"Cam A\", \"shot\": \"orbit\", \"target\": \"Hero\", \"start\": 0, "
             "\"duration\": 5, \"radius\": 5, \"height\": 1.8, \"from\": -30, \"to\": 60}.",
             "animation",
             object({{"sequence", any("Sequence path or entity with a sequencer")},
                     {"camera", string("Camera entity (created if missing; default \"Shot Camera\")")},
                     {"shot", enumeration(anim::shotKinds(), "Kind of move")},
                     {"start", number("Start time in seconds (default: after the camera's previous shot)")},
                     {"duration", number("Seconds")},
                     {"target", any("Entity name or [x,y,z] the camera frames")},
                     {"offset", vec3("Added to the target point (e.g. [0, 1.6, 0] for eye height)")},
                     {"radius", number("orbit radius (m)")},
                     {"height", any("Camera height above the target: number or [from, to]")},
                     {"distance", any("Distance from the target: number or [from, to]")},
                     {"angle", number("Direction from the target (yaw degrees, 0 = +Z)")},
                     {"from", any("Start: angle (orbit/pan), lateral meters (track), point (dolly/pan)")},
                     {"to", any("End: like from")},
                     {"position", vec3("Camera position (pan, static)")},
                     {"points", array(Json::object({{"type", "array"}}), "Path points (path)")},
                     {"pitch", number("Pan pitch (degrees)")},
                     {"fov", any("Field of view: number or [from, to] (zoom)")},
                     {"roll", number("Dutch angle (degrees)")},
                     {"ease", any("linear, smooth (default), ease_in, ease_out or [x1,y1,x2,y2]")},
                     {"cut", boolean("Switch to this camera at start (default true)")}},
                    {"sequence", "shot", "duration"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto target = resolveSequence(engine, a.get("sequence"));
                 if (!target) return ToolResult::error(target.error());
                 auto def = loadDef(engine, target->path);
                 if (!def) return ToolResult::error(def.error());
                 Scene& s = engine.scene();
                 std::string camName = a.get("camera").asString("Shot Camera");
                 EntityId cam = s.find(camName);
                 if (!cam) {
                     Status st = engine.edit(ctx.actor, "Create " + camName, [&]() -> Status {
                         cam = s.create(camName);
                         return s.patchComponent(cam, "camera", Json::object({{"primary", false}, {"fov", 45}}));
                     });
                     if (!st) return fail(st);
                 } else if (!s.get<Camera>(cam)) {
                     return ToolResult::error(Error::make("not_a_camera", formatEntityRef(cam) + " has no camera component"));
                 }
                 std::string camRef = entityRef(s, cam);
                 if (a.get("target").isString()) {
                     if (auto t = resolve(engine, a.get("target")); !t) return ToolResult::error(t.error());
                 }
                 Json doc = def->toJson();
                 Json& track = trackFor(doc, [&](const Json& t) { return t.get("type").asString() == "shot" && t.get("camera").asString() == camRef; },
                                        Json::object({{"type", "shot"}, {"camera", camRef}}));
                 float start = 0.f;
                 for (const auto& k : track.get("keys").elements()) start = std::max(start, k.get("t").asFloat() + k.get("duration").asFloat());
                 if (a.contains("start")) start = std::max(0.f, a.get("start").asFloat());
                 Json key = Json::object({{"t", start}, {"shot", a.get("shot")}, {"duration", a.get("duration")}});
                 for (const char* f : {"target", "offset", "radius", "height", "distance", "angle", "from", "to", "position", "points",
                                       "pitch", "fov", "roll", "ease"}) {
                     if (a.contains(f)) key[f] = a.get(f);
                 }
                 putKey(track, std::move(key));
                 if (a.get("cut").asBool(true)) {
                     Json& cuts = trackFor(doc, [](const Json& t) { return t.get("type").asString() == "camera"; }, Json::object({{"type", "camera"}}));
                     putKey(cuts, Json::object({{"t", start}, {"camera", camRef}}));
                 }
                 auto parsed = anim::SequenceDef::fromJson(doc);
                 if (!parsed) return ToolResult::error(parsed.error());
                 if (Status st = saveDef(engine, target->path, *parsed); !st) return fail(st);
                 // Where the camera goes (helps agents sanity-check the move).
                 Json path = Json::array();
                 for (const auto& t : parsed->tracks) {
                     if (t.type != anim::TrackType::Shot || t.camera != camRef) continue;
                     float dur = a.get("duration").asFloat();
                     for (float f : {0.f, 0.5f, 1.f}) {
                         auto pose = anim::evaluateShot(t, start + f * dur, [&](const std::string& ref) -> std::optional<Vec3> {
                             EntityId id = s.find(ref);
                             if (!id) return std::nullopt;
                             if (s.get<MeshRenderer>(id)) return s.localBounds(id).transformed(s.worldMatrix(id)).center();
                             return s.worldMatrix(id).translation();
                         });
                         if (pose) {
                             path.push(Json::object({{"t", start + f * dur},
                                                     {"position", reflect::vec3ToJson(pose->position)},
                                                     {"rotation", reflect::vec3ToJson(pose->rotation)}}));
                         }
                     }
                 }
                 Json r = sequenceSummary(*parsed, target->path);
                 r["camera"] = cam;
                 r["start"] = start;
                 r["preview"] = path;
                 return ToolResult::json(r, a.get("shot").asString() + " shot on " + camRef + " at " + secs(start));
             }});

    reg.add({"sequence_get", "Inspect sequence",
             "A sequence's tracks (type, entity, property, key count and time span) and length; with `time`, the "
             "evaluated property values and live camera at that moment.",
             "animation",
             object({{"sequence", any("Sequence path or entity with a sequencer")}, {"time", number("Evaluate at this time (seconds)")}},
                    {"sequence"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto target = resolveSequence(engine, a.get("sequence"));
                 if (!target) return ToolResult::error(target.error());
                 auto def = loadDef(engine, target->path);
                 if (!def) return ToolResult::error(def.error());
                 Json r = sequenceSummary(*def, target->path);
                 if (target->player) r["player"] = target->player;
                 if (a.contains("time")) {
                     float t = a.get("time").asFloat();
                     Json values = Json::array();
                     for (const auto& tr : def->tracks) {
                         if (tr.type != anim::TrackType::Property) continue;
                         values.push(Json::object({{"entity", tr.entity}, {"property", tr.property}, {"value", anim::evaluateProperty(tr, t)}}));
                     }
                     r["values"] = values;
                     if (target->player) {
                         if (EntityId cam = engine.animation().sequenceCamera(target->player, t)) r["camera"] = entityRef(engine.scene(), cam);
                     }
                 }
                 if (target->player && engine.playState() != PlayState::Editing) r["playback"] = engine.animation().sequenceState(target->player);
                 return ToolResult::json(r, def->name + ": " + std::to_string(def->tracks.size()) + " tracks, " +
                                                secs(def->length()));
             }});

    reg.add({"sequence_play", "Play sequence",
             "Play or stop a sequence. Sequences run in the simulation: while editing this starts the simulation (like "
             "sim_control play) and plays the sequence from `from`; while playing it restarts / seeks or stops it. "
             "Sequencers with playOnStart start by themselves when the simulation starts. Watch with viewport_capture "
             "view=\"scene\" after sim_control step.",
             "animation",
             object({{"sequence", any("Entity with a sequencer, or a sequence path played by one")},
                     {"action", enumeration({"play", "stop"}, "Default play")},
                     {"from", number("Start time in seconds (default 0)")}},
                    {"sequence"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 auto target = resolveSequence(engine, a.get("sequence"));
                 if (!target) return ToolResult::error(target.error());
                 if (!target->player) {
                     return ToolResult::error(Error::make("no_sequencer", "no entity plays " + target->path,
                                                          "create one with sequence_create entity, or add a sequencer component"));
                 }
                 bool started = false;
                 if (a.get("action").asString("play") == "stop") {
                     if (engine.playState() == PlayState::Editing) return ToolResult::text("not playing (the simulation is stopped)");
                     if (Status s = engine.animation().stopSequence(target->player); !s) return fail(s);
                 } else {
                     if (engine.playState() == PlayState::Editing) {
                         engine.play();
                         started = true;
                     }
                     if (Status s = engine.animation().playSequence(target->player, a.get("from").asFloat(0.f)); !s) return fail(s);
                 }
                 Json r = engine.animation().sequenceState(target->player);
                 r["simulation"] = toString(engine.playState());
                 return ToolResult::json(r, started ? "simulation started; sequence playing" : "sequence updated");
             }});

    reg.add({"sequence_scrub", "Scrub sequence",
             "Look at a sequence at any time without playing: renders through the sequence's live camera (cuts and "
             "shots) with every keyed property and animation applied, then restores the scene. times=[...] returns up "
             "to 8 images (a storyboard). Render a whole shot to disk with fps + from/to + save_dir (PNG frames, e.g. "
             "for a video). persist=true instead leaves the editor showing that time (sequencer preview). Only while "
             "editing. Example: {\"sequence\": \"Intro\", \"times\": [0, 2.5, 5]}.",
             "animation",
             object({{"sequence", any("Entity with a sequencer, or a sequence path played by one")},
                     {"time", number("Seconds")},
                     {"times", array(Json::object({{"type", "number"}}), "Several times (max 8 images)")},
                     {"fps", number("Render frames at this rate between from and to into save_dir")},
                     {"from", number("Render range start (default 0)")},
                     {"to", number("Render range end (default: the sequence length)")},
                     {"save_dir", string("Project-relative folder for rendered frames (frame_0000.png ...)")},
                     {"camera", enumeration({"sequence", "editor"}, "Look through the sequence's camera (default) or the editor camera")},
                     {"width", integer("Image width (default 768)")},
                     {"height", integer("Image height (default 432)")},
                     {"persist", boolean("Keep showing this time in the editor (sets sequencer preview/time)")},
                     {"include_image", boolean("Return the images (default true; false = just render / save)")},
                     {"transient", boolean("Only show `time` in the editor viewport (no capture, not saved) until clear")},
                     {"clear", boolean("End a transient scrub")}},
                    {"sequence"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 if (engine.playState() != PlayState::Editing) {
                     return ToolResult::error(Error::make("invalid_state", "scrubbing works while editing",
                                                          "stop the simulation (sim_control stop) or seek with sequence_play from"));
                 }
                 auto target = resolveSequence(engine, a.get("sequence"));
                 if (!target) return ToolResult::error(target.error());
                 if (!target->player) {
                     return ToolResult::error(Error::make("no_sequencer", "no entity plays " + target->path,
                                                          "create one with sequence_create entity, or add a sequencer component"));
                 }
                 EntityId player = target->player;
                 anim::AnimationSystem& as = engine.animation();
                 auto def = as.sequence(target->path);
                 if (!def) return ToolResult::error(def.error());
                 if (a.get("clear").asBool()) {
                     as.clearSequenceScrub(player);
                     return ToolResult::text("scrub cleared");
                 }
                 if (a.get("transient").asBool()) {
                     as.setSequenceScrub(player, std::max(0.f, a.get("time").asFloat(0.f)));
                     return ToolResult::json(Json::object({{"time", a.get("time")}}), "showing " + secs(a.get("time").asFloat(0.f)));
                 }
                 if (a.get("persist").asBool()) {
                     float t = a.get("time").asFloat(0.f);
                     Status st = engine.edit(ctx.actor, "Scrub to " + secs(t), [&]() {
                         return engine.scene().patchComponent(player, "sequencer", Json::object({{"preview", true}, {"time", t}}));
                     });
                     if (!st) return fail(st);
                     return ToolResult::json(Json::object({{"time", t}}), "the editor now shows the sequence at " + secs(t));
                 }
                 std::vector<float> times;
                 std::string saveDir = a.get("save_dir").asString();
                 if (a.contains("fps")) {
                     float fps = std::clamp(a.get("fps").asFloat(), 1.f, 120.f);
                     float from = std::max(0.f, a.get("from").asFloat(0.f));
                     float to = a.get("to").asFloat((*def)->length());
                     if (saveDir.empty()) return ToolResult::error(Error::make("invalid_arguments", "fps rendering needs save_dir"));
                     for (int i = 0; from + static_cast<float>(i) / fps <= to + 1e-4f && i < 3600; ++i) times.push_back(from + static_cast<float>(i) / fps);
                 } else {
                     for (const auto& t : a.get("times").elements()) times.push_back(std::max(0.f, t.asFloat()));
                     if (times.empty()) times.push_back(std::max(0.f, a.get("time").asFloat(0.f)));
                     if (times.size() > 8 && saveDir.empty()) return ToolResult::error(Error::make("too_many", "at most 8 images; use fps + save_dir to render more"));
                 }
                 CaptureOptions o;
                 o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(768), 16, 2048));
                 o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(432), 16, 2048));
                 o.annotate = false;
                 o.editorOverlays = false;
                 const bool sequenceCam = a.get("camera").asString("sequence") == "sequence";
                 if (!saveDir.empty()) {
                     std::error_code ec;
                     fs::create_directories(engine.resolvePath(saveDir), ec);
                 }
                 ToolResult res = ToolResult::text("");
                 Json shots = Json::array();
                 for (size_t i = 0; i < times.size(); ++i) {
                     as.setSequenceScrub(player, times[i]);
                     o.cameraEntity = sequenceCam ? as.sequenceCamera(player, times[i]) : kNoEntity;
                     auto cap = engine.capture(o);
                     as.clearSequenceScrub(player);
                     if (!cap) return ToolResult::error(cap.error());
                     Json shot = Json::object({{"time", times[i]}});
                     if (o.cameraEntity) shot["camera"] = entityRef(engine.scene(), o.cameraEntity);
                     if (!saveDir.empty()) {
                         char name[32];
                         std::snprintf(name, sizeof(name), "frame_%04zu.png", i);
                         std::string file = (fs::path(saveDir) / name).generic_string();
                         if (Status st = writePng(cap->image, engine.resolvePath(file)); !st) return fail(st);
                         shot["file"] = file;
                     }
                     if (times.size() <= 8 && a.get("include_image").asBool(true)) {
                         std::vector<uint8_t> png = encodePng(cap->image);
                         res.image(str::base64Encode(png.data(), png.size()));
                     }
                     if (times.size() <= 64) shots.push(std::move(shot));
                 }
                 res.content.front().text = saveDir.empty() ? "scrubbed " + std::to_string(times.size()) + " time(s)"
                                                            : "rendered " + std::to_string(times.size()) + " frame(s) to " + saveDir;
                 res.structured = Json::object({{"frames", times.size()}, {"shots", shots}, {"length", (*def)->length()}});
                 return res;
             }});
}

}  // namespace sky::tools
