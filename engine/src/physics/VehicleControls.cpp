// Player controls for vehicles (the drive input actions) and the chase camera.

#include <algorithm>
#include <cmath>

#include "skywalker/ecs/Components.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/physics/Vehicle.h"

namespace sky::physics {

std::vector<input::Action> driveActions() {
    static const char* kDrive = R"JSON({
      "throttle": {"type": "button", "description": "Vehicle accelerator (analog on the right trigger)",
                   "bindings": ["key:w", "key:up", "pad:rightTrigger"]},
      "brake": {"type": "button", "description": "Vehicle brake; held at a standstill it reverses",
                "bindings": ["key:s", "key:down", "pad:leftTrigger"]},
      "steer": {"type": "axis", "description": "Vehicle steering: -1 left .. 1 right",
                "bindings": ["ad", "leftright", "pad:leftStick.x"]},
      "handbrake": {"type": "button", "description": "Vehicle handbrake (slides, drifts, hairpins)",
                    "bindings": ["key:space", "pad:south"]},
      "shift_up": {"type": "button", "description": "Manual gearbox: next gear", "bindings": ["key:e", "pad:rightShoulder"]},
      "shift_down": {"type": "button", "description": "Manual gearbox: previous gear", "bindings": ["key:q", "pad:leftShoulder"]}
    })JSON";
    std::vector<input::Action> out;
    auto parsed = Json::parse(kDrive);
    if (!parsed) return out;
    for (const char* name : {"throttle", "brake", "steer", "handbrake", "shift_up", "shift_down"}) {
        if (auto a = input::ActionMap::actionFromJson(name, parsed->get(name))) out.push_back(std::move(*a));
    }
    return out;
}

namespace {

const input::GamepadState* firstPad(const input::InputState& in) {
    for (const auto& p : in.pads) {
        if (p.connected) return &p;
    }
    return nullptr;
}

bool held(const input::InputState& in, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (in.held.count(k)) return true;
    }
    return false;
}

bool pressedKey(const input::InputState& in, const char* key) { return in.pressed.count(key) != 0; }

}  // namespace

void applyVehicleControls(Scene& scene, const input::InputState& in) {
    if (scene.registry().count<Vehicle>() == 0) return;
    const input::GamepadState* pad = firstPad(in);
    auto action = [&](const char* name) -> const input::ActionState* {
        auto it = in.actions.find(name);
        return it == in.actions.end() ? nullptr : &it->second;
    };
    // The drive actions when the project defines them; otherwise the same default bindings read raw.
    const input::ActionState* aThrottle = action("throttle");
    const input::ActionState* aBrake = action("brake");
    const input::ActionState* aSteer = action("steer");
    const input::ActionState* aHand = action("handbrake");
    const input::ActionState* aUp = action("shift_up");
    const input::ActionState* aDown = action("shift_down");
    float throttle = aThrottle ? aThrottle->x
                               : std::max(held(in, {"w", "up"}) ? 1.f : 0.f, pad ? pad->rt : 0.f);
    float brake = aBrake ? aBrake->x : std::max(held(in, {"s", "down"}) ? 1.f : 0.f, pad ? pad->lt : 0.f);
    float steer = aSteer ? aSteer->x
                         : (held(in, {"d", "right"}) ? 1.f : 0.f) - (held(in, {"a", "left"}) ? 1.f : 0.f) +
                               (pad && std::fabs(pad->lx) > 0.15f ? pad->lx : 0.f);
    float hand = aHand ? aHand->x
                       : (held(in, {"space"}) || (pad && pad->button(input::PadButton::South)) ? 1.f : 0.f);
    int shift = 0;
    if (aUp ? aUp->pressed : pressedKey(in, "e")) ++shift;
    if (aDown ? aDown->pressed : pressedKey(in, "q")) --shift;
    for (EntityId e : scene.entities()) {
        Vehicle* v = scene.get<Vehicle>(e);
        if (!v || v->control != "player" || !scene.isActive(e)) continue;
        v->throttle = std::clamp(throttle, 0.f, 1.f);
        v->brake = std::clamp(brake, 0.f, 1.f);
        v->steer = std::clamp(steer, -1.f, 1.f);
        v->handbrake = std::clamp(hand, 0.f, 1.f);
        if (shift != 0 && v->transmission == "manual") v->gear = std::clamp(v->gear + shift, -1, 20);
    }
}

