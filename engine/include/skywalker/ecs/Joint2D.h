#pragma once
// `joint2d`: connects this entity's 2D body to another one (`other`, an entity link) or to the world.
// Included at the end of Components.h; table in physics2d/Physics2DComponents.cpp.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct Joint2D {
    std::string kind = "revolute";  // revolute | prismatic | distance | weld | wheel | target
    EntityLink other;               // the other body (empty = the world)
    Vec2 anchor{0.f, 0.f};          // pivot in this entity's local space
    Vec2 otherAnchor{0.f, 0.f};     // the other end, in the other body's local space (or a world point)
    Vec2 axis{1.f, 0.f};            // prismatic / wheel: sliding (suspension) axis, local to this entity
    float length = 0.f;             // distance: rest length (0 = the distance when play starts)
    float limitMin = 0.f;           // degrees (revolute) or units (prismatic, wheel, distance); active when min < max
    float limitMax = 0.f;
    float motorSpeed = 0.f;         // degrees/s (revolute, wheel) or units/s (prismatic, distance)
    float motorForce = 0.f;         // max torque (N m) or force (N); 0 = motor off
    float stiffness = 0.f;          // spring frequency in Hz (distance, prismatic, revolute, wheel, weld, target); 0 = rigid
    float damping = 0.7f;           // spring damping ratio
    Vec2 target{0.f, 0.f};          // target: world point the body is pulled to (when `other` is empty)
    float maxForce = 1000.f;        // target: strongest pull (N)
    float breakForce = 0.f;         // N: the joint breaks above this (0 = unbreakable)
    bool collideConnected = false;
    bool enabled = true;            // false after breaking (`on event "joint_broken"`)

    static const TypeInfo& type();
};

}  // namespace sky
