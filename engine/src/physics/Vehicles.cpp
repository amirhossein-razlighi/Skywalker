#include "Vehicles.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>

#include "Shapes.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/MeshData.h"

namespace sky::physics {

namespace {

constexpr float kKmh = 3.6f;

// Default tire and engine curves (normalized; the grips / maxTorque scale them). The tires peak
// early and fall off gently, so slides are progressive and catchable rather than snappy.
const std::vector<std::pair<float, float>> kLongitudinalCurve{{0.f, 0.f}, {0.08f, 1.f}, {0.25f, 0.9f}, {1.f, 0.75f}};
const std::vector<std::pair<float, float>> kLateralCurve{{0.f, 0.f}, {5.f, 1.f}, {14.f, 0.9f}, {40.f, 0.78f}, {90.f, 0.7f}};
const std::vector<std::pair<float, float>> kTorqueCurve{{0.f, 0.75f}, {0.25f, 0.9f}, {0.6f, 1.f}, {0.85f, 0.95f}, {1.f, 0.8f}};
const std::vector<float> kGearRatios{2.66f, 1.78f, 1.3f, 1.0f, 0.74f};

bool startsWithWheel(const std::string& name) { return str::startsWith(str::lower(name), "wheel"); }

std::vector<std::pair<float, float>> curveFrom(const Json& j, const std::vector<std::pair<float, float>>& fallback) {
    std::vector<std::pair<float, float>> out;
    if (j.isArray()) {
        for (const auto& p : j.elements()) {
            if (p.isArray() && p.size() >= 2) out.emplace_back(p[size_t{0}].asFloat(0.f), p[size_t{1}].asFloat(0.f));
        }
    }
    if (out.size() < 2) return fallback;
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

JPH::LinearCurve linearCurve(const std::vector<std::pair<float, float>>& points, float scale) {
    JPH::LinearCurve c;
    c.Reserve(static_cast<JPH::uint>(points.size()));
    for (const auto& [x, y] : points) c.AddPoint(x, y * scale);
    return c;
}

float entryFloat(const Json& entry, const char* key, float fallback) {
    const Json& v = entry.get(key);
    return v.isNumber() ? v.asFloat(fallback) : fallback;
}

Vec3 divide(Vec3 a, Vec3 b) {
    auto safe = [](float x) { return std::fabs(x) < 1e-6f ? 1.f : x; };
    return {a.x / safe(b.x), a.y / safe(b.y), a.z / safe(b.z)};
}

/// World-space bounds of the meshes in a subtree (wheel visuals), or nullopt.
std::optional<Aabb> subtreeMeshBounds(const Scene& s, EntityId root, const MeshProvider& meshes) {
    std::vector<EntityId> stack{root};
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    bool any = false;
    while (!stack.empty()) {
        EntityId e = stack.back();
        stack.pop_back();
        if (const MeshRenderer* mr = s.get<MeshRenderer>(e)) {
            const MeshData* md = meshes && str::startsWith(mr->mesh, "asset:") ? meshes(mr->mesh) : nullptr;
            Aabb b = (md ? md->bounds : s.localBounds(e)).transformed(s.worldMatrix(e));
            if (b.min.x <= b.max.x) {
                box.min = vmin(box.min, b.min);
                box.max = vmax(box.max, b.max);
                any = true;
            }
        }
        for (EntityId c : s.children(e)) stack.push_back(c);
    }
    if (!any) return std::nullopt;
    return box;
}

/// One wheel as set up from the scene (body space = the chassis entity without scale).
struct WheelSetup {
    EntityId visual = kNoEntity;
    std::string name;
    int axle = 0;
    bool left = true;
    bool center = false;
    bool steers = false, driven = false, handbrake = false;
    Vec3 position;  // wheel center at rest, body space
    float radius = 0.35f, width = 0.25f;
    float minLength = 0.1f, maxLength = 0.35f, frequency = 1.6f, damping = 0.5f;
    float maxSteerDeg = 32.f, brakeTorque = 2500.f, handbrakeTorque = 5000.f;
    float longitudinalGrip = 1.4f, lateralGrip = 1.3f;
    float restLength = 0.f;
    Json entry;  // the `wheels` entry it came from (explicit steer/drive/handbrake flags)
    // Visual rest pose (chassis-entity space, scale included).
    Mat4 chassisFromParent;  // the visual's parent in chassis space
    Transform restLocal;
    Mat4 restInChassis;
    Vec3 restCenterChassis;
};

struct Fit {
    std::vector<WheelSetup> wheels;
    int axles = 0;
    std::vector<std::string> warnings;
};

/// Wheels from `wheels` entries, or from the children named wheel* (positions, radius and width
/// measured from their meshes). Assigns axles, sides and the steer/drive/handbrake roles.
Fit fitWheels(const Scene& s, EntityId e, const Vehicle& v, const MeshProvider& meshes) {
    Fit fit;
    const std::string label = "vehicle '" + (s.record(e) ? s.record(e)->name : std::string("?")) + "'";
    Decomposed chassis = decompose(s.worldMatrix(e));
    Mat4 bodyToWorld = rigidMatrix(chassis.translation, chassis.rotation);
    Mat4 worldToBody = bodyToWorld.inverse();
    Vec3 scale = chassis.scale;

    std::vector<Json> entries;
    if (v.wheels.isArray() && v.wheels.size() > 0) {
        for (const auto& w : v.wheels.elements()) entries.push_back(w);
    } else {
        // Children named wheel* (not nested inside another wheel visual), in scene order.
        std::vector<EntityId> stack;
        for (EntityId c : s.children(e)) stack.push_back(c);
        std::vector<EntityId> found;
        while (!stack.empty()) {
            EntityId c = stack.back();
            stack.pop_back();
            const EntityRecord* r = s.record(c);
            if (!r || !r->enabled) continue;
            if (startsWithWheel(r->name)) {
                found.push_back(c);
                continue;
            }
            for (EntityId g : s.children(c)) stack.push_back(g);
        }
        std::sort(found.begin(), found.end(), [&](EntityId a, EntityId b) {
            const auto& order = s.entities();
            return std::find(order.begin(), order.end(), a) < std::find(order.begin(), order.end(), b);
        });
        for (EntityId c : found) entries.push_back(Json::object({{"entity", static_cast<int64_t>(c)}}));
    }

    auto isDescendant = [&](EntityId c) {
        for (const EntityRecord* r = s.record(c); r && r->parent; r = s.record(r->parent)) {
            if (r->parent == e) return true;
        }
        return false;
    };

    for (const Json& entry : entries) {
        WheelSetup w;
        const Json& ent = entry.get("entity");
        if (ent.isNumber()) {
            w.visual = static_cast<EntityId>(ent.asInt(0));
        } else if (ent.isString() && !ent.asString().empty()) {
            w.visual = s.findNear(ent.asString(), e);
        }
        if (w.visual != kNoEntity && (!s.exists(w.visual) || !isDescendant(w.visual))) {
            fit.warnings.push_back(label + ": wheel entity \"" + ent.dump() + "\" is not a child of the vehicle; it will not move");
            w.visual = kNoEntity;
        } else if (!ent.isNull() && w.visual == kNoEntity && !(ent.isString() && ent.asString().empty())) {
            fit.warnings.push_back(label + ": wheel entity " + ent.dump() + " not found");
        }
        std::optional<Aabb> measured;
        if (w.visual != kNoEntity) {
            if (auto world = subtreeMeshBounds(s, w.visual, meshes)) {
                Aabb local{Vec3(1e30f), Vec3(-1e30f)};
                for (int i = 0; i < 8; ++i) {
                    Vec3 corner{(i & 1) ? world->max.x : world->min.x, (i & 2) ? world->max.y : world->min.y,
                                (i & 4) ? world->max.z : world->min.z};
                    Vec3 p = worldToBody.transformPoint(corner);
                    local.min = vmin(local.min, p);
                    local.max = vmax(local.max, p);
                }
                measured = local;
            }
        }
        Vec3 pos;
        if (Vec3 given; reflect::jsonToVec3(entry.get("position"), given)) {
            pos = given * scale;  // chassis-local -> body space
        } else if (measured) {
            pos = measured->center();
        } else if (w.visual != kNoEntity) {
            pos = worldToBody.transformPoint(s.worldMatrix(w.visual).translation());
        } else {
            fit.warnings.push_back(label + ": a wheel has neither an entity nor a position; skipped");
            continue;
        }
        w.position = pos;
        Vec3 size = measured ? measured->max - measured->min : Vec3(0.f);
        float measuredRadius = std::max(size.y, size.z) * 0.5f;
        w.radius = entryFloat(entry, "radius", v.wheelRadius > 0 ? v.wheelRadius : (measuredRadius > 0.01f ? measuredRadius : 0.35f));
        w.width = entryFloat(entry, "width", v.wheelWidth > 0 ? v.wheelWidth : (size.x > 0.01f ? size.x : 0.25f));
        w.radius = std::clamp(w.radius, 0.02f, 10.f);
        w.width = std::clamp(w.width, 0.01f, 10.f);
        w.minLength = std::max(0.f, entryFloat(entry, "suspensionMinLength", v.suspensionMinLength));
        w.maxLength = std::max(w.minLength + 0.01f, entryFloat(entry, "suspensionMaxLength", v.suspensionMaxLength));
        w.frequency = std::clamp(entryFloat(entry, "suspensionFrequency", v.suspensionFrequency), 0.1f, 20.f);
        w.damping = std::clamp(entryFloat(entry, "suspensionDamping", v.suspensionDamping), 0.f, 5.f);
        w.maxSteerDeg = std::clamp(entryFloat(entry, "maxSteerAngle", v.maxSteerAngle), 0.f, 89.f);
        w.brakeTorque = std::max(0.f, entryFloat(entry, "brakeTorque", v.brakeTorque));
        w.handbrakeTorque = std::max(0.f, entryFloat(entry, "handbrakeTorque", v.handbrakeTorque));
        w.longitudinalGrip = std::max(0.f, entryFloat(entry, "longitudinalGrip", v.longitudinalGrip));
        w.lateralGrip = std::max(0.f, entryFloat(entry, "lateralGrip", v.lateralGrip));
        // Visual rest pose.
        if (w.visual != kNoEntity) {
            Mat4 chain;
            std::vector<EntityId> path;
            for (const EntityRecord* r = s.record(s.record(w.visual)->parent); r && r->id != e; r = s.record(r->parent)) {
                path.push_back(r->id);
            }
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                if (const Transform* t = s.get<Transform>(*it)) chain = chain * t->local();
            }
            w.chassisFromParent = chain;
            if (const Transform* t = s.get<Transform>(w.visual)) w.restLocal = *t;
            w.restInChassis = chain * w.restLocal.local();
            w.restCenterChassis = divide(pos, scale);
        }
        w.entry = entry;
        fit.wheels.push_back(w);
    }
    if (fit.wheels.empty()) return fit;

    // Axles: group by z (forward is -Z, so the front axle has the smallest z).
    std::vector<size_t> order(fit.wheels.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return fit.wheels[a].position.z < fit.wheels[b].position.z; });
    int axle = -1;
    float axleZ = -1e30f;
    for (size_t i : order) {
        WheelSetup& w = fit.wheels[i];
        float tolerance = std::max(0.2f, w.radius * 0.75f);
        if (axle < 0 || std::fabs(w.position.z - axleZ) > tolerance) {
            ++axle;
            axleZ = w.position.z;
        }
        w.axle = axle;
    }
    fit.axles = axle + 1;
    const int last = fit.axles - 1;
    for (WheelSetup& w : fit.wheels) {
        const Json& entry = w.entry;
        w.center = std::fabs(w.position.x) < 0.02f;
        w.left = w.position.x < 0.f;
        std::string axleName = fit.axles == 2 ? (w.axle == 0 ? "front" : "rear")
                               : fit.axles == 1 ? "axle"
                                                : "axle" + std::to_string(w.axle);
        w.name = axleName + (w.center ? "_center" : (w.left ? "_left" : "_right"));
        bool steer = v.steering == "all" || (v.steering == "front" && w.axle == 0) || (v.steering == "rear" && w.axle == last && last > 0);
        bool drive = v.drive == "awd" || (v.drive == "fwd" && w.axle == 0) || (v.drive == "rwd" && (w.axle > 0 || last == 0));
        bool hand = w.axle > 0 || last == 0;
        w.steers = entry.get("steer").asBool(steer);
        w.driven = entry.get("drive").asBool(drive);
        w.handbrake = entry.get("handbrake").asBool(hand);
    }
    bool anyDriven = std::any_of(fit.wheels.begin(), fit.wheels.end(), [](const WheelSetup& w) { return w.driven; });
    if (!anyDriven) fit.warnings.push_back(label + ": no wheel is driven (check `drive` and the wheels' drive flags)");
    return fit;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

bool isWheelVisual(const Scene& scene, EntityId owner, EntityId child) {
    const Vehicle* v = scene.get<Vehicle>(owner);
    if (!v) return false;
    const EntityRecord* r = scene.record(child);
    if (!r) return false;
    if (startsWithWheel(r->name)) return true;
    if (v->wheels.isArray()) {
        for (const auto& w : v->wheels.elements()) {
            const Json& ent = w.get("entity");
            if ((ent.isNumber() && static_cast<EntityId>(ent.asInt(0)) == child) || (ent.isString() && ent.asString() == r->name)) {
                return true;
            }
        }
    }
    return false;
}

JPH::RefConst<JPH::Shape> vehicleChassisShape(JPH::RefConst<JPH::Shape> shape, const Vehicle& v) {
    if (!shape || length(v.centerOfMass) < 1e-5f) return shape;
    return new JPH::OffsetCenterOfMassShape(shape, toJolt(v.centerOfMass));
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

struct VehicleSet::Entry {
    EntityId entity = kNoEntity;
    JPH::BodyID body;
    uint64_t signature = 0;
    uint64_t geometry = 0;
    JPH::Ref<JPH::VehicleConstraint> constraint;
    JPH::Ref<JPH::VehicleCollisionTester> tester;
    JPH::WheeledVehicleController* controller = nullptr;
    std::vector<WheelSetup> wheels;
    bool manual = false;
    int forwardGears = 5;
    float wheelBase = 2.5f;
    float maxSteerDeg = 32.f;
    float mass = 1300.f;
    float minRpm = 1000.f, maxRpm = 7000.f;
    // Read by the Jolt step callbacks (job threads; written only between updates).
    std::vector<float> longitudinalScale, lateralScale;
    std::vector<float> surfaceGrip;
    std::vector<uint8_t> absHit;
    std::vector<float> peakLongitudinal;  // per wheel: grip * the curve's peak (what an ideal ABS brakes with)
    std::vector<float> visualAngle;       // per wheel: spin shown on the visual (ABS keeps it rolling)
    bool braking = false;
    std::vector<float> spin;  // per wheel: excess surface speed / ground speed (traction control)
    bool abs = false, tractionControl = false;
    // Driver and assists.
    std::optional<VehicleInput> override;
    VehicleInput input, applied;
    float steer = 0.f;
    bool reversing = false;
    float tractionScale = 1.f;
    float spinFiltered = 0.f;  // wheelspin smoothed over ~0.1 s (Jolt alternates spin and grip step to step)
    int requestedGear = 1;
    int lastGearWritten = 0;
    bool driftActive = false;
    float bodySlip = 0.f;
    // Telemetry.
    Vec3 lastVelocity;
    bool hasLastVelocity = false;
    float lateralG = 0.f, longitudinalG = 0.f, load = 0.f;
    float subDt = 1.f / 60.f;
    float audioVolume = -1.f;  // the audio component's volume when the vehicle was built
    bool stepped = false;
};

VehicleSet::VehicleSet(VehicleHost host) : host_(std::move(host)) {}

VehicleSet::~VehicleSet() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto next = std::next(it);
        remove(it);
        it = next;
    }
}

void VehicleSet::remove(std::map<EntityId, std::unique_ptr<Entry>>::iterator it) {
    if (it->second->constraint) {
        host_.system->RemoveStepListener(it->second->constraint);
        host_.system->RemoveConstraint(it->second->constraint);
    }
    entries_.erase(it);
}

void VehicleSet::forgetBodies(const std::set<uint32_t>& doomed) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto next = std::next(it);
        if (doomed.count(it->second->body.GetIndexAndSequenceNumber())) remove(it);
        it = next;
    }
}

