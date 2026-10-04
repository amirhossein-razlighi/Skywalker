// Wander builtins for vehicles (docs/PHYSICS.md "Vehicles"): drive, shift, and read the live
// telemetry (speed, wheels, skids) that tire smoke, skid marks, sounds and HUDs hook into.

#include <algorithm>

#include "skywalker/engine/Engine.h"
#include "skywalker/physics/Vehicle.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

Vehicle& vehicleOf(CallContext& c, EntityRef id) {
    Vehicle* v = c.scene().get<Vehicle>(id);
    if (!v) {
        c.fail(c.def().name + "(): '" + c.scene().record(id)->name +
               "' has no vehicle component (vehicle_create builds one from a chassis and wheel_* children)");
    }
    return *v;
}

std::optional<physics::VehicleTelemetry> live(CallContext& c, EntityRef id) {
    Engine* engine = c.service<Engine>();
    if (!engine) return std::nullopt;
    physics::PhysicsWorld* world = engine->physics().playWorld();
    return world ? world->vehicle(id) : std::nullopt;
}

Value wheelValue(const physics::WheelTelemetry& w) {
    Value m = Value::map();
    MapObj& o = m.mutMap();
    o.set("name", Value::string(w.name));
    o.set("contact", Value::boolean(w.contact));
    o.set("point", Value::vec(w.contact ? w.contactPoint : w.center - Vec3{0.f, w.radius, 0.f}));
    o.set("normal", Value::vec(w.contact ? w.contactNormal : Vec3{0.f, 1.f, 0.f}));
    o.set("surface", w.contact && w.surface ? Value::entity(w.surface) : Value());
    o.set("center", Value::vec(w.center));
    o.set("load", Value::number(w.load));
    o.set("slip_ratio", Value::number(std::min(w.slipRatio, 100.f)));
    o.set("slip_angle", Value::number(w.slipAngle));
    o.set("skid", Value::number(w.skid));
    o.set("compression", Value::number(w.compression));
    o.set("steer", Value::number(w.steerAngle));
    o.set("rpm", Value::number(w.angularVelocity * 60.f / (2.f * kPi)));
    o.set("driven", Value::boolean(w.driven));
    return m;
}

void def(BuiltinRegistry& reg, const char* name, std::vector<BuiltinParam> params, TypeSet returns, const char* doc, const char* example,
         BuiltinImpl fn) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = returns;
    d.category = "vehicle";
    d.doc = doc;
    d.example = example;
    d.owner = "engine";
    d.fn = fn;
    reg.add(std::move(d));
}

}  // namespace

