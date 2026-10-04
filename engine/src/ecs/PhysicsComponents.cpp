#include "skywalker/ecs/PhysicsComponents.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<RigidBody>);
static_assert(std::is_standard_layout_v<Collider>);
static_assert(std::is_standard_layout_v<CharacterController>);
static_assert(std::is_standard_layout_v<Joint>);
static_assert(std::is_standard_layout_v<PhysicsSettings>);
static_assert(std::is_standard_layout_v<NavAgent>);
static_assert(std::is_standard_layout_v<NavMeshSurface>);

#define SKY_LAYERS "default", "static", "player", "enemy", "projectile", "trigger", "debris"
#define SKY_AXES "none", "x", "y", "z", "xy", "xz", "yz", "xyz"

const TypeInfo& RigidBody::type() {
    static const TypeInfo info{
        "body",
        "Simulated rigid body (Jolt). dynamic = moved by forces and collisions, kinematic = moved by its transform "
        "(platforms, doors) and pushes dynamic bodies, static = never moves. Needs no collider: the shape is fitted to "
        "the mesh. Child entities with a collider (and no body of their own) become parts of this body.",
        {
            SKY_FIELD_ENUM(RigidBody, motion, "How the body moves", "dynamic", "static", "kinematic"),
            SKY_FIELD_RANGE(RigidBody, mass, Float, "Mass in kg (crate 20, person 70, car 1200)", 0.001f, 1e7f),
            SKY_FIELD_RANGE(RigidBody, friction, Float, "Surface friction (ice 0.05, wood 0.5, rubber 1)", 0.f, 10.f),
            SKY_FIELD_RANGE(RigidBody, restitution, Float, "Bounciness: 0 = no bounce, 1 = perfectly elastic", 0.f, 1.f),
            SKY_FIELD_RANGE(RigidBody, linearDamping, Float, "Air drag on movement", 0.f, 100.f),
            SKY_FIELD_RANGE(RigidBody, angularDamping, Float, "Drag on spinning", 0.f, 100.f),
            SKY_FIELD_RANGE(RigidBody, gravityScale, Float, "Gravity multiplier (0 = floats, -1 = falls up)", -100.f, 100.f),
            SKY_FIELD_ENUM(RigidBody, lockPosition, "World axes along which the body cannot move (\"z\" for 2D games)", SKY_AXES),
            SKY_FIELD_ENUM(RigidBody, lockRotation, "World axes around which the body cannot rotate (\"xz\" stays upright)",
                           SKY_AXES),
            SKY_FIELD(RigidBody, ccd, Bool, "Continuous collision detection: fast objects never tunnel through walls"),
            SKY_FIELD(RigidBody, startAwake, Bool, "Simulate from the start (false = sleep until touched)"),
            SKY_FIELD(RigidBody, velocity, Vec3, "Linear velocity m/s (initial; live while playing, writable)"),
            SKY_FIELD(RigidBody, angularVelocity, Vec3, "Angular velocity in degrees/s (initial; live while playing)"),
            SKY_FIELD_ENUM(RigidBody, layer, "Collision layer (which layers collide: physics_world.ignorePairs)", SKY_LAYERS),
        }};
    return info;
}

const TypeInfo& Collider::type() {
    static const TypeInfo info{
        "collider",
        "Collision shape. Alone it makes static geometry (walls, floors); with a body it shapes the body. auto fits "
        "the mesh (box/sphere/capsule/cylinder for primitives, convex hull for dynamic models, triangle mesh for static "
        "models). mesh = exact triangles (static/kinematic only); convex = hull of the mesh; heightfield = terrain. "
        "isTrigger makes a sensor zone that fires `on trigger_enter` / `on trigger_exit` instead of blocking.",
        {
            SKY_FIELD_ENUM(Collider, shape, "Shape type", "auto", "box", "sphere", "capsule", "cylinder", "convex", "mesh",
                           "heightfield"),
            SKY_FIELD(Collider, size, Vec3, "Box: full size (local units); heightfield: x/z extent, y = max height"),
            SKY_FIELD_RANGE(Collider, radius, Float, "Sphere/capsule/cylinder radius (local units)", 0.001f, 1e5f),
            SKY_FIELD_RANGE(Collider, height, Float, "Capsule/cylinder total height along local Y", 0.001f, 1e5f),
            SKY_FIELD(Collider, offset, Vec3, "Shape center relative to the entity origin (local)"),
            SKY_FIELD(Collider, rotation, Vec3, "Shape rotation relative to the entity (Euler degrees)"),
            SKY_FIELD(Collider, isTrigger, Bool, "Sensor: detects overlaps (trigger_enter/exit) but never blocks"),
            SKY_FIELD_RANGE(Collider, friction, Float, "Material friction override (-1 = use the body's)", -1.f, 10.f),
            SKY_FIELD_RANGE(Collider, restitution, Float, "Material bounciness override (-1 = use the body's)", -1.f, 1.f),
            SKY_FIELD(Collider, heightmap, String,
                      "Heightfield source, project-relative: 16-bit .r16/.raw (square) or .hdr; empty = sample the mesh"),
            SKY_FIELD(Collider, resolution, Int, "Heightfield samples per side (8..1024)"),
        }};
    return info;
}