namespace {

uint64_t vehicleSignature(const Vehicle& v, JPH::BodyID body) {
    Hasher h;
    // Inputs, telemetry, assists and aero are read live every tick: they never rebuild the constraint.
    h.reflected(&v, Vehicle::type(),
                {"preset", "throttle", "brake", "steer", "handbrake", "gear", "speed", "rpm", "wheelsOnGround", "skid", "control",
                 "steerSpeed", "speedSensitiveSteering", "tractionControl", "abs", "driftAssist", "autoReverse", "engineAudio",
                 "downforce", "drag"});
    h.pod(body.GetIndexAndSequenceNumber());
    return h.h;
}

/// The rest pose of the wheel visuals (only checked while the world has never been stepped,
/// i.e. edit-time worlds: moving a wheel child in the editor refits the vehicle).
uint64_t vehicleGeometry(const Scene& s, EntityId e, const Vehicle& v) {
    Hasher h;
    h.mat(s.worldMatrix(e));
    std::vector<EntityId> stack;
    for (EntityId c : s.children(e)) stack.push_back(c);
    while (!stack.empty()) {
        EntityId c = stack.back();
        stack.pop_back();
        if (!isWheelVisual(s, e, c)) {
            for (EntityId g : s.children(c)) stack.push_back(g);
            continue;
        }
        h.pod(c);
        h.mat(s.worldMatrix(c));
        if (const MeshRenderer* mr = s.get<MeshRenderer>(c)) h.str(mr->mesh);
    }
    (void)v;
    return h.h;
}

}  // namespace

