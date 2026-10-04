// Vehicles: the vehicle component (Jolt wheeled vehicle), presets, tools, Wander builtins,
// telemetry, wheel visuals, the chase camera, determinism and the test-drive autopilot.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "skywalker/engine/Engine.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/render/DebugViews.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Engine> makeVehicleEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Null;
    cfg.projectDir = (fs::temp_directory_path() / ("skywalker-vehicle-" + AssetDatabase::newGuid().substr(0, 8))).string();
    fs::create_directories(cfg.projectDir);
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("Vehicles", false);
    return e;
}

ToolResult callRaw(Engine& e, const char* tool, const std::string& args) {
    auto parsed = Json::parse(args);
    REQUIRE(parsed);
    return e.callTool(tool, parsed.value(), "agent:test");
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = callRaw(e, tool, args);
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

EntityId make(Engine& e, const std::string& doc, EntityId parent = kNoEntity) {
    Scene& s = e.scene();
    auto parsed = Json::parse(doc);
    REQUIRE(parsed);
    EntityId id = s.create(parsed.value().get("name").asString(), parent);
    Status st = s.applyEntityJson(id, parsed.value());
    INFO((st.ok() ? std::string() : st.error().message));
    REQUIRE(st);
    return id;
}

/// Flat ground and a car: an unscaled chassis root, a body mesh child and four cylinder wheels
/// (radius 0.33 m, wheel bottoms at the chassis origin height).
EntityId buildCar(Engine& e, const char* name = "Car", float x = 0.f) {
    if (e.scene().find("Ground") == kNoEntity) {
        make(e, R"({"name":"Ground","components":{"transform":{"position":[0,-0.5,0],"scale":[800,1,800]},"mesh":{"mesh":"cube"},
                   "collider":{}}})");
    }
    std::string root = std::string(R"({"name":")") + name + R"(","components":{"transform":{"position":[)" + std::to_string(x) +
                       R"(,0,0]}}})";
    EntityId car = make(e, root);
    make(e, R"({"name":"Body","components":{"transform":{"position":[0,0.65,0],"scale":[1.7,0.5,4.0]},"mesh":{"mesh":"cube"}}})", car);
    const char* wheels[4][2] = {{"wheel_fl", "-0.8,0.33,-1.3"}, {"wheel_fr", "0.8,0.33,-1.3"}, {"wheel_rl", "-0.8,0.33,1.3"},
                                {"wheel_rr", "0.8,0.33,1.3"}};
    for (auto& w : wheels) {
        make(e,
             std::string(R"({"name":")") + w[0] + R"(","components":{"transform":{"position":[)" + w[1] +
                 R"(],"rotation":[0,0,90],"scale":[0.66,0.25,0.66]},"mesh":{"mesh":"cylinder"}}})",
             car);
    }
    return car;
}

physics::VehicleTelemetry telemetry(Engine& e, EntityId car) {
    physics::PhysicsWorld* w = e.physics().playWorld();
    REQUIRE(w);
    auto t = w->vehicle(car);
    REQUIRE(t);
    return *t;
}

void drive(Engine& e, EntityId car, float throttle, float steer, float brake, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        Vehicle* v = e.scene().get<Vehicle>(car);
        REQUIRE(v);
        v->throttle = throttle;
        v->steer = steer;
        v->brake = brake;
        e.step(1);
    }
}

EntityId createScripted(Engine& e, const char* preset = "sports", const char* name = "Car") {
    EntityId car = buildCar(e, name);
    call(e, "vehicle_create",
         std::string(R"({"entity":")") + name + R"(","preset":")" + preset +
             R"(","overrides":{"control":"script"},"engine_sound":false,"input_actions":false})");
    return car;
}

}  // namespace

