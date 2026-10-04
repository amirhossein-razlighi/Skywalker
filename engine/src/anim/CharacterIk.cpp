// Foot / hand IK math (anim/CharacterIk.h) and its AnimationSystem integration: the `characterIk`
// component, ground probes, two-handed grips on props held by the other hand, and turn in place.

#include "skywalker/anim/CharacterIk.h"

#include <algorithm>
#include <cmath>

#include "AnimationSystemInternal.h"
#include "skywalker/core/Strings.h"

namespace sky::anim {

namespace {

float smoothK(float rate, float dt) { return dt <= 0.f ? 1.f : 1.f - std::exp(-std::max(rate, 0.f) * dt); }

Vec3 flat(Vec3 v, Vec3 up) { return v - up * dot(v, up); }

float smoothstep01(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

/// The ground's tilt for a foot: up -> normal, limited to maxSlope degrees.
Quat slopeTilt(Vec3 up, Vec3 normal, float maxSlopeDeg) {
    if (length(normal) < 1e-6f) return {};
    Vec3 n = normalize(normal);
    if (dot(n, up) < 0.f) n = -n;
    Quat q = Quat::fromTo(up, n);
    float angle = q.angle(), limit = radians(std::clamp(maxSlopeDeg, 0.f, 89.f));
    if (angle > limit && angle > 1e-5f) q = slerp(Quat{}, q, limit / angle);
    return q;
}

Vec3 jsonVec(Vec3 v) { return {std::round(v.x * 1000.f) / 1000.f, std::round(v.y * 1000.f) / 1000.f, std::round(v.z * 1000.f) / 1000.f}; }

}  // namespace

FootIkResult solveFeet(const FootIkSettings& s, const std::array<FootInput, 2>& feet, Vec3 base, Vec3 up, float dt,
                       FootIkState& state) {
    FootIkResult r;
    up = length(up) > 1e-6f ? normalize(up) : Vec3{0, 1, 0};
    const float k = state.initialized ? smoothK(s.smoothing, dt) : 1.f;
    const bool simulate = dt > 0.f && state.initialized;  // previews (dt = 0) snap and never lock
    float lowest = 0.f;
    for (int i = 0; i < 2; ++i) {
        FootIkState::Foot& f = state.feet[static_cast<size_t>(i)];
        const FootInput& in = feet[static_cast<size_t>(i)];
        // --- contact ---------------------------------------------------------------------------
        const float lift = dot(in.ankle - base, up) - s.footHeight;  // the animation's own lift of the sole
        const float speed = f.hasLast && dt > 0.f ? length(flat(in.ankle - f.lastAnimated, up)) / dt : 0.f;
        const float liftLimit = f.contact ? 0.08f : 0.05f, speedLimit = s.lockSpeed * (f.contact ? 1.5f : 1.f);
        bool contact = false;
        if (s.contact == "always") contact = true;
        else if (s.contact == "never") contact = false;
        else if (s.contact == "events") contact = f.eventContact;
        else if (s.contact == "velocity") contact = f.hasLast && speed < speedLimit;
        else contact = f.hasLast && lift < liftLimit && speed < speedLimit;  // auto
        f.contact = contact;
        f.lastAnimated = in.ankle;
        f.hasLast = true;
        // --- ground --------------------------------------------------------------------------------
        const GroundProbe& g = in.heel.hit ? in.heel : in.toeProbe;
        float offset = 0.f;
        bool reach = false;
        if (g.hit) {
            offset = dot(g.point - base, up);
            reach = std::fabs(offset) <= s.stepHeight;
        }
        f.ground = g;
        f.weight += ((reach ? 1.f : 0.f) - f.weight) * (state.initialized ? smoothK(s.smoothing * 0.6f, dt) : 1.f);
        if (reach) f.offset += (offset - f.offset) * k;
        // --- target ---------------------------------------------------------------------------------
        Vec3 target = in.ankle + up * f.offset;
        Quat tilt = s.alignFeet && g.hit && reach ? slopeTilt(up, g.normal, s.maxSlope) : Quat{};
        // The toe never sinks into a higher step or rising ground.
        if (in.toeProbe.hit) {
            Vec3 toe = in.toe + up * f.offset;
            float need = dot(in.toeProbe.point - base, up) + s.toeHeight - dot(toe - base, up);
            if (need > 0.f) target += up * std::min(need, s.stepHeight);
        }
        // --- locking ------------------------------------------------------------------------------
        if (simulate && contact && f.weight > 0.5f) {
            if (!f.locked) {
                f.locked = true;
                f.lockPoint = target;
            } else if (length(flat(target - f.lockPoint, up)) > s.lockDistance) {
                f.replantFrom = f.lockPoint;  // the animation moved on: step to the new spot
                f.replant = 0.f;
                f.lockPoint = target;
            }
        } else if (f.locked) {
            f.locked = false;
            f.replantFrom = f.target;
            f.replant = 0.f;
        }
        Vec3 desired = target;
        if (f.locked) desired = f.lockPoint - up * dot(f.lockPoint, up) + up * dot(target, up);  // planted, ground height kept fresh
        if (simulate && f.replant < 1.f) {
            f.replant = std::min(1.f, f.replant + dt / 0.16f);
            float e = smoothstep01(f.replant);
            desired = lerp(f.replantFrom, desired, e) + up * (std::sin(e * kPi) * 0.045f * (f.locked ? 1.f : 0.f));
        } else if (!simulate) {
            f.replant = 1.f;
        }
        f.target = desired;
        f.tilt = tilt;
        r.ankle[static_cast<size_t>(i)] = desired;
        r.tilt[static_cast<size_t>(i)] = tilt;
        r.weight[static_cast<size_t>(i)] = std::clamp(f.weight * s.weight, 0.f, 1.f);
        lowest = std::min(lowest, dot(desired - in.ankle, up) * f.weight);
    }
    const float pelvisGoal = s.pelvis ? lowest * std::clamp(s.weight, 0.f, 1.f) : 0.f;
    state.pelvisOffset += (pelvisGoal - state.pelvisOffset) * k;
    r.pelvisOffset = state.pelvisOffset;
    state.initialized = true;
    return r;
}

void applyFeet(const Skeleton& sk, const HumanoidMap& map, const FootIkResult& r, const Mat4& toModel, Pose& pose,
               std::vector<Mat4>& globals) {
    computeGlobals(sk, pose, globals);
    const int hips = map[HumanBone::Hips];
    if (hips >= 0 && std::fabs(r.pelvisOffset) > 1e-6f) {
        // The pelvis moves along the world up axis, expressed in the skeleton's model space.
        Vec3 d = toModel.transformDir(Vec3{0, 1, 0}) * r.pelvisOffset;
        const int parent = sk.bones[static_cast<size_t>(hips)].parent;
        Vec3 now = globals[static_cast<size_t>(hips)].translation() + d;
        pose[static_cast<size_t>(hips)].t = parent >= 0 ? globals[static_cast<size_t>(parent)].inverse().transformPoint(now) : now;
        computeGlobals(sk, pose, globals);
    }
    const Quat toModelQ = rotationOf(toModel);
    for (int i = 0; i < 2; ++i) {
        const HumanBone up = i ? HumanBone::RightUpperLeg : HumanBone::LeftUpperLeg;
        const HumanBone lo = i ? HumanBone::RightLowerLeg : HumanBone::LeftLowerLeg;
        const HumanBone ft = i ? HumanBone::RightFoot : HumanBone::LeftFoot;
        const float w = r.weight[static_cast<size_t>(i)];
        if (w <= 1e-4f || map[up] < 0 || map[lo] < 0 || map[ft] < 0) continue;
        const int foot = map[ft];
        const Quat footBefore = rotationOf(globals[static_cast<size_t>(foot)]);
        // Bend plane: the animated knee, or forward (+Z, the model's facing) when the leg is straight.
        Vec3 hip = globals[static_cast<size_t>(map[up])].translation(), knee = globals[static_cast<size_t>(map[lo])].translation();
        Vec3 ankle = globals[static_cast<size_t>(foot)].translation();
        Vec3 axis = ankle - hip;
        Vec3 bend = knee - hip - (length(axis) > 1e-6f ? normalize(axis) * dot(knee - hip, normalize(axis)) : Vec3{0, 0, 0});
        Vec3 pole = length(bend) > 0.01f * std::max(length(axis), 1e-3f) ? bend : Vec3{0, 0, 1};
        solveTwoBoneChain(sk, pose, globals, map[up], map[lo], foot, toModel.transformPoint(r.ankle[static_cast<size_t>(i)]), pole, w);
        // The foot keeps its animated orientation (the leg IK turned it with the shin), then tilts onto the slope.
        Quat tiltModel = (toModelQ * r.tilt[static_cast<size_t>(i)] * toModelQ.conjugate()).normalized();
        Quat want = (slerp(Quat{}, tiltModel, w) * footBefore).normalized();
        const int parent = sk.bones[static_cast<size_t>(foot)].parent;
        Quat parentQ = parent >= 0 ? rotationOf(globals[static_cast<size_t>(parent)]) : Quat{};
        pose[static_cast<size_t>(foot)].r = (parentQ.conjugate() * want).normalized();
        computeGlobals(sk, pose, globals);
    }
}

bool applyHand(const Skeleton& sk, const HumanoidMap& map, bool left, const Mat4& targetWorld, const Mat4& toModel, float weight,
               bool matchRotation, Pose& pose, std::vector<Mat4>& globals) {
    const int up = map[left ? HumanBone::LeftUpperArm : HumanBone::RightUpperArm];
    const int lo = map[left ? HumanBone::LeftLowerArm : HumanBone::RightLowerArm];
    const int hand = map[left ? HumanBone::LeftHand : HumanBone::RightHand];
    if (up < 0 || lo < 0 || hand < 0 || weight <= 0.f) return false;
    computeGlobals(sk, pose, globals);
    Mat4 target = toModel * targetWorld;
    if (!solveTwoBoneChain(sk, pose, globals, up, lo, hand, target.translation(), {0, 0, 0}, std::clamp(weight, 0.f, 1.f))) return false;
    if (matchRotation) {
        Quat want = rotationOf(target);
        const int parent = sk.bones[static_cast<size_t>(hand)].parent;
        Quat parentQ = parent >= 0 ? rotationOf(globals[static_cast<size_t>(parent)]) : Quat{};
        Quat local = (parentQ.conjugate() * want).normalized();
        Trs& t = pose[static_cast<size_t>(hand)];
        t.r = weight >= 1.f ? local : slerp(t.r, local, weight);
        computeGlobals(sk, pose, globals);
    }
    return true;
}

void restFootHeights(const Skeleton& sk, const HumanoidMap& map, float& ankleHeight, float& toeHeight) {
    std::vector<Mat4> rest;
    computeGlobals(sk, restPose(sk), rest);
    float ankle = 0.f, toe = 0.f, lowest = 1e30f;
    int n = 0;
    for (auto [ft, te] : {std::pair{HumanBone::LeftFoot, HumanBone::LeftToes}, std::pair{HumanBone::RightFoot, HumanBone::RightToes}}) {
        if (map[ft] < 0) continue;
        float a = rest[static_cast<size_t>(map[ft])].translation().y;
        float t = map[te] >= 0 ? rest[static_cast<size_t>(map[te])].translation().y : a;
        ankle += a;
        toe += t;
        ++n;
        // The lowest joint of the foot chain (toe tips included).
        for (size_t b = 0; b < sk.bones.size(); ++b) {
            if (sk.isDescendant(static_cast<int>(b), map[ft])) lowest = std::min(lowest, rest[b].translation().y);
        }
    }
    if (n == 0) {
        ankleHeight = 0.08f;
        toeHeight = 0.02f;
        return;
    }
    ankle /= static_cast<float>(n);
    toe /= static_cast<float>(n);
    // Characters usually stand on their model origin; otherwise the sole is just below the lowest joint.
    float sole = lowest >= -0.02f * std::max(1.f, std::fabs(ankle) * 10.f) ? std::min(0.f, lowest) : lowest - 0.02f;
    if (ankle - sole <= 1e-4f) sole = lowest - 0.02f;
    ankleHeight = std::max(ankle - sole, 1e-3f);
    toeHeight = std::max(toe - sole, 0.f);
}

// ---------------------------------------------------------------------------------------------
// AnimationSystem integration
// ---------------------------------------------------------------------------------------------

const HumanoidMap* AnimationSystem::humanoidOf(Instance& inst) {
    if (!inst.lib) return nullptr;
    if (!inst.humanoid) {
        inst.humanoid = detectHumanoid(inst.lib->skeleton);
        restFootHeights(inst.lib->skeleton, *inst.humanoid, inst.restAnkleHeight, inst.restToeHeight);
    }
    return &*inst.humanoid;
}

Mat4 AnimationSystem::freshWorld(EntityId target, EntityId character, Instance& inst) {
    // A target carried by this character (a grip point on a staff in the other hand) follows the bone
    // pose of *this* tick, not last tick's attachment transform: walk up to the attached ancestor.
    Mat4 chain;
    for (EntityId cur = target; cur && scene_.exists(cur);) {
        if (const BoneAttachment* at = scene_.get<BoneAttachment>(cur)) {
            EntityId owner = !at->character.empty() ? scene_.resolve(at->character, cur) : kNoEntity;
            if (!owner) {
                if (const EntityRecord* rec = scene_.record(cur); rec && rec->parent) owner = animatorFor(rec->parent);
            }
            int bone = owner == character && inst.lib ? inst.lib->skeleton.find(at->bone) : -1;
            if (bone >= 0 && static_cast<size_t>(bone) < inst.globals.size()) {
                Mat4 b = modelToWorld(inst, character) * inst.globals[static_cast<size_t>(bone)];
                if (!at->followScale) {
                    Trs d = Trs::fromMatrix(b);
                    b = Mat4::translate(d.t) * d.r.matrix();
                }
                return b * Mat4::trs(at->offset, at->rotation, {1, 1, 1}) * chain;
            }
            break;
        }
        const Transform* t = scene_.get<Transform>(cur);
        chain = (t ? t->local() : Mat4{}) * chain;
        const EntityRecord* rec = scene_.record(cur);
        cur = rec ? rec->parent : kNoEntity;
    }
    return scene_.worldMatrix(target);
}

void AnimationSystem::applyCharacterIk(Instance& inst, EntityId e) {
    const CharacterIk* c = scene_.get<CharacterIk>(e);
    if (!c) {
        inst.ikStatus = Json();
        return;
    }
    const HumanoidMap* map = humanoidOf(inst);
    const Skeleton& sk = inst.lib->skeleton;
    Json status = Json::object();
    if (!map || !map->complete()) {
        status["warning"] = "the skeleton is not a complete humanoid; foot and hand IK are off (character_inspect shows the bone map)";
        inst.ikStatus = status;
        return;
    }
    computeGlobals(sk, inst.pose, inst.globals);
    const Mat4 modelWorld = modelToWorld(inst, e);
    const Mat4 toModel = modelWorld.inverse();
    const Vec3 upWorld = [&] {
        Vec3 u = scene_.worldMatrix(inst.meshEntity && scene_.exists(inst.meshEntity) ? inst.meshEntity : e).transformDir({0, 1, 0});
        return length(u) > 1e-6f ? normalize(u) : Vec3{0, 1, 0};
    }();
    const float unit = length(modelWorld.transformDir({0, 1, 0}));  // model units -> meters (along up)

    // --- feet ---------------------------------------------------------------------------------------
    Json feetJ = Json::array();
    if (c->feet && c->feetWeight > 0.f && hooks.ground) {
        FootIkSettings s;
        s.weight = std::clamp(c->feetWeight, 0.f, 1.f);
        s.stepHeight = std::max(c->stepHeight, 0.01f);
        s.footHeight = c->footHeight >= 0.f ? c->footHeight : inst.restAnkleHeight * unit;
        s.toeHeight = inst.restToeHeight * unit;
        s.pelvis = c->pelvis;
        s.alignFeet = c->alignFeet;
        s.maxSlope = c->maxSlope;
        s.contact = c->contact;
        s.lockSpeed = c->lockSpeed;
        s.lockDistance = c->lockDistance;
        s.smoothing = c->smoothing;
        const Vec3 base = scene_.worldMatrix(e).translation();
        std::array<FootInput, 2> in;
        for (int i = 0; i < 2; ++i) {
            int foot = (*map)[i ? HumanBone::RightFoot : HumanBone::LeftFoot];
            int toe = (*map)[i ? HumanBone::RightToes : HumanBone::LeftToes];
            FootInput& f = in[static_cast<size_t>(i)];
            f.ankle = modelWorld.transformPoint(inst.globals[static_cast<size_t>(foot)].translation());
            f.toe = toe >= 0 ? modelWorld.transformPoint(inst.globals[static_cast<size_t>(toe)].translation()) : f.ankle;
            const float reach = s.stepHeight + s.footHeight;
            // Probe from above the highest reachable ground straight down past the lowest.
            Vec3 heelFrom = f.ankle - upWorld * dot(f.ankle - base, upWorld) + upWorld * (s.stepHeight + reach);
            Vec3 toeFrom = f.toe - upWorld * dot(f.toe - base, upWorld) + upWorld * (s.stepHeight + reach);
            f.heel = hooks.ground(heelFrom, -upWorld, 2.f * s.stepHeight + 2.f * reach, e);
            f.toeProbe = hooks.ground(toeFrom, -upWorld, 2.f * s.stepHeight + 2.f * reach, e);
        }
        FootIkResult r = solveFeet(s, in, base, upWorld, inst.tickDt, inst.feet);
        applyFeet(sk, *map, r, toModel, inst.pose, inst.globals);
        for (int i = 0; i < 2; ++i) {
            const auto& f = inst.feet.feet[static_cast<size_t>(i)];
            Json fj = Json::object({{"side", i ? "right" : "left"},
                                    {"contact", f.contact},
                                    {"locked", f.locked},
                                    {"weight", std::round(r.weight[static_cast<size_t>(i)] * 100.f) / 100.f},
                                    {"groundOffset", std::round(f.offset * 1000.f) / 1000.f},
                                    {"target", reflect::vec3ToJson(jsonVec(f.target))},
                                    {"animated", reflect::vec3ToJson(jsonVec(in[static_cast<size_t>(i)].ankle))}});
            if (f.ground.hit) {
                fj["ground"] = reflect::vec3ToJson(jsonVec(f.ground.point));
                fj["slopeDeg"] = std::round(degrees(std::acos(std::clamp(dot(normalize(f.ground.normal), upWorld), -1.f, 1.f))) * 10.f) / 10.f;
            } else {
                fj["ground"] = Json();
            }
            feetJ.push(std::move(fj));
        }
        status["pelvisOffset"] = std::round(r.pelvisOffset * 1000.f) / 1000.f;
        status["footHeight"] = std::round(s.footHeight * 1000.f) / 1000.f;
    } else if (c->feet && !hooks.ground) {
        status["warning"] = "no ground probe available (headless tools without a scene query)";
    }
    status["feet"] = feetJ;

    // --- hands (right first: a left-hand grip on a prop held in the right hand follows it) -------------
    Json handsJ = Json::array();
    for (int side = 1; side >= 0; --side) {
        const EntityLink& link = side ? c->rightHand : c->leftHand;
        const float w = side ? c->rightHandWeight : c->leftHandWeight;
        if (link.empty() || w <= 0.f) continue;
        EntityId t = scene_.resolve(link, e);
        Json hj = Json::object({{"side", side ? "right" : "left"}, {"weight", w}});
        if (!t || !scene_.isActive(t)) {
            hj["warning"] = "target \"" + link.name + "\" not found or inactive";
            handsJ.push(std::move(hj));
            continue;
        }
        Mat4 target = freshWorld(t, e, inst);
        bool ok = applyHand(sk, *map, side == 0, target, toModel, w, c->handRotation, inst.pose, inst.globals);
        int hand = (*map)[side ? HumanBone::RightHand : HumanBone::LeftHand];
        Vec3 reached = modelWorld.transformPoint(inst.globals[static_cast<size_t>(hand)].translation());
        hj["target"] = reflect::vec3ToJson(jsonVec(target.translation()));
        hj["error"] = std::round(distance(reached, target.translation()) * 1000.f) / 1000.f;  // m: > 0 = out of reach
        if (!ok) hj["warning"] = "the arm chain is incomplete";
        handsJ.push(std::move(hj));
    }
    status["hands"] = handsJ;
    inst.ikStatus = status;
}

void AnimationSystem::updateTurn(Instance& inst, EntityId e, float dt) {
    if (!inst.turnTarget) return;
    Transform* t = scene_.get<Transform>(e);
    if (!t) {
        inst.turnTarget.reset();
        return;
    }
    const CharacterIk* c = scene_.get<CharacterIk>(e);
    const float rate = c ? std::max(c->turnSpeed, 1.f) : 220.f;
    Vec3 fwd = scene_.worldMatrix(e).transformDir({0, 0, -1});  // the engine's forward (-Z)
    float yaw = degrees(std::atan2(-fwd.x, -fwd.z));
    float left = *inst.turnTarget - yaw;
    while (left > 180.f) left -= 360.f;
    while (left < -180.f) left += 360.f;
    float step = std::clamp(left, -rate * dt, rate * dt);
    if (std::fabs(left) < 0.05f) step = left;
    if (std::fabs(step) > 1e-5f) applyRootYaw(inst, e, radians(step));
    left -= step;
    const bool turning = std::fabs(left) > 0.05f;
    (void)inst.rt.setParam("turn", turning ? left : 0.f);  // controllers may blend turn clips on these
    (void)inst.rt.setParam("turning", turning ? 1.f : 0.f);
    if (!turning) inst.turnTarget.reset();
}

Status AnimationSystem::turnInPlace(EntityId e, float yawDegrees) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    if (!std::isfinite(yawDegrees)) return Error::make("invalid_argument", "the yaw must be a number of degrees");
    inst->turnTarget = yawDegrees;
    return {};
}

Result<Json> AnimationSystem::characterStatus(EntityId e) {
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!inst) return Error::make("not_found", "entity " + formatEntityRef(e) + " has no animator");
    Json j = Json::object({{"animator", ae}});
    if (!inst->lib) {
        j["error"] = inst->error;
        return j;
    }
    const Animator* a = scene_.get<Animator>(ae);
    if (a && !playing_) poseEditing(*inst, ae, *a);
    else if (a && !inst->posed) finishPose(*inst, ae, *a);
    if (const HumanoidMap* m = humanoidOf(*inst)) j["humanoid"] = m->toJson(inst->lib->skeleton);
    j["ik"] = inst->ikStatus.isNull() ? Json::object({{"enabled", false}}) : inst->ikStatus;
    if (inst->turnTarget) j["turnTarget"] = *inst->turnTarget;
    j["restFootHeight"] = std::round(inst->restAnkleHeight * 1000.f) / 1000.f;
    return j;
}

std::vector<std::pair<Vec3, Vec3>> AnimationSystem::skeletonLines(EntityId e) {
    std::vector<std::pair<Vec3, Vec3>> out;
    EntityId ae = animatorFor(e);
    Instance* inst = ae ? instance(ae) : nullptr;
    if (!inst || !inst->lib) return out;
    const Animator* a = scene_.get<Animator>(ae);
    if (a && !playing_) poseEditing(*inst, ae, *a);
    else if (a && !inst->posed) finishPose(*inst, ae, *a);
    const Skeleton& sk = inst->lib->skeleton;
    if (inst->globals.size() != sk.bones.size()) return out;
    const Mat4 mw = modelToWorld(*inst, ae);
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        int p = sk.bones[b].parent;
        if (p < 0) continue;
        out.push_back({mw.transformPoint(inst->globals[static_cast<size_t>(p)].translation()), mw.transformPoint(inst->globals[b].translation())});
    }
    return out;
}

}  // namespace sky::anim
