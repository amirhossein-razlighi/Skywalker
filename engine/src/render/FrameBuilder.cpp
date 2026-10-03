#include <algorithm>
#include <cmath>

#include "skywalker/render/Renderer.h"

namespace sky {

Mat4 ViewCamera::projection(float aspect) const {
    if (orthographic) return Mat4::orthographic(orthoSize, aspect, nearPlane, farPlane);
    return Mat4::perspective(radians(fovDeg), aspect, nearPlane, farPlane);
}

Ray ViewCamera::rayAt(float x, float y, int width, int height) const {
    float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
    Mat4 inv = (projection(aspect) * view()).inverse();
    float nx = 2.f * x / static_cast<float>(width) - 1.f;
    float ny = 1.f - 2.f * y / static_cast<float>(height);
    Vec3 nearP = inv.transformPoint({nx, ny, 0.f});
    Vec3 farP = inv.transformPoint({nx, ny, 1.f});
    return {nearP, normalize(farP - nearP)};
}

// ---------------------------------------------------------------------------
// OrbitCamera
// ---------------------------------------------------------------------------

void OrbitCamera::orbit(float dYaw, float dPitch) {
    yaw = std::remainder(yaw + dYaw, 360.f);
    pitch = std::clamp(pitch + dPitch, -89.f, 89.f);
}

void OrbitCamera::pan(float dx, float dy) {
    ViewCamera v = toView();
    Vec3 fwd = normalize(v.target - v.eye);
    Vec3 right = normalize(cross(fwd, {0, 1, 0}));
    Vec3 up = cross(right, fwd);
    target += (right * -dx + up * dy) * distance;
}

void OrbitCamera::zoom(float factor) { distance = std::clamp(distance * factor, 0.2f, 5000.f); }

void OrbitCamera::frame(const Aabb& box) {
    target = box.center();
    float radius = std::max(length(box.extents()), 0.25f);
    distance = std::clamp(radius / std::sin(radians(fovDeg * 0.5f)) * 0.9f, 0.5f, 5000.f);
}

void OrbitCamera::lookAt(Vec3 eye, Vec3 newTarget) {
    target = newTarget;
    Vec3 d = eye - newTarget;
    distance = std::max(length(d), 0.2f);
    pitch = std::clamp(degrees(std::asin(std::clamp(d.y / distance, -1.f, 1.f))), -89.f, 89.f);
    yaw = degrees(std::atan2(d.x, d.z));
}

ViewCamera OrbitCamera::toView() const {
    float cp = std::cos(radians(pitch)), sp = std::sin(radians(pitch));
    float cy = std::cos(radians(yaw)), sy = std::sin(radians(yaw));
    ViewCamera v;
    v.target = target;
    v.eye = target + Vec3{cp * sy, sp, cp * cy} * distance;
    v.fovDeg = fovDeg;
    v.nearPlane = std::max(0.01f, distance * 0.005f);
    v.farPlane = std::max(1000.f, distance * 50.f);
    return v;
}

Json OrbitCamera::toJson() const {
    ViewCamera v = toView();
    return Json::object({{"eye", reflect::vec3ToJson(v.eye)},
                         {"target", reflect::vec3ToJson(target)},
                         {"yaw", yaw},
                         {"pitch", pitch},
                         {"distance", distance},
                         {"fov", fovDeg}});
}

// ---------------------------------------------------------------------------
// Frame building
// ---------------------------------------------------------------------------

bool sceneCamera(const Scene& scene, ViewCamera& out, EntityId preferred) {
    EntityId chosen = kNoEntity;
    if (preferred && scene.get<Camera>(preferred)) {
        chosen = preferred;
    } else {
        for (EntityId e : scene.entities()) {
            const Camera* c = scene.get<Camera>(e);
            if (c && c->primary && scene.isActive(e)) {
                chosen = e;
                break;
            }
        }
    }
    if (!chosen) return false;
    const Camera* c = scene.get<Camera>(chosen);
    Mat4 world = scene.worldMatrix(chosen);
    out.eye = world.translation();
    out.target = out.eye + normalize(world.transformDir({0, 0, -1}));
    out.up = normalize(world.transformDir({0, 1, 0}));
    out.fovDeg = c->fov;
    out.nearPlane = c->nearPlane;
    out.farPlane = c->farPlane;
    out.orthographic = c->orthographic;
    out.orthoSize = c->orthoSize;
    return true;
}

FrameData buildFrame(const Scene& scene, const ViewCamera& camera, int width, int height, const BuildOptions& opts) {
    FrameData f;
    f.width = std::max(width, 1);
    f.height = std::max(height, 1);
    f.camera = camera;
    f.view = camera.view();
    f.projection = camera.projection(static_cast<float>(f.width) / static_cast<float>(f.height));
    f.environment = scene.environment();
    f.drawGrid = opts.editorOverlays && scene.environment().showGrid;
    f.time = opts.time;

    for (EntityId e : scene.entities()) {
        if (!scene.isActive(e)) continue;
        Mat4 world = scene.worldMatrix(e);
        if (const MeshRenderer* m = scene.get<MeshRenderer>(e); m && m->visible) {
            DrawItem d;
            d.entity = e;
            d.mesh = m->mesh;
            d.texture = m->texture;
            d.color = m->color;
            d.emissive = m->emissive;
            d.metallic = m->metallic;
            d.roughness = m->roughness;
            d.unlit = m->unlit;
            if (!m->material.empty() && opts.material) {
                if (const ResolvedMaterial* mat = opts.material(m->material)) {
                    d.color = mat->color;
                    d.metallic = mat->metallic;
                    d.roughness = mat->roughness;
                    d.emissive = mat->emissive;
                    d.texture = mat->texture;
                    d.tiling = mat->tiling;
                    d.unlit = d.unlit || mat->unlit;
                }
            }
            d.model = world;
            if (m->billboard) {
                Vec3 p = world.translation();
                Vec3 s{length(world.transformDir({1, 0, 0})), length(world.transformDir({0, 1, 0})),
                       length(world.transformDir({0, 0, 1}))};
                Vec3 d2c = camera.eye - p;
                float horiz = std::sqrt(d2c.x * d2c.x + d2c.z * d2c.z);
                float yaw = degrees(std::atan2(d2c.x, d2c.z));
                float pitch = degrees(std::atan2(d2c.y, horiz));
                d.model = Mat4::trs(p, {-pitch, yaw, 0}, s);
            }
            d.selected = opts.editorOverlays &&
                         std::find(opts.selection.begin(), opts.selection.end(), e) != opts.selection.end();
            d.worldBounds = scene.localBounds(e).transformed(d.model);
            f.draws.push_back(std::move(d));
        }
        if (const Light* l = scene.get<Light>(e); l && f.lights.size() < FrameData::kMaxLights) {
            LightItem li;
            li.kind = l->kind == "directional" ? LightItem::Kind::Directional
                      : l->kind == "spot"      ? LightItem::Kind::Spot
                                               : LightItem::Kind::Point;
            li.position = world.translation();
            li.direction = normalize(world.transformDir({0, 0, -1}));
            li.color = l->color.xyz();
            li.intensity = l->intensity;
            li.range = l->range;
            li.cosCone = std::cos(radians(l->spotAngle));
            f.lights.push_back(li);
        }
    }
    return f;
}

std::vector<VisibleEntity> visibleEntities(const Scene& scene, const FrameData& frame) {
    std::vector<VisibleEntity> out;
    const Mat4 vp = frame.viewProjection();
    const float W = static_cast<float>(frame.width), H = static_cast<float>(frame.height);

    auto project = [&](Vec3 p, Vec2& screen) -> bool {
        Vec4 c = vp * Vec4(p, 1.f);
        if (c.w <= 1e-5f) return false;
        screen = {(c.x / c.w * 0.5f + 0.5f) * W, (0.5f - c.y / c.w * 0.5f) * H};
        return true;
    };

    for (const DrawItem& d : frame.draws) {
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        int inFront = 0;
        for (int i = 0; i < 8; ++i) {
            const Aabb& b = d.worldBounds;
            Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
            Vec2 s;
            if (!project(c, s)) continue;
            ++inFront;
            minX = std::min(minX, s.x);
            minY = std::min(minY, s.y);
            maxX = std::max(maxX, s.x);
            maxY = std::max(maxY, s.y);
        }
        if (inFront == 0) continue;
        if (inFront < 8) {  // partially behind the camera: conservatively extend to edges
            minX = std::min(minX, 0.f);
            minY = std::min(minY, 0.f);
            maxX = std::max(maxX, W);
            maxY = std::max(maxY, H);
        }
        float x0 = std::clamp(minX, 0.f, W), y0 = std::clamp(minY, 0.f, H);
        float x1 = std::clamp(maxX, 0.f, W), y1 = std::clamp(maxY, 0.f, H);
        if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) continue;
        const EntityRecord* rec = scene.record(d.entity);
        out.push_back({d.entity, rec ? rec->name : "", x0, y0, x1 - x0, y1 - y0,
                       distance(frame.camera.eye, d.worldBounds.center()), (x1 - x0) * (y1 - y0) / (W * H)});
    }
    // Non-mesh entities (lights, cameras, empties) appear as small markers.
    for (EntityId e : scene.entities()) {
        if (!scene.isActive(e)) continue;
        const MeshRenderer* m = scene.get<MeshRenderer>(e);
        if (m && m->visible) continue;
        Vec3 p = scene.worldMatrix(e).translation();
        Vec2 s;
        if (!project(p, s) || s.x < 0 || s.y < 0 || s.x > W || s.y > H) continue;
        out.push_back({e, scene.record(e)->name, s.x - 6, s.y - 6, 12, 12, distance(frame.camera.eye, p), 0.f});
    }
    std::sort(out.begin(), out.end(), [](const VisibleEntity& a, const VisibleEntity& b) { return a.depth < b.depth; });
    return out;
}