TEST_CASE("vehicle: presets build a working car with fitted wheels") {
    for (const char* preset : {"sports", "hatchback", "truck", "kart"}) {
        INFO(preset);
        auto e = makeVehicleEngine();
        EntityId car = buildCar(*e);
        Json r = call(*e, "vehicle_create", std::string(R"({"entity":"Car","preset":")") + preset + R"("})");
        const Vehicle* v = e->scene().get<Vehicle>(car);
        REQUIRE(v);
        CHECK(v->preset == preset);
        const RigidBody* rb = e->scene().get<RigidBody>(car);
        REQUIRE(rb);
        CHECK(rb->motion == "dynamic");
        CHECK(rb->mass == doctest::Approx(v->mass));
        const Collider* col = e->scene().get<Collider>(car);
        REQUIRE(col);
        CHECK(col->shape == "box");
        CHECK(col->offset.y - col->size.y * 0.5f > 0.1f);  // the chassis box clears the wheel bottoms
        REQUIRE(r.get("wheels").size() == 4);
        int driven = 0, steering = 0;
        for (const auto& w : r.get("wheels").elements()) {
            CHECK(w.get("radius").asFloat(0.f) == doctest::Approx(0.33f).epsilon(0.02));
            CHECK(w.get("width").asFloat(0.f) == doctest::Approx(0.25f).epsilon(0.02));
            driven += w.get("driven").asBool() ? 1 : 0;
            steering += w.get("steers").asBool() ? 1 : 0;
        }
        CHECK(steering == 2);
        CHECK(driven == (std::string(preset) == "truck" ? 4 : 2));
        CHECK(r.get("warnings").size() == 0);
        CHECK(e->scene().get<AudioSource>(car));                      // engine sound
        CHECK(e->actionMap().find("throttle") != nullptr);            // drive actions
        bool chase = false;
        for (EntityId id : e->scene().entities()) chase = chase || e->scene().get<ChaseCamera>(id);
        CHECK(chase);
        // One undoable edit.
        REQUIRE(e->history().undo());
        CHECK_FALSE(e->scene().get<Vehicle>(car));
        CHECK_FALSE(e->scene().get<RigidBody>(car));
    }
}

TEST_CASE("vehicle: the suspension holds the authored ride height at rest") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    e->play();
    e->step(180);
    auto t = telemetry(*e, car);
    CHECK(t.wheelsOnGround == 4);
    // The wheels were modeled touching the ground at the chassis origin height: the body rests there.
    CHECK(std::fabs(e->scene().worldMatrix(car).translation().y) < 0.035f);
    CHECK(std::fabs(t.speedKmh) < 0.5f);
    for (const auto& w : t.wheels) {
        INFO(w.name);
        CHECK(w.compression > 0.05f);
        CHECK(w.compression < 0.95f);
        CHECK(w.load > 1000.f);  // the car's weight is on its wheels
    }
    e->stop();
}

TEST_CASE("vehicle: straight-line acceleration, braking to a stop, steering yaws the right way") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    e->play();
    e->step(30);
    drive(*e, car, 1.f, 0.f, 0.f, 240);  // 4 s full throttle
    auto t = telemetry(*e, car);
    CHECK(t.speedKmh > 50.f);
    CHECK(t.gear >= 1);
    Vec3 p = e->scene().worldMatrix(car).translation();
    CHECK(p.z < -20.f);                // drove forward (-Z)
    CHECK(std::fabs(p.x) < 1.f);       // straight
    CHECK(e->scene().get<Vehicle>(car)->speed > 50.f);  // telemetry written back
    CHECK(e->scene().get<Vehicle>(car)->rpm > 1000.f);

    const float zBrake = p.z;
    for (int i = 0; i < 600 && telemetry(*e, car).speedKmh > 0.5f; ++i) drive(*e, car, 0.f, 0.f, 1.f, 1);
    t = telemetry(*e, car);
    CHECK(std::fabs(t.speedKmh) < 1.f);
    float distance = std::fabs(e->scene().worldMatrix(car).translation().z - zBrake);
    CHECK(distance < 60.f);
    CHECK(distance > 5.f);
    e->stop();

    // Steering right (steer > 0) turns the nose toward +X (clockwise seen from above).
    e->play();
    e->step(30);
    drive(*e, car, 0.6f, 1.f, 0.f, 150);
    t = telemetry(*e, car);
    CHECK(t.forward.x > 0.3f);
    CHECK(e->scene().worldMatrix(car).translation().x > 1.f);
    CHECK(t.angularVelocity.y < 0.f);  // yaw rate: clockwise
    e->stop();
}

