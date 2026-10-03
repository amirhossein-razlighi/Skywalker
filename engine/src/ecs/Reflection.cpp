#include "skywalker/ecs/Reflection.h"

#include <cmath>
#include <cstdio>

#include "skywalker/core/Strings.h"

namespace sky {

const char* toString(FieldType t) {
    switch (t) {
        case FieldType::Float: return "float";
        case FieldType::Int: return "int";
        case FieldType::Bool: return "bool";
        case FieldType::String: return "string";
        case FieldType::Vec3: return "vec3";
        case FieldType::Color: return "color";
        case FieldType::Enum: return "enum";
        case FieldType::Vec2: return "vec2";
        case FieldType::Vec4: return "vec4";
        case FieldType::Json: return "json";
    }
    return "?";
}

const FieldInfo* TypeInfo::field(std::string_view fieldName) const {
    for (const auto& f : fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

std::vector<std::string> TypeInfo::fieldNames() const {
    std::vector<std::string> names;
    for (const auto& f : fields) names.push_back(f.name);
    return names;
}

namespace reflect {

namespace {

template <typename T>
T& at(void* object, const FieldInfo& f) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + f.offset);
}
template <typename T>
const T& at(const void* object, const FieldInfo& f) {
    return *reinterpret_cast<const T*>(static_cast<const char*>(object) + f.offset);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

float round4(float v) { return std::round(v * 10000.f) / 10000.f; }

Error typeError(std::string_view context, const FieldInfo& f, const Json& got, std::string_view expected) {
    return Error::make("invalid_value",
                       std::string(context) + "." + f.name + " expects " + std::string(expected) + ", got " +
                           Json::typeName(got.type()) + " (" + got.dump() + ")");
}

}  // namespace

bool parseHexColor(std::string_view hex, Vec4& out) {
    if (hex.empty() || hex[0] != '#') return false;
    hex.remove_prefix(1);
    std::vector<int> d;
    for (char c : hex) {
        int v = hexDigit(c);
        if (v < 0) return false;
        d.push_back(v);
    }
    auto byte = [&](size_t i) { return static_cast<float>(d[i] * 16 + d[i + 1]) / 255.f; };
    auto nib = [&](size_t i) { return static_cast<float>(d[i] * 17) / 255.f; };
    switch (d.size()) {
        case 3: out = {nib(0), nib(1), nib(2), 1.f}; return true;
        case 4: out = {nib(0), nib(1), nib(2), nib(3)}; return true;
        case 6: out = {byte(0), byte(2), byte(4), 1.f}; return true;
        case 8: out = {byte(0), byte(2), byte(4), byte(6)}; return true;
        default: return false;
    }
}

std::string toHexColor(Vec4 c) {
    auto b = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    char buf[16];
    if (b(c.w) == 255) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", b(c.x), b(c.y), b(c.z));
    } else {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", b(c.x), b(c.y), b(c.z), b(c.w));
    }
    return buf;
}

bool jsonToVec3(const Json& j, Vec3& out) {
    if (j.isArray() && j.size() == 3 && j[0].isNumber() && j[1].isNumber() && j[2].isNumber()) {
        out = {j[0].asFloat(), j[1].asFloat(), j[2].asFloat()};
        return true;
    }
    if (j.isObject()) {
        const Json* x = j.find("x");
        const Json* y = j.find("y");
        const Json* z = j.find("z");
        if (x && y && z && x->isNumber() && y->isNumber() && z->isNumber()) {
            out = {x->asFloat(), y->asFloat(), z->asFloat()};
            return true;
        }
    }
    if (j.isNumber()) {  // uniform shorthand: "scale": 2
        out = Vec3(j.asFloat());
        return true;
    }
    return false;
}

Json vec3ToJson(Vec3 v) { return Json::array({round4(v.x), round4(v.y), round4(v.z)}); }

bool jsonToColor(const Json& j, Vec4& out) {
    if (j.isString()) return parseHexColor(j.asString(), out);
    if (j.isArray() && (j.size() == 3 || j.size() == 4)) {
        for (size_t i = 0; i < j.size(); ++i) {
            if (!j[i].isNumber()) return false;
        }
        out = {j[0].asFloat(), j[1].asFloat(), j[2].asFloat(), j.size() == 4 ? j[3].asFloat() : 1.f};
        return true;
    }
    return false;
}

Json colorToJson(Vec4 c) {
    // Hex is compact and natural for both people and models, but loses HDR values > 1.
    auto unit = [](float v) { return v >= 0.f && v <= 1.f; };
    if (unit(c.x) && unit(c.y) && unit(c.z) && unit(c.w)) return toHexColor(c);
    return Json::array({round4(c.x), round4(c.y), round4(c.z), round4(c.w)});
}

bool jsonToVec2(const Json& j, Vec2& out) {
    if (j.isArray() && (j.size() == 2 || j.size() == 3) && j[0].isNumber() && j[1].isNumber()) {
        out = {j[0].asFloat(), j[1].asFloat()};
        return true;
    }
    if (j.isObject()) {
        const Json* x = j.find("x");
        const Json* y = j.find("y");
        if (x && y && x->isNumber() && y->isNumber()) {
            out = {x->asFloat(), y->asFloat()};
            return true;
        }
    }
    if (j.isNumber()) {
        out = {j.asFloat(), j.asFloat()};
        return true;
    }
    return false;
}

Json vec2ToJson(Vec2 v) { return Json::array({round4(v.x), round4(v.y)}); }

bool jsonToVec4(const Json& j, Vec4& out) {
    if (j.isNumber()) {
        float v = j.asFloat();
        out = {v, v, v, v};
        return true;
    }
    if (!j.isArray() || j.size() < 1 || j.size() > 4) return false;
    float v[4];
    for (size_t i = 0; i < j.size(); ++i) {
        if (!j[i].isNumber()) return false;
        v[i] = j[i].asFloat();
    }
    switch (j.size()) {  // CSS shorthand: [all], [vertical, horizontal], [top, horizontal, bottom]
        case 1: out = {v[0], v[0], v[0], v[0]}; break;
        case 2: out = {v[0], v[1], v[0], v[1]}; break;
        case 3: out = {v[0], v[1], v[2], v[1]}; break;
        default: out = {v[0], v[1], v[2], v[3]}; break;
    }
    return true;
}

Json vec4ToJson(Vec4 v) { return Json::array({round4(v.x), round4(v.y), round4(v.z), round4(v.w)}); }

Json fieldToJson(const void* object, const FieldInfo& f) {
    switch (f.type) {
        case FieldType::Float: return round4(at<float>(object, f));
        case FieldType::Int: return at<int>(object, f);
        case FieldType::Bool: return at<bool>(object, f);
        case FieldType::String:
        case FieldType::Enum: return at<std::string>(object, f);
        case FieldType::Vec3: return vec3ToJson(at<Vec3>(object, f));
        case FieldType::Color: return colorToJson(at<Vec4>(object, f));
        case FieldType::Vec2: return vec2ToJson(at<Vec2>(object, f));
        case FieldType::Vec4: return vec4ToJson(at<Vec4>(object, f));
        case FieldType::Json: return at<Json>(object, f);
    }
    return {};
}

Status fieldFromJson(void* object, const FieldInfo& f, const Json& v, std::string_view context) {
    switch (f.type) {
        case FieldType::Float: {
            if (!v.isNumber()) return typeError(context, f, v, "a number");
            at<float>(object, f) = std::clamp(v.asFloat(), f.minValue, f.maxValue);
            return {};
        }
        case FieldType::Int: {
            if (!v.isNumber()) return typeError(context, f, v, "an integer");
            at<int>(object, f) = static_cast<int>(v.asInt());
            return {};
        }
        case FieldType::Bool: {
            if (!v.isBool()) return typeError(context, f, v, "true or false");
            at<bool>(object, f) = v.asBool();
            return {};
        }
        case FieldType::String: {
            if (!v.isString()) return typeError(context, f, v, "a string");
            at<std::string>(object, f) = v.asString();
            return {};
        }
        case FieldType::Enum: {
            if (!v.isString()) return typeError(context, f, v, "a string");
            const std::string& s = v.asString();
            bool okValue = false;
            for (const auto& e : f.enumValues) okValue = okValue || e == s;
            if (!okValue) {
                std::string allowed;
                for (const auto& e : f.enumValues) allowed += (allowed.empty() ? "" : ", ") + e;
                std::string guess = str::closest(s, f.enumValues, 3);
                return Error::make("invalid_value",
                                   std::string(context) + "." + f.name + " must be one of: " + allowed,
                                   guess.empty() ? "" : "did you mean \"" + guess + "\"?");
            }
            at<std::string>(object, f) = s;
            return {};
        }
        case FieldType::Vec3: {
            Vec3 out;
            if (!jsonToVec3(v, out)) return typeError(context, f, v, "[x, y, z]");
            at<Vec3>(object, f) = out;
            return {};
        }
        case FieldType::Color: {
            Vec4 out;
            if (!jsonToColor(v, out)) return typeError(context, f, v, "a color like \"#ff8800\" or [r, g, b(, a)]");
            at<Vec4>(object, f) = out;
            return {};
        }
        case FieldType::Vec2: {
            Vec2 out;
            if (!jsonToVec2(v, out)) return typeError(context, f, v, "[x, y]");
            at<Vec2>(object, f) = out;
            return {};
        }
        case FieldType::Vec4: {
            Vec4 out;
            if (!jsonToVec4(v, out)) return typeError(context, f, v, "[a, b, c, d] (or a number / CSS-style shorthand)");
            at<Vec4>(object, f) = out;
            return {};
        }
        case FieldType::Json: {
            // Structured data: the owning system validates the content; null resets it.
            at<Json>(object, f) = v;
            return {};
        }
    }
    return Error::make("internal", "unknown field type");
}

Json toJson(const void* object, const TypeInfo& type) {
    Json out = Json::object();
    for (const auto& f : type.fields) out[f.name] = fieldToJson(object, f);
    return out;
}

Status applyJson(void* object, const TypeInfo& type, const Json& patch) {
    if (!patch.isObject()) {
        return Error::make("invalid_value", type.name + " expects an object of fields, got " + patch.dump());
    }
    // Validate everything first so a failed patch leaves the component untouched.
    for (const auto& [key, value] : patch.members()) {
        const FieldInfo* f = type.field(key);
        if (!f) {
            std::string guess = str::closest(key, type.fieldNames(), 3);
            std::string fields;
            for (const auto& n : type.fieldNames()) fields += (fields.empty() ? "" : ", ") + n;
            return Error::make("unknown_field", type.name + " has no field \"" + key + "\" (fields: " + fields + ")",
                               guess.empty() ? "" : "did you mean \"" + guess + "\"?");
        }
    }
    // Two-phase: apply to a scratch copy of the bytes is not possible for non-trivial
    // types, so validate each value by applying into the real object and roll back on
    // failure using a JSON snapshot.
    Json before = toJson(object, type);
    for (const auto& [key, value] : patch.members()) {
        Status s = fieldFromJson(object, *type.field(key), value, type.name);
        if (!s) {
            for (const auto& [k, v] : before.members()) (void)fieldFromJson(object, *type.field(k), v, type.name);
            return s;
        }
    }
    return {};
}

Json schema(const TypeInfo& type) {
    Json props = Json::object();
    for (const auto& f : type.fields) {
        Json p = Json::object();
        switch (f.type) {
            case FieldType::Float:
                p["type"] = "number";
                if (f.minValue > -1e29f) p["minimum"] = f.minValue;
                if (f.maxValue < 1e29f) p["maximum"] = f.maxValue;
                break;
            case FieldType::Int: p["type"] = "integer"; break;
            case FieldType::Bool: p["type"] = "boolean"; break;
            case FieldType::String: p["type"] = "string"; break;
            case FieldType::Enum: {
                p["type"] = "string";
                Json values = Json::array();
                for (const auto& e : f.enumValues) values.push(e);
                p["enum"] = values;
                break;
            }
            case FieldType::Vec3:
                p["type"] = "array";
                p["items"] = Json::object({{"type", "number"}});
                p["minItems"] = 3;
                p["maxItems"] = 3;
                break;
            case FieldType::Color:
                p["description"] = "hex string \"#rrggbb[aa]\" or [r,g,b(,a)] in 0..1";
                break;
            case FieldType::Vec2:
                p["type"] = "array";
                p["items"] = Json::object({{"type", "number"}});
                p["minItems"] = 2;
                p["maxItems"] = 2;
                break;
            case FieldType::Vec4:
                p["type"] = "array";
                p["items"] = Json::object({{"type", "number"}});
                p["minItems"] = 4;
                p["maxItems"] = 4;
                break;
            case FieldType::Json:
                p["type"] = Json::array({"object", "array", "string", "null"});
                p["x-skywalker-json"] = true;
                break;
        }
        if (!f.doc.empty()) {
            std::string d = f.doc;
            if (const Json* existing = p.find("description")) d += " — " + existing->asString();
            p["description"] = d;
        }
        props[f.name] = p;
    }
    return Json::object({{"type", "object"}, {"description", type.doc}, {"properties", props},
                         {"additionalProperties", false}});
}

}  // namespace reflect
}  // namespace sky
