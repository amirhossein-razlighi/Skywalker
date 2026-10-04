#pragma once
// Field-level reflection for components.
//
// Every component registers a table of FieldInfo. From that single table the engine
// derives: JSON (de)serialization, validation with precise error messages, the JSON
// Schema advertised to agents, the editor inspector, and Wander property access
// (`self.light.intensity`). Adding a component = write a struct + one table.

#include <cstddef>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky {

/// Json fields hold structured data (lists of layers, clips, curves...) as a sky::Json member;
/// `jsonSchema` (JSON text) describes it to agents and the editor.
/// Vec2/Vec4 store sky::Vec2/Vec4 (Vec4 accepts CSS-style shorthand for paddings/margins).
enum class FieldType { Float, Int, Bool, String, Vec3, Color, Enum, Json, Vec2, Vec4 };

const char* toString(FieldType t);

struct FieldInfo {
    std::string name;
    FieldType type;
    size_t offset;
    std::string doc;
    std::vector<std::string> enumValues;  // FieldType::Enum (stored as std::string)
    float minValue = -1e30f;
    float maxValue = 1e30f;
    std::string jsonSchema;  // FieldType::Json: JSON Schema of the value (as JSON text)
};

struct TypeInfo {
    std::string name;  // json/wander key, e.g. "transform"
    std::string doc;
    std::vector<FieldInfo> fields;

    const FieldInfo* field(std::string_view fieldName) const;
    std::vector<std::string> fieldNames() const;
};

namespace reflect {

Json fieldToJson(const void* object, const FieldInfo& field);
/// Validates and writes; on error `object` is unchanged and the error names the field.
Status fieldFromJson(void* object, const FieldInfo& field, const Json& value, std::string_view context);

Json toJson(const void* object, const TypeInfo& type);
/// Partial update: only keys present in `patch` are written. Unknown keys are errors
/// (with did-you-mean hints) so agents learn about typos instead of silently failing.
Status applyJson(void* object, const TypeInfo& type, const Json& patch);

/// JSON Schema for the type (used in tool descriptions and `component_schema`).
Json schema(const TypeInfo& type);

/// Parses "#rgb", "#rrggbb", "#rrggbbaa" into linear-ish 0..1 color.
bool parseHexColor(std::string_view hex, Vec4& out);
std::string toHexColor(Vec4 c);

/// Accepts [x,y,z] or {"x":..,"y":..,"z":..}.
bool jsonToVec3(const Json& j, Vec3& out);
Json vec3ToJson(Vec3 v);
/// Accepts [r,g,b], [r,g,b,a] or a hex string.
bool jsonToColor(const Json& j, Vec4& out);
Json colorToJson(Vec4 c);
/// Accepts [x, y] (or [x, y, z], z ignored) or a number (uniform).
bool jsonToVec2(const Json& j, Vec2& out);
Json vec2ToJson(Vec2 v);
/// Accepts [a, b, c, d], a number (uniform), or CSS shorthand [v, h] / [top, h, bottom].
bool jsonToVec4(const Json& j, Vec4& out);
Json vec4ToJson(Vec4 v);

}  // namespace reflect

// Helper for writing tables:  SKY_FIELD(Transform, position, Vec3, "World position")
#define SKY_FIELD(Type, member, ftype, docstr) \
    ::sky::FieldInfo { #member, ::sky::FieldType::ftype, offsetof(Type, member), docstr, {}, -1e30f, 1e30f, {} }
#define SKY_FIELD_RANGE(Type, member, ftype, docstr, lo, hi) \
    ::sky::FieldInfo { #member, ::sky::FieldType::ftype, offsetof(Type, member), docstr, {}, lo, hi, {} }
#define SKY_FIELD_JSON(Type, member, docstr, schemaText) \
    ::sky::FieldInfo { #member, ::sky::FieldType::Json, offsetof(Type, member), docstr, {}, -1e30f, 1e30f, schemaText }
#define SKY_FIELD_ENUM(Type, member, docstr, ...) \
    ::sky::FieldInfo { #member, ::sky::FieldType::Enum, offsetof(Type, member), docstr, {__VA_ARGS__}, -1e30f, 1e30f, {} }

}  // namespace sky
