#pragma once
// Internal glue between Skywalker and Jolt Physics: one-time library setup, math
// conversions, collision layers and the layer matrix. Only physics sources include this.

// Jolt.h must come first in every translation unit that uses Jolt.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/math/Math.h"

namespace sky::physics {

/// Registers Jolt's allocator, factory and types exactly once per process (thread-safe).
void ensureJoltInitialized();
/// A process-wide worker pool for PhysicsSystem::Update (deterministic regardless of thread count).
JPH::JobSystem& sharedJobSystem();

// --- Math -----------------------------------------------------------------------------------
inline JPH::Vec3 toJolt(Vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline Vec3 fromJolt(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }

/// Euler degrees (Skywalker convention: R = Ry(yaw) * Rx(pitch) * Rz(roll)) <-> quaternion.
JPH::Quat eulerToQuat(Vec3 degrees);
Vec3 quatToEuler(JPH::QuatArg q);

/// Splits an affine matrix (no shear) into translation, rotation and per-axis scale.
struct Decomposed {
    Vec3 translation;
    JPH::Quat rotation = JPH::Quat::sIdentity();
    Vec3 scale{1.f};
};
Decomposed decompose(const Mat4& m);
/// Rigid transform (rotation + translation) as a Skywalker matrix.
Mat4 rigidMatrix(Vec3 position, JPH::QuatArg rotation);

// --- Layers -----------------------------------------------------------------------------------
// An object layer is the user-facing collision layer (low bits) plus a bit for non-moving
// bodies, so static geometry can live on any layer yet stay in the cheap static broad phase.
constexpr int kLayerCount = 7;
enum Layer : JPH::ObjectLayer { kDefault = 0, kStatic, kPlayer, kEnemy, kProjectile, kTrigger, kDebris };
constexpr JPH::ObjectLayer kNonMovingBit = 8;
constexpr JPH::ObjectLayer kSensorBit = 16;  // triggers never test against static geometry
inline JPH::ObjectLayer objectLayer(JPH::ObjectLayer layer, bool nonMoving, bool sensor = false) {
    return static_cast<JPH::ObjectLayer>(layer | (nonMoving ? kNonMovingBit : 0) | (sensor ? kSensorBit : 0));
}
inline JPH::ObjectLayer userLayer(JPH::ObjectLayer objectLayer) { return static_cast<JPH::ObjectLayer>(objectLayer & 7); }
inline bool isNonMoving(JPH::ObjectLayer objectLayer) { return (objectLayer & kNonMovingBit) != 0; }
inline bool isSensorLayer(JPH::ObjectLayer objectLayer) { return (objectLayer & kSensorBit) != 0; }
const std::array<std::string_view, kLayerCount>& layerNames();
/// Layer index for a name; falls back to `fallback` for unknown names.
JPH::ObjectLayer layerIndex(std::string_view name, JPH::ObjectLayer fallback = kDefault);

namespace bp {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr unsigned kCount = 2;
}  // namespace bp

/// Which layer pairs collide. Edited only between simulation steps (read from job threads).
class LayerMatrix final : public JPH::ObjectLayerPairFilter {
public:
    LayerMatrix();
    /// Parses "a-b, c-d" pairs; returns human-readable problems (unknown layer names).
    std::vector<std::string> setIgnoredPairs(std::string_view spec);
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override;

private:
    std::array<std::array<bool, kLayerCount>, kLayerCount> collide_{};
};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    unsigned GetNumBroadPhaseLayers() const override { return bp::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return isNonMoving(layer) ? bp::kNonMoving : bp::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp::kNonMoving ? "static" : "moving";
    }
#endif
};

/// Static bodies only need to test against moving ones; everything else tests both.
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override {
        if (bpLayer == bp::kNonMoving) return !isNonMoving(layer) && !isSensorLayer(layer);
        return true;
    }
};

/// Per-collider surface material (friction/restitution overrides), combined in the contact listener.
class SurfaceMaterial final : public JPH::PhysicsMaterial {
public:
    SurfaceMaterial(float frictionValue, float restitutionValue) : friction(frictionValue), restitution(restitutionValue) {}
    const float friction;
    const float restitution;
};

}  // namespace sky::physics