TEST_CASE("vehicle: wheel visuals spin, steer and follow the suspension") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    EntityId fl = e->scene().find("wheel_fl");
    Transform rest = *e->scene().get<Transform>(fl);
    e->play();
    e->step(30);
    drive(*e, car, 0.5f, 1.f, 0.f, 40);
    Transform now = *e->scene().get<Transform>(fl);
    CHECK(std::fabs(now.position.x - rest.position.x) < 0.01f);
    CHECK(std::fabs(now.position.z - rest.position.z) < 0.01f);
    CHECK(std::fabs(now.position.y - rest.position.y) < 0.15f);
    // Rotation since rest, applied to the axle direction (+X; spinning about it leaves it unchanged):
    // steering right turns the axle toward +Z.
    Mat4 delta = now.local() * rest.local().inverse();
    Vec3 axle = normalize(delta.transformDir({1.f, 0.f, 0.f}));
    CHECK(axle.z > 0.1f);
    auto t = telemetry(*e, car);
    CHECK(t.wheels[0].steerAngle < -5.f);  // Jolt convention: + is left
    e->stop();
    // Play restores the authored pose.
    CHECK(e->scene().get<Transform>(fl)->rotation.z == doctest::Approx(rest.rotation.z));
    CHECK(e->scene().get<Vehicle>(car)->speed == 0.f);
}

TEST_CASE("vehicle: the simulation replays identically and resets on stop") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    auto run = [&] {
        e->play();
        std::vector<float> trace;
        for (int i = 0; i < 420; ++i) {
            Vehicle* v = e->scene().get<Vehicle>(car);
            v->throttle = i < 200 ? 1.f : 0.3f;
            v->steer = i > 150 ? std::sin(static_cast<float>(i) * 0.05f) : 0.f;
            v->handbrake = (i > 300 && i < 330) ? 1.f : 0.f;
            e->step(1);
            const Transform* t = e->scene().get<Transform>(car);
            trace.push_back(t->position.x);
            trace.push_back(t->position.y);
            trace.push_back(t->position.z);
            trace.push_back(t->rotation.y);
            trace.push_back(e->scene().get<Vehicle>(car)->rpm);
            trace.push_back(e->scene().get<Transform>(e->scene().find("wheel_rr"))->rotation.x);
        }
        e->stop();
        return trace;
    };
    Vec3 start = e->scene().worldMatrix(car).translation();
    auto a = run();
    CHECK(e->scene().worldMatrix(car).translation().z == doctest::Approx(start.z));
    CHECK(e->scene().get<Vehicle>(car)->rpm == 0.f);
    auto b = run();
    REQUIRE(a.size() == b.size());
    size_t mismatches = 0;
    for (size_t i = 0; i < a.size(); ++i) mismatches += a[i] == b[i] ? 0 : 1;
    CHECK(mismatches == 0);
    CHECK(std::fabs(a[a.size() - 4]) > 10.f);  // it actually went somewhere
}

