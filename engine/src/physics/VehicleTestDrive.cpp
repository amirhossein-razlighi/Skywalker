// vehicle_test_drive: an autopilot drives a copy of the vehicle through standard maneuvers in a
// private physics world (the scene is never changed) and measures the handling, so agents can
// tune by numbers: 0-100 km/h, braking distance, slalom speed, skidpad lateral g, top speed.
//
// The proving ground is an endless flat surface with full grip; `track: "scene"` drives in the
// level as it is. Every maneuver starts from a fresh world, so results are repeatable.

#include "JoltCommon.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Components.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/physics/Vehicle.h"

namespace sky::physics {

namespace {

constexpr float kDt = 1.f / 60.f;
constexpr float kG = 9.81f;

float r2(float x) { return std::round(x * 100.f) / 100.f; }

Vec3 flatten(Vec3 v) { return {v.x, 0.f, v.z}; }

const std::vector<std::string>& maneuvers() {
    static const std::vector<std::string> m{"accel", "braking", "slalom", "skidpad", "top_speed", "custom", "all"};
    return m;
}

struct Sandbox {
    std::unique_ptr<Scene> scene;
    std::unique_ptr<PhysicsWorld> world;
    EntityId car = kNoEntity;
};

/// A private copy of the vehicle (and, for track "scene", of the whole level) with its own world.
Result<Sandbox> makeSandbox(const PhysicsSystem& physics, const Scene& src, EntityId vehicle, const TestDriveOptions& o) {
    Sandbox sb;
    sb.scene = std::make_unique<Scene>();
    Scene& s = *sb.scene;
    s.assetBounds = src.assetBounds;
    const bool proving = o.track == "proving_ground";
    if (proving) {
        std::unordered_map<EntityId, EntityId> map;
        auto roots = s.cloneTrees(src, {vehicle}, kNoEntity, &map);
        if (roots.empty()) return Error::make("internal", "could not copy the vehicle");
        sb.car = roots.front();
        Decomposed d = decompose(src.worldMatrix(vehicle));
        Transform* t = s.get<Transform>(sb.car);
        t->position = {0.f, 0.f, 0.f};
        t->rotation = {0.f, 0.f, 0.f};
        t->scale = d.scale;
        EntityId ground = s.create("Proving Ground");
        Transform& gt = s.add<Transform>(ground);
        gt.position = {0.f, -0.5f, 0.f};
        gt.scale = {6000.f, 1.f, 6000.f};
        if (Status st = s.patchComponent(ground, "collider", Json::object({{"shape", "box"}, {"friction", 1.0}})); !st) return st.error();
    } else {
        if (Status st = s.loadJson(src.toJson()); !st) return st.error();
        sb.car = vehicle;
        if (!s.exists(sb.car)) return Error::make("internal", "the vehicle is missing from the scene copy");
    }
    Json patch = Json::object({{"control", "script"}, {"throttle", 0}, {"brake", 0}, {"steer", 0}, {"handbrake", 0}});
    if (o.overrides.isObject()) {
        for (const auto& [k, v] : o.overrides.members()) patch[k] = v;
    }
    if (Status st = s.patchComponent(sb.car, "vehicle", patch); !st) return st.error();
    if (Status st = s.patchComponent(sb.car, "body", Json::object({{"velocity", Json::array({0, 0, 0})},
                                                                   {"angularVelocity", Json::array({0, 0, 0})}}));
        !st) {
        return st.error();
    }
    WorldOptions wo;
    wo.multithreaded = false;
    wo.writeBack = false;
    sb.world = physics.makeWorld(std::move(wo));
    sb.world->sync(s, 0.f);
    if (proving) {
        // Stand the car on the ground: the lowest wheel bottom at y = 0 (rest pose).
        auto t = sb.world->vehicle(sb.car);
        if (!t) {
            std::string why;
            for (const auto& w : sb.world->drainWarnings()) why += (why.empty() ? "" : "; ") + w;
            return Error::make("not_a_vehicle", "the vehicle could not be simulated" + (why.empty() ? std::string() : ": " + why),
                               "it needs a dynamic body and wheels (children named wheel*, or `wheels`); try vehicle_create");
        }
        float bottom = 1e30f;
        for (const auto& w : t->wheels) bottom = std::min(bottom, w.center.y - w.radius);
        if (bottom < 1e29f) s.get<Transform>(sb.car)->position.y = -bottom + 0.01f;
        sb.world->sync(s, 0.f);
    }
    if (!sb.world->vehicle(sb.car)) return Error::make("not_a_vehicle", "the vehicle could not be simulated");
    return sb;
}

/// Drives the sandboxed car one tick at a time and keeps running statistics.
struct Driver {
    Sandbox& sb;
    float time = 0.f;
    VehicleTelemetry t;
    float maxLatG = 0.f, maxSpeed = 0.f, maxDrift = 0.f;
    int maxGear = 0;
    float wheelBase = 2.5f, maxSteerDeg = 30.f, halfWidth = 0.9f, sss = 0.5f;
    Json trace = Json::array();
    bool tracing = false;
    float nextSample = 0.f;

