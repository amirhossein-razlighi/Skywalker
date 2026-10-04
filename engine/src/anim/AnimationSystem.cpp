#include "skywalker/anim/AnimationSystem.h"

#include <algorithm>
#include <cmath>

#include "AnimationSystemInternal.h"
#include "skywalker/anim/Retarget.h"

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/Gltf.h"
#include "skywalker/render/MeshData.h"

namespace sky::anim {

namespace {

constexpr size_t kRecentEvents = 16;
constexpr float kLookRate = 6.f;  // look-at weight blend speed (per second)

std::string stripAsset(std::string ref) {
    if (str::startsWith(ref, "asset:")) ref = ref.substr(6);
    return ref;
}

std::string meshFileOf(const std::string& meshKey) {
    std::string f = stripAsset(meshKey);
    size_t hash = f.rfind('#');
    return hash == std::string::npos ? f : f.substr(0, hash);
}

bool hasExt(const std::string& path, const char* ext) {
    std::string p = str::lower(path);
    std::string e = ext;
    return p.size() >= e.size() && p.compare(p.size() - e.size(), e.size(), e) == 0;
}

std::string didYouMean(std::string_view word, const std::vector<std::string>& options) {
    std::string g = str::closest(word, options, 3);
    if (!g.empty()) return "did you mean \"" + g + "\"?";
    std::string all;
    for (size_t i = 0; i < options.size() && i < 30; ++i) all += (i ? ", " : "") + options[i];
    return options.empty() ? "" : "known: " + all;
}

/// Rotation-only (unit columns) copy of an affine matrix.
Mat4 withoutScale(const Mat4& m) {
    Mat4 r = m;
    for (int c = 0; c < 3; ++c) {
        Vec3 col{m.at(c, 0), m.at(c, 1), m.at(c, 2)};
        float len = length(col);
        if (len < 1e-12f) continue;
        for (int k = 0; k < 3; ++k) r.at(c, k) = m.at(c, k) / len;
    }
    return r;
}

int boneDepth(const Skeleton& sk, int b) {
    int d = 0;
    for (int p = sk.bones[static_cast<size_t>(b)].parent; p >= 0; p = sk.bones[static_cast<size_t>(p)].parent) ++d;
    return d;
}

/// Head, then neck and up to two spine bones above it (root-most first): the look-at chain.
std::vector<int> lookChain(const Skeleton& sk) {
    int head = -1;
    for (size_t i = 0; i < sk.bones.size(); ++i) {
        std::string n = str::lower(sk.bones[i].name);
        if (n.find("head") == std::string::npos) continue;
        if (n.find("end") != std::string::npos || n.find("top") != std::string::npos || n.find("nub") != std::string::npos ||
            n.find("tip") != std::string::npos) {
            continue;
        }
        if (head < 0 || boneDepth(sk, static_cast<int>(i)) < boneDepth(sk, head)) head = static_cast<int>(i);
    }
    if (head < 0) return {};
    std::vector<int> chain{head};
    int b = sk.bones[static_cast<size_t>(head)].parent;
    int spines = 0;
    while (b >= 0 && chain.size() < 4) {
        std::string n = str::lower(sk.bones[static_cast<size_t>(b)].name);
        bool neck = n.find("neck") != std::string::npos;
        bool spine = n.find("spine") != std::string::npos || n.find("chest") != std::string::npos;
        if (!neck && !(spine && spines < 2)) break;
        spines += spine;
        chain.insert(chain.begin(), b);
        b = sk.bones[static_cast<size_t>(b)].parent;
    }
    return chain;
}

}  // namespace

AnimationSystem::AnimationSystem(Scene& scene) : scene_(scene) {}
AnimationSystem::~AnimationSystem() = default;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AnimationSystem::reset() {
    animators_.clear();
    sequencers_.clear();
    scrubs_.clear();
}

void AnimationSystem::invalidate(const std::string& path) {
    if (path.empty()) {
        libraries_.clear();
        controllers_.clear();
        sequences_.clear();
    } else {
        libraries_.erase(path);
        controllers_.erase(path);
        sequences_.erase(path);
        // A re-imported model changes its library too.
        for (auto it = libraries_.begin(); it != libraries_.end();) {
            it = str::startsWith(it->first, path) ? libraries_.erase(it) : std::next(it);
        }
    }
    retargeted_.clear();
    retargetSetups_.clear();
    ++assetGeneration_;
}

void AnimationSystem::warnOnce(const std::string& key, const std::string& message) {
    if (warned_.emplace(key, true).second) log::warn("anim", message);
}

// ---------------------------------------------------------------------------
// Assets
// ---------------------------------------------------------------------------

Result<std::shared_ptr<const Library>> AnimationSystem::library(const std::string& refIn) {
    std::string ref = stripAsset(refIn);
    if (ref.empty()) return Error::make("not_found", "no animation library given");
    auto it = libraries_.find(ref);
    if (it == libraries_.end()) {
        std::string abs = hooks.resolvePath ? hooks.resolvePath(ref) : ref;
        std::shared_ptr<const Library> lib;
        std::string error;
        if (hasExt(ref, ".anim")) {
            auto r = loadLibrary(abs);
            if (r) lib = std::make_shared<Library>(std::move(*r));
            else error = r.error().message;
        } else if (hasExt(ref, ".glb") || hasExt(ref, ".gltf")) {
            // Prefer the library written at import; fall back to reading the model itself.
            std::string imported = hooks.libraryFor ? hooks.libraryFor(ref) : "";
            if (!imported.empty() && imported != ref) {
                auto r = library(imported);
                if (r) return r;
            }
            auto g = loadGltf(abs, false);
            if (g && g->animation) lib = g->animation;
            else error = g ? ref + " has no skeleton or animations" : g.error().message;
        } else {
            error = "\"" + ref + "\" is not an animation library (.anim) or a glTF model";
        }
        it = libraries_.emplace(ref, std::make_pair(lib, error)).first;
    }
    if (!it->second.first) return Error::make("invalid_animation", it->second.second, "use animation_list to see libraries and clips");
    return it->second.first;
}

Result<std::shared_ptr<const ControllerDef>> AnimationSystem::controller(const std::string& path) {
    auto it = controllers_.find(path);
    if (it == controllers_.end()) {
        auto r = loadController(hooks.resolvePath ? hooks.resolvePath(path) : path);
        std::shared_ptr<const ControllerDef> def = r ? std::make_shared<ControllerDef>(std::move(*r)) : nullptr;
        it = controllers_.emplace(path, std::make_pair(def, r ? std::string() : r.error().message)).first;
    }
    if (!it->second.first) return Error::make("invalid_controller", it->second.second);
    return it->second.first;
}

Result<std::shared_ptr<const SequenceDef>> AnimationSystem::sequence(const std::string& path) {
    auto it = sequences_.find(path);
    if (it == sequences_.end()) {
        auto r = loadSequence(hooks.resolvePath ? hooks.resolvePath(path) : path);
        std::shared_ptr<const SequenceDef> def = r ? std::make_shared<SequenceDef>(std::move(*r)) : nullptr;
        it = sequences_.emplace(path, std::make_pair(def, r ? std::string() : r.error().message)).first;
    }
    if (!it->second.first) return Error::make("invalid_sequence", it->second.second);
    return it->second.first;
}

std::string AnimationSystem::retargetMethod(const Library& src, const Library& dst, const std::string& mode) {
    if (src.skeleton.names() == dst.skeleton.names()) return "same";
    if (mode == "name") return "name";
    if (mode == "pose") return "pose";
    // auto: rigs of one family (every humanoid slot of the target found by name in the source) keep
    // the name mapping; different rigs go through pose space when both are humanoids.
    HumanoidMap tm = detectHumanoid(dst.skeleton);
    if (!tm.retargetable()) return "name";
    bool allByName = true;
    for (size_t i = 0; i < kHumanBones && allByName; ++i) {
        int b = tm.bones[i];
        if (b >= 0 && src.skeleton.find(dst.skeleton.bones[static_cast<size_t>(b)].name) < 0) allByName = false;
    }
    if (allByName) return "name";
    return detectHumanoid(src.skeleton).retargetable() ? "pose" : "name";
}

Result<std::shared_ptr<const RetargetSetup>> AnimationSystem::retargetSetup(const std::shared_ptr<const Library>& src,
                                                                            const std::shared_ptr<const Library>& dst) {
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(src.get())) + ">" + std::to_string(reinterpret_cast<uintptr_t>(dst.get()));
    auto it = retargetSetups_.find(key);
    if (it == retargetSetups_.end()) {
        auto r = prepareRetarget(src->skeleton, src->rootBone, dst->skeleton, dst->rootBone);
        std::shared_ptr<const RetargetSetup> setup = r ? std::make_shared<const RetargetSetup>(std::move(*r)) : nullptr;
        it = retargetSetups_.emplace(key, std::make_pair(setup, r ? std::string() : r.error().message)).first;
    }
    if (!it->second.first) return Error::make("not_humanoid", it->second.second, "animation_retarget {preview: true} shows the bone maps");
    return it->second.first;
}

