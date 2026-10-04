#include <algorithm>
#include <cmath>

#include "skywalker/render/ReflectionProbes.h"
#include "skywalker/render/RenderLayers.h"
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

void ViewCamera::lookFrom(Vec3 e, Vec3 t) {
    eye = e;
    target = t;
    nearPlane = std::clamp(distance(e, t) * 0.005f, 0.02f, 0.25f);
    farPlane = std::max(farPlane, 40000.f);
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
    out.aperture = c->aperture;
    out.focusDistance = c->focusDistance;
    out.motionBlur = c->motionBlur;
    out.tiltShift = c->tiltShift;
    out.cullMask = static_cast<uint32_t>(c->cullMask) & render::kAllLayers;
    return true;
}

Shading shadingFromString(std::string_view s) {
    if (s == "toon") return Shading::Toon;
    if (s == "unlit") return Shading::Unlit;
    if (s == "water") return Shading::Water;
    return Shading::Pbr;
}

void prioritizeLights(FrameData& f) {
    // Directional lights first, then point/spot lights whose influence sphere is closest to
    // what the camera looks at (stable order for ties). Effects use the first kMaxEffectLights.
    auto score = [&](const LightItem& l) {
        if (l.kind == LightItem::Kind::Directional) return -1e30f;
        float d = std::min(distance(l.position, f.camera.target), distance(l.position, f.camera.eye));
        return std::max(0.f, d - l.range) - l.range * 0.05f * std::min(std::fabs(l.intensity), 10.f);
    };
    std::stable_sort(f.lights.begin(), f.lights.end(), [&](const LightItem& a, const LightItem& b) { return score(a) < score(b); });
    if (f.lights.size() > FrameData::kMaxLights) f.lights.resize(FrameData::kMaxLights);
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
        if (const MeshRenderer* m = scene.get<MeshRenderer>(e);
            m && m->visible && (static_cast<uint32_t>(m->layers) & camera.cullMask & render::kAllLayers) != 0) {
            DrawItem d;
            d.entity = e;
            d.layers = static_cast<uint32_t>(m->layers) & render::kAllLayers;
            d.mesh = m->mesh;
            d.castShadows = m->castShadows;
            Surface& s = d.surface;
            s.color = m->color;
            s.emissive = m->emissive;
            s.metallic = m->metallic;
            s.roughness = m->roughness;
            s.texture = m->texture;
            s.normalMap = m->normalMap;
            s.ormMap = m->ormMap;
            s.emissiveMap = m->emissiveMap;
            s.tiling = {m->tiling, m->tiling};
            s.normalStrength = m->normalStrength;
            s.triplanar = m->triplanar;
            s.shading = m->unlit ? Shading::Unlit : shadingFromString(m->shading);
            s.clearcoat = m->clearcoat;
            s.subsurface = m->subsurface;
            s.rim = m->rim;
            s.outline = m->outline;
            s.outlineColor = m->outlineColor;
            s.doubleSided = m->doubleSided;
            s.alphaCutoff = m->alphaCutoff;
            if (!m->material.empty() && opts.material) {
                if (const ResolvedMaterial* mat = opts.material(m->material)) {
                    s = *mat;
                    if (m->unlit) s.shading = Shading::Unlit;
                    s.doubleSided = s.doubleSided || m->doubleSided;
                    if (s.alphaCutoff <= 0.f) s.alphaCutoff = m->alphaCutoff;
                    if (m->outline > 0.f && s.outline <= 0.f) {
                        s.outline = m->outline;
                        s.outlineColor = m->outlineColor;
                    }
                }
            }
            d.model = world;
            if (m->billboard) {
                Vec3 p = world.translation();
                Vec3 sc{length(world.transformDir({1, 0, 0})), length(world.transformDir({0, 1, 0})),
                       length(world.transformDir({0, 0, 1}))};
                Vec3 d2c = camera.eye - p;
                float horiz = std::sqrt(d2c.x * d2c.x + d2c.z * d2c.z);
                float yaw = degrees(std::atan2(d2c.x, d2c.z));
                float pitch = degrees(std::atan2(d2c.y, horiz));
                d.model = Mat4::trs(p, {-pitch, yaw, 0}, sc);
            }
            d.selected = opts.editorOverlays &&
                         std::find(opts.selection.begin(), opts.selection.end(), e) != opts.selection.end();
            d.worldBounds = scene.localBounds(e).transformed(d.model);
            // Animation: skinned meshes draw a per-instance posed copy (see SkinItem).
            if (opts.skin && !m->billboard) {
                if (const SkinPose* sp = opts.skin(e, m->mesh); sp && sp->palette) {
                    d.skin = static_cast<int>(f.skins.size());
                    f.skins.push_back({m->mesh, m->mesh + "@skin" + std::to_string(e), sp->palette});
                    d.mesh = f.skins.back().key;
                    d.worldBounds = sp->bounds.transformed(d.model);
                }
            }
            f.draws.push_back(std::move(d));
        }
        if (const FluidVolume* v = scene.get<FluidVolume>(e)) {
            VolumeItem vi;
            vi.entity = e;
            Vec3 scl{length(world.transformDir({1, 0, 0})), length(world.transformDir({0, 1, 0})), length(world.transformDir({0, 0, 1}))};
            // Keep position and rotation; the entity's scale multiplies the box size.
            vi.model = world * Mat4::scale({1.f / std::max(scl.x, 1e-6f), 1.f / std::max(scl.y, 1e-6f), 1.f / std::max(scl.z, 1e-6f)});
            vi.params = *v;
            vi.params.size = {v->size.x * scl.x, v->size.y * scl.y, v->size.z * scl.z};
            float wa = radians(scene.environment().windDirection);
            vi.wind = Vec3{std::sin(wa), 0.f, std::cos(wa)} * (scene.environment().windSpeed * v->wind);
            f.volumes.push_back(vi);
            if (v->light > 0.f && (v->emitting || v->burst > 0.f)) {
                // The fire lights its surroundings; it flickers like the flames above it.
                float t = opts.time + static_cast<float>(e) * 1.3f;
                float flicker = 0.82f + 0.1f * std::sin(t * 11.3f) + 0.06f * std::sin(t * 23.7f + 1.f) + 0.04f * std::sin(t * 41.f);
                LightItem li;
                li.kind = LightItem::Kind::Point;
                li.position = world.transformPoint(v->sourceOffset + Vec3{0, v->size.y * 0.18f, 0});
                li.color = v->lightColor.xyz();
                li.intensity = v->light * flicker;
                li.range = v->lightRange;
                li.id = lightId(e, 1);
                li.shadows = v->lightShadows;
                f.lights.push_back(li);
            }
        }
        if (const Light* l = scene.get<Light>(e); l && l->intensity > 0.f && (static_cast<uint32_t>(l->cullMask) & render::kAllLayers) != 0) {
            LightItem li;
            li.kind = l->kind == "directional" ? LightItem::Kind::Directional
                      : l->kind == "spot"      ? LightItem::Kind::Spot
                                               : LightItem::Kind::Point;
            li.position = world.translation();
            li.direction = normalize(world.transformDir({0, 0, -1}));
            li.color = l->color.xyz();
            if (l->temperature > 0.f) {
                const Vec3 k = render::kelvinToRgb(l->temperature);
                li.color = {li.color.x * k.x, li.color.y * k.y, li.color.z * k.z};
            }
            li.intensity = l->intensity;
            if (l->distanceFade && li.kind != LightItem::Kind::Directional) {
                li.intensity *= render::lightDistanceFade(distance(camera.eye, li.position), l->fadeBegin, l->fadeLength);
                if (li.intensity <= 0.f) continue;  // faded out: not sent to the GPU at all
            }
            li.range = l->range;
            li.cosCone = std::cos(radians(l->spotAngle));
            li.mask = static_cast<uint32_t>(l->cullMask) & render::kAllLayers;
            li.specular = l->specular;
            li.indirect = l->indirect;
            li.volumetric = l->volumetric;
            li.cosInner = l->innerAngle > 0.f ? std::cos(radians(std::min(l->innerAngle, l->spotAngle - 0.1f))) : 0.f;
            li.inverseSquare = l->attenuation == "inverse_square";
            li.size = std::max(l->size, 0.001f);
            li.negative = l->negative;
            li.id = lightId(e, 0);
            li.shadows = l->castShadows;
            li.shadowMode = l->shadowMode == "dual_paraboloid" ? 1 : 0;
            li.shadowBias = l->shadowBias;
            li.shadowNormalBias = l->shadowNormalBias;
            li.shadowResolution = l->shadowResolution;
            li.shadowMaxDistance = l->shadowMaxDistance;
            f.lights.push_back(li);
        }
        if (const ReflectionProbe* rp = scene.get<ReflectionProbe>(e)) f.probes.push_back(probes::makeItem(e, world, *rp));
    }
    prioritizeLights(f);
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
        } else if (d.skin >= 0) {
            tLocal = intersect(ray, d.worldBounds) >= 0 ? intersect(local, d.worldBounds.transformed(inv)) : -1.f;  // posed bounds
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
