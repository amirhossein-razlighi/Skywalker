#include "CharacterHooks.h"

#include <algorithm>
#include <cmath>

#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/fx/Groom.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/physics/PhysicsWorld.h"
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

}  // namespace

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
