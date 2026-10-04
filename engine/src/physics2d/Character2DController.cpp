// character2d: a kinematic platformer controller built on Box2D's mover queries.
//
// Each tick: input -> horizontal velocity (ground acceleration, air control), jump (now, within
// coyote time, or buffered until landing), gravity (stronger while falling). The capsule then moves
// in up to 5 iterations: gather collision planes around it (b2World_CollideMover), solve the move
// against them (b2SolvePlanes), sweep it (b2World_CastMover) and advance. One-way platforms are left
// out of the sweep and only block a capsule whose feet are above them while it moves down. Afterwards
// a short probe below the feet decides `grounded` (walkable when the normal is within maxSlope) and
// snaps the capsule down slopes and small steps. The kinematic proxy body follows the capsule, so
// sensors see the character and dynamic bodies it touches are pushed.

#include <algorithm>
#include <cmath>

#include "Physics2DInternal.h"

namespace sky::physics2d {

namespace {

constexpr float kDeg = 3.14159265358979f / 180.f;
constexpr int kMaxPlanes = 16;

struct MoverContext {
    const CharEntry* c = nullptr;
    float feetY = 0.f;   // bottom of the capsule before the move
    bool rising = false;
    b2CollisionPlane planes[kMaxPlanes];
    int count = 0;
    struct Push {
        b2BodyId body;
        Vec2 normal;
    } pushes[kMaxPlanes];
    int pushCount = 0;
};

/// Whether a one-way platform blocks this character now: it is below the feet and the character is not rising.
bool oneWayBlocks(const CharEntry& c, b2ShapeId shape, Vec2 normal, float feetY, bool rising) {
    if (rising || c.dropTimer > 0.f) return false;
    const auto* tag = static_cast<const ShapeTag*>(b2Shape_GetUserData(shape));
    const Vec2 up = tag ? tag->up : Vec2{0.f, 1.f};
    if (dot2(normal, up) < 0.7f) return false;
    const b2AABB box = b2Shape_GetAABB(shape);
    return feetY >= box.upperBound.y - 0.05f;
}

bool planeCallback(b2ShapeId shape, const b2PlaneResult* plane, void* context) {
    auto* ctx = static_cast<MoverContext*>(context);
    const auto* tag = static_cast<const ShapeTag*>(b2Shape_GetUserData(shape));
    if (tag && (tag->character || tag->sensor)) return true;  // itself, other characters (soft), sensors
    const Vec2 n = fromB2(plane->plane.normal);
    if (tag && tag->oneWay && !oneWayBlocks(*ctx->c, shape, n, ctx->feetY, ctx->rising)) return true;
    if (ctx->count < kMaxPlanes) {
        ctx->planes[ctx->count++] = {plane->plane, FLT_MAX, 0.f, true};
        b2BodyId body = b2Shape_GetBody(shape);
        if (b2Body_GetType(body) == b2_dynamicBody && ctx->pushCount < kMaxPlanes) ctx->pushes[ctx->pushCount++] = {body, n};
    }
    return true;
}

struct CastContext {
    const CharEntry* c = nullptr;
    float feetY = 0.f;
    bool rising = false;
    bool oneWayOnly = false;
    float fraction = 1.f;
    Vec2 normal{0.f, 1.f};
    b2ShapeId shape = b2_nullShapeId;
};

float castCallback(b2ShapeId shape, b2Vec2, b2Vec2 normal, float fraction, void* context) {
    auto* ctx = static_cast<CastContext*>(context);
    const auto* tag = static_cast<const ShapeTag*>(b2Shape_GetUserData(shape));
    if (tag && (tag->character || tag->sensor)) return -1.f;
    const bool oneWay = tag && tag->oneWay;
    if (ctx->oneWayOnly && !oneWay) return -1.f;
    if (oneWay && !oneWayBlocks(*ctx->c, shape, fromB2(normal), ctx->feetY, ctx->rising)) return -1.f;
    if (fraction < ctx->fraction) {
        ctx->fraction = fraction;
        ctx->normal = fromB2(normal);
        ctx->shape = shape;
    }
    return fraction;
}

float approach(float v, float target, float step) {
    if (v < target) return std::min(v + step, target);
    return std::max(v - step, target);
}

}  // namespace

void Physics2DWorld::Impl::moveCharacters(float dt) {
    for (auto& [e, c] : chars) moveCharacter(c, dt);
}

void Physics2DWorld::Impl::moveCharacter(CharEntry& c, float dt) {
    const Character2D& k = c.settings;
    const float r = std::max(0.02f, k.radius);
    const float half = std::max(0.f, k.height * 0.5f - r);
    const float cosSlope = std::cos(std::clamp(k.maxSlope, 0.f, 89.f) * kDeg);
    auto capsuleAt = [&](Vec2 pos, float radius) {
        b2Capsule cap;
        cap.center1 = toB2(pos + k.offset + Vec2{0.f, -half});
        cap.center2 = toB2(pos + k.offset + Vec2{0.f, half});
        cap.radius = radius;
        return cap;
    };
    auto feetOf = [&](Vec2 pos) { return pos.y + k.offset.y - half - r; };

    // --- timers and input -----------------------------------------------------------------------
    c.dropTimer = std::max(0.f, c.dropTimer - dt);
    if (c.grounded) c.coyote = k.coyoteTime;
    else c.coyote = std::max(0.f, c.coyote - dt);
    c.jumpBuffer = std::max(0.f, c.jumpBuffer - dt);
    const float target = std::clamp(c.desired, -1.f, 1.f) * k.moveSpeed;
    const float accel = k.acceleration * (c.grounded ? 1.f : std::clamp(k.airControl, 0.f, 1.f));
    c.velocity.x = approach(c.velocity.x, target, accel * dt);
    c.desired = 0.f;  // move2d is called every tick while held

    bool jumped = false;
    if (c.jumpBuffer > 0.f && (c.grounded || c.coyote > 0.f)) {
        c.velocity.y = c.pendingJumpSpeed > 0.f ? c.pendingJumpSpeed : k.jumpSpeed;
        c.grounded = false;
        c.coyote = 0.f;
        c.jumpBuffer = 0.f;
        jumped = true;
    }
    const bool walking = c.grounded && !jumped;
    if (walking) {
        c.velocity.y = 0.f;
    } else {
        const float g = k.gravity * (c.velocity.y < 0.f ? std::max(1.f, k.fallMultiplier) : 1.f);
        c.velocity.y = std::max(c.velocity.y - g * dt, -std::abs(k.maxFallSpeed));
    }

    // --- desired translation ---------------------------------------------------------------------
    Vec2 delta = c.velocity * dt;
    if (walking) {
        // Along the ground: walking up or down a slope keeps contact.
        Vec2 tangent{c.groundNormal.y, -c.groundNormal.x};
        delta = tangent * (c.velocity.x * dt);
    }

    const b2QueryFilter collideFilter{c.category, (c.mask | kOneWayBit) & ~kCharacterBit};
    const b2QueryFilter castFilter{c.category, c.mask & ~(kOneWayBit | kCharacterBit)};
    const b2QueryFilter oneWayFilter{c.category, kOneWayBit};
    // Walking up a slope moves up without rising: only a jump or an upward launch passes one-way platforms.
    const bool rising = jumped || (!walking && c.velocity.y > 0.f);

    Vec2 pos = c.position;
    const Vec2 goal = pos + delta;
    MoverContext mc;
    mc.c = &c;
    mc.rising = rising;
    for (int iteration = 0; iteration < 5; ++iteration) {
        mc.count = 0;
        mc.feetY = feetOf(pos);
        b2Capsule mover = capsuleAt(pos, r);
        b2World_CollideMover(world, &mover, collideFilter, planeCallback, &mc);
        b2PlaneSolverResult solved = b2SolvePlanes(toB2(goal - pos), mc.planes, mc.count);
        Vec2 step = fromB2(solved.translation);
        float fraction = b2World_CastMover(world, &mover, toB2(step), castFilter);
        if (step.y < 0.f) {  // landing on one-way platforms
            CastContext cc;
            cc.c = &c;
            cc.feetY = mc.feetY;
            cc.oneWayOnly = true;
            b2Capsule shrunk = capsuleAt(pos, r - kSkin);
            b2ShapeProxy proxy = b2MakeProxy(&shrunk.center1, 2, shrunk.radius);
            b2World_CastShape(world, &proxy, toB2(step), oneWayFilter, castCallback, &cc);
            if (cc.fraction < 1.f) {
                const float len = len2(step);
                fraction = std::min(fraction, std::max(0.f, (cc.fraction * len - kSkin) / std::max(len, 1e-6f)));
            }
        }
        const Vec2 moved = step * fraction;
        pos = pos + moved;
        if (len2(moved) < 1e-4f) break;
    }
    c.velocity = fromB2(b2ClipVector(toB2(c.velocity), mc.planes, mc.count));
    if (walking) c.velocity.y = 0.f;  // speed along the ground is horizontal speed: no launch at the top of a slope
    // Push dynamic bodies it walked into.
    for (int i = 0; i < mc.pushCount; ++i) {
        const auto& p = mc.pushes[i];
        const float into = -dot2(Vec2{target, 0.f}, p.normal);
        if (into <= 0.f) continue;
        const float mass = b2Body_GetMass(p.body);
        b2Body_ApplyLinearImpulseToCenter(p.body, toB2(p.normal * (-into * mass * 0.25f)), true);
    }

    // --- ground probe and snapping -------------------------------------------------------------------
    const bool snap = c.grounded && !jumped && c.velocity.y <= 0.f;
    const float probe = (snap ? std::max(k.snapDistance, kSkin) : kSkin) + 2.f * kSkin;
    CastContext gc;
    gc.c = &c;
    gc.feetY = feetOf(pos);
    gc.rising = rising && c.velocity.y > 0.01f;
    b2Capsule shrunk = capsuleAt(pos, r - kSkin);
    b2ShapeProxy proxy = b2MakeProxy(&shrunk.center1, 2, shrunk.radius);
    const b2QueryFilter groundFilter{c.category, (c.mask | kOneWayBit) & ~kCharacterBit};
    b2World_CastShape(world, &proxy, b2Vec2{0.f, -probe}, groundFilter, castCallback, &gc);
    c.grounded = false;
    c.groundShape = b2_nullShapeId;
    if (gc.fraction < 1.f && !gc.rising && gc.normal.y >= cosSlope) {
        const float gap = gc.fraction * probe - kSkin;  // distance from the real capsule to the ground
        if (gap <= 2.f * kSkin || snap) {
            if (gap > kSkin) pos.y -= gap - kSkin * 0.5f;
            c.grounded = true;
            c.groundNormal = gc.normal;
            c.groundShape = gc.shape;
            if (c.velocity.y < 0.f) c.velocity.y = 0.f;
        }
    }
    if (!c.grounded) c.groundNormal = {0.f, 1.f};

    c.position = pos;
    if (B2_IS_NON_NULL(c.id)) b2Body_SetTargetTransform(c.id, b2Transform{toB2(pos), b2Rot_identity}, dt);
}

void Physics2DWorld::Impl::writeBackCharacters(Scene& s) {
    bool moved = false;
    for (auto& [e, c] : chars) {
        Character2D* ch = s.get<Character2D>(e);
        Transform* t = s.get<Transform>(e);
        if (!ch || !t) continue;
        const EntityRecord* r = s.record(e);
        if (r && r->parent && s.exists(r->parent)) {
            const Mat4 parent = s.worldMatrix(r->parent);
            Vec3 local = parent.inverse().transformPoint({c.position.x, c.position.y, c.z});
            t->position.x = local.x;
            t->position.y = local.y;
        } else {
            t->position.x = c.position.x;
            t->position.y = c.position.y;
        }
        c.lastPose = poseOf(s.worldMatrix(e));
        ch->velocity = c.velocity;
        ch->grounded = c.grounded;
        c.lastVelocity = c.velocity;
        moved = true;
    }
    if (moved) s.markDirty();
}

bool Physics2DWorld::move(EntityId e, float direction) {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return false;
    it->second.desired = std::clamp(direction, -1.f, 1.f);
    return true;
}

bool Physics2DWorld::jump(EntityId e, float speed) {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return false;
    CharEntry& c = it->second;
    c.pendingJumpSpeed = speed;
    const bool now = c.grounded || c.coyote > 0.f;
    // Executed at the start of the next move: now when allowed, else when landing within jumpBuffer.
    c.jumpBuffer = std::max(now ? 1e-3f : 0.f, c.settings.jumpBuffer) + (now ? 1.f / 60.f : 0.f);
    return now;
}

bool Physics2DWorld::dropThrough(EntityId e) {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return false;
    CharEntry& c = it->second;
    if (B2_IS_NULL(c.groundShape) || !b2Shape_IsValid(c.groundShape)) return false;
    const auto* tag = static_cast<const ShapeTag*>(b2Shape_GetUserData(c.groundShape));
    if (!tag || !tag->oneWay) return false;
    c.dropTimer = 0.25f;
    c.grounded = false;
    return true;
}

std::optional<bool> Physics2DWorld::grounded(EntityId e) const {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return std::nullopt;
    return it->second.grounded;
}

bool Physics2DWorld::hasCharacter(EntityId e) const { return impl_->chars.count(e) != 0; }

}  // namespace sky::physics2d
