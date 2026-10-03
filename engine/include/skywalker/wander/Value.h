#pragma once
// Wander runtime values.
//
// A Value is 24 bytes: a type tag plus a payload. Numbers, booleans, vectors, colors and
// entity ids are stored inline; strings, lists and maps are reference-counted heap objects
// with *value semantics* (copy-on-write): assigning a list copies it logically, mutation
// through one name never shows through another, and values can never form cycles. That
// keeps scripts free of aliasing surprises, makes every value serializable to JSON (vars
// are persisted and visible to agents), and makes memory management leak-proof.
//
// The layout is identical to the C ABI `SkyValue` (skywalker/native/value.h) so AOT-compiled
// behaviors and native modules share values with the VM without conversion.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/math/Math.h"
#include "skywalker/native/value.h"

namespace sky::wander {

using EntityRef = uint64_t;

enum class VType : uint32_t {
    None = SKY_NONE,
    Bool = SKY_BOOL,
    Number = SKY_NUMBER,
    Vec = SKY_VEC,
    Color = SKY_COLOR,
    Entity = SKY_ENTITY,
    String = SKY_STRING,
    List = SKY_LIST,
    Map = SKY_MAP,
};

const char* typeName(VType t);

class Value;

struct Obj {
    int32_t rc = 1;
    VType kind;
    explicit Obj(VType k) : kind(k) {}
};
struct StrObj {
    Obj h{VType::String};
    std::string s;
};
struct ListObj {
    Obj h{VType::List};
    std::vector<Value> items;
};
struct MapObj {
    Obj h{VType::Map};
    std::vector<std::pair<std::string, Value>> entries;  // insertion order (deterministic)

    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    void set(std::string_view key, Value v);
    bool erase(std::string_view key);
};

/// Frees a heap object whose reference count dropped to zero.
void destroyObject(Obj* o);

class Value {
public:
    Value() noexcept { u_.n = 0; }
    ~Value() { release(); }
    Value(const Value& o) noexcept : t_(o.t_), u_(o.u_) { retain(); }
    Value(Value&& o) noexcept : t_(o.t_), u_(o.u_) {
        o.t_ = VType::None;
        o.u_.n = 0;
    }
    Value& operator=(const Value& o) noexcept {
        if (this != &o) {
            if (o.isObject()) ++o.u_.o->rc;
            release();
            t_ = o.t_;
            u_ = o.u_;
        }
        return *this;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            release();
            t_ = o.t_;
            u_ = o.u_;
            o.t_ = VType::None;
            o.u_.n = 0;
        }
        return *this;
    }

    static Value number(double n) {
        Value v;
        v.t_ = VType::Number;
        v.u_.n = n;
        return v;
    }
    static Value boolean(bool b) {
        Value v;
        v.t_ = VType::Bool;
        v.u_.b = b ? 1u : 0u;
        return v;
    }
    static Value vec(Vec3 x) {
        Value v;
        v.t_ = VType::Vec;
        v.u_.f[0] = x.x;
        v.u_.f[1] = x.y;
        v.u_.f[2] = x.z;
        v.u_.f[3] = 0;
        return v;
    }
    static Value color(Vec4 x) {
        Value v;
        v.t_ = VType::Color;
        v.u_.f[0] = x.x;
        v.u_.f[1] = x.y;
        v.u_.f[2] = x.z;
        v.u_.f[3] = x.w;
        return v;
    }
    static Value entity(EntityRef id) {
        Value v;
        v.t_ = VType::Entity;
        v.u_.e = id;
        return v;
    }
    static Value string(std::string s);
    static Value list(std::vector<Value> items = {});
    static Value map();

    /// In-place setters for the VM's hot paths (no temporary Value).
    void setNumber(double n) {
        release();
        t_ = VType::Number;
        u_.n = n;
    }
    void setBool(bool b) {
        release();
        t_ = VType::Bool;
        u_.n = 0;
        u_.b = b ? 1u : 0u;
    }

    VType type() const { return t_; }
    bool isNone() const { return t_ == VType::None; }
    bool isNumber() const { return t_ == VType::Number; }
    bool isBool() const { return t_ == VType::Bool; }
    bool isString() const { return t_ == VType::String; }
    bool isList() const { return t_ == VType::List; }
    bool isMap() const { return t_ == VType::Map; }
    bool isVec() const { return t_ == VType::Vec; }
    bool isColor() const { return t_ == VType::Color; }
    bool isEntity() const { return t_ == VType::Entity; }
    bool isObject() const { return static_cast<uint32_t>(t_) >= SKY_STRING; }

