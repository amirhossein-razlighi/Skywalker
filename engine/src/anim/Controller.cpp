#include "skywalker/anim/Controller.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"

namespace sky::anim {

const char* toString(ParamType t) {
    switch (t) {
        case ParamType::Float: return "float";
        case ParamType::Int: return "int";
        case ParamType::Bool: return "bool";
        case ParamType::Trigger: return "trigger";
    }
    return "float";
}

namespace {

constexpr float kTriggerLife = 0.25f;  // an unconsumed trigger disarms after this long (seconds)

MotionDef clipMotion(std::string clip) {
    MotionDef m;
    m.clip = std::move(clip);
    return m;
}

Error invalid(const std::string& where, const std::string& what, const std::string& hint = {}) {
    return Error::make("invalid_controller", where + ": " + what, hint);
}

std::string didYouMean(std::string_view word, const std::vector<std::string>& options) {
    std::string g = str::closest(word, options, 3);
    if (!g.empty()) return "did you mean \"" + g + "\"?";
    std::string all;
    for (size_t i = 0; i < options.size() && i < 24; ++i) all += (i ? ", " : "") + options[i];
    return options.empty() ? "" : "known: " + all;
}

Status checkKeys(const Json& obj, std::initializer_list<const char*> allowed, const std::string& where) {
    std::vector<std::string> keys(allowed.begin(), allowed.end());
    for (const auto& [k, v] : obj.members()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            return invalid(where, "unknown field \"" + k + "\"", didYouMean(k, keys));
        }
    }
    return {};
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\n"), b = s.find_last_not_of(" \t\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

Result<ConditionDef> parseConditionText(const std::string& text, const std::string& where) {
    std::string s = trim(text);
    ConditionDef c;
    if (s.empty()) return invalid(where, "empty condition");
    if (s[0] == '!' || str::startsWith(s, "not ")) {
        c.param = trim(s.substr(s[0] == '!' ? 1 : 4));
        c.op = CondOp::IsFalse;
        return c;
    }
    static const std::pair<const char*, CondOp> ops[] = {{">=", CondOp::GreaterEq}, {"<=", CondOp::LessEq}, {"==", CondOp::Equal},
                                                         {"!=", CondOp::NotEqual},  {">", CondOp::Greater},  {"<", CondOp::Less},
                                                         {"=", CondOp::Equal}};
    for (const auto& [tok, op] : ops) {
        size_t at = s.find(tok);
        if (at == std::string::npos) continue;
        c.param = trim(s.substr(0, at));
        std::string rhs = trim(s.substr(at + std::strlen(tok)));
        c.op = op;
        if (rhs == "true" || rhs == "false") {
            bool b = rhs == "true";
            if (op != CondOp::Equal && op != CondOp::NotEqual) return invalid(where, "booleans only compare with == or !=");
            c.op = (op == CondOp::Equal) == b ? CondOp::IsTrue : CondOp::IsFalse;
            return c;
        }
        char* end = nullptr;
        c.value = std::strtof(rhs.c_str(), &end);
        if (rhs.empty() || end == rhs.c_str() || *end != '\0') {
            return invalid(where, "\"" + rhs + "\" is not a number", "write conditions like \"speed > 0.1\", \"grounded\", \"!crouch\"");
        }
        if (c.param.empty()) return invalid(where, "condition without a parameter");
        return c;
    }
    c.param = s;
    c.op = CondOp::IsTrue;  // bool true, or (resolved later) a trigger
    return c;
}

Result<std::vector<ConditionDef>> parseConditions(const Json& when, const std::string& where) {
    std::vector<ConditionDef> out;
    auto addText = [&](const std::string& text) -> Status {
        std::string rest = text;
        // Split on "and" / "&&" / ","
        for (const char* sep : {"&&", ","}) {
            for (size_t at; (at = rest.find(sep)) != std::string::npos;) rest.replace(at, std::strlen(sep), " and ");
        }
        size_t start = 0;
        while (true) {
            size_t at = rest.find(" and ", start);
            std::string part = rest.substr(start, at == std::string::npos ? std::string::npos : at - start);
            if (!trim(part).empty()) {
                auto c = parseConditionText(part, where);
                if (!c) return c.error();
                out.push_back(*c);
            }
            if (at == std::string::npos) break;
            start = at + 5;
        }
        return {};
    };
    if (when.isNull()) return out;
    if (when.isString()) {
        if (Status s = addText(when.asString()); !s) return s.error();
        return out;
    }
    if (!when.isArray()) return invalid(where, "\"when\" must be a string or a list");
    for (const auto& c : when.elements()) {
        if (c.isString()) {
            if (Status s = addText(c.asString()); !s) return s.error();
        } else if (c.isObject()) {
            if (Status s = checkKeys(c, {"param", "op", "value"}, where); !s) return s.error();
            std::string text = c.get("param").asString();
            if (c.contains("op")) {
                text += " " + c.get("op").asString() + " ";
                text += c.get("value").isBool() ? (c.get("value").asBool() ? "true" : "false") : c.get("value").dump();
            }
            if (Status s = addText(text); !s) return s.error();
        } else {
            return invalid(where, "each condition must be a string or {param, op, value}");
        }
    }
    return out;
}

Result<MotionDef> parseMotion(const Json& j, const std::string& where, bool twoD) {
    MotionDef m;
    if (j.isString()) {
        m.clip = j.asString();
        return m;
    }
    if (Status s = checkKeys(j, {"clip", "at", "pos", "speed"}, where); !s) return s.error();
    m.clip = j.get("clip").asString();
    if (m.clip.empty()) return invalid(where, "motion needs a \"clip\"");
    m.threshold = j.get("at").asFloat();
    m.speed = j.get("speed").asFloat(1.f);
    if (twoD) {
        const Json& p = j.get("pos");
        if (p.size() != 2) return invalid(where, "2D blend motions need \"pos\": [x, y]");
        m.position = {p[0].asFloat(), p[1].asFloat()};
    }
    return m;
}

Result<StateDef> parseState(const std::string& name, const Json& j, const std::string& where) {
    StateDef s;
    s.name = name;
    if (j.isString()) {  // "Idle": "IdleClip"
        s.motions.push_back(clipMotion(j.asString()));
        return s;
    }
    if (!j.isObject()) return invalid(where, "a state must be an object");
    if (Status st = checkKeys(j, {"name", "clip", "blend", "blend2d", "speed", "speedParameter", "loop", "events"}, where); !st) {
        return st.error();
    }
    s.speed = j.get("speed").asFloat(1.f);
    s.speedParam = j.get("speedParameter").asString();
    s.loop = j.get("loop").asBool(true);
    int kinds = j.contains("clip") + j.contains("blend") + j.contains("blend2d");
    if (kinds != 1) return invalid(where, "a state needs exactly one of \"clip\", \"blend\" (1D) or \"blend2d\"");
    if (j.contains("clip")) {
        s.motions.push_back(clipMotion(j.get("clip").asString()));
    } else {
        bool twoD = j.contains("blend2d");
        const Json& b = j.get(twoD ? "blend2d" : "blend");
        if (Status st = checkKeys(b, {"parameter", "x", "y", "motions", "mode"}, where + " blend"); !st) return st.error();
        s.kind = twoD ? StateKind::Blend2D : StateKind::Blend1D;
        if (b.contains("mode")) {
            std::string mode = b.get("mode").asString();
            if (!twoD || (mode != "cartesian" && mode != "directional")) {
                return invalid(where, "blend2d \"mode\" is \"cartesian\" (default) or \"directional\" (2D blends only)",
                               "directional: motions are velocities ([x, y] = strafe, forward in m/s); direction and speed blend separately");
            }
            s.mode2d = mode == "directional" ? Blend2DMode::Directional : Blend2DMode::Cartesian;
        }
        s.paramX = twoD ? b.get("x").asString() : b.get("parameter").asString();
        s.paramY = b.get("y").asString();
        if (s.paramX.empty() || (twoD && s.paramY.empty())) {
            return invalid(where, twoD ? "blend2d needs \"x\" and \"y\" parameters" : "blend needs a \"parameter\"");
        }
        for (const auto& mj : b.get("motions").elements()) {
            auto m = parseMotion(mj, where + " motion", twoD);
            if (!m) return m.error();
            s.motions.push_back(*m);
        }
        if (s.motions.empty()) return invalid(where, "blend has no motions");
    }
    for (const auto& ej : j.get("events").elements()) {
        if (Status st = checkKeys(ej, {"time", "name"}, where + " event"); !st) return st.error();
        EventDef e{std::clamp(ej.get("time").asFloat(), 0.f, 1.f), ej.get("name").asString()};
        if (e.name.empty()) return invalid(where, "event needs a \"name\"");
        s.events.push_back(e);
    }
    return s;
}

Result<LayerDef> parseLayer(const Json& j, size_t index) {
    LayerDef L;
    std::string where = "layer " + std::to_string(index);
    if (Status st = checkKeys(j, {"name", "weight", "weightParameter", "blending", "mask", "default", "states", "transitions"}, where); !st) {
        return st.error();
    }
    L.name = j.get("name").asString(index == 0 ? "Base" : "Layer" + std::to_string(index));
    where = "layer \"" + L.name + "\"";
    L.weight = std::clamp(j.get("weight").asFloat(1.f), 0.f, 1.f);
    L.weightParam = j.get("weightParameter").asString();
    std::string blending = j.get("blending").asString("override");
    if (blending != "override" && blending != "additive") return invalid(where, "blending must be \"override\" or \"additive\"");
    L.additive = blending == "additive";
    for (const auto& m : j.get("mask").elements()) L.mask.push_back(m.asString());
    const Json& states = j.get("states");
    if (states.isObject()) {
        for (const auto& [name, sj] : states.members()) {
            auto s = parseState(name, sj, where + " state \"" + name + "\"");
            if (!s) return s.error();
            L.states.push_back(std::move(*s));
        }
    } else if (states.isArray()) {
        for (const auto& sj : states.elements()) {
            std::string name = sj.get("name").asString();
            if (name.empty()) return invalid(where, "states in a list need a \"name\"");
            auto s = parseState(name, sj, where + " state \"" + name + "\"");
            if (!s) return s.error();
            L.states.push_back(std::move(*s));
        }
    } else {
        return invalid(where, "\"states\" must be an object {name: state} or a list");
    }
    if (L.states.empty()) return invalid(where, "a layer needs at least one state");
    for (size_t a = 0; a < L.states.size(); ++a) {
        for (size_t b = a + 1; b < L.states.size(); ++b) {
            if (L.states[a].name == L.states[b].name) return invalid(where, "duplicate state \"" + L.states[a].name + "\"");
        }
    }
    L.defaultState = j.get("default").asString(L.states.front().name);
    for (const auto& tj : j.get("transitions").elements()) {
        std::string tw = where + " transition";
        if (Status st = checkKeys(tj, {"from", "to", "when", "duration", "exit", "exitTime", "offset", "interruptible"}, tw); !st) {
            return st.error();
        }
        TransitionDef t;
        t.from = tj.get("from").asString("any");
        if (t.from == "*") t.from = "any";
        t.to = tj.get("to").asString();
        tw = where + " transition " + t.from + " -> " + t.to;
        if (t.to.empty()) return invalid(tw, "a transition needs \"to\"");
        auto conds = parseConditions(tj.get("when"), tw);
        if (!conds) return conds.error();
        t.conditions = std::move(*conds);
        t.duration = std::max(0.f, tj.get("duration").asFloat(0.2f));
        t.exitTime = tj.contains("exit") ? tj.get("exit").asFloat(-1.f) : tj.get("exitTime").asFloat(-1.f);
        if (t.conditions.empty() && t.exitTime < 0.f) t.exitTime = 1.f;  // unconditional: leave at the end
        t.offset = std::clamp(tj.get("offset").asFloat(0.f), 0.f, 1.f);
        t.interruptible = tj.get("interruptible").asBool(false);
        L.transitions.push_back(std::move(t));
    }
    return L;
}

Json motionJson(const MotionDef& m, StateKind kind) {
    if (kind == StateKind::Clip) return m.clip;
    Json j = Json::object({{"clip", m.clip}});
    if (kind == StateKind::Blend1D) j["at"] = m.threshold;
    if (kind == StateKind::Blend2D) j["pos"] = Json::array({m.position.x, m.position.y});
    if (m.speed != 1.f) j["speed"] = m.speed;
    return j;
}

const char* opText(CondOp op) {
    switch (op) {
        case CondOp::Greater: return ">";
        case CondOp::Less: return "<";
        case CondOp::GreaterEq: return ">=";
        case CondOp::LessEq: return "<=";
        case CondOp::Equal: return "==";
        case CondOp::NotEqual: return "!=";
        default: return "";
    }
}

std::string conditionText(const ConditionDef& c) {
    if (c.op == CondOp::IsTrue || c.op == CondOp::Trigger) return c.param;
    if (c.op == CondOp::IsFalse) return "!" + c.param;
    std::ostringstream os;
    os << c.param << " " << opText(c.op) << " " << c.value;
    return os.str();
}

float smooth01(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

float fract(float x) { return x - std::floor(x); }

}  // namespace

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

int LayerDef::stateIndex(std::string_view n) const {
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i].name == n) return static_cast<int>(i);
    }
    for (size_t i = 0; i < states.size(); ++i) {
        if (str::lower(states[i].name) == str::lower(n)) return static_cast<int>(i);
    }
    return -1;
}

