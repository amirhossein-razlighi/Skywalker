#pragma once
// Wheeled vehicles: the Jolt-free interface of the vehicle simulation (physics/Vehicles.cpp),
// its presets, the player controls, the chase camera, the test-drive autopilot and the debug
// overlay. The components themselves are in skywalker/ecs/VehicleComponents.h.
//
// Per tick (Engine::step): the drive actions become vehicle inputs (applyVehicleControls) ->
// Wander may override them -> PhysicsWorld::sync builds/updates one VehicleConstraint per
// `vehicle` -> PhysicsWorld::step applies assists, aero and inputs, simulates, then writes
// telemetry, wheel visuals and engine audio back -> chase cameras follow (updateChaseCameras).

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/input/ActionMap.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/Image.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/scene/Scene.h"

namespace sky::physics {

class PhysicsWorld;
class PhysicsSystem;

/// Driver inputs before assists: throttle/brake/handbrake 0..1, steer -1 (left) .. 1 (right).
struct VehicleInput {
    float throttle = 0.f;
    float brake = 0.f;
    float steer = 0.f;
    float handbrake = 0.f;
};

/// One wheel as simulated this tick (world space).
struct WheelTelemetry {
    EntityId visual = kNoEntity;   // the wheel's visual entity (may be none)
    std::string name;              // "front_left", "rear_right", "axle2_left", ...
    int axle = 0;                  // 0 = front-most
    bool left = true;
    bool steers = false, driven = false, handbrake = false;
    float radius = 0.f, width = 0.f;
    Vec3 mount;                    // suspension mount (top of travel), world
    Vec3 center;                   // wheel center, world
    Vec3 down;                     // suspension direction, world
    Vec3 forward, right;           // tire basis (steered), world
    float suspensionLength = 0.f, suspensionMin = 0.f, suspensionMax = 0.f, restLength = 0.f;
    float compression = 0.f;       // 0 = fully extended .. 1 = on the bump stop
    bool contact = false;
    Vec3 contactPoint, contactNormal;
    EntityId surface = kNoEntity;  // what the tire touches
    float surfaceGrip = 1.f;       // surface friction factor applied to the tire
    float load = 0.f;              // N pressing the tire on the ground
    float longitudinalForce = 0.f; // N along the tire (+ = driving forward)
    float lateralForce = 0.f;      // N sideways
    float slipRatio = 0.f;         // |wheel surface speed - ground speed| / ground speed
    float slipAngle = 0.f;         // degrees between tire heading and travel
    float angularVelocity = 0.f;   // rad/s (+ = rolling forward)
    float steerAngle = 0.f;        // degrees (+ = left)
    float skid = 0.f;              // 0..1 slide intensity (smoke, marks, squeal)
};

/// A vehicle's live state.
struct VehicleTelemetry {
    EntityId entity = kNoEntity;
    Vec3 position, velocity, angularVelocity;  // world; angular in rad/s
    Vec3 forward, right, up;                   // chassis axes, world
    float speedKmh = 0.f;          // along the heading (negative reversing)
    float rpm = 0.f;
    int gear = 0;
    float clutch = 1.f;
    bool shifting = false;
    VehicleInput input;            // what the driver asked for
    VehicleInput applied;          // after assists (steer smoothing, auto reverse, traction control...)
    bool reversing = false, tractionControlActive = false, absActive = false, driftAssistActive = false;
    float tractionScale = 1.f;     // throttle kept by traction control
    float bodySlipAngle = 0.f;     // degrees between heading and travel (drift angle)
    float lateralG = 0.f, longitudinalG = 0.f;
    float load = 0.f;              // 0..1 engine load (audio)
    int wheelsOnGround = 0;
    float skid = 0.f;
    float mass = 0.f;
    Vec3 centerOfMass;             // world
    bool stepped = false;          // false until the first simulated tick (edit-time worlds show the rest pose)
    std::vector<WheelTelemetry> wheels;
};

/// Timing and counts for perf_stats.
struct VehicleStats {
    int vehicles = 0;
    int wheels = 0;
    double lastStepMs = 0.0;  // vehicle work (assists, aero, write-back) of the last tick, excluding the Jolt solve
};

// --- Presets (VehiclePresets.cpp) ---------------------------------------------------------------

/// "sports", "hatchback", "truck", "kart".
const std::vector<std::string>& vehiclePresetNames();
/// "arcade", "sim".
const std::vector<std::string>& vehicleHandlingNames();
/// A `vehicle` component patch for a preset and handling style (unknown names fail with did-you-mean).
Result<Json> vehiclePreset(const std::string& preset, const std::string& handling = "arcade");
/// Chase-camera settings that suit a preset.
Json chaseCameraPreset(const std::string& preset);
/// Whether `child` (somewhere under `owner`) is one of the owner vehicle's wheel visuals: a name
/// starting with "wheel" (any case) or an entity listed in `wheels`.
bool isWheelVisual(const Scene& scene, EntityId owner, EntityId child);

// --- Player controls (VehicleControls.cpp) --------------------------------------------------------

/// The drive input actions: throttle, brake, steer, handbrake, shift_up, shift_down (keyboard + gamepad).
std::vector<input::Action> driveActions();
/// Writes the drive actions (or raw keys/pads when the project has no such actions) into every
/// active vehicle with control = "player". Call once per tick after the action map was evaluated.
void applyVehicleControls(Scene& scene, const input::InputState& input);

/// Chase-camera state across ticks (reset when play stops).
class ChaseCameras {
public:
    /// Moves every active chase_camera toward its target (after physics, every tick).
    void update(Scene& scene, PhysicsWorld* world, float dt);
    void reset() { state_.clear(); }

private:
    struct State {
        Vec3 position;
        Vec3 heading;
        float fov = 0.f;
    };
    std::unordered_map<EntityId, State> state_;
};

// --- Test drives (VehicleTestDrive.cpp) -----------------------------------------------------------

struct TestDriveOptions {
    std::string maneuver = "all";          // accel | braking | slalom | skidpad | top_speed | custom | all
    std::string track = "proving_ground";  // proving_ground (flat, isolated) | scene (the level as it is)
    float speedKmh = 0.f;                  // braking start / slalom / skidpad entry speed (0 = maneuver default)
    float duration = 0.f;                  // custom / top_speed seconds (0 = default)
    float radius = 40.f;                   // skidpad radius (m)
    float coneSpacing = 18.f;              // slalom (m)
    Json inputs;                           // custom: [{t, throttle, brake, steer, handbrake}, ...] (held until the next key)
    Json overrides;                        // vehicle component patch for this drive only
    bool trace = false;                    // include a sampled trace
};
/// Simulates the vehicle in a private world (the scene is not changed) and returns metrics:
/// 0-100 km/h, braking distance, max lateral g, slalom and skidpad results, warnings.
Result<Json> runTestDrive(const PhysicsSystem& physics, const Scene& scene, EntityId vehicle, const TestDriveOptions& options);

// --- Debug overlay (VehicleDebugDraw.cpp) -----------------------------------------------------------

/// Draws suspension rays, wheels, contact points, tire forces, velocity and center of mass of
/// every vehicle in `world` over a rendered image (debug view "vehicles").
void drawVehicleOverlay(Image& image, const ViewCamera& camera, const PhysicsWorld& world);

/// JSON for vehicle_info and the Wander builtins.
Json telemetryJson(const Scene& scene, const VehicleTelemetry& t, bool wheels = true);

}  // namespace sky::physics