std::shared_ptr<const Clip> AnimationSystem::resolveClip(const std::shared_ptr<const Library>& lib, const std::string& refIn,
                                                         const std::string& mode, const std::string& fallbackLibrary) {
    if (!lib) return nullptr;
    std::string ref = refIn;
    size_t hash = ref.rfind('#');
    if ((hash == std::string::npos || hash == 0) && !lib->clip(ref) && !fallbackLibrary.empty()) {
        ref = fallbackLibrary + "#" + ref;  // Animator::retargetFrom: a shared clip pack
        hash = ref.rfind('#');
    }
    if (hash != std::string::npos && hash > 0) {
        std::string file = ref.substr(0, hash), name = ref.substr(hash + 1);
        std::string key = std::to_string(reinterpret_cast<uintptr_t>(lib.get())) + "|" + mode + "|" + ref;
        if (auto it = retargeted_.find(key); it != retargeted_.end()) return it->second;
        auto other = library(file);
        std::shared_ptr<const Clip> out;
        if (other) {
            const Library& src = **other;
            if (const Clip* c = src.clip(name)) {
                std::string method = retargetMethod(src, *lib, mode);
                if (method == "same") {
                    out = std::shared_ptr<const Clip>(*other, c);  // same rig: no retargeting needed
                } else if (method == "pose") {
                    if (auto setup = retargetSetup(*other, lib)) {
                        out = std::make_shared<Clip>(retargetClipPose(**setup, src.skeleton, *c, lib->skeleton));
                    } else {
                        warnOnce("retarget:" + file, "pose-space retargeting " + file + ": " + setup.error().message + " (falling back to bone names)");
                    }
                }
                if (!out && method != "same") {
                    auto rc = std::make_shared<Clip>();
                    if (retarget(*c, src.skeleton, lib->skeleton, *rc) > 0) out = rc;
                }
            }
        }
        retargeted_[key] = out;
        return out;
    }
    const Clip* c = lib->clip(ref);
    return c ? std::shared_ptr<const Clip>(lib, c) : nullptr;
}

Result<std::shared_ptr<const Library>> AnimationSystem::libraryOf(EntityId e) {
    Instance* inst = instance(e);
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator component",
                                  "add one with animator_setup, or entity_update {\"components\": {\"animator\": {}}}");
    if (!inst->lib) return Error::make("invalid_animation", inst->error.empty() ? "the animator has no animation library" : inst->error,
                                       "set animator.library to an imported .anim file (animation_list lists them)");
    return inst->lib;
}

// ---------------------------------------------------------------------------
// Animator instances
// ---------------------------------------------------------------------------

EntityId AnimationSystem::animatorFor(EntityId e) const {
    for (const EntityRecord* r = scene_.record(e); r; r = r->parent ? scene_.record(r->parent) : nullptr) {
        if (scene_.get<Animator>(r->id)) return r->id;
    }
    return kNoEntity;
}

void AnimationSystem::bindMesh(Instance& inst, EntityId e) {
    inst.bindRevision = scene_.revision();
    auto skinnedMesh = [&](EntityId id) -> const MeshData* {
        const MeshRenderer* m = scene_.get<MeshRenderer>(id);
        if (!m || !str::startsWith(m->mesh, "asset:") || !hooks.mesh) return nullptr;
        const MeshData* md = hooks.mesh(m->mesh);
        return md && md->skinned() ? md : nullptr;
    };
    auto under = [&](EntityId id) {
        for (const EntityRecord* r = scene_.record(id); r; r = r->parent ? scene_.record(r->parent) : nullptr) {
            if (r->id == e) return true;
        }
        return false;
    };
    // Keep a still-valid binding (cheap); search the subtree otherwise.
    bool valid = inst.meshEntity && inst.bindGeneration == assetGeneration_ && scene_.exists(inst.meshEntity) &&
                 under(inst.meshEntity) && scene_.get<MeshRenderer>(inst.meshEntity) &&
                 scene_.get<MeshRenderer>(inst.meshEntity)->mesh == inst.meshKey;
    if (valid) {
        if (const MeshData* md = skinnedMesh(inst.meshEntity)) inst.transform = md->skin.transform;
        return;
    }
    inst.bindGeneration = assetGeneration_;
    inst.meshEntity = kNoEntity;
    inst.meshKey.clear();
    inst.libraryFromMesh.clear();
    inst.transform = Mat4{};
    std::vector<EntityId> queue{e};  // breadth first: the mesh closest to the animator wins
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        EntityId id = queue[qi];
        if (const MeshData* md = skinnedMesh(id)) {
            inst.meshEntity = id;
            inst.meshKey = scene_.get<MeshRenderer>(id)->mesh;
            inst.transform = md->skin.transform;
            std::string file = meshFileOf(inst.meshKey);
            inst.libraryFromMesh = hooks.libraryFor ? hooks.libraryFor(file) : "";
            if (inst.libraryFromMesh.empty()) inst.libraryFromMesh = file;
            return;
        }
        for (EntityId c : scene_.children(id)) queue.push_back(c);
    }
}