void VehicleSet::sync(const Scene& s) {
    std::set<EntityId> wanted;
    for (EntityId e : s.entities()) {
        const Vehicle* v = s.get<Vehicle>(e);
        if (!v || !s.isActive(e)) continue;
        JPH::BodyID body = host_.bodyOf(e);
        const RigidBody* rb = s.get<RigidBody>(e);
        const std::string label = "vehicle '" + s.record(e)->name + "'";
        if (body.IsInvalid() || !rb || rb->motion != "dynamic") {
            host_.warn(label + " needs a dynamic `body` on the same entity (vehicle_create sets one up)");
            continue;
        }
        wanted.insert(e);
        uint64_t sig = vehicleSignature(*v, body);
        auto it = entries_.find(e);
        uint64_t geometry = it != entries_.end() && stepped_ ? it->second->geometry : vehicleGeometry(s, e, *v);
        if (it != entries_.end() && it->second->signature == sig && it->second->geometry == geometry) continue;
        std::unique_ptr<Entry> previous;
        if (it != entries_.end()) {
            host_.system->RemoveStepListener(it->second->constraint);
            host_.system->RemoveConstraint(it->second->constraint);
            previous = std::move(it->second);
            entries_.erase(it);
        }
        create(s, e, *v, body, sig, geometry, previous.get());
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto next = std::next(it);
        if (!wanted.count(it->first)) remove(it);
        it = next;
    }
}