int ControllerDef::paramIndex(std::string_view n) const {
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].name == n) return static_cast<int>(i);
    }
    return -1;
}

Result<ControllerDef> ControllerDef::fromJson(const Json& doc) {
    if (!doc.isObject()) return invalid("controller", "must be a JSON object");
    if (Status st = checkKeys(doc, {"format", "version", "library", "parameters", "layers", "states", "transitions", "default"}, "controller");
        !st) {
        return st.error();
    }
    if (doc.contains("format") && doc.get("format").asString() != "skywalker.animctl") {
        return invalid("controller", "format must be \"skywalker.animctl\"");
    }
    ControllerDef c;
    c.library = doc.get("library").asString();
    const Json& paramsJ = doc.get("parameters");
    auto addParam = [&](const std::string& name, const Json& spec) -> Status {
        ParamDef p;
        p.name = name;
        std::string type = spec.isString() ? spec.asString() : spec.get("type").asString("float");
        if (spec.isObject()) {
            if (Status st = checkKeys(spec, {"name", "type", "default"}, "parameter \"" + name + "\""); !st) return st;
        }
        static const std::vector<std::string> types{"float", "int", "bool", "trigger"};
        if (type == "float") p.type = ParamType::Float;
        else if (type == "int") p.type = ParamType::Int;
        else if (type == "bool") p.type = ParamType::Bool;
        else if (type == "trigger") p.type = ParamType::Trigger;
        else return invalid("parameter \"" + name + "\"", "unknown type \"" + type + "\"", didYouMean(type, types));
        const Json& d = spec.get("default");
        p.value = d.isBool() ? (d.asBool() ? 1.f : 0.f) : d.asFloat(0.f);
        if (p.type == ParamType::Trigger) p.value = 0.f;
        if (c.paramIndex(name) >= 0) return invalid("parameter \"" + name + "\"", "declared twice");
        c.params.push_back(p);
        return {};
    };
    if (paramsJ.isObject()) {
        for (const auto& [name, spec] : paramsJ.members()) {
            if (Status st = addParam(name, spec); !st) return st.error();
        }
    } else if (paramsJ.isArray()) {
        for (const auto& spec : paramsJ.elements()) {
            if (Status st = addParam(spec.get("name").asString(), spec); !st) return st.error();
        }
    }
    if (doc.contains("layers")) {
        size_t i = 0;
        for (const auto& lj : doc.get("layers").elements()) {
            auto L = parseLayer(lj, i++);
            if (!L) return L.error();
            c.layers.push_back(std::move(*L));
        }
    } else if (doc.contains("states")) {  // shorthand: a single layer at the top level
        Json lj = Json::object({{"states", doc.get("states")}});
        if (doc.contains("transitions")) lj["transitions"] = doc.get("transitions");
        if (doc.contains("default")) lj["default"] = doc.get("default");
        auto L = parseLayer(lj, 0);
        if (!L) return L.error();
        c.layers.push_back(std::move(*L));
    }
    if (c.layers.empty()) return invalid("controller", "needs \"layers\" (or top-level \"states\")");
    // Bare names in conditions refer to triggers when the parameter is one.
    for (auto& L : c.layers) {
        for (auto& t : L.transitions) {
            for (auto& cond : t.conditions) {
                int pi = c.paramIndex(cond.param);
                if (pi >= 0 && c.params[static_cast<size_t>(pi)].type == ParamType::Trigger && cond.op == CondOp::IsTrue) {
                    cond.op = CondOp::Trigger;
                }
            }
        }
    }
    return c;
}


