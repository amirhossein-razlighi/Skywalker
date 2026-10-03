#include "JoltCommon.h"

#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/physics/PhysicsWorld.h"

namespace sky::physics {

namespace {

void joltTrace(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log::info("physics", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    log::error("physics", std::string("Jolt assert: ") + expression + (message ? std::string(" (") + message + ")" : "") +
                              " at " + file + ":" + std::to_string(line));
    return true;  // break into the debugger / abort: asserts flag engine bugs, not user errors
}
#endif

}  // namespace

void ensureJoltInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = joltTrace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = joltAssertFailed;
#endif
        // Never destroyed: worlds can outlive static destruction order in tools/tests.
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::JobSystem& sharedJobSystem() {
    ensureJoltInitialized();
    static JPH::JobSystemThreadPool pool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                         static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 6u)) - 1);
    return pool;
}

JPH::Quat eulerToQuat(Vec3 deg) {
    JPH::Quat qy = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), radians(deg.y));
    JPH::Quat qx = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), radians(deg.x));
    JPH::Quat qz = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), radians(deg.z));
    return (qy * qx * qz).Normalized();
}

Vec3 quatToEuler(JPH::QuatArg q) {
    // R = Ry * Rx * Rz  =>  R(1,2) = -sin(pitch), R(1,0) = cos(p) sin(roll), R(1,1) = cos(p) cos(roll),
    //                       R(0,2) = sin(yaw) cos(p), R(2,2) = cos(yaw) cos(p).
    JPH::Mat44 r = JPH::Mat44::sRotation(q.Normalized());
    float sp = std::clamp(-r(1, 2), -1.f, 1.f);
    float pitch = std::asin(sp);
    float yaw, roll;
    if (std::fabs(sp) < 0.99999f) {
        yaw = std::atan2(r(0, 2), r(2, 2));
        roll = std::atan2(r(1, 0), r(1, 1));
    } else {  // gimbal lock: fold roll into yaw
        roll = 0.f;
        yaw = std::atan2(-r(2, 0), r(0, 0));
    }
    auto clean = [](float d) {
        float v = degrees(d);
        if (std::fabs(v) < 1e-4f) v = 0.f;  // avoid -0 and denormal noise in the scene file
        return v;
    };
    return {clean(pitch), clean(yaw), clean(roll)};
}

Decomposed decompose(const Mat4& m) {
    Decomposed d;
    d.translation = m.translation();
    Vec3 c0{m.at(0, 0), m.at(0, 1), m.at(0, 2)};
    Vec3 c1{m.at(1, 0), m.at(1, 1), m.at(1, 2)};
    Vec3 c2{m.at(2, 0), m.at(2, 1), m.at(2, 2)};
    d.scale = {length(c0), length(c1), length(c2)};
    if (dot(cross(c0, c1), c2) < 0) d.scale.x = -d.scale.x;  // mirrored
    auto safe = [](float s) { return std::fabs(s) > 1e-12f ? s : 1e-12f; };
    c0 = c0 / safe(d.scale.x);
    c1 = c1 / safe(d.scale.y);
    c2 = c2 / safe(d.scale.z);
    // Gram-Schmidt so slight shear or float noise still yields a valid rotation.
    c0 = normalize(c0);
    c1 = normalize(c1 - c0 * dot(c0, c1));
    c2 = cross(c0, c1);
    if (length(c0) < 0.5f || length(c1) < 0.5f) {
        d.rotation = JPH::Quat::sIdentity();
        return d;
    }
    JPH::Mat44 r(JPH::Vec4(c0.x, c0.y, c0.z, 0), JPH::Vec4(c1.x, c1.y, c1.z, 0), JPH::Vec4(c2.x, c2.y, c2.z, 0),
                 JPH::Vec4(0, 0, 0, 1));
    d.rotation = r.GetQuaternion().Normalized();
    return d;
}

Mat4 rigidMatrix(Vec3 position, JPH::QuatArg rotation) {
    JPH::Mat44 r = JPH::Mat44::sRotation(rotation);
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) m.at(c, row) = r(row, c);
    }
    m.at(3, 0) = position.x;
    m.at(3, 1) = position.y;
    m.at(3, 2) = position.z;
    return m;
}

const std::array<std::string_view, kLayerCount>& layerNames() {
    static const std::array<std::string_view, kLayerCount> names{"default", "static", "player", "enemy",
                                                                 "projectile", "trigger", "debris"};
    return names;
}

const std::vector<std::string>& layerNameList() {
    static const std::vector<std::string> names(layerNames().begin(), layerNames().end());
    return names;
}

JPH::ObjectLayer layerIndex(std::string_view name, JPH::ObjectLayer fallback) {
    const auto& names = layerNames();
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return static_cast<JPH::ObjectLayer>(i);
    }
    return fallback;
}

LayerMatrix::LayerMatrix() {
    for (auto& row : collide_) row.fill(true);
}

std::vector<std::string> LayerMatrix::setIgnoredPairs(std::string_view spec) {
    for (auto& row : collide_) row.fill(true);
    std::vector<std::string> problems;
    for (const std::string& raw : str::split(std::string(spec), ',')) {
        std::string pair = str::trim(raw);
        if (pair.empty()) continue;
        size_t dash = pair.find('-');
        if (dash == std::string::npos) {
            problems.push_back("physics_world.ignorePairs: \"" + pair + "\" is not a layer pair like \"debris-player\"");
            continue;
        }
        std::string a = str::trim(pair.substr(0, dash)), b = str::trim(pair.substr(dash + 1));
        JPH::ObjectLayer la = layerIndex(a, 0xffff), lb = layerIndex(b, 0xffff);
        if (la == 0xffff || lb == 0xffff) {
            const std::string& bad = la == 0xffff ? a : b;
            std::string guess = str::closest(bad, layerNameList(), 3);
            problems.push_back("physics_world.ignorePairs: unknown layer \"" + bad + "\"" +
                               (guess.empty() ? "" : " (did you mean \"" + guess + "\"?)"));
            continue;
        }
        collide_[la][lb] = collide_[lb][la] = false;
    }
    return problems;
}

bool LayerMatrix::ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const {
    if (isNonMoving(a) && isNonMoving(b)) return false;
    if ((isSensorLayer(a) && isNonMoving(b)) || (isSensorLayer(b) && isNonMoving(a))) return false;
    JPH::ObjectLayer ua = userLayer(a), ub = userLayer(b);
    if (ua >= kLayerCount || ub >= kLayerCount) return true;
    return collide_[ua][ub];
}

}  // namespace sky::physics