AnimationSystem::Instance* AnimationSystem::instance(EntityId e) {
    const Animator* a = scene_.get<Animator>(e);
    if (!a) {
        animators_.erase(e);
        return nullptr;
    }
    auto& slot = animators_[e];
    if (!slot) slot = std::make_unique<Instance>();
    Instance& inst = *slot;
    if (inst.bindRevision != scene_.revision() || inst.bindGeneration != assetGeneration_) bindMesh(inst, e);
    std::string signature = a->library + "|" + a->controller + "|" + a->clip + "|" + (a->loop ? "1" : "0") + "|" +
                            inst.libraryFromMesh + "|" + a->retargetFrom + "|" + a->retarget + "|" + std::to_string(assetGeneration_);
    if (signature != inst.signature) {
        inst.signature = signature;
        initRuntime(inst, *a);
    }
    const bool pin = a->rootMotion || a->inPlace;
    const int rootMode = (pin ? 2 : 0) + (pin && a->rootYaw ? 1 : 0);
    if (inst.lib && inst.rootMode != rootMode) {
        inst.rootMode = rootMode;
        Vec3 up = normalize(inst.transform.inverse().transformDir({0, 1, 0}));
        inst.rt.setRootMotion(pin, up, a->rootYaw);
    }
    return &inst;
}

void AnimationSystem::initRuntime(Instance& inst, const Animator& a) {
    inst.rt = AnimatorRuntime{};
    inst.lib.reset();
    inst.ctl.reset();
    inst.error.clear();
    inst.posed = false;
    inst.editKey.clear();
    inst.skins.clear();
    inst.lookChain.clear();
    inst.restGlobals.clear();
    inst.lookTarget.reset();
    inst.rootMode = -1;
    inst.humanoid.reset();
    inst.capsules.clear();
    inst.feet = FootIkState{};
    inst.ikStatus = Json();
    ++inst.version;

    std::shared_ptr<const ControllerDef> ctl;
    if (!a.controller.empty()) {
        auto c = controller(a.controller);
        if (c) {
            ctl = *c;
        } else {
            inst.error = c.error().message;
            warnOnce("ctl:" + a.controller, "animator controller " + a.controller + ": " + c.error().message);
        }
    }
    std::string libRef = !a.library.empty() ? a.library : (ctl && !ctl->library.empty() ? ctl->library : inst.libraryFromMesh);
    if (libRef.empty()) {
        if (inst.error.empty()) inst.error = "no animation library: the animator has no rigged mesh and no library set";
        return;
    }
    auto lib = library(libRef);
    if (!lib) {
        inst.error = lib.error().message;
        warnOnce("lib:" + libRef, "animation library " + libRef + ": " + lib.error().message);
        return;
    }
    inst.lib = *lib;
    std::shared_ptr<const Library> fallback;
    if (!a.retargetFrom.empty()) {
        if (auto f = library(a.retargetFrom)) {
            fallback = *f;
        } else {
            warnOnce("retargetFrom:" + a.retargetFrom, "animator retargetFrom " + a.retargetFrom + ": " + f.error().message);
        }
    }
    if (!ctl) {
        std::string clip = a.clip;
        const bool inFallback = fallback && fallback->clip(clip);
        if (!clip.empty() && !inst.lib->clip(clip) && clip.find('#') == std::string::npos && !inFallback) {
            warnOnce("clip:" + libRef + clip, "animator clip \"" + clip + "\" is not in " + libRef);
            clip.clear();
        }
        std::vector<std::string> names = inst.lib->clipNames();
        if (fallback) {  // a shared clip pack: its clips play too (retargeted)
            for (const auto& n : fallback->clipNames()) {
                if (!inst.lib->clip(n)) names.push_back(n);
            }
            if (clip.empty() && inst.lib->clips.empty() && !fallback->clips.empty()) clip = fallback->clips.front().name;
        }
        if (!clip.empty() && std::find(names.begin(), names.end(), clip) == names.end()) names.push_back(clip);
        ctl = std::make_shared<ControllerDef>(simpleController(names, clip, a.loop));
    }
    inst.ctl = ctl;
    std::shared_ptr<const Library> libPtr = inst.lib;
    const std::string mode = a.retarget, from = fallback ? a.retargetFrom : std::string();
    if (Status s = inst.rt.init(libPtr, ctl, [this, libPtr, mode, from](const std::string& ref) { return resolveClip(libPtr, ref, mode, from); });
        !s) {
        inst.error = s.error().message;
        inst.lib.reset();
        return;
    }
    inst.lookChain = lookChain(inst.lib->skeleton);
    computeGlobals(inst.lib->skeleton, restPose(inst.lib->skeleton), inst.restGlobals);
}

Mat4 AnimationSystem::modelToWorld(const Instance& inst, EntityId e) const {
    if (inst.meshEntity && scene_.exists(inst.meshEntity)) return scene_.worldMatrix(inst.meshEntity) * inst.transform;
    return scene_.worldMatrix(e);
}

void AnimationSystem::applyRootYaw(const Instance& inst, EntityId e, float yaw) {
    Transform* t = scene_.get<Transform>(e);
    if (!t) return;
    // Turn about the character's up axis as seen in the world (the mesh's +Y: world up when upright).
    EntityId upright = inst.meshEntity && scene_.exists(inst.meshEntity) ? inst.meshEntity : e;
    Vec3 upWorld = scene_.worldMatrix(upright).transformDir({0, 1, 0});
    upWorld = length(upWorld) > 1e-6f ? normalize(upWorld) : Vec3{0, 1, 0};
    const EntityRecord* rec = scene_.record(e);
    Mat4 parentInv = rec && rec->parent ? scene_.worldMatrix(rec->parent).inverse() : Mat4{};
    Vec3 axis = normalize(parentInv.transformDir(upWorld));
    Quat local = Quat::fromEulerDeg(t->rotation);
    Quat turned = (Quat::axisAngle(axis, yaw) * local).normalized();
    t->rotation = eulerDegFromQuat(turned);
    scene_.markDirty();
}

