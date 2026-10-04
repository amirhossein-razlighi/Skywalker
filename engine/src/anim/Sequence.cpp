#include "skywalker/anim/Sequence.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"

namespace sky::anim {

const char* toString(TrackType t) {
    switch (t) {
        case TrackType::Property: return "property";
        case TrackType::Camera: return "camera";
        case TrackType::Shot: return "shot";
        case TrackType::Event: return "event";
        case TrackType::Animation: return "animation";
    }
    return "property";
}

const std::vector<std::string>& shotKinds() {
    static const std::vector<std::string> kinds{"orbit", "dolly", "crane", "track", "pan", "static", "path", "flyover"};
    return kinds;
}

namespace {

const std::vector<std::string>& easings() {
    static const std::vector<std::string> e{"linear", "step", "smooth", "ease_in", "ease_out", "auto", "bezier"};
    return e;
}

Error invalid(const std::string& where, const std::string& what, const std::string& hint = {}) {
    return Error::make("invalid_sequence", where + ": " + what, hint);
}

std::string didYouMean(std::string_view word, const std::vector<std::string>& options) {
    std::string g = str::closest(word, options, 3);
    if (!g.empty()) return "did you mean \"" + g + "\"?";
    std::string all;
    for (size_t i = 0; i < options.size(); ++i) all += (i ? ", " : "") + options[i];
    return "known: " + all;
}

// --- Values ---------------------------------------------------------------------------

enum class ValueKind { None, Number, Vector, Color, Other };

ValueKind toFloats(const Json& v, std::vector<float>& out) {
    out.clear();
    if (v.isNumber()) {
        out.push_back(v.asFloat());
        return ValueKind::Number;
    }
    if (v.isArray()) {
        for (const auto& e : v.elements()) {
            if (!e.isNumber()) return ValueKind::Other;
            out.push_back(e.asFloat());
        }
        return out.empty() ? ValueKind::Other : ValueKind::Vector;
    }
    if (v.isString() && !v.asString().empty() && v.asString()[0] == '#') {
        Vec4 c;
        if (reflect::parseHexColor(v.asString(), c)) {
            out = {c.x, c.y, c.z, c.w};
            return ValueKind::Color;
        }
    }
    return v.isNull() ? ValueKind::None : ValueKind::Other;
}

Json fromFloats(ValueKind kind, const std::vector<float>& v) {
    if (kind == ValueKind::Number) return v.empty() ? Json(0.0) : Json(v[0]);
    Json a = Json::array();
    for (float f : v) a.push(f);
    return a;
}

float cubicBezier(const float b[4], float s) {
    // CSS cubic-bezier(x1, y1, x2, y2): solve x(u) = s, return y(u).
    auto bez = [](float p1, float p2, float u) {
        float iu = 1.f - u;
        return 3.f * iu * iu * u * p1 + 3.f * iu * u * u * p2 + u * u * u;
    };
    auto dbez = [](float p1, float p2, float u) {
        float iu = 1.f - u;
        return 3.f * iu * iu * p1 + 6.f * iu * u * (p2 - p1) + 3.f * u * u * (1.f - p2);
    };
    float x1 = std::clamp(b[0], 0.f, 1.f), x2 = std::clamp(b[2], 0.f, 1.f);
    float u = s;
    for (int i = 0; i < 8; ++i) {  // Newton
        float x = bez(x1, x2, u) - s;
        float d = dbez(x1, x2, u);
        if (std::fabs(x) < 1e-6f) break;
        if (std::fabs(d) < 1e-6f) break;
        u = std::clamp(u - x / d, 0.f, 1.f);
    }
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 30 && std::fabs(bez(x1, x2, u) - s) > 1e-5f; ++i) {  // bisection fallback
        u = 0.5f * (lo + hi);
        if (bez(x1, x2, u) < s) lo = u;
        else hi = u;
    }
    return bez(b[1], b[3], u);
}

Result<SeqKey> parseKey(const Json& j, TrackType type, const std::string& where) {
    if (!j.isObject()) return invalid(where, "each key must be an object with \"t\"");
    SeqKey k;
    if (!j.get("t").isNumber()) return invalid(where, "key needs a time \"t\" in seconds");
    k.t = std::max(0.f, j.get("t").asFloat());
    const Json& e = j.get("ease");
    if (e.isArray()) {
        if (e.size() != 4) return invalid(where, "bezier ease needs [x1, y1, x2, y2]");
        k.ease = "bezier";
        for (size_t i = 0; i < 4; ++i) k.bezier[i] = e[i].asFloat();
    } else if (e.isString()) {
        k.ease = e.asString();
        if (std::find(easings().begin(), easings().end(), k.ease) == easings().end()) {
            return invalid(where, "unknown ease \"" + k.ease + "\"", didYouMean(k.ease, easings()));
        }
    } else if (type == TrackType::Shot) {
        k.ease = "smooth";
    }
    auto allow = [&](std::initializer_list<const char*> keys) -> Status {
        std::vector<std::string> ok(keys.begin(), keys.end());
        ok.push_back("t");
        ok.push_back("ease");
        for (const auto& [name, v] : j.members()) {
            if (std::find(ok.begin(), ok.end(), name) == ok.end()) {
                return invalid(where, "unknown key field \"" + name + "\"", didYouMean(name, ok));
            }
        }
        return {};
    };
    switch (type) {
        case TrackType::Property: {
            if (Status s = allow({"value", "v"}); !s) return s.error();
            k.value = j.contains("value") ? j.get("value") : j.get("v");
            if (k.value.isNull()) return invalid(where, "property key needs a \"value\"");
            break;
        }
        case TrackType::Camera:
            if (Status s = allow({"camera"}); !s) return s.error();
            if (!j.get("camera").isString()) return invalid(where, "camera key needs \"camera\": entity name");
            break;
        case TrackType::Event:
            if (Status s = allow({"event", "target"}); !s) return s.error();
            if (j.get("event").asString().empty()) return invalid(where, "event key needs \"event\": name");
            break;
        case TrackType::Animation:
            if (Status s = allow({"play", "fade", "loop", "layer", "speed", "params"}); !s) return s.error();
            if (j.get("play").asString().empty() && !j.contains("params")) {
                return invalid(where, "animation key needs \"play\": state or clip (or \"params\")");
            }
            break;
        case TrackType::Shot: {
            if (Status s = allow({"shot", "duration", "target", "offset", "radius", "height", "distance", "angle", "from", "to",
                                  "position", "points", "pitch", "fov", "roll"});
                !s) {
                return s.error();
            }
            std::string kind = j.get("shot").asString();
            if (std::find(shotKinds().begin(), shotKinds().end(), kind) == shotKinds().end()) {
                return invalid(where, "unknown shot \"" + kind + "\"", didYouMean(kind, shotKinds()));
            }
            if (j.get("duration").asFloat() <= 0.f) return invalid(where, "shot needs \"duration\" > 0 seconds");
            break;
        }
    }
    if (type != TrackType::Property) {
        k.data = j;
        k.data.erase("t");
        k.data.erase("ease");
    }
    return k;
}

Json keyToJson(const SeqKey& k, TrackType type) {
    Json j = Json::object({{"t", k.t}});
    if (type == TrackType::Property) j["value"] = k.value;
    else for (const auto& [name, v] : k.data.members()) j[name] = v;
    bool defaultEase = type == TrackType::Shot ? k.ease == "smooth" : k.ease == "linear";
    if (k.ease == "bezier") {
        j["ease"] = Json::array({k.bezier[0], k.bezier[1], k.bezier[2], k.bezier[3]});
    } else if (!defaultEase) {
        j["ease"] = k.ease;
    }
    return j;
}

Vec3 jvec(const Json& j, Vec3 fallback) {
    Vec3 v;
    return reflect::jsonToVec3(j, v) ? v : fallback;
}

/// [a, b] or a single number for both ends.
std::pair<float, float> range(const Json& j, float a, float b) {
    if (j.isNumber()) return {j.asFloat(), j.asFloat()};
    if (j.isArray() && j.size() == 2) return {j[0].asFloat(a), j[1].asFloat(b)};
    return {a, b};
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Vec3 catmull(const std::vector<Vec3>& pts, float s) {
    if (pts.empty()) return {};
    if (pts.size() == 1) return pts[0];
    float x = std::clamp(s, 0.f, 1.f) * static_cast<float>(pts.size() - 1);
    size_t i = std::min(static_cast<size_t>(x), pts.size() - 2);
    float u = x - static_cast<float>(i);
    Vec3 p0 = pts[i > 0 ? i - 1 : 0], p1 = pts[i], p2 = pts[i + 1], p3 = pts[std::min(i + 2, pts.size() - 1)];
    float u2 = u * u, u3 = u2 * u;
    return (p1 * 2.f + (p2 - p0) * u + (p0 * 2.f - p1 * 5.f + p2 * 4.f - p3) * u2 + (p1 * 3.f - p0 - p2 * 3.f + p3) * u3) * 0.5f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parse / serialize
// ---------------------------------------------------------------------------

Result<SequenceDef> SequenceDef::fromJson(const Json& doc) {
    if (!doc.isObject()) return invalid("sequence", "must be a JSON object");
    static const std::vector<std::string> top{"format", "version", "name", "duration", "tracks"};
    for (const auto& [k, v] : doc.members()) {
        if (std::find(top.begin(), top.end(), k) == top.end()) return invalid("sequence", "unknown field \"" + k + "\"", didYouMean(k, top));
    }
    if (doc.contains("format") && doc.get("format").asString() != "skywalker.sequence") {
        return invalid("sequence", "format must be \"skywalker.sequence\"");
    }
    SequenceDef s;
    s.name = doc.get("name").asString("Sequence");
    s.duration = std::max(0.f, doc.get("duration").asFloat(0.f));
    static const std::vector<std::string> types{"property", "camera", "shot", "event", "animation"};
    size_t ti = 0;
    for (const auto& tj : doc.get("tracks").elements()) {
        std::string where = "track " + std::to_string(ti++);
        if (!tj.isObject()) return invalid(where, "must be an object");
        static const std::vector<std::string> trackKeys{"type", "entity", "property", "camera", "keys", "muted", "name"};
        for (const auto& [k, v] : tj.members()) {
            if (std::find(trackKeys.begin(), trackKeys.end(), k) == trackKeys.end()) {
                return invalid(where, "unknown field \"" + k + "\"", didYouMean(k, trackKeys));
            }
        }
        Track t;
        std::string type = tj.get("type").asString("property");
        if (type == "property") t.type = TrackType::Property;
        else if (type == "camera") t.type = TrackType::Camera;
        else if (type == "shot") t.type = TrackType::Shot;
        else if (type == "event") t.type = TrackType::Event;
        else if (type == "animation") t.type = TrackType::Animation;
        else return invalid(where, "unknown track type \"" + type + "\"", didYouMean(type, types));
        t.entity = tj.get("entity").asString();
        t.property = tj.get("property").asString();
        t.camera = tj.get("camera").asString();
        t.muted = tj.get("muted").asBool(false);
        if (t.type == TrackType::Property) {
            if (t.property.find('.') == std::string::npos) {
                return invalid(where, "property must be \"component.field\" (e.g. \"transform.position\", \"light.intensity\")");
            }
            if (t.entity.empty() && !str::startsWith(t.property, "environment.")) {
                return invalid(where, "property track needs \"entity\" (only environment.* properties have none)");
            }
        }
        if (t.type == TrackType::Animation && t.entity.empty()) return invalid(where, "animation track needs \"entity\"");
        if (t.type == TrackType::Shot && t.camera.empty()) return invalid(where, "shot track needs \"camera\": the camera entity it moves");
        for (const auto& kj : tj.get("keys").elements()) {
            auto k = parseKey(kj, t.type, where + " key");
            if (!k) return k.error();
            t.keys.push_back(std::move(*k));
        }
        std::stable_sort(t.keys.begin(), t.keys.end(), [](const SeqKey& a, const SeqKey& b) { return a.t < b.t; });
        s.tracks.push_back(std::move(t));
    }
    return s;
}

Json SequenceDef::toJson() const {
    Json tracksJ = Json::array();
    for (const auto& t : tracks) {
        Json tj = Json::object({{"type", toString(t.type)}});
        if (!t.entity.empty()) tj["entity"] = t.entity;
        if (!t.property.empty()) tj["property"] = t.property;
        if (!t.camera.empty()) tj["camera"] = t.camera;
        if (t.muted) tj["muted"] = true;
        Json keys = Json::array();
        for (const auto& k : t.keys) keys.push(keyToJson(k, t.type));
        tj["keys"] = keys;
        tracksJ.push(std::move(tj));
    }
    return Json::object({{"format", "skywalker.sequence"}, {"version", 1}, {"name", name}, {"duration", duration}, {"tracks", tracksJ}});
}

float SequenceDef::length() const {
    float end = duration;
    if (end > 0.f) return end;
    for (const auto& t : tracks) {
        for (const auto& k : t.keys) {
            end = std::max(end, k.t + (t.type == TrackType::Shot ? k.data.get("duration").asFloat() : 0.f));
        }
    }
    return end;
}

Result<SequenceDef> loadSequence(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Error::make("not_found", "cannot read sequence " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    if (!doc) return doc.error();
    return SequenceDef::fromJson(*doc);
}

Status saveSequence(const std::string& path, const SequenceDef& def) {
    std::ofstream f(path);
    if (!f) return Error::make("io_error", "cannot write " + path);
    f << def.toJson().dump(2) << "\n";
    return {};
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

float ease(const SeqKey& key, float s) {
    s = std::clamp(s, 0.f, 1.f);
    const std::string& e = key.ease;
    if (e == "step") return 0.f;
    if (e == "smooth") return s * s * (3.f - 2.f * s);
    if (e == "ease_in") return s * s;
    if (e == "ease_out") return 1.f - (1.f - s) * (1.f - s);
    if (e == "bezier") return cubicBezier(key.bezier, s);
    return s;  // linear, auto (auto is a spline in value space, see evaluateProperty)
}

Json evaluateProperty(const Track& track, float t) {
    const auto& keys = track.keys;
    if (keys.empty()) return {};
    if (t <= keys.front().t || keys.size() == 1) return keys.front().value;
    if (t >= keys.back().t) return keys.back().value;
    size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].t <= t) ++i;
    const SeqKey& a = keys[i];
    const SeqKey& b = keys[i + 1];
    float dt = b.t - a.t;
    float s = dt > 0.f ? (t - a.t) / dt : 1.f;
    std::vector<float> va, vb;
    ValueKind ka = toFloats(a.value, va), kb = toFloats(b.value, vb);
    if (ka == ValueKind::Other || ka == ValueKind::None || ka != kb || va.size() != vb.size() || a.ease == "step") {
        return s >= 1.f ? b.value : a.value;  // non-numeric values step
    }
    std::vector<float> out(va.size());
    if (a.ease == "auto") {
        // Catmull-Rom tangents from the neighbouring keys (non-uniform times).
        auto tangent = [&](size_t k, std::vector<float>& m) {
            m.assign(va.size(), 0.f);
            size_t lo = k > 0 ? k - 1 : k, hi = k + 1 < keys.size() ? k + 1 : k;
            std::vector<float> vl, vh;
            if (toFloats(keys[lo].value, vl) != ka || toFloats(keys[hi].value, vh) != ka || vl.size() != va.size() ||
                vh.size() != va.size() || keys[hi].t <= keys[lo].t) {
                return;
            }
            for (size_t c = 0; c < m.size(); ++c) m[c] = (vh[c] - vl[c]) / (keys[hi].t - keys[lo].t);
        };
        std::vector<float> ma, mb;
        tangent(i, ma);
        tangent(i + 1, mb);
        float s2 = s * s, s3 = s2 * s;
        float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        for (size_t c = 0; c < out.size(); ++c) out[c] = h00 * va[c] + h10 * dt * ma[c] + h01 * vb[c] + h11 * dt * mb[c];
    } else {
        float e = ease(a, s);
        for (size_t c = 0; c < out.size(); ++c) out[c] = lerpf(va[c], vb[c], e);
    }
    return fromFloats(ka, out);
}

Vec3 lookRotation(Vec3 eye, Vec3 target, float roll) {
    Vec3 d = normalize(target - eye);
    if (length(d) < 1e-6f) return {0, 0, roll};
    float pitch = degrees(std::asin(std::clamp(d.y, -1.f, 1.f)));
    float yaw = degrees(std::atan2(-d.x, -d.z));
    return {pitch, yaw, roll};
}

std::optional<ShotPose> evaluateShot(const Track& track, float t, const PointLookup& lookup) {
    if (track.keys.empty()) return std::nullopt;
    size_t i = 0;
    while (i + 1 < track.keys.size() && track.keys[i + 1].t <= t) ++i;
    const SeqKey& k = track.keys[i];
    const Json& d = k.data;
    float duration = std::max(d.get("duration").asFloat(1.f), 1e-3f);
    float s = ease(k, std::clamp((t - k.t) / duration, 0.f, 1.f));
    const std::string kind = d.get("shot").asString();

    auto point = [&](const Json& j) -> std::optional<Vec3> {
        Vec3 v;
        if (reflect::jsonToVec3(j, v)) return v;
        if (j.isString() && lookup) return lookup(j.asString());
        return std::nullopt;
    };
    std::optional<Vec3> target = point(d.get("target"));
    Vec3 aim = target.value_or(Vec3{0, 0, 0}) + jvec(d.get("offset"), {0, 0, 0});
    const bool hasTarget = target.has_value();
    auto dirOf = [](float yawDeg) { return Vec3{std::sin(radians(yawDeg)), 0.f, std::cos(radians(yawDeg))}; };

    ShotPose out;
    std::optional<Vec3> lookAt = hasTarget ? std::optional<Vec3>(aim) : std::nullopt;
    if (kind == "orbit") {
        float a0 = d.get("from").asFloat(0.f), a1 = d.get("to").asFloat(90.f);
        float r = d.get("radius").asFloat(6.f);
        auto [h0, h1] = range(d.get("height"), 2.f, 2.f);
        float a = lerpf(a0, a1, s);
        out.position = aim + dirOf(a) * r + Vec3{0, lerpf(h0, h1, s), 0};
        lookAt = aim;
    } else if (kind == "dolly") {
        Vec3 p0, p1;
        if (reflect::jsonToVec3(d.get("from"), p0) && reflect::jsonToVec3(d.get("to"), p1)) {
            out.position = lerp(p0, p1, s);
            if (!lookAt) lookAt = out.position + normalize(p1 - p0);
        } else {
            auto [d0, d1] = range(d.get("distance"), 8.f, 4.f);
            float angle = d.get("angle").asFloat(0.f);
            auto [h0, h1] = range(d.get("height"), 1.5f, 1.5f);
            out.position = aim + dirOf(angle) * lerpf(d0, d1, s) + Vec3{0, lerpf(h0, h1, s), 0};
            lookAt = aim;
        }
    } else if (kind == "crane") {
        auto [h0, h1] = range(d.get("height"), 0.5f, 6.f);
        auto [d0, d1] = range(d.get("distance"), 6.f, 6.f);
        float angle = d.get("angle").asFloat(0.f);
        out.position = aim + dirOf(angle) * lerpf(d0, d1, s) + Vec3{0, lerpf(h0, h1, s), 0};
        lookAt = aim;
    } else if (kind == "track") {
        float angle = d.get("angle").asFloat(90.f);
        auto [d0, d1] = range(d.get("distance"), 6.f, 6.f);
        auto [h0, h1] = range(d.get("height"), 1.5f, 1.5f);
        float l0 = d.get("from").asFloat(-2.f), l1 = d.get("to").asFloat(2.f);
        Vec3 fwd = dirOf(angle);
        Vec3 right = normalize(cross({0, 1, 0}, fwd));
        float lateral = lerpf(l0, l1, s);
        out.position = aim + fwd * lerpf(d0, d1, s) + right * lateral + Vec3{0, lerpf(h0, h1, s), 0};
        lookAt = aim + right * lateral;
    } else if (kind == "pan") {
        out.position = jvec(d.get("position"), aim + Vec3{0, 1.6f, 6.f});
        std::optional<Vec3> a = point(d.get("from")), b = point(d.get("to"));
        if (a && b && !d.get("from").isNumber()) {
            lookAt = lerp(*a, *b, s);
        } else {
            float y0 = d.get("from").asFloat(-30.f), y1 = d.get("to").asFloat(30.f);
            float pitch = d.get("pitch").asFloat(0.f);
            out.rotation = {pitch, lerpf(y0, y1, s), d.get("roll").asFloat(0.f)};
            lookAt.reset();
        }
    } else if (kind == "flyover") {
        // A straight aerial pass over the target: from `distance` m on the `angle` side to as far past it,
        // keeping it framed (the classic establishing flyover).
        float dist = d.get("distance").isNumber() ? d.get("distance").asFloat() : 30.f;
        auto [h0, h1] = range(d.get("height"), 12.f, 12.f);
        Vec3 dir = dirOf(d.get("angle").asFloat(0.f));
        out.position = aim + dir * lerpf(dist, -dist, s) + Vec3{0, lerpf(h0, h1, s), 0};
        lookAt = aim;
    } else if (kind == "static") {
        out.position = jvec(d.get("position"), aim + Vec3{0, 1.6f, 6.f});
    } else if (kind == "path") {
        std::vector<Vec3> pts;
        for (const auto& p : d.get("points").elements()) {
            Vec3 v;
            if (reflect::jsonToVec3(p, v)) pts.push_back(v);
        }
        if (pts.empty()) return std::nullopt;
        out.position = catmull(pts, s);
        if (!lookAt) lookAt = out.position + normalize(catmull(pts, std::min(s + 0.01f, 1.f)) - catmull(pts, std::max(s - 0.01f, 0.f)));
    } else {
        return std::nullopt;
    }
    if (lookAt) out.rotation = lookRotation(out.position, *lookAt, d.get("roll").asFloat(0.f));
    const Json& fov = d.get("fov");
    if (fov.isNumber() || fov.isArray()) {
        auto [f0, f1] = range(fov, 50.f, 50.f);
        out.fov = std::clamp(lerpf(f0, f1, s), 1.f, 170.f);
    }
    return out;
}

}  // namespace sky::anim
