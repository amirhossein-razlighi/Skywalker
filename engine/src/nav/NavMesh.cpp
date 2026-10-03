#include "skywalker/nav/NavMesh.h"

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sky::nav {

namespace {

constexpr char kMagic[8] = {'S', 'K', 'Y', 'N', 'A', 'V', '0', '1'};
constexpr uint32_t kVersion = 1;
constexpr int kMaxPathPolys = 512;
constexpr int kMaxStraight = 256;
constexpr unsigned short kWalkFlag = 1;

/// RAII for Recast's C-style allocations.
template <typename T, void (*Free)(T*)>
struct RcPtr {
    T* p = nullptr;
    explicit RcPtr(T* x) : p(x) {}
    ~RcPtr() { Free(p); }
    RcPtr(const RcPtr&) = delete;
    RcPtr& operator=(const RcPtr&) = delete;
    T* operator->() const { return p; }
    T& operator*() const { return *p; }
    explicit operator bool() const { return p != nullptr; }
};
using Heightfield = RcPtr<rcHeightfield, rcFreeHeightField>;
using CompactHeightfield = RcPtr<rcCompactHeightfield, rcFreeCompactHeightfield>;
using ContourSet = RcPtr<rcContourSet, rcFreeContourSet>;
using PolyMesh = RcPtr<rcPolyMesh, rcFreePolyMesh>;
using PolyMeshDetail = RcPtr<rcPolyMeshDetail, rcFreePolyMeshDetail>;

unsigned nextPow2(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}
unsigned ilog2(unsigned v) {
    unsigned r = 0;
    while (v >>= 1) ++r;
    return r;
}

template <typename T>
void put(std::ofstream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool get(std::ifstream& in, T& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

}  // namespace

NavMesh::NavMesh() = default;
NavMesh::~NavMesh() { reset(); }

void NavMesh::reset() {
    dtFreeNavMeshQuery(query_);
    dtFreeNavMesh(mesh_);
    query_ = nullptr;
    mesh_ = nullptr;
    report_ = {};
}

bool NavMesh::valid() const { return mesh_ && query_; }

Status NavMesh::initQuery() {
    query_ = dtAllocNavMeshQuery();
    if (!query_ || dtStatusFailed(query_->init(mesh_, 4096))) return Error::make("nav_error", "could not initialize the navmesh query");
    return {};
}

uint64_t NavMesh::hashInput(const std::vector<Vec3>& triangles, const BuildSettings& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    size_t n = triangles.size();
    mix(&n, sizeof(n));
    if (!triangles.empty()) mix(triangles.data(), triangles.size() * sizeof(Vec3));
    mix(&s, sizeof(BuildSettings));
    return h;
}

Result<BuildReport> NavMesh::build(const std::vector<Vec3>& tris, const BuildSettings& in) {
    auto start = std::chrono::steady_clock::now();
    reset();
    settings_ = in;
    BuildSettings s = in;
    s.cellSize = std::max(s.cellSize, 0.01f);
    s.cellHeight = std::max(s.cellHeight, 0.01f);
    s.tileSize = std::clamp(s.tileSize, 16, 256);
    const size_t triCount = tris.size() / 3;
    if (triCount == 0) {
        return Error::make("nav_no_geometry", "nothing to build a navmesh from",
                           "add static colliders or meshes (floors, terrain) — dynamic bodies and characters are ignored");
    }

    Aabb bounds{Vec3(1e30f), Vec3(-1e30f)};
    for (const Vec3& v : tris) {
        bounds.min = vmin(bounds.min, v);
        bounds.max = vmax(bounds.max, v);
    }
    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = std::clamp(s.agentMaxSlope, 0.f, 89.f);
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(s.edgeMaxLen / cfg.cs);
    cfg.maxSimplificationError = s.edgeMaxError;
    cfg.minRegionArea = static_cast<int>(s.regionMinSize * s.regionMinSize);
    cfg.mergeRegionArea = static_cast<int>(s.regionMergeSize * s.regionMergeSize);
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.tileSize = s.tileSize;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = s.detailSampleDist < 0.9f ? 0 : cfg.cs * s.detailSampleDist;
    cfg.detailSampleMaxError = cfg.ch * s.detailSampleMaxError;

    float bmin[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
    float bmax[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
    int gw = 0, gh = 0;
    rcCalcGridSize(bmin, bmax, cfg.cs, &gw, &gh);
    const int tw = (gw + cfg.tileSize - 1) / cfg.tileSize;
    const int th = (gh + cfg.tileSize - 1) / cfg.tileSize;
    unsigned tileBits = std::min(ilog2(nextPow2(static_cast<unsigned>(std::max(tw * th, 1)))), 14u);
    unsigned polyBits = 22 - tileBits;
    if (static_cast<long long>(tw) * th > (1ll << tileBits)) {
        return Error::make("nav_too_large", "the level is too large for the navmesh tile budget",
                           "increase navmesh.tileSize or cellSize");
    }

    mesh_ = dtAllocNavMesh();
    if (!mesh_) return Error::make("nav_error", "out of memory");
    dtNavMeshParams params{};
    rcVcopy(params.orig, bmin);
    params.tileWidth = static_cast<float>(cfg.tileSize) * cfg.cs;
    params.tileHeight = static_cast<float>(cfg.tileSize) * cfg.cs;
    params.maxTiles = 1 << tileBits;
    params.maxPolys = 1 << polyBits;
    if (dtStatusFailed(mesh_->init(&params))) {
        reset();
        return Error::make("nav_error", "could not initialize the navmesh");
    }

    // Bin triangles into tiles (with the border) so each tile rasterizes only what touches it.
    const float tcs = params.tileWidth;
    const float border = static_cast<float>(cfg.borderSize) * cfg.cs;
    std::vector<std::vector<int>> bins(static_cast<size_t>(tw) * th);
    std::vector<unsigned char> areas(triCount, RC_NULL_AREA);
    const float walkableThr = std::cos(cfg.walkableSlopeAngle / 180.f * RC_PI);
    for (size_t t = 0; t < triCount; ++t) {
        const Vec3 &a = tris[t * 3], &b = tris[t * 3 + 1], &c = tris[t * 3 + 2];
        Vec3 n = normalize(cross(b - a, c - a));
        if (n.y > walkableThr) areas[t] = RC_WALKABLE_AREA;
        float x0 = std::min({a.x, b.x, c.x}) - border, x1 = std::max({a.x, b.x, c.x}) + border;
        float z0 = std::min({a.z, b.z, c.z}) - border, z1 = std::max({a.z, b.z, c.z}) + border;
        int tx0 = std::clamp(static_cast<int>(std::floor((x0 - bmin[0]) / tcs)), 0, tw - 1);
        int tx1 = std::clamp(static_cast<int>(std::floor((x1 - bmin[0]) / tcs)), 0, tw - 1);
        int tz0 = std::clamp(static_cast<int>(std::floor((z0 - bmin[2]) / tcs)), 0, th - 1);
        int tz1 = std::clamp(static_cast<int>(std::floor((z1 - bmin[2]) / tcs)), 0, th - 1);
        for (int z = tz0; z <= tz1; ++z) {
            for (int x = tx0; x <= tx1; ++x) bins[static_cast<size_t>(z) * tw + x].push_back(static_cast<int>(t));
        }
    }

    rcContext ctx(false);
    BuildReport report;
    report.tileGrid[0] = tw;
    report.tileGrid[1] = th;
    report.inputTriangles = static_cast<int>(triCount);
    report.bounds = bounds;
    std::vector<float> soup;
    std::vector<unsigned char> soupAreas;
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            const auto& bin = bins[static_cast<size_t>(ty) * tw + tx];
            if (bin.empty()) continue;
            rcConfig tc = cfg;
            tc.bmin[0] = bmin[0] + static_cast<float>(tx) * tcs - border;
            tc.bmin[1] = bmin[1];
            tc.bmin[2] = bmin[2] + static_cast<float>(ty) * tcs - border;
            tc.bmax[0] = bmin[0] + static_cast<float>(tx + 1) * tcs + border;
            tc.bmax[1] = bmax[1];
            tc.bmax[2] = bmin[2] + static_cast<float>(ty + 1) * tcs + border;

            soup.clear();
            soupAreas.clear();
            for (int t : bin) {
                for (int k = 0; k < 3; ++k) {
                    const Vec3& v = tris[static_cast<size_t>(t) * 3 + k];
                    soup.insert(soup.end(), {v.x, v.y, v.z});
                }
                soupAreas.push_back(areas[static_cast<size_t>(t)]);
            }
            Heightfield hf(rcAllocHeightfield());
            if (!hf || !rcCreateHeightfield(&ctx, *hf, tc.width, tc.height, tc.bmin, tc.bmax, tc.cs, tc.ch)) continue;
            if (!rcRasterizeTriangles(&ctx, soup.data(), soupAreas.data(), static_cast<int>(soupAreas.size()), *hf,
                                      tc.walkableClimb)) {
                continue;
            }
            rcFilterLowHangingWalkableObstacles(&ctx, tc.walkableClimb, *hf);
            rcFilterLedgeSpans(&ctx, tc.walkableHeight, tc.walkableClimb, *hf);
            rcFilterWalkableLowHeightSpans(&ctx, tc.walkableHeight, *hf);
            CompactHeightfield chf(rcAllocCompactHeightfield());
            if (!chf || !rcBuildCompactHeightfield(&ctx, tc.walkableHeight, tc.walkableClimb, *hf, *chf)) continue;
            if (!rcErodeWalkableArea(&ctx, tc.walkableRadius, *chf)) continue;
            if (!rcBuildDistanceField(&ctx, *chf)) continue;
            if (!rcBuildRegions(&ctx, *chf, tc.borderSize, tc.minRegionArea, tc.mergeRegionArea)) continue;
            ContourSet cset(rcAllocContourSet());
            if (!cset || !rcBuildContours(&ctx, *chf, tc.maxSimplificationError, tc.maxEdgeLen, *cset)) continue;
            if (cset->nconts == 0) continue;
            PolyMesh pmesh(rcAllocPolyMesh());
            if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, tc.maxVertsPerPoly, *pmesh)) continue;
            PolyMeshDetail dmesh(rcAllocPolyMeshDetail());
            if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, tc.detailSampleDist, tc.detailSampleMaxError, *dmesh)) continue;
            if (pmesh->npolys == 0) continue;
            for (int i = 0; i < pmesh->npolys; ++i) {
                if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->areas[i] = 0;
                pmesh->flags[i] = kWalkFlag;
            }
            dtNavMeshCreateParams cp{};
            cp.verts = pmesh->verts;
            cp.vertCount = pmesh->nverts;
            cp.polys = pmesh->polys;
            cp.polyAreas = pmesh->areas;
            cp.polyFlags = pmesh->flags;
            cp.polyCount = pmesh->npolys;
            cp.nvp = pmesh->nvp;
            cp.detailMeshes = dmesh->meshes;
            cp.detailVerts = dmesh->verts;
            cp.detailVertsCount = dmesh->nverts;
            cp.detailTris = dmesh->tris;
            cp.detailTriCount = dmesh->ntris;
            cp.walkableHeight = s.agentHeight;
            cp.walkableRadius = s.agentRadius;
            cp.walkableClimb = s.agentMaxClimb;
            cp.tileX = tx;
            cp.tileY = ty;
            cp.tileLayer = 0;
            rcVcopy(cp.bmin, pmesh->bmin);
            rcVcopy(cp.bmax, pmesh->bmax);
            cp.cs = tc.cs;
            cp.ch = tc.ch;
            cp.buildBvTree = true;
            unsigned char* data = nullptr;
            int dataSize = 0;
            if (!dtCreateNavMeshData(&cp, &data, &dataSize)) continue;
            if (dtStatusFailed(mesh_->addTile(data, dataSize, DT_TILE_FREE_DATA, 0, nullptr))) {
                dtFree(data);
                continue;
            }
            ++report.tiles;
            report.polygons += pmesh->npolys;
        }
    }
    if (Status st = initQuery(); !st) {
        reset();
        return st.error();
    }
    if (report.tiles == 0) {
        reset();
        return Error::make("nav_no_walkable", "no walkable surface found",
                           "floors must be flatter than maxSlope and leave agentHeight of headroom; agentRadius erodes "
                           "narrow areas away");
    }
    // Approximate walkable area from the polygon outlines.
    for (const auto& poly : polygons()) {
        for (size_t i = 1; i + 1 < poly.size(); ++i) {
            report.walkableArea += 0.5f * length(cross(poly[i] - poly[0], poly[i + 1] - poly[0]));
        }
    }
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    report_ = report;
    sourceHash_ = hashInput(tris, in);
    return report;
}