bool VehicleSet::create(const Scene& s, EntityId e, const Vehicle& v, JPH::BodyID bodyId, uint64_t signature, uint64_t geometry,
                        const Entry* previous) {
    Fit fit = fitWheels(s, e, v, host_.meshes);
    for (auto& w : fit.warnings) host_.warn(std::move(w));
    const std::string label = "vehicle '" + s.record(e)->name + "'";
    if (fit.wheels.empty()) {
        host_.warn(label + " has no wheels: name its wheel child entities wheel_* (wheel_fl, wheel_fr, wheel_rl, wheel_rr) "
                           "or list them in `wheels`");
        return false;
    }
    JPH::BodyLockWrite lock(host_.system->GetBodyLockInterfaceNoLock(), bodyId);
    if (!lock.Succeeded()) return false;
    JPH::Body& body = lock.GetBody();

    auto entry = std::make_unique<Entry>();
    Entry& en = *entry;
    en.entity = e;
    en.body = bodyId;
    en.signature = signature;
    en.geometry = geometry;
    en.mass = 1.f / std::max(body.GetMotionProperties()->GetInverseMass(), 1e-9f);
    en.minRpm = std::max(0.f, v.minRpm);
    en.maxRpm = std::max(en.minRpm + 100.f, v.maxRpm);

    // Static load per wheel (lever rule between the front-most and rear-most axles) and the
    // effective mass the suspension spring acts on: the rest length puts each wheel center at its
    // modeled position when the car stands still, so the body keeps its authored ride height.
    const JPH::Mat44 bodyRot = JPH::Mat44::sRotation(body.GetRotation());
    const JPH::Mat44 invInertia = body.GetMotionProperties()->GetInverseInertiaForRotation(bodyRot);
    const float invMass = body.GetMotionProperties()->GetInverseMass();
    const JPH::RVec3 com = body.GetCenterOfMassPosition();
    const JPH::RMat44 bodyXf = body.GetWorldTransform();
    const float g = host_.system->GetGravity().Length();
    const Vec3 comBody = fromJolt(JPH::Vec3(bodyXf.InversedRotationTranslation() * com));
    float frontZ = 1e30f, rearZ = -1e30f;
    std::vector<int> wheelsPerAxle(static_cast<size_t>(fit.axles), 0);
    for (const auto& w : fit.wheels) {
        ++wheelsPerAxle[static_cast<size_t>(w.axle)];
        if (w.axle == 0) frontZ = std::min(frontZ, w.position.z);
        if (w.axle == fit.axles - 1) rearZ = std::max(rearZ, w.position.z);
    }
    float frontShare = 1.f;
    if (fit.axles > 1 && rearZ - frontZ > 1e-3f) frontShare = std::clamp((rearZ - comBody.z) / (rearZ - frontZ), 0.05f, 0.95f);
    en.wheelBase = fit.axles > 1 ? std::max(rearZ - frontZ, 0.3f) : 1.f;
    for (auto& w : fit.wheels) {
        float axleShare = fit.axles == 1 ? 1.f
                          : w.axle == 0  ? frontShare
                                         : (1.f - frontShare) / static_cast<float>(fit.axles - 1);
        float wheelLoad = en.mass * g * axleShare / static_cast<float>(std::max(wheelsPerAxle[static_cast<size_t>(w.axle)], 1));
        JPH::Vec3 r = JPH::Vec3(bodyXf * toJolt(w.position) - com);
        JPH::Vec3 n = bodyRot.Multiply3x3(JPH::Vec3::sAxisY());
        JPH::Vec3 rn = r.Cross(n);
        float effMass = 1.f / std::max(invMass + rn.Dot(invInertia.Multiply3x3(rn)), 1e-9f);
        float omega = 2.f * kPi * w.frequency;
        float sag = wheelLoad / std::max(effMass * omega * omega, 1e-6f);
        w.restLength = std::clamp(w.maxLength - sag, w.minLength + 0.01f, w.maxLength);
        if (w.maxLength - sag < w.minLength) {
            host_.warn(label + ": suspension too soft for the weight on " + w.name +
                       " (it rests on the bump stop): raise suspensionFrequency or suspensionMaxLength");
        }
    }

    JPH::VehicleConstraintSettings vs;
    vs.mUp = JPH::Vec3::sAxisY();
    vs.mForward = JPH::Vec3(0, 0, -1);
    vs.mMaxPitchRollAngle = radians(std::clamp(v.maxTilt, 5.f, 180.f));
    vs.mConstraintPriority = static_cast<JPH::uint32>(e);  // unique => deterministic order
    const auto longCurve = curveFrom(v.longitudinalCurve, kLongitudinalCurve);
    const auto latCurve = curveFrom(v.lateralCurve, kLateralCurve);
    float maxSteer = 0.f;
    for (const auto& w : fit.wheels) {
        auto* ws = new JPH::WheelSettingsWV();
        ws->mPosition = toJolt(w.position + Vec3{0.f, w.restLength, 0.f});
        ws->mSuspensionForcePoint = toJolt(w.position);
        ws->mSuspensionDirection = JPH::Vec3(0, -1, 0);
        ws->mSteeringAxis = JPH::Vec3::sAxisY();
        ws->mWheelUp = JPH::Vec3::sAxisY();
        ws->mWheelForward = JPH::Vec3(0, 0, -1);
        ws->mSuspensionMinLength = w.minLength;
        ws->mSuspensionMaxLength = w.maxLength;
        ws->mSuspensionSpring.mFrequency = w.frequency;
        ws->mSuspensionSpring.mDamping = w.damping;
        ws->mRadius = w.radius;
        ws->mWidth = w.width;
        float wheelMass = std::clamp(en.mass * 0.025f, 3.f, 120.f);  // tire, rim, brake and half shaft
        ws->mInertia = 0.5f * wheelMass * w.radius * w.radius;
        ws->mAngularDamping = 0.2f;
        ws->mMaxSteerAngle = w.steers ? radians(w.maxSteerDeg) : 0.f;
        ws->mMaxBrakeTorque = w.brakeTorque;
        ws->mMaxHandBrakeTorque = w.handbrake ? w.handbrakeTorque : 0.f;
        ws->mLongitudinalFriction = linearCurve(longCurve, w.longitudinalGrip);
        ws->mLateralFriction = linearCurve(latCurve, w.lateralGrip);
        vs.mWheels.push_back(ws);
        float peak = 0.f;
        for (const auto& [x, y] : longCurve) peak = std::max(peak, y);
        en.peakLongitudinal.push_back(peak * w.longitudinalGrip);
        if (w.steers) maxSteer = std::max(maxSteer, w.maxSteerDeg);
    }
    en.maxSteerDeg = maxSteer > 0 ? maxSteer : 30.f;

    auto* cs = new JPH::WheeledVehicleControllerSettings();
    cs->mEngine.mMaxTorque = std::max(0.f, v.maxTorque);
    cs->mEngine.mMinRPM = en.minRpm;
    cs->mEngine.mMaxRPM = en.maxRpm;
    cs->mEngine.mInertia = std::max(0.01f, v.engineInertia);
    cs->mEngine.mAngularDamping = std::max(0.f, v.engineDamping);
    cs->mEngine.mNormalizedTorque = linearCurve(curveFrom(v.torqueCurve, kTorqueCurve), 1.f);
    en.manual = v.transmission == "manual";
    cs->mTransmission.mMode = en.manual ? JPH::ETransmissionMode::Manual : JPH::ETransmissionMode::Auto;
    std::vector<float> ratios;
    if (v.gearRatios.isArray()) {
        for (const auto& r : v.gearRatios.elements()) {
            if (r.isNumber() && r.asFloat(0.f) > 0.f) ratios.push_back(r.asFloat(1.f));
        }
    }
    if (ratios.empty()) ratios = kGearRatios;
    cs->mTransmission.mGearRatios.assign(ratios.begin(), ratios.end());
    cs->mTransmission.mReverseGearRatios = {-std::max(0.1f, v.reverseRatio)};
    en.forwardGears = static_cast<int>(ratios.size());
    cs->mTransmission.mSwitchTime = std::max(0.f, v.shiftTime);
    cs->mTransmission.mClutchReleaseTime = std::max(0.f, v.shiftTime * 0.6f);
    cs->mTransmission.mSwitchLatency = 0.3f;
    cs->mTransmission.mShiftUpRPM = std::clamp(v.shiftUpRpm, en.minRpm + 100.f, en.maxRpm - 10.f);
    cs->mTransmission.mShiftDownRPM = std::clamp(v.shiftDownRpm, 50.f, cs->mTransmission.mShiftUpRPM - 50.f);
    cs->mTransmission.mClutchStrength = std::max(0.1f, v.clutchStrength);
    const float limitedSlip = v.limitedSlip >= 10.f ? FLT_MAX : std::max(1.01f, v.limitedSlip);
    cs->mDifferentialLimitedSlipRatio = limitedSlip;
    float frontRatio = 0.f, otherRatio = 0.f;
    int frontDiffs = 0, otherDiffs = 0;
    for (int a = 0; a < fit.axles; ++a) {
        int left = -1, right = -1;
        float minX = 1e30f, maxX = -1e30f;
        for (size_t i = 0; i < fit.wheels.size(); ++i) {
            const WheelSetup& w = fit.wheels[i];
            if (w.axle != a || !w.driven) continue;
            if (w.position.x < minX) {
                minX = w.position.x;
                left = static_cast<int>(i);
            }
            if (w.position.x > maxX) {
                maxX = w.position.x;
                right = static_cast<int>(i);
            }
        }
        if (left < 0) continue;
        if (right == left) right = -1;
        JPH::VehicleDifferentialSettings d;
        d.mLeftWheel = left;
        d.mRightWheel = right;
        d.mDifferentialRatio = std::max(0.1f, v.differentialRatio);
        d.mLimitedSlipRatio = limitedSlip;
        cs->mDifferentials.push_back(d);
        (a == 0 ? frontDiffs : otherDiffs)++;
    }
    if (!cs->mDifferentials.empty()) {
        if (v.drive == "awd" && frontDiffs > 0 && otherDiffs > 0) {
            frontRatio = std::clamp(v.frontTorqueSplit, 0.f, 1.f) / static_cast<float>(frontDiffs);
            otherRatio = (1.f - std::clamp(v.frontTorqueSplit, 0.f, 1.f)) / static_cast<float>(otherDiffs);
        } else {
            frontRatio = otherRatio = 1.f / static_cast<float>(cs->mDifferentials.size());
        }
        float sum = 0.f;
        for (auto& d : cs->mDifferentials) {
            bool front = fit.wheels[static_cast<size_t>(d.mLeftWheel)].axle == 0;
            d.mEngineTorqueRatio = front ? frontRatio : otherRatio;
            sum += d.mEngineTorqueRatio;
        }
        if (sum <= 1e-6f) {
            for (auto& d : cs->mDifferentials) d.mEngineTorqueRatio = 1.f / static_cast<float>(cs->mDifferentials.size());
        } else {
            for (auto& d : cs->mDifferentials) d.mEngineTorqueRatio /= sum;
        }
    }
    vs.mController = cs;
    for (int a = 0; a < fit.axles; ++a) {
        int left = -1, right = -1;
        float minX = 1e30f, maxX = -1e30f;
        for (size_t i = 0; i < fit.wheels.size(); ++i) {
            const WheelSetup& w = fit.wheels[i];
            if (w.axle != a) continue;
            if (w.position.x < minX) {
                minX = w.position.x;
                left = static_cast<int>(i);
            }
            if (w.position.x > maxX) {
                maxX = w.position.x;
                right = static_cast<int>(i);
            }
        }
        float stiffness = a == 0 ? v.antiRollFront : v.antiRollRear;
        if (left < 0 || right == left || stiffness <= 0.f) continue;
        JPH::VehicleAntiRollBar bar;
        bar.mLeftWheel = left;
        bar.mRightWheel = right;
        bar.mStiffness = stiffness;
        vs.mAntiRollBars.push_back(bar);
    }

    en.constraint = new JPH::VehicleConstraint(body, vs);
    en.controller = static_cast<JPH::WheeledVehicleController*>(en.constraint->GetController());
    en.tester = new JPH::VehicleCollisionTesterCastCylinder(objectLayer(kDefault, false), 0.1f);
    en.constraint->SetVehicleCollisionTester(en.tester);
    en.wheels = std::move(fit.wheels);
    const size_t n = en.wheels.size();
    en.longitudinalScale.assign(n, 1.f);
    en.lateralScale.assign(n, 1.f);
    en.surfaceGrip.assign(n, 1.f);
    en.absHit.assign(n, 0);
    en.visualAngle.assign(n, 0.f);
    en.spin.assign(n, 0.f);
    if (const AudioSource* audio = s.get<AudioSource>(e)) en.audioVolume = audio->volume;

    Entry* self = entry.get();  // entries live in unique_ptrs: the address is stable
    // Surfaces: friction >= 0.5 (asphalt, concrete, the default) gives the tire its full grip; ice
    // (0.05) or mud (0.25) scale it down. Per-collider friction overrides are honored.
    en.constraint->SetCombineFriction([self](JPH::uint wheel, float& longitudinal, float& lateral, const JPH::Body& ground,
                                             const JPH::SubShapeID& sub) {
        float friction = ground.GetFriction();
        const JPH::PhysicsMaterial* m = ground.GetShape()->GetMaterial(sub);
        if (m && m != JPH::PhysicsMaterial::sDefault.GetPtr()) friction = static_cast<const SurfaceMaterial*>(m)->friction;
        float grip = std::clamp(friction / 0.5f, 0.f, 1.f);
        if (wheel < self->surfaceGrip.size()) self->surfaceGrip[wheel] = grip;
        longitudinal *= grip;
        lateral *= grip;
    });
    en.controller->SetTireMaxImpulseCallback([self](JPH::uint wheel, float& outLongitudinal, float& outLateral, float suspensionImpulse,
                                                    float longitudinalFriction, float lateralFriction, float longitudinalSlip,
                                                    float, float) {
        if (wheel >= self->wheels.size()) {
            outLongitudinal = longitudinalFriction * suspensionImpulse;
            outLateral = lateralFriction * suspensionImpulse;
            return;
        }
        float longitudinal = longitudinalFriction, lateral = lateralFriction;
        if (self->absHit[wheel]) {
            longitudinal = std::max(longitudinal, self->peakLongitudinal[wheel] * self->surfaceGrip[wheel]);
        } else if (self->braking && longitudinalSlip > 0.9f) {
            lateral *= 0.35f;  // a locked tire slides: little steering
        }
        outLongitudinal = longitudinal * suspensionImpulse * self->longitudinalScale[wheel];
        outLateral = lateral * suspensionImpulse * self->lateralScale[wheel];
    });

    if (previous && previous->wheels.size() == n && previous->stepped) {
        // Rebuilt while driving (vehicle_tune): keep the engine and wheels spinning.
        en.controller->GetEngine().SetCurrentRPM(previous->controller->GetEngine().GetCurrentRPM());
        for (JPH::uint i = 0; i < n; ++i) {
            en.constraint->GetWheel(i)->SetAngularVelocity(previous->constraint->GetWheel(i)->GetAngularVelocity());
        }
        en.steer = previous->steer;
        en.reversing = previous->reversing;
        en.requestedGear = previous->requestedGear;
        en.lastVelocity = previous->lastVelocity;
        en.hasLastVelocity = previous->hasLastVelocity;
        en.stepped = true;
    }
    en.lastGearWritten = v.gear;
    if (en.manual) en.requestedGear = std::clamp(v.gear, -1, en.forwardGears);

    host_.system->AddConstraint(en.constraint);
    host_.system->AddStepListener(en.constraint);
    entries_[e] = std::move(entry);
    return true;
}