TEST_CASE("vehicle: Wander builtins drive the car and read its telemetry") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    Json b = Json::array({Json::object({{"name", "Driver"},
                                        {"source", R"(behavior Driver
  var kmh = 0
  var ground = 0
  var load = 0
  on tick
    vehicle_drive(self, 1, 0)
    let s = vehicle_state(self)
    kmh = vehicle_speed(self)
    ground = s.wheels_on_ground
    let w = vehicle_wheel(self, "rear_left")
    if w then
      load = w.load
    end
  end
end
)"}})});
    REQUIRE(e->scene().setBehaviors(car, b));
    e->play();
    e->step(180);
    const EntityRecord* r = e->scene().record(car);
    CHECK(r->vars.get("kmh").asFloat(0.f) > 20.f);
    CHECK(r->vars.get("ground").asFloat(0.f) == 4.f);
    CHECK(r->vars.get("load").asFloat(0.f) > 500.f);
    for (const auto& m : e->recentMessages()) CHECK(m.get("kind").asString() == "log");
    e->stop();
}

TEST_CASE("vehicle: the player drive actions and the chase camera") {
    auto e = makeVehicleEngine();
    EntityId car = buildCar(*e);
    call(*e, "vehicle_create", R"({"entity":"Car","preset":"hatchback"})");
    EntityId cam = kNoEntity;
    for (EntityId id : e->scene().entities()) {
        if (e->scene().get<ChaseCamera>(id)) cam = id;
    }
    REQUIRE(cam);
    e->play();
    e->step(20);
    call(*e, "sim_input", R"({"hold":["w"]})");
    e->step(240);
    auto t = telemetry(*e, car);
    CHECK(t.speedKmh > 30.f);
    Vec3 cp = e->scene().worldMatrix(cam).translation();
    Vec3 vp = e->scene().worldMatrix(car).translation();
    CHECK(cp.z > vp.z + 2.f);  // behind the car (it drives toward -Z)
    CHECK(cp.y > vp.y + 1.f);
    CHECK(length(cp - vp) < 12.f);
    CHECK(e->scene().get<Camera>(cam)->fov > 60.f);  // widened with speed
    const AudioSource* engineSound = e->scene().get<AudioSource>(car);  // the engine audio hook follows the revs
    REQUIRE(engineSound);
    CHECK(engineSound->clip == "audio/engine_loop.wav");
    CHECK(engineSound->pitch > 0.7f);
    e->stop();
}

TEST_CASE("vehicle: tools validate, inform and measure") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    // Unknown fields fail with a did-you-mean.
    ToolResult bad = callRaw(*e, "vehicle_tune", R"({"entity":"Car","set":{"maxTorq":600}})");
    CHECK(bad.isError);
    CHECK(bad.content.front().text.find("maxTorque") != std::string::npos);
    ToolResult badWheel = callRaw(*e, "vehicle_tune", R"({"entity":"Car","wheels":{"rearr":{"lateralGrip":1}}})");
    CHECK(badWheel.isError);
    CHECK(badWheel.content.front().text.find("rear") != std::string::npos);
    Json tuned = call(*e, "vehicle_tune", R"({"entity":"Car","set":{"maxTorque":600,"shiftUpRpm":9000}})");
    CHECK(tuned.get("changed").contains("maxTorque"));
    CHECK(tuned.get("warnings").size() > 0);  // shiftUpRpm above maxRpm
    Json perWheel = call(*e, "vehicle_tune", R"({"entity":"Car","wheels":{"rear":{"lateralGrip":1.1}}})");
    const Vehicle* v = e->scene().get<Vehicle>(car);
    REQUIRE(v->wheels.size() == 4);
    int rearOverrides = 0;
    for (const auto& w : v->wheels.elements()) rearOverrides += w.get("lateralGrip").isNumber() ? 1 : 0;
    CHECK(rearOverrides == 2);
    // A car without wheels.
    make(*e, R"({"name":"Crate","components":{"transform":{"position":[10,1,0]},"mesh":{"mesh":"cube"}}})");
    ToolResult noWheels = callRaw(*e, "vehicle_create", R"({"entity":"Crate"})");
    CHECK(noWheels.isError);
    CHECK(noWheels.content.front().text.find("wheel") != std::string::npos);
    ToolResult badPreset = callRaw(*e, "vehicle_create", R"({"entity":"Car","preset":"sprots"})");
    CHECK(badPreset.isError);
    CHECK(badPreset.content.front().text.find("sports") != std::string::npos);
    ToolResult badManeuver = callRaw(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"slalon"})");
    CHECK(badManeuver.isError);
    CHECK(badManeuver.content.front().text.find("slalom") != std::string::npos);
    // vehicle_info while editing shows the fitted setup.
    Json info = call(*e, "vehicle_info", R"({"entity":"Car"})");
    REQUIRE(info.get("vehicles").size() == 1);
    CHECK(info.get("vehicles")[size_t{0}].get("wheels").size() == 4);
    CHECK_FALSE(info.get("vehicles")[size_t{0}].get("simulated").asBool(true));
    // The vehicles debug view renders (overlay on the null renderer image).
    ToolResult cap = callRaw(*e, "viewport_capture", R"({"width":96,"height":54,"samples":1,"debug_view":"vehicles"})");
    CHECK_FALSE(cap.isError);
    ToolResult infoCap = callRaw(*e, "vehicle_info", R"({"entity":"Car","capture":true,"width":96,"height":54})");
    CHECK_FALSE(infoCap.isError);
    bool image = false;
    for (const auto& c : infoCap.content) image = image || c.type == ContentBlock::Type::Image;
    CHECK(image);
    CHECK(debugViewName(debugview::kVehicles) == std::string("vehicles"));
}

TEST_CASE("vehicle: the test-drive autopilot returns repeatable handling numbers") {
    auto e = makeVehicleEngine();
    EntityId car = createScripted(*e);
    Json accel = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"accel"})");
    float t100 = accel.get("accel").get("zeroTo100s").asFloat(-1.f);
    CHECK(t100 > 2.f);
    CHECK(t100 < 15.f);
    Json again = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"accel"})");
    CHECK(again.get("accel").get("zeroTo100s").asFloat(-2.f) == t100);
    Json brake = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"braking"})");
    float d = brake.get("braking").get("distanceM").asFloat(0.f);
    CHECK(d > 20.f);
    CHECK(d < 60.f);
    Json slalom = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"slalom","speed":50})");
    CHECK(slalom.get("slalom").get("completed").asBool());
    Json skid = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"skidpad"})");
    CHECK(skid.get("skidpad").get("lateralG").asFloat(0.f) > 0.5f);
    // Overrides try settings without editing: more torque = quicker.
    Json quick = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"accel","overrides":{"maxTorque":900}})");
    CHECK(quick.get("accel").get("zeroTo100s").asFloat(99.f) < t100);
    CHECK(e->scene().get<Vehicle>(car)->maxTorque < 900.f);
    // Custom keyframes.
    Json custom = call(*e, "vehicle_test_drive",
                       R"({"entity":"Car","maneuver":"custom","inputs":[{"t":0,"throttle":1},{"t":2,"throttle":0,"brake":1}],"duration":4,"trace":true})");
    CHECK(custom.get("custom").get("trace").size() > 10);
}

