#include "skywalker/ecs/VehicleComponents.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<Vehicle>);
static_assert(std::is_standard_layout_v<ChaseCamera>);

const TypeInfo& Vehicle::type() {
    static const char* kWheelsSchema = R"JSON({"type": "array", "items": {"type": "object", "properties": {
        "entity": {"type": "string", "description": "child entity that shows this wheel (spins, steers, follows the suspension)"},
        "position": {"type": "array", "items": {"type": "number"}, "description": "wheel center at rest, chassis-local m"},
        "radius": {"type": "number"}, "width": {"type": "number"},
        "steer": {"type": "boolean"}, "drive": {"type": "boolean"}, "handbrake": {"type": "boolean"},
        "maxSteerAngle": {"type": "number"}, "brakeTorque": {"type": "number"}, "handbrakeTorque": {"type": "number"},
        "suspensionMinLength": {"type": "number"}, "suspensionMaxLength": {"type": "number"},
        "suspensionFrequency": {"type": "number"}, "suspensionDamping": {"type": "number"},
        "longitudinalGrip": {"type": "number"}, "lateralGrip": {"type": "number"}}}})JSON";
    static const char* kCurveSchema =
        R"({"type": "array", "items": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}})";
    static const char* kRatiosSchema = R"({"type": "array", "items": {"type": "number", "exclusiveMinimum": 0}})";
    static const TypeInfo info{
        "vehicle",
        "Drivable wheeled vehicle (Jolt vehicle constraint): suspension, tire friction curves, engine torque curve, "
        "auto/manual gearbox, FWD/RWD/AWD limited-slip differentials, anti-roll bars, downforce and drag. Needs a dynamic "
        "`body` on the same entity (the chassis). Child entities named wheel* are the wheels (fitted automatically when "
        "`wheels` is empty) and spin/steer/bounce while playing. Drive it with the inputs (throttle, brake, steer, "
        "handbrake): control=player reads the drive input actions, or set them from Wander (vehicle_drive(self, ...)). "
        "Build one with vehicle_create, tune by numbers with vehicle_tune + vehicle_test_drive, watch it with vehicle_info.",
        {
            SKY_FIELD(Vehicle, preset, String, "Preset it was built from (informational): sports, hatchback, truck, kart, custom"),
            SKY_FIELD_RANGE(Vehicle, mass, Float, "Chassis mass in kg (kart 160, hatchback 1150, sports 1350, truck 2600)", 1.f,
                            1e6f),
            SKY_FIELD(Vehicle, centerOfMass, Vec3, "Center-of-mass offset in m (local); lower y = less body roll, harder to flip"),
            SKY_FIELD_RANGE(Vehicle, maxTilt, Float, "Max pitch/roll in degrees before the chassis is held upright (180 = can flip)",
                            5.f, 180.f),
            SKY_FIELD_JSON(Vehicle, wheels,
                           "Per-wheel setup in chassis space; empty = fitted at play from children named wheel*. Any field "
                           "overrides the vehicle default for that wheel",
                           kWheelsSchema),
            SKY_FIELD_RANGE(Vehicle, wheelRadius, Float, "Wheel radius in m (0 = measured from the wheel meshes)", 0.f, 10.f),
            SKY_FIELD_RANGE(Vehicle, wheelWidth, Float, "Wheel width in m (0 = measured)", 0.f, 10.f),
            SKY_FIELD_RANGE(Vehicle, suspensionMinLength, Float, "Suspension length fully compressed (m below the mount)", 0.f, 5.f),
            SKY_FIELD_RANGE(Vehicle, suspensionMaxLength, Float, "Suspension length fully extended (m below the mount)", 0.01f,
                            5.f),
            SKY_FIELD_RANGE(Vehicle, suspensionFrequency, Float, "Spring frequency in Hz (1 soft, 1.6 road, 2.5+ race)", 0.1f,
                            20.f),
            SKY_FIELD_RANGE(Vehicle, suspensionDamping, Float, "Damping ratio (0.3 floaty, 0.5 road, 1 stiff)", 0.f, 5.f),
            SKY_FIELD_ENUM(Vehicle, steering, "Which wheels steer", "front", "rear", "all", "none"),
            SKY_FIELD_RANGE(Vehicle, maxSteerAngle, Float, "Steering lock in degrees", 0.f, 89.f),
            SKY_FIELD_RANGE(Vehicle, brakeTorque, Float, "Brake torque per wheel in N m at full brake", 0.f, 1e6f),
            SKY_FIELD_RANGE(Vehicle, handbrakeTorque, Float, "Handbrake torque per rear wheel in N m", 0.f, 1e6f),
            SKY_FIELD_RANGE(Vehicle, longitudinalGrip, Float, "Peak tire friction accelerating/braking (1 street, 1.6 slicks)",
                            0.f, 10.f),
            SKY_FIELD_RANGE(Vehicle, lateralGrip, Float, "Peak tire friction cornering (lower = slides sooner)", 0.f, 10.f),
            SKY_FIELD_JSON(Vehicle, longitudinalCurve,
                           "Tire friction vs slip ratio [[slip, 0..1], ...] (scaled by longitudinalGrip); empty = default",
                           kCurveSchema),
            SKY_FIELD_JSON(Vehicle, lateralCurve,
                           "Tire friction vs slip angle [[degrees, 0..1], ...] (scaled by lateralGrip); empty = default",
                           kCurveSchema),
            SKY_FIELD_ENUM(Vehicle, drive, "Driven wheels", "fwd", "rwd", "awd"),
            SKY_FIELD_RANGE(Vehicle, frontTorqueSplit, Float, "AWD: share of the torque sent to the front axle", 0.f, 1.f),
            SKY_FIELD_RANGE(Vehicle, limitedSlip, Float, "Limited-slip ratio of the differentials (1.4 tight, >= 10 open)", 1.01f,
                            1e6f),
            SKY_FIELD_RANGE(Vehicle, differentialRatio, Float, "Final drive ratio", 0.1f, 50.f),
            SKY_FIELD_RANGE(Vehicle, maxTorque, Float, "Peak engine torque in N m", 0.f, 1e6f),
            SKY_FIELD_RANGE(Vehicle, minRpm, Float, "Idle rpm", 0.f, 30000.f),
            SKY_FIELD_RANGE(Vehicle, maxRpm, Float, "Redline rpm", 100.f, 30000.f),
            SKY_FIELD_RANGE(Vehicle, engineInertia, Float, "Engine inertia in kg m^2 (lower revs faster)", 0.01f, 100.f),
            SKY_FIELD_RANGE(Vehicle, engineDamping, Float, "Engine braking / internal friction", 0.f, 10.f),
            SKY_FIELD_JSON(Vehicle, torqueCurve, "Torque vs rpm [[rpm fraction, torque fraction], ...]; empty = default",
                           kCurveSchema),
            SKY_FIELD_ENUM(Vehicle, transmission, "Gearbox: auto shifts by rpm; manual uses vehicle_shift / shift actions", "auto",
                           "manual"),
            SKY_FIELD_JSON(Vehicle, gearRatios, "Forward gear ratios, first gear first; empty = [2.66, 1.78, 1.3, 1.0, 0.74]",
                           kRatiosSchema),
            SKY_FIELD_RANGE(Vehicle, reverseRatio, Float, "Reverse gear ratio (magnitude)", 0.1f, 50.f),
            SKY_FIELD_RANGE(Vehicle, shiftUpRpm, Float, "Auto gearbox shifts up above this rpm", 100.f, 30000.f),
            SKY_FIELD_RANGE(Vehicle, shiftDownRpm, Float, "Auto gearbox shifts down below this rpm", 50.f, 30000.f),
            SKY_FIELD_RANGE(Vehicle, shiftTime, Float, "Seconds without drive while shifting", 0.f, 5.f),
            SKY_FIELD_RANGE(Vehicle, clutchStrength, Float, "Clutch coupling strength", 0.1f, 1000.f),
            SKY_FIELD_RANGE(Vehicle, antiRollFront, Float, "Front anti-roll bar stiffness in N/m (0 = none)", 0.f, 1e7f),
            SKY_FIELD_RANGE(Vehicle, antiRollRear, Float, "Rear anti-roll bar stiffness in N/m (stiffer rear = more oversteer)",
                            0.f, 1e7f),
            SKY_FIELD_RANGE(Vehicle, downforce, Float, "Aero downforce in N per (m/s)^2 (0.4 road car, 2+ race car)", 0.f, 1000.f),
            SKY_FIELD_RANGE(Vehicle, drag, Float, "Aero drag in N per (m/s)^2 (limits top speed; ~0.5 * rho * Cd * area)", 0.f,
                            1000.f),
            SKY_FIELD_ENUM(Vehicle, control, "Who sets the inputs: player = drive input actions, script = Wander/tools, none",
                           "player", "script", "none"),
            SKY_FIELD_RANGE(Vehicle, steerSpeed, Float, "Steering response: full lock per second (0 = instant)", 0.f, 100.f),
            SKY_FIELD_RANGE(Vehicle, speedSensitiveSteering, Float, "Less steering lock at speed, 0..1", 0.f, 1.f),
            SKY_FIELD(Vehicle, tractionControl, Bool, "Traction control: limits wheelspin under throttle"),
            SKY_FIELD(Vehicle, abs, Bool, "Anti-lock brakes: keeps the wheels rolling (and steering) under hard braking"),
            SKY_FIELD_RANGE(Vehicle, driftAssist, Float, "Arcade drift help 0..1: easy slides that hold their angle", 0.f, 1.f),
            SKY_FIELD(Vehicle, autoReverse, Bool, "Holding brake at a standstill reverses"),
            SKY_FIELD(Vehicle, engineAudio, Bool, "Drive the entity's audio component: pitch from rpm, volume from load"),
            SKY_FIELD_RANGE(Vehicle, throttle, Float, "Input: accelerator 0..1 (live)", 0.f, 1.f),
            SKY_FIELD_RANGE(Vehicle, brake, Float, "Input: brake pedal 0..1 (live)", 0.f, 1.f),
            SKY_FIELD_RANGE(Vehicle, steer, Float, "Input: steering -1 left .. 1 right (live)", -1.f, 1.f),
            SKY_FIELD_RANGE(Vehicle, handbrake, Float, "Input: handbrake 0..1 (live)", 0.f, 1.f),
            SKY_FIELD_RANGE(Vehicle, gear, Int, "Current gear (-1 reverse, 0 neutral); manual gearbox: write to shift", -10.f,
                            20.f),
            SKY_FIELD(Vehicle, speed, Float, "Telemetry: km/h along the heading (negative reversing)"),
            SKY_FIELD(Vehicle, rpm, Float, "Telemetry: engine rpm"),
            SKY_FIELD(Vehicle, wheelsOnGround, Int, "Telemetry: wheels touching the ground"),
            SKY_FIELD(Vehicle, skid, Float, "Telemetry: strongest tire slide 0..1 (tire smoke, skid marks, squeal)"),
        }};
    return info;
}

