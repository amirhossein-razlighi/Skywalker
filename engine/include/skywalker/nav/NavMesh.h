#pragma once
// NavMesh: a tiled Recast/Detour navigation mesh built from world-space triangles, with path
// queries and a compact binary save format. Recast-free header (pimpl).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace sky::nav {

struct BuildSettings {
    float cellSize = 0.2f;
    float cellHeight = 0.1f;
    float agentHeight = 1.8f;
    float agentRadius = 0.4f;
    float agentMaxClimb = 0.4f;
    float agentMaxSlope = 45.f;  // degrees
    int tileSize = 64;           // cells
    float regionMinSize = 8.f;   // cells (sqrt of area)
    float regionMergeSize = 20.f;
    float edgeMaxLen = 12.f;     // m
    float edgeMaxError = 1.3f;   // cells
    float detailSampleDist = 6.f;   // cells
    float detailSampleMaxError = 1.f;  // cell heights
};

struct BuildReport {
    int tiles = 0;          // tiles that contain walkable area
    int tileGrid[2] = {0, 0};
    int polygons = 0;
    int inputTriangles = 0;
    double seconds = 0;
    Aabb bounds;
    float walkableArea = 0;  // m^2 (approximate, from the polygon mesh)
};

struct PathResult {
    bool found = false;     // a path exists (possibly partial)
    bool partial = false;   // the goal is unreachable; the path ends at the closest reachable point
    std::vector<Vec3> points;  // straight (string-pulled) path, start to end
    float length = 0;       // m along the path
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    /// Builds from a triangle soup (3 vertices per triangle, world space). Replaces any mesh.
    Result<BuildReport> build(const std::vector<Vec3>& triangles, const BuildSettings& settings);
    Status save(const std::string& path) const;
    Status load(const std::string& path);
    bool valid() const;

    /// Hash of the input geometry + settings this mesh was built from (staleness check).
    uint64_t sourceHash() const { return sourceHash_; }
    void setSourceHash(uint64_t h) { sourceHash_ = h; }
    const BuildSettings& settings() const { return settings_; }
    const BuildReport& report() const { return report_; }

    PathResult findPath(Vec3 from, Vec3 to) const;
    /// Closest point on the mesh within `searchExtents` (half sizes), if any.
    std::optional<Vec3> nearestPoint(Vec3 p, Vec3 searchExtents = {2.f, 4.f, 2.f}) const;
    /// Polygon outlines (world space) for debug views.
    std::vector<std::vector<Vec3>> polygons() const;

    dtNavMesh* detour() const { return mesh_; }
    dtNavMeshQuery* query() const { return query_; }

    /// Hash of a triangle soup plus build settings.
    static uint64_t hashInput(const std::vector<Vec3>& triangles, const BuildSettings& settings);

private:
    void reset();
    Status initQuery();

    dtNavMesh* mesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
    BuildSettings settings_;
    BuildReport report_;
    uint64_t sourceHash_ = 0;
};

}  // namespace sky::nav
