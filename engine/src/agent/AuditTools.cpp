// scene_audit: the look-dev quality gate. Builds the frame a camera would render (any view, or
// sequence shots at given times) and audits it on the CPU: visible builtin primitives with their
// screen coverage, primitive "characters", default / untextured materials, missing textures,
// LODs, texel density, sky and shadows. See render/SceneAudit.h and docs/RENDERING.md.

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/SceneAudit.h"

namespace sky::tools {

namespace {

using namespace schema;

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string fileStem(const std::string& path) { return std::filesystem::path(path).stem().stem().string(); }

Status customView(Engine& engine, const Json& cam, CaptureOptions& o) {
    Vec3 eye, target;
    if (!reflect::jsonToVec3(cam.get("eye"), eye)) {
        return Error::make("invalid_argument", "camera.eye must be [x, y, z]",
                           "e.g. {\"camera\": {\"eye\": [0, 1.6, 6], \"target\": [0, 1, 0], \"fov\": 45}}");
    }
    o.hasCustomView = true;
    o.customView = engine.camera().toView();
    if (!reflect::jsonToVec3(cam.get("target"), target)) target = o.customView.target;
    o.customView.lookFrom(eye, target);
    if (cam.contains("fov")) o.customView.fovDeg = std::clamp(cam.get("fov").asFloat(), 5.f, 150.f);
    return {};
}

/// The entity playing a sequence: an entity reference, or the sequencer that plays a path.
Result<EntityId> sequencePlayer(Engine& engine, const Json& ref) {
    Scene& s = engine.scene();
    if (ref.isString() && endsWith(ref.asString(), ".sequence.json")) {
        std::vector<std::string> known;
        for (EntityId e : s.entities()) {
            if (const SequencePlayer* sp = s.get<SequencePlayer>(e)) {
                if (sp->sequence == ref.asString()) return e;
                known.push_back(sp->sequence);
            }
        }
        std::string near = str::closest(ref.asString(), known, 8);
        return Error::make("no_sequencer", "no entity in the scene plays " + ref.asString(),
                           near.empty() ? "add a sequencer (sequence_create entity) or pass a sequencer entity"
                                        : "did you mean " + near + "?");
    }
    auto id = resolve(engine, ref);
    if (!id) return id.error();
    const SequencePlayer* sp = s.get<SequencePlayer>(*id);
    if (!sp || sp->sequence.empty()) {
        return Error::make("no_sequencer", "entity " + formatEntityRef(*id) + " has no sequencer with a sequence",
                           "pass a .sequence.json path played by an entity in this scene");
    }
    return *id;
}

/// Kit prefabs for fix hints: tagged kit-character / kit-prop, or under */characters/ and */props/ of a kit.
void kitPrefabs(Engine& engine, audit::Sources& src) {
    AssetDatabase::Query q;
    q.type = AssetType::Prefab;
    q.limit = 2000;
    for (const AssetRecord* r : engine.assets().query(q)) {
        const auto& tags = r->tags;
        auto tagged = [&](const char* t) { return std::find(tags.begin(), tags.end(), t) != tags.end(); };
        if (tagged("kit-character") || r->path.find("kit/characters/") != std::string::npos) {
            src.kitCharacters.push_back(r->path);
        } else if (tagged("kit-prop") || r->path.find("kit/props/") != std::string::npos) {
            src.kitProps.push_back(r->path);
        }
    }
    std::sort(src.kitCharacters.begin(), src.kitCharacters.end());
    std::sort(src.kitProps.begin(), src.kitProps.end());
}

std::string summaryLine(const Json& v) {
    std::ostringstream os;
    os << (v.get("pass").asBool() ? "PASS" : "FAIL");
    if (v.contains("view")) os << " [" << v.get("view").asString() << "]";
    os << ": primitives " << v.get("primitiveCoverage").asFloat() << "% (" << v.get("primitiveCount").asInt() << ")";
    os << ", primitive characters " << v.get("primitiveCharacters").size();
    size_t def = 0, flat = 0;
    for (const auto& m : v.get("materials").elements()) (m.get("issue").asString() == "default" ? def : flat)++;
    os << ", default materials " << def << ", untextured " << flat;
    size_t errors = 0, warnings = 0;
    for (const auto& w : v.get("warnings").elements()) (w.get("severity").asString() == "error" ? errors : warnings)++;
    os << ", " << errors << " error(s), " << warnings << " other finding(s)";
    return os.str();
}

}  // namespace

void addAuditTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"scene_audit", "Audit what the camera sees",
             "Quality gate before filming or screenshots (read-only, CPU, no GPU needed). Builds the frame a camera sees and "
             "reports: visible builtin PRIMITIVE meshes (cube, sphere, capsule, cylinder, plane, cone, quad, torus) with entity "
             "and screen coverage % (occlusion-aware: a ground plane hidden under terrain or water does not count); "
             "PRIMITIVE CHARACTERS (an animator / character controller / nav agent on primitive meshes, or primitive parts "
             "arranged like a body, e.g. a sphere head on a capsule) with a kit character to replace them; DEFAULT grey or "
             "UNTEXTURED materials on visible meshes; missing texture / material / HDRI files; big meshes without LODs; "
             "texel-density outliers; a default sky; lights without shadows on hero meshes. Returns `pass` and `warnings` "
             "[{severity, code, message, hint, entity}]. Views: `camera` {eye, target, fov}, several `cameras`, "
             "`camera_entity`, `view` (editor | scene), or a `sequence` (path or sequencer entity) at `times` (seconds; "
             "default: 12 evenly spaced shots). strict=true fails when primitives (or default materials) cover more than "
             "0.1% of any view (default limit 2%); override with max_primitive_coverage (percent). stylized=true accepts "
             "flat-colored materials (a deliberate stylized look). Examples: {\"view\": \"scene\", \"strict\": true}, "
             "{\"sequence\": \"sequences/hero.sequence.json\", \"times\": [1, 4, 8], \"strict\": true}, {\"camera\": "
             "{\"eye\": [0, 1.7, 5], \"target\": [0, 1.2, 0], \"fov\": 40}}.",
             "view",
             object({{"camera", Json::object({{"type", "object"}, {"description", "Custom view {eye: [x,y,z], target: [x,y,z], fov: degrees}"}})},
                     {"cameras", array(Json::object({{"type", "object"}}), "Several custom views [{eye, target, fov, label?}]")},
                     {"camera_entity", schema::entity("Audit through this camera entity")},
                     {"view", enumeration({"editor", "scene"}, "Editor camera (default) or the scene's primary camera")},
                     {"sequence", any("Sequence path (.sequence.json) or an entity with a sequencer: audit its live camera")},
                     {"times", array(Json::object({{"type", "number"}}), "Sequence times in seconds (default: 12 shots across it)")},
                     {"strict", boolean("Fail when primitives or default materials cover more than 0.1% of a view (showcase bar)")},
                     {"max_primitive_coverage", number("Primitive coverage limit in percent (default 0.1 strict, 2 otherwise)")},
                     {"stylized", boolean("Deliberately stylized look: flat-colored materials are info, not warnings")},
                     {"width", integer("Frame width for the aspect ratio (default 1920)")},
                     {"height", integer("Frame height (default 1080)")},
                     {"resolution", integer("Coverage raster width in pixels (default 320, max 1280); higher = finer coverage")},
                     {"details", boolean("Include per-view details when auditing several views (default true)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 audit::Options opts;
                 opts.strict = a.get("strict").asBool(false);
                 opts.stylized = a.get("stylized").asBool(false);
                 opts.resolution = static_cast<int>(std::clamp<int64_t>(a.get("resolution").asInt(320), 32, 1280));
                 if (a.contains("max_primitive_coverage")) {
                     opts.maxPrimitiveCoverage = std::max(0.f, a.get("max_primitive_coverage").asFloat()) / 100.f;
                 }
                 audit::Sources src;
                 src.mesh = [&engine](const std::string& key) { return engine.cpuMesh(key); };
                 src.posed = [&engine](EntityId e, const std::string& key) { return engine.animation().posedMesh(e, key); };
                 src.resolvePath = [&engine](const std::string& p) { return engine.resolvePath(p); };
                 kitPrefabs(engine, src);

                 CaptureOptions base;
                 base.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(1920), 16, 8192));
                 base.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(1080), 16, 8192));
                 base.editorOverlays = false;
                 base.samples = 4;  // a still: every foliage chunk in range is generated
                 base.listVisible = false;