void AnimationSystem::applyLookAt(Instance& inst, EntityId e, const Animator& a) {
    // Where to look: another character's head, else the middle of a mesh, else a pivot.
    std::optional<Vec3> targetWorld;
    if (!a.lookAt.empty() && !inst.lookChain.empty()) {
        EntityId t = scene_.resolve(a.lookAt, e);
        if (t && t != e && scene_.isActive(t)) {
            auto other = animators_.find(t);
            if (other != animators_.end() && other->second->posed && !other->second->lookChain.empty() &&
                other->second->globals.size() > static_cast<size_t>(other->second->lookChain.back())) {
                targetWorld = (modelToWorld(*other->second, t) * other->second->globals[static_cast<size_t>(other->second->lookChain.back())])
                                  .translation();
            } else if (scene_.get<MeshRenderer>(t)) {
                targetWorld = scene_.localBounds(t).transformed(scene_.worldMatrix(t)).center();
            } else {
                targetWorld = scene_.worldMatrix(t).translation();
            }
        }
    }
    if (targetWorld) inst.lookTarget = targetWorld;
    const float blend = inst.lookBlend;
    if (blend <= 1e-4f || !inst.lookTarget || inst.lookChain.empty()) return;  // off, or faded out
    const Skeleton& sk = inst.lib->skeleton;
    computeGlobals(sk, inst.pose, inst.globals);
    Mat4 toModel = modelToWorld(inst, e).inverse();
    Vec3 target = toModel.transformPoint(*inst.lookTarget);  // the last target while fading out
    const std::vector<Mat4>& rest = inst.restGlobals;
    const int head = inst.lookChain.back();
    const Vec3 fwd{0, 0, 1};  // glTF characters face +Z in their own space
    auto facing = [&](int bone) {
        Quat now = rotationOf(inst.globals[static_cast<size_t>(bone)]);
        Quat restQ = rotationOf(rest[static_cast<size_t>(bone)]);
        return (now * restQ.conjugate()).rotate(fwd);
    };
    // Clamp the aim to a cone around the body's facing (the chain's parent).
    int base = sk.bones[static_cast<size_t>(inst.lookChain.front())].parent;
    Vec3 body = base >= 0 ? facing(base) : fwd;
    Vec3 desired = normalize(target - inst.globals[static_cast<size_t>(head)].translation());
    if (length(desired) < 1e-6f) return;
    float limit = radians(std::clamp(a.lookAtLimit, 0.f, 180.f));
    float angle = std::acos(std::clamp(dot(normalize(body), desired), -1.f, 1.f));
    if (angle > limit && angle > 1e-4f) desired = slerp(Quat{}, Quat::fromTo(body, desired), limit / angle).rotate(normalize(body));
    // Spread the turn: spine/neck take a share, the head the rest.
    const size_t n = inst.lookChain.size();
    for (size_t i = 0; i < n; ++i) {
        int b = inst.lookChain[i];
        float share = i + 1 == n ? 1.f : (str::lower(sk.bones[static_cast<size_t>(b)].name).find("neck") != std::string::npos ? 0.4f : 0.2f);
        Quat q = Quat::fromTo(facing(head), desired);
        q = slerp(Quat{}, q, share * blend);
        int p = sk.bones[static_cast<size_t>(b)].parent;
        Quat parentQ = p >= 0 ? rotationOf(inst.globals[static_cast<size_t>(p)]) : Quat{};
        Quat global = rotationOf(inst.globals[static_cast<size_t>(b)]);
        inst.pose[static_cast<size_t>(b)].r = (parentQ.conjugate() * q * global).normalized();
        computeGlobals(sk, inst.pose, inst.globals);
    }
}

const std::vector<EntityId>* AnimationSystem::ikEffectors(EntityId e) {
    if (!scene_.registry().count<IkTarget>()) return nullptr;
    if (ikRevision_ != scene_.revision()) {
        ikRevision_ = scene_.revision();
        ikIndex_.clear();
        for (EntityId id : scene_.entities()) {
            const IkTarget* ik = scene_.get<IkTarget>(id);
            if (!ik || ik->bone.empty()) continue;
            EntityId owner = kNoEntity;
            if (!ik->character.empty()) {
                owner = scene_.resolve(ik->character, id);
            } else if (const EntityRecord* rec = scene_.record(id); rec && rec->parent) {
                owner = animatorFor(rec->parent);
            }
            if (owner && owner != id) ikIndex_[owner].push_back(id);
        }
    }
    auto it = ikIndex_.find(e);
    return it == ikIndex_.end() ? nullptr : &it->second;
}

void AnimationSystem::applyIk(Instance& inst, EntityId e) {
    const std::vector<EntityId>* effectors = ikEffectors(e);
    if (!effectors) return;
    const Skeleton& sk = inst.lib->skeleton;
    computeGlobals(sk, inst.pose, inst.globals);
    const Mat4 toModel = modelToWorld(inst, e).inverse();
    const Mat4 characterWorld = scene_.worldMatrix(e);
    for (EntityId id : *effectors) {
        const IkTarget* ik = scene_.get<IkTarget>(id);
        if (!ik || !scene_.isActive(id) || ik->weight <= 0.f) continue;
        int bone = sk.find(ik->bone);
        if (bone < 0) {
            warnOnce("ik:" + std::to_string(id) + ik->bone, "ik on " + formatEntityRef(id) + ": no bone "" + ik->bone + """);
            continue;
        }
        Mat4 effector = scene_.worldMatrix(id);
        Vec3 target = toModel.transformPoint(effector.translation());
        Vec3 pole = length(ik->pole) > 1e-6f ? toModel.transformDir(characterWorld.transformDir(ik->pole)) : Vec3{0, 0, 0};
        float w = std::clamp(ik->weight, 0.f, 1.f);
        if (!solveTwoBoneIk(sk, inst.pose, inst.globals, bone, target, pole, w)) {
            warnOnce("ikchain:" + std::to_string(id), "ik on " + formatEntityRef(id) + ": "" + ik->bone + "" needs a parent and grandparent with length");
            continue;
        }
        if (ik->matchRotation) {
            // Same convention as bone attachments: the bone's world rotation becomes the entity's.
            Quat want = rotationOf(toModel) * rotationOf(effector);
            int parent = sk.bones[static_cast<size_t>(bone)].parent;
            Quat parentQ = parent >= 0 ? rotationOf(inst.globals[static_cast<size_t>(parent)]) : Quat{};
            Quat local = (parentQ.conjugate() * want).normalized();
            Trs& t = inst.pose[static_cast<size_t>(bone)];
            t.r = w >= 1.f ? local : slerp(t.r, local, w);
            computeGlobals(sk, inst.pose, inst.globals);
        }
    }
}

void AnimationSystem::finishPose(Instance& inst, EntityId e, const Animator& a) {
    inst.rt.evaluate(inst.pose);
    applyLookAt(inst, e, a);
    applyCharacterIk(inst, e);  // feet, pelvis and hand targets (characterIk component)
    applyIk(inst, e);
    computeGlobals(inst.lib->skeleton, inst.pose, inst.globals);
    inst.posed = true;
    ++inst.version;
}