EntityId pick(const Scene& scene, const FrameData& frame, float x, float y) {
    Ray ray = frame.camera.rayAt(x, y, frame.width, frame.height);
    EntityId best = kNoEntity;
    float bestT = 1e30f;
    for (const DrawItem& d : frame.draws) {
        if (intersect(ray, d.worldBounds) < 0) continue;  // cheap reject
        Mat4 inv = d.model.inverse();
        Ray local{inv.transformPoint(ray.origin), inv.transformDir(ray.dir)};
        float tLocal = -1;
        if (d.mesh == "sphere") {
            // |o + t d|^2 = 0.25
            float a = dot(local.dir, local.dir), b = 2 * dot(local.origin, local.dir);
            float c = dot(local.origin, local.origin) - 0.25f;
            float disc = b * b - 4 * a * c;
            if (disc >= 0) {
                float sq = std::sqrt(disc);
                float t0 = (-b - sq) / (2 * a), t1 = (-b + sq) / (2 * a);
                tLocal = t0 >= 0 ? t0 : t1;
            }
        } else {
            tLocal = intersect(local, scene.localBounds(d.entity));
        }
        if (tLocal < 0) continue;
        Vec3 hit = d.model.transformPoint(local.origin + local.dir * tLocal);
        float t = distance(ray.origin, hit);
        if (t < bestT) {
            bestT = t;
            best = d.entity;
        }
    }
    return best;
}

}  // namespace sky
