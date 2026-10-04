// Animation tools: inspect rigs and clips, set up animator state machines, preview poses,
// drive animators, attach props to bones. Sequencer (cinematics) tools live in
// SequenceTools.cpp and are registered from here.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/Image.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

float round3(float v) { return std::round(v * 1000.f) / 1000.f; }

/// The entity's animator (at or above it), with a helpful error.
Result<EntityId> animatorOf(Engine& engine, const Json& ref) {
    auto id = resolve(engine, ref);
    if (!id) return id.error();
    EntityId ae = engine.animation().animatorFor(*id);
    if (!ae) {
        return Error::make("no_animator", "entity " + formatEntityRef(*id) + " has no animator (nor do its parents)",
                           "import a rigged glTF with asset_import (it creates one), or call animator_setup on the character");
    }
    return ae;
}

/// Model-space up axis and world units per model unit for an animator.
std::pair<Vec3, float> modelFrame(Engine& engine, EntityId ae) {
    Mat4 t = ae ? engine.animation().modelTransform(ae) : Mat4{};
    Vec3 up = normalize(t.inverse().transformDir({0, 1, 0}));
    float scale = length(t.transformDir(up));
    if (ae) scale *= length(engine.scene().worldMatrix(ae).transformDir({0, 1, 0}));
    return {length(up) > 0.f ? up : Vec3{0, 1, 0}, scale > 0.f ? scale : 1.f};
}

Json clipsJson(const anim::Library& lib, Vec3 up, float scale) {
    Json out = Json::array();
    for (const auto& c : lib.clips) {
        Json j = Json::object({{"name", c.name}, {"duration", round3(c.duration)}});
        float speed = anim::rootSpeed(lib, c, up) * scale;
        if (speed > 0.05f) j["rootSpeed"] = round3(speed);  // m/s the clip moves (locomotion)
        out.push(std::move(j));
    }
    return out;
}

Json controllerSummary(const anim::ControllerDef& c) {
    Json params = Json::array();
    for (const auto& p : c.params) params.push(Json::object({{"name", p.name}, {"type", anim::toString(p.type)}}));
    Json layers = Json::array();
    for (const auto& L : c.layers) {
        Json states = Json::array();
        for (const auto& s : L.states) states.push(s.name);
        layers.push(Json::object({{"name", L.name}, {"default", L.defaultState}, {"states", states}, {"transitions", L.transitions.size()}}));
    }
    return Json::object({{"parameters", params}, {"layers", layers}});
}

std::string findClip(const anim::Library& lib, std::initializer_list<const char*> words, const std::vector<std::string>& exclude = {}) {
    for (const char* w : words) {
        for (const auto& c : lib.clips) {
            std::string n = str::lower(c.name);
            if (n.find(w) == std::string::npos) continue;
            if (std::find(exclude.begin(), exclude.end(), c.name) != exclude.end()) continue;
            return c.name;
        }
    }
    return {};
}