const TypeInfo& CharacterController::type() {
    static const TypeInfo info{
        "character",
        "Capsule character controller: walks up slopes and stairs, slides along walls, rides moving platforms, pushes "
        "dynamic bodies and is detected by triggers. Drive it from Wander every tick: walk(self, dir), jump(self), "
        "grounded(self). Units are meters (not scaled by the transform).",
        {
            SKY_FIELD_RANGE(CharacterController, height, Float, "Capsule height in m", 0.1f, 50.f),
            SKY_FIELD_RANGE(CharacterController, radius, Float, "Capsule radius in m", 0.05f, 10.f),
            SKY_FIELD(CharacterController, offset, Vec3,
                      "Capsule center relative to the entity origin in m (y = height/2 for models with the origin at the feet)"),
            SKY_FIELD_RANGE(CharacterController, maxSlope, Float, "Steepest walkable slope in degrees", 0.f, 89.f),
            SKY_FIELD_RANGE(CharacterController, stepHeight, Float, "Highest step/curb climbed automatically (m)", 0.f, 5.f),
            SKY_FIELD_RANGE(CharacterController, moveSpeed, Float, "Walking speed in m/s for walk(self, dir) with |dir| = 1", 0.f,
                            100.f),
            SKY_FIELD_RANGE(CharacterController, jumpSpeed, Float, "Upward speed of jump() in m/s", 0.f, 100.f),
            SKY_FIELD_RANGE(CharacterController, gravity, Float, "Downward acceleration in m/s^2", 0.f, 200.f),
            SKY_FIELD_RANGE(CharacterController, airControl, Float, "Steering while airborne (0 none .. 1 full)", 0.f, 1.f),
            SKY_FIELD_RANGE(CharacterController, turnSpeed, Float, "Degrees/s to turn toward the walk direction (0 = off)", 0.f,
                            100000.f),
            SKY_FIELD_RANGE(CharacterController, pushStrength, Float, "Max force in N used to push dynamic bodies", 0.f, 1e6f),
            SKY_FIELD_RANGE(CharacterController, mass, Float, "Mass in kg", 1.f, 1e5f),
            SKY_FIELD_ENUM(CharacterController, layer, "Collision layer", SKY_LAYERS),
            SKY_FIELD(CharacterController, velocity, Vec3, "Current velocity m/s (live while playing; write for knockback)"),
        }};
    return info;
}

const TypeInfo& Joint::type() {
    static const TypeInfo info{
        "joint",
        "Connects this entity's body to another body (`target`) or to the world. fixed = welded; hinge = doors, wheels, "
        "levers (axis, limits in degrees, motor); ball = chains, ragdoll shoulders; slider = pistons, drawers (axis, "
        "limits in m); distance = rope/rod (limits = min/max length); spring = bouncy link (stiffness Hz, damping). "
        "Breaks above breakForce (fires `on event \"joint_broken\"` on this entity).",
        {
            SKY_FIELD_ENUM(Joint, kind, "Joint type", "fixed", "hinge", "ball", "slider", "distance", "spring"),
            SKY_FIELD_ENTITY(Joint, target, "Other entity (name, #id or {\"id\"}); empty = attached to the world"),
            SKY_FIELD(Joint, anchor, Vec3, "Pivot point in this entity's local space"),
            SKY_FIELD(Joint, connectedAnchor, Vec3,
                      "distance/spring: the other end in the target's local space (a world point when attached to the world)"),
            SKY_FIELD(Joint, axis, Vec3, "Hinge axis / slider direction in local space"),
            SKY_FIELD(Joint, limitMin, Float, "Lower limit: degrees (hinge), m (slider, distance); used when min < max"),
            SKY_FIELD(Joint, limitMax, Float, "Upper limit"),
            SKY_FIELD(Joint, motorSpeed, Float, "Motor target speed: degrees/s (hinge) or m/s (slider)"),
            SKY_FIELD_RANGE(Joint, motorForce, Float, "Motor max torque (N m) or force (N); 0 = no motor", 0.f, 1e9f),
            SKY_FIELD_RANGE(Joint, stiffness, Float, "Spring frequency in Hz (spring joints)", 0.f, 1000.f),
            SKY_FIELD_RANGE(Joint, damping, Float, "Spring damping ratio (0 = bouncy, 1 = critically damped)", 0.f, 100.f),
            SKY_FIELD_RANGE(Joint, breakForce, Float, "Breaks when the joint force exceeds this (N); 0 = unbreakable", 0.f, 1e12f),
            SKY_FIELD(Joint, collideConnected, Bool, "Let the two connected bodies collide with each other"),
            SKY_FIELD(Joint, enabled, Bool, "Active (set to false when the joint breaks)"),
        }};
    return info;
}