                 struct View {
                     std::string label;
                     CaptureOptions o;
                     EntityId player = kNoEntity;
                     float time = -1.f;
                 };
                 std::vector<View> views;
                 if (a.contains("sequence")) {
                     if (engine.playState() != PlayState::Editing) {
                         return ToolResult::error(Error::make("invalid_state", "sequence audits work while editing",
                                                              "sim_control stop, then audit; or audit the live view without `sequence`"));
                     }
                     auto player = sequencePlayer(engine, a.get("sequence"));
                     if (!player) return ToolResult::error(player.error());
                     const SequencePlayer* sp = engine.scene().get<SequencePlayer>(*player);
                     auto def = engine.animation().sequence(sp->sequence);
                     if (!def) return ToolResult::error(def.error());
                     std::vector<float> times;
                     for (const auto& t : a.get("times").elements()) times.push_back(std::max(0.f, t.asFloat()));
                     if (times.empty()) {
                         const float len = std::max(0.1f, (*def)->length());
                         for (int i = 0; i < 12; ++i) times.push_back(len * (static_cast<float>(i) + 0.5f) / 12.f);
                     }
                     if (times.size() > 64) return ToolResult::error(Error::make("too_many", "at most 64 times per call"));
                     for (float t : times) {
                         char label[64];
                         std::snprintf(label, sizeof(label), "%s @ %.2fs", fileStem(sp->sequence).c_str(), static_cast<double>(t));
                         views.push_back({label, base, *player, t});
                     }
                 } else if (a.contains("cameras")) {
                     int i = 0;
                     for (const auto& c : a.get("cameras").elements()) {
                         View v{c.get("label").asString("camera " + std::to_string(i++)), base};
                         if (Status s = customView(engine, c, v.o); !s) return fail(s);
                         views.push_back(std::move(v));
                     }
                     if (views.empty()) return ToolResult::error(Error::make("invalid_argument", "cameras is empty"));
                 } else {
                     View v{"", base};
                     if (a.contains("camera")) {
                         if (Status s = customView(engine, a.get("camera"), v.o); !s) return fail(s);
                         v.label = "camera";
                     } else if (a.contains("camera_entity")) {
                         auto id = resolve(engine, a.get("camera_entity"));
                         if (!id) return ToolResult::error(id.error());
                         v.o.cameraEntity = *id;
                         v.label = engine.scene().record(*id)->name;
                     } else {
                         v.o.useSceneCamera = a.get("view").asString() == "scene";
                         v.label = v.o.useSceneCamera ? "scene camera" : "editor camera";
                     }
                     views.push_back(std::move(v));
                 }

