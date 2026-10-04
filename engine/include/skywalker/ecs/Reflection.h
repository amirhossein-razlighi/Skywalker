#pragma once
// Field-level reflection for components.
//
// Every component registers a table of FieldInfo. From that single table the engine
// derives: JSON (de)serialization, validation with precise error messages, the JSON
// Schema advertised to agents, the editor inspector, and Wander property access
// (`self.light.intensity`). Adding a component = write a struct + one table.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky {

/// A reference from a component to another entity (joint targets, camera follow, look-at, bone attachment...).
/// It stores the entity's stable id plus its name: the id keeps the link working through renames, the name keeps
/// files readable and is the fallback for hand-written JSON ("Door") until the scene binds it to an id.
/// Scene::resolve() turns a link into a live entity; Scene::entityToJson() always writes the target's current name.
struct EntityLink {
    uint64_t id = 0;   // 0 = not bound to an id (empty, or a name that is resolved on use)
    std::string name;  // last known name of the target (or the name to look up when id is 0)

    bool empty() const { return id == 0 && name.empty(); }
    bool operator==(const EntityLink& o) const { return id == o.id && name == o.name; }
    bool operator!=(const EntityLink& o) const { return !(*this == o); }
};

/// Json fields hold structured data (lists of layers, clips, curves...) as a sky::Json member;
/// `jsonSchema` (JSON text) describes it to agents and the editor.
/// Vec2/Vec4 store sky::Vec2/Vec4 (Vec4 accepts CSS-style shorthand for paddings/margins).
/// Entity stores an EntityLink; EntityList a std::vector<EntityLink> (e.g. collider sets).
enum class FieldType { Float, Int, Bool, String, Vec3, Color, Enum, Json, Vec2, Vec4, Entity, EntityList };

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
/// Entity links accept "#12", "Door", 12, {"id": 12, "name": "Door"}, {"pid": 3} (prefab-local id inside prefab
/// files), {"$entity": 12} (Wander values) and null / "" (no entity). Written as {"id", "name"} or null.
bool jsonToEntityLink(const Json& j, EntityLink& out);
Json entityLinkToJson(const EntityLink& link);
/// Lists accept an array of links or a comma-separated string of names ("Shoulders, Hands").
bool jsonToEntityLinks(const Json& j, std::vector<EntityLink>& out);
Json entityLinksToJson(const std::vector<EntityLink>& links);

}  // namespace reflect

// Helper for writing tables:  SKY_FIELD(Transform, position, Vec3, "World position")
#define SKY_FIELD(Type, member, ftype, docstr) \
    ::sky::FieldInfo { #member, ::sky::FieldType::ftype, offsetof(Type, member), docstr, {}, -1e30f, 1e30f, {} }
#define SKY_FIELD_RANGE(Type, member, ftype, docstr, lo, hi) \
    ::sky::FieldInfo { #member, ::sky::FieldType::ftype, offsetof(Type, member), docstr, {}, lo, hi, {} }
#define SKY_FIELD_JSON(Type, member, docstr, schemaText) \
    ::sky::FieldInfo { #member, ::sky::FieldType::Json, offsetof(Type, member), docstr, {}, -1e30f, 1e30f, schemaText }
/// An EntityLink (FieldType::Entity) or std::vector<EntityLink> (FieldType::EntityList) member.
#define SKY_FIELD_ENTITY(Type, member, docstr) \
    ::sky::FieldInfo { #member, ::sky::FieldType::Entity, offsetof(Type, member), docstr, {}, -1e30f, 1e30f, {} }
#define SKY_FIELD_ENTITIES(Type, member, docstr) \
    ::sky::FieldInfo { #member, ::sky::FieldType::EntityList, offsetof(Type, member), docstr, {}, -1e30f, 1e30f, {} }
#define SKY_FIELD_ENUM(Type, member, docstr, ...) \
    ::sky::FieldInfo { #member, ::sky::FieldType::Enum, offsetof(Type, member), docstr, {__VA_ARGS__}, -1e30f, 1e30f, {} }

}  // namespace sky
