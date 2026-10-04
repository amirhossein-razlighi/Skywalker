#include "skywalker/fx/GroomBinding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "skywalker/anim/AnimMath.h"

namespace sky::fx {

namespace {

Vec3 vpos(const float* v, size_t i) { return {v[i * 12], v[i * 12 + 1], v[i * 12 + 2]}; }
Vec3 vnrm(const float* v, size_t i) { return {v[i * 12 + 3], v[i * 12 + 4], v[i * 12 + 5]}; }

/// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5): barycentrics (v, w).
void closestOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c, float& v, float& w) {
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) { v = 0.f; w = 0.f; return; }
    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) { v = 1.f; w = 0.f; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) { v = d1 / (d1 - d3); w = 0.f; return; }
    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) { v = 0.f; w = 1.f; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) { v = 0.f; w = d2 / (d2 - d6); return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        v = 1.f - w;
        return;
    }
    float denom = 1.f / (va + vb + vc);
    v = vb * denom;
    w = vc * denom;
}

Vec4 quatFromBasis(Vec3 x, Vec3 y, Vec3 z) {
    Mat4 m;
    m.at(0, 0) = x.x, m.at(0, 1) = x.y, m.at(0, 2) = x.z;
    m.at(1, 0) = y.x, m.at(1, 1) = y.y, m.at(1, 2) = y.z;
    m.at(2, 0) = z.x, m.at(2, 1) = z.y, m.at(2, 2) = z.z;
    anim::Quat q = anim::Quat::fromMatrix(m);
    return {q.x, q.y, q.z, q.w};
}

}  // namespace

Vec4 quatMul(const Vec4& a, const Vec4& b) {
    anim::Quat r = anim::Quat{a.x, a.y, a.z, a.w} * anim::Quat{b.x, b.y, b.z, b.w};
    return {r.x, r.y, r.z, r.w};
}

Vec3 quatRotate(const Vec4& q, Vec3 v) { return anim::Quat{q.x, q.y, q.z, q.w}.rotate(v); }

Vec4 rootRotation(const Vec4& current, const Vec4& rest) { return quatMul(current, Vec4{-rest.x, -rest.y, -rest.z, rest.w}); }

RootFrame evalRoot(const float* v, size_t n, const GroomData::RootBind& b, Vec3 s) {
    RootFrame f;
    if (!v || n == 0) return f;
    size_t i0 = std::min<size_t>(b.v[0], n - 1), i1 = std::min<size_t>(b.v[1], n - 1), i2 = std::min<size_t>(b.v[2], n - 1);
    float b0 = 1.f - b.b1 - b.b2;
    Vec3 p0 = vpos(v, i0) * s, p1 = vpos(v, i1) * s, p2 = vpos(v, i2) * s;
    f.position = p0 * b0 + p1 * b.b1 + p2 * b.b2;
    Vec3 nrm = vnrm(v, i0) * b0 + vnrm(v, i1) * b.b1 + vnrm(v, i2) * b.b2;
    Vec3 face = cross(p1 - p0, p2 - p0);
    if (length(nrm) < 1e-8f) nrm = face;
    nrm = length(nrm) > 1e-12f ? normalize(nrm) : Vec3{0, 1, 0};
    Vec3 t = p1 - p0;
    t = t - nrm * dot(t, nrm);
    if (length(t) < 1e-9f) {
        t = p2 - p0;
        t = t - nrm * dot(t, nrm);
    }
    if (length(t) < 1e-9f) t = std::fabs(nrm.x) < 0.9f ? cross(nrm, Vec3{1, 0, 0}) : cross(nrm, Vec3{0, 0, 1});
    t = normalize(t);
    Vec3 bt = cross(t, nrm);
    f.rotation = quatFromBasis(t, nrm, bt);
    return f;
}

void computeRestFrames(GroomData& d, const MeshData& rest, Vec3 scale) {
    d.meshVertices = static_cast<uint32_t>(rest.vertexCount());
    d.guideFrames.resize(d.guideBind.size());
    for (size_t g = 0; g < d.guideBind.size(); ++g) {
        d.guideFrames[g] = evalRoot(rest.vertices.data(), rest.vertexCount(), d.guideBind[g], scale).rotation;
    }
}

