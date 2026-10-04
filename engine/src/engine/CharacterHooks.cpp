#include "CharacterHooks.h"

#include <algorithm>
#include <cmath>

#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/fx/Groom.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/render/DebugViews.h"
#include "skywalker/world/WorldRuntime.h"

namespace sky {

namespace {

bool under(const Scene& scene, EntityId e, EntityId ancestor) {
    for (const EntityRecord* r = scene.record(e); r; r = r->parent ? scene.record(r->parent) : nullptr) {
        if (r->id == ancestor) return true;
    }
    return false;
}

void subtree(const Scene& scene, EntityId root, std::vector<EntityId>& out) {
    out.push_back(root);
    for (EntityId c : scene.children(root)) subtree(scene, c, out);
}

/// The nearest walkable surface along a ray: terrain, physics colliders (while playing) and static
/// meshes. Animated (skinned) meshes and the character's own subtree (body, clothes, held props)
/// are ignored, so feet never stand on the character itself or on other characters.
anim::GroundProbe groundProbe(Engine& engine, Vec3 origin, Vec3 dir, float maxDist, EntityId character) {
    anim::GroundProbe best;
    float bestDist = maxDist;
    Scene& scene = engine.scene();
    const Ray ray{origin, normalize(dir)};
    auto consider = [&](float dist, Vec3 point, Vec3 normal) {
        if (dist < 0.f || dist > bestDist) return;
        bestDist = dist;
        best.hit = true;
        best.point = point;
        best.normal = dot(normal, ray.dir) > 0.f ? -normal : normal;
    };
    if (auto th = engine.world().raycast(scene, ray, maxDist); th && !under(scene, th->entity, character)) {
        consider(th->distance, th->point, th->normal);
    }
    if (physics::PhysicsWorld* w = engine.physics().playWorld()) {
        physics::QueryFilter filter;
        subtree(scene, character, filter.exclude);
        if (auto h = w->raycast(ray.origin, ray.dir, bestDist, filter)) consider(h->distance, h->point, h->normal);
    }
    for (EntityId e : scene.entities()) {
        const MeshRenderer* m = scene.get<MeshRenderer>(e);
        if (!m || !m->visible || !scene.isActive(e) || under(scene, e, character)) continue;
        const MeshData* mesh = engine.cpuMesh(m->mesh);
        if (!mesh || mesh->skinned() || mesh->indices.empty()) continue;
        const Mat4 world = scene.worldMatrix(e);
        float enter = intersect(ray, scene.localBounds(e).transformed(world));
        if (enter < 0.f || enter > bestDist) continue;
        const Mat4 inv = world.inverse();
        const Vec3 o = inv.transformPoint(ray.origin), d = inv.transformDir(ray.dir);
        const auto& v = mesh->vertices;
        auto P = [&](uint32_t i) {
            const float* p = &v[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
            return Vec3{p[0], p[1], p[2]};
        };
        for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
            Vec3 a = P(mesh->indices[t]), b = P(mesh->indices[t + 1]), c = P(mesh->indices[t + 2]);
            Vec3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
            float det = dot(e1, pv);
            if (std::fabs(det) < 1e-12f) continue;
            float invDet = 1.f / det;
            Vec3 tv = o - a;
            float u = dot(tv, pv) * invDet;
            if (u < 0.f || u > 1.f) continue;
            Vec3 qv = cross(tv, e1);
            float w = dot(d, qv) * invDet;
            if (w < 0.f || u + w > 1.f) continue;
            float tl = dot(e2, qv) * invDet;
            if (tl <= 0.f) continue;
            Vec3 hit = world.transformPoint(o + d * tl);
            float dist = distance(ray.origin, hit);
            if (dist >= bestDist) continue;
            consider(dist, hit, normalize(inv.transposed().transformDir(cross(e1, e2))));
        }
    }
    return best;
}

/// Overlay boxes: segments and markers about `pixels` wide on screen.
struct OverlayPen {
    FrameData& frame;
    float pixelAt1m = 0.001f;
    void segment(Vec3 a, Vec3 b, Vec4 color, float pixels = 2.f) {
        Vec3 d = b - a;
        float len = length(d);
        if (len < 1e-5f) return;
        Vec3 mid = (a + b) * 0.5f;
        float t = std::max(pixelAt1m * std::max(distance(frame.camera.eye, mid), 0.05f) * pixels, 0.0008f);
        anim::Quat q = anim::Quat::fromTo({0, 1, 0}, d / len);
        frame.overlays.push_back({"cube", Mat4::translate(mid) * q.matrix() * Mat4::scale({t, len, t}), color});
    }
    void marker(Vec3 p, Vec4 color, float pixels = 7.f) {
        float s = std::max(pixelAt1m * std::max(distance(frame.camera.eye, p), 0.05f) * pixels, 0.002f);
        frame.overlays.push_back({"cube", Mat4::translate(p) * Mat4::scale({s, s, s}), color});
    }
};

Vec3 jsonVec3(const Json& j) {
    Vec3 v{0, 0, 0};
    reflect::jsonToVec3(j, v);
    return v;
}

}  // namespace

void addCharacterDebugOverlays(Engine& engine, FrameData& frame) {
    Scene& scene = engine.scene();
    anim::AnimationSystem& as = engine.animation();
    OverlayPen pen{frame};
    const float H = static_cast<float>(std::max(frame.height, 1));
    pen.pixelAt1m = frame.camera.orthographic ? frame.camera.orthoSize * 2.f / H : 2.f * std::tan(radians(frame.camera.fovDeg) * 0.5f) / H;
    const int view = frame.debugView;
    constexpr size_t kMaxOverlays = 12000;  // bounded draw work however many characters and roots
    if (view == debugview::kSkeleton) {
        for (EntityId e : scene.entities()) {
            if (!scene.get<Animator>(e) || !scene.isActive(e)) continue;
            for (const auto& [a, b] : as.skeletonLines(e)) {
                if (frame.overlays.size() >= kMaxOverlays) return;
                pen.segment(a, b, {0.2f, 0.9f, 1.f, 1.f});
            }
            auto status = as.characterStatus(e);
            if (!status) continue;
            const Json& bones = status->get("humanoid").get("bones");
            const std::pair<const char*, Vec4> keys[] = {{"hips", {1.f, 0.2f, 0.2f, 1.f}},     {"head", {1.f, 0.9f, 0.2f, 1.f}},
                                                         {"leftHand", {0.2f, 1.f, 0.3f, 1.f}}, {"rightHand", {0.2f, 1.f, 0.3f, 1.f}},
                                                         {"leftFoot", {1.f, 0.55f, 0.1f, 1.f}}, {"rightFoot", {1.f, 0.55f, 0.1f, 1.f}}};
            for (const auto& [slot, color] : keys) {
                std::string bone = bones.get(slot).asString();
                if (bone.empty()) continue;
                if (auto w = as.boneWorld(e, bone)) pen.marker(w->translation(), color, 9.f);
            }
        }
    } else if (view == debugview::kIkTargets) {
        for (EntityId e : scene.entities()) {
            if (!scene.isActive(e)) continue;
            if (const IkTarget* ik = scene.get<IkTarget>(e); ik && !ik->bone.empty()) pen.marker(scene.worldMatrix(e).translation(), {1, 1, 1, 1}, 9.f);
            if (!scene.get<CharacterIk>(e) || !scene.get<Animator>(e)) continue;
            auto status = as.characterStatus(e);
            if (!status) continue;
            const Json& ik = status->get("ik");
            for (const auto& foot : ik.get("feet").elements()) {
                Vec3 target = jsonVec3(foot.get("target")), animated = jsonVec3(foot.get("animated"));
                const bool locked = foot.get("locked").asBool(false);
                if (!foot.get("ground").isNull()) pen.marker(jsonVec3(foot.get("ground")), {1.f, 0.9f, 0.1f, 1.f}, 8.f);
                pen.marker(animated, {0.55f, 0.55f, 0.6f, 1.f}, 6.f);
                pen.segment(animated, target, {0.9f, 0.3f, 0.9f, 1.f}, 1.5f);
                pen.marker(target, locked ? Vec4{0.2f, 1.f, 0.3f, 1.f} : Vec4{1.f, 0.25f, 0.95f, 1.f}, 10.f);
            }
            for (const auto& hand : ik.get("hands").elements()) {
                if (!hand.contains("target")) continue;
                Vec3 target = jsonVec3(hand.get("target"));
                pen.marker(target, {0.2f, 0.95f, 1.f, 1.f}, 10.f);
                std::string bone = status->get("humanoid").get("bones").get(hand.get("side").asString() == "left" ? "leftHand" : "rightHand").asString();
                if (hand.get("error").asFloat(0.f) > 0.01f && !bone.empty()) {
                    if (auto w = as.boneWorld(e, bone)) pen.segment(w->translation(), target, {1.f, 0.2f, 0.2f, 1.f}, 2.f);
                }
            }
        }
    } else if (view == debugview::kGroomRoots) {
        auto meshes = [&engine](const std::string& k) { return engine.cpuMesh(k); };
        auto paths = [&engine](const std::string& p) { return engine.resolvePath(p); };
        for (EntityId e : scene.entities()) {
            const Groom* g = scene.get<Groom>(e);
            if (!g || !g->visible || !scene.isActive(e)) continue;
            auto roots = engine.grooms().roots(scene, e, meshes, paths);
            if (!roots) continue;
            const size_t n = roots->roots.size();
            const size_t stride = std::max<size_t>(1, n / 1500);
            const Vec4 color = roots->skinned ? Vec4{0.25f, 1.f, 0.35f, 1.f} : Vec4{0.3f, 0.55f, 1.f, 1.f};
            const bool farRoots = roots->bindError > 0.01f;
            for (size_t i = 0; i < n; i += stride) {
                if (frame.overlays.size() + 2 >= kMaxOverlays) return;
                pen.marker(roots->roots[i], farRoots ? Vec4{1.f, 0.2f, 0.2f, 1.f} : color, 4.f);
                pen.segment(roots->roots[i], roots->roots[i] + roots->normals[i] * 0.012f, color, 1.f);
            }
        }
    }
}

void installCharacterHooks(Engine& engine) {
    engine.animation().hooks.ground = [&engine](Vec3 origin, Vec3 dir, float maxDist, EntityId character) {
        return groundProbe(engine, origin, dir, maxDist, character);
    };
    // Skinned grooms: palettes, CPU-skinned fallbacks, body capsules and bone families.
    fx::GroomSystem::Hooks& g = engine.grooms().hooks;
    g.palette = [&engine](EntityId e, const std::string& key) -> std::shared_ptr<const std::vector<Mat4>> {
        const SkinPose* sp = engine.animation().skin(e, key);
        return sp ? sp->palette : nullptr;
    };
    g.posed = [&engine](EntityId e, const std::string& key) { return engine.animation().posedMesh(e, key); };
    g.bodyColliders = [&engine](EntityId e) {
        const MeshRenderer* m = engine.scene().get<MeshRenderer>(e);
        return m ? engine.animation().bodyColliders(e, m->mesh) : std::vector<FxCollider>{};
    };
    g.boneFamily = [&engine](const std::string& meshKey, const std::string& bone) {
        std::vector<std::string> out;
        std::string file = meshKey.rfind("asset:", 0) == 0 ? meshKey.substr(6) : meshKey;
        if (size_t hash = file.rfind('#'); hash != std::string::npos) file = file.substr(0, hash);
        auto lib = engine.animation().library(file);
        if (!lib) return out;
        const anim::Skeleton& sk = (*lib)->skeleton;
        int b = sk.find(bone);
        if (b < 0) return out;
        for (size_t i = 0; i < sk.bones.size(); ++i) {
            if (sk.isDescendant(static_cast<int>(i), b)) out.push_back(sk.bones[i].name);
        }
        return out;
    };
}

}  // namespace sky
