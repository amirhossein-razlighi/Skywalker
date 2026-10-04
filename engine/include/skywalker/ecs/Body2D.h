#pragma once
// `body2d`: a simulated 2D rigid body (Box2D) on the XY plane. Included at the end of Components.h;
// table in physics2d/Physics2DComponents.cpp. The body moves the entity's transform: position x/y and
// the rotation around Z (z, the other rotations and scale are kept). See docs/PHYSICS.md "2D physics".
//
// The 2D component model mirrors the 3D one:
//   * `collider2d` alone             -> static geometry (ground, walls, tilemap collision),
//   * `body2d` (+ collider2d)        -> a simulated body; collider2d on child entities without their own
//                                       body2d become extra shapes of it,
//   * `character2d`                  -> a kinematic platformer controller (slopes, one-way platforms,
//                                       coyote time, jump buffering),
//   * `joint2d`                      -> connects this body to another one (or to the world),
//   * `physics2d_world`              -> scene-wide settings (gravity, sub-steps, debug draw).

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct Body2D {
    std::string motion = "dynamic";  // dynamic | kinematic | static
    float mass = 0.f;                // kg; 0 = from the colliders' density and area
    float gravityScale = 1.f;
    float linearDamping = 0.f;
    float angularDamping = 0.05f;
    bool fixedRotation = false;      // never rotates (characters driven by forces, crates that must stay upright)
    bool bullet = false;             // continuous collision against other moving bodies (fast projectiles)
    bool allowSleep = true;
    bool startAwake = true;
    Vec2 velocity{0.f, 0.f};         // units/s: initial value; live (read/write) while playing
    float angularVelocity = 0.f;     // degrees/s around Z: initial value; live while playing
    bool sleeping = false;           // live while playing (read-only)

    static const TypeInfo& type();
};

}  // namespace sky