void AnimationSystem::poseEditing(Instance& inst, EntityId e, const Animator& a) {
    std::string key = a.preview + "|" + std::to_string(a.time) + "|" + std::to_string(inst.paramsVersion) + "|" +
                      std::to_string(inst.editClock) + "|" + std::to_string(a.lookAt.id) + a.lookAt.name + "|" + std::to_string(a.lookAtWeight) + "|" +
                      std::to_string(a.lookAtLimit);
    if (inst.preview) key += "|p:" + inst.preview->first + "@" + std::to_string(inst.preview->second);
    if (inst.seqPreview) key += "|s:" + inst.seqPreview->first + "@" + std::to_string(inst.seqPreview->second);
    if (!a.lookAt.empty()) {
        // A moving look target changes the pose without changing the key.
        if (EntityId t = scene_.resolve(a.lookAt, e)) {
            Vec3 p = scene_.worldMatrix(t).translation();
            key += "|" + std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z);
        }
        Vec3 me = scene_.worldMatrix(e).translation();
        key += "|" + std::to_string(me.x) + "," + std::to_string(me.y) + "," + std::to_string(me.z);
    }
    if (const CharacterIk* cik = scene_.get<CharacterIk>(e)) {  // foot / hand IK settings and targets re-pose
        key += "|cik" + reflect::toJson(cik, CharacterIk::type()).dump();
        for (const EntityLink* l : {&cik->leftHand, &cik->rightHand}) {
            if (EntityId t = l->empty() ? kNoEntity : scene_.resolve(*l, e)) {
                for (float v : scene_.worldMatrix(t).m) key += "," + std::to_string(v);
            }
        }
        Vec3 me = scene_.worldMatrix(e).translation();  // the ground under the feet changes as it moves
        key += "|" + std::to_string(me.x) + "," + std::to_string(me.y) + "," + std::to_string(me.z);
    }
    if (const std::vector<EntityId>* effectors = ikEffectors(e)) {  // moving an IK target re-poses
        for (EntityId id : *effectors) {
            Mat4 m = scene_.worldMatrix(id);
            for (float f : m.m) key += "," + std::to_string(f);
            if (const IkTarget* ik = scene_.get<IkTarget>(id)) key += "|" + std::to_string(ik->weight) + ik->bone;
        }
    }
    if (inst.posed && key == inst.editKey) return;
    inst.editKey = key;
    auto seekSeconds = [&](const std::string& name, float seconds) {
        if (!inst.rt.seek(name, 0.f)) return;
        float dur = inst.rt.currentDuration(0);
        (void)inst.rt.seek(name, dur > 1e-4f ? seconds / dur : 0.f);
    };
    if (inst.preview) {
        seekSeconds(inst.preview->first, inst.preview->second);
    } else if (inst.seqPreview) {
        seekSeconds(inst.seqPreview->first, inst.seqPreview->second);
    } else if (a.preview == "rest") {
        inst.pose = restPose(inst.lib->skeleton);
        inst.lookBlend = 0.f;
        computeGlobals(inst.lib->skeleton, inst.pose, inst.globals);
        inst.posed = true;
        ++inst.version;
        return;
    } else if (a.preview != "play") {
        seekSeconds("", a.time);
    }
    // Previews snap (no smoothing) and never keep aiming at an old target.
    const bool looking = !a.lookAt.empty() && scene_.resolve(a.lookAt, e) != kNoEntity;
    inst.lookBlend = looking ? std::clamp(a.lookAtWeight, 0.f, 1.f) : 0.f;
    if (!looking) inst.lookTarget.reset();
    finishPose(inst, e, a);
}

void AnimationSystem::editorUpdate(float dt) {
    if (playing_) return;
    for (auto& [e, inst] : animators_) {
        const Animator* a = scene_.get<Animator>(e);
        if (!a || !inst->lib || a->preview != "play" || inst->preview) continue;
        inst->rt.update(dt * a->speed);
        inst->editClock += dt;
    }
}

// ---------------------------------------------------------------------------
// Simulation tick
// ---------------------------------------------------------------------------

void AnimationSystem::tick(float baseDt) {
    playing_ = true;
    ecs::Registry& reg = scene_.registry();
    // 1. Sequencers
    if (reg.count<SequencePlayer>()) {
        for (EntityId e : std::vector<EntityId>(scene_.entities())) {
            const SequencePlayer* sp = scene_.get<SequencePlayer>(e);
            if (!sp || !scene_.isActive(e)) continue;
            if (process && !process->runs(e)) continue;  // paused (process mode): the cutscene holds
            const float dt = baseDt * (process ? process->scale(e) : 1.f);
            SeqInstance* s = seqInstance(e);
            if (!s || !s->def) continue;
            if (!s->started) {
                s->started = true;
                if (sp->playOnStart) {
                    s->playing = true;
                    s->time = 0.f;
                    applySequence(e, *s, 0.f, -1e-6f, true, nullptr);
                }
                continue;
            }
            if (!s->playing) continue;
            float len = s->def->length();
            float prev = s->time;
            s->time += dt * sp->speed;
            if (s->time >= len) {
                applySequence(e, *s, len, prev, true, nullptr);
                if (sp->loop && len > 0.f) {
                    s->time = std::fmod(s->time, len);
                    applySequence(e, *s, s->time, -1e-6f, true, nullptr);
                } else {
                    s->time = len;
                    s->playing = false;  // hold the last frame
                }
            } else {
                applySequence(e, *s, s->time, prev, true, nullptr);
            }
        }
    }
    // 2. Animators
    if (reg.count<Animator>()) {
        for (EntityId e : std::vector<EntityId>(scene_.entities())) {
            const Animator* a = scene_.get<Animator>(e);
            if (!a || !scene_.isActive(e)) continue;
            Instance* inst = instance(e);
            if (!inst || !inst->lib) continue;
            if (process && !process->runs(e)) {  // paused (process mode): the pose holds, nothing to blend
                inst->prevGlobals = inst->globals;
                continue;
            }
            const float dt = baseDt * (process ? process->scale(e) : 1.f);
            if (inst->posed) inst->prevGlobals = inst->globals;
            updateTurn(*inst, e, dt);  // turn_in_place: rotate toward the target, feed `turn` / `turning`
            std::vector<AnimatorRuntime::Event> events;
            Vec3 delta{0, 0, 0};
            float yaw = 0.f;
            inst->rt.update(dt * a->speed, &events, a->rootMotion ? &delta : nullptr, a->rootMotion && a->rootYaw ? &yaw : nullptr);
            for (const auto& ev : events) {
                // Foot contact markers (characterIk.contact = events).
                if (ev.name == "foot_l_down" || ev.name == "foot_l_up") inst->feet.feet[0].eventContact = ev.name == "foot_l_down";
                if (ev.name == "foot_r_down" || ev.name == "foot_r_up") inst->feet.feet[1].eventContact = ev.name == "foot_r_down";
                if (hooks.emit) hooks.emit("anim:" + ev.name, e);
                inst->recentEvents.push_back(ev.name);
                if (inst->recentEvents.size() > kRecentEvents) inst->recentEvents.erase(inst->recentEvents.begin());
            }
            inst->lastRootMotion = {0, 0, 0};
            inst->lastRootYaw = yaw;
            if (a->rootMotion && length(delta) > 0.f) {
                Vec3 world = modelToWorld(*inst, e).transformDir(delta);
                inst->lastRootMotion = world;
                if (!(hooks.rootMotion && hooks.rootMotion(e, world))) {
                    if (Transform* t = scene_.get<Transform>(e)) {
                        const EntityRecord* rec = scene_.record(e);
                        Vec3 local = rec && rec->parent ? scene_.worldMatrix(rec->parent).inverse().transformDir(world) : world;
                        t->position += local;
                        scene_.markDirty();
                    }
                }
            }
            if (std::fabs(yaw) > 1e-7f) applyRootYaw(*inst, e, yaw);
            const bool looking = !a->lookAt.empty() && scene_.resolve(a->lookAt, e) != kNoEntity;
            float goal = looking ? std::clamp(a->lookAtWeight, 0.f, 1.f) : 0.f;  // turns fade in and out
            inst->lookBlend += (goal - inst->lookBlend) * std::min(1.f, dt * kLookRate);
            inst->tickDt = dt;  // foot IK smoothing and contact locking advance with the simulation
            finishPose(*inst, e, *a);
            inst->tickDt = 0.f;
        }
    }
    // 3. Bone attachments
    updateAttachments(nullptr);
}