    double num() const { return u_.n; }
    bool b() const { return u_.b != 0; }
    Vec3 v() const { return {u_.f[0], u_.f[1], u_.f[2]}; }
    Vec4 c() const { return {u_.f[0], u_.f[1], u_.f[2], u_.f[3]}; }
    EntityRef e() const { return u_.e; }
    const std::string& str() const { return reinterpret_cast<const StrObj*>(u_.o)->s; }
    const std::vector<Value>& items() const { return reinterpret_cast<const ListObj*>(u_.o)->items; }
    const MapObj& mapObj() const { return *reinterpret_cast<const MapObj*>(u_.o); }

    /// Mutable access with copy-on-write: the payload is cloned first if shared.
    std::vector<Value>& mutItems();
    MapObj& mutMap();

    /// Direct payload access for the VM's fast paths and the C ABI.
    float* floats() { return u_.f; }
    SkyValue* abi() { return reinterpret_cast<SkyValue*>(this); }
    const SkyValue* abi() const { return reinterpret_cast<const SkyValue*>(this); }
    static Value& fromAbi(SkyValue* v) { return *reinterpret_cast<Value*>(v); }
    static const Value& fromAbi(const SkyValue* v) { return *reinterpret_cast<const Value*>(v); }

    /// Truthiness: none, false, 0, "" and empty lists/maps are false. (Entities are
    /// truthy only while they exist; the runtime handles that case.)
    bool truthyData() const;

    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }

private:
    void retain() const {
        if (isObject()) ++u_.o->rc;
    }
    void release() {
        if (isObject() && --u_.o->rc == 0) destroyObject(u_.o);
    }

    VType t_ = VType::None;
    [[maybe_unused]] uint32_t reserved_ = 0;
    union U {
        double n;
        uint32_t b;
        float f[4];
        uint64_t e;
        Obj* o;
    } u_;
};

static_assert(sizeof(Value) == sizeof(SkyValue), "Value must match the SkyValue ABI");
static_assert(sizeof(Obj) == sizeof(SkyObject), "Obj must match the SkyObject ABI");

/// Number formatting used by `log`, `str()` and string interpolation (up to 6
/// significant digits, like C++ streams: 0.1, 2, 3.14159, 1e+06).
std::string formatNumber(double d);

/// Text form of a value. Entity names are resolved by the caller (see Runtime); here an
/// entity prints as "#<id>".
std::string toDisplayString(const Value& v);

/// JSON conversion used for entity vars, event payloads and tool results. Entities become
/// {"$entity": id}; vectors [x, y, z]; colors "#rrggbb(aa)".
Json toJson(const Value& v);
Value fromJson(const Json& j);

// ---------------------------------------------------------------------------
// Static types: a type is the *set* of runtime types an expression may produce.
// Gradual typing: Any (every bit) never causes an error; errors are reported only when
// the sets of what is given and what is accepted are disjoint.
// ---------------------------------------------------------------------------

using TypeSet = uint32_t;
constexpr TypeSet tbit(VType t) { return 1u << static_cast<uint32_t>(t); }
constexpr TypeSet kTNone = tbit(VType::None);
constexpr TypeSet kTBool = tbit(VType::Bool);
constexpr TypeSet kTNumber = tbit(VType::Number);
constexpr TypeSet kTVec = tbit(VType::Vec);
constexpr TypeSet kTColor = tbit(VType::Color);
constexpr TypeSet kTEntity = tbit(VType::Entity);
constexpr TypeSet kTString = tbit(VType::String);
constexpr TypeSet kTList = tbit(VType::List);
constexpr TypeSet kTMap = tbit(VType::Map);
constexpr TypeSet kTAny = 0x1ff;
constexpr TypeSet kTPoint = kTVec | kTEntity;  // a position: a vector or an entity

/// "number", "entity|none", "any", ...
std::string typeSetName(TypeSet t);
/// Parses an annotation: number, bool, string, vec (vector), color, entity, list, map, any,
/// none, unions with '|', and a trailing '?' for "or none". Returns 0 if unknown.
TypeSet parseTypeName(std::string_view text);

}  // namespace sky::wander