/// Capture options for looking at a character: explicit eye/target, a camera, or an
/// automatic framing from the front / three-quarter / side / back.
CaptureOptions characterView(Engine& engine, EntityId e, const Json& a) {
    CaptureOptions o;
    o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(512), 64, 2048));
    o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(512), 64, 2048));
    o.annotate = false;
    o.editorOverlays = false;
    Vec3 eye, target;
    if (reflect::jsonToVec3(a.get("eye"), eye)) {
        o.hasCustomView = true;
        o.customView = engine.camera().toView();
        if (!reflect::jsonToVec3(a.get("target"), target)) target = o.customView.target;
        o.customView.lookFrom(eye, target);
        return o;
    }
    Scene& s = engine.scene();
    // Bounds from the CPU meshes: an asset that has not been drawn yet has no cached bounds in the
    // scene (the first preview of a freshly instantiated character would frame a unit cube).
    std::optional<Aabb> meshBox;
    std::vector<EntityId> subtree;
    collectSubtree(s, e, subtree);
    for (EntityId id : subtree) {
        const MeshRenderer* m = s.get<MeshRenderer>(id);
        if (!m || !m->visible) continue;
        const MeshData* md = str::startsWith(m->mesh, "asset:") ? engine.cpuMesh(m->mesh) : nullptr;
        Aabb b = (md ? md->bounds : s.localBounds(id)).transformed(s.worldMatrix(id));
        meshBox = meshBox ? Aabb{vmin(meshBox->min, b.min), vmax(meshBox->max, b.max)} : b;
    }
    Aabb box = meshBox.value_or(subtreeBounds(s, e).value_or(s.localBounds(e).transformed(s.worldMatrix(e))));
    Vec3 c = box.center();
    float radius = std::max(length(box.extents()), 0.3f) * 1.15f;  // room for limbs swinging out
    Vec3 fwd = s.worldMatrix(e).transformDir({0, 0, -1});
    fwd.y = 0.f;
    fwd = length(fwd) > 1e-4f ? normalize(fwd) : Vec3{0, 0, -1};
    Vec3 right = normalize(cross(fwd, {0, 1, 0}));
    std::string view = a.get("view").asString("three_quarter");
    Vec3 dir = view == "front" ? fwd : view == "side" ? right : view == "back" ? -fwd : normalize(fwd + right * 0.75f);
    dir = normalize(dir + Vec3{0, 0.18f, 0});
    ViewCamera cam;
    cam.fovDeg = 35.f;
    cam.eye = c + dir * (radius / std::sin(radians(cam.fovDeg * 0.5f)));
    cam.target = c;
    cam.nearPlane = 0.05f;
    cam.farPlane = 500.f;
    o.hasCustomView = true;
    o.customView = cam;
    return o;
}

Json previewSchema() {
    return object({{"entity", entity("The character (or any entity under its animator)")},
                   {"clip", string("Clip or state to show (default: what the animator plays by default)")},
                   {"time", number("Seconds into the clip/state (default 0)")},
                   {"times", array(Json::object({{"type", "number"}}), "Several times -> one image each (max 8), e.g. a walk cycle contact sheet")},
                   {"params", Json::object({{"type", "object"}, {"description", "Controller parameters for the preview, e.g. {\"speed\": 2.5}"}})},
                   {"view", enumeration({"three_quarter", "front", "side", "back"}, "Automatic framing (default three_quarter)")},
                   {"eye", vec3("Custom camera position (instead of automatic framing)")},
                   {"target", vec3("Custom look-at point (with eye)")},
                   {"width", integer("Image width (default 512)")},
                   {"height", integer("Image height (default 512)")},
                   {"save_path", string("Also write the (first) PNG to this project-relative path")}},
                  {"entity"});
}

}  // namespace

void addSequenceTools(Engine& engine, ToolRegistry& reg);  // SequenceTools.cpp

void addAnimationTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"animation_list", "List animations",
             "Skeleton and clips of a rigged model or animated character. With `entity`: its animator's library "
             "(clips with duration and rootSpeed in m/s for locomotion clips), controller (parameters, states), "
             "current state, recent animation events and warnings. With `model`: a .anim library or an imported "
             ".glb/.gltf. With neither: every animation library in the project. bones=true lists bone names "
             "(for bone_attach, layer masks, look-at). Example: {\"entity\": \"Hero\"}.",
             "animation",
             object({{"entity", entity("Character with an animator (or a child of one)")},
                     {"model", string("Animation library (.anim) or rigged model (.glb/.gltf), project-relative")},
                     {"bones", boolean("Include the bone hierarchy (default: true for model, false for entity)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 anim::AnimationSystem& as = engine.animation();
                 if (a.contains("entity")) {
                     auto ae = animatorOf(engine, a.get("entity"));
                     if (!ae) return ToolResult::error(ae.error());
                     auto info = as.describe(*ae);
                     if (!info) return ToolResult::error(info.error());
                     Json j = *info;
                     const Animator* an = engine.scene().get<Animator>(*ae);
                     j["component"] = reflect::toJson(an, Animator::type());
                     if (auto lib = as.libraryOf(*ae)) {
                         auto [up, scale] = modelFrame(engine, *ae);
                         j["clips"] = clipsJson(**lib, up, scale);
                         if (a.get("bones").asBool(false)) j["bones"] = anim::libraryToJson(**lib, true).get("bones");
                         if ((*lib)->rootBone >= 0) j["rootBone"] = (*lib)->skeleton.bones[static_cast<size_t>((*lib)->rootBone)].name;
                     }
                     if (!an->controller.empty()) {
                         if (auto c = as.controller(an->controller)) j["controller"] = controllerSummary(**c);
                     }
                     std::ostringstream os;
                     os << "animator " << formatEntityRef(*ae) << ": state " << j.get("state").get("layers")[size_t{0}].get("state").asString()
                        << ", " << j.get("clips").size() << " clips";
                     return ToolResult::json(j, os.str());
                 }
                 if (a.contains("model")) {
                     auto lib = as.library(a.get("model").asString());
                     if (!lib) return ToolResult::error(lib.error());
                     Json j = anim::libraryToJson(**lib, a.get("bones").asBool(true));
                     j["clips"] = clipsJson(**lib, {0, 1, 0}, 1.f);
                     j["model"] = a.get("model");
                     return ToolResult::json(j, std::to_string((*lib)->clips.size()) + " clips, " +
                                                    std::to_string((*lib)->skeleton.bones.size()) + " bones");
                 }
                 AssetDatabase::Query q;
                 q.type = AssetType::Animation;
                 q.limit = 200;
                 Json libs = Json::array();
                 for (const AssetRecord* r : engine.assets().query(q)) {
                     Json item = Json::object({{"path", r->path}});
                     if (auto lib = as.library(r->path)) {
                         Json clips = Json::array();
                         for (const auto& c : (*lib)->clips) clips.push(c.name);
                         item["clips"] = clips;
                         item["bones"] = (*lib)->skeleton.bones.size();
                     }
                     if (r->source.contains("importedFrom")) item["model"] = r->source.get("importedFrom");
                     libs.push(std::move(item));
                 }
                 return ToolResult::json(Json::object({{"libraries", libs}}), std::to_string(libs.size()) + " animation libraries");
             }});

    reg.add({"animator_setup", "Set up animator",
             "Create a state machine controller (*.animctl.json) for a character and assign it. preset=locomotion "
             "(default) finds idle/walk/run/jump clips by name and builds: parameter `speed` (m/s) driving a 1D blend "
             "Idle -> Walk -> Run whose thresholds are the clips' real root speeds (feet don't slide when you set speed to "
             "the actual velocity), and if there is a jump clip a `jump` trigger with Jump -> back to locomotion. "
             "preset=clips: one state per clip, no transitions (drive with play_animation). Override clip choices with "
             "`clips` ({\"idle\": \"Breathe\", \"run\": \"Sprint\"}), or pass a full `controller` document (parameters, "
             "layers with states/blend/blend2d, transitions with \"when\" conditions like \"speed > 0.1 and grounded\", "
             "exit times, events, masks) — it is validated with did-you-mean errors. Then drive it from Wander: "
             "set_param(self, \"speed\", v), trigger(self, \"jump\"), play_animation(self, \"Wave\").",
             "animation",
             object({{"entity", entity("The character")},
                     {"preset", enumeration({"locomotion", "clips"}, "Generated controller (default locomotion)")},
                     {"clips", Json::object({{"type", "object"}, {"description", "Clip choice overrides: idle, walk, run, jump"}})},
                     {"controller", Json::object({{"type", "object"}, {"description", "A full controller document instead of a preset"}})},
                     {"path", string("Where to save the controller (default: next to the library, <name>.animctl.json)")},
                     {"library", string("Animation library (.anim) if the character has none yet")},
                     {"root_motion", boolean("Move the character by its clips' root motion (default: unchanged)")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto target = resolve(engine, a.get("entity"));
                 if (!target) return ToolResult::error(target.error());
                 anim::AnimationSystem& as = engine.animation();
                 Scene& s = engine.scene();
                 EntityId ae = as.animatorFor(*target);
                 if (!ae) ae = *target;
                 std::string savedPath;
                 Json controllerJ;
                 Json drive = Json::array();
                 Status st = engine.edit(ctx.actor, "Animator setup " + s.record(ae)->name, [&]() -> Status {
                     if (!s.get<Animator>(ae)) {
                         Json init = Json::object();
                         if (a.contains("library")) init["library"] = a.get("library");
                         if (Status r = s.patchComponent(ae, "animator", init); !r) return r;
                     } else if (a.contains("library")) {
                         if (Status r = s.patchComponent(ae, "animator", Json::object({{"library", a.get("library")}})); !r) return r;
                     }
                     auto lib = as.libraryOf(ae);
                     if (!lib) return lib.error();
                     const anim::Library& L = **lib;
                     anim::ControllerDef def;
                     if (a.contains("controller")) {
                         auto parsed = anim::ControllerDef::fromJson(a.get("controller"));
                         if (!parsed) return parsed.error();
                         def = std::move(*parsed);
                     } else if (a.get("preset").asString("locomotion") == "clips") {
                         std::string idle = findClip(L, {"idle", "stand", "breath"});
                         def = anim::simpleController(L.clipNames(), idle, true);
                         drive.push("play_animation(self, \"<clip>\") with any of the " + std::to_string(L.clips.size()) + " clips");
                     } else {
                         const Json& pick = a.get("clips");
                         auto choose = [&](const char* role, std::initializer_list<const char*> words) {
                             std::string v = pick.get(role).asString();
                             return v.empty() ? findClip(L, words) : v;
                         };
                         std::string idle = choose("idle", {"idle", "stand", "breath", "rest"});
                         std::string walk = choose("walk", {"walk"});
                         std::string run = choose("run", {"run", "jog", "sprint"});
                         std::string jump = choose("jump", {"jump"});
                         if (idle.empty() && !L.clips.empty()) idle = L.clips.front().name;
                         if (idle.empty()) return Error::make("no_clips", "the animation library has no clips");
                         auto [up, scale] = modelFrame(engine, ae);
                         auto speedOf = [&](const std::string& name, float fallback) {
                             const anim::Clip* c = L.clip(name);
                             float v = c ? anim::rootSpeed(L, *c, up) * scale : 0.f;
                             return v > 0.1f ? round3(v) : fallback;
                         };
                         Json motions = Json::array({Json::object({{"clip", idle}, {"at", 0}})});
                         float walkAt = 0.f;
                         if (!walk.empty()) {
                             walkAt = speedOf(walk, 1.5f);
                             motions.push(Json::object({{"clip", walk}, {"at", walkAt}}));
                         }
                         if (!run.empty() && run != walk) motions.push(Json::object({{"clip", run}, {"at", std::max(speedOf(run, 4.f), walkAt + 0.5f)}}));
                         Json states = Json::object();
                         states["Locomotion"] = motions.size() > 1 ? Json::object({{"blend", Json::object({{"parameter", "speed"}, {"motions", motions}})}})
                                                                   : Json::object({{"clip", idle}});
                         Json params = Json::object({{"speed", "float"}});
                         Json transitions = Json::array();
                         if (!jump.empty()) {
                             params["jump"] = "trigger";
                             states["Jump"] = Json::object({{"clip", jump}, {"loop", false}});
                             transitions.push(Json::object({{"from", "any"}, {"to", "Jump"}, {"when", "jump"}, {"duration", 0.1}}));
                             transitions.push(Json::object({{"from", "Jump"}, {"to", "Locomotion"}, {"exit", 0.9}, {"duration", 0.2}}));
                         }
                         Json doc = Json::object({{"parameters", params},
                                                  {"layers", Json::array({Json::object({{"name", "Base"},
                                                                                        {"default", "Locomotion"},
                                                                                        {"states", states},
                                                                                        {"transitions", transitions}})})}});
                         auto parsed = anim::ControllerDef::fromJson(doc);
                         if (!parsed) return parsed.error();
                         def = std::move(*parsed);
                         drive.push("set_param(self, \"speed\", <m/s>)  — " + std::string(motions.size() > 1 ? "blends " : "") + idle +
                                    (walk.empty() ? "" : " -> " + walk) + (run.empty() || run == walk ? "" : " -> " + run));
                         if (!jump.empty()) drive.push("trigger(self, \"jump\")");
                     }
                     std::shared_ptr<const anim::Library> libPtr = *lib;
                     Status valid = def.validate([&](const std::string& ref) { return as.resolveClip(libPtr, ref) != nullptr; }, L.clipNames(),
                                                 &L.skeleton);
                     if (!valid) return valid;
                     const Animator* an = s.get<Animator>(ae);
                     std::string path = a.get("path").asString();
                     if (path.empty()) {
                         std::string libRef = !an->library.empty() ? an->library : "";
                         fs::path dir = libRef.empty() ? fs::path("animations") : fs::path(libRef).parent_path();
                         std::string stem = s.record(ae)->name;
                         for (char& ch : stem) {
                             if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
                         }
                         path = (dir / (str::lower(stem) + ".animctl.json")).generic_string();
                     }
                     if (path.size() < 13 || str::lower(path).compare(path.size() - 13, 13, ".animctl.json") != 0) {
                         return Error::make("invalid_path", "controller paths end in .animctl.json", "e.g. characters/hero.animctl.json");
                     }
                     std::error_code ec;
                     fs::create_directories(fs::path(engine.resolvePath(path)).parent_path(), ec);
                     if (Status w = anim::saveController(engine.resolvePath(path), def); !w) return w;
                     as.invalidate(path);
                     (void)engine.assets().registerFile(engine.resolvePath(path));
                     savedPath = path;
                     controllerJ = def.toJson();
                     Json patch = Json::object({{"controller", path}});
                     if (a.contains("root_motion")) patch["rootMotion"] = a.get("root_motion");
                     return s.patchComponent(ae, "animator", patch);
                 });
                 if (!st) return fail(st);
                 Json r = Json::object({{"entity", ae}, {"controller", savedPath}, {"document", controllerJ}, {"drive", drive}});
                 return ToolResult::json(r, "controller " + savedPath + " assigned to " + formatEntityRef(ae));
             }});

    reg.add({"animation_preview", "Preview animation",
             "Render a character posed at a clip or state at given times (editor preview; the scene is not changed). "
             "Frames the character automatically (three_quarter / front / side / back) unless eye/target are given. "
             "times=[0, 0.25, 0.5, 0.75] returns a contact sheet of images — check walk cycles, attacks, hand poses, "
             "props attached to bones. While playing, captures the live pose instead. Example: "
             "{\"entity\": \"Hero\", \"clip\": \"Run\", \"times\": [0, 0.2, 0.4]}.",
             "animation", previewSchema(), false, false, [&engine](const Json& a, ToolContext&) {
                 auto ae = animatorOf(engine, a.get("entity"));
                 if (!ae) return ToolResult::error(ae.error());
                 auto id = resolve(engine, a.get("entity"));
                 anim::AnimationSystem& as = engine.animation();
                 std::vector<float> times;
                 for (const auto& t : a.get("times").elements()) times.push_back(std::max(0.f, t.asFloat()));
                 if (times.empty()) times.push_back(std::max(0.f, a.get("time").asFloat(0.f)));
                 if (times.size() > 8) return ToolResult::error(Error::make("too_many", "at most 8 times per preview"));
                 const bool editing = engine.playState() == PlayState::Editing;
                 for (const auto& [name, v] : a.get("params").members()) {
                     if (Status s = as.setParam(*ae, name, v); !s) return fail(s);
                 }
                 CaptureOptions o = characterView(engine, *id == *ae ? *ae : *id, a);
                 ToolResult res = ToolResult::text("");
                 Json shots = Json::array();
                 std::string clip = a.get("clip").asString();
                 for (size_t i = 0; i < times.size(); ++i) {
                     if (editing) {
                         if (Status s = as.setPreview(*ae, clip, times[i]); !s) {
                             as.clearPreview(*ae);
                             return fail(s);
                         }
                     }
                     auto cap = engine.capture(o);
                     if (!cap) {
                         as.clearPreview(*ae);
                         return ToolResult::error(cap.error());
                     }
                     if (i == 0 && a.contains("save_path")) {
                         if (Status s = writePng(cap->image, engine.resolvePath(a.get("save_path").asString())); !s) {
                             as.clearPreview(*ae);
                             return fail(s);
                         }
                     }
                     std::vector<uint8_t> png = encodePng(cap->image);
                     res.image(str::base64Encode(png.data(), png.size()));
                     shots.push(Json::object({{"time", times[i]}}));
                     if (!editing) break;
                 }
                 if (editing) as.clearPreview(*ae);
                 std::string what = clip.empty() ? "the default state" : clip;
                 res.content.front().text = editing ? "previewed " + what + " at " + std::to_string(times.size()) + " time(s)"
                                                    : "captured the live pose (simulation is running)";
                 res.structured = Json::object({{"animator", *ae}, {"shots", shots}, {"live", !editing}});
                 return res;
             }});

    reg.add({"animator_set", "Drive animator",
             "Set an animator's parameters and triggers, or play a state/clip with a crossfade — live while the "
             "simulation runs (same as Wander set_param / trigger / play_animation), or as the editor preview while "
             "editing. Also sets look_at (head/spine IK toward an entity; \"\" clears), the editor preview mode/time. "
             "Returns the animator state. Example: {\"entity\": \"Hero\", \"params\": {\"speed\": 3}, \"trigger\": \"jump\"}.",
             "animation",
             object({{"entity", entity("The character")},
                     {"params", Json::object({{"type", "object"}, {"description", "Parameter values, e.g. {\"speed\": 2.5, \"grounded\": true}"}})},
                     {"trigger", any("Trigger name or list of names")},
                     {"play", string("State or clip to crossfade to (editing: shown as the preview)")},
                     {"fade", number("Crossfade seconds (default 0.2)")},
                     {"loop", boolean("Loop the played clip (default: the state's setting; clips play once and return)")},
                     {"layer", integer("Layer index for play (default 0)")},
                     {"time", number("Editor preview time in seconds")},
                     {"preview", enumeration({"rest", "pose", "play"}, "Editor preview mode")},
                     {"look_at", string("Entity the head turns toward (\"\" = off)")},
                     {"preview_time", number("Editing: show the default state (or `play`) at this time without saving it (live scrubbing)")},
                     {"clear_preview", boolean("Remove a preview set by play / preview_time while editing")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto ae = animatorOf(engine, a.get("entity"));
                 if (!ae) return ToolResult::error(ae.error());
                 anim::AnimationSystem& as = engine.animation();
                 const bool editing = engine.playState() == PlayState::Editing;
                 for (const auto& [name, v] : a.get("params").members()) {
                     if (Status s = as.setParam(*ae, name, v); !s) return fail(s);
                 }
                 std::vector<std::string> triggers;
                 if (a.get("trigger").isString()) triggers.push_back(a.get("trigger").asString());
                 for (const auto& t : a.get("trigger").elements()) triggers.push_back(t.asString());
                 for (const auto& t : triggers) {
                     if (Status s = as.trigger(*ae, t); !s) return fail(s);
                 }
                 Json patch = Json::object();
                 if (a.contains("time")) patch["time"] = a.get("time");
                 if (a.contains("preview")) patch["preview"] = a.get("preview");
                 if (a.contains("look_at")) {
                     std::string target = a.get("look_at").asString();
                     if (!target.empty()) {
                         auto t = resolve(engine, a.get("look_at"));
                         if (!t) return ToolResult::error(t.error());
                     }
                     patch["lookAt"] = target;
                 }
                 if (patch.size()) {
                     Status st = engine.edit(ctx.actor, "Animator " + engine.scene().record(*ae)->name,
                                             [&]() { return engine.scene().patchComponent(*ae, "animator", patch); });
                     if (!st) return fail(st);
                 }
                 if (a.get("clear_preview").asBool()) as.clearPreview(*ae);
                 if (a.contains("play") || (editing && a.contains("preview_time"))) {
                     std::string what = a.get("play").asString();
                     Status s;
                     if (editing) {
                         s = as.setPreview(*ae, what, a.contains("preview_time") ? a.get("preview_time").asFloat() : a.get("time").asFloat(0.f));
                     } else {
                         std::optional<bool> loop;
                         if (a.get("loop").isBool()) loop = a.get("loop").asBool();
                         s = as.play(*ae, what, a.get("fade").asFloat(0.2f), loop, static_cast<int>(a.get("layer").asInt(0)));
                     }
                     if (!s) return fail(s);
                 }
                 auto info = as.describe(*ae);
                 if (!info) return ToolResult::error(info.error());
                 return ToolResult::json(*info, editing ? "animator updated (editor preview)" : "animator updated (live)");
             }});

    reg.add({"bone_attach", "Attach to bone",
             "Make an entity follow a bone of an animated character: a sword in the right hand, a hat on the head, a "
             "lantern on the hip. Validates the bone (did-you-mean; animation_list bones=true lists them), adds the "
             "`attach` component and (by default) parents the entity under the character. offset/rotation are in the "
             "bone's space. Example: {\"entity\": \"Sword\", \"to\": \"Hero\", \"bone\": \"RightHand\", \"offset\": [0, 0.08, 0.02]}.",
             "animation",
             object({{"entity", entity("The prop to attach")},
                     {"to", entity("The character (entity with the animator)")},
                     {"bone", string("Bone name, e.g. RightHand, Head, mixamorig:Spine2")},
                     {"offset", vec3("Position in bone space (m)")},
                     {"rotation", vec3("Rotation in bone space (Euler degrees)")},
                     {"follow_scale", boolean("Inherit the bone's scale (default false)")},
                     {"parent", boolean("Parent the prop under the character (default true)")}},
                    {"entity", "to", "bone"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto prop = resolve(engine, a.get("entity"));
                 if (!prop) return ToolResult::error(prop.error());
                 auto ae = animatorOf(engine, a.get("to"));
                 if (!ae) return ToolResult::error(ae.error());
                 if (*prop == *ae) return ToolResult::error(Error::make("invalid_arguments", "an entity cannot follow its own bone"));
                 auto bw = engine.animation().boneWorld(*ae, a.get("bone").asString());
                 if (!bw) return ToolResult::error(bw.error());
                 Scene& s = engine.scene();
                 std::string targetRef;
                 bool parent = a.get("parent").asBool(true);
                 if (!parent) targetRef = s.find(s.record(*ae)->name) == *ae ? s.record(*ae)->name : formatEntityRef(*ae);
                 Status st = engine.edit(ctx.actor, "Attach " + s.record(*prop)->name + " to " + a.get("bone").asString(), [&]() -> Status {
                     if (parent) {
                         if (Status r = s.setParent(*prop, *ae); !r) return r;
                     }
                     Json c = Json::object({{"character", targetRef}, {"bone", a.get("bone")}});
                     if (a.contains("offset")) c["offset"] = a.get("offset");
                     if (a.contains("rotation")) c["rotation"] = a.get("rotation");
                     if (a.contains("follow_scale")) c["followScale"] = a.get("follow_scale");
                     return s.patchComponent(*prop, "attach", c);
                 });
                 if (!st) return fail(st);
                 Vec3 p = bw->translation();
                 return ToolResult::json(Json::object({{"entity", *prop}, {"animator", *ae}, {"bone", a.get("bone")}, {"boneWorld", reflect::vec3ToJson(p)}}),
                                         "attached " + s.record(*prop)->name + " to " + a.get("bone").asString());
             }});

    reg.add({"bone_ik", "Reach with IK",
             "Make a character's hand or foot reach a point with two-bone IK (the elbow/knee and shoulder/hip bend; "
             "bone lengths are kept): a hand on a door handle, rail or lever, a foot planted on a step. Either give "
             "`entity` (an existing object becomes the effector — the bone reaches it) or a `position` (a new effector "
             "entity is created under the character; default: where the bone is now). Move or keyframe the effector "
             "(sequence_key transform.position) to animate the reach; `weight` blends with the animation. "
             "pole = bend direction in the character's space ([0,0,-1] knees forward; elbows usually [0,0,1]). "
             "Example: {\"character\": \"Hero\", \"bone\": \"RightHand\", \"entity\": \"Door Handle\"}.",
             "animation",
             object({{"character", entity("The character (entity with the animator)")},
                     {"bone", string("End bone: LeftHand, RightHand, LeftFoot, RightFoot...")},
                     {"entity", entity("Existing entity to reach (it gets the ik component)")},
                     {"position", vec3("World position for a new effector entity")},
                     {"name", string("Name of the new effector (default \"<bone> IK\")")},
                     {"weight", number("0..1 (default 1)")},
                     {"pole", vec3("Bend direction in the character's space (default: keep the animated bend)")},
                     {"match_rotation", boolean("Also orient the bone like the effector (default false)")}},
                    {"character", "bone"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto ae = animatorOf(engine, a.get("character"));
                 if (!ae) return ToolResult::error(ae.error());
                 std::string bone = a.get("bone").asString();
                 auto bw = engine.animation().boneWorld(*ae, bone);
                 if (!bw) return ToolResult::error(bw.error());
                 auto lib = engine.animation().libraryOf(*ae);
                 if (!lib) return ToolResult::error(lib.error());
                 int b = (*lib)->skeleton.find(bone);
                 int mid = b >= 0 ? (*lib)->skeleton.bones[static_cast<size_t>(b)].parent : -1;
                 if (mid < 0 || (*lib)->skeleton.bones[static_cast<size_t>(mid)].parent < 0) {
                     return ToolResult::error(Error::make("invalid_bone", "\"" + bone + "\" needs a parent and a grandparent bone (e.g. a hand or a foot)"));
                 }
                 Scene& s = engine.scene();
                 EntityId effector = kNoEntity;
                 Status st = engine.edit(ctx.actor, "IK " + bone, [&]() -> Status {
                     Json c = Json::object({{"bone", bone}});
                     if (a.contains("entity")) {
                         auto id = resolve(engine, a.get("entity"));
                         if (!id) return id.error();
                         if (*id == *ae) return Error::make("invalid_arguments", "the character cannot be its own IK target");
                         effector = *id;
                         c["character"] = s.find(s.record(*ae)->name) == *ae ? s.record(*ae)->name : formatEntityRef(*ae);
                     } else {
                         effector = s.create(a.get("name").asString(bone + " IK"), *ae);
                         Vec3 world = bw->translation();
                         reflect::jsonToVec3(a.get("position"), world);
                         Vec3 local = s.worldMatrix(*ae).inverse().transformPoint(world);
                         if (Status r = s.patchComponent(effector, "transform", Json::object({{"position", reflect::vec3ToJson(local)}})); !r) return r;
                     }
                     if (a.contains("weight")) c["weight"] = a.get("weight");
                     if (a.contains("pole")) c["pole"] = a.get("pole");
                     if (a.contains("match_rotation")) c["matchRotation"] = a.get("match_rotation");
                     return s.patchComponent(effector, "ik", c);
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"effector", effector}, {"animator", *ae}, {"bone", bone},
                                                       {"effectorWorld", reflect::vec3ToJson(s.worldMatrix(effector).translation())}}),
                                         bone + " reaches " + formatEntityRef(effector) + " (" + s.record(effector)->name + ")");
             }});

    addSequenceTools(engine, reg);
}

}  // namespace sky::tools
