// Physics2DWorld queries (raycasts, overlaps, point tests), statistics and debug drawing.

#include <algorithm>
#include <cmath>

#include "Physics2DInternal.h"

namespace sky::physics2d {

namespace {

const ShapeTag* tagOf(b2ShapeId id) { return static_cast<const ShapeTag*>(b2Shape_GetUserData(id)); }

struct RayContext {
    const Physics2DWorld::Impl* impl;
    const Filter2D* filter;
    bool all = false;
    float maxDistance = 0.f;
    std::vector<Hit2D> hits;
};

float rayCallback(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context) {
    auto* ctx = static_cast<RayContext*>(context);
    if (!ctx->impl->accept(shape, *ctx->filter)) return -1.f;
    const ShapeTag* tag = tagOf(shape);
    Hit2D h{tag->owner, fromB2(point), fromB2(normal), fraction * ctx->maxDistance};
    if (!ctx->all) {
        ctx->hits.assign(1, h);
        return fraction;  // clip: only nearer hits from now on
    }
    ctx->hits.push_back(h);
    return 1.f;
}

struct OverlapContext {
    const Physics2DWorld::Impl* impl;
    const Filter2D* filter;
    std::vector<b2ShapeId> shapes;
};

bool overlapCallback(b2ShapeId shape, void* context) {
    auto* ctx = static_cast<OverlapContext*>(context);
    if (ctx->impl->accept(shape, *ctx->filter)) ctx->shapes.push_back(shape);
    return true;
}

/// Owners of the shapes, nearest to `center` first (ties by entity id), each once.
std::vector<EntityId> nearestOwners(const std::vector<b2ShapeId>& shapes, Vec2 center) {
    std::vector<std::pair<float, EntityId>> found;
    for (b2ShapeId s : shapes) {
        const ShapeTag* tag = tagOf(s);
        float d = 0.f;
        if (!b2Shape_TestPoint(s, toB2(center))) d = len2(fromB2(b2Shape_GetClosestPoint(s, toB2(center))) - center);
        found.push_back({d, tag->owner});
    }
    std::sort(found.begin(), found.end());
    std::vector<EntityId> out;
    for (const auto& [d, e] : found) {
        if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
    }
    return out;
}

void circleLines(std::vector<Vec2>& out, Vec2 c, float r, float angle) {
    constexpr int kSegments = 20;
    for (int i = 0; i < kSegments; ++i) {
        float a0 = angle + 2.f * 3.14159265f * static_cast<float>(i) / kSegments;
        float a1 = angle + 2.f * 3.14159265f * static_cast<float>(i + 1) / kSegments;
        out.push_back(c + Vec2{std::cos(a0), std::sin(a0)} * r);
        out.push_back(c + Vec2{std::cos(a1), std::sin(a1)} * r);
    }
    out.push_back(c);
    out.push_back(c + Vec2{std::cos(angle), std::sin(angle)} * r);  // shows rotation
}

void shapeLines(b2ShapeId s, std::vector<Vec2>& out) {
    const b2Transform xf = b2Body_GetTransform(b2Shape_GetBody(s));
    auto w = [&](b2Vec2 p) { return fromB2(b2TransformPoint(xf, p)); };
    switch (b2Shape_GetType(s)) {
        case b2_polygonShape: {
            b2Polygon p = b2Shape_GetPolygon(s);
            for (int i = 0; i < p.count; ++i) {
                out.push_back(w(p.vertices[i]));
                out.push_back(w(p.vertices[(i + 1) % p.count]));
            }
            break;
        }
        case b2_circleShape: {
            b2Circle c = b2Shape_GetCircle(s);
            circleLines(out, w(c.center), c.radius, b2Rot_GetAngle(xf.q));
            break;
        }
        case b2_capsuleShape: {
            b2Capsule c = b2Shape_GetCapsule(s);
            Vec2 a = w(c.center1), b = w(c.center2);
            Vec2 axis = b - a;
            float len = len2(axis);
            Vec2 n = len > 1e-6f ? Vec2{-axis.y / len, axis.x / len} : Vec2{1.f, 0.f};
            out.push_back(a + n * c.radius);
            out.push_back(b + n * c.radius);
            out.push_back(a - n * c.radius);
            out.push_back(b - n * c.radius);
            circleLines(out, a, c.radius, 0.f);
            circleLines(out, b, c.radius, 0.f);
            break;
        }
        case b2_segmentShape: {
            b2Segment g = b2Shape_GetSegment(s);
            out.push_back(w(g.point1));
            out.push_back(w(g.point2));
            break;
        }
        case b2_chainSegmentShape: {
            b2ChainSegment g = b2Shape_GetChainSegment(s);
            out.push_back(w(g.segment.point1));
            out.push_back(w(g.segment.point2));
            break;
        }
        default: break;
    }
}

}  // namespace

b2QueryFilter Physics2DWorld::Impl::queryFilter(const Filter2D&) const {
    return {~0ull, ~0ull};  // layers, sensors and excludes are checked per shape (accept)
}

bool Physics2DWorld::Impl::accept(b2ShapeId shape, const Filter2D& f) const {
    const ShapeTag* tag = tagOf(shape);
    if (!tag) return false;
    if (tag->sensor && !f.includeSensors) return false;
    if (!(tag->layer & f.layerMask)) return false;
    for (EntityId x : f.exclude) {
        if (x == tag->owner || x == tag->entity) return false;
    }
    return true;
}

std::optional<Hit2D> Physics2DWorld::raycast(Vec2 origin, Vec2 direction, float maxDistance, const Filter2D& filter) const {
    float len = len2(direction);
    if (len < 1e-6f || maxDistance <= 0.f) return std::nullopt;
    RayContext ctx{impl_.get(), &filter, false, maxDistance, {}};
    b2World_CastRay(impl_->world, toB2(origin), toB2(direction * (maxDistance / len)), impl_->queryFilter(filter), rayCallback, &ctx);
    if (ctx.hits.empty()) return std::nullopt;
    return ctx.hits.front();
}

std::vector<Hit2D> Physics2DWorld::raycastAll(Vec2 origin, Vec2 direction, float maxDistance, const Filter2D& filter) const {
    float len = len2(direction);
    if (len < 1e-6f || maxDistance <= 0.f) return {};
    RayContext ctx{impl_.get(), &filter, true, maxDistance, {}};
    b2World_CastRay(impl_->world, toB2(origin), toB2(direction * (maxDistance / len)), impl_->queryFilter(filter), rayCallback, &ctx);
    std::sort(ctx.hits.begin(), ctx.hits.end(),
              [](const Hit2D& a, const Hit2D& b) { return a.distance != b.distance ? a.distance < b.distance : a.entity < b.entity; });
    std::vector<Hit2D> out;
    for (const Hit2D& h : ctx.hits) {
        if (std::none_of(out.begin(), out.end(), [&](const Hit2D& o) { return o.entity == h.entity; })) out.push_back(h);
    }
    return out;
}

std::vector<EntityId> Physics2DWorld::overlapCircle(Vec2 center, float radius, const Filter2D& filter) const {
    OverlapContext ctx{impl_.get(), &filter, {}};
    b2Vec2 c = toB2(center);
    b2ShapeProxy proxy = b2MakeProxy(&c, 1, std::max(radius, 0.001f));
    b2World_OverlapShape(impl_->world, &proxy, impl_->queryFilter(filter), overlapCallback, &ctx);
    return nearestOwners(ctx.shapes, center);
}

std::vector<EntityId> Physics2DWorld::overlapBox(Vec2 center, Vec2 halfExtents, float angleDegrees, const Filter2D& filter) const {
    OverlapContext ctx{impl_.get(), &filter, {}};
    const float a = angleDegrees * 3.14159265358979f / 180.f;
    const Vec2 hx = rotate2({std::max(halfExtents.x, 0.001f), 0.f}, a), hy = rotate2({0.f, std::max(halfExtents.y, 0.001f)}, a);
    b2Vec2 pts[4] = {toB2(center - hx - hy), toB2(center + hx - hy), toB2(center + hx + hy), toB2(center - hx + hy)};
    b2ShapeProxy proxy = b2MakeProxy(pts, 4, 0.f);
    b2World_OverlapShape(impl_->world, &proxy, impl_->queryFilter(filter), overlapCallback, &ctx);
    return nearestOwners(ctx.shapes, center);
}

std::vector<EntityId> Physics2DWorld::pointQuery(Vec2 point, const Filter2D& filter) const {
    OverlapContext ctx{impl_.get(), &filter, {}};
    b2AABB box{{point.x - 1e-3f, point.y - 1e-3f}, {point.x + 1e-3f, point.y + 1e-3f}};
    b2World_OverlapAABB(impl_->world, box, impl_->queryFilter(filter), overlapCallback, &ctx);
    std::vector<b2ShapeId> inside;
    for (b2ShapeId s : ctx.shapes) {
        if (b2Shape_TestPoint(s, toB2(point))) inside.push_back(s);
    }
    return nearestOwners(inside, point);
}

Stats2D Physics2DWorld::stats() const {
    Stats2D st;
    for (const auto& [e, b] : impl_->bodies) {
        ++st.bodies;
        if (b.type == b2_dynamicBody) ++st.dynamicBodies;
        else if (b.type == b2_kinematicBody) ++st.kinematicBodies;
        else ++st.staticBodies;
        if (b.type != b2_staticBody) {
            if (b2Body_IsAwake(b.id)) ++st.awakeBodies;
            else ++st.sleepingBodies;
        }
        st.shapes += static_cast<int>(b.shapes.size());
        st.chains += b.chains;
        for (const auto& t : b.tags) st.sensors += t->sensor ? 1 : 0;
    }
    st.characters = static_cast<int>(impl_->chars.size());
    st.joints = static_cast<int>(impl_->joints.size());
    st.contacts = b2World_GetCounters(impl_->world).contactCount;
    return st;
}

DebugDraw2D Physics2DWorld::debugDraw() const {
    DebugDraw2D out;
    for (const auto& [e, b] : impl_->bodies) {
        const bool sleeping = b.type != b2_staticBody && !b2Body_IsAwake(b.id);
        for (b2ShapeId s : b.shapes) {
            if (!b2Shape_IsValid(s)) continue;
            const ShapeTag* tag = tagOf(s);
            DebugShape2D d;
            d.entity = tag ? tag->entity : e;
            d.kind = tag && tag->sensor   ? "sensor"
                     : tag && tag->oneWay ? "one_way"
                     : b.type == b2_dynamicBody   ? "dynamic"
                     : b.type == b2_kinematicBody ? "kinematic"
                                                  : "static";
            d.sleeping = sleeping;
            shapeLines(s, d.lines);
            // Merge consecutive segments of one entity and kind (chains) into one wireframe.
            if (!out.shapes.empty() && out.shapes.back().entity == d.entity && out.shapes.back().kind == d.kind &&
                b2Shape_GetType(s) == b2_chainSegmentShape) {
                out.shapes.back().lines.insert(out.shapes.back().lines.end(), d.lines.begin(), d.lines.end());
            } else {
                out.shapes.push_back(std::move(d));
            }
        }
        if (b.type == b2_dynamicBody) {
            std::vector<b2ContactData> contacts(static_cast<size_t>(b2Body_GetContactCapacity(b.id)));
            int n = b2Body_GetContactData(b.id, contacts.data(), static_cast<int>(contacts.size()));
            for (int i = 0; i < n; ++i) {
                const b2Manifold& m = contacts[static_cast<size_t>(i)].manifold;
                for (int k = 0; k < m.pointCount; ++k) out.contacts.push_back({fromB2(m.points[k].point), fromB2(m.normal)});
            }
        }
    }
    for (const auto& [e, c] : impl_->chars) {
        DebugShape2D d;
        d.entity = e;
        d.kind = "character";
        std::vector<b2ShapeId> shapes(static_cast<size_t>(b2Body_GetShapeCount(c.id)));
        b2Body_GetShapes(c.id, shapes.data(), static_cast<int>(shapes.size()));
        for (b2ShapeId s : shapes) shapeLines(s, d.lines);
        out.shapes.push_back(std::move(d));
    }
    for (const auto& [e, j] : impl_->joints) {
        if (B2_IS_NULL(j.id) || !b2Joint_IsValid(j.id)) continue;
        b2BodyId a = b2Joint_GetBodyA(j.id), b = b2Joint_GetBodyB(j.id);
        out.joints.push_back({fromB2(b2Body_GetWorldPoint(a, b2Joint_GetLocalAnchorA(j.id))),
                              fromB2(b2Body_GetWorldPoint(b, b2Joint_GetLocalAnchorB(j.id)))});
    }
    // Deterministic order of contacts (several bodies report the same manifold).
    std::sort(out.contacts.begin(), out.contacts.end(), [](const auto& x, const auto& y) {
        return std::tie(x.first.x, x.first.y) < std::tie(y.first.x, y.first.y);
    });
    out.contacts.erase(std::unique(out.contacts.begin(), out.contacts.end()), out.contacts.end());
    return out;
}

}  // namespace sky::physics2d