const TypeInfo& PhysicsSettings::type() {
    static const TypeInfo info{
        "physics_world",
        "Scene-wide physics settings. One per scene (on any entity; the physics_settings tool manages it).",
        {
            SKY_FIELD(PhysicsSettings, gravity, Vec3, "Gravity in m/s^2 (Earth [0,-9.81,0], Moon [0,-1.62,0])"),
            SKY_FIELD(PhysicsSettings, substeps, Int, "Collision steps per tick (1..8; more = stabler stacks, fast objects)"),
            SKY_FIELD(PhysicsSettings, ignorePairs, String,
                      "Comma-separated layer pairs that never collide, e.g. \"debris-player, projectile-projectile\" "
                      "(layers: default, static, player, enemy, projectile, trigger, debris)"),
            SKY_FIELD(PhysicsSettings, allowSleep, Bool, "Let resting bodies sleep (much cheaper)"),
            SKY_FIELD(PhysicsSettings, enabled, Bool, "Run the physics simulation while playing"),
        }};
    return info;
}

const TypeInfo& NavAgent::type() {
    static const TypeInfo info{
        "nav_agent",
        "Path-finding agent on the navigation mesh with local avoidance (crowds). Wander: navigate(self, target), "
        "stop_navigation(self), arrived(self); fires `on event \"arrived\"`. With a character component it walks the "
        "controller; otherwise it moves the transform.",
        {
            SKY_FIELD_RANGE(NavAgent, speed, Float, "Max speed in m/s", 0.f, 100.f),
            SKY_FIELD_RANGE(NavAgent, acceleration, Float, "Acceleration in m/s^2", 0.01f, 1000.f),
            SKY_FIELD_RANGE(NavAgent, radius, Float, "Radius used for avoidance in m", 0.05f, 20.f),
            SKY_FIELD_RANGE(NavAgent, height, Float, "Height in m", 0.1f, 50.f),
            SKY_FIELD_RANGE(NavAgent, stoppingDistance, Float, "Arrival tolerance in m", 0.f, 100.f),
            SKY_FIELD_RANGE(NavAgent, turnSpeed, Float, "Degrees/s to face the movement (0 = never rotate)", 0.f, 100000.f),
            SKY_FIELD(NavAgent, destination, Vec3, "World point to walk to (with navigating = true)"),
            SKY_FIELD(NavAgent, navigating, Bool, "Heading to the destination (cleared on arrival)"),
            SKY_FIELD(NavAgent, autoRepath, Bool, "Re-plan when blocked or when the destination changes"),
            SKY_FIELD_ENUM(NavAgent, avoidance, "Local avoidance quality", "none", "low", "medium", "high"),
        }};
    return info;
}

const TypeInfo& NavMeshSurface::type() {
    static const TypeInfo info{
        "navmesh",
        "Navigation-mesh bake settings and the saved bake (one per scene; nav_build creates it). Walkable surfaces "
        "come from static colliders and static meshes.",
        {
            SKY_FIELD_RANGE(NavMeshSurface, agentRadius, Float, "Agent radius in m (walls are eroded by it)", 0.f, 20.f),
            SKY_FIELD_RANGE(NavMeshSurface, agentHeight, Float, "Agent height in m (lower ceilings block)", 0.1f, 50.f),
            SKY_FIELD_RANGE(NavMeshSurface, maxClimb, Float, "Highest step in m", 0.f, 20.f),
            SKY_FIELD_RANGE(NavMeshSurface, maxSlope, Float, "Steepest walkable slope in degrees", 0.f, 89.f),
            SKY_FIELD_RANGE(NavMeshSurface, cellSize, Float, "Voxel size in m (radius/2..radius/3 is typical)", 0.01f, 5.f),
            SKY_FIELD_RANGE(NavMeshSurface, cellHeight, Float, "Voxel height in m", 0.01f, 5.f),
            SKY_FIELD(NavMeshSurface, tileSize, Int, "Cells per tile side (16..256)"),
            SKY_FIELD_ENUM(NavMeshSurface, geometry,
                           "Input: both (colliders where present, else meshes), colliders only, or render meshes only",
                           "both", "colliders", "meshes"),
            SKY_FIELD(NavMeshSurface, data, String, "Baked navmesh file (project-relative), written by nav_build"),
            SKY_FIELD(NavMeshSurface, autoBuild, Bool, "Bake when play starts if the saved bake is missing or stale"),
        }};
    return info;
}

#undef SKY_LAYERS
#undef SKY_AXES

}  // namespace sky
