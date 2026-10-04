// Vehicle presets: tuned `vehicle` component values for common vehicle types, in two handling
// styles. arcade = assists on (traction control, ABS, drift assist), quick steering, protected
// from rolling over; sim = raw tires and steering, may flip. vehicle_test_drive gives the numbers
// behind each (docs/PHYSICS.md "Vehicles").

#include <string>
#include <vector>

#include "skywalker/core/Strings.h"
#include "skywalker/physics/Vehicle.h"

namespace sky::physics {

const std::vector<std::string>& vehiclePresetNames() {
    static const std::vector<std::string> names{"sports", "hatchback", "truck", "kart"};
    return names;
}

const std::vector<std::string>& vehicleHandlingNames() {
    static const std::vector<std::string> names{"arcade", "sim"};
    return names;
}

namespace {

Json ratios(std::initializer_list<double> r) {
    Json a = Json::array();
    for (double x : r) a.push(x);
    return a;
}

Result<std::string> pick(const std::string& value, const std::vector<std::string>& valid, const char* what) {
    for (const auto& v : valid) {
        if (v == value) return value;
    }
    std::string list;
    for (const auto& v : valid) list += (list.empty() ? "" : ", ") + v;
    std::string guess = str::closest(value, valid, 3);
    return Error::make("invalid_preset", std::string("unknown vehicle ") + what + " \"" + value + "\"",
                       guess.empty() ? std::string(what) + "s: " + list : "did you mean \"" + guess + "\"? (" + list + ")");
}

}  // namespace

Result<Json> vehiclePreset(const std::string& presetName, const std::string& handlingName) {
    auto preset = pick(presetName, vehiclePresetNames(), "preset");
    if (!preset) return preset.error();
    auto handling = pick(handlingName, vehicleHandlingNames(), "handling");
    if (!handling) return handling.error();
    Json v = Json::object();
    float drift = 0.f;
    if (*preset == "sports") {
        // A light, rear-driven coupe: strong engine, six close gears, firm springs, a little aero.
        v = Json::object({{"mass", 1350},
                          {"centerOfMass", Json::array({0, -0.3, 0.1})},
                          {"suspensionMinLength", 0.08},
                          {"suspensionMaxLength", 0.3},
                          {"suspensionFrequency", 2.0},
                          {"suspensionDamping", 0.55},
                          {"maxSteerAngle", 30},
                          {"brakeTorque", 3200},
                          {"handbrakeTorque", 4500},
                          {"longitudinalGrip", 1.25},
                          {"lateralGrip", 1.12},
                          {"drive", "rwd"},
                          {"limitedSlip", 1.4},
                          {"differentialRatio", 3.6},
                          {"maxTorque", 520},
                          {"minRpm", 1000},
                          {"maxRpm", 7800},
                          {"engineInertia", 0.4},
                          {"gearRatios", ratios({3.2, 2.2, 1.6, 1.25, 1.0, 0.82})},
                          {"reverseRatio", 3.0},
                          {"shiftUpRpm", 7200},
                          {"shiftDownRpm", 3600},
                          {"shiftTime", 0.18},
                          {"antiRollFront", 700},
                          {"antiRollRear", 450},
                          {"downforce", 0.8},
                          {"drag", 0.36}});
        drift = 0.35f;
    } else if (*preset == "hatchback") {
        // A front-driven city car: modest engine, soft springs, understeers gently at the limit.
        v = Json::object({{"mass", 1150},
                          {"centerOfMass", Json::array({0, -0.25, -0.1})},
                          {"suspensionMinLength", 0.1},
                          {"suspensionMaxLength", 0.34},
                          {"suspensionFrequency", 1.5},
                          {"suspensionDamping", 0.5},
                          {"maxSteerAngle", 34},
                          {"brakeTorque", 2400},
                          {"handbrakeTorque", 3500},
                          {"longitudinalGrip", 1.0},
                          {"lateralGrip", 0.92},
                          {"drive", "fwd"},
                          {"limitedSlip", 10},
                          {"differentialRatio", 4.1},
                          {"maxTorque", 260},
                          {"minRpm", 900},
                          {"maxRpm", 6500},
                          {"engineInertia", 0.35},
                          {"gearRatios", ratios({3.4, 2.0, 1.4, 1.05, 0.85})},
                          {"reverseRatio", 3.2},
                          {"shiftUpRpm", 5800},
                          {"shiftDownRpm", 2600},
                          {"shiftTime", 0.25},
                          {"antiRollFront", 600},
                          {"antiRollRear", 250},
                          {"downforce", 0.2},
                          {"drag", 0.38}});
        drift = 0.2f;
    } else if (*preset == "truck") {
        // A heavy four-wheel-drive pickup: torque, long travel, soft and stable.
        v = Json::object({{"mass", 2600},
                          {"centerOfMass", Json::array({0, -0.35, 0})},
                          {"suspensionMinLength", 0.12},
                          {"suspensionMaxLength", 0.45},
                          {"suspensionFrequency", 1.3},
                          {"suspensionDamping", 0.45},
                          {"maxSteerAngle", 32},
                          {"brakeTorque", 5500},
                          {"handbrakeTorque", 7000},
                          {"longitudinalGrip", 0.95},
                          {"lateralGrip", 0.85},
                          {"drive", "awd"},
                          {"frontTorqueSplit", 0.4},
                          {"limitedSlip", 1.6},
                          {"differentialRatio", 3.7},
                          {"maxTorque", 700},
                          {"minRpm", 800},
                          {"maxRpm", 5000},
                          {"engineInertia", 0.8},
                          {"gearRatios", ratios({3.6, 2.2, 1.5, 1.1, 0.85, 0.7})},
                          {"reverseRatio", 3.4},
                          {"shiftUpRpm", 4200},
                          {"shiftDownRpm", 1800},
                          {"shiftTime", 0.3},
                          {"antiRollFront", 900},
                          {"antiRollRear", 600},
                          {"downforce", 0.0},
                          {"drag", 0.6}});
        drift = 0.15f;
    } else {  // kart
        // A go-kart: no suspension to speak of, a solid rear axle, one gear, very direct.
        v = Json::object({{"mass", 170},
                          {"centerOfMass", Json::array({0, -0.1, 0.1})},
                          {"suspensionMinLength", 0.0},
                          {"suspensionMaxLength", 0.07},
                          {"suspensionFrequency", 3.5},
                          {"suspensionDamping", 0.8},
                          {"maxSteerAngle", 28},
                          {"brakeTorque", 350},
                          {"handbrakeTorque", 600},
                          {"longitudinalGrip", 1.35},
                          {"lateralGrip", 1.25},
                          {"drive", "rwd"},
                          {"limitedSlip", 1.05},
                          {"differentialRatio", 5.3},
                          {"maxTorque", 42},
                          {"minRpm", 2000},
                          {"maxRpm", 9500},
                          {"engineInertia", 0.05},
                          {"gearRatios", ratios({1.0})},
                          {"reverseRatio", 1.0},
                          {"shiftUpRpm", 9300},
                          {"shiftDownRpm", 2500},
                          {"shiftTime", 0.1},
                          {"antiRollFront", 0},
                          {"antiRollRear", 0},
                          {"downforce", 0.1},
                          {"drag", 0.25}});
        drift = 0.6f;
    }
    v["preset"] = *preset;
    v["transmission"] = "auto";
    v["steering"] = "front";
    if (*handling == "arcade") {
        v["tractionControl"] = true;
        v["abs"] = true;
        v["driftAssist"] = drift;
        v["steerSpeed"] = 4.5;
        v["speedSensitiveSteering"] = 0.55;
        v["maxTilt"] = *preset == "kart" ? 40 : 55;
    } else {
        v["tractionControl"] = false;
        v["abs"] = true;
        v["driftAssist"] = 0;
        v["steerSpeed"] = 2.5;
        v["speedSensitiveSteering"] = 0.3;
        v["maxTilt"] = 180;
    }
    return v;
}

Json chaseCameraPreset(const std::string& preset) {
    if (preset == "kart") {
        return Json::object({{"distance", 3.6}, {"height", 1.3}, {"targetHeight", 0.4}, {"fovMin", 62}, {"fovMax", 80}, {"fovSpeed", 25}});
    }
    if (preset == "truck") {
        return Json::object({{"distance", 8.0}, {"height", 2.8}, {"targetHeight", 1.2}, {"fovMin", 58}, {"fovMax", 70}, {"fovSpeed", 40}});
    }
    if (preset == "hatchback") {
        return Json::object({{"distance", 5.6}, {"height", 1.9}, {"targetHeight", 0.8}, {"fovMin", 60}, {"fovMax", 74}, {"fovSpeed", 45}});
    }
    return Json::object({{"distance", 6.0}, {"height", 1.8}, {"targetHeight", 0.7}, {"fovMin", 60}, {"fovMax", 78}, {"fovSpeed", 55}});
}

}  // namespace sky::physics