TEST_CASE("vehicle: perf stats and physics stats count vehicles") {
    auto e = makeVehicleEngine();
    createScripted(*e);
    e->play();
    e->step(10);
    Json perf = call(*e, "perf_stats", "{}");
    CHECK(perf.get("vehicles").get("vehicles").asInt() == 1);
    CHECK(perf.get("vehicles").get("wheels").asInt() == 4);
    CHECK(e->physics().playWorld()->stats().vehicles == 1);
    e->stop();
}

TEST_CASE("vehicle: a handbrake flick and power hold a drift; ABS shortens stops") {
    auto e = makeVehicleEngine();
    createScripted(*e);
    // Handbrake into a left turn, then throttle with a little counter-steer: a sustained slide.
    Json r = call(*e, "vehicle_test_drive",
                  R"({"entity":"Car","maneuver":"custom","duration":7,"inputs":[{"t":0,"throttle":1},
                      {"t":3.4,"throttle":0.3,"steer":-1,"handbrake":1},{"t":3.9,"throttle":1,"steer":-0.3,"handbrake":0},
                      {"t":4.3,"throttle":1,"steer":0.3}]})");
    CHECK(r.get("custom").get("maxDriftAngle").asFloat(0.f) > 15.f);
    CHECK(r.get("custom").get("maxDriftAngle").asFloat(99.f) < 70.f);  // held, not spun
    Json withAbs = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"braking"})");
    Json without = call(*e, "vehicle_test_drive", R"({"entity":"Car","maneuver":"braking","overrides":{"abs":false}})");
    CHECK(withAbs.get("braking").get("distanceM").asFloat(99.f) < without.get("braking").get("distanceM").asFloat(0.f));
    CHECK(without.get("braking").get("wheelsLocked").asBool());
    CHECK_FALSE(withAbs.get("braking").get("wheelsLocked").asBool());
}

TEST_CASE("vehicle: input_map adds the drive actions as a preset") {
    auto e = makeVehicleEngine();
    Json r = call(*e, "input_map", R"({"operation":"add_preset","preset":"drive"})");
    CHECK(r.get("changed").get("added").size() == 6);
    for (const char* a : {"throttle", "brake", "steer", "handbrake", "shift_up", "shift_down"}) CHECK(e->actionMap().find(a));
    Json again = call(*e, "input_map", R"({"operation":"add_preset","preset":"drive"})");
    CHECK(again.get("changed").get("added").size() == 0);  // the project's own bindings are kept
    call(*e, "input_map", R"({"operation":"add_preset","preset":"flying"})", false);
}
