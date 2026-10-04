#include "skywalker/anim/BodyColliders.h"

#include <algorithm>
#include <cmath>

#include "AnimationSystemInternal.h"

namespace sky::anim {

namespace {

float distToSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec3 ab = b - a;
    float t = dot(ab, ab) > 1e-12f ? std::clamp(dot(p - a, ab) / dot(ab, ab), 0.f, 1.f) : 0.f;
    return distance(p, a + ab * t);
}

}  // namespace

std::vector<BodyCapsule> fitBodyCapsules(const Skeleton& sk, const MeshData& mesh, const std::vector<int>& slotToBone, size_t maxCapsules) {
    std::vector<BodyCapsule> out;
    const SkinStream& skin = mesh.skin;
    const size_t nv = mesh.vertexCount();
    if (skin.empty() || skin.weights.size() < nv * 4 || skin.bind.size() < nv * 6 || sk.bones.empty()) return out;
    std::vector<Mat4> rest, palette;
    computeGlobals(sk, restPose(sk), rest);
    skinPalette(skin, slotToBone, rest, palette);
    const Mat4 toModel = skin.transform.inverse();  // mesh space -> glTF model space (where the skeleton lives)
    // Rest-posed vertices (model space) and their dominant bone.
    std::vector<std::vector<Vec3>> owned(sk.bones.size());
    for (size_t v = 0; v < nv; ++v) {
        Mat4 m;
        for (float& f : m.m) f = 0.f;
        int best = -1;
        float bestW = 0.f;
        for (int k = 0; k < SkinStream::kInfluences; ++k) {
            float w = skin.weights[v * 4 + static_cast<size_t>(k)];
            uint16_t slot = skin.joints[v * 4 + static_cast<size_t>(k)];
            if (w <= 0.f || slot >= palette.size()) continue;
            for (int e = 0; e < 16; ++e) m.m[e] += palette[slot].m[e] * w;
            if (w > bestW && slot < slotToBone.size() && slotToBone[slot] >= 0) {
                bestW = w;
                best = slotToBone[slot];
            }
        }
        if (best < 0 || bestW < 0.4f) continue;
        const float* bp = &skin.bind[v * 6];
        owned[static_cast<size_t>(best)].push_back(toModel.transformPoint(m.transformPoint({bp[0], bp[1], bp[2]})));
    }
    std::vector<size_t> subtreeCount(sk.bones.size(), 0);
    for (size_t b = sk.bones.size(); b-- > 0;) {
        subtreeCount[b] += owned[b].size();
        int p = sk.bones[b].parent;
        if (p >= 0) subtreeCount[static_cast<size_t>(p)] += subtreeCount[b];
    }
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        const auto& pts = owned[b];
        if (pts.size() < 12) continue;
        BodyCapsule c;
        c.bone = static_cast<int>(b);
        c.vertices = pts.size();
        const Vec3 j = rest[b].translation();
        Vec3 centroid{0, 0, 0};
        for (const Vec3& p : pts) centroid += p;
        centroid = centroid / static_cast<float>(pts.size());
        // The end: the child joint the vertices extend toward (limbs, spine), else along the vertices.
        int children = 0;
        float bestAlign = 0.3f;
        for (size_t k = 0; k < sk.bones.size(); ++k) {
            if (sk.bones[k].parent != static_cast<int>(b) || subtreeCount[k] == 0) continue;
            ++children;
            Vec3 d = rest[k].translation() - j;
            Vec3 toVerts = centroid - j;
            if (length(d) < 1e-5f || length(toVerts) < 1e-5f) continue;
            float align = dot(normalize(d), normalize(toVerts));
            if (align > bestAlign) {
                bestAlign = align;
                c.child = static_cast<int>(k);
            }
        }
        if (children >= 2 && c.child >= 0) {
            // A branching bone (hips, chest): toward the child only if the vertices really follow it.
            Vec3 d = rest[static_cast<size_t>(c.child)].translation() - j;
            if (length(centroid - j) < 0.25f * length(d)) c.child = -1;
        }
        Vec3 end;
        if (c.child >= 0) {
            end = rest[static_cast<size_t>(c.child)].translation();
        } else {
            end = j + (centroid - j) * 2.f;
            c.endLocal = rest[b].inverse().transformPoint(end);
        }
        std::vector<float> d;
        d.reserve(pts.size());
        for (const Vec3& p : pts) d.push_back(distToSegment(p, j, end));
        std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() * 6 / 10), d.end());
        c.radius = std::max(d[d.size() * 6 / 10], 1e-3f);
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const BodyCapsule& a, const BodyCapsule& b) { return a.vertices > b.vertices; });
    if (out.size() > maxCapsules) out.resize(maxCapsules);
    return out;
}

std::vector<FxCollider> AnimationSystem::bodyColliders(EntityId drawEntity, const std::string& meshKey) {
    std::vector<FxCollider> out;
    const MeshData* mesh = hooks.mesh ? hooks.mesh(meshKey) : nullptr;
    EntityId ae = animatorFor(drawEntity);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!mesh || !mesh->skinned() || !inst || !inst->lib) return out;
    const Animator* a = scene_.get<Animator>(ae);
    if (a && !playing_) poseEditing(*inst, ae, *a);
    else if (a && !inst->posed) finishPose(*inst, ae, *a);
    auto it = inst->capsules.find(meshKey);
    if (it == inst->capsules.end()) {
        it = inst->capsules.emplace(meshKey, fitBodyCapsules(inst->lib->skeleton, *mesh, mapSkin(mesh->skin, inst->lib->skeleton))).first;
    }
    const auto& g = inst->globals;
    if (g.size() != inst->lib->skeleton.bones.size()) return out;
    // The capsules live in the skeleton's model space; the draw entity places this mesh's skeleton.
    const Mat4 mw = scene_.worldMatrix(drawEntity) * mesh->skin.transform;
    const float s = (length(mw.transformDir({1, 0, 0})) + length(mw.transformDir({0, 1, 0})) + length(mw.transformDir({0, 0, 1}))) / 3.f;
    for (const BodyCapsule& c : it->second) {
        const Mat4& gb = g[static_cast<size_t>(c.bone)];
        FxCollider col;
        col.kind = FxCollider::Kind::Capsule;
        col.a = mw.transformPoint(gb.translation());
        col.b = mw.transformPoint(c.child >= 0 ? g[static_cast<size_t>(c.child)].translation() : gb.transformPoint(c.endLocal));
        col.radius = c.radius * s * 0.92f;  // slightly inside the skin: strands rest on it
        out.push_back(col);
    }
    return out;
}

}  // namespace sky::anim