// ---------------------------------------------------------------------------
// Stepping
// ---------------------------------------------------------------------------

void VehicleSet::preStep(const Scene& s, float dt) {
    auto start = std::chrono::steady_clock::now();
    JPH::BodyInterface& bi = host_.system->GetBodyInterfaceNoLock();
    for (auto& [e, ptr] : entries_) {
        Entry& en = *ptr;
        const Vehicle* v = s.get<Vehicle>(e);
        if (!v) continue;
        VehicleInput in;
        if (en.override) {
            in = *en.override;
        } else if (v->control != "none") {
            in = {v->throttle, v->brake, v->steer, v->handbrake};
        }
        in.throttle = std::clamp(std::isfinite(in.throttle) ? in.throttle : 0.f, 0.f, 1.f);
        in.brake = std::clamp(std::isfinite(in.brake) ? in.brake : 0.f, 0.f, 1.f);
        in.steer = std::clamp(std::isfinite(in.steer) ? in.steer : 0.f, -1.f, 1.f);
        in.handbrake = std::clamp(std::isfinite(in.handbrake) ? in.handbrake : 0.f, 0.f, 1.f);
        en.input = in;

        JPH::Quat rot = bi.GetRotation(en.body);
        JPH::Vec3 vel = bi.GetLinearVelocity(en.body);
        JPH::Vec3 fwd = rot * JPH::Vec3(0, 0, -1);
        JPH::Vec3 up = rot * JPH::Vec3::sAxisY();
        JPH::Vec3 right = rot * JPH::Vec3::sAxisX();
        const float forwardSpeed = vel.Dot(fwd);
        JPH::Vec3 flat = vel - up * vel.Dot(up);
        const float flatSpeed = flat.Length();
        int grounded = 0;
        for (const JPH::Wheel* w : en.constraint->GetWheels()) grounded += w->HasContact() ? 1 : 0;

        // Body slip (drift) angle: + when the car travels to the right of where it points.
        en.bodySlip = flatSpeed > 1.f ? degrees(std::atan2(flat.Dot(right), std::max(std::fabs(flat.Dot(fwd)), 1e-3f))) : 0.f;
        if (forwardSpeed < 0) en.bodySlip = -en.bodySlip;

        // Pedals: automatic gearboxes reverse when braking at a standstill (and brake when the
        // driver asks for the other direction while still rolling).
        float forward = in.throttle, brake = in.brake;
        if (!en.manual) {
            if (!en.reversing) {
                if (v->autoReverse && in.brake > 0.1f && in.throttle < 0.05f && std::fabs(forwardSpeed) < 0.6f) en.reversing = true;
            } else if ((in.throttle > 0.05f && forwardSpeed > -0.6f) || (in.brake < 0.05f && forwardSpeed > 0.6f)) {
                en.reversing = false;
            }
            if (en.reversing) {
                forward = -in.brake;
                brake = forwardSpeed < -0.6f ? in.throttle : 0.f;
            } else if (forwardSpeed < -1.f && in.throttle > 0.05f) {
                forward = 0.f;  // still rolling backward: stop first
                brake = std::max(brake, in.throttle);
            }
        } else {
            en.reversing = false;
            if (v->gear != en.lastGearWritten) en.requestedGear = std::clamp(v->gear, -1, en.forwardGears);
            en.controller->GetTransmission().Set(en.requestedGear, 1.f);
        }

        // Steering: less lock at speed, rate-limited like a real steering rack, plus the drift
        // assist's automatic counter-steer.
        float lockScale = 1.f - std::clamp(v->speedSensitiveSteering, 0.f, 1.f) * 0.65f * std::clamp(flatSpeed / 35.f, 0.f, 1.f);
        float target = in.steer * lockScale;
        const float drift = std::clamp(v->driftAssist, 0.f, 1.f);
        en.driftActive = false;
        if (drift > 0 && forwardSpeed > 4.f && grounded >= 2 && std::fabs(en.bodySlip) > 4.f && !en.reversing) {
            target += drift * 0.7f * std::clamp(en.bodySlip / std::max(en.maxSteerDeg, 1.f), -1.f, 1.f);
            en.driftActive = true;
        }
        target = std::clamp(target, -1.f, 1.f);
        if (v->steerSpeed > 0) {
            float delta = target - en.steer;
            bool returning = std::fabs(target) < std::fabs(en.steer) || target * en.steer < 0;
            float maxDelta = v->steerSpeed * dt * (returning ? 1.8f : 1.f);
            en.steer += std::clamp(delta, -maxDelta, maxDelta);
        } else {
            en.steer = target;
        }

        // Wheel slip after the last step: tire surface speed minus ground speed, relative to at least
        // 3 m/s (Jolt's slip ratio explodes near a standstill). + = spinning, - = skidding.
        std::vector<float> groundSpeed(en.wheels.size(), 0.f);
        {
            JPH::BodyLockRead lock(host_.system->GetBodyLockInterfaceNoLock(), en.body);
            for (size_t i = 0; i < en.wheels.size() && lock.Succeeded(); ++i) {
                const JPH::Wheel* w = en.constraint->GetWheel(static_cast<JPH::uint>(i));
                en.spin[i] = 0.f;
                if (!w->HasContact()) continue;
                JPH::Vec3 rel = lock.GetBody().GetPointVelocity(w->GetContactPosition()) - w->GetContactPointVelocity();
                groundSpeed[i] = rel.Dot(w->GetContactLongitudinal());
                float surface = w->GetAngularVelocity() * w->GetSettings()->mRadius;
                en.spin[i] = (std::fabs(surface) - std::fabs(groundSpeed[i])) / std::max(std::fabs(groundSpeed[i]), 3.f);
            }
        }

        // Anti-lock brakes (an ideal ABS): while braking, each tire brakes with its peak friction even
        // if Jolt locked the wheel, and keeps its cornering grip. Without ABS a locked tire slides at the
        // lower locked friction and barely steers (see the tire callback).
        en.abs = v->abs;
        en.braking = brake > 0.f;
        for (size_t i = 0; i < en.wheels.size(); ++i) {
            bool on = v->abs && brake > 0.f && std::fabs(groundSpeed[i]) > 1.5f;
            en.absHit[i] = on ? 1 : 0;
        }

        // Traction control: backs off the throttle while the driven wheels spin.
        en.tractionControl = v->tractionControl;
        if (v->tractionControl) {
            float maxSpin = 0.f;
            for (size_t i = 0; i < en.wheels.size(); ++i) {
                if (en.wheels[i].driven) maxSpin = std::max(maxSpin, en.spin[i]);
            }
            en.spinFiltered += (maxSpin - en.spinFiltered) * std::min(1.f, dt / 0.08f);
            // Wheelspin in a straight line still accelerates (the tire keeps most of its grip); it only
            // hurts when it steps the car out sideways. So the throttle is trimmed while the driven
            // wheels spin *and* the car slides (power oversteer, donuts), not on a straight launch.
            const float allowed = 0.35f;
            const bool sliding = std::fabs(en.bodySlip) > 5.f || (flatSpeed < 6.f && std::fabs(in.steer) > 0.3f);
            if (en.spinFiltered > allowed && sliding && std::fabs(forward) > 0.2f) {
                en.tractionScale = std::max(0.45f, en.tractionScale - (en.spinFiltered - allowed) * 3.f * dt);
            } else {
                en.tractionScale = std::min(1.f, en.tractionScale + 1.5f * dt);
            }
            forward *= en.tractionScale;
        } else {
            en.tractionScale = 1.f;
        }

        // Drift assist grip shaping: the handbrake and power break the rear loose more easily.
        for (size_t i = 0; i < en.wheels.size(); ++i) {
            float lat = 1.f;
            if (drift > 0 && en.wheels[i].axle > 0) {
                lat -= 0.35f * drift * in.handbrake;
                if (in.throttle > 0.6f && std::fabs(en.bodySlip) > 6.f) lat -= 0.15f * drift;
            }
            en.lateralScale[i] = std::max(lat, 0.2f);
            en.longitudinalScale[i] = 1.f;
        }

        en.applied = {std::max(forward, 0.f), brake, en.steer, in.handbrake};
        if (forward < 0) en.applied.throttle = -forward;
        en.controller->SetDriverInput(forward, en.steer, brake, in.handbrake);
        bool anyInput = std::fabs(forward) > 0 || brake > 0 || in.handbrake > 0 || std::fabs(en.steer) > 1e-3f;
        if (anyInput) bi.ActivateBody(en.body);

        // Aero: downforce grows with the square of the forward speed, drag opposes the motion.
        const float speedSq = vel.LengthSq();
        if (speedSq > 0.25f && bi.IsActive(en.body)) {
            JPH::Vec3 force = -up * (std::max(v->downforce, 0.f) * forwardSpeed * forwardSpeed);
            force -= vel.Normalized() * (std::max(v->drag, 0.f) * speedSq);
            bi.AddForce(en.body, force, JPH::EActivation::DontActivate);
        }

        // Drift assist: a sliding car keeps its momentum along its nose and its rotation in check,
        // so slides are long, controllable arcs instead of spins.
        if (en.driftActive) {
            JPH::Vec3 w = bi.GetAngularVelocity(en.body);
            float yaw = w.Dot(up);
            float maxYaw = flatSpeed * std::tan(radians(en.maxSteerDeg)) / std::max(en.wheelBase, 0.5f) * (1.f + 0.6f * drift) + 0.4f;
            if (std::fabs(yaw) > maxYaw) {
                float limited = (yaw > 0 ? maxYaw : -maxYaw);
                w += up * ((limited - yaw) * std::min(1.f, drift * 8.f * dt));
                bi.SetAngularVelocity(en.body, w);
            }
            float slip = radians(en.bodySlip);
            float turn = std::clamp(std::fabs(slip), 0.f, drift * 0.9f * dt) * (slip > 0 ? 1.f : -1.f);
            // Rotate the horizontal velocity toward the heading by `turn` (keeps its magnitude).
            JPH::Quat q = JPH::Quat::sRotation(up, turn);
            JPH::Vec3 vertical = up * vel.Dot(up);
            bi.SetLinearVelocity(en.body, q * flat + vertical);
        }
    }
    lastStepMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}