void registerVehicleBuiltins(BuiltinRegistry& reg) {
    def(reg, "vehicle_drive",
        {{"e", kTEntity}, {"throttle", kTNumber}, {"steer", kTNumber}, {"brake", kTNumber, true}, {"handbrake", kTNumber, true}},
        kTNone,
        "Sets a vehicle's inputs for this tick: throttle 0..1, steer -1 (left)..1 (right), brake 0..1, handbrake 0..1. "
        "Overrides the player's drive actions this tick; with control \"script\" it is the only driver. Holding brake at "
        "a standstill reverses (autoReverse).",
        "vehicle_drive(self, 1, axis(\"steer\"), 0)", [](CallContext& c) -> Value {
            Vehicle& v = vehicleOf(c, c.entity(0));
            v.throttle = static_cast<float>(std::clamp(c.number(1), 0.0, 1.0));
            v.steer = static_cast<float>(std::clamp(c.number(2), -1.0, 1.0));
            v.brake = c.argc() > 3 ? static_cast<float>(std::clamp(c.number(3), 0.0, 1.0)) : 0.f;
            v.handbrake = c.argc() > 4 ? static_cast<float>(std::clamp(c.number(4), 0.0, 1.0)) : 0.f;
            return {};
        });
    def(reg, "vehicle_shift", {{"e", kTEntity}, {"gear", kTNumber}}, kTNone,
        "Manual gearbox: selects a gear (-1 reverse, 0 neutral, 1..). Automatic gearboxes shift by themselves.",
        "if pressed(\"shift_up\") then vehicle_shift(self, self.vehicle.gear + 1) end", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            Vehicle& v = vehicleOf(c, id);
            if (v.transmission != "manual") c.fail("vehicle_shift(): the gearbox is automatic (set vehicle.transmission to \"manual\")");
            int gear = static_cast<int>(std::clamp(c.number(1), -1.0, 20.0));
            v.gear = gear;
            if (Engine* engine = c.service<Engine>()) {
                if (physics::PhysicsWorld* world = engine->physics().playWorld()) world->shiftVehicle(id, gear);
            }
            return {};
        });
    def(reg, "vehicle_speed", {{"e", kTEntity}}, kTNumber, "Speed along the vehicle's heading in km/h (negative reversing).",
        "if vehicle_speed(self) > 120 then ... end", [](CallContext& c) {
            EntityRef id = c.entity(0);
            Vehicle& v = vehicleOf(c, id);
            if (auto t = live(c, id)) return Value::number(t->speedKmh);
            return Value::number(v.speed);
        });
    def(reg, "vehicle_wheel", {{"e", kTEntity}, {"wheel", kTNumber | kTString}}, kTMap | kTNone,
        "One wheel's live state by index (0-based) or name (\"rear_left\"): contact, point, normal, surface, load (N), "
        "slip_ratio, slip_angle (degrees), skid 0..1, compression 0..1, steer, rpm, driven. none before play. Tire smoke "
        "and skid marks: spawn at `point` while `skid` > 0.3.",
        "let w = vehicle_wheel(self, \"rear_left\")", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            vehicleOf(c, id);
            auto t = live(c, id);
            if (!t) return Value();
            const physics::WheelTelemetry* found = nullptr;
            if (c.arg(1).isString()) {
                for (const auto& w : t->wheels) {
                    if (w.name == c.string(1)) found = &w;
                }
                if (!found) {
                    std::string names;
                    for (const auto& w : t->wheels) names += (names.empty() ? "" : ", ") + w.name;
                    c.fail("vehicle_wheel(): no wheel named '" + c.string(1) + "' (wheels: " + names + ")");
                }
            } else {
                double i = c.number(1);
                if (i < 0 || i >= static_cast<double>(t->wheels.size())) {
                    c.fail("vehicle_wheel(): index out of range (the vehicle has " + std::to_string(t->wheels.size()) + " wheels)");
                }
                found = &t->wheels[static_cast<size_t>(i)];
            }
            return wheelValue(*found);
        });
    def(reg, "vehicle_state", {{"e", kTEntity}}, kTMap,
        "Live vehicle state: speed (km/h), rpm, gear, wheels_on_ground, skid 0..1, drift_angle (degrees), lateral_g, "
        "longitudinal_g, load 0..1 and skidding (a list of contact points of sliding wheels, for smoke and marks).",
        "let s = vehicle_state(self)", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            Vehicle& v = vehicleOf(c, id);
            Value m = Value::map();
            MapObj& o = m.mutMap();
            auto t = live(c, id);
            o.set("speed", Value::number(t ? t->speedKmh : v.speed));
            o.set("rpm", Value::number(t ? t->rpm : v.rpm));
            o.set("gear", Value::number(t ? t->gear : v.gear));
            o.set("wheels_on_ground", Value::number(t ? t->wheelsOnGround : v.wheelsOnGround));
            o.set("skid", Value::number(t ? t->skid : v.skid));
            o.set("drift_angle", Value::number(t ? t->bodySlipAngle : 0.f));
            o.set("lateral_g", Value::number(t ? t->lateralG : 0.f));
            o.set("longitudinal_g", Value::number(t ? t->longitudinalG : 0.f));
            o.set("load", Value::number(t ? t->load : 0.f));
            std::vector<Value> skidding;
            if (t) {
                for (const auto& w : t->wheels) {
                    if (w.contact && w.skid > 0.3f) skidding.push_back(Value::vec(w.contactPoint));
                }
            }
            o.set("skidding", Value::list(std::move(skidding)));
            return m;
        });
}

}  // namespace sky
