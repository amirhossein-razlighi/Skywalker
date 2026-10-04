// Character tools (docs/CHARACTERS.md):
//   character_inspect   skeleton map, clips with root motion, IK status, groom attachment, material models
//   character_ik        foot planting, pelvis, hand targets and turn in place (the characterIk component)
//   animation_retarget  pose-space retargeting of clips between different humanoid rigs (.anim output)

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "ToolHelpers.h"
#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/anim/Retarget.h"
#include "skywalker/core/Strings.h"
#include "skywalker/fx/Groom.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

float r3(float v) { return std::round(v * 1000.f) / 1000.f; }

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
Json v3(Vec3 v) { return reflect::vec3ToJson({r3(v.x), r3(v.y), r3(v.z)}); }

const char* shadingName(Shading s) {
    switch (s) {
        case Shading::Pbr: return "pbr";
        case Shading::Toon: return "toon";
        case Shading::Unlit: return "unlit";
        case Shading::Water: return "water";
        case Shading::Skin: return "skin";
        case Shading::Eye: return "eye";
        case Shading::Cloth: return "cloth";
        case Shading::HairCard: return "hair_card";
    }
    return "pbr";
}

Result<EntityId> characterOf(Engine& engine, const Json& ref) {
    auto id = resolve(engine, ref);
    if (!id) return id.error();
    EntityId ae = engine.animation().animatorFor(*id);
    if (!ae) {
        return Error::make("no_animator", "entity " + formatEntityRef(*id) + " has no animator (nor do its parents)",
                           "import a rigged glTF with asset_import, instantiate a character prefab, or run animator_setup");
    }
    return ae;
}

/// A library from a path (.anim / rigged .glb) or from an entity's animator.
Result<std::shared_ptr<const anim::Library>> libraryFrom(Engine& engine, const Json& ref, std::string* path) {
    anim::AnimationSystem& as = engine.animation();
    if (ref.isString()) {
        std::string p = ref.asString();
        std::string lower = str::lower(p);
        if (endsWith(lower, ".anim") || endsWith(lower, ".glb") || endsWith(lower, ".gltf")) {
            if (path) *path = p;
            return as.library(p);
        }
    }
    auto ae = characterOf(engine, ref);
    if (!ae) return ae.error();
    if (path) {
        const Animator* a = engine.scene().get<Animator>(*ae);
        *path = a ? a->library : "";
    }
    return as.libraryOf(*ae);
}

Json groomAttachment(Engine& engine, EntityId groomEntity, const Json& rendererGrooms) {
    Scene& s = engine.scene();
    const Groom* g = s.get<Groom>(groomEntity);
    Json j = Json::object({{"entity", groomEntity}, {"name", s.record(groomEntity)->name}, {"preset", g->preset}, {"attach", g->attach}});
    auto view = engine.grooms().roots(
        s, groomEntity, [&engine](const std::string& k) { return engine.cpuMesh(k); },
        [&engine](const std::string& p) { return engine.resolvePath(p); });
    if (!view) {
        j["error"] = view.error().message;
        return j;
    }
    j["followsSkin"] = view->skinned;
    j["strands"] = view->children;
    j["boundRoots"] = view->bound;
    j["guides"] = view->roots.size();
    if (view->bindError > 0.f) j["bindErrorM"] = r3(view->bindError);
    for (const auto& item : rendererGrooms.elements()) {
        if (item.get("entity").asInt() != static_cast<int64_t>(groomEntity)) continue;
        j["drawn"] = item.get("drawn");
        j["colliders"] = item.get("colliders");
        if (item.contains("skinned")) j["rootSource"] = item.get("skinned").get("roots");
    }
    if (!view->skinned && g->attach != "rigid") {
        j["warning"] = view->bound == 0 ? "the roots are not bound to a mesh (imported without a target mesh): they follow the entity only"
                                        : "the target mesh is not animated (no animator above it, or not skinned): roots follow the entity";
    }
    return j;
}

}  // namespace

void addCharacterTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"character_inspect", "Inspect character",
             "Everything about a character's animation and look in one call: the humanoid bone map of its skeleton "
             "(slots like hips, leftHand, rightFoot with the bone names found, convention mixamo / ue / generic, missing "
             "slots), key bone positions in the world, clips with their root speed (m/s) and root turn (degrees per cycle), "
             "root motion / in-place / root yaw settings, retargeting (retargetFrom and how its clips map), foot and hand IK "
             "status of the last solve (contact, locked, ground offset, slope, pelvis drop, hand reach error), hair and fur "
             "attachment (follows the skin?, bound roots, colliders), and the material model of every mesh part (skin, eye, "
             "cloth, hair_card, pbr). Start here before retargeting, adding IK or grooming a character. "
             "Example: {\"entity\": \"Hero\"}.",
             "animation",
             object({{"entity", entity("The character (or any of its parts)")},
                     {"clips", boolean("Include the clip list (default true)")}},
                    {"entity"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto ae = characterOf(engine, a.get("entity"));
                 if (!ae) return ToolResult::error(ae.error());
                 anim::AnimationSystem& as = engine.animation();
                 Scene& s = engine.scene();
                 const Animator* an = s.get<Animator>(*ae);
                 Json j = Json::object({{"entity", *ae}, {"name", s.record(*ae)->name}});
                 Json warnings = Json::array();
                 auto status = as.characterStatus(*ae);
                 if (!status) return ToolResult::error(status.error());
                 if (status->contains("error")) {
                     j["error"] = status->get("error");
                     return ToolResult::json(j, "the animator has no usable library: " + status->get("error").asString());
                 }
                 auto lib = as.libraryOf(*ae);
                 j["library"] = an->library;
                 j["controller"] = an->controller;
                 if (auto d = as.describe(*ae)) j["state"] = d->get("state");
                 j["humanoid"] = status->get("humanoid");
                 // Key bones (world): where to aim cameras, masks and props.
                 Json keys = Json::object();
                 const Json& bones = status->get("humanoid").get("bones");
                 for (const char* slot : {"hips", "head", "neck", "leftHand", "rightHand", "leftFoot", "rightFoot"}) {
                     std::string bone = bones.get(slot).asString();
                     if (bone.empty()) continue;
                     if (auto w = as.boneWorld(*ae, bone)) keys[slot] = v3(w->translation());
                 }
                 j["keyBones"] = keys;
                 j["rootMotion"] = Json::object({{"rootMotion", an->rootMotion}, {"rootYaw", an->rootYaw}, {"inPlace", an->inPlace}});
                 if (lib && a.get("clips").asBool(true)) {
                     Mat4 t = as.modelTransform(*ae);
                     Vec3 up = normalize(t.inverse().transformDir({0, 1, 0}));
                     float scale = length(t.transformDir(up)) * length(s.worldMatrix(*ae).transformDir({0, 1, 0}));
                     Json clips = Json::array();
                     for (const auto& c : (*lib)->clips) {
                         Json cj = Json::object({{"name", c.name}, {"duration", r3(c.duration)}});
                         float speed = anim::rootSpeed(**lib, c, up) * scale;
                         float turn = degrees(anim::rootTurn(**lib, c, up));
                         if (speed > 0.05f) cj["rootSpeed"] = r3(speed);
                         if (std::fabs(turn) > 2.f) cj["rootTurnDeg"] = std::round(turn);
                         clips.push(std::move(cj));
                         if (clips.size() >= 80) break;
                     }
                     j["clips"] = clips;
                     if ((*lib)->clips.size() > 80) j["clipsTruncated"] = (*lib)->clips.size();
                 }
                 if (!an->retargetFrom.empty()) {
                     Json rt = Json::object({{"retargetFrom", an->retargetFrom}, {"mode", an->retarget}});
                     auto src = as.library(an->retargetFrom);
                     if (src && lib) {
                         rt["method"] = as.retargetMethod(**src, **lib, an->retarget);
                         if (rt.get("method").asString() == "pose") {
                             if (auto setup = as.retargetSetup(*src, *lib)) {
                                 rt["scale"] = r3((*setup)->scale);
                                 Json w = Json::array();
                                 for (const auto& x : (*setup)->warnings) w.push(x);
                                 rt["warnings"] = w;
                             } else {
                                 rt["error"] = setup.error().message;
                             }
                         }
                     } else if (!src) {
                         rt["error"] = src.error().message;
                     }
                     j["retarget"] = rt;
                 }
                 j["ik"] = status->get("ik");
                 if (!s.get<CharacterIk>(*ae)) warnings.push("no characterIk: feet will not plant on uneven ground (character_ik {feet: true})");
                 if (!status->get("humanoid").get("complete").asBool(false)) {
                     warnings.push("the skeleton is not a complete humanoid: foot / hand IK and pose-space retargeting need the "
                                   "missing slots (map them with animation_retarget source_map / target_map)");
                 }
                 // Parts: meshes below the animator, their material model and skinning.
                 std::vector<EntityId> subtree;
                 collectSubtree(s, *ae, subtree);
                 Json parts = Json::array();
                 std::vector<EntityId> meshEntities;
                 for (EntityId id : subtree) {
                     const MeshRenderer* m = s.get<MeshRenderer>(id);
                     if (!m) continue;
                     meshEntities.push_back(id);
                     Json p = Json::object({{"entity", id}, {"name", s.record(id)->name}, {"mesh", m->mesh}});
                     const MeshData* md = engine.cpuMesh(m->mesh);
                     if (md) {
                         p["vertices"] = md->vertexCount();
                         p["skinned"] = md->skinned();
                     }
                     if (!m->visible) p["visible"] = false;
                     // World bounds in the current pose (frame close-ups of eyes, hands, the face).
                     Aabb b = md ? md->bounds : s.localBounds(id);
                     if (const SkinPose* sp = md && md->skinned() ? as.skin(id, m->mesh) : nullptr) b = sp->bounds;
                     b = b.transformed(s.worldMatrix(id));
                     p["bounds"] = Json::object({{"min", v3(b.min)}, {"max", v3(b.max)}});
                     if (!m->material.empty()) {
                         p["material"] = m->material;
                         if (const ResolvedMaterial* rm = engine.resolveMaterial(m->material)) p["shading"] = shadingName(rm->shading);
                     }
                     parts.push(std::move(p));
                 }
                 j["parts"] = parts;
                 // Grooms on any of the character's meshes.
                 Json grooms = Json::array();
                 Json rendererGrooms = engine.renderer().stats().get("grooms");
                 for (EntityId id : s.entities()) {
                     if (!s.get<Groom>(id)) continue;
                     EntityId target = fx::groomMeshEntity(s, id);
                     if (std::find(meshEntities.begin(), meshEntities.end(), target) == meshEntities.end()) continue;
                     grooms.push(groomAttachment(engine, id, rendererGrooms));
                 }
                 j["grooms"] = grooms;
                 size_t capsules = 0;
                 for (EntityId id : meshEntities) {
                     const MeshRenderer* m = s.get<MeshRenderer>(id);
                     const MeshData* md = engine.cpuMesh(m->mesh);
                     if (md && md->skinned()) capsules = std::max(capsules, as.bodyColliders(id, m->mesh).size());
                 }
                 j["bodyColliders"] = capsules;
                 for (const auto& w : status->get("humanoid").get("warnings").elements()) warnings.push(w);
                 j["warnings"] = warnings;
                 std::string sum = s.record(*ae)->name + ": " + status->get("humanoid").get("convention").asString() + " rig (" +
                                   (status->get("humanoid").get("complete").asBool() ? "complete humanoid" : "not a complete humanoid") +
                                   "), " + std::to_string(parts.size()) + " parts, " + std::to_string(grooms.size()) + " groom(s)";
                 return ToolResult::json(j, sum);
             }});

    reg.add({"character_ik", "Character foot and hand IK",
             "Set up automatic foot IK, hand targets and turning for a humanoid character (adds or updates its characterIk "
             "component; undoable). Feet plant on stairs, slopes and rocks under them (ground probes every tick), the pelvis "
             "drops so the lower foot reaches, feet pitch / roll onto the slope (max_slope) and lock in place while in contact "
             "(contact: auto = low and slow, velocity, events = foot_l_down / foot_l_up / foot_r_down / foot_r_up animation "
             "events, always, never), re-planting with a short step once the animation pulls a foot lock_distance away. Hands "
             "reach entities: a grip point on a staff held in the other hand (no lag), a ledge, a rail; animate the weights "
             "for grabs. turn_to turns a standing character in place (yaw degrees, or an entity / point to face). Returns the "
             "solve status. Example: {\"entity\": \"Monk\", \"feet\": true, \"left_hand\": \"Staff Grip\"}.",
             "animation",
             object({{"entity", entity("The character")},
                     {"feet", boolean("Automatic foot planting (default true when the component is created)")},
                     {"feet_weight", number("0..1 blend of the foot IK")},
                     {"step_height", number("Meters a foot may reach up or down (stairs: 0.3-0.5)")},
                     {"foot_height", number("Ankle height above the sole in meters (-1 = from the rest pose)")},
                     {"pelvis", boolean("Lower the hips for the lower foot")},
                     {"align_feet", boolean("Tilt the feet onto the slope")},
                     {"max_slope", number("Steepest slope the feet align to (degrees)")},
                     {"contact", enumeration({"auto", "velocity", "events", "always", "never"}, "When a foot locks in place")},
                     {"lock_speed", number("m/s below which a foot counts as planted")},
                     {"lock_distance", number("m a locked foot may lag before re-planting")},
                     {"smoothing", number("1/s: how fast pelvis and feet follow the ground")},
                     {"left_hand", any("Entity the left hand reaches (\"\" clears)")},
                     {"left_hand_weight", number("0..1")},
                     {"right_hand", any("Entity the right hand reaches (\"\" clears)")},
                     {"right_hand_weight", number("0..1")},
                     {"hand_rotation", boolean("Also orient the hands like their targets")},
                     {"turn_speed", number("Turn-in-place rate (degrees per second)")},
                     {"turn_to", any("Turn in place: world yaw in degrees (0 = -Z), or an entity / [x, y, z] point to face")},
                     {"remove", boolean("Remove the characterIk component")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto ae = characterOf(engine, a.get("entity"));
                 if (!ae) return ToolResult::error(ae.error());
                 Scene& s = engine.scene();
                 Json patch = Json::object();
                 const std::pair<const char*, const char*> fields[] = {
                     {"feet", "feet"}, {"feet_weight", "feetWeight"}, {"step_height", "stepHeight"}, {"foot_height", "footHeight"},
                     {"pelvis", "pelvis"}, {"align_feet", "alignFeet"}, {"max_slope", "maxSlope"}, {"contact", "contact"},
                     {"lock_speed", "lockSpeed"}, {"lock_distance", "lockDistance"}, {"smoothing", "smoothing"},
                     {"left_hand_weight", "leftHandWeight"}, {"right_hand_weight", "rightHandWeight"}, {"hand_rotation", "handRotation"},
                     {"turn_speed", "turnSpeed"}};
                 for (auto [arg, field] : fields) {
                     if (a.contains(arg)) patch[field] = a.get(arg);
                 }
                 for (auto [arg, field] : {std::pair{"left_hand", "leftHand"}, std::pair{"right_hand", "rightHand"}}) {
                     if (!a.contains(arg)) continue;
                     const Json& v = a.get(arg);
                     if (v.isNull() || (v.isString() && v.asString().empty())) {
                         patch[field] = Json();
                         continue;
                     }
                     auto t = resolve(engine, v);
                     if (!t) return ToolResult::error(t.error());
                     patch[field] = static_cast<int64_t>(*t);
                 }
                 Status st = engine.edit(ctx.actor, "Character IK " + s.record(*ae)->name, [&]() -> Status {
                     if (a.get("remove").asBool(false)) {
                         if (const ComponentKind* k = s.componentKind("characterIk"); k && k->has(s, *ae)) k->remove(s, *ae);
                         return {};
                     }
                     return s.patchComponent(*ae, "characterIk", patch);
                 });
                 if (!st) return fail(st);
                 if (a.contains("turn_to")) {
                     const Json& t = a.get("turn_to");
                     float yaw = 0.f;
                     Vec3 p;
                     if (t.isNumber()) {
                         yaw = t.asFloat();
                     } else {
                         if (!reflect::jsonToVec3(t, p)) {
                             auto id = resolve(engine, t);
                             if (!id) return ToolResult::error(id.error());
                             p = s.worldMatrix(*id).translation();
                         }
                         Vec3 to = p - s.worldMatrix(*ae).translation();
                         yaw = degrees(std::atan2(-to.x, -to.z));
                     }
                     if (Status r = engine.animation().turnInPlace(*ae, yaw); !r) return fail(r);
                 }
                 auto status = engine.animation().characterStatus(*ae);
                 if (!status) return ToolResult::error(status.error());
                 Json j = Json::object({{"entity", *ae}, {"ik", status->get("ik")}, {"humanoid", status->get("humanoid").get("complete")}});
                 if (const Json* tt = status->find("turnTarget")) j["turnTarget"] = *tt;
                 if (const CharacterIk* c = s.get<CharacterIk>(*ae)) j["component"] = reflect::toJson(c, CharacterIk::type());
                 return ToolResult::json(j, a.get("remove").asBool(false) ? "characterIk removed" : "characterIk updated on " + s.record(*ae)->name);
             }});

    reg.add({"animation_retarget", "Retarget animations",
             "Retarget clips from one humanoid rig to another in pose space and save them as a new library (.anim) on the "
             "target's skeleton: different bone names (mixamo / UE / generic, detected by name and topology), rest poses "
             "(T-pose vs A-pose), bone axes and proportions (the hips move by the source motion scaled by the leg-length "
             "ratio; bones keep their own lengths, so nothing stretches). `source` / `target` are a library (.anim or a rigged "
             ".glb) or a character entity. preview=true only reports the bone maps and alignment. Use the result with "
             "\"<output>#<Clip>\" clip references in controllers, play_animation, or animator.retargetFrom (which retargets "
             "on the fly). Example: {\"source\": \"anims/pack.anim\", \"target\": \"Knight\", \"clips\": [\"Walk\", \"Run\"]}.",
             "animation",
             object({{"source", any("Library path (.anim / .glb) or character entity with the clips")},
                     {"target", any("Library path or character entity to retarget onto")},
                     {"clips", array(string("Clip name"), "Clips to retarget (default: all)")},
                     {"output", string("Output .anim path (default: next to the target library, <target>_<source>.anim)")},
                     {"fps", number("Resampling rate of the output clips (default 30)")},
                     {"translation", boolean("Move the hips / root with the source (default true; false keeps them at rest)")},
                     {"source_map", Json::object({{"type", "object"}, {"description", "Slot overrides for the source, e.g. {\"leftHand\": \"L_Wrist\"}"}})},
                     {"target_map", Json::object({{"type", "object"}, {"description", "Slot overrides for the target"}})},
                     {"preview", boolean("Only report the bone maps and alignment (no file)")}},
                    {"source", "target"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 std::string srcPath, dstPath;
                 auto src = libraryFrom(engine, a.get("source"), &srcPath);
                 if (!src) return ToolResult::error(src.error());
                 auto dst = libraryFrom(engine, a.get("target"), &dstPath);
                 if (!dst) return ToolResult::error(dst.error());
                 anim::RetargetOptions opt;
                 opt.fps = a.get("fps").asFloat(30.f);
                 opt.translation = a.get("translation").asBool(true);
                 opt.sourceMap = a.get("source_map");
                 opt.targetMap = a.get("target_map");
                 auto setup = anim::prepareRetarget((*src)->skeleton, (*src)->rootBone, (*dst)->skeleton, (*dst)->rootBone, opt);
                 Json j = Json::object();
                 if (!setup) {
                     j["source"] = anim::detectHumanoid((*src)->skeleton).toJson((*src)->skeleton);
                     j["target"] = anim::detectHumanoid((*dst)->skeleton).toJson((*dst)->skeleton);
                     ToolResult r = ToolResult::error(setup.error());
                     r.structured = j;
                     return r;
                 }
                 j = setup->toJson((*src)->skeleton, (*dst)->skeleton);
                 if (a.get("preview").asBool(false)) return ToolResult::json(j, "bone maps ready (preview, nothing written)");
                 std::vector<std::string> names;
                 for (const auto& c : a.get("clips").elements()) names.push_back(c.asString());
                 if (names.empty()) names = (*src)->clipNames();
                 anim::Library out;
                 out.skeleton = (*dst)->skeleton;
                 out.rootBone = (*dst)->rootBone;
                 out.source = "retargeted from " + (srcPath.empty() ? std::string("a character") : srcPath);
                 Json clips = Json::array();
                 for (const auto& n : names) {
                     const anim::Clip* c = (*src)->clip(n);
                     if (!c) {
                         std::string near = str::closest(n, (*src)->clipNames(), 4);
                         return ToolResult::error(Error::make("unknown_clip", "no clip \"" + n + "\" in the source",
                                                              near.empty() ? "animation_list {model: <source>} lists them" : "did you mean \"" + near + "\"?"));
                     }
                     anim::Clip rc = anim::retargetClipPose(*setup, (*src)->skeleton, *c, (*dst)->skeleton, opt);
                     float stretch = anim::retargetStretch((*dst)->skeleton, rc);
                     clips.push(Json::object({{"name", rc.name}, {"duration", r3(rc.duration)}, {"channels", rc.channels.size()}, {"stretch", r3(stretch)}}));
                     out.clips.push_back(std::move(rc));
                 }
                 std::string output = a.get("output").asString();
                 if (output.empty()) {
                     fs::path dir = dstPath.empty() ? fs::path("animations") : fs::path(dstPath).parent_path();
                     std::string dstStem = dstPath.empty() ? "character" : fs::path(dstPath).stem().string();
                     std::string srcStem = srcPath.empty() ? "clips" : fs::path(srcPath).stem().string();
                     output = (dir / (dstStem + "_" + srcStem + ".anim")).generic_string();
                 }
                 if (!endsWith(str::lower(output), ".anim")) {
                     return ToolResult::error(Error::make("invalid_path", "output must end in .anim", "e.g. characters/knight_walks.anim"));
                 }
                 std::error_code ec;
                 fs::create_directories(fs::path(engine.resolvePath(output)).parent_path(), ec);
                 if (Status w = anim::saveLibrary(engine.resolvePath(output), out); !w) return fail(w);
                 engine.animation().invalidate(output);
                 (void)engine.assets().registerFile(engine.resolvePath(output));
                 j["output"] = output;
                 j["clips"] = clips;
                 j["use"] = "play_animation(self, \"" + output + "#" + (out.clips.empty() ? std::string("Clip") : out.clips.front().name) +
                            "\") or controller motions \"" + output + "#<Clip>\"";
                 return ToolResult::json(j, "retargeted " + std::to_string(out.clips.size()) + " clip(s) to " + output);
             }});
}

}  // namespace sky::tools