void VehicleSet::postStep(Scene* s, float dt, int collisionSteps) {
    auto start = std::chrono::steady_clock::now();
    stepped_ = true;
    JPH::BodyInterface& bi = host_.system->GetBodyInterfaceNoLock();
    for (auto& [e, ptr] : entries_) {
        Entry& en = *ptr;
        en.stepped = true;
        en.subDt = dt / static_cast<float>(std::max(collisionSteps, 1));
        // Accelerations (smoothed over ~0.1 s) for g readouts.
        JPH::Vec3 vel = bi.GetLinearVelocity(en.body);
        JPH::Quat rot = bi.GetRotation(en.body);
        Vec3 v = fromJolt(vel);
        if (en.hasLastVelocity && dt > 0) {
            Vec3 a = (v - en.lastVelocity) / dt;
            float lat = dot(a, fromJolt(rot * JPH::Vec3::sAxisX())) / 9.81f;
            float lon = dot(a, fromJolt(rot * JPH::Vec3(0, 0, -1))) / 9.81f;
            float k = std::min(1.f, dt / 0.1f);
            en.lateralG += (lat - en.lateralG) * k;
            en.longitudinalG += (lon - en.longitudinalG) * k;
        }
        en.lastVelocity = v;
        en.hasLastVelocity = true;
        float targetLoad = std::clamp(std::fabs(en.applied.throttle) * en.controller->GetTransmission().GetClutchFriction(), 0.f, 1.f);
        en.load += (targetLoad - en.load) * std::min(1.f, dt / 0.15f);
        if (!s) continue;

        auto t = telemetry(e);
        Vehicle* comp = s->get<Vehicle>(e);
        if (!t || !comp) continue;
        comp->speed = std::round(t->speedKmh * 100.f) / 100.f;
        comp->rpm = std::round(t->rpm);
        comp->gear = t->gear;
        comp->wheelsOnGround = t->wheelsOnGround;
        comp->skid = std::round(t->skid * 1000.f) / 1000.f;
        en.lastGearWritten = t->gear;

        // Wheel visuals: rest pose rotated by spin and steer about the wheel center, moved with the suspension.
        Decomposed chassis = decompose(s->worldMatrix(e));
        for (size_t i = 0; i < en.wheels.size(); ++i) {
            const WheelSetup& ws = en.wheels[i];
            if (ws.visual == kNoEntity) continue;
            Transform* tr = s->get<Transform>(ws.visual);
            if (!tr) continue;
            const JPH::Wheel* w = en.constraint->GetWheel(static_cast<JPH::uint>(i));
            Vec3 centerBody = ws.position + Vec3{0.f, ws.restLength - w->GetSuspensionLength(), 0.f};
            Vec3 centerChassis = divide(centerBody, chassis.scale);
            float omega = en.absHit[i] && std::fabs(w->GetAngularVelocity()) < 1e-3f ? 0.92f * t->speedKmh / 3.6f / ws.radius
                                                                                     : w->GetAngularVelocity();
            en.visualAngle[i] = std::fmod(en.visualAngle[i] + omega * dt, 2.f * kPi);
            Mat4 spinSteer = Mat4::rotateEulerDeg({-degrees(en.visualAngle[i]), degrees(w->GetSteerAngle()), 0.f});
            Mat4 m = Mat4::translate(centerChassis) * spinSteer * Mat4::translate(-ws.restCenterChassis) * ws.restInChassis;
            Mat4 local = ws.chassisFromParent.inverse() * m;
            Decomposed d = decompose(local);
            tr->position = d.translation;
            tr->rotation = quatToEuler(d.rotation);
        }

        // Engine audio: pitch follows the rpm, volume the load.
        if (comp->engineAudio) {
            if (AudioSource* audio = s->get<AudioSource>(e)) {
                if (en.audioVolume < 0) en.audioVolume = audio->volume;
                float rpmFraction = std::clamp((t->rpm - en.minRpm) / std::max(en.maxRpm - en.minRpm, 1.f), 0.f, 1.f);
                audio->pitch = std::round((0.6f + 1.5f * rpmFraction) * 1000.f) / 1000.f;
                audio->volume = std::round(en.audioVolume * (0.4f + 0.6f * en.load) * 1000.f) / 1000.f;
            }
        }
    }
    if (s && !entries_.empty()) s->markDirty();
    lastStepMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::vector<EntityId> VehicleSet::entities() const {
    std::vector<EntityId> out;
    for (const auto& [e, en] : entries_) out.push_back(e);
    return out;
}

std::optional<VehicleTelemetry> VehicleSet::telemetry(EntityId e) const {
    auto it = entries_.find(e);
    if (it == entries_.end()) return std::nullopt;
    const Entry& en = *it->second;
    JPH::BodyLockRead lock(host_.system->GetBodyLockInterfaceNoLock(), en.body);
    if (!lock.Succeeded()) return std::nullopt;
    const JPH::Body& body = lock.GetBody();
    VehicleTelemetry t;
    t.entity = e;
    const JPH::RMat44 xf = body.GetWorldTransform();
    t.position = fromJolt(JPH::Vec3(body.GetPosition()));
    t.velocity = fromJolt(body.GetLinearVelocity());
    t.angularVelocity = fromJolt(body.GetAngularVelocity());
    t.forward = fromJolt(xf.Multiply3x3(JPH::Vec3(0, 0, -1)));
    t.right = fromJolt(xf.Multiply3x3(JPH::Vec3::sAxisX()));
    t.up = fromJolt(xf.Multiply3x3(JPH::Vec3::sAxisY()));
    t.speedKmh = dot(t.velocity, t.forward) * kKmh;
    t.rpm = en.controller->GetEngine().GetCurrentRPM();
    t.gear = en.controller->GetTransmission().GetCurrentGear();
    t.clutch = en.controller->GetTransmission().GetClutchFriction();
    t.shifting = en.controller->GetTransmission().IsSwitchingGear();
    t.input = en.input;
    t.applied = en.applied;
    t.reversing = en.reversing;
    t.tractionScale = en.tractionScale;
    t.bodySlipAngle = en.bodySlip;
    t.lateralG = en.lateralG;
    t.longitudinalG = en.longitudinalG;
    t.load = en.load;
    t.mass = en.mass;
    t.centerOfMass = fromJolt(JPH::Vec3(body.GetCenterOfMassPosition()));
    t.stepped = en.stepped;
    t.driftAssistActive = en.driftActive;
    for (size_t i = 0; i < en.wheels.size(); ++i) {
        const WheelSetup& ws = en.wheels[i];
        const auto* w = static_cast<const JPH::WheelWV*>(en.constraint->GetWheel(static_cast<JPH::uint>(i)));
        const JPH::WheelSettings* settings = w->GetSettings();
        WheelTelemetry wt;
        wt.visual = ws.visual;
        wt.name = ws.name;
        wt.axle = ws.axle;
        wt.left = ws.left;
        wt.steers = ws.steers;
        wt.driven = ws.driven;
        wt.handbrake = ws.handbrake;
        wt.radius = ws.radius;
        wt.width = ws.width;
        wt.suspensionMin = ws.minLength;
        wt.suspensionMax = ws.maxLength;
        wt.restLength = ws.restLength;
        wt.suspensionLength = en.stepped ? w->GetSuspensionLength() : ws.restLength;
        wt.compression = std::clamp((ws.maxLength - wt.suspensionLength) / std::max(ws.maxLength - ws.minLength, 1e-3f), 0.f, 1.f);
        JPH::Vec3 mountLocal = settings->mPosition;
        wt.mount = fromJolt(JPH::Vec3(xf * mountLocal));
        wt.down = fromJolt(xf.Multiply3x3(settings->mSuspensionDirection));
        wt.center = wt.mount + wt.down * wt.suspensionLength;
        JPH::Vec3 f, u, r;
        en.constraint->GetWheelLocalBasis(w, f, u, r);
        wt.forward = fromJolt(xf.Multiply3x3(f));
        wt.right = fromJolt(xf.Multiply3x3(r));
        wt.steerAngle = degrees(w->GetSteerAngle());
        wt.angularVelocity = w->GetAngularVelocity();
        wt.surfaceGrip = en.surfaceGrip[i];
        if (en.stepped && w->HasContact()) {
            wt.contact = true;
            ++t.wheelsOnGround;
            wt.contactPoint = fromJolt(JPH::Vec3(w->GetContactPosition()));
            wt.contactNormal = fromJolt(w->GetContactNormal());
            wt.surface = host_.entityOf(w->GetContactBodyID().GetIndexAndSequenceNumber());
            float inv = 1.f / std::max(en.subDt, 1e-6f);
            wt.load = std::max(0.f, w->GetSuspensionLambda() * inv);
            wt.longitudinalForce = w->GetLongitudinalLambda() * inv;
            wt.lateralForce = w->GetLateralLambda() * inv;
            wt.slipRatio = w->mLongitudinalSlip;
            wt.slipAngle = degrees(w->mLateralSlip);
            JPH::Vec3 rel = body.GetPointVelocity(w->GetContactPosition()) - w->GetContactPointVelocity();
            float speed = rel.Length();
            if (speed > 2.f) {
                float longitudinal = std::clamp((std::min(wt.slipRatio, 3.f) - 0.2f) / 0.6f, 0.f, 1.f);
                float lateral = std::clamp((wt.slipAngle - 7.f) / 15.f, 0.f, 1.f);
                wt.skid = std::max(longitudinal, lateral) * std::clamp(speed / 6.f, 0.f, 1.f);
            }
            t.skid = std::max(t.skid, wt.skid);
            t.absActive = t.absActive || en.absHit[i] != 0;
        }
        t.wheels.push_back(wt);
    }
    t.tractionControlActive = t.tractionControlActive || en.tractionScale < 0.99f;
    return t;
}

bool VehicleSet::setInput(EntityId e, std::optional<VehicleInput> input) {
    auto it = entries_.find(e);
    if (it == entries_.end()) return false;
    it->second->override = input;
    return true;
}

bool VehicleSet::shift(EntityId e, int gear) {
    auto it = entries_.find(e);
    if (it == entries_.end()) return false;
    it->second->requestedGear = std::clamp(gear, -1, it->second->forwardGears);
    return true;
}

VehicleStats VehicleSet::stats() const {
    VehicleStats st;
    st.vehicles = static_cast<int>(entries_.size());
    for (const auto& [e, en] : entries_) st.wheels += static_cast<int>(en->wheels.size());
    st.lastStepMs = lastStepMs_;
    return st;
}

}  // namespace sky::physics