Json ControllerDef::toJson() const {
    Json paramsJ = Json::object();
    for (const auto& p : params) {
        if (p.type == ParamType::Trigger || p.value == 0.f) {
            paramsJ[p.name] = toString(p.type);
        } else {
            Json d = p.type == ParamType::Bool ? Json(p.value != 0.f) : Json(p.value);
            paramsJ[p.name] = Json::object({{"type", toString(p.type)}, {"default", d}});
        }
    }
    Json layersJ = Json::array();
    for (const auto& L : layers) {
        Json states = Json::object();
        for (const auto& s : L.states) {
            Json sj = Json::object();
            if (s.kind == StateKind::Clip) {
                sj["clip"] = s.motions.empty() ? Json("") : Json(s.motions[0].clip);
            } else {
                Json motions = Json::array();
                for (const auto& m : s.motions) motions.push(motionJson(m, s.kind));
                if (s.kind == StateKind::Blend1D) {
                    sj["blend"] = Json::object({{"parameter", s.paramX}, {"motions", motions}});
                } else {
                    sj["blend2d"] = Json::object({{"x", s.paramX}, {"y", s.paramY}, {"motions", motions}});
                    if (s.mode2d == Blend2DMode::Directional) sj["blend2d"]["mode"] = "directional";
                }
            }
            if (s.speed != 1.f) sj["speed"] = s.speed;
            if (!s.speedParam.empty()) sj["speedParameter"] = s.speedParam;
            if (!s.loop) sj["loop"] = false;
            if (!s.events.empty()) {
                Json ev = Json::array();
                for (const auto& e : s.events) ev.push(Json::object({{"time", e.time}, {"name", e.name}}));
                sj["events"] = ev;
            }
            states[s.name] = sj;
        }
        Json transitions = Json::array();
        for (const auto& t : L.transitions) {
            Json tj = Json::object({{"from", t.from}, {"to", t.to}});
            if (!t.conditions.empty()) {
                std::string when;
                for (const auto& c : t.conditions) when += (when.empty() ? "" : " and ") + conditionText(c);
                tj["when"] = when;
            }
            tj["duration"] = t.duration;
            if (t.exitTime >= 0.f) tj["exit"] = t.exitTime;
            if (t.offset > 0.f) tj["offset"] = t.offset;
            if (t.interruptible) tj["interruptible"] = true;
            transitions.push(std::move(tj));
        }
        Json lj = Json::object({{"name", L.name}, {"default", L.defaultState}, {"states", states}, {"transitions", transitions}});
        if (L.weight != 1.f) lj["weight"] = L.weight;
        if (!L.weightParam.empty()) lj["weightParameter"] = L.weightParam;
        if (L.additive) lj["blending"] = "additive";
        if (!L.mask.empty()) {
            Json mask = Json::array();
            for (const auto& m : L.mask) mask.push(m);
            lj["mask"] = mask;
        }
        layersJ.push(std::move(lj));
    }
    Json doc = Json::object({{"format", "skywalker.animctl"}, {"version", 1}});
    if (!library.empty()) doc["library"] = library;
    doc["parameters"] = paramsJ;
    doc["layers"] = layersJ;
    return doc;
}

