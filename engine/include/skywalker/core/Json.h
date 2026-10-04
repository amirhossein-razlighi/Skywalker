#pragma once
// sky::Json — the engine's lingua franca.
//
// Everything an agent touches (tool arguments, component data, scenes, history,
// events) is expressed as Json. Objects keep insertion order so that output is
// deterministic: the same scene always serializes to the same bytes, which makes
// diffs, caching and agent reasoning far more reliable than hash-ordered maps.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky {

class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}                                                     // NOLINT
    Json(bool b) : type_(Type::Bool), bool_(b) {}                              // NOLINT
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T v) : type_(Type::Number), number_(static_cast<double>(v)) {}         // NOLINT
    Json(float v) : type_(Type::Number), number_(v) {}                         // NOLINT
    Json(double v) : type_(Type::Number), number_(v) {}                        // NOLINT
    Json(const char* s) : type_(Type::String), string_(s) {}                   // NOLINT
    Json(std::string s) : type_(Type::String), string_(std::move(s)) {}        // NOLINT
    Json(std::string_view s) : type_(Type::String), string_(s) {}              // NOLINT
    Json(Array a) : type_(Type::Array), array_(std::move(a)) {}                // NOLINT
    Json(Object o) : type_(Type::Object), object_(std::move(o)) {}             // NOLINT

    static Json array(std::initializer_list<Json> items = {}) { return Json(Array(items)); }
    static Json object(std::initializer_list<Member> members = {}) { return Json(Object(members)); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    static const char* typeName(Type t);

    // Lenient accessors: return `fallback` when the type does not match.
    bool asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double asNumber(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
    float asFloat(float fallback = 0.f) const { return isNumber() ? static_cast<float>(number_) : fallback; }
    int64_t asInt(int64_t fallback = 0) const { return isNumber() ? static_cast<int64_t>(number_) : fallback; }
    const std::string& asString() const;  // empty string if not a string
    std::string asString(std::string_view fallback) const { return isString() ? string_ : std::string(fallback); }

    // Arrays
    const Array& elements() const;  // empty if not an array
    Array& elements();              // converts null -> array
    void push(Json value);
    size_t size() const;
    const Json& operator[](size_t index) const;
    // Non-const and int overloads: `j[size_t{0}]` / `j[0]` on a mutable Json must not be
    // ambiguous with the object-key overload (GCC treats a zero integer as a null pointer).
    const Json& operator[](size_t index) { return static_cast<const Json&>(*this)[index]; }
    const Json& operator[](int index) const { return (*this)[static_cast<size_t>(index)]; }
    const Json& operator[](int index) { return static_cast<const Json&>(*this)[static_cast<size_t>(index)]; }

    // Objects
    const Object& members() const;  // empty if not an object
    Object& members();              // converts null -> object
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    const Json& get(std::string_view key) const;  // null Json if missing
    Json& operator[](std::string_view key);        // inserts null if missing
    const Json& operator[](std::string_view key) const { return get(key); }
    void set(std::string_view key, Json value) { (*this)[key] = std::move(value); }
    bool erase(std::string_view key);

    /// Deep-merge `patch` into this object (RFC 7386 style: null deletes a key).
    void mergePatch(const Json& patch);

    std::string dump(int indent = -1) const;
    static Result<Json> parse(std::string_view text);

    bool operator==(const Json& other) const;
    bool operator!=(const Json& other) const { return !(*this == other); }

    static const Json& null();

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

}  // namespace sky