Status NavMesh::save(const std::string& path) const {
    if (!mesh_) return Error::make("nav_error", "no navmesh to save", "build one with nav_build");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return Error::make("io_error", "cannot write " + path);
    out.write(kMagic, sizeof(kMagic));
    put(out, kVersion);
    put(out, sourceHash_);
    put(out, settings_);
    put(out, report_);
    const dtNavMeshParams* params = mesh_->getParams();
    put(out, *params);
    int count = 0;
    const dtNavMesh* cm = mesh_;
    for (int i = 0; i < cm->getMaxTiles(); ++i) {
        const dtMeshTile* tile = cm->getTile(i);
        if (tile && tile->header && tile->dataSize) ++count;
    }
    put(out, count);
    for (int i = 0; i < cm->getMaxTiles(); ++i) {
        const dtMeshTile* tile = cm->getTile(i);
        if (!tile || !tile->header || !tile->dataSize) continue;
        dtTileRef ref = cm->getTileRef(tile);
        put(out, ref);
        put(out, tile->dataSize);
        out.write(reinterpret_cast<const char*>(tile->data), tile->dataSize);
    }
    if (!out) return Error::make("io_error", "failed writing " + path);
    return {};
}

Status NavMesh::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error::make("not_found", "no navmesh file at " + path, "build one with nav_build");
    char magic[8];
    uint32_t version = 0;
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || !get(in, version) ||
        version != kVersion) {
        return Error::make("invalid_navmesh", path + " is not a Skywalker navmesh (or an older format)",
                           "rebuild it with nav_build");
    }
    uint64_t hash = 0;
    BuildSettings settings;
    BuildReport report;
    dtNavMeshParams params{};
    int count = 0;
    if (!get(in, hash) || !get(in, settings) || !get(in, report) || !get(in, params) || !get(in, count) || count < 0) {
        return Error::make("invalid_navmesh", path + " is truncated");
    }
    reset();
    mesh_ = dtAllocNavMesh();
    if (!mesh_ || dtStatusFailed(mesh_->init(&params))) {
        reset();
        return Error::make("nav_error", "could not initialize the navmesh");
    }
    for (int i = 0; i < count; ++i) {
        dtTileRef ref = 0;
        int size = 0;
        if (!get(in, ref) || !get(in, size) || size <= 0 || size > (256 << 20)) {
            reset();
            return Error::make("invalid_navmesh", path + " is corrupt");
        }
        auto* data = static_cast<unsigned char*>(dtAlloc(static_cast<size_t>(size), DT_ALLOC_PERM));
        if (!data || !in.read(reinterpret_cast<char*>(data), size)) {
            dtFree(data);
            reset();
            return Error::make("invalid_navmesh", path + " is truncated");
        }
        if (dtStatusFailed(mesh_->addTile(data, size, DT_TILE_FREE_DATA, ref, nullptr))) {
            dtFree(data);
            reset();
            return Error::make("invalid_navmesh", path + " has an invalid tile");
        }
    }
    if (Status st = initQuery(); !st) {
        reset();
        return st;
    }
    settings_ = settings;
    report_ = report;
    sourceHash_ = hash;
    return {};
}