Status ControllerDef::validate(const std::function<bool(const std::string&)>& hasClip, const std::vector<std::string>& clipNames,
                               const Skeleton* skeleton) const {
    std::vector<std::string> paramNames;
    for (const auto& p : params) paramNames.push_back(p.name);
    auto numericParam = [&](const std::string& name, const std::string& where) -> Status {
        int pi = paramIndex(name);
        if (pi < 0) return invalid(where, "unknown parameter \"" + name + "\"", didYouMean(name, paramNames));
        ParamType t = params[static_cast<size_t>(pi)].type;
        if (t == ParamType::Trigger) return invalid(where, "parameter \"" + name + "\" is a trigger, not a number");
        return {};
    };
    for (const auto& L : layers) {
        std::vector<std::string> stateNames;
        for (const auto& s : L.states) stateNames.push_back(s.name);
        std::string where = "layer \"" + L.name + "\"";
        if (L.stateIndex(L.defaultState) < 0) {
            return invalid(where, "default state \"" + L.defaultState + "\" does not exist", didYouMean(L.defaultState, stateNames));
        }
        if (!L.weightParam.empty()) {
            if (Status s = numericParam(L.weightParam, where); !s) return s;
        }
        for (const auto& s : L.states) {
            std::string sw = where + " state \"" + s.name + "\"";
            for (const auto& m : s.motions) {
                if (hasClip && !hasClip(m.clip)) {
                    return Error::make("unknown_clip", sw + ": no clip \"" + m.clip + "\"", didYouMean(m.clip, clipNames));
                }
            }
            if (s.kind != StateKind::Clip) {
                if (Status st = numericParam(s.paramX, sw); !st) return st;
                if (s.kind == StateKind::Blend2D) {
                    if (Status st = numericParam(s.paramY, sw); !st) return st;
                }
            }
            if (!s.speedParam.empty()) {
                if (Status st = numericParam(s.speedParam, sw); !st) return st;
            }
        }
        for (const auto& t : L.transitions) {
            std::string tw = where + " transition " + t.from + " -> " + t.to;
            if (t.from != "any" && L.stateIndex(t.from) < 0) {
                return invalid(tw, "no state \"" + t.from + "\"", didYouMean(t.from, stateNames));
            }
            if (L.stateIndex(t.to) < 0) return invalid(tw, "no state \"" + t.to + "\"", didYouMean(t.to, stateNames));
            for (const auto& c : t.conditions) {
                int pi = paramIndex(c.param);
                if (pi < 0) return invalid(tw, "unknown parameter \"" + c.param + "\"", didYouMean(c.param, paramNames));
                ParamType type = params[static_cast<size_t>(pi)].type;
                bool compare = c.op != CondOp::IsTrue && c.op != CondOp::IsFalse && c.op != CondOp::Trigger;
                if (compare && (type == ParamType::Trigger || type == ParamType::Bool)) {
                    return invalid(tw, "\"" + c.param + "\" is a " + toString(type) + "; compare numbers only",
                                   type == ParamType::Bool ? "write \"" + c.param + "\" or \"!" + c.param + "\"" : "write just \"" + c.param + "\"");
                }
                if (!compare && (type == ParamType::Float || type == ParamType::Int)) {
                    return invalid(tw, "\"" + c.param + "\" is a number; compare it", "e.g. \"" + c.param + " > 0.1\"");
                }
            }
        }
        if (skeleton) {
            for (const auto& bone : L.mask) {
                if (skeleton->find(bone) < 0) {
                    return Error::make("unknown_bone", where + ": mask bone \"" + bone + "\" is not in the skeleton",
                                       didYouMean(bone, skeleton->names()));
                }
            }
        }
    }
    return {};
}

Result<ControllerDef> loadController(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Error::make("not_found", "cannot read controller " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    if (!doc) return doc.error();
    return ControllerDef::fromJson(*doc);
}

Status saveController(const std::string& path, const ControllerDef& def) {
    std::ofstream f(path);
    if (!f) return Error::make("io_error", "cannot write " + path);
    f << def.toJson().dump(2) << "\n";
    return {};
}

ControllerDef simpleController(const std::vector<std::string>& clips, const std::string& defaultClip, bool loop) {
    ControllerDef c;
    LayerDef L;
    for (const auto& name : clips) {
        StateDef s;
        s.name = name;
        s.motions.push_back(clipMotion(name));
        s.loop = loop;
        L.states.push_back(std::move(s));
    }
    L.defaultState = defaultClip.empty() && !clips.empty() ? clips.front() : defaultClip;
    c.layers.push_back(std::move(L));
    return c;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

Status AnimatorRuntime::init(std::shared_ptr<const Library> library, std::shared_ptr<const ControllerDef> controller,
                             const ClipResolver& resolve) {
    if (!library || !controller) return Error::make("invalid_argument", "animator needs a library and a controller");
    library_ = std::move(library);
    controller_ = std::move(controller);
    resolve_ = resolve;
    params_.clear();
    triggerAge_.clear();
    for (const auto& p : controller_->params) {
        params_.push_back(p.value);
        triggerAge_.push_back(-1.f);
    }
    layers_.clear();
    const Skeleton& sk = library_->skeleton;
    for (const auto& L : controller_->layers) {
        Layer layer;
        layer.def = &L;
        for (const auto& s : L.states) {
            State st;
            st.def = &s;
            for (const auto& m : s.motions) st.motions.push_back({resolve_ ? resolve_(m.clip) : nullptr, m});
            if (s.kind == StateKind::Blend1D) {
                std::stable_sort(st.motions.begin(), st.motions.end(),
                                 [](const Motion& a, const Motion& b) { return a.def.threshold < b.def.threshold; });
            }
            layer.states.push_back(std::move(st));
        }
        if (!L.mask.empty()) {
            layer.mask.assign(sk.bones.size(), false);
            for (const auto& name : L.mask) {
                int root = sk.find(name);
                if (root < 0) continue;
                for (size_t b = 0; b < sk.bones.size(); ++b) {
                    if (sk.isDescendant(static_cast<int>(b), root)) layer.mask[b] = true;
                }
            }
        }
        layer.current.state = L.states.empty() ? -1 : std::max(0, L.stateIndex(L.defaultState));  // a library without clips plays nothing
        layers_.push_back(std::move(layer));
    }
    setRootMotion(rootMotion_, up_, rootYaw_);
    return {};
}

void AnimatorRuntime::setRootMotion(bool enabled, Vec3 up, bool yaw) {
    rootMotion_ = enabled;
    rootYaw_ = enabled && yaw;
    up_ = length(up) > 1e-6f ? normalize(up) : Vec3{0, 1, 0};
    // Characters face +Z in their model (glTF) space; keep the forward perpendicular to up.
    forward_ = Vec3{0, 0, 1} - up_ * up_.z;
    if (length(forward_) < 1e-3f) forward_ = Vec3{1, 0, 0} - up_ * up_.x;
    forward_ = normalize(forward_);
    rootParent_ = Mat4{};
    if (library_ && library_->rootBone >= 0) {
        const Skeleton& sk = library_->skeleton;
        std::vector<Mat4> rest;
        computeGlobals(sk, restPose(sk), rest);
        int parent = sk.bones[static_cast<size_t>(library_->rootBone)].parent;
        if (parent >= 0) rootParent_ = rest[static_cast<size_t>(parent)];
    }
    rootParentInv_ = rootParent_.inverse();
}

bool AnimatorRuntime::setParam(std::string_view name, float value) {
    int i = controller_ ? controller_->paramIndex(name) : -1;
    if (i < 0) return false;
    const ParamDef& p = controller_->params[static_cast<size_t>(i)];
    switch (p.type) {
        case ParamType::Int: value = std::round(value); break;
        case ParamType::Bool: value = value != 0.f ? 1.f : 0.f; break;
        case ParamType::Trigger:
            value = value != 0.f ? 1.f : 0.f;
            triggerAge_[static_cast<size_t>(i)] = value != 0.f ? 0.f : -1.f;
            break;
        case ParamType::Float: break;
    }
    params_[static_cast<size_t>(i)] = std::isfinite(value) ? value : 0.f;
    return true;
}

bool AnimatorRuntime::trigger(std::string_view name) { return setParam(name, 1.f); }

std::optional<float> AnimatorRuntime::param(std::string_view name) const {
    int i = controller_ ? controller_->paramIndex(name) : -1;
    if (i < 0) return std::nullopt;
    return params_[static_cast<size_t>(i)];
}

Json AnimatorRuntime::paramsJson() const {
    Json j = Json::object();
    if (!controller_) return j;
    for (size_t i = 0; i < controller_->params.size(); ++i) {
        const ParamDef& p = controller_->params[i];
        if (p.type == ParamType::Bool || p.type == ParamType::Trigger) {
            j[p.name] = params_[i] != 0.f;
        } else {
            j[p.name] = params_[i];
        }
    }
    return j;
}

int AnimatorRuntime::resolveState(Layer& layer, const std::string& name, bool allowClips) {
    int s = layer.def->stateIndex(name);
    if (s >= 0) return s;
    for (size_t i = layer.def->states.size(); i < layer.states.size(); ++i) {  // one-shot clip states
        if (layer.states[i].owned.name == name) return static_cast<int>(i);
    }
    if (!allowClips || !resolve_) return -1;
    std::shared_ptr<const Clip> clip = resolve_(name);
    if (!clip) return -1;
    State st;
    st.owned.name = name;
    st.owned.motions.push_back(clipMotion(name));
    st.owned.loop = false;
    st.motions.push_back({clip, st.owned.motions[0]});
    layer.states.push_back(std::move(st));
    return static_cast<int>(layer.states.size() - 1);
}

void AnimatorRuntime::enter(Layer& layer, int state, float fade, float offset, bool interruptible) {
    if (fade > 0.f && layer.current.state >= 0) {
        if (layer.fading) {  // interrupted: blend out of the pose we are showing right now
            Pose snapshot;
            layerPose(layer, snapshot);
            layer.frozen = std::move(snapshot);
            layer.frozenSource = true;
        } else {
            layer.previous = layer.current;
            layer.frozenSource = false;
        }
        layer.fading = true;
        layer.fadeElapsed = 0.f;
        layer.fadeDuration = fade;
    } else {
        layer.fading = false;
        layer.frozenSource = false;
    }
    layer.interruptible = interruptible;
    layer.current = Playing{};
    layer.current.state = state;
    layer.current.nt = offset;
}

Status AnimatorRuntime::play(const std::string& name, float fade, int layerIndex, std::optional<bool> loop) {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= layers_.size()) {
        return Error::make("invalid_argument", "animator has no layer " + std::to_string(layerIndex));
    }
    Layer& layer = layers_[static_cast<size_t>(layerIndex)];
    int s = resolveState(layer, name, true);
    if (s < 0) {
        std::vector<std::string> options;
        for (const auto& st : layer.def->states) options.push_back(st.name);
        for (const auto& c : library_->clips) options.push_back(c.name);
        return Error::make("not_found", "no state or clip \"" + name + "\"", didYouMean(name, options));
    }
    if (s == layer.current.state && !(layer.fading && layer.previous.state == s)) {
        if (loop) layer.current.loop = loop;  // already playing: keep going (calling play every tick is harmless)
        return {};
    }
    enter(layer, s, std::max(0.f, fade), 0.f, true);
    layer.current.loop = loop;
    const StateDef& def = layer.states[static_cast<size_t>(s)].def ? *layer.states[static_cast<size_t>(s)].def
                                                                  : layer.states[static_cast<size_t>(s)].owned;
    bool hasExits = false;
    for (const auto& t : layer.def->transitions) hasExits = hasExits || t.from == def.name;
    layer.current.returnToDefault = s != layer.def->stateIndex(layer.def->defaultState) && !hasExits;
    return {};
}

Status AnimatorRuntime::seek(const std::string& name, float normalizedTime, int layerIndex) {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= layers_.size()) {
        return Error::make("invalid_argument", "animator has no layer " + std::to_string(layerIndex));
    }
    Layer& layer = layers_[static_cast<size_t>(layerIndex)];
    int s = name.empty() ? layer.def->stateIndex(layer.def->defaultState) : resolveState(layer, name, true);
    if (s < 0) {
        std::vector<std::string> options;
        for (const auto& st : layer.def->states) options.push_back(st.name);
        for (const auto& c : library_->clips) options.push_back(c.name);
        return Error::make("not_found", "no state or clip \"" + name + "\"", didYouMean(name, options));
    }
    layer.fading = false;
    layer.frozenSource = false;
    layer.current = Playing{};
    layer.current.state = s;
    layer.current.nt = std::max(0.f, normalizedTime);
    return {};
}