std::vector<GroomData::RootBind> bindToMesh(const MeshData& mesh, const std::vector<Vec3>& points, float* maxError) {
    std::vector<GroomData::RootBind> out(points.size());
    if (maxError) *maxError = 0.f;
    const size_t tris = mesh.indices.size() / 3, nv = mesh.vertexCount();
    if (tris == 0 || nv == 0) return out;
    const float* v = mesh.vertices.data();
    Aabb b{Vec3(1e30f), Vec3(-1e30f)};
    for (size_t i = 0; i < nv; ++i) {
        b.min = vmin(b.min, vpos(v, i));
        b.max = vmax(b.max, vpos(v, i));
    }
    const float cell = std::max(length(b.max - b.min) / std::max(8.f, std::cbrt(static_cast<float>(tris)) * 2.f), 1e-4f);
    auto cellOf = [&](Vec3 p) {
        return std::array<int, 3>{static_cast<int>(std::floor((p.x - b.min.x) / cell)), static_cast<int>(std::floor((p.y - b.min.y) / cell)),
                                  static_cast<int>(std::floor((p.z - b.min.z) / cell))};
    };
    auto key = [](int x, int y, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1fffff) << 42) | (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1fffff) << 21) |
               static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1fffff);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (size_t t = 0; t < tris; ++t) {
        Vec3 a = vpos(v, mesh.indices[t * 3]), c1 = vpos(v, mesh.indices[t * 3 + 1]), c2 = vpos(v, mesh.indices[t * 3 + 2]);
        auto lo = cellOf(vmin(a, vmin(c1, c2))), hi = cellOf(vmax(a, vmax(c1, c2)));
        for (int z = lo[2]; z <= hi[2]; ++z) {
            for (int y = lo[1]; y <= hi[1]; ++y) {
                for (int x = lo[0]; x <= hi[0]; ++x) grid[key(x, y, z)].push_back(static_cast<uint32_t>(t));
            }
        }
    }
    float worst = 0.f;
    for (size_t pi = 0; pi < points.size(); ++pi) {
        const Vec3 p = points[pi];
        auto c = cellOf(p);
        float best = 1e30f;
        GroomData::RootBind bestBind;
        for (int ring = 0; ring < 64; ring = ring ? ring * 2 : 1) {
            for (int z = c[2] - ring; z <= c[2] + ring; ++z) {
                for (int y = c[1] - ring; y <= c[1] + ring; ++y) {
                    for (int x = c[0] - ring; x <= c[0] + ring; ++x) {
                        auto it = grid.find(key(x, y, z));
                        if (it == grid.end()) continue;
                        for (uint32_t t : it->second) {
                            uint32_t i0 = mesh.indices[t * 3], i1 = mesh.indices[t * 3 + 1], i2 = mesh.indices[t * 3 + 2];
                            Vec3 a = vpos(v, i0), e1 = vpos(v, i1), e2 = vpos(v, i2);
                            float bv = 0.f, bw = 0.f;
                            closestOnTriangle(p, a, e1, e2, bv, bw);
                            Vec3 q = a * (1.f - bv - bw) + e1 * bv + e2 * bw;
                            float dd = distance(p, q);
                            if (dd < best) {
                                best = dd;
                                bestBind.v[0] = i0;
                                bestBind.v[1] = i1;
                                bestBind.v[2] = i2;
                                bestBind.b1 = bv;
                                bestBind.b2 = bw;
                            }
                        }
                    }
                }
            }
            // Everything outside the searched block is at least ring * cell away.
            if (best <= static_cast<float>(ring) * cell) break;
        }
        out[pi] = bestBind;
        worst = std::max(worst, best < 1e29f ? best : 0.f);
    }
    if (maxError) *maxError = worst;
    return out;
}

std::vector<RootFrame> skinnedGuideRoots(const GroomData& d, const MeshData& mesh, const std::vector<Mat4>& palette, Vec3 scale) {
    std::vector<RootFrame> out(d.guideBind.size());
    const SkinStream& skin = mesh.skin;
    const size_t nv = mesh.vertexCount();
    if (skin.empty() || palette.empty() || skin.bind.size() < nv * 6 || skin.joints.size() < nv * 4) {
        for (size_t g = 0; g < out.size(); ++g) out[g] = evalRoot(mesh.vertices.data(), nv, d.guideBind[g], scale);
        return out;
    }
    // Skin only the touched vertices (3 per guide) into a small scratch copy of the vertex layout.
    std::unordered_map<uint32_t, uint32_t> slot;
    std::vector<float> scratch;
    std::vector<GroomData::RootBind> local(d.guideBind.size());
    auto skinned = [&](uint32_t vi) -> uint32_t {
        vi = std::min<uint32_t>(vi, static_cast<uint32_t>(nv - 1));
        auto it = slot.find(vi);
        if (it != slot.end()) return it->second;
        Mat4 m;
        for (float& f : m.m) f = 0.f;
        for (int k = 0; k < SkinStream::kInfluences; ++k) {
            float w = skin.weights[vi * 4 + static_cast<size_t>(k)];
            if (w <= 0.f) continue;
            const Mat4& j = palette[std::min<size_t>(skin.joints[vi * 4 + static_cast<size_t>(k)], palette.size() - 1)];
            for (int e = 0; e < 16; ++e) m.m[e] += j.m[e] * w;
        }
        const float* bp = &skin.bind[vi * 6];
        Vec3 p = m.transformPoint({bp[0], bp[1], bp[2]}), n = m.transformDir({bp[3], bp[4], bp[5]});
        uint32_t idx = static_cast<uint32_t>(scratch.size() / 12);
        scratch.insert(scratch.end(), {p.x, p.y, p.z, n.x, n.y, n.z, 0, 0, 0, 0, 0, 0});
        slot.emplace(vi, idx);
        return idx;
    };
    for (size_t g = 0; g < d.guideBind.size(); ++g) {
        const auto& b = d.guideBind[g];
        local[g] = b;
        for (int k = 0; k < 3; ++k) local[g].v[k] = skinned(b.v[k]);
    }
    for (size_t g = 0; g < out.size(); ++g) out[g] = evalRoot(scratch.data(), scratch.size() / 12, local[g], scale);
    return out;
}

}  // namespace sky::fx