const TypeInfo& ChaseCamera::type() {
    static const TypeInfo info{
        "chase_camera",
        "Spring-arm chase camera: follows `target` from behind with look-ahead into corners, collision pull-in and a field "
        "of view that widens with speed. Put it on an entity with a camera (vehicle_create chase_camera=true sets it up). "
        "Moves every tick while playing.",
        {
            SKY_FIELD_ENTITY(ChaseCamera, target, "Entity to follow (the vehicle)"),
            SKY_FIELD_RANGE(ChaseCamera, distance, Float, "Distance behind the target in m", 0.f, 1000.f),
            SKY_FIELD_RANGE(ChaseCamera, height, Float, "Height above the target in m", -100.f, 1000.f),
            SKY_FIELD_RANGE(ChaseCamera, targetHeight, Float, "Aim point above the target origin in m", -100.f, 100.f),
            SKY_FIELD_RANGE(ChaseCamera, lookAhead, Float, "Seconds of velocity to aim ahead", 0.f, 5.f),
            SKY_FIELD_RANGE(ChaseCamera, stiffness, Float, "Position spring in 1/s (higher = tighter)", 0.1f, 100.f),
            SKY_FIELD_RANGE(ChaseCamera, turnStiffness, Float, "How fast the arm swings behind the target in 1/s", 0.1f, 100.f),
            SKY_FIELD_RANGE(ChaseCamera, fovMin, Float, "Field of view at a standstill (degrees)", 5.f, 170.f),
            SKY_FIELD_RANGE(ChaseCamera, fovMax, Float, "Field of view at fovSpeed (degrees)", 5.f, 170.f),
            SKY_FIELD_RANGE(ChaseCamera, fovSpeed, Float, "Speed in m/s where the field of view reaches fovMax", 0.1f, 1000.f),
            SKY_FIELD(ChaseCamera, collide, Bool, "Pull the camera in when a wall blocks the view"),
        }};
    return info;
}

}  // namespace sky