// ---------------------------------------------------------------------------
// Edit-mode frame overrides
// ---------------------------------------------------------------------------

bool AnimationSystem::writeComponent(EntityId e, const std::string& component, const Json& patch, FrameOverrides* ov) {
    const ComponentKind* kind = scene_.componentKind(component);
    if (!kind || !scene_.exists(e)) return false;
    if (ov) {
        bool recorded = false;
        for (const auto& [id, rec] : ov->components) recorded = recorded || (id == e && rec.first == component);
        if (!recorded) ov->components.push_back({e, {component, kind->has(scene_, e) ? kind->toJson(scene_, e) : Json()}});
    }
    if (!kind->apply(scene_, e, patch)) return false;
    if (!ov) scene_.markDirty();
    return true;
}

AnimationSystem::FrameOverrides AnimationSystem::beginFrame(bool editing) {
    FrameOverrides ov;
    playing_ = !editing;
    // Forget runtime state of entities that were deleted (or lost their component).
    std::erase_if(animators_, [&](const auto& kv) { return !scene_.get<Animator>(kv.first); });
    std::erase_if(sequencers_, [&](const auto& kv) { return !scene_.get<SequencePlayer>(kv.first); });
    std::erase_if(scrubs_, [&](const auto& kv) { return !scene_.exists(kv.first); });
    if (!editing) return ov;
    ov.active = true;
    ecs::Registry& reg = scene_.registry();
    if (reg.count<SequencePlayer>()) {
        for (EntityId e : scene_.entities()) {
            const SequencePlayer* sp = scene_.get<SequencePlayer>(e);
            if (!sp) continue;
            float t = -1.f;
            if (auto it = scrubs_.find(e); it != scrubs_.end()) t = it->second;
            else if (sp->preview) t = sp->time;
            if (t < 0.f) continue;
            SeqInstance* s = seqInstance(e);
            if (s && s->def) applySequence(e, *s, t, t, false, &ov);
        }
    }
    updateAttachments(&ov);
    return ov;
}

void AnimationSystem::endFrame(FrameOverrides& ov) {
    if (!ov.active) return;
    for (auto it = ov.components.rbegin(); it != ov.components.rend(); ++it) {
        const ComponentKind* kind = scene_.componentKind(it->second.first);
        if (!kind || !scene_.exists(it->first)) continue;
        if (it->second.second.isNull()) kind->remove(scene_, it->first);
        else (void)kind->apply(scene_, it->first, it->second.second);
    }
    if (ov.environment) (void)reflect::applyJson(&scene_.environment(), Environment::type(), ov.oldEnvironment);
    for (auto& [e, inst] : animators_) inst->seqPreview.reset();
    ov = FrameOverrides{};
}

// ---------------------------------------------------------------------------
// Rendering & queries
// ---------------------------------------------------------------------------

const SkinPose* AnimationSystem::skin(EntityId drawEntity, const std::string& meshKey) {
    const MeshData* mesh = hooks.mesh ? hooks.mesh(meshKey) : nullptr;
    if (!mesh || !mesh->skinned()) return nullptr;
    EntityId ae = animatorFor(drawEntity);
    if (!ae) return nullptr;
    Instance* inst = instance(ae);
    if (!inst || !inst->lib) return nullptr;
    const Animator* a = scene_.get<Animator>(ae);
    if (!playing_) {
        if (a->preview == "rest" && !inst->preview && !inst->seqPreview) return nullptr;  // the static mesh is the rest pose
        poseEditing(*inst, ae, *a);
    } else if (!inst->posed) {
        finishPose(*inst, ae, *a);  // first frame of play, before any tick
    }
    auto& se = inst->skins[meshKey];
    if (se.mesh != mesh || se.map.size() != mesh->skin.slots()) {
        se.mesh = mesh;
        se.map = mapSkin(mesh->skin, inst->lib->skeleton);
        se.version = ~0ull;
        se.posedVersion = ~0ull;
    }
    if (se.version != inst->version) {
        auto palette = std::make_shared<std::vector<Mat4>>();
        skinPalette(mesh->skin, se.map, inst->globals, *palette);
        se.pose.bounds = posedBounds(mesh->skin, *palette);
        se.pose.palette = std::move(palette);
        se.version = inst->version;
    }
    // Render interpolation: real-time frames show the pose between the last two ticks.
    const bool blend = playing_ && displayAlpha_ < 1.f && !inst->prevGlobals.empty() &&
                       inst->prevGlobals.size() == inst->globals.size() && (!process || process->interpolates(ae));
    if (!blend) return &se.pose;
    if (se.blendedVersion != inst->version || se.blendedAlpha != displayAlpha_) {
        const float t = std::clamp(displayAlpha_, 0.f, 1.f);
        std::vector<Mat4> globals(inst->globals.size());
        for (size_t i = 0; i < globals.size(); ++i) {
            globals[i] = lerp(Trs::fromMatrix(inst->prevGlobals[i]), Trs::fromMatrix(inst->globals[i]), t).matrix();
        }
        auto palette = std::make_shared<std::vector<Mat4>>();
        skinPalette(mesh->skin, se.map, globals, *palette);
        se.blended.bounds = posedBounds(mesh->skin, *palette);
        se.blended.palette = std::move(palette);
        se.blendedVersion = inst->version;
        se.blendedAlpha = displayAlpha_;
    }
    return &se.blended;
}

std::shared_ptr<const MeshData> AnimationSystem::posedMesh(EntityId drawEntity, const std::string& meshKey) {
    const SkinPose* sp = skin(drawEntity, meshKey);
    if (!sp) return nullptr;
    Instance* inst = instance(animatorFor(drawEntity));
    auto& se = inst->skins[meshKey];
    if (!se.posed || se.posedVersion != se.version) {
        auto posed = std::make_shared<MeshData>();
        skinMesh(*se.mesh, *sp->palette, *posed);
        se.posed = std::move(posed);
        se.posedVersion = se.version;
    }
    return se.posed;
}

Result<Mat4> AnimationSystem::boneWorld(EntityId e, std::string_view bone) {
    Instance* inst = instance(e);
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    if (!inst->lib) return Error::make("invalid_animation", inst->error.empty() ? "the animator has no library" : inst->error);
    const Animator* a = scene_.get<Animator>(e);
    if (!playing_) poseEditing(*inst, e, *a);
    else if (!inst->posed) finishPose(*inst, e, *a);
    int b = inst->lib->skeleton.find(bone);
    if (b < 0) {
        return Error::make("unknown_bone", "no bone \"" + std::string(bone) + "\" in the skeleton",
                           didYouMean(bone, inst->lib->skeleton.names()));
    }
    return modelToWorld(*inst, e) * inst->globals[static_cast<size_t>(b)];
}

Mat4 AnimationSystem::modelTransform(EntityId e) {
    Instance* inst = instance(e);
    return inst ? inst->transform : Mat4{};
}

