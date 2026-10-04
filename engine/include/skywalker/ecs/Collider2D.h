#pragma once
// `collider2d`: a 2D collision shape on the XY plane (Box2D). Alone it is static geometry; with a
// `body2d` (on this entity or an ancestor) it shapes that body. `tilemap` builds merged collision from
// the entity's tilemap. Included at the end of Components.h; table in physics2d/Physics2DComponents.cpp.

#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct Collider2D {
    std::string shape = "box";      // box | circle | capsule | polygon | chain | segment | tilemap
    Vec2 size{1.f, 1.f};            // box: full width and height (local units, scaled by the transform)
    float radius = 0.5f;            // circle / capsule
    float height = 1.f;             // capsule: total height along local Y (>= 2 * radius)
    Vec2 offset{0.f, 0.f};          // shape center relative to the entity origin (local)
    float rotation = 0.f;           // degrees around Z relative to the entity
    Json points = Json::array();    // polygon (convex, <= 8), chain or segment points, local: [[x, y], ...]
    bool loop = false;              // chain: closed (collides on the outside when counter-clockwise)
    float friction = 0.6f;
    float restitution = 0.f;        // bounciness 0..1
    float density = 1.f;            // kg per square unit (mass of dynamic bodies)
    bool sensor = false;            // detects overlaps (on trigger_enter / trigger_exit), never blocks
    std::string layer = "default";  // default | static | player | enemy | projectile | trigger | debris
    std::string mask = "all";       // layers it collides with: "all" or names ("default, player")
    bool oneWay = false;            // platform: blocks only from above (local +Y), passable from below and the sides
    std::string tileMerge = "chains";  // tilemap: chains (outlines, no ghost bumps) | boxes (merged rectangles)
    Json tileShapes = Json::object();  // tilemap: per-tile shapes over the tileset's ({"7": "slope_up", "9": "top"})

    static const TypeInfo& type();
};

}  // namespace sky
