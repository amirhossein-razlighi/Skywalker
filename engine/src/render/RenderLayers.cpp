#include "skywalker/render/RenderLayers.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "skywalker/core/Strings.h"

namespace sky::render {

namespace {

/// "3", "layer3", "layer_3", "Layer 3" -> 3 (1..20), else 0.
int layerNumber(std::string_view text) {
    std::string t = str::lower(text);
    for (const char* prefix : {"layer_", "layer ", "layer"}) {
        if (str::startsWith(t, prefix)) {
            t = t.substr(std::string_view(prefix).size());
            break;
        }
    }
    if (t.empty() || t.size() > 2 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    int n = std::atoi(t.c_str());
    return n >= 1 && n <= kLayerCount ? n : 0;
}

std::string layerList(const LayerNames& names) {
    std::string out;
    for (int i = 0; i < kLayerCount; ++i) {
        if (names.names[static_cast<size_t>(i)].empty()) continue;
        out += (out.empty() ? "" : ", ") + std::to_string(i + 1) + " = " + names.names[static_cast<size_t>(i)];
    }
    return out.empty() ? "no layers are named yet (game.json render.layers, or render_layers {action: \"name\"})" : "named layers: " + out;
}

Result<uint32_t> parseOne(const Json& v, const LayerNames& names, bool inList) {
    if (v.isNumber()) {
        const double d = v.asNumber();
        if (d < 0 || d != std::floor(d)) return Error::make("invalid_layer", "layer masks are whole numbers >= 0", "e.g. 5 = layers 1 and 3");
        if (inList) {  // inside a list a number is a layer number
            const int n = static_cast<int>(d);
            if (n < 1 || n > kLayerCount) {
                return Error::make("invalid_layer", "layer " + std::to_string(n) + " is out of range", "layers are numbered 1..20");
            }
            return 1u << (n - 1);
        }
        if (d > static_cast<double>(kAllLayers)) {
            return Error::make("invalid_layer", "mask " + v.dump() + " uses bits beyond the 20 layers", "the largest mask is 1048575 (all)");
        }
        return static_cast<uint32_t>(d);
    }
    if (!v.isString()) return Error::make("invalid_layer", "a layer mask is a number, a layer name, or a list of names / layer numbers");
    const std::string s = v.asString();
    const std::string low = str::lower(s);
    if (low == "all" || low == "everything") return kAllLayers;
    if (low == "none" || low == "nothing") return 0u;
    if (int n = names.find(s)) return 1u << (n - 1);
    if (int n = layerNumber(s)) return 1u << (n - 1);
    std::string guess = str::closest(s, names.known(), 3);
    return Error::make("unknown_layer", "no render layer named '" + s + "'",
                       (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") + layerList(names) +
                           "; layer numbers (\"3\") and \"all\" / \"none\" also work");
}

}  // namespace

int LayerNames::find(std::string_view name) const {
    const std::string want = str::lower(name);
    if (want.empty()) return 0;
    for (int i = 0; i < kLayerCount; ++i) {
        if (!names[static_cast<size_t>(i)].empty() && str::lower(names[static_cast<size_t>(i)]) == want) return i + 1;
    }
    return 0;
}

std::vector<std::string> LayerNames::known() const {
    std::vector<std::string> out{"all", "none"};
    for (const auto& n : names) {
        if (!n.empty()) out.push_back(n);
    }
    return out;
}

Result<LayerNames> LayerNames::fromJson(const Json& layers) {
    LayerNames out;
    if (layers.isNull()) return out;
    if (!layers.isObject()) {
        return Error::make("invalid_game_json", "render.layers must be an object like {\"1\": \"world\", \"2\": \"player\"}");
    }
    for (const auto& [key, value] : layers.members()) {
        const int n = layerNumber(key);
        if (n == 0) {
            return Error::make("invalid_game_json", "render.layers: '" + key + "' is not a layer number",
                               "keys are layer numbers \"1\" .. \"20\", values their names");
        }
        if (!value.isString()) return Error::make("invalid_game_json", "render.layers." + key + " must be a name (string)");
        const std::string name = value.asString();
        if (name.empty() || name.size() > 40 || layerNumber(name) != 0 || str::lower(name) == "all" || str::lower(name) == "none") {
            return Error::make("invalid_game_json", "render.layers." + key + ": '" + name + "' is not a usable layer name",
                               "use a short word like \"player\" (not empty, not a number, not all/none)");
        }
        if (int other = out.find(name); other != 0 && other != n) {
            return Error::make("invalid_game_json", "render.layers: '" + name + "' names both layer " + std::to_string(other) +
                                                        " and layer " + std::to_string(n));
        }
        out.names[static_cast<size_t>(n - 1)] = name;
    }
    return out;
}

Json LayerNames::toJson() const {
    Json j = Json::object();
    for (int i = 0; i < kLayerCount; ++i) {
        if (!names[static_cast<size_t>(i)].empty()) j[std::to_string(i + 1)] = names[static_cast<size_t>(i)];
    }
    return j;
}

Result<uint32_t> parseLayerMask(const Json& value, const LayerNames& names) {
    if (value.isArray()) {
        uint32_t mask = 0;
        for (const Json& e : value.elements()) {
            auto m = parseOne(e, names, true);
            if (!m) return m.error();
            mask |= *m;
        }
        return mask;
    }
    return parseOne(value, names, false);
}

Json describeLayerMask(uint32_t mask, const LayerNames& names) {
    Json out = Json::array();
    for (int i = 0; i < kLayerCount; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::string& n = names.names[static_cast<size_t>(i)];
        if (n.empty()) {
            out.push(i + 1);
        } else {
            out.push(n);
        }
    }
    return out;
}

Vec3 kelvinToRgb(float kelvin) {
    // Fit of the Planckian locus in sRGB (Tanner Helland), normalized to a max channel of 1.
    const double t = std::clamp(static_cast<double>(kelvin), 1000.0, 40000.0) / 100.0;
    double r, g, b;
    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
    } else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    }
    if (t >= 66.0) {
        b = 255.0;
    } else if (t <= 19.0) {
        b = 0.0;
    } else {
        b = 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    }
    Vec3 c{static_cast<float>(std::clamp(r, 0.0, 255.0) / 255.0), static_cast<float>(std::clamp(g, 0.0, 255.0) / 255.0),
           static_cast<float>(std::clamp(b, 0.0, 255.0) / 255.0)};
    const float m = std::max({c.x, c.y, c.z, 1e-6f});
    return c * (1.f / m);
}

float lightDistanceFade(float distance, float begin, float length) {
    if (distance <= begin) return 1.f;
    return std::clamp(1.f - (distance - begin) / std::max(length, 1e-3f), 0.f, 1.f);
}

}  // namespace sky::render
