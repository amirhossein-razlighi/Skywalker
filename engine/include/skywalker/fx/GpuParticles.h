#pragma once
// CPU side of GPU particles (`particles` with simulation "gpu"): parsing of curve/gradient
// strings and vector-field files, mesh surface samplers for emission, and packing of an
// emitter into the constant block the compute and render shaders read
// (GpuParticles.metal mirrors GpuEmitterParams field by field).

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/render/FxItems.h"
#include "skywalker/render/MeshData.h"

namespace sky::fx {

/// "#ffd080@0 #ff6020cc@0.4 #00000000@1" -> sorted (t, linear rgba) stops. A stop without
/// "@t" is spread evenly. Colors are sRGB hex like everywhere else (converted to linear).
Result<std::vector<std::pair<float, Vec4>>> parseGradient(std::string_view text);
/// "0@0 1@0.2 0.5@1" -> sorted (t, value) stops.
Result<std::vector<std::pair<float, float>>> parseCurve(std::string_view text);
/// Bakes color (start/end or gradient, times opacity curve) and size curves to tables.
/// Invalid strings fall back to the plain fields and report a warning.
GpuCurves bakeCurves(const ParticleEmitter& em, std::string* warning = nullptr);

/// FGA vector field (the text format Houdini and EmberGen export):
/// "nx,ny,nz,\nminx,miny,minz,\nmaxx,maxy,maxz,\n vx,vy,vz,\n ..." (x fastest).
Result<VectorField> parseFga(std::string_view text);
Result<VectorField> loadFga(const std::string& path);

/// Area-weighted triangle sampler for shape "mesh".
MeshSurface buildMeshSurface(const MeshData& mesh);
/// Built-in particle meshes (fx:leaf, fx:shard, fx:pebble).
bool builtinParticleMesh(const std::string& key, MeshData& out);

enum class GpuShape : int { Point = 0, Sphere = 1, Box = 2, Disc = 3, Cone = 4, Mesh = 5 };
enum class GpuFacing : int { Camera = 0, Velocity = 1, Horizontal = 2, Ribbon = 3, Mesh = 4 };
constexpr int kGpuLookSprite = 8;  // after ParticleLook values
constexpr int kGpuMaxColliders = 8;

/// The constant block of one GPU emitter. All members are float4 / float4x4 rows of floats,
/// so the layout is identical in C++ and Metal (16-byte aligned, no implicit padding).
struct alignas(16) GpuEmitterParams {
    float world[16];         // emitter -> world (column major)
    float delta[16];         // previous world -> current world (worldSpace false: particles follow)
    float fieldInv[16];      // world -> vector field box [0,1]^3
    float spawn[4];          // x shape, y speed, z speed jitter, w cos(spread)
    float shapeSize[4];      // xyz half size (m), w lifetime (s)
    float direction[4];      // xyz emission direction (world), w lifetime jitter
    float forces[4];         // x gravity (m/s^2, down), y drag (1/s), z turbulence (m/s), w 1/turbulence scale
    float wind[4];           // xyz wind (m/s, world, scaled by the emitter's wind), w worldSpace
    float size[4];           // x size start, y size end, z size jitter, w stretch (s)
    float look[4];           // x look, y facing, z intensity, w softness (m)
    float floorPlane[4];     // x enabled, y height, z bounce, w friction
    float collision[4];      // x depth collision, y stick, z collider count, w sub-emit mask (1 death, 2 collision)
    float field[4];          // x type (0 none, 1 vortex, 2 attractor, 3 texture), y strength, z radius, w pull
    float fieldCenter[4];    // xyz world, w lift
    float fieldAxis[4];      // xyz world (unit), w 0
    float flipbook[4];       // x columns, y rows, z fps (0 = over life), w frame count
    float material[4];       // x roughness, y metallic, z spin (rad/s), w has texture
    float sub[4];            // x count per event, y velocity inherit, z has sub-emitter, w seed
    float trail[4];          // x segments, y sample interval (s), z head slot, w 0
    float frame[4];          // x time (s), y dt (s), z particles to spawn this step, w step counter
    float light[4];          // xyz light color (linear), w strength (0 = no light)
    float extra[4];          // x hue variation, y sort, z particles per sub-emitter event (backend), w thin mesh
    float colliders[kGpuMaxColliders][8];  // [ax, ay, az, kind], [bx, by, bz, radius]
    float colorTable[GpuCurves::kSamples][4];
    float sizeTable[GpuCurves::kSamples];
};
static_assert(sizeof(GpuEmitterParams) % 16 == 0);

/// Packs the frame-independent part of an emitter (frame / trail head / delta are filled
/// by the backend each step).
GpuEmitterParams packEmitter(const GpuEmitterItem& item);

/// Emission-direction cone, shape and look identifiers (exposed for tests).
int gpuShapeId(const std::string& shape);
int gpuFacingId(const std::string& facing);
int gpuLookId(const std::string& look);

}  // namespace sky::fx