void AnimationSystem::updateAttachments(FrameOverrides* ov) {
    if (!scene_.registry().count<BoneAttachment>()) return;
    for (EntityId e : std::vector<EntityId>(scene_.entities())) {
        const BoneAttachment* at = scene_.get<BoneAttachment>(e);
        if (!at || at->bone.empty() || !scene_.isActive(e)) continue;
        EntityId target = kNoEntity;
        if (!at->character.empty()) {
            target = scene_.resolve(at->character, e);
        } else if (const EntityRecord* rec = scene_.record(e); rec && rec->parent) {
            target = animatorFor(rec->parent);
        }
        if (!target || target == e) {
            warnOnce("attach:" + std::to_string(e), "bone attachment on " + formatEntityRef(e) + ": no animator to follow");
            continue;
        }
        // Never follow a bone of our own descendant (would feed back into itself).
        bool cycle = false;
        for (const EntityRecord* r = scene_.record(target); r && !cycle; r = r->parent ? scene_.record(r->parent) : nullptr) {
            cycle = r->id == e;
        }
        if (cycle) continue;
        auto bw = boneWorld(target, at->bone);
        if (!bw) {
            warnOnce("attach:" + std::to_string(e) + at->bone, "bone attachment on " + formatEntityRef(e) + ": " + bw.error().message);
            continue;
        }
        Mat4 boneM = at->followScale ? *bw : withoutScale(*bw);
        Mat4 desired = boneM * Mat4::trs(at->offset, at->rotation, {1, 1, 1});
        const EntityRecord* rec = scene_.record(e);
        Mat4 local = rec->parent ? scene_.worldMatrix(rec->parent).inverse() * desired : desired;
        Trs d = Trs::fromMatrix(local);
        Json patch = Json::object({{"position", reflect::vec3ToJson(d.t)}, {"rotation", reflect::vec3ToJson(eulerDegFromMatrix(local))}});
        if (at->followScale) patch["scale"] = reflect::vec3ToJson(d.s);
        writeComponent(e, "transform", patch, ov);
    }
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

Status AnimationSystem::setParam(EntityId e, std::string_view name, const Json& value) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    if (!inst->lib) return Error::make("invalid_animation", inst->error);
    float v;
    if (value.isBool()) v = value.asBool() ? 1.f : 0.f;
    else if (value.isNumber()) v = value.asFloat();
    else return Error::make("invalid_value", "parameter values are numbers or booleans");
    if (!inst->rt.setParam(name, v)) {
        std::vector<std::string> names;
        for (const auto& p : inst->ctl->params) names.push_back(p.name);
        return Error::make("unknown_parameter", "the animator's controller has no parameter \"" + std::string(name) + "\"",
                           names.empty() ? "this animator has no controller; create one with animator_setup" : didYouMean(name, names));
    }
    ++inst->paramsVersion;
    return {};
}

Status AnimationSystem::trigger(EntityId e, std::string_view name) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (inst && inst->ctl) {
        int pi = inst->ctl->paramIndex(name);
        if (pi >= 0 && inst->ctl->params[static_cast<size_t>(pi)].type != ParamType::Trigger) {
            return Error::make("invalid_value", "\"" + std::string(name) + "\" is not a trigger parameter", "use set_param for it");
        }
    }
    return setParam(e, name, Json(true));
}

Status AnimationSystem::play(EntityId e, const std::string& name, float fade, std::optional<bool> loop, int layer) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    if (!inst->lib) return Error::make("invalid_animation", inst->error);
    Status s = inst->rt.play(name, fade, layer, loop);
    if (s) ++inst->paramsVersion;
    return s;
}

std::string AnimationSystem::stateName(EntityId e) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    return inst && inst->lib ? inst->rt.stateName() : "";
}

Result<Json> AnimationSystem::describe(EntityId e) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    Json j = Json::object({{"animator", ae}});
    if (!inst->lib) {
        j["error"] = inst->error;
        return j;
    }
    j["state"] = inst->rt.stateJson();
    Json clips = Json::array();
    for (const auto& c : inst->lib->clips) clips.push(c.name);
    j["clips"] = clips;
    if (inst->meshEntity) j["mesh"] = inst->meshKey;
    if (!inst->recentEvents.empty()) {
        Json ev = Json::array();
        for (const auto& n : inst->recentEvents) ev.push(n);
        j["recentEvents"] = ev;
    }
    if (length(inst->lastRootMotion) > 0.f) j["rootMotion"] = reflect::vec3ToJson(inst->lastRootMotion);
    if (std::fabs(inst->lastRootYaw) > 1e-7f) j["rootYawDeg"] = std::round(degrees(inst->lastRootYaw) * 1000.f) / 1000.f;
    if (!inst->error.empty()) j["warning"] = inst->error;
    return j;
}

Status AnimationSystem::setPreview(EntityId e, const std::string& name, float seconds) {
    Instance* inst = instance(e);
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    if (!inst->lib) return Error::make("invalid_animation", inst->error);
    if (!name.empty()) {
        if (Status s = inst->rt.seek(name, 0.f); !s) return s;
    }
    inst->preview = std::make_pair(name, std::max(0.f, seconds));
    return {};
}

void AnimationSystem::clearPreview(EntityId e) {
    if (auto it = animators_.find(e); it != animators_.end()) {
        it->second->preview.reset();
        it->second->editKey.clear();
    }
}

// ---------------------------------------------------------------------------
// Sequencer
// ---------------------------------------------------------------------------

AnimationSystem::SeqInstance* AnimationSystem::seqInstance(EntityId e) {
    const SequencePlayer* sp = scene_.get<SequencePlayer>(e);
    if (!sp) {
        sequencers_.erase(e);
        return nullptr;
    }
    auto& slot = sequencers_[e];
    if (!slot) slot = std::make_unique<SeqInstance>();
    SeqInstance& s = *slot;
    std::string signature = sp->sequence + "|" + std::to_string(assetGeneration_);
    if (signature != s.signature) {
        s.signature = signature;
        s.def.reset();
        s.error.clear();
        if (sp->sequence.empty()) {
            s.error = "no sequence asset set";
        } else if (auto d = sequence(sp->sequence)) {
            s.def = *d;
        } else {
            s.error = d.error().message;
            warnOnce("seq:" + sp->sequence, "sequence " + sp->sequence + ": " + s.error);
        }
    }
    return &s;
}