namespace {

/// Weights sum to 1; outside every band the nearest motion plays alone.
void normalizeBlendWeights(const std::vector<Vec2>& pos, Vec2 pt, std::vector<float>& w) {
    float total = 0.f;
    for (float x : w) total += x;
    if (total > 1e-6f) {
        for (float& x : w) x /= total;
        return;
    }
    size_t best = 0;
    float bestD = 1e30f;
    for (size_t i = 0; i < pos.size(); ++i) {
        Vec2 d = pos[i] - pt;
        if (d.x * d.x + d.y * d.y < bestD) {
            bestD = d.x * d.x + d.y * d.y;
            best = i;
        }
    }
    std::fill(w.begin(), w.end(), 0.f);
    if (!w.empty()) w[best] = 1.f;
}

}  // namespace

std::vector<float> blendWeights2D(const std::vector<Vec2>& pos, Vec2 pt) {
    const size_t n = pos.size();
    std::vector<float> w(n, 0.f);
    for (size_t i = 0; i < n; ++i) {
        float h = 1.f;
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            Vec2 pij = pos[j] - pos[i];
            float len2 = pij.x * pij.x + pij.y * pij.y;
            if (len2 < 1e-9f) continue;
            Vec2 pip = pt - pos[i];
            h = std::min(h, std::clamp(1.f - (pip.x * pij.x + pip.y * pij.y) / len2, 0.f, 1.f));
        }
        w[i] = h;
    }
    normalizeBlendWeights(pos, pt, w);
    return w;
}

std::vector<float> blendWeightsDirectional(const std::vector<Vec2>& pos, Vec2 pt) {
    // Polar gradient bands (Johansen 2009): the band coordinates are the magnitude difference
    // (relative to the pair's mean speed) and the angle between directions (times kAngle). A pair
    // with a motion at the origin (idle) has no direction: it blends by speed only (against the motions
    // ahead of the query), so idle fades out with speed in every direction and never steals weight at
    // full speed between two clips.
    constexpr float kAngle = 2.f;
    const size_t n = pos.size();
    std::vector<float> w(n, 0.f);
    auto len = [](Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); };
    auto angleBetween = [](Vec2 a, Vec2 b) { return std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y); };
    const float lp = len(pt);
    for (size_t i = 0; i < n; ++i) {
        const float li = len(pos[i]);
        float h = 1.f;
        for (size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            const float lj = len(pos[j]);
            const float mean = (li + lj) * 0.5f;
            if (mean < 1e-6f) continue;
            // Idle fades by the speed of the motions ahead of the query, not of those behind it.
            if (li < 1e-6f && lp >= 1e-6f && pt.x * pos[j].x + pt.y * pos[j].y <= 0.f) continue;
            float aij = 0.f, aip = 0.f;
            if (li >= 1e-6f && lj >= 1e-6f) {
                aij = angleBetween(pos[i], pos[j]);
                aip = lp < 1e-6f ? 0.f : angleBetween(pos[i], pt);
            }
            Vec2 bij{(lj - li) / mean, aij * kAngle};
            Vec2 bip{(lp - li) / mean, aip * kAngle};
            float len2 = bij.x * bij.x + bij.y * bij.y;
            if (len2 < 1e-9f) continue;
            h = std::min(h, std::clamp(1.f - (bip.x * bij.x + bip.y * bij.y) / len2, 0.f, 1.f));
        }
        w[i] = h;
    }
    normalizeBlendWeights(pos, pt, w);
    return w;
}

