#pragma once
// Internal: the wheeled vehicles of one PhysicsWorld. One Jolt VehicleConstraint (wheeled
// controller: engine, gearbox, differentials, anti-roll bars) per active `vehicle` entity whose
// chassis has a dynamic body. PhysicsWorld drives it: sync() after the bodies, preStep() before
// the Jolt update (inputs, assists, aero), postStep() after it (telemetry, write-back).

#include "JoltCommon.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "skywalker/ecs/Components.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/physics/Vehicle.h"

namespace sky::physics {

/// What the vehicles need from their world.
struct VehicleHost {
    JPH::PhysicsSystem* system = nullptr;
    std::function<JPH::BodyID(EntityId)> bodyOf;        // the entity's own solid body (invalid if none)
    std::function<EntityId(uint32_t)> entityOf;         // body id (index and sequence) -> entity
    std::function<void(std::string)> warn;
    MeshProvider meshes;
};

/// The chassis shape with the vehicle's center-of-mass offset applied.
JPH::RefConst<JPH::Shape> vehicleChassisShape(JPH::RefConst<JPH::Shape> shape, const Vehicle& v);

class VehicleSet {
public:
    explicit VehicleSet(VehicleHost host);
    ~VehicleSet();
    VehicleSet(const VehicleSet&) = delete;
    VehicleSet& operator=(const VehicleSet&) = delete;

    /// Vehicles whose chassis body is about to be destroyed (ids as GetIndexAndSequenceNumber).
    void forgetBodies(const std::set<uint32_t>& doomed);
    /// Creates, rebuilds and removes constraints to match the scene.
    void sync(const Scene& scene);
    /// Inputs, assists and aero forces for the next update.
    void preStep(const Scene& scene, float dt);
    /// Telemetry after the update; with a scene, writes components, wheel visuals and audio.
    void postStep(Scene* scene, float dt, int collisionSteps);

    std::vector<EntityId> entities() const;
    std::optional<VehicleTelemetry> telemetry(EntityId e) const;
    bool setInput(EntityId e, std::optional<VehicleInput> input);
    bool shift(EntityId e, int gear);
    VehicleStats stats() const;
    size_t size() const { return entries_.size(); }

    struct Entry;

private:
    void remove(std::map<EntityId, std::unique_ptr<Entry>>::iterator it);
    bool create(const Scene& scene, EntityId e, const Vehicle& v, JPH::BodyID body, uint64_t signature, uint64_t geometry,
                const Entry* previous);

    VehicleHost host_;
    std::map<EntityId, std::unique_ptr<Entry>> entries_;
    std::map<EntityId, std::pair<uint64_t, uint64_t>> failed_;  // setups that could not be built: (signature, geometry)
    bool stepped_ = false;
    double lastStepMs_ = 0.0;
};

}  // namespace sky::physics
