// Vehicle observability: the "vehicles" debug-view overlay and telemetry as JSON.

#include <algorithm>
#include <cmath>

#include "skywalker/physics/DebugDraw.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/physics/Vehicle.h"

namespace sky::physics {

namespace {

float r2(float x) { return std::round(x * 100.f) / 100.f; }
float r3(float x) { return std::round(x * 1000.f) / 1000.f; }
Json v3(Vec3 v) { return Json::array({r3(v.x), r3(v.y), r3(v.z)}); }

}  // namespace

void drawVehicleOverlay(Image& image, const ViewCamera& camera, const PhysicsWorld& world) {
    debugdraw::Canvas canvas(image, camera);
    for (EntityId e : world.vehicleEntities()) {
        auto t = world.vehicle(e);
        if (!t) continue;
        const float wheelCount = static_cast<float>(std::max<size_t>(t->wheels.size(), 1));
        const float cornerWeight = std::max(t->mass * 9.81f / wheelCount, 1.f);  // N: a wheel's share at rest
        // Velocity (where the car goes in the next quarter second) and the center of mass.
        canvas.line(t->centerOfMass, t->centerOfMass + t->velocity * 0.25f, {60, 230, 255, 255}, 2);
        canvas.line(t->centerOfMass, t->centerOfMass + t->forward * 1.2f, {255, 255, 255, 160}, 1);
        canvas.dot(t->centerOfMass, 5.f, {255, 220, 40, 255});
        for (const WheelTelemetry& w : t->wheels) {
            // Suspension travel (gray) and its current length (white -> orange as it compresses).
            canvas.line(w.mount, w.mount + w.down * w.suspensionMax, {130, 130, 140, 200}, 1);
            uint8_t g = static_cast<uint8_t>(255.f - 155.f * w.compression);
            canvas.line(w.mount, w.center, {255, g, static_cast<uint8_t>(g / 2), 255}, 2);
            canvas.dot(w.mount, 3.f, {200, 200, 210, 255});
            // The wheel: green on the ground, gray in the air, red while sliding.
            Vec3 upAxis = normalize(cross(w.right, w.forward));
            std::vector<Vec3> ring;
            constexpr int kSegments = 28;
            for (int i = 0; i < kSegments; ++i) {
                float a0 = 2.f * kPi * static_cast<float>(i) / kSegments, a1 = 2.f * kPi * static_cast<float>(i + 1) / kSegments;
                ring.push_back(w.center + (w.forward * std::cos(a0) + upAxis * std::sin(a0)) * w.radius);
                ring.push_back(w.center + (w.forward * std::cos(a1) + upAxis * std::sin(a1)) * w.radius);
            }
            debugdraw::Rgba wheelColor = !w.contact ? debugdraw::Rgba{150, 150, 160, 220}
                                                    : debugdraw::Rgba{static_cast<uint8_t>(80 + 175 * w.skid),
                                                                      static_cast<uint8_t>(230 - 170 * w.skid), 90, 255};
            canvas.lines(ring, wheelColor, 2);
            canvas.line(w.center, w.center + w.forward * (w.radius * 1.4f), wheelColor, 1);  // where the tire points
            if (!w.contact) continue;
            // Contact point, then the tire forces at it: load (blue, along the normal), drive/brake
            // (orange, along the tire) and cornering (red, sideways). 0.6 m = the wheel's static load.
            canvas.dot(w.contactPoint, 4.f, w.skid > 0.3f ? debugdraw::Rgba{255, 60, 60, 255} : debugdraw::Rgba{90, 255, 120, 255});
            float k = 0.6f / cornerWeight;
            canvas.line(w.contactPoint, w.contactPoint + w.contactNormal * (w.load * k), {90, 140, 255, 255}, 2);
            canvas.line(w.contactPoint, w.contactPoint + w.forward * (w.longitudinalForce * k), {255, 160, 30, 255}, 2);
            canvas.line(w.contactPoint, w.contactPoint + w.right * (w.lateralForce * k), {255, 70, 70, 255}, 2);
        }
    }
}

Json telemetryJson(const Scene& scene, const VehicleTelemetry& t, bool wheels) {
    auto name = [&](EntityId e) -> Json {
        const EntityRecord* r = e ? scene.record(e) : nullptr;
        return r ? Json(r->name) : Json();
    };
    Json j = Json::object({{"entity", static_cast<int64_t>(t.entity)},
                           {"name", name(t.entity)},
                           {"speedKmh", r2(t.speedKmh)},
                           {"rpm", std::round(t.rpm)},
                           {"gear", t.gear},
                           {"clutch", r2(t.clutch)},
                           {"shifting", t.shifting},
                           {"input", Json::object({{"throttle", r2(t.input.throttle)},
                                                   {"brake", r2(t.input.brake)},
                                                   {"steer", r2(t.input.steer)},
                                                   {"handbrake", r2(t.input.handbrake)}})},
                           {"applied", Json::object({{"throttle", r2(t.applied.throttle)},
                                                     {"brake", r2(t.applied.brake)},
                                                     {"steer", r2(t.applied.steer)},
                                                     {"handbrake", r2(t.applied.handbrake)}})},
                           {"reversing", t.reversing},
                           {"assists", Json::object({{"tractionControl", t.tractionControlActive},
                                                     {"tractionScale", r2(t.tractionScale)},
                                                     {"abs", t.absActive},
                                                     {"driftAssist", t.driftAssistActive}})},
                           {"driftAngle", r2(t.bodySlipAngle)},
                           {"lateralG", r2(t.lateralG)},
                           {"longitudinalG", r2(t.longitudinalG)},
                           {"load", r2(t.load)},
                           {"wheelsOnGround", t.wheelsOnGround},
                           {"skid", r2(t.skid)},
                           {"position", v3(t.position)},
                           {"velocity", v3(t.velocity)},
                           {"forward", v3(t.forward)},
                           {"up", v3(t.up)},
                           {"mass", r2(t.mass)},
                           {"centerOfMass", v3(t.centerOfMass)},
                           {"simulated", t.stepped}});
    if (!wheels) return j;
    Json list = Json::array();
    for (const auto& w : t.wheels) {
        Json wj = Json::object({{"name", w.name},
                                {"visual", name(w.visual)},
                                {"axle", w.axle},
                                {"steers", w.steers},
                                {"driven", w.driven},
                                {"handbrake", w.handbrake},
                                {"radius", r3(w.radius)},
                                {"width", r3(w.width)},
                                {"center", v3(w.center)},
                                {"suspensionLength", r3(w.suspensionLength)},
                                {"restLength", r3(w.restLength)},
                                {"compression", r2(w.compression)},
                                {"steerAngle", r2(w.steerAngle)},
                                {"rpm", std::round(w.angularVelocity * 60.f / (2.f * kPi))},
                                {"contact", w.contact}});
        if (w.contact) {
            wj["contactPoint"] = v3(w.contactPoint);
            wj["surface"] = name(w.surface);
            wj["surfaceGrip"] = r2(w.surfaceGrip);
            wj["load"] = std::round(w.load);
            wj["longitudinalForce"] = std::round(w.longitudinalForce);
            wj["lateralForce"] = std::round(w.lateralForce);
            wj["slipRatio"] = r3(std::min(w.slipRatio, 100.f));
            wj["slipAngle"] = r2(w.slipAngle);
            wj["skid"] = r2(w.skid);
        }
        list.push(wj);
    }
    j["wheels"] = list;
    return j;
}

}  // namespace sky::physics