// ---------------------------------------------------------------------------
// Chase camera
// ---------------------------------------------------------------------------

void ChaseCameras::update(Scene& scene, PhysicsWorld* world, float dt) {
    if (scene.registry().count<ChaseCamera>() == 0 || dt <= 0) return;
    bool moved = false;
    for (EntityId e : scene.entities()) {
        const ChaseCamera* cc = scene.get<ChaseCamera>(e);
        Transform* tr = scene.get<Transform>(e);
        if (!cc || !tr || !scene.isActive(e)) continue;
        EntityId target = scene.resolve(cc->target, e);
        if (target == kNoEntity || target == e) continue;
        const Mat4 tw = scene.worldMatrix(target);
        const Vec3 up{0.f, 1.f, 0.f};
        const Vec3 tpos = tw.translation();
        Vec3 fwd = tw.transformDir({0.f, 0.f, -1.f});
        fwd.y = 0.f;
        fwd = length(fwd) > 1e-4f ? normalize(fwd) : Vec3{0.f, 0.f, -1.f};
        Vec3 vel{0.f};
        if (world) {
            if (auto t = world->vehicle(target)) vel = t->velocity;
            else if (auto bv = world->velocity(target)) vel = *bv;
        }
        Vec3 flat{vel.x, 0.f, vel.z};
        float speed = length(flat);
        // The arm swings behind the direction of travel (when moving forward), so slides and
        // drifts stay framed, and behind the nose otherwise.
        Vec3 desired = fwd;
        if (speed > 3.f && dot(flat, fwd) > 0.f) desired = normalize(fwd + normalize(flat));
        State& st = state_[e];
        const bool first = st.fov <= 0.f;
        float kTurn = 1.f - std::exp(-cc->turnStiffness * dt);
        st.heading = first ? desired : normalize(st.heading + (desired - st.heading) * kTurn);
        if (length(st.heading) < 0.5f) st.heading = desired;
        Vec3 aim = tpos + up * cc->targetHeight + flat * cc->lookAhead;
        Vec3 goal = tpos - st.heading * cc->distance + up * cc->height;
        if (cc->collide && world) {
            Vec3 from = tpos + up * cc->targetHeight;
            Vec3 dir = goal - from;
            float len = length(dir);
            if (len > 0.1f) {
                QueryFilter f;
                f.exclude.push_back(target);
                for (EntityId c : scene.children(target)) f.exclude.push_back(c);
                if (auto hit = world->raycast(from, dir / len, len, f)) goal = from + dir / len * std::max(hit->distance - 0.3f, 0.5f);
            }
        }
        float kPos = 1.f - std::exp(-cc->stiffness * dt);
        st.position = first ? goal : st.position + (goal - st.position) * kPos;
        Vec3 look = aim - st.position;
        if (length(look) < 1e-3f) look = fwd;
        look = normalize(look);
        Vec3 rotation{degrees(std::asin(std::clamp(look.y, -1.f, 1.f))), degrees(std::atan2(-look.x, -look.z)), 0.f};
        const EntityRecord* r = scene.record(e);
        if (r && r->parent) {
            Mat4 parent = scene.worldMatrix(r->parent).inverse();
            tr->position = parent.transformPoint(st.position);
        } else {
            tr->position = st.position;
        }
        tr->rotation = rotation;
        if (Camera* cam = scene.get<Camera>(e)) {
            float fovTarget = cc->fovMin + (cc->fovMax - cc->fovMin) * std::clamp(speed / std::max(cc->fovSpeed, 0.1f), 0.f, 1.f);
            st.fov = first ? fovTarget : st.fov + (fovTarget - st.fov) * (1.f - std::exp(-2.f * dt));
            cam->fov = std::round(st.fov * 100.f) / 100.f;
        } else {
            st.fov = 1.f;
        }
        moved = true;
    }
    if (moved) scene.markDirty();
}

}  // namespace sky::physics