std::vector<float> AnimatorRuntime::weights(const Layer& layer, const Playing& p) const {
    const State& st = layer.states[static_cast<size_t>(p.state)];
    const StateDef& def = st.def ? *st.def : st.owned;
    const size_t n = st.motions.size();
    std::vector<float> w(n, 0.f);
    if (n == 0) return w;
    if (def.kind == StateKind::Clip || n == 1) {
        w[0] = 1.f;
        return w;
    }
    auto value = [&](const std::string& name) {
        auto v = param(name);
        return v ? *v : 0.f;
    };
    if (def.kind == StateKind::Blend1D) {
        float x = value(def.paramX);
        if (x <= st.motions.front().def.threshold) {
            w.front() = 1.f;
        } else if (x >= st.motions.back().def.threshold) {
            w.back() = 1.f;
        } else {
            for (size_t i = 0; i + 1 < n; ++i) {
                float a = st.motions[i].def.threshold, b = st.motions[i + 1].def.threshold;
                if (x >= a && x <= b) {
                    float t = b - a > 1e-6f ? (x - a) / (b - a) : 0.f;
                    w[i] = 1.f - t;
                    w[i + 1] = t;
                    break;
                }
            }
        }
        return w;
    }
    // 2D freeform (gradient band interpolation, Johansen 2009).
    Vec2 pt{value(def.paramX), value(def.paramY)};
    std::vector<Vec2> pos(n);
    for (size_t i = 0; i < n; ++i) pos[i] = st.motions[i].def.position;
    return def.mode2d == Blend2DMode::Directional ? blendWeightsDirectional(pos, pt) : blendWeights2D(pos, pt);
}

bool AnimatorRuntime::looping(const Layer& layer, const Playing& p) const {
    const State& st = layer.states[static_cast<size_t>(p.state)];
    return p.loop.value_or(st.def ? st.def->loop : st.owned.loop);
}

float AnimatorRuntime::stateDuration(int layerIndex, int state) const {
    const Layer& layer = layers_[static_cast<size_t>(layerIndex)];
    Playing p;
    p.state = state;
    std::vector<float> w = weights(layer, p);
    const State& st = layer.states[static_cast<size_t>(state)];
    float d = 0.f;
    for (size_t i = 0; i < st.motions.size(); ++i) {
        if (!st.motions[i].clip) continue;
        d += w[i] * st.motions[i].clip->duration / std::max(st.motions[i].def.speed, 1e-3f);
    }
    return d;
}

float AnimatorRuntime::currentDuration(int layerIndex) const {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= layers_.size()) return 0.f;
    int s = layers_[static_cast<size_t>(layerIndex)].current.state;
    return s < 0 ? 0.f : stateDuration(layerIndex, s);
}

void AnimatorRuntime::advance(Layer& layer, Playing& p, float dt) const {
    if (p.state < 0) return;
    const State& st = layer.states[static_cast<size_t>(p.state)];
    const StateDef& def = st.def ? *st.def : st.owned;
    float speed = def.speed;
    if (!def.speedParam.empty()) speed *= param(def.speedParam).value_or(1.f);
    float dur = stateDuration(static_cast<int>(&layer - layers_.data()), p.state);
    p.nt += dt * speed / (dur > 1e-4f ? dur : 1.f);
    if (!looping(layer, p)) p.nt = std::min(p.nt, 1.f);
    p.nt = std::max(p.nt, 0.f);
}

void AnimatorRuntime::samplePlaying(const Layer& layer, const Playing& p, Pose& out) const {
    const Skeleton& sk = library_->skeleton;
    out = restPose(sk);
    if (p.state < 0) return;
    const State& st = layer.states[static_cast<size_t>(p.state)];
    std::vector<float> w = weights(layer, p);
    const bool loop = looping(layer, p);
    float phase = loop ? fract(p.nt) : std::clamp(p.nt, 0.f, 1.f);
    if (loop && phase == 0.f && p.nt > 0.f) phase = 1.f;  // the end of a cycle shows its last frame (scrubbing to the end)
    const int root = library_->rootBone;
    const bool pinRoot = rootMotion_ && root >= 0 && &layer == &layers_.front();
    bool first = true;
    Pose tmp;
    for (size_t i = 0; i < st.motions.size(); ++i) {
        if (w[i] <= 1e-5f || !st.motions[i].clip) continue;
        const Clip& clip = *st.motions[i].clip;
        tmp = restPose(sk);
        sampleClip(clip, phase * clip.duration, tmp);
        if (pinRoot) {
            // Keep the root bone over its starting spot; the movement becomes root motion.
            Vec3 now = tmp[static_cast<size_t>(root)].t;
            Vec3 start = sampleTranslation(clip, root, 0.f, sk.bones[static_cast<size_t>(root)].rest.t);
            Vec3 posModel = rootParent_.transformPoint(now) - horizontal(now) + horizontal(start);
            tmp[static_cast<size_t>(root)].t = rootParentInv_.transformPoint(posModel);
            if (rootYaw_) {
                // The turn so far becomes the entity's: the body keeps facing the entity's forward.
                const Quat parent = rotationOf(rootParent_);
                const Quat undo = Quat::axisAngle(up_, -rootTurn(clip, 0.f, phase));
                Quat& r = tmp[static_cast<size_t>(root)].r;
                r = (parent.conjugate() * undo * parent * r).normalized();
            }
        }
        if (first) {
            for (size_t b = 0; b < out.size(); ++b) {
                out[b].t = tmp[b].t * w[i];
                out[b].s = tmp[b].s * w[i];
                out[b].r = Quat{tmp[b].r.x * w[i], tmp[b].r.y * w[i], tmp[b].r.z * w[i], tmp[b].r.w * w[i]};
            }
            first = false;
            continue;
        }
        for (size_t b = 0; b < out.size(); ++b) {
            out[b].t += tmp[b].t * w[i];
            out[b].s += tmp[b].s * w[i];
            Quat q = tmp[b].r;
            float sgn = dot(out[b].r, q) < 0.f ? -w[i] : w[i];
            out[b].r = Quat{out[b].r.x + q.x * sgn, out[b].r.y + q.y * sgn, out[b].r.z + q.z * sgn, out[b].r.w + q.w * sgn};
        }
    }
    if (first) return;  // no motions: rest pose
    for (auto& t : out) t.r = t.r.normalized();
}

