#pragma once
// Physics and navigation components (Jolt Physics + Recast/Detour). Plain reflected data like
// every other component: JSON I/O, agent schemas, the editor property grid and Wander access
// (`self.body.velocity`, `self.collider.isTrigger`, ...) all come from the tables in
// PhysicsComponents.cpp. The simulation lives in skywalker/physics and skywalker/nav.
//
// The component model:
//   * `collider` alone            -> static geometry (walls, floors, level meshes),
//   * `body` (+ optional collider) -> a simulated rigid body; colliders on child entities
//                                     without their own body become parts of its compound shape,
//   * `character`                 -> a capsule character controller (slopes, steps, platforms),
//   * `joint`                     -> connects this entity's body to another body (or the world),
//   * `physics_world`             -> scene-wide settings (gravity, substeps, layer matrix),
//   * `nav_agent` / `navmesh`     -> path-finding agents and the navigation-mesh bake settings.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// A simulated rigid body. Without a `collider` the shape is fitted to the mesh ("auto").
struct RigidBody {
    std::string motion = "dynamic";   // dynamic | static | kinematic
    float mass = 1.f;                 // kg
    float friction = 0.5f;
    float restitution = 0.f;          // bounciness 0..1
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float gravityScale = 1.f;
    std::string lockPosition = "none";  // none | x | y | z | xy | xz | yz | xyz (world axes that cannot move)
    std::string lockRotation = "none";  // axes the body cannot rotate around ("xz" keeps it upright)
    bool ccd = false;                 // continuous collision detection (fast projectiles)
    bool startAwake = true;
    Vec3 velocity{0.f};               // m/s: initial value; live (read/write) while playing
    Vec3 angularVelocity{0.f};        // degrees/s: initial value; live while playing
    std::string layer = "default";    // default | static | player | enemy | projectile | trigger | debris

    static const TypeInfo& type();
};

/// Collision shape. Dimensions are in the entity's local space (scaled by its transform).
struct Collider {
    std::string shape = "auto";  // auto | box | sphere | capsule | cylinder | convex | mesh | heightfield
    Vec3 size{1.f, 1.f, 1.f};    // box: full size; heightfield: x/z extent and y = max height
    float radius = 0.5f;         // sphere / capsule / cylinder
    float height = 1.f;          // capsule / cylinder total height (along local Y)
    Vec3 offset{0.f};            // shape center relative to the entity origin
    Vec3 rotation{0.f};          // shape rotation (Euler degrees)
    bool isTrigger = false;      // sensor: reports enter/exit, never blocks
    float friction = -1.f;       // < 0: use the body's (or 0.5 for static colliders)
    float restitution = -1.f;    // < 0: use the body's (or 0)
    std::string heightmap;       // heightfield source: .r16/.raw (16-bit) or .hdr; empty = sample the mesh
    int resolution = 64;         // heightfield samples per side

    static const TypeInfo& type();
};

/// Capsule character controller (Jolt CharacterVirtual): walks up slopes and steps, rides
/// moving platforms, pushes dynamic bodies. Drive it from Wander with walk(), jump(), grounded().
struct CharacterController {
    float height = 1.8f;         // m, total capsule height
    float radius = 0.35f;        // m
    Vec3 offset{0.f};            // capsule center relative to the entity origin (m); y = height/2 for feet-origin models
    float maxSlope = 45.f;       // degrees: steeper ground is a wall
    float stepHeight = 0.35f;    // m: stairs/curbs climbed automatically
    float moveSpeed = 4.f;       // m/s at walk(dir) with |dir| = 1
    float jumpSpeed = 6.f;       // m/s upward on jump()
    float gravity = 20.f;        // m/s^2 downward (games feel better above the real 9.81)
    float airControl = 0.3f;     // 0 = no steering in the air, 1 = full
    float turnSpeed = 720.f;     // degrees/s to face the walk direction (0 = never rotate)
    float pushStrength = 200.f;  // N: how hard it pushes dynamic bodies
    float mass = 70.f;           // kg
    std::string layer = "player";
    Vec3 velocity{0.f};          // live while playing (read/write: knockback, launch pads)

    static const TypeInfo& type();
};

/// Connects this entity's body to `target`'s body (or to the world).
struct Joint {
    std::string kind = "fixed";  // fixed | hinge | ball | slider | distance | spring
    EntityLink target;           // the other body; empty = the world
    Vec3 anchor{0.f};            // pivot in this entity's local space
    Vec3 connectedAnchor{0.f};   // distance/spring: the other end, in the target's local space (or world point)
    Vec3 axis{0.f, 1.f, 0.f};    // hinge axis / slider direction, local space
    float limitMin = 0.f;        // degrees (hinge) or meters (slider, distance); active when min < max
    float limitMax = 0.f;
    float motorSpeed = 0.f;      // degrees/s (hinge) or m/s (slider)
    float motorForce = 0.f;      // max torque (N m) / force (N); 0 = motor off
    float stiffness = 2.f;       // spring frequency (Hz) for `spring`
    float damping = 0.5f;        // spring damping ratio
    float breakForce = 0.f;      // N: the joint breaks above this (0 = unbreakable)
    bool collideConnected = false;
    bool enabled = true;         // false after breaking (`on event "joint_broken"`)

    static const TypeInfo& type();
};

/// Scene-wide physics settings. Put it on one entity (physics_settings creates "Physics");
/// without one the defaults below apply.
struct PhysicsSettings {
    Vec3 gravity{0.f, -9.81f, 0.f};
    int substeps = 1;                                // collision steps per 1/60 s tick (stacks, fast objects)
    std::string ignorePairs = "debris-player, debris-enemy";  // layer pairs that never collide
    bool allowSleep = true;
    bool enabled = true;

    static const TypeInfo& type();
};

/// A path-finding agent on the navigation mesh (DetourCrowd steering with local avoidance).
/// With a `character` it drives the controller; otherwise it moves the transform directly.
struct NavAgent {
    float speed = 3.5f;             // m/s
    float acceleration = 8.f;       // m/s^2
    float radius = 0.4f;            // m (avoidance)
    float height = 1.8f;            // m
    float stoppingDistance = 0.3f;  // m: arrival tolerance
    float turnSpeed = 540.f;        // degrees/s to face the movement (0 = never rotate)
    Vec3 destination{0.f};
    bool navigating = false;        // true while heading to `destination` (set by navigate())
    bool autoRepath = true;         // re-plan when the path is blocked or the destination moves
    std::string avoidance = "medium";  // none | low | medium | high

    static const TypeInfo& type();
};

/// Navigation-mesh bake settings and the saved bake. One per scene.
struct NavMeshSurface {
    float agentRadius = 0.4f;   // m: walls are eroded by this
    float agentHeight = 1.8f;   // m: minimum ceiling height
    float maxClimb = 0.4f;      // m: highest step
    float maxSlope = 45.f;      // degrees
    float cellSize = 0.2f;      // m: horizontal voxel size (smaller = more precise, slower)
    float cellHeight = 0.1f;    // m: vertical voxel size
    int tileSize = 64;          // cells per tile side (large worlds build tile by tile)
    std::string geometry = "both";  // both | colliders | meshes: what blocks/supports walking
    std::string data;           // baked navmesh file, project-relative (written by nav_build)
    bool autoBuild = true;      // bake at play when `data` is missing or stale

    static const TypeInfo& type();
};

}  // namespace sky