    explicit Driver(Sandbox& s, bool traceOn) : sb(s), tracing(traceOn) {
        t = *sb.world->vehicle(sb.car);
        float zMin = 1e30f, zMax = -1e30f, xMax = 0.f;
        Mat4 inv = sb.scene->worldMatrix(sb.car).inverse();
        for (const auto& w : t.wheels) {
            Vec3 local = inv.transformPoint(w.center);
            zMin = std::min(zMin, local.z);
            zMax = std::max(zMax, local.z);
            xMax = std::max(xMax, std::fabs(local.x) + w.width * 0.5f);
        }
        wheelBase = std::max(zMax - zMin, 0.5f);
        halfWidth = std::max(xMax, 0.3f);
        if (const Vehicle* v = sb.scene->get<Vehicle>(sb.car)) {
            maxSteerDeg = std::max(v->maxSteerAngle, 1.f);
            sss = v->speedSensitiveSteering;
        }
    }

    const VehicleTelemetry& step(VehicleInput in) {
        sb.world->setVehicleInput(sb.car, in);
        sb.world->step(*sb.scene, kDt);
        time += kDt;
        t = *sb.world->vehicle(sb.car);
        maxLatG = std::max(maxLatG, std::fabs(t.lateralG));
        maxSpeed = std::max(maxSpeed, t.speedKmh);
        maxDrift = std::max(maxDrift, std::fabs(t.bodySlipAngle));
        maxGear = std::max(maxGear, t.gear);
        if (tracing && time >= nextSample) {
            nextSample += 0.25f;
            trace.push(Json::object({{"t", r2(time)},
                                     {"x", r2(t.position.x)},
                                     {"z", r2(t.position.z)},
                                     {"speedKmh", r2(t.speedKmh)},
                                     {"gear", t.gear},
                                     {"rpm", std::round(t.rpm)},
                                     {"throttle", r2(t.applied.throttle)},
                                     {"brake", r2(t.applied.brake)},
                                     {"steer", r2(t.applied.steer)},
                                     {"lateralG", r2(t.lateralG)},
                                     {"driftAngle", r2(t.bodySlipAngle)},
                                     {"skid", r2(t.skid)}}));
        }
        return t;
    }

    /// Steering input that drives the car through world point `target` (pure pursuit).
    float steerToward(Vec3 target) const {
        Vec3 d = flatten(target - t.position);
        float dist2 = std::max(dot(d, d), 1.f);
        float lateral = dot(d, normalize(flatten(t.right)));
        float curvature = 2.f * lateral / dist2;
        float angle = degrees(std::atan(curvature * wheelBase));
        float speed = length(flatten(t.velocity));
        float lock = maxSteerDeg * (1.f - std::clamp(sss, 0.f, 1.f) * 0.65f * std::clamp(speed / 35.f, 0.f, 1.f));
        return std::clamp(angle / std::max(lock, 1.f), -1.f, 1.f);
    }

    float lookahead() const { return std::clamp(0.35f * length(t.velocity) + 4.f, 5.f, 30.f); }

    /// Throttle/brake to hold `kmh`.
    VehicleInput holdSpeed(float kmh) const {
        VehicleInput in;
        float err = kmh - t.speedKmh;
        in.throttle = std::clamp(err * 0.12f + 0.15f, 0.f, 1.f);
        if (err < -4.f) {
            in.throttle = 0.f;
            in.brake = std::clamp(-err * 0.04f, 0.f, 1.f);
        }
        return in;
    }