void AnimatorRuntime::layerPose(const Layer& layer, Pose& out) const {
    samplePlaying(layer, layer.current, out);
    if (!layer.fading) return;
    Pose src;
    if (layer.frozenSource) {
        src = layer.frozen;
    } else {
        samplePlaying(layer, layer.previous, src);
    }
    float w = smooth01(layer.fadeDuration > 0.f ? layer.fadeElapsed / layer.fadeDuration : 1.f);
    blendPoses(src, out, w, out);
}

void AnimatorRuntime::evaluate(Pose& out) const {
    if (!library_) {
        out.clear();
        return;
    }
    if (layers_.empty()) {
        out = restPose(library_->skeleton);
        return;
    }
    layerPose(layers_.front(), out);
    Pose lp, ref;
    for (size_t li = 1; li < layers_.size(); ++li) {
        const Layer& layer = layers_[li];
        float w = layer.def->weight;
        if (!layer.def->weightParam.empty()) w *= std::clamp(param(layer.def->weightParam).value_or(0.f), 0.f, 1.f);
        if (w <= 1e-4f || layer.current.state < 0) continue;
        layerPose(layer, lp);
        if (!layer.def->additive) {
            if (layer.mask.empty()) {
                blendPoses(out, lp, w, out);
            } else {
                blendPosesMasked(out, lp, w, layer.mask, out);
            }
            continue;
        }
        // Additive: the layer's motion relative to its own first frame, scaled by weight.
        Playing start = layer.current;
        start.nt = 0.f;
        samplePlaying(layer, start, ref);
        for (size_t b = 0; b < out.size() && b < lp.size(); ++b) {
            if (!layer.mask.empty() && !layer.mask[b]) continue;
            Quat delta = slerp(Quat{}, ref[b].r.conjugate() * lp[b].r, w);
            out[b].r = (out[b].r * delta).normalized();
            out[b].t += (lp[b].t - ref[b].t) * w;
        }
    }
}

bool AnimatorRuntime::conditionsHold(const TransitionDef& t) const {
    for (const auto& c : t.conditions) {
        auto v = param(c.param);
        if (!v) return false;
        float x = *v;
        bool ok = false;
        switch (c.op) {
            case CondOp::Greater: ok = x > c.value; break;
            case CondOp::Less: ok = x < c.value; break;
            case CondOp::GreaterEq: ok = x >= c.value; break;
            case CondOp::LessEq: ok = x <= c.value; break;
            case CondOp::Equal: ok = std::fabs(x - c.value) < 1e-5f; break;
            case CondOp::NotEqual: ok = std::fabs(x - c.value) >= 1e-5f; break;
            case CondOp::IsTrue:
            case CondOp::Trigger: ok = x != 0.f; break;
            case CondOp::IsFalse: ok = x == 0.f; break;
        }
        if (!ok) return false;
    }
    return true;
}

void AnimatorRuntime::consumeTriggers(const TransitionDef& t) {
    for (const auto& c : t.conditions) {
        if (c.op != CondOp::Trigger) continue;
        int i = controller_->paramIndex(c.param);
        if (i < 0) continue;
        params_[static_cast<size_t>(i)] = 0.f;
        triggerAge_[static_cast<size_t>(i)] = -1.f;
    }
}

Vec3 AnimatorRuntime::horizontal(Vec3 rootLocal) const {
    Vec3 p = rootParent_.transformPoint(rootLocal);
    return p - up_ * dot(p, up_);
}

float AnimatorRuntime::rootHeading(const Clip& clip, float seconds) const {
    const int root = library_->rootBone;
    const Trs& rest = library_->skeleton.bones[static_cast<size_t>(root)].rest;
    Quat local = rest.r;
    for (const auto& ch : clip.channels) {
        if (ch.bone == root && ch.path == Path::Rotation) {
            Trs base;
            base.r = rest.r;
            local = sampleChannel(ch, seconds, base).r;
            break;
        }
    }
    // The rest-relative turn of the root in model space, applied to the model's forward.
    const Quat parent = rotationOf(rootParent_);
    const Quat now = parent * local, restQ = parent * rest.r;
    Vec3 f = (now * restQ.conjugate()).rotate(forward_);
    f = f - up_ * dot(f, up_);
    if (length(f) < 1e-5f) return 0.f;
    f = normalize(f);
    return std::atan2(dot(cross(forward_, f), up_), dot(forward_, f));
}

float AnimatorRuntime::rootTurn(const Clip& clip, float a, float b) const {
    // Integrate wrapped differences so a 360 degree turn clip counts fully.
    const int steps = std::clamp(static_cast<int>(std::ceil(std::fabs(b - a) * 24.f)), 1, 48);
    float total = 0.f, prev = rootHeading(clip, a * clip.duration);
    for (int i = 1; i <= steps; ++i) {
        float h = rootHeading(clip, (a + (b - a) * static_cast<float>(i) / static_cast<float>(steps)) * clip.duration);
        float d = h - prev;
        while (d > kPi) d -= 2.f * kPi;
        while (d < -kPi) d += 2.f * kPi;
        total += d;
        prev = h;
    }
    return total;
}

AnimatorRuntime::RootStep AnimatorRuntime::rootMotion(const Layer& layer, const Playing& p, float a, float b) const {
    const int root = library_->rootBone;
    if (p.state < 0 || root < 0) return {};
    const State& st = layer.states[static_cast<size_t>(p.state)];
    std::vector<float> w = weights(layer, p);
    const bool loop = looping(layer, p);
    const Vec3 rest = library_->skeleton.bones[static_cast<size_t>(root)].rest.t;
    RootStep total;
    for (size_t i = 0; i < st.motions.size(); ++i) {
        if (w[i] <= 1e-5f || !st.motions[i].clip) continue;
        const Clip& clip = *st.motions[i].clip;
        auto h = [&](float phase) { return horizontal(sampleTranslation(clip, root, phase * clip.duration, rest)); };
        // Displacements are measured in the clip's frame at its cycle start; with root yaw, each
        // later cycle starts turned by the cycle's total turn, and the result is expressed in the
        // frame the entity faces at phase a (it has already turned by the clip's heading so far).
        auto turnAbout = [&](float angle, Vec3 v) { return rootYaw_ ? Quat::axisAngle(up_, angle).rotate(v) : v; };
        Vec3 d{0, 0, 0};
        float yaw = 0.f;
        if (!loop) {
            float pa = std::clamp(a, 0.f, 1.f), pb = std::clamp(b, 0.f, 1.f);
            d = turnAbout(rootYaw_ ? -rootTurn(clip, 0.f, pa) : 0.f, h(pb) - h(pa));
            if (rootYaw_) yaw = rootTurn(clip, pa, pb);
        } else {
            float ka = std::floor(a), kb = std::floor(b);
            float fa = a - ka, fb = b - kb;
            const float cycleTurn = rootYaw_ ? rootTurn(clip, 0.f, 1.f) : 0.f;
            Vec3 inCycle;  // in the frame of the cycle containing `a`
            if (ka == kb) {
                inCycle = h(fb) - h(fa);
                if (rootYaw_) yaw = rootTurn(clip, fa, fb);
            } else {
                const Vec3 cycle = h(1.f) - h(0.f);
                inCycle = h(1.f) - h(fa);
                float heading = cycleTurn;  // each following cycle starts turned by one more cycle
                int full = static_cast<int>(std::min(kb - ka - 1.f, 4.f));
                for (int c = 0; c < full; ++c, heading += cycleTurn) inCycle += turnAbout(heading, cycle);
                inCycle += turnAbout(heading, h(fb) - h(0.f));
                if (rootYaw_) yaw = rootTurn(clip, fa, 1.f) + cycleTurn * static_cast<float>(full) + rootTurn(clip, 0.f, fb);
            }
            d = turnAbout(rootYaw_ ? -rootTurn(clip, 0.f, fa) : 0.f, inCycle);
        }
        total.delta += d * w[i];
        total.yaw += yaw * w[i];
    }
    return total;
}