std::optional<Vec3> NavMesh::nearestPoint(Vec3 p, Vec3 ext) const {
    if (!valid()) return std::nullopt;
    dtQueryFilter filter;
    float pos[3] = {p.x, p.y, p.z}, extents[3] = {ext.x, ext.y, ext.z}, nearest[3];
    dtPolyRef ref = 0;
    if (dtStatusFailed(query_->findNearestPoly(pos, extents, &filter, &ref, nearest)) || !ref) return std::nullopt;
    return Vec3{nearest[0], nearest[1], nearest[2]};
}

PathResult NavMesh::findPath(Vec3 from, Vec3 to) const {
    PathResult r;
    if (!valid()) return r;
    dtQueryFilter filter;
    float ext[3] = {2.f, 4.f, 2.f};
    float sp[3] = {from.x, from.y, from.z}, ep[3] = {to.x, to.y, to.z};
    float ns[3], ne[3];
    dtPolyRef startRef = 0, endRef = 0;
    query_->findNearestPoly(sp, ext, &filter, &startRef, ns);
    query_->findNearestPoly(ep, ext, &filter, &endRef, ne);
    if (!startRef || !endRef) return r;
    dtPolyRef path[kMaxPathPolys];
    int npath = 0;
    if (dtStatusFailed(query_->findPath(startRef, endRef, ns, ne, &filter, path, &npath, kMaxPathPolys)) || npath == 0) return r;
    float target[3];
    rcVcopy(target, ne);
    if (path[npath - 1] != endRef) {
        r.partial = true;
        bool over = false;
        query_->closestPointOnPoly(path[npath - 1], ne, target, &over);
    }
    float straight[kMaxStraight * 3];
    unsigned char flags[kMaxStraight];
    dtPolyRef refs[kMaxStraight];
    int n = 0;
    if (dtStatusFailed(query_->findStraightPath(ns, target, path, npath, straight, flags, refs, &n, kMaxStraight)) || n == 0) {
        return r;
    }
    r.found = true;
    for (int i = 0; i < n; ++i) r.points.push_back({straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]});
    for (size_t i = 1; i < r.points.size(); ++i) r.length += distance(r.points[i - 1], r.points[i]);
    return r;
}

std::vector<std::vector<Vec3>> NavMesh::polygons() const {
    std::vector<std::vector<Vec3>> out;
    if (!mesh_) return out;
    const dtNavMesh* cm = mesh_;
    for (int i = 0; i < cm->getMaxTiles(); ++i) {
        const dtMeshTile* tile = cm->getTile(i);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            std::vector<Vec3> pts;
            for (int v = 0; v < poly.vertCount; ++v) {
                const float* vp = &tile->verts[poly.verts[v] * 3];
                pts.push_back({vp[0], vp[1], vp[2]});
            }
            out.push_back(std::move(pts));
        }
    }
    return out;
}

}  // namespace sky::nav
