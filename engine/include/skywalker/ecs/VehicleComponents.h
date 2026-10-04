#pragma once
// Vehicle components: a wheeled vehicle simulated by Jolt's VehicleConstraint (suspension, tires,
// engine, gearbox, differentials, anti-roll bars) plus a chase camera that follows it. Plain
// reflected data like every component (JSON, agent schemas, the editor grid, Wander access:
// `self.vehicle.throttle = 1`). The simulation lives in skywalker/physics (Vehicles.cpp).
//
// Setup: an entity with a dynamic `body` (the chassis: its mesh or colliders give the collision
// shape) and a `vehicle`. Child entities named `wheel*` are the wheel visuals: positions, radius
// and width are fitted from them when `wheels` is empty, and they follow suspension, steering and
// rotation while playing. Axes: -Z is forward, +X right, +Y up (the engine convention).

#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// A drivable wheeled vehicle (car, truck, kart). Inputs are live fields (throttle, brake, steer,
/// handbrake); telemetry (speed, rpm, gear, wheelsOnGround, skid) is written back every tick.
struct Vehicle {
    std::string preset = "sports";  // what vehicle_create built it from (informational): sports | hatchback | truck | kart | custom

    // --- Chassis ---------------------------------------------------------------------------
    float mass = 1300.f;                    // kg (replaces body.mass)
    Vec3 centerOfMass{0.f, -0.25f, 0.f};    // offset from the shape's center of mass (m, local); lower = harder to roll
    float maxTilt = 70.f;                   // degrees the chassis may pitch/roll before it is held (180 = may flip)

    // --- Wheels (defaults; each `wheels` entry may override) ----------------------------------
    Json wheels = Json::array();      // [{entity, position, radius, width, steer, drive, handbrake, ...}]; empty = fit children named wheel*
    float wheelRadius = 0.f;          // m; 0 = measured from the wheel meshes
    float wheelWidth = 0.f;           // m; 0 = measured
    float suspensionMinLength = 0.1f; // m: fully compressed (bump stop) below the mount point
    float suspensionMaxLength = 0.35f;// m: fully extended (droop)
    float suspensionFrequency = 1.6f; // Hz: spring stiffness (1 soft .. 3 race)
    float suspensionDamping = 0.5f;   // damping ratio (0.3 floaty .. 1 stiff)
    std::string steering = "front";   // which wheels steer: front | rear | all | none
    float maxSteerAngle = 32.f;       // degrees at full lock
    float brakeTorque = 2500.f;       // N m per wheel at full brake
    float handbrakeTorque = 5000.f;   // N m per rear wheel at full handbrake
    float longitudinalGrip = 1.2f;    // peak tire friction accelerating/braking (scales longitudinalCurve)
    float lateralGrip = 1.1f;         // peak tire friction cornering (scales lateralCurve)
    Json longitudinalCurve = Json::array();  // [[slip ratio, friction 0..1], ...]; empty = default curve
    Json lateralCurve = Json::array();       // [[slip angle in degrees, friction 0..1], ...]; empty = default curve

    // --- Drivetrain ------------------------------------------------------------------------------
    std::string drive = "rwd";        // fwd | rwd | awd
    float frontTorqueSplit = 0.4f;    // awd: share of torque to the front axle
    float limitedSlip = 1.4f;         // max/min wheel speed ratio of the differentials (>= 10 = open differential)
    float differentialRatio = 3.42f;  // final drive

    // --- Engine ----------------------------------------------------------------------------------
    float maxTorque = 450.f;          // N m at the peak of torqueCurve
    float minRpm = 1000.f;            // idle
    float maxRpm = 7000.f;            // redline
    float engineInertia = 0.5f;       // kg m^2 (lower revs faster)
    float engineDamping = 0.2f;       // engine braking
    Json torqueCurve = Json::array(); // [[rpm fraction 0..1, torque fraction 0..1], ...]; empty = default curve

    // --- Transmission ------------------------------------------------------------------------------
    std::string transmission = "auto";  // auto | manual (gear from vehicle_shift / the shift actions)
    Json gearRatios = Json::array();    // forward gears, e.g. [2.66, 1.78, 1.3, 1.0, 0.74]; empty = that 5-speed
    float reverseRatio = 2.9f;          // reverse gear ratio (magnitude)
    float shiftUpRpm = 6000.f;
    float shiftDownRpm = 2800.f;
    float shiftTime = 0.25f;            // s without drive while shifting (auto)
    float clutchStrength = 10.f;        // how hard the clutch couples engine and wheels

    // --- Anti-roll and aero --------------------------------------------------------------------------
    float antiRollFront = 600.f;      // anti-roll bar stiffness (0 = none, 300 soft .. 2000 very stiff)
    float antiRollRear = 400.f;
    float downforce = 0.4f;           // N per (m/s)^2 pressing the car down (grip at speed)
    float drag = 0.38f;               // N per (m/s)^2 against the motion (top speed)

    // --- Control and assists -----------------------------------------------------------------------
    std::string control = "player";  // player = the drive input actions each tick | script = Wander/tools set the inputs | none
    float steerSpeed = 4.f;          // how fast steering follows the input (full lock per second; 0 = instant)
    float speedSensitiveSteering = 0.5f;  // 0..1: less lock at speed (stable at 200 km/h, nimble when parking)
    bool tractionControl = true;     // limits wheelspin
    bool abs = true;                 // anti-lock brakes (steer while braking)
    float driftAssist = 0.f;         // 0..1 arcade drift: easier slides that hold their angle (karts 0.6)
    bool autoReverse = true;         // holding brake at a standstill drives backward
    bool engineAudio = true;         // drive the entity's `audio` component: pitch from rpm, volume from load

    // --- Inputs (live; written by the player actions, Wander or vehicle_* tools) --------------------
    float throttle = 0.f;   // 0..1
    float brake = 0.f;      // 0..1
    float steer = 0.f;      // -1 (left) .. 1 (right)
    float handbrake = 0.f;  // 0..1

    // --- Telemetry (live while playing) ------------------------------------------------------------
    int gear = 0;            // current gear (-1 reverse, 0 neutral, 1..); manual: write to request a gear
    float speed = 0.f;       // km/h along the heading (negative = reversing)
    float rpm = 0.f;
    int wheelsOnGround = 0;
    float skid = 0.f;        // 0..1 strongest tire slide (tire smoke, skid marks, squeal)

    static const TypeInfo& type();
};

/// A spring-arm chase camera: follows `target` from behind with look-ahead and a speed-based
/// field of view. Put it on an entity with a `camera`; it moves every tick while playing.
struct ChaseCamera {
    EntityLink target;           // the vehicle (or any entity) to follow
    float distance = 6.f;        // m behind the target
    float height = 2.f;          // m above the target
    float targetHeight = 0.8f;   // m above the target origin to aim at
    float lookAhead = 0.3f;      // s of velocity to aim ahead (into corners)
    float stiffness = 8.f;       // position spring (1/s): higher follows tighter
    float turnStiffness = 4.f;   // how fast the arm swings behind a turning target (1/s)
    float fovMin = 60.f;         // degrees at a standstill
    float fovMax = 76.f;         // degrees at fovSpeed
    float fovSpeed = 50.f;       // m/s where the field of view reaches fovMax
    bool collide = true;         // pull in when a wall is between the target and the camera

    static const TypeInfo& type();
};

}  // namespace sky