                 anim::AnimationSystem& as = engine.animation();
                 Json results = Json::array();
                 bool pass = true;
                 float worstPrimitive = 0.f;
                 std::map<std::string, Json> uniqueWarnings;  // code + entity -> first finding
                 std::map<EntityId, Json> characters;
                 for (auto& v : views) {
                     if (v.player) {
                         as.setSequenceScrub(v.player, v.time);
                         v.o.cameraEntity = as.sequenceCamera(v.player, v.time);
                     }
                     FrameData frame = engine.frame(v.o);
                     Json r = audit::auditFrame(engine.scene(), frame, src, opts, v.label);
                     if (v.player) as.clearSequenceScrub(v.player);
                     if (v.player && v.o.cameraEntity) r["cameraEntity"] = engine.scene().record(v.o.cameraEntity)->name;
                     pass = pass && r.get("pass").asBool();
                     worstPrimitive = std::max(worstPrimitive, r.get("primitiveCoverage").asFloat());
                     for (const auto& w : r.get("warnings").elements()) {
                         std::string key = w.get("code").asString() + "#" + std::to_string(w.get("entity").asInt());
                         if (!uniqueWarnings.count(key)) uniqueWarnings[key] = w;
                     }
                     for (const auto& c : r.get("primitiveCharacters").elements()) characters[static_cast<EntityId>(c.get("id").asInt())] = c;
                     results.push(std::move(r));
                 }
                 if (results.size() == 1) {
                     Json r = results[size_t{0}];
                     return ToolResult::json(r, summaryLine(r));
                 }
                 Json warnings = Json::array();
                 for (const char* sev : {"error", "warning", "info"}) {
                     for (const auto& [k, w] : uniqueWarnings) {
                         if (w.get("severity").asString() == sev) warnings.push(w);
                     }
                 }
                 Json chars = Json::array();
                 for (const auto& [id, c] : characters) chars.push(c);
                 std::ostringstream os;
                 os << (pass ? "PASS" : "FAIL") << " over " << results.size() << " views (worst primitive coverage "
                    << worstPrimitive << "%)\n";
                 for (const auto& r : results.elements()) os << "  " << summaryLine(r) << "\n";
                 Json out = Json::object({{"pass", pass},
                                          {"strict", opts.strict},
                                          {"views", results.size()},
                                          {"worstPrimitiveCoverage", worstPrimitive},
                                          {"primitiveCharacters", chars},
                                          {"warnings", warnings}});
                 if (a.get("details").asBool(true)) {
                     out["results"] = results;
                 } else {
                     Json brief = Json::array();
                     for (const auto& r : results.elements()) {
                         brief.push(Json::object({{"view", r.get("view")}, {"pass", r.get("pass")},
                                                  {"primitiveCoverage", r.get("primitiveCoverage")}}));
                     }
                     out["results"] = brief;
                 }
                 return ToolResult::json(out, os.str());
             }});
}

}  // namespace sky::tools
