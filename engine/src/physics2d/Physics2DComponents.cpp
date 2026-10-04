// Reflection tables of the 2D physics components (ecs/Body2D.h, Collider2D.h, Joint2D.h,
// Character2D.h, Physics2DSettings.h): JSON I/O, agent schemas, the editor property grid and
// Wander access (`self.body2d.velocity`, `self.character2d.grounded`) come from these.

#include <type_traits>

#include "skywalker/ecs/Components.h"

namespace sky {

static_assert(std::is_standard_layout_v<Body2D>);
static_assert(std::is_standard_layout_v<Collider2D>);
static_assert(std::is_standard_layout_v<Joint2D>);
static_assert(std::is_standard_layout_v<Character2D>);
static_assert(std::is_standard_layout_v<Physics2DSettings>);

#define SKY_LAYERS_2D "default", "static", "player", "enemy", "projectile", "trigger", "debris"

const TypeInfo& Body2D::type() {
    static const TypeInfo info{
        "body2d",
        "Simulated 2D rigid body (Box2D) on the XY plane: dynamic = moved by gravity, forces and collisions; kinematic = "
        "moved by its transform or velocity (moving platforms, doors) and pushes dynamic bodies; static = never moves. "
        "Shape it with a collider2d on the entity or its children. It writes position x/y and the Z rotation.",
        {
            SKY_FIELD_ENUM(Body2D, motion, "How the body moves", "dynamic", "kinematic", "static"),
            SKY_FIELD_RANGE(Body2D, mass, Float, "Mass in kg (0 = from the colliders' density x area)", 0.f, 1e7f),
            SKY_FIELD_RANGE(Body2D, gravityScale, Float, "Gravity multiplier (0 = floats, -1 = falls up)", -100.f, 100.f),
            SKY_FIELD_RANGE(Body2D, linearDamping, Float, "Drag on movement", 0.f, 100.f),
            SKY_FIELD_RANGE(Body2D, angularDamping, Float, "Drag on spinning", 0.f, 100.f),
            SKY_FIELD(Body2D, fixedRotation, Bool, "Never rotates (upright crates, force-driven characters)"),
            SKY_FIELD(Body2D, bullet, Bool, "Continuous collision against other moving bodies: fast projectiles never tunnel"),
            SKY_FIELD(Body2D, allowSleep, Bool, "May sleep when at rest (saves time; woken by contacts)"),
            SKY_FIELD(Body2D, startAwake, Bool, "Simulate from the start (false = sleep until touched)"),
            SKY_FIELD(Body2D, velocity, Vec2, "Linear velocity in units/s (initial; live while playing, writable)"),
            SKY_FIELD(Body2D, angularVelocity, Float, "Angular velocity in degrees/s around Z (initial; live while playing)"),
            SKY_FIELD(Body2D, sleeping, Bool, "Whether the body sleeps (live while playing, read-only)"),
        }};
    return info;
}

const TypeInfo& Collider2D::type() {
    static const TypeInfo info{
        "collider2d",
        "2D collision shape on the XY plane. Alone it is static geometry (ground, walls); with a body2d it shapes the "
        "body; on a child without its own body2d it adds a shape to the ancestor's body. sensor makes a zone that fires "
        "`on trigger_enter` / `on trigger_exit`. oneWay makes a platform you can jump through from below. shape "
        "tilemap builds merged collision from the entity's tilemap (solid layers, per-tile slopes and one-way tiles).",
        {
            SKY_FIELD_ENUM(Collider2D, shape, "Shape type", "box", "circle", "capsule", "polygon", "chain", "segment", "tilemap"),
            SKY_FIELD(Collider2D, size, Vec2, "Box: full width and height (local units, scaled by the transform)"),
            SKY_FIELD_RANGE(Collider2D, radius, Float, "Circle/capsule radius (local units)", 0.f, 1e5f),
            SKY_FIELD_RANGE(Collider2D, height, Float, "Capsule total height along local Y (>= 2 x radius)", 0.001f, 1e5f),
            SKY_FIELD(Collider2D, offset, Vec2, "Shape center relative to the entity origin (local)"),
            SKY_FIELD(Collider2D, rotation, Float, "Shape rotation around Z relative to the entity (degrees)"),
            SKY_FIELD_JSON(Collider2D, points,
                           "Polygon (convex, 3..8 points), chain (>= 4 points) or segment (2 points), local: [[x, y], ...]",
                           R"({"type": "array", "items": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}})"),
            SKY_FIELD(Collider2D, loop, Bool, "Chain: closed loop (counter-clockwise collides on the outside)"),
            SKY_FIELD_RANGE(Collider2D, friction, Float, "Surface friction (ice 0.02, wood 0.6, rubber 1)", 0.f, 10.f),
            SKY_FIELD_RANGE(Collider2D, restitution, Float, "Bounciness: 0 = none, 1 = perfectly elastic", 0.f, 1.f),
            SKY_FIELD_RANGE(Collider2D, density, Float, "kg per square unit (gives dynamic bodies their mass)", 0.f, 1e6f),
            SKY_FIELD(Collider2D, sensor, Bool, "Sensor: detects overlaps (trigger_enter/exit) but never blocks"),
            SKY_FIELD_ENUM(Collider2D, layer, "Collision layer", SKY_LAYERS_2D),
            SKY_FIELD(Collider2D, mask, String, "Layers it collides with: \"all\" or names (\"default, player\")"),
            SKY_FIELD(Collider2D, oneWay, Bool, "One-way platform: blocks only from above (local +Y), passable from below"),
            SKY_FIELD_ENUM(Collider2D, tileMerge, "Tilemap: chains = merged outlines (smooth), boxes = merged rectangles",
                           "chains", "boxes"),
            SKY_FIELD_JSON(Collider2D, tileShapes,
                           "Tilemap: per-tile shapes over the tileset's collision table, by tile id: full | none | slope_up | "
                           "slope_down | half_bottom | half_top | top (one-way), or {\"points\": [[px, py], ...], \"oneWay\": true} "
                           "in tile pixels (origin top-left, y down)",
                           R"({"type": "object"})"),
        }};
    return info;
}

const TypeInfo& Joint2D::type() {
    static const TypeInfo info{
        "joint2d",
        "Connects this entity's 2D body to another body (other) or to the world. revolute = hinge/pivot (wheels, doors, "
        "ragdoll limbs), prismatic = slider, distance = rod or spring, weld = glue (breakable with breakForce), wheel = "
        "car wheel with suspension along axis, target = pulls the body to a world point or the other entity (drag, "
        "grapple).",
        {
            SKY_FIELD_ENUM(Joint2D, kind, "Joint type", "revolute", "prismatic", "distance", "weld", "wheel", "target"),
            SKY_FIELD_ENTITY(Joint2D, other, "The other body (empty = the world; target: the entity to follow)"),
            SKY_FIELD(Joint2D, anchor, Vec2, "Pivot in this entity's local space"),
            SKY_FIELD(Joint2D, otherAnchor, Vec2, "The other end in the other body's local space (world point without other)"),
            SKY_FIELD(Joint2D, axis, Vec2, "Prismatic/wheel axis in this entity's local space"),
            SKY_FIELD_RANGE(Joint2D, length, Float, "Distance: rest length (0 = the distance at play start)", 0.f, 1e5f),
            SKY_FIELD(Joint2D, limitMin, Float, "Lower limit: degrees (revolute) or units; active when min < max"),
            SKY_FIELD(Joint2D, limitMax, Float, "Upper limit"),
            SKY_FIELD(Joint2D, motorSpeed, Float, "Motor speed: degrees/s (revolute, wheel) or units/s (prismatic)"),
            SKY_FIELD_RANGE(Joint2D, motorForce, Float, "Max motor torque (N m) or force (N); 0 = motor off", 0.f, 1e9f),
            SKY_FIELD_RANGE(Joint2D, stiffness, Float, "Spring frequency in Hz (0 = rigid)", 0.f, 1000.f),
            SKY_FIELD_RANGE(Joint2D, damping, Float, "Spring damping ratio (0.7 = settles without bouncing)", 0.f, 100.f),
            SKY_FIELD(Joint2D, target, Vec2, "Target joint: world point to pull to (when other is empty)"),
            SKY_FIELD_RANGE(Joint2D, maxForce, Float, "Target joint: strongest pull in N", 0.f, 1e9f),
            SKY_FIELD_RANGE(Joint2D, breakForce, Float, "Breaks above this force in N (0 = unbreakable)", 0.f, 1e9f),
            SKY_FIELD(Joint2D, collideConnected, Bool, "The two bodies collide with each other"),
            SKY_FIELD(Joint2D, enabled, Bool, "false after breaking (on event \"joint_broken\")"),
        }};
    return info;
}

const TypeInfo& Character2D::type() {
    static const TypeInfo info{
        "character2d",
        "Kinematic platformer controller on the XY plane: a capsule that slides along walls, walks up and down slopes up "
        "to maxSlope, lands on one-way platforms, with its own gravity, coyote time and jump buffering. Drive it every "
        "tick from Wander: move2d(self, axis(\"move\").x), if pressed(\"jump\") then jump2d(self) end, grounded2d(self). "
        "Dynamic bodies are pushed by it; sensors detect it.",
        {
            SKY_FIELD_RANGE(Character2D, height, Float, "Capsule height (units)", 0.05f, 100.f),
            SKY_FIELD_RANGE(Character2D, radius, Float, "Capsule radius (units)", 0.02f, 50.f),
            SKY_FIELD(Character2D, offset, Vec2, "Capsule center relative to the entity origin (y = height/2: origin at the feet)"),
            SKY_FIELD_RANGE(Character2D, moveSpeed, Float, "Run speed in units/s at move2d(self, 1)", 0.f, 1000.f),
            SKY_FIELD_RANGE(Character2D, acceleration, Float, "Ground acceleration in units/s^2", 0.f, 10000.f),
            SKY_FIELD_RANGE(Character2D, airControl, Float, "Share of the acceleration in the air (0..1)", 0.f, 1.f),
            SKY_FIELD_RANGE(Character2D, jumpSpeed, Float, "Jump take-off speed in units/s", 0.f, 1000.f),
            SKY_FIELD_RANGE(Character2D, gravity, Float, "Downward acceleration in units/s^2", 0.f, 10000.f),
            SKY_FIELD_RANGE(Character2D, fallMultiplier, Float, "Gravity multiplier while falling (snappier jumps)", 1.f, 10.f),
            SKY_FIELD_RANGE(Character2D, maxFallSpeed, Float, "Terminal fall speed in units/s", 0.f, 1000.f),
            SKY_FIELD_RANGE(Character2D, maxSlope, Float, "Steepest walkable slope in degrees", 0.f, 89.f),
            SKY_FIELD_RANGE(Character2D, coyoteTime, Float, "Seconds after leaving a ledge in which a jump still works", 0.f, 2.f),
            SKY_FIELD_RANGE(Character2D, jumpBuffer, Float, "Seconds a jump pressed in the air is remembered until landing", 0.f, 2.f),
            SKY_FIELD_RANGE(Character2D, snapDistance, Float, "Stays glued to the ground across slopes and small steps (units)", 0.f, 10.f),
            SKY_FIELD_ENUM(Character2D, layer, "Collision layer", SKY_LAYERS_2D),
            SKY_FIELD(Character2D, mask, String, "Layers it collides with: \"all\" or names"),
            SKY_FIELD(Character2D, velocity, Vec2, "Velocity in units/s (live while playing; writable: knockback, launch pads)"),
            SKY_FIELD(Character2D, grounded, Bool, "Standing on walkable ground (live while playing, read-only)"),
        }};
    return info;
}

const TypeInfo& Physics2DSettings::type() {
    static const TypeInfo info{
        "physics2d_world",
        "Scene-wide 2D physics settings (put it on one entity; defaults apply without one): gravity, solver sub-steps, "
        "sleeping, the impact event threshold and debug drawing of shapes, contacts and joints.",
        {
            SKY_FIELD(Physics2DSettings, gravity, Vec2, "Gravity in units/s^2 (default 0, -20)"),
            SKY_FIELD_RANGE(Physics2DSettings, substeps, Int, "Solver sub-steps per tick (1..16; more = stiffer stacks)", 1.f, 16.f),
            SKY_FIELD(Physics2DSettings, allowSleep, Bool, "Bodies at rest may sleep"),
            SKY_FIELD_RANGE(Physics2DSettings, impactSpeed, Float, "Approach speed (units/s) above which `on impact` fires", 0.f, 1e4f),
            SKY_FIELD(Physics2DSettings, debugDraw, Bool, "Draw shapes, contacts and joints in the frame"),
            SKY_FIELD(Physics2DSettings, enabled, Bool, "Simulate 2D physics"),
        }};
    return info;
}

}  // namespace sky
