#include "skywalker/wander/Value.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/core/Strings.h"

namespace sky::wander {

const char* typeName(VType t) {
    switch (t) {
        case VType::None: return "none";
        case VType::Bool: return "bool";
        case VType::Number: return "number";
        case VType::Vec: return "vector";
        case VType::Color: return "color";
        case VType::Entity: return "entity";
        case VType::String: return "string";
        case VType::List: return "list";
        case VType::Map: return "map";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Heap objects
// ---------------------------------------------------------------------------

void destroyObject(Obj* o) {
    switch (o->kind) {
        case VType::String: delete reinterpret_cast<StrObj*>(o); return;
        case VType::List: delete reinterpret_cast<ListObj*>(o); return;
        case VType::Map: delete reinterpret_cast<MapObj*>(o); return;
        default: return;
    }
}

const Value* MapObj::find(std::string_view key) const {
    for (const auto& [k, v] : entries) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value* MapObj::find(std::string_view key) {
    for (auto& [k, v] : entries) {
        if (k == key) return &v;
    }
    return nullptr;
}

void MapObj::set(std::string_view key, Value v) {
    if (Value* existing = find(key)) {
        *existing = std::move(v);
        return;
    }
    entries.emplace_back(std::string(key), std::move(v));
}

bool MapObj::erase(std::string_view key) {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first == key) {
            entries.erase(it);
            return true;
        }
    }
    return false;
}

Value Value::string(std::string s) {
    auto* o = new StrObj;
    o->s = std::move(s);
    Value v;
    v.t_ = VType::String;
    v.u_.o = &o->h;
    return v;
}

Value Value::list(std::vector<Value> items) {
    auto* o = new ListObj;
    o->items = std::move(items);
    Value v;
    v.t_ = VType::List;
    v.u_.o = &o->h;
    return v;
}

Value Value::map() {
    auto* o = new MapObj;
    Value v;
    v.t_ = VType::Map;
    v.u_.o = &o->h;
    return v;
}

std::vector<Value>& Value::mutItems() {
    auto* o = reinterpret_cast<ListObj*>(u_.o);
    if (o->h.rc > 1) {  // shared: copy on write
        auto* copy = new ListObj;
        copy->items = o->items;
        --o->h.rc;
        u_.o = &copy->h;
        o = copy;
    }
    return o->items;
}

MapObj& Value::mutMap() {
    auto* o = reinterpret_cast<MapObj*>(u_.o);
    if (o->h.rc > 1) {
        auto* copy = new MapObj;
        copy->entries = o->entries;
        --o->h.rc;
        u_.o = &copy->h;
        o = copy;
    }
    return *o;
}

bool Value::truthyData() const {
    switch (t_) {
        case VType::None: return false;
        case VType::Bool: return u_.b != 0;
        case VType::Number: return u_.n != 0;
        case VType::String: return !str().empty();
        case VType::List: return !items().empty();
        case VType::Map: return !mapObj().entries.empty();
        default: return true;
    }
}

bool Value::operator==(const Value& o) const {
    // Numbers and booleans compare by value (true == 1), like Wander 1.
    auto numeric = [](const Value& x) { return x.t_ == VType::Number || x.t_ == VType::Bool; };
    if (numeric(*this) && numeric(o)) {
        double a = t_ == VType::Bool ? (u_.b ? 1.0 : 0.0) : u_.n;
        double b = o.t_ == VType::Bool ? (o.u_.b ? 1.0 : 0.0) : o.u_.n;
        return a == b;
    }
    if (t_ != o.t_) return false;
    switch (t_) {
        case VType::None: return true;
        case VType::Vec: return v() == o.v();
        case VType::Color: return c() == o.c();
        case VType::Entity: return u_.e == o.u_.e;
        case VType::String: return u_.o == o.u_.o || str() == o.str();
        case VType::List: return u_.o == o.u_.o || items() == o.items();
        case VType::Map: {
            if (u_.o == o.u_.o) return true;
            const auto& a = mapObj().entries;
            const auto& b = o.mapObj();
            if (a.size() != b.entries.size()) return false;
            for (const auto& [k, v] : a) {
                const Value* other = b.find(k);
                if (!other || !(*other == v)) return false;
            }
            return true;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Text and JSON
// ---------------------------------------------------------------------------

std::string formatNumber(double d) {
    if (d == 0) return "0";  // also maps -0 to "0"
    std::ostringstream os;
    os << d;
    return os.str();
}

namespace {

void appendDisplay(std::string& out, const Value& v, bool quoteStrings) {
    switch (v.type()) {
        case VType::None: out += "none"; return;
        case VType::Bool: out += v.b() ? "true" : "false"; return;
        case VType::Number: out += formatNumber(v.num()); return;
        case VType::Vec: {
            Vec3 x = v.v();
            out += "(" + formatNumber(x.x) + ", " + formatNumber(x.y) + ", " + formatNumber(x.z) + ")";
            return;
        }
        case VType::Color: out += reflect::toHexColor(v.c()); return;
        case VType::Entity: out += "#" + std::to_string(v.e()); return;
        case VType::String:
            if (quoteStrings) {
                out += '"';
                out += v.str();
                out += '"';
            } else {
                out += v.str();
            }
            return;
        case VType::List: {
            out += "[";
            bool first = true;
            for (const auto& item : v.items()) {
                if (!first) out += ", ";
                first = false;
                appendDisplay(out, item, true);
            }
            out += "]";
            return;
        }
        case VType::Map: {
            out += "{";
            bool first = true;
            for (const auto& [k, item] : v.mapObj().entries) {
                if (!first) out += ", ";
                first = false;
                out += k + ": ";
                appendDisplay(out, item, true);
            }
            out += "}";
            return;
        }
    }
}

bool allNumbers(const Json& j) {
    for (const auto& e : j.elements()) {
        if (!e.isNumber()) return false;
    }
    return true;
}

}  // namespace

std::string toDisplayString(const Value& v) {
    std::string out;
    appendDisplay(out, v, false);
    return out;
}

Json toJson(const Value& v) {
    switch (v.type()) {
        case VType::None: return {};
        case VType::Bool: return v.b();
        case VType::Number: return std::isfinite(v.num()) ? Json(v.num()) : Json();
        case VType::String: return v.str();
        // Full precision (no rounding): vars round-trip exactly through JSON.
        case VType::Vec: return Json::array({v.v().x, v.v().y, v.v().z});
        case VType::Color: {
            Vec4 c = v.c();
            return Json::array({c.x, c.y, c.z, c.w});
        }
        case VType::Entity: return Json::object({{"$entity", v.e()}});
        case VType::List: {
            Json arr = Json::array();
            for (const auto& item : v.items()) arr.push(toJson(item));
            return arr;
        }
        case VType::Map: {
            Json obj = Json::object();
            for (const auto& [k, item] : v.mapObj().entries) obj[k] = toJson(item);
            return obj;
        }
    }
    return {};
}

Value fromJson(const Json& j) {
    switch (j.type()) {
        case Json::Type::Null: return {};
        case Json::Type::Bool: return Value::boolean(j.asBool());
        case Json::Type::Number: return Value::number(j.asNumber());
        case Json::Type::String: {
            Vec4 c;
            const std::string& s = j.asString();
            if (s.size() > 1 && s[0] == '#' && reflect::parseHexColor(s, c)) return Value::color(c);
            return Value::string(s);
        }
        case Json::Type::Array: {
            // [x, y, z] is a vector and [r, g, b, a] a color (how vars, components and agents
            // write them); anything else is a list.
            if (j.size() == 3 && allNumbers(j)) return Value::vec({j[0].asFloat(), j[1].asFloat(), j[2].asFloat()});
            if (j.size() == 4 && allNumbers(j)) {
                return Value::color({j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j[3].asFloat()});
            }
            std::vector<Value> items;
            items.reserve(j.size());
            for (const auto& e : j.elements()) items.push_back(fromJson(e));
            return Value::list(std::move(items));
        }
        case Json::Type::Object: {
            if (j.members().size() == 1) {
                if (const Json* e = j.find("$entity"); e && e->isNumber()) {
                    return Value::entity(static_cast<EntityRef>(e->asInt()));
                }
            }
            Value m = Value::map();
            auto& mo = m.mutMap();
            for (const auto& [k, v] : j.members()) mo.entries.emplace_back(k, fromJson(v));
            return m;
        }
    }
    return {};
}

std::string typeSetName(TypeSet t) {
    if ((t & kTAny) == kTAny) return "any";
    if (t == 0) return "nothing";
    std::string out;
    for (uint32_t i = 0; i <= static_cast<uint32_t>(VType::Map); ++i) {
        if (t & (1u << i)) {
            if (!out.empty()) out += "|";
            out += typeName(static_cast<VType>(i));
        }
    }
    return out;
}

TypeSet parseTypeName(std::string_view text) {
    TypeSet out = 0;
    std::string s(text);
    bool optional = !s.empty() && s.back() == '?';
    if (optional) s.pop_back();
    for (const auto& part : str::split(s, '|')) {
        std::string p = str::trim(part);
        TypeSet t = 0;
        if (p == "number" || p == "num" || p == "float" || p == "int") t = kTNumber;
        else if (p == "bool" || p == "boolean") t = kTBool;
        else if (p == "string" || p == "str" || p == "text") t = kTString;
        else if (p == "vec" || p == "vector" || p == "vec3") t = kTVec;
        else if (p == "color") t = kTColor;
        else if (p == "entity") t = kTEntity;
        else if (p == "list") t = kTList;
        else if (p == "map") t = kTMap;
        else if (p == "any") t = kTAny;
        else if (p == "none") t = kTNone;
        else if (p == "point") t = kTPoint;
        if (!t) return 0;
        out |= t;
    }
    if (optional) out |= kTNone;
    return out;
}

}  // namespace sky::wander