void AnimatorRuntime::update(float dt, std::vector<Event>* events, Vec3* rootDelta, float* yawDelta) {
    if (rootDelta) *rootDelta = {0, 0, 0};
    if (yawDelta) *yawDelta = 0.f;
    if (!library_) return;
    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& layer = layers_[li];
        if (layer.current.state < 0) continue;
        const float before = layer.current.nt;
        const float beforePrev = layer.previous.nt;
        advance(layer, layer.current, dt);
        if (layer.fading && !layer.frozenSource) advance(layer, layer.previous, dt);
        float fadeW = 1.f;
        if (layer.fading) {
            layer.fadeElapsed += dt;
            fadeW = smooth01(layer.fadeDuration > 0.f ? layer.fadeElapsed / layer.fadeDuration : 1.f);
        }
        // Root motion (base layer): blended like the poses.
        if (li == 0 && rootMotion_ && rootDelta) {
            RootStep d = rootMotion(layer, layer.current, before, layer.current.nt);
            if (layer.fading && !layer.frozenSource) {
                RootStep ds = rootMotion(layer, layer.previous, beforePrev, layer.previous.nt);
                d.delta = ds.delta * (1.f - fadeW) + d.delta * fadeW;
                d.yaw = ds.yaw * (1.f - fadeW) + d.yaw * fadeW;
            } else if (layer.fading) {
                d.delta = d.delta * fadeW;
                d.yaw *= fadeW;
            }
            *rootDelta += d.delta;
            if (yawDelta && rootYaw_) *yawDelta += d.yaw;
        }
        // Events of the state being played.
        const State& st = layer.states[static_cast<size_t>(layer.current.state)];
        const StateDef& def = st.def ? *st.def : st.owned;
        const float from = layer.current.fresh ? before - 1e-6f : before;  // a new state fires its time-0 events
        const float now = layer.current.nt;
        const bool loop = looping(layer, layer.current);
        // Values e + k (k >= 0) crossed in (from, now]: k = floor(from - e) + 1 .. floor(now - e).
        auto crossings = [&](float e) {
            if (now < e) return 0;
            if (!loop) return from < e ? 1 : 0;
            float lo = std::max(std::floor(from - e) + 1.f, 0.f), hi = std::floor(now - e);
            return hi >= lo ? static_cast<int>(std::min(hi - lo + 1.f, 4.f)) : 0;
        };
        float layerWeight = 1.f;  // silent layers (weight 0) play on but fire no events
        if (li > 0) {
            layerWeight = layer.def->weight;
            if (!layer.def->weightParam.empty()) layerWeight *= std::clamp(param(layer.def->weightParam).value_or(0.f), 0.f, 1.f);
        }
        if (events && layerWeight > 1e-4f) {
            for (const auto& e : def.events) {
                for (int k = crossings(e.time); k > 0; --k) events->push_back({e.name, def.name, static_cast<int>(li)});
            }
        }
        layer.current.fresh = false;
        if (layer.fading && layer.fadeElapsed >= layer.fadeDuration) {
            layer.fading = false;
            layer.frozenSource = false;
        }
        // Transitions: "any" first, then from the current state; first match wins.
        if (!layer.fading || layer.interruptible) {
            const LayerDef& L = *layer.def;
            const TransitionDef* fire = nullptr;
            for (int pass = 0; pass < 2 && !fire; ++pass) {
                for (const auto& t : L.transitions) {
                    bool any = t.from == "any";
                    if ((pass == 0) != any) continue;
                    int to = L.stateIndex(t.to);
                    if (to < 0) continue;
                    if (any && to == layer.current.state) continue;
                    if (!any && L.stateIndex(t.from) != layer.current.state) continue;
                    if (t.exitTime >= 0.f) {
                        // Looping states leave when the exit time is crossed (any cycle);
                        // one-shot states once it has been reached.
                        bool reached = loop ? crossings(t.exitTime) > 0 : now >= std::min(t.exitTime, 1.f) - 1e-6f;
                        if (!reached) continue;
                    }
                    if (!conditionsHold(t)) continue;
                    fire = &t;
                    break;
                }
            }
            if (fire) {
                consumeTriggers(*fire);
                enter(layer, L.stateIndex(fire->to), fire->duration, fire->offset, fire->interruptible);
            } else if (layer.current.returnToDefault && !loop && now >= 1.f && !layer.fading) {
                enter(layer, std::max(0, L.stateIndex(L.defaultState)), 0.25f, 0.f, true);
            }
        }
    }
    // Unconsumed triggers disarm after a short grace period.
    for (size_t i = 0; i < triggerAge_.size(); ++i) {
        if (triggerAge_[i] < 0.f) continue;
        triggerAge_[i] += dt;
        if (triggerAge_[i] > kTriggerLife) {
            triggerAge_[i] = -1.f;
            params_[i] = 0.f;
        }
    }
}

std::string AnimatorRuntime::stateName(int layerIndex) const {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= layers_.size()) return "";
    const Layer& layer = layers_[static_cast<size_t>(layerIndex)];
    if (layer.current.state < 0) return "";
    const State& st = layer.states[static_cast<size_t>(layer.current.state)];
    return st.def ? st.def->name : st.owned.name;
}

float AnimatorRuntime::normalizedTime(int layerIndex) const {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= layers_.size()) return 0.f;
    return layers_[static_cast<size_t>(layerIndex)].current.nt;
}

bool AnimatorRuntime::inTransition(int layerIndex) const {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= layers_.size()) return false;
    return layers_[static_cast<size_t>(layerIndex)].fading;
}

Json AnimatorRuntime::stateJson() const {
    Json layers = Json::array();
    for (size_t li = 0; li < layers_.size(); ++li) {
        const Layer& layer = layers_[li];
        Json lj = Json::object({{"layer", layer.def->name},
                                {"state", stateName(static_cast<int>(li))},
                                {"normalizedTime", std::round(layer.current.nt * 1000.f) / 1000.f}});
        if (layer.fading) {
            lj["transition"] = Json::object({{"progress", std::round(layer.fadeElapsed / std::max(layer.fadeDuration, 1e-4f) * 100.f) / 100.f}});
        }
        layers.push(std::move(lj));
    }
    return Json::object({{"layers", layers}, {"parameters", paramsJson()}});
}

}  // namespace sky::anim