    bool spun() const {
        return std::fabs(t.bodySlipAngle) > 70.f || t.up.y < 0.3f || (t.speedKmh < -5.f && time > 1.f);
    }
};

/// Lets the car settle on its springs (half a second without input).
Json settle(Driver& d, std::vector<std::string>& warnings) {
    for (int i = 0; i < 30; ++i) d.step({});
    if (d.t.wheelsOnGround == 0) warnings.push_back("no wheel touches the ground after settling: check the wheel radius/positions");
    float height = 0.f;
    for (const auto& w : d.t.wheels) height += w.compression;
    return Json::object({{"wheelsOnGround", d.t.wheelsOnGround},
                         {"avgCompression", r2(d.t.wheels.empty() ? 0.f : height / static_cast<float>(d.t.wheels.size()))}});
}

Json finish(Driver& d, Json metrics, std::vector<std::string>& warnings, bool trace) {
    metrics["maxLateralG"] = r2(d.maxLatG);
    metrics["maxSpeedKmh"] = r2(d.maxSpeed);
    metrics["maxDriftAngle"] = r2(d.maxDrift);
    metrics["seconds"] = r2(d.time);
    Json w = Json::array();
    for (const auto& s : warnings) w.push(s);
    metrics["warnings"] = w;
    if (trace) metrics["trace"] = d.trace;
    return metrics;
}

Json runAccel(Driver& d, std::vector<std::string>& warnings) {
    float t60 = -1, t100 = -1, t400m = -1, speed400m = 0, maxSlip = 0;
    int shifts = 0, lastGear = d.t.gear;
    const float start = d.time;
    const Vec3 origin = d.t.position;
    while (d.time - start < 40.f) {
        VehicleInput in;
        in.throttle = 1.f;
        Vec3 ahead{origin.x, 0.f, d.t.position.z - d.lookahead()};
        in.steer = d.steerToward(ahead);
        d.step(in);
        float elapsed = d.time - start;
        for (const auto& w : d.t.wheels) {
            if (w.driven && w.contact) maxSlip = std::max(maxSlip, std::min(w.slipRatio, 10.f));
        }
        if (d.t.gear != lastGear && d.t.gear > 0) ++shifts;
        lastGear = d.t.gear;
        if (t60 < 0 && d.t.speedKmh >= 60.f) t60 = elapsed;
        if (t100 < 0 && d.t.speedKmh >= 100.f) t100 = elapsed;
        if (t400m < 0 && length(flatten(d.t.position - origin)) >= 402.f) {
            t400m = elapsed;
            speed400m = d.t.speedKmh;
        }
        if (t100 >= 0 && t400m >= 0) break;
        if (d.spun()) {
            warnings.push_back("lost control under full throttle (spun or flipped)");
            break;
        }
    }
    if (t100 < 0) warnings.push_back("never reached 100 km/h in 40 s (max " + std::to_string(static_cast<int>(d.maxSpeed)) + " km/h)");
    return Json::object({{"zeroTo60s", t60 < 0 ? Json() : Json(r2(t60))},
                         {"zeroTo100s", t100 < 0 ? Json() : Json(r2(t100))},
                         {"quarterMileS", t400m < 0 ? Json() : Json(r2(t400m))},
                         {"quarterMileKmh", t400m < 0 ? Json() : Json(r2(speed400m))},
                         {"upshifts", shifts},
                         {"launchWheelspin", r2(maxSlip)}});
}

Json runBraking(Driver& d, float fromKmh, std::vector<std::string>& warnings) {
    const Vec3 origin = d.t.position;
    auto lineTarget = [&] { return Vec3{origin.x, 0.f, d.t.position.z - d.lookahead()}; };
    float runup = 0.f;
    while (d.t.speedKmh < fromKmh && runup < 45.f) {
        VehicleInput in;
        in.throttle = 1.f;
        in.steer = d.steerToward(lineTarget());
        d.step(in);
        runup += kDt;
        if (d.spun()) break;
    }
    const float v0 = d.t.speedKmh;
    if (v0 < fromKmh - 1.f) warnings.push_back("only reached " + std::to_string(static_cast<int>(v0)) + " km/h before braking");
    const Vec3 brakeStart = d.t.position;
    const float tStart = d.time;
    float maxDecel = 0.f, absTime = 0.f, maxDeviation = 0.f;
    bool locked = false;
    while (d.t.speedKmh > 0.5f && d.time - tStart < 20.f) {
        VehicleInput in;
        in.brake = 1.f;
        in.steer = d.steerToward(lineTarget());
        d.step(in);
        maxDecel = std::max(maxDecel, -d.t.longitudinalG);
        absTime += d.t.absActive ? kDt : 0.f;
        maxDeviation = std::max(maxDeviation, std::fabs(d.t.position.x - origin.x));
        for (const auto& w : d.t.wheels) locked = locked || (w.contact && w.angularVelocity == 0.f && d.t.speedKmh > 5.f);
        if (d.spun()) {
            warnings.push_back("unstable under braking (spun)");
            break;
        }
    }
    float distance = length(flatten(d.t.position - brakeStart));
    float v = v0 / 3.6f;
    float meanG = distance > 0.1f ? v * v / (2.f * distance) / kG : 0.f;
    if (maxDeviation > 1.5f) warnings.push_back("pulled " + std::to_string(maxDeviation).substr(0, 4) + " m off line while braking");
    return Json::object({{"fromKmh", r2(v0)},
                         {"distanceM", r2(distance)},
                         {"seconds", r2(d.time - tStart)},
                         {"meanDecelG", r2(meanG)},
                         {"peakDecelG", r2(maxDecel)},
                         {"absActiveFraction", r2(absTime / std::max(d.time - tStart, kDt))},
                         {"wheelsLocked", locked},
                         {"maxDeviationM", r2(maxDeviation)}});
}

Json runSlalom(Driver& d, float kmh, float spacing, std::vector<std::string>& warnings) {
    const int cones = 8;
    const float amplitude = std::max(1.6f, d.halfWidth + 0.75f);
    const Vec3 origin = d.t.position;
    // Distance along -Z where the first cone stands: enough run-up to reach the speed.
    const float runup = std::max(80.f, (kmh / 3.6f) * (kmh / 3.6f) / 4.f + 40.f);
    const float first = runup;
    const float last = first + spacing * static_cast<float>(cones - 1);
    auto pathX = [&](float s) {
        float x = 0.f;
        if (s <= first - spacing) {
            x = 0.f;
        } else if (s < first) {
            x = -amplitude * 0.5f * (1.f - std::cos(kPi * (s - (first - spacing)) / spacing));
        } else if (s <= last) {
            x = -amplitude * std::cos(kPi * (s - first) / spacing);
        } else if (s < last + spacing) {
            float end = -amplitude * std::cos(kPi * (last - first) / spacing);
            x = end * 0.5f * (1.f + std::cos(kPi * (s - last) / spacing));
        }
        return origin.x + x;
    };
    float maxErr = 0.f, tEnter = -1.f, tExit = -1.f, minSpeed = 1e9f;
    int hits = 0;
    std::vector<bool> passed(static_cast<size_t>(cones), false);
    bool completed = false;
    while (d.time < 90.f) {
        float s = origin.z - d.t.position.z;
        float sa = s + d.lookahead();
        VehicleInput in = d.holdSpeed(kmh);
        in.steer = d.steerToward(Vec3{pathX(sa), 0.f, origin.z - sa});
        d.step(in);
        s = origin.z - d.t.position.z;
        if (s >= first - spacing * 0.5f && s <= last + spacing * 0.5f) {
            if (tEnter < 0) tEnter = d.time;
            maxErr = std::max(maxErr, std::fabs(d.t.position.x - pathX(s)));
            minSpeed = std::min(minSpeed, d.t.speedKmh);
        }
        for (int i = 0; i < cones; ++i) {
            float coneS = first + spacing * static_cast<float>(i);
            if (!passed[static_cast<size_t>(i)] && s >= coneS) {
                passed[static_cast<size_t>(i)] = true;
                if (std::fabs(d.t.position.x - origin.x) < d.halfWidth + 0.15f) ++hits;
            }
        }
        if (s > last + spacing * 0.5f) {
            tExit = d.time;
            completed = true;
            break;
        }
        if (d.spun()) {
            warnings.push_back("spun out in the slalom");
            break;
        }
    }
    float through = tEnter >= 0 && tExit > tEnter ? tExit - tEnter : 0.f;
    float avg = through > 0 ? (last - first + spacing) / through * 3.6f : 0.f;
    if (hits > 0) warnings.push_back(std::to_string(hits) + " cone(s) hit: the car cannot follow the line at this speed");
    return Json::object({{"targetKmh", r2(kmh)},
                         {"cones", cones},
                         {"spacingM", r2(spacing)},
                         {"completed", completed},
                         {"conesHit", hits},
                         {"avgKmh", r2(avg)},
                         {"minKmh", minSpeed < 1e8f ? r2(minSpeed) : Json()},
                         {"maxLineErrorM", r2(maxErr)}});
}

Json runSkidpad(Driver& d, float radius, float startKmh, std::vector<std::string>& warnings) {
    const Vec3 origin = d.t.position;
    // Counter-clockwise circle (seen from above) tangent to the start heading (-Z), center on the left.
    const Vec3 center = origin + Vec3{-radius, 0.f, 0.f};
    float bestG = 0.f, bestKmh = 0.f, measuredG = 0.f;
    std::string limit = "none";
    float target = startKmh;
    const float t0 = d.time;
    while (d.time - t0 < 75.f) {
        Vec3 rel = flatten(d.t.position - center);
        float angle = std::atan2(rel.z, rel.x);
        float ahead = angle - d.lookahead() / radius;  // counter-clockwise from above: decreasing atan2(z, x)
        Vec3 goal = center + Vec3{std::cos(ahead) * radius, 0.f, std::sin(ahead) * radius};
        if (d.time - t0 > 4.f) target += 1.2f * kDt;  // ramp the speed: +1.2 km/h per second
        VehicleInput in = d.holdSpeed(target);
        in.steer = d.steerToward(goal);
        d.step(in);
        float err = length(flatten(d.t.position - center)) - radius;
        if (d.time - t0 > 4.f && std::fabs(err) < 1.5f && std::fabs(target - d.t.speedKmh) < 4.f) {
            float v = d.t.speedKmh / 3.6f;
            float g = v * v / radius / kG;
            if (g > bestG) {
                bestG = g;
                bestKmh = d.t.speedKmh;
                measuredG = std::fabs(d.t.lateralG);
            }
        }
        if (d.spun()) {
            limit = "oversteer (spun)";
            break;
        }
        if (err > 3.f) {
            limit = "understeer (ran wide)";
            break;
        }
        if (err < -3.f) {
            limit = "oversteer (tucked in)";
            break;
        }
        if (d.time - t0 > 4.f && std::fabs(target - d.t.speedKmh) > 8.f && d.t.applied.throttle > 0.95f) {
            limit = "power (cannot go faster on this circle)";
            break;
        }
    }
    if (bestG <= 0.f) warnings.push_back("could not hold the skidpad circle at any speed");
    return Json::object({{"radiusM", r2(radius)},
                         {"lateralG", r2(bestG)},
                         {"measuredLateralG", r2(measuredG)},
                         {"speedKmh", r2(bestKmh)},
                         {"limit", limit}});
}

Json runTopSpeed(Driver& d, float seconds, std::vector<std::string>& warnings) {
    const Vec3 origin = d.t.position;
    float top = 0.f, tTop = 0.f;
    const float t0 = d.time;
    while (d.time - t0 < seconds) {
        VehicleInput in;
        in.throttle = 1.f;
        in.steer = d.steerToward(Vec3{origin.x, 0.f, d.t.position.z - d.lookahead()});
        d.step(in);
        if (d.t.speedKmh > top + 0.5f) {
            top = d.t.speedKmh;
            tTop = d.time - t0;
        }
        if (d.spun()) {
            warnings.push_back("lost control at speed");
            break;
        }
    }
    if (tTop > seconds - 3.f) warnings.push_back("still accelerating at the end: run longer (duration) for the true top speed");
    return Json::object({{"topSpeedKmh", r2(top)}, {"reachedAfterS", r2(tTop)}});
}

Json runCustom(Driver& d, const Json& keys, float duration, std::vector<std::string>& warnings) {
    struct Key {
        float t = 0;
        VehicleInput in;
    };
    std::vector<Key> list;
    VehicleInput cur;
    for (const auto& k : keys.elements()) {
        Key key;
        key.t = k.get("t").asFloat(0.f);
        cur.throttle = k.get("throttle").asFloat(cur.throttle);
        cur.brake = k.get("brake").asFloat(cur.brake);
        cur.steer = k.get("steer").asFloat(cur.steer);
        cur.handbrake = k.get("handbrake").asFloat(cur.handbrake);
        key.in = cur;
        list.push_back(key);
    }
    std::stable_sort(list.begin(), list.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    if (duration <= 0) duration = (list.empty() ? 0.f : list.back().t) + 3.f;
    duration = std::min(duration, 120.f);
    const Vec3 origin = d.t.position;
    const float t0 = d.time;
    while (d.time - t0 < duration) {
        float now = d.time - t0;
        VehicleInput in;
        for (const auto& k : list) {
            if (k.t <= now) in = k.in;
        }
        d.step(in);
    }
    if (list.empty()) warnings.push_back("no inputs given: the car stood still");
    return Json::object({{"distanceM", r2(length(flatten(d.t.position - origin)))},
                         {"finalSpeedKmh", r2(d.t.speedKmh)},
                         {"finalPosition", Json::array({r2(d.t.position.x), r2(d.t.position.y), r2(d.t.position.z)})},
                         {"finalHeadingDeg", r2(degrees(std::atan2(-d.t.forward.x, -d.t.forward.z)))}});
}

}  // namespace

Result<Json> runTestDrive(const PhysicsSystem& physics, const Scene& scene, EntityId vehicle, const TestDriveOptions& o) {
    if (!scene.get<Vehicle>(vehicle)) {
        return Error::make("not_a_vehicle", "entity has no vehicle component", "build one with vehicle_create");
    }
    if (std::find(maneuvers().begin(), maneuvers().end(), o.maneuver) == maneuvers().end()) {
        std::string guess = str::closest(o.maneuver, maneuvers(), 3);
        return Error::make("invalid_maneuver", "unknown maneuver \"" + o.maneuver + "\"",
                           (guess.empty() ? std::string() : "did you mean \"" + guess + "\"? ") +
                               "maneuvers: accel, braking, slalom, skidpad, top_speed, custom, all");
    }
    if (o.track != "proving_ground" && o.track != "scene") {
        return Error::make("invalid_track", "unknown track \"" + o.track + "\"", "tracks: proving_ground, scene");
    }
    std::vector<std::string> list = o.maneuver == "all" ? std::vector<std::string>{"accel", "braking", "slalom", "skidpad"}
                                                        : std::vector<std::string>{o.maneuver};
    Json out = Json::object();
    Json summary = Json::object();
    for (const auto& m : list) {
        auto sb = makeSandbox(physics, scene, vehicle, o);
        if (!sb) return sb.error();
        Driver d(*sb, o.trace);
        std::vector<std::string> warnings;
        Json rest = settle(d, warnings);
        Json r;
        if (m == "accel") {
            r = runAccel(d, warnings);
            summary["zeroTo100s"] = r.get("zeroTo100s");
        } else if (m == "braking") {
            r = runBraking(d, o.speedKmh > 0 ? o.speedKmh : 100.f, warnings);
            summary["brakingDistanceM"] = r.get("distanceM");
            summary["brakingFromKmh"] = r.get("fromKmh");
        } else if (m == "slalom") {
            r = runSlalom(d, o.speedKmh > 0 ? o.speedKmh : 60.f, std::clamp(o.coneSpacing, 8.f, 60.f), warnings);
            summary["slalomAvgKmh"] = r.get("avgKmh");
            summary["slalomConesHit"] = r.get("conesHit");
        } else if (m == "skidpad") {
            r = runSkidpad(d, std::clamp(o.radius, 10.f, 200.f), o.speedKmh > 0 ? o.speedKmh : 30.f, warnings);
            summary["skidpadLateralG"] = r.get("lateralG");
        } else if (m == "top_speed") {
            r = runTopSpeed(d, o.duration > 0 ? std::min(o.duration, 120.f) : 45.f, warnings);
            summary["topSpeedKmh"] = r.get("topSpeedKmh");
        } else {
            r = runCustom(d, o.inputs, o.duration, warnings);
        }
        r["rest"] = rest;
        r = finish(d, r, warnings, o.trace);
        float g = r.get("maxLateralG").asFloat(0.f);
        summary["maxLateralG"] = std::max(summary.get("maxLateralG").asFloat(0.f), g);
        out[m] = r;
    }
    out["summary"] = summary;
    out["track"] = o.track;
    return out;
}

}  // namespace sky::physics