void AnimationSystem::applySequence(EntityId owner, SeqInstance& s, float t, float prevT, bool playing, FrameOverrides* ov,
                                    bool valuesOnly) {
    const SequenceDef& def = *s.def;
    auto entity = [&](const std::string& ref, const char* what) -> EntityId {
        EntityId id = ref.empty() ? kNoEntity : scene_.find(ref);
        if (!id) warnOnce("seqent:" + std::to_string(owner) + ref, "sequence " + def.name + ": no " + what + " entity \"" + ref + "\"");
        return id;
    };
    PointLookup lookup = [&](const std::string& ref) -> std::optional<Vec3> {
        EntityId id = scene_.find(ref);
        if (!id) return std::nullopt;
        if (scene_.get<MeshRenderer>(id)) return scene_.localBounds(id).transformed(scene_.worldMatrix(id)).center();
        return scene_.worldMatrix(id).translation();
    };
    for (const Track& track : def.tracks) {
        if (track.muted || track.keys.empty()) continue;
        if (valuesOnly && (track.type == TrackType::Event || track.type == TrackType::Animation)) continue;
        switch (track.type) {
            case TrackType::Property: {
                size_t dot = track.property.find('.');
                std::string comp = track.property.substr(0, dot), field = track.property.substr(dot + 1);
                Json value = evaluateProperty(track, t);
                if (value.isNull()) break;
                if (comp == "environment") {
                    if (ov && !ov->environment) {
                        ov->environment = true;
                        ov->oldEnvironment = reflect::toJson(&scene_.environment(), Environment::type());
                    }
                    if (!reflect::applyJson(&scene_.environment(), Environment::type(), Json::object({{field, value}}))) {
                        warnOnce("seqprop:" + track.property, "sequence " + def.name + ": cannot set " + track.property);
                    }
                    if (!ov) scene_.markDirty();
                    break;
                }
                EntityId id = entity(track.entity, "property");
                if (!id) break;
                const ComponentKind* kind = scene_.componentKind(comp);
                if (!kind || !kind->info || !kind->info->field(field)) {
                    warnOnce("seqprop:" + track.property, "sequence " + def.name + ": unknown property " + track.property);
                    break;
                }
                if (!writeComponent(id, comp, Json::object({{field, value}}), ov)) {
                    warnOnce("seqval:" + track.property, "sequence " + def.name + ": invalid value for " + track.property);
                }
                break;
            }
            case TrackType::Camera: {
                const SeqKey* key = &track.keys.front();
                for (const auto& k : track.keys) {
                    if (k.t <= t) key = &k;
                }
                EntityId cam = entity(key->data.get("camera").asString(), "camera");
                if (!cam || !scene_.get<Camera>(cam)) break;
                for (EntityId id : scene_.entities()) {
                    const Camera* c = scene_.get<Camera>(id);
                    if (c && c->primary != (id == cam)) writeComponent(id, "camera", Json::object({{"primary", id == cam}}), ov);
                }
                break;
            }
            case TrackType::Shot: {
                EntityId cam = entity(track.camera, "camera");
                if (!cam) break;
                auto pose = evaluateShot(track, t, lookup);
                if (!pose) break;
                Mat4 world = Mat4::trs(pose->position, pose->rotation, {1, 1, 1});
                const EntityRecord* rec = scene_.record(cam);
                Mat4 local = rec->parent ? scene_.worldMatrix(rec->parent).inverse() * world : world;
                writeComponent(cam, "transform",
                               Json::object({{"position", reflect::vec3ToJson(local.translation())},
                                             {"rotation", reflect::vec3ToJson(eulerDegFromMatrix(local))}}),
                               ov);
                if (pose->fov && scene_.get<Camera>(cam)) writeComponent(cam, "camera", Json::object({{"fov", *pose->fov}}), ov);
                break;
            }
            case TrackType::Event: {
                if (!playing) break;
                for (const auto& k : track.keys) {
                    if (!(k.t > prevT && k.t <= t)) continue;
                    EntityId target = kNoEntity;
                    if (!k.data.get("target").asString().empty()) {
                        target = entity(k.data.get("target").asString(), "event target");
                        if (!target) continue;
                    }
                    if (hooks.emit) hooks.emit(k.data.get("event").asString(), target);
                }
                break;
            }
            case TrackType::Animation: {
                EntityId id = entity(track.entity, "animated");
                EntityId ae = id ? animatorFor(id) : kNoEntity;
                if (!ae) break;
                if (playing) {
                    for (const auto& k : track.keys) {
                        if (!(k.t > prevT && k.t <= t)) continue;
                        for (const auto& [pname, pv] : k.data.get("params").members()) (void)setParam(ae, pname, pv);
                        std::string what = k.data.get("play").asString();
                        if (what.empty()) continue;
                        std::optional<bool> loop;
                        if (k.data.get("loop").isBool()) loop = k.data.get("loop").asBool();
                        if (Status st = play(ae, what, k.data.get("fade").asFloat(0.2f), loop, static_cast<int>(k.data.get("layer").asInt(0)));
                            !st) {
                            warnOnce("seqanim:" + what, "sequence " + def.name + ": " + st.error().message);
                        }
                    }
                } else {
                    const SeqKey* key = nullptr;
                    for (const auto& k : track.keys) {
                        if (k.t <= t && !k.data.get("play").asString().empty()) key = &k;
                    }
                    if (!key) break;
                    if (Instance* inst = instance(ae)) {
                        inst->seqPreview = std::make_pair(key->data.get("play").asString(), t - key->t);
                    }
                }
                break;
            }
        }
    }
}

Status AnimationSystem::playSequence(EntityId e, float from) {
    SeqInstance* s = seqInstance(e);
    if (!s) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no sequencer component");
    if (!s->def) return Error::make("invalid_sequence", s->error);
    s->started = true;
    s->playing = true;
    s->time = std::clamp(from, 0.f, s->def->length());
    applySequence(e, *s, s->time, s->time - 1e-6f, playing_, nullptr);
    return {};
}

Status AnimationSystem::stopSequence(EntityId e) {
    SeqInstance* s = seqInstance(e);
    if (!s) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no sequencer component");
    s->started = true;
    s->playing = false;
    return {};
}

Json AnimationSystem::sequenceState(EntityId e) {
    SeqInstance* s = seqInstance(e);
    if (!s) return {};
    Json j = Json::object({{"playing", s->playing}, {"time", std::round(s->time * 1000.f) / 1000.f}});
    if (s->def) {
        j["name"] = s->def->name;
        j["length"] = s->def->length();
    }
    if (!s->error.empty()) j["error"] = s->error;
    return j;
}

void AnimationSystem::setSequenceScrub(EntityId e, float seconds) { scrubs_[e] = std::max(0.f, seconds); }
void AnimationSystem::clearSequenceScrub(EntityId e) { scrubs_.erase(e); }

AnimationSystem::FrameOverrides AnimationSystem::overrideSequenceAt(EntityId e, float seconds) {
    FrameOverrides ov;
    ov.active = true;
    SeqInstance* s = seqInstance(e);
    if (s && s->def) applySequence(e, *s, seconds, seconds, false, &ov, /*valuesOnly=*/true);
    return ov;
}

EntityId AnimationSystem::sequenceCamera(EntityId e, float seconds) {
    SeqInstance* s = seqInstance(e);
    if (!s || !s->def) return kNoEntity;
    EntityId shotCam = kNoEntity;
    for (const Track& track : s->def->tracks) {
        if (track.muted || track.keys.empty()) continue;
        if (track.type == TrackType::Camera) {
            const SeqKey* key = &track.keys.front();
            for (const auto& k : track.keys) {
                if (k.t <= seconds) key = &k;
            }
            if (EntityId cam = scene_.find(key->data.get("camera").asString())) return cam;
        } else if (track.type == TrackType::Shot && !shotCam) {
            shotCam = scene_.find(track.camera);
        }
    }
    return shotCam;
}

}  // namespace sky::anim
