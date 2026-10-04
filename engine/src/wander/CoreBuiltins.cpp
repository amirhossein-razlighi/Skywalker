// The core Wander library: math, vectors, colors, randomness, entities, input, text,
// lists, maps and the statement helpers (move, rotate, emit, log, ...).
// Every entry is documented here; `wander_reference` and docs/WANDER.md are generated
// from these declarations.

#include <algorithm>
#include <cmath>
#include <numeric>

#include "RuntimeInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/wander/Builtins.h"

namespace sky::wander {

namespace {

using P = BuiltinParam;

void fn(BuiltinRegistry& r, const char* name, std::vector<BuiltinParam> params, TypeSet ret, const char* category,
        const char* doc, const char* example, BuiltinImpl impl, bool pure = false, bool variadic = false) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.variadic = variadic;
    d.returns = ret;
    d.category = category;
    d.doc = doc;
    d.example = example;
    d.fn = impl;
    d.pure = pure;
    d.owner = "core";
    r.add(std::move(d));
}

void method(BuiltinRegistry& r, TypeSet receiver, const char* name, std::vector<BuiltinParam> params, TypeSet ret,
            const char* category, const char* doc, const char* example, BuiltinImpl impl, bool mutates = false) {
    BuiltinDef d;
    d.name = name;
    d.receiver = receiver;
    d.params = std::move(params);
    d.returns = ret;
    d.category = category;
    d.doc = doc;
    d.example = example;
    d.fn = impl;
    d.pure = !mutates;
    d.mutates = mutates;
    d.owner = "core";
    r.add(std::move(d));
}

void hidden(BuiltinRegistry& r, const char* name, std::vector<BuiltinParam> params, BuiltinImpl impl) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = kTNone;
    d.category = "statement";
    d.fn = impl;
    d.hidden = true;
    d.owner = "core";
    r.add(std::move(d));
}

Value num(double d) { return Value::number(d); }

// The transform of entity argument i (added if missing), with a single record lookup.
Transform& transformArg(CallContext& c, int i) {
    const Value& v = c.arg(i);
    EntityRecord* rec = v.isEntity() ? c.scene().record(v.e()) : nullptr;
    if (!rec) (void)c.entity(i);  // reports the precise error
    if (Transform* t = c.scene().registry().get<Transform>(rec->handle)) return *t;
    return c.scene().add<Transform>(rec->id);
}

Vec3 entityDir(CallContext& c, int i, Vec3 local) {
    EntityRef id = c.entity(i);
    return normalize(c.scene().worldMatrix(id).transformDir(local));
}

double hash1(double x) {
    double s = std::sin(x * 127.1 + 311.7) * 43758.5453123;
    return s - std::floor(s);
}
double valueNoise(double x, double y) {
    double ix = std::floor(x), iy = std::floor(y);
    double fx = x - ix, fy = y - iy;
    auto h = [](double a, double b) { return hash1(a * 57.0 + b * 131.0); };
    double ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    double a = h(ix, iy), b = h(ix + 1, iy), cc = h(ix, iy + 1), d = h(ix + 1, iy + 1);
    return (a + (b - a) * ux + (cc - a) * uy + (a - b - cc + d) * ux * uy) * 2 - 1;
}

bool lessValue(CallContext& c, const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) return a.num() < b.num();
    if (a.isString() && b.isString()) return a.str() < b.str();
    c.fail(std::string("sort() can only order numbers or strings (found ") + typeName(a.type()) + " and " +
           typeName(b.type()) + ")");
}

std::string toLower(std::string s) { return str::lower(s); }
std::string toUpper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

int64_t wholeIndex(CallContext& c, int i, size_t size, bool allowEnd) {
    double d = c.number(i);
    if (d != std::floor(d)) c.fail(c.def().name + "(): the index must be a whole number, got " + formatNumber(d));
    auto k = static_cast<int64_t>(d);
    if (k < 0) k += static_cast<int64_t>(size);
    int64_t limit = static_cast<int64_t>(size) + (allowEnd ? 1 : 0);
    if (k < 0 || k >= limit) {
        c.fail(c.def().name + "(): index " + formatNumber(d) + " is out of range (the list has " + std::to_string(size) +
               (size == 1 ? " item)" : " items)"));
    }
    return k;
}

std::pair<size_t, size_t> sliceRange(CallContext& c, size_t size) {
    auto clampIndex = [&](double d) {
        auto k = static_cast<int64_t>(std::floor(d));
        if (k < 0) k += static_cast<int64_t>(size);
        return static_cast<size_t>(std::clamp<int64_t>(k, 0, static_cast<int64_t>(size)));
    };
    size_t from = c.argc() > 1 ? clampIndex(c.number(1)) : 0;
    size_t to = c.argc() > 2 ? clampIndex(c.number(2)) : size;
    if (to < from) to = from;
    return {from, to};
}

}  // namespace

void registerCoreBuiltins(BuiltinRegistry& r) {
    const TypeSet N = kTNumber, V = kTVec, S = kTString, E = kTEntity, L = kTList, M = kTMap, A = kTAny, B = kTBool,
                  C = kTColor;

    // --- math -----------------------------------------------------------------------
    fn(r, "sin", {{"radians", N}}, N, "math", "Sine of an angle in radians.", "sin(time * 2)",
       [](CallContext& c) { return num(std::sin(c.number(0))); }, true);
    fn(r, "cos", {{"radians", N}}, N, "math", "Cosine of an angle in radians.", "cos(time)",
       [](CallContext& c) { return num(std::cos(c.number(0))); }, true);
    fn(r, "tan", {{"radians", N}}, N, "math", "Tangent of an angle in radians.", "tan(0.5)",
       [](CallContext& c) { return num(std::tan(c.number(0))); }, true);
    fn(r, "asin", {{"x", N}}, N, "math", "Arc sine in radians (x is clamped to -1..1).", "asin(0.5)",
       [](CallContext& c) { return num(std::asin(std::clamp(c.number(0), -1.0, 1.0))); }, true);
    fn(r, "acos", {{"x", N}}, N, "math", "Arc cosine in radians (x is clamped to -1..1).", "acos(dot(a, b))",
       [](CallContext& c) { return num(std::acos(std::clamp(c.number(0), -1.0, 1.0))); }, true);
    fn(r, "atan", {{"x", N}}, N, "math", "Arc tangent in radians.", "atan(1)",
       [](CallContext& c) { return num(std::atan(c.number(0))); }, true);
    fn(r, "atan2", {{"y", N}, {"x", N}}, N, "math", "Angle of the point (x, y) in radians, -pi..pi.",
       "deg(atan2(d.x, d.z))", [](CallContext& c) { return num(std::atan2(c.number(0), c.number(1))); }, true);
    fn(r, "abs", {{"x", N}}, N, "math", "Absolute value.", "abs(-3)", [](CallContext& c) { return num(std::fabs(c.number(0))); },
       true);
    fn(r, "sqrt", {{"x", N}}, N, "math", "Square root (0 for negative input).", "sqrt(2)",
       [](CallContext& c) { return num(std::sqrt(std::max(0.0, c.number(0)))); }, true);
    fn(r, "pow", {{"base", N}, {"exponent", N}}, N, "math", "base raised to exponent.", "pow(2, 10)",
       [](CallContext& c) { return num(std::pow(c.number(0), c.number(1))); }, true);
    fn(r, "exp", {{"x", N}}, N, "math", "e raised to x.", "exp(-dt * 4)", [](CallContext& c) { return num(std::exp(c.number(0))); },
       true);
    fn(r, "ln", {{"x", N}}, N, "math", "Natural logarithm (x > 0).", "ln(10)", [](CallContext& c) {
        double x = c.number(0);
        if (x <= 0) c.fail("ln(): x must be greater than 0, got " + formatNumber(x));
        return num(std::log(x));
    }, true);
    fn(r, "floor", {{"x", N}}, N, "math", "Largest whole number <= x.", "floor(2.7)",
       [](CallContext& c) { return num(std::floor(c.number(0))); }, true);
    fn(r, "ceil", {{"x", N}}, N, "math", "Smallest whole number >= x.", "ceil(2.1)",
       [](CallContext& c) { return num(std::ceil(c.number(0))); }, true);
    fn(r, "round", {{"x", N}, {"digits", N, true}}, N, "math", "Rounds to the nearest whole number, or to `digits` decimals.",
       "round(3.14159, 2)", [](CallContext& c) {
           double x = c.number(0);
           if (c.argc() < 2) return num(std::round(x));
           double f = std::pow(10.0, std::clamp(std::round(c.number(1)), 0.0, 12.0));
           return num(std::round(x * f) / f);
       }, true);
    fn(r, "sign", {{"x", N}}, N, "math", "-1, 0 or 1.", "sign(velocity.x)", [](CallContext& c) {
        double x = c.number(0);
        return num(x > 0 ? 1 : x < 0 ? -1 : 0);
    }, true);
    fn(r, "min", {{"a", N | L}, {"b", N, true}}, N, "math", "Smaller of numbers, or the smallest number in a list.",
       "min(hp, 10)   min([3, 1, 2])", [](CallContext& c) {
           if (c.argc() == 1) {
               const auto& l = c.list(0);
               if (l.empty()) c.fail("min(): the list is empty");
               double m = 1e308;
               for (const auto& v : l) {
                   if (!v.isNumber()) c.fail("min(): the list must contain numbers only");
                   m = std::min(m, v.num());
               }
               return num(m);
           }
           double m = c.number(0);
           for (int i = 1; i < c.argc(); ++i) m = std::min(m, c.number(i));
           return num(m);
       }, true, true);
    fn(r, "max", {{"a", N | L}, {"b", N, true}}, N, "math", "Larger of numbers, or the largest number in a list.",
       "max(0, hp - damage)", [](CallContext& c) {
           if (c.argc() == 1) {
               const auto& l = c.list(0);
               if (l.empty()) c.fail("max(): the list is empty");
               double m = -1e308;
               for (const auto& v : l) {
                   if (!v.isNumber()) c.fail("max(): the list must contain numbers only");
                   m = std::max(m, v.num());
               }
               return num(m);
           }
           double m = c.number(0);
           for (int i = 1; i < c.argc(); ++i) m = std::max(m, c.number(i));
           return num(m);
       }, true, true);
    fn(r, "clamp", {{"x", N}, {"lo", N}, {"hi", N}}, N, "math", "x limited to the range lo..hi.", "clamp(speed, 0, 10)",
       [](CallContext& c) {
           double lo = c.number(1), hi = c.number(2);
           return num(std::clamp(c.number(0), std::min(lo, hi), std::max(lo, hi)));
       }, true);
    fn(r, "lerp", {{"a", N | V | C}, {"b", N | V | C}, {"t", N}}, N | V | C, "math",
       "Linear blend from a (t=0) to b (t=1); numbers, vectors or colors.", "lerp(self.position, target, 0.1)",
       [](CallContext& c) {
           double t = c.number(2);
           const Value& a = c.arg(0);
           const Value& b = c.arg(1);
           if (a.isColor() || b.isColor()) {
               Vec4 x = c.color(0), y = c.color(1);
               auto f = static_cast<float>(t);
               return Value::color({x.x + (y.x - x.x) * f, x.y + (y.y - x.y) * f, x.z + (y.z - x.z) * f, x.w + (y.w - x.w) * f});
           }
           if (a.isVec() || b.isVec() || a.isEntity() || b.isEntity()) return Value::vec(lerp(c.point(0), c.point(1), static_cast<float>(t)));
           double x = c.number(0), y = c.number(1);
           return num(x + (y - x) * t);
       }, true);
    fn(r, "inverse_lerp", {{"a", N}, {"b", N}, {"x", N}}, N, "math", "Where x lies between a (0) and b (1).",
       "inverse_lerp(0, 100, hp)", [](CallContext& c) {
           double a = c.number(0), b = c.number(1);
           return num(b == a ? 0.0 : (c.number(2) - a) / (b - a));
       }, true);
    fn(r, "smoothstep", {{"edge0", N}, {"edge1", N}, {"x", N}}, N, "math", "Smooth 0..1 ramp between two edges.",
       "smoothstep(0, 1, t)", [](CallContext& c) {
           double e0 = c.number(0), e1 = c.number(1);
           double t = e1 == e0 ? (c.number(2) >= e1 ? 1.0 : 0.0) : std::clamp((c.number(2) - e0) / (e1 - e0), 0.0, 1.0);
           return num(t * t * (3 - 2 * t));
       }, true);
    fn(r, "approach", {{"current", N}, {"target", N}, {"max_step", N}}, N, "math",
       "Moves current toward target by at most max_step (never overshoots).", "speed = approach(speed, 10, 4 * dt)",
       [](CallContext& c) {
           double cur = c.number(0), tgt = c.number(1), step = std::fabs(c.number(2));
           if (std::fabs(tgt - cur) <= step) return num(tgt);
           return num(cur + (tgt > cur ? step : -step));
       }, true);
    fn(r, "deg", {{"radians", N}}, N, "math", "Radians to degrees.", "deg(atan2(d.x, d.z))",
       [](CallContext& c) { return num(c.number(0) * 180.0 / 3.14159265358979323846); }, true);
    fn(r, "rad", {{"degrees", N}}, N, "math", "Degrees to radians.", "sin(rad(45))",
       [](CallContext& c) { return num(c.number(0) * 3.14159265358979323846 / 180.0); }, true);
    fn(r, "noise", {{"x", N}, {"y", N, true}}, N, "math", "Smooth deterministic value noise in -1..1 (1D or 2D).",
       "move self by (0, noise(time) * 0.01, 0)",
       [](CallContext& c) { return num(valueNoise(c.number(0), c.argc() > 1 ? c.number(1) : 0.0)); }, true);

    // --- vectors & colors ---------------------------------------------------------------
    fn(r, "vec", {{"x", N}, {"y", N}, {"z", N}}, V, "vector", "Makes a vector (same as (x, y, z)).", "vec(0, 1, 0)",
       [](CallContext& c) {
           return Value::vec({static_cast<float>(c.number(0)), static_cast<float>(c.number(1)), static_cast<float>(c.number(2))});
       }, true);
    fn(r, "length", {{"v", V}}, N, "vector", "Length (magnitude) of a vector.", "length(velocity)",
       [](CallContext& c) { return num(length(c.vec(0))); }, true);
    fn(r, "distance", {{"a", kTPoint}, {"b", kTPoint}}, N, "vector",
       "Distance between two points; entities count as their world position.", "distance(self, player) < 3",
       [](CallContext& c) { return num(distance(c.point(0), c.point(1))); });
    fn(r, "direction", {{"from", kTPoint}, {"to", kTPoint}}, V, "vector", "Unit vector pointing from one point to another.",
       "move self by direction(self, goal) * speed * dt",
       [](CallContext& c) { return Value::vec(normalize(c.point(1) - c.point(0))); });
    fn(r, "normalize", {{"v", V}}, V, "vector", "Vector scaled to length 1 (zero stays zero).", "normalize(v)",
       [](CallContext& c) { return Value::vec(normalize(c.vec(0))); }, true);
    fn(r, "dot", {{"a", V}, {"b", V}}, N, "vector", "Dot product.", "dot(forward(self), direction(self, player)) > 0.7",
       [](CallContext& c) { return num(dot(c.vec(0), c.vec(1))); }, true);
    fn(r, "cross", {{"a", V}, {"b", V}}, V, "vector", "Cross product.", "cross((0, 1, 0), forward(self))",
       [](CallContext& c) { return Value::vec(cross(c.vec(0), c.vec(1))); }, true);
    fn(r, "forward", {{"e", E}}, V, "vector", "World direction the entity faces (its -Z axis).",
       "move self by forward(self) * speed * dt", [](CallContext& c) { return Value::vec(entityDir(c, 0, {0, 0, -1})); });
    fn(r, "right", {{"e", E}}, V, "vector", "World direction of the entity's right side (+X).", "right(self)",
       [](CallContext& c) { return Value::vec(entityDir(c, 0, {1, 0, 0})); });
    fn(r, "up", {{"e", E}}, V, "vector", "World direction of the entity's up axis (+Y).", "up(self)",
       [](CallContext& c) { return Value::vec(entityDir(c, 0, {0, 1, 0})); });
    fn(r, "world_position", {{"e", E}}, V, "vector", "World position (self.position is relative to the parent).",
       "world_position(self)", [](CallContext& c) { return Value::vec(worldPosition(c.scene(), c.entity(0))); });
    fn(r, "color", {{"r", N}, {"g", N}, {"b", N}, {"a", N, true}}, C, "color", "Makes a color from 0..1 channels.",
       "color(1, 0.5, 0)", [](CallContext& c) {
           return Value::color({static_cast<float>(c.number(0)), static_cast<float>(c.number(1)),
                                static_cast<float>(c.number(2)), c.argc() > 3 ? static_cast<float>(c.number(3)) : 1.f});
       }, true);
    fn(r, "hsv", {{"h", N}, {"s", N}, {"v", N}}, C, "color", "Color from hue (0..1, wraps), saturation and value.",
       "self.color = hsv(time * 0.1, 0.8, 1)", [](CallContext& c) {
           double h = c.number(0), s = std::clamp(c.number(1), 0.0, 1.0), v = std::clamp(c.number(2), 0.0, 1.0);
           h = (h - std::floor(h)) * 6.0;
           int i = static_cast<int>(h) % 6;
           double f = h - std::floor(h);
           double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
           double rr[6] = {v, q, p, p, t, v}, gg[6] = {t, v, v, q, p, p}, bb[6] = {p, p, t, v, v, q};
           return Value::color({static_cast<float>(rr[i]), static_cast<float>(gg[i]), static_cast<float>(bb[i]), 1.f});
       }, true);

    // --- random (seeded: runs replay exactly) ------------------------------------------------
    fn(r, "random", {{"lo_or_hi", N, true}, {"hi", N, true}}, N, "random",
       "random() is 0..1; random(hi) is 0..hi; random(lo, hi) is lo..hi. Seeded by the scene: replays exactly.",
       "random(-1, 1)", [](CallContext& c) {
           double x = c.rng().nextFloat();
           if (c.argc() == 0) return num(x);
           if (c.argc() == 1) return num(x * c.number(0));
           return num(c.number(0) + (c.number(1) - c.number(0)) * x);
       });
    fn(r, "random_int", {{"lo", N}, {"hi", N}}, N, "random", "A whole number from lo to hi (both included).",
       "random_int(1, 6)", [](CallContext& c) {
           double lo = std::ceil(std::min(c.number(0), c.number(1))), hi = std::floor(std::max(c.number(0), c.number(1)));
           if (hi < lo) return num(lo);
           double span = hi - lo + 1;
           return num(lo + std::min(span - 1, std::floor(c.rng().nextFloat() * span)));
       });
    fn(r, "chance", {{"p", N}}, B, "random", "true with probability p (0..1).", "if chance(0.1) then ... end",
       [](CallContext& c) { return Value::boolean(c.rng().nextFloat() < c.number(0)); });
    fn(r, "pick", {{"items", L}}, A, "random", "A random element of a list (none if empty).", "pick([\"red\", \"blue\"])",
       [](CallContext& c) {
           const auto& l = c.list(0);
           if (l.empty()) return Value();
           auto i = std::min(l.size() - 1, static_cast<size_t>(c.rng().nextFloat() * static_cast<float>(l.size())));
           return l[i];
       });
    fn(r, "shuffle", {{"items", L}}, L, "random", "A shuffled copy of a list.", "for e in shuffle(find_all(\"spawn\")) ... end",
       [](CallContext& c) {
           std::vector<Value> l = c.list(0);
           c.charge(static_cast<int64_t>(l.size()));
           for (size_t i = l.size(); i > 1; --i) {
               auto j = std::min(i - 1, static_cast<size_t>(c.rng().nextFloat() * static_cast<float>(i)));
               std::swap(l[i - 1], l[j]);
           }
           return Value::list(std::move(l));
       });

    // --- entities & scene ---------------------------------------------------------------------
    fn(r, "find", {{"name", S}}, E | kTNone, "scene",
       "Entity by name (or \"#id\"); none if missing. \"%Name\" finds the entity marked unique (entity_update "
       "unique=true) inside this prefab instance, else in the scene, so each copy of a prefab finds its own part.",
       "let muzzle = find(\"%Muzzle\")", [](CallContext& c) {
           const std::string& n = c.string(0);
           EntityId id = !n.empty() && n[0] == '%' ? c.scene().findUnique(n, c.self()) : c.scene().find(n);
           return id ? Value::entity(id) : Value();
       });
    fn(r, "find_all", {{"tag", S}}, L, "scene", "Active entities with a tag, in scene order.",
       "for e in find_all(\"enemy\") ... end", [](CallContext& c) {
           auto ids = c.scene().findTagged(c.string(0));
           c.charge(static_cast<int64_t>(ids.size()));
           std::vector<Value> out;
           for (EntityId id : ids) {
               if (c.scene().isActive(id)) out.push_back(Value::entity(id));
           }
           return Value::list(std::move(out));
       });
    fn(r, "nearest", {{"tag", S}, {"max_distance", N, true}}, E | kTNone, "scene",
       "Closest other active entity with a tag (within max_distance); none if there is none.",
       "let p = nearest(\"player\", 10)", [](CallContext& c) {
           Vec3 me = worldPosition(c.scene(), c.self());
           double limit = c.argc() > 1 ? c.number(1) : 1e30;
           EntityId best = kNoEntity;
           double bestD = limit;
           auto ids = c.scene().findTagged(c.string(0));
           c.charge(static_cast<int64_t>(ids.size()));
           for (EntityId id : ids) {
               if (id == c.self() || !c.scene().isActive(id)) continue;
               double d = distance(me, worldPosition(c.scene(), id));
               if (d < bestD || (best == kNoEntity && d <= bestD)) {
                   bestD = d;
                   best = id;
               }
           }
           return best ? Value::entity(best) : Value();
       });
    fn(r, "count", {{"tag", S}}, N, "scene", "How many entities have a tag.", "if count(\"coin\") == 0 then ... end",
       [](CallContext& c) { return num(static_cast<double>(c.scene().findTagged(c.string(0)).size())); });
    fn(r, "tagged", {{"e", A}, {"tag", S}}, B, "scene", "Whether an entity has a tag (false for none).",
       "if tagged(other, \"player\") then ... end", [](CallContext& c) {
           const Value& v = c.arg(0);
           if (!v.isEntity() || !c.scene().exists(v.e())) return Value::boolean(false);
           const auto& tags = c.scene().record(v.e())->tags;
           return Value::boolean(std::find(tags.begin(), tags.end(), c.string(1)) != tags.end());
       });
    fn(r, "add_tag", {{"e", E}, {"tag", S}}, kTNone, "scene", "Adds a tag to an entity.", "add_tag(self, \"carried\")",
       [](CallContext& c) {
           EntityRecord* rec = c.scene().record(c.entity(0));
           const std::string& t = c.string(1);
           if (std::find(rec->tags.begin(), rec->tags.end(), t) == rec->tags.end()) rec->tags.push_back(t);
           c.scene().markDirty();
           return Value();
       });
    fn(r, "remove_tag", {{"e", E}, {"tag", S}}, kTNone, "scene", "Removes a tag from an entity.", "remove_tag(self, \"carried\")",
       [](CallContext& c) {
           EntityRecord* rec = c.scene().record(c.entity(0));
           std::erase(rec->tags, c.string(1));
           c.scene().markDirty();
           return Value();
       });
    fn(r, "exists", {{"e", A}}, B, "scene", "Whether a value is an entity that still exists.", "if exists(target) then ... end",
       [](CallContext& c) { return Value::boolean(c.arg(0).isEntity() && c.scene().exists(c.arg(0).e())); });
    fn(r, "children", {{"e", E}}, L, "scene", "Direct children of an entity, in order.", "for child in children(self) ... end",
       [](CallContext& c) {
           std::vector<Value> out;
           for (EntityId id : c.scene().children(c.entity(0))) out.push_back(Value::entity(id));
           return Value::list(std::move(out));
       });
    fn(r, "has", {{"e", E}, {"component", S}}, B, "scene", "Whether the entity has a component (\"light\", \"particles\", ...).",
       "if has(self, \"light\") then self.light.intensity = 2 end", [](CallContext& c) {
           const ComponentKind* k = c.scene().componentKind(c.string(1));
           if (!k) {
               std::string guess = str::closest(c.string(1), c.scene().componentNames());
               c.fail("has(): unknown component '" + c.string(1) + "'" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
           }
           return Value::boolean(k->has(c.scene(), c.entity(0)));
       });
    fn(r, "spawn", {{"mesh", S}, {"position", kTPoint, true}, {"name", A, true}}, E, "scene",
       "Creates an entity: a primitive mesh (\"cube\", \"sphere\"...), \"asset:models/x.glb\", or a whole prefab "
       "\"prefab:prefabs/coin.prefab.json\" (with its children and behaviors). Its behaviors start next tick. "
       "Limits: 256 spawns per tick, 20000 entities.",
       "let coin = spawn(\"prefab:prefabs/coin.prefab.json\", self.position + (0, 1, 0))", [](CallContext& c) {
           ExecState& st = CallContextAccess(c);
           if (++st.impl.spawnedThisTick > 256) c.fail("too many spawns in one tick (limit 256)");
           if (c.scene().size() >= 20000) c.fail("entity limit reached (20000)");
           std::string mesh = c.string(0);
           Vec3 pos = c.argc() > 1 ? c.point(1) : Vec3{0, 0, 0};
           if (str::startsWith(mesh, "prefab:")) {
               if (!c.runtime().spawnPrefab) c.fail("prefabs are not available in this context");
               auto id = c.runtime().spawnPrefab(mesh.substr(7), pos, c.argc() > 2 ? c.display(c.arg(2)) : "");
               if (!id) c.fail(id.error().message);
               return Value::entity(*id);
           }
           const auto& prims = MeshRenderer::primitives();
           if (std::find(prims.begin(), prims.end(), mesh) == prims.end() && !str::startsWith(mesh, "asset:")) {
               std::string guess = str::closest(mesh, prims, 3);
               c.fail("unknown mesh '" + mesh + "'" + (guess.empty() ? " (primitives, \"asset:path\" or \"prefab:path\")"
                                                                       : " (did you mean '" + guess + "'?)"));
           }
           std::string name = c.argc() > 2 ? c.display(c.arg(2)) : mesh;
           EntityId id = c.scene().create(name);
           c.scene().add<MeshRenderer>(id).mesh = mesh;
           if (c.argc() > 1) c.scene().add<Transform>(id).position = pos;
           return Value::entity(id);
       });

    // --- input ---------------------------------------------------------------------------------
    fn(r, "key", {{"name", S}}, B, "input", "Whether a key is held (\"w\", \"space\", \"left\", ...).",
       "if key(\"w\") then move self by (0, 0, -speed * dt) end",
       [](CallContext& c) { return Value::boolean(c.input().held.count(str::lower(c.string(0))) != 0); });
    fn(r, "key_pressed", {{"name", S}}, B, "input", "Whether a key was pressed this tick (once per press).",
       "if key_pressed(\"space\") then jump() end",
       [](CallContext& c) { return Value::boolean(c.input().pressed.count(str::lower(c.string(0))) != 0); });

    // --- text -------------------------------------------------------------------------------------
    fn(r, "str", {{"value", A}}, S, "text", "Text form of any value (entities give their name).", "log \"hp: \" + str(hp)",
       [](CallContext& c) { return Value::string(c.display(c.arg(0))); });
    fn(r, "num", {{"text", A}}, N | kTNone, "text", "Number parsed from text (none if it is not a number).", "num(\"3.5\")",
       [](CallContext& c) {
           const Value& v = c.arg(0);
           if (v.isNumber()) return v;
           if (v.isBool()) return num(v.b() ? 1 : 0);
           if (!v.isString()) return Value();
           double d = 0;
           if (!str::parseDouble(str::trim(v.str()), d)) return Value();
           return num(d);
       }, true);
    fn(r, "type_of", {{"value", A}}, S, "text", "The type name: number, bool, string, vector, color, entity, list, map, none.",
       "if type_of(data) == \"map\" then ... end", [](CallContext& c) { return Value::string(typeName(c.arg(0).type())); }, true);

    // --- list methods --------------------------------------------------------------------------------
    method(r, L, "push", {{"item", A}}, kTNone, "list", "Appends an item.", "self.visited.push(spot)", [](CallContext& c) {
        c.mutArg(0).mutItems().push_back(c.arg(1));
        return Value();
    }, true);
    method(r, L, "pop", {}, A, "list", "Removes and returns the last item.", "let last = stack.pop()", [](CallContext& c) {
        auto& l = c.mutArg(0).mutItems();
        if (l.empty()) c.fail("pop(): the list is empty");
        Value v = std::move(l.back());
        l.pop_back();
        return v;
    }, true);
    method(r, L, "insert", {{"index", N}, {"item", A}}, kTNone, "list", "Inserts an item before index (index = length appends).",
           "queue.insert(0, job)", [](CallContext& c) {
               size_t size = c.arg(0).items().size();
               auto i = wholeIndex(c, 1, size, true);
               auto& l = c.mutArg(0).mutItems();
               l.insert(l.begin() + i, c.arg(2));
               return Value();
           }, true);
    method(r, L, "remove_at", {{"index", N}}, A, "list", "Removes and returns the item at index.", "let first = queue.remove_at(0)",
           [](CallContext& c) {
               size_t size = c.arg(0).items().size();
               auto i = wholeIndex(c, 1, size, false);
               auto& l = c.mutArg(0).mutItems();
               Value v = std::move(l[static_cast<size_t>(i)]);
               l.erase(l.begin() + i);
               return v;
           }, true);
    method(r, L | M, "remove", {{"item_or_key", A}}, B, "list",
           "Lists: removes the first item equal to the argument. Maps: removes a key. Returns whether something was removed.",
           "targets.remove(other)", [](CallContext& c) {
               Value& self = c.mutArg(0);
               if (self.isMap()) {
                   if (!c.arg(1).isString()) return Value::boolean(false);
                   if (!self.mapObj().find(c.arg(1).str())) return Value::boolean(false);
                   return Value::boolean(self.mutMap().erase(c.arg(1).str()));
               }
               const auto& items = self.items();
               for (size_t i = 0; i < items.size(); ++i) {
                   if (items[i] == c.arg(1)) {
                       auto& l = self.mutItems();
                       l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
                       return Value::boolean(true);
                   }
               }
               return Value::boolean(false);
           }, true);
    method(r, L | M | S, "contains", {{"item", A}}, B, "list",
           "Lists: has an equal item. Maps: has the key. Strings: has the substring. (Same as `x in y`.)",
           "if inventory.contains(\"key\") then ... end", [](CallContext& c) {
               const Value& self = c.arg(0);
               const Value& x = c.arg(1);
               c.charge(self.isList() ? static_cast<int64_t>(self.items().size()) : 1);
               if (self.isList()) return Value::boolean(std::find(self.items().begin(), self.items().end(), x) != self.items().end());
               if (self.isMap()) return Value::boolean(x.isString() && self.mapObj().find(x.str()) != nullptr);
               return Value::boolean(x.isString() && self.str().find(x.str()) != std::string::npos);
           });
    method(r, L | S, "index_of", {{"item", A}}, N, "list", "Position of the first equal item/substring, or -1.",
           "let i = names.index_of(\"Bob\")", [](CallContext& c) {
               const Value& self = c.arg(0);
               if (self.isString()) {
                   if (!c.arg(1).isString()) return num(-1);
                   size_t p = self.str().find(c.arg(1).str());
                   return num(p == std::string::npos ? -1.0 : static_cast<double>(utf8Length(self.str().substr(0, p))));
               }
               const auto& l = self.items();
               for (size_t i = 0; i < l.size(); ++i) {
                   if (l[i] == c.arg(1)) return num(static_cast<double>(i));
               }
               return num(-1);
           });
    method(r, L, "sort", {}, kTNone, "list", "Sorts numbers or strings in place (ascending).", "scores.sort()", [](CallContext& c) {
        auto& l = c.mutArg(0).mutItems();
        c.charge(static_cast<int64_t>(l.size()) * 4);
        std::stable_sort(l.begin(), l.end(), [&](const Value& a, const Value& b) { return lessValue(c, a, b); });
        return Value();
    }, true);
    method(r, L, "sorted", {}, L, "list", "A sorted copy (numbers or strings).", "for s in scores.sorted() ... end",
           [](CallContext& c) {
               std::vector<Value> l = c.arg(0).items();
               c.charge(static_cast<int64_t>(l.size()) * 4);
               std::stable_sort(l.begin(), l.end(), [&](const Value& a, const Value& b) { return lessValue(c, a, b); });
               return Value::list(std::move(l));
           });
    method(r, L, "reverse", {}, kTNone, "list", "Reverses the list in place.", "path.reverse()", [](CallContext& c) {
        auto& l = c.mutArg(0).mutItems();
        std::reverse(l.begin(), l.end());
        return Value();
    }, true);
    method(r, L | S, "slice", {{"from", N}, {"to", N, true}}, L | S, "list",
           "Items (or characters) from index `from` up to, not including, `to`. Negative indexes count from the end.",
           "let firstThree = items.slice(0, 3)", [](CallContext& c) {
               const Value& self = c.arg(0);
               if (self.isString()) {
                   auto chars = utf8Chars(self.str());
                   auto [a, b] = sliceRange(c, chars.size());
                   std::string out;
                   for (size_t i = a; i < b; ++i) out += chars[i];
                   return Value::string(std::move(out));
               }
               const auto& l = self.items();
               auto [a, b] = sliceRange(c, l.size());
               return Value::list(std::vector<Value>(l.begin() + static_cast<std::ptrdiff_t>(a), l.begin() + static_cast<std::ptrdiff_t>(b)));
           });
    method(r, L, "join", {{"separator", S, true}}, S, "list", "Text of all items joined by a separator.",
           "log names.join(\", \")", [](CallContext& c) {
               std::string sep = c.argc() > 1 ? c.string(1) : "";
               std::string out;
               bool first = true;
               for (const auto& v : c.arg(0).items()) {
                   if (!first) out += sep;
                   first = false;
                   out += c.display(v);
               }
               return Value::string(std::move(out));
           });
    method(r, L | M, "clear", {}, kTNone, "list", "Removes everything.", "self.targets.clear()", [](CallContext& c) {
        Value& self = c.mutArg(0);
        if (self.isList()) self.mutItems().clear();
        else self.mutMap().entries.clear();
        return Value();
    }, true);
    method(r, L, "first", {}, A, "list", "The first item (none if empty).", "let next = queue.first()", [](CallContext& c) {
        const auto& l = c.arg(0).items();
        return l.empty() ? Value() : l.front();
    });
    method(r, L, "last", {}, A, "list", "The last item (none if empty).", "path.last()", [](CallContext& c) {
        const auto& l = c.arg(0).items();
        return l.empty() ? Value() : l.back();
    });
    method(r, L | M | S, "is_empty", {}, B, "list", "Whether it has no items / keys / characters.", "if queue.is_empty() then ... end",
           [](CallContext& c) {
               const Value& self = c.arg(0);
               if (self.isList()) return Value::boolean(self.items().empty());
               if (self.isMap()) return Value::boolean(self.mapObj().entries.empty());
               return Value::boolean(self.str().empty());
           });
    method(r, L, "sum", {}, N, "list", "Sum of a list of numbers.", "let total = scores.sum()", [](CallContext& c) {
        double s = 0;
        for (const auto& v : c.arg(0).items()) {
            if (!v.isNumber()) c.fail("sum(): the list must contain numbers only");
            s += v.num();
        }
        return num(s);
    });

    // --- map methods ---------------------------------------------------------------------------------
    method(r, M, "keys", {}, L, "map", "The keys, in insertion order.", "for k in stats.keys() ... end", [](CallContext& c) {
        std::vector<Value> out;
        for (const auto& [k, v] : c.arg(0).mapObj().entries) out.push_back(Value::string(k));
        return Value::list(std::move(out));
    });
    method(r, M, "values", {}, L, "map", "The values, in insertion order.", "stats.values().sum()", [](CallContext& c) {
        std::vector<Value> out;
        for (const auto& [k, v] : c.arg(0).mapObj().entries) out.push_back(v);
        return Value::list(std::move(out));
    });
    method(r, M, "has", {{"key", S}}, B, "map", "Whether the map has a key.", "if data.has(\"amount\") then ... end",
           [](CallContext& c) { return Value::boolean(c.arg(0).mapObj().find(c.string(1)) != nullptr); });
    method(r, M, "get", {{"key", S}, {"default", A, true}}, A, "map", "Value for a key, or the default (none) if missing.",
           "let dmg = data.get(\"amount\", 1)", [](CallContext& c) {
               const Value* v = c.arg(0).mapObj().find(c.string(1));
               if (v) return *v;
               return c.argc() > 2 ? c.arg(2) : Value();
           });
    method(r, M, "set", {{"key", S}, {"value", A}}, kTNone, "map", "Sets a key (same as m[key] = value).",
           "self.stats.set(\"hp\", 10)", [](CallContext& c) {
               std::string k = c.string(1);
               c.mutArg(0).mutMap().set(k, c.arg(2));
               return Value();
           }, true);

    // --- string methods --------------------------------------------------------------------------------
    method(r, S, "upper", {}, S, "text", "Uppercase copy.", "name.upper()", [](CallContext& c) { return Value::string(toUpper(c.arg(0).str())); });
    method(r, S, "lower", {}, S, "text", "Lowercase copy.", "answer.lower()", [](CallContext& c) { return Value::string(toLower(c.arg(0).str())); });
    method(r, S, "trim", {}, S, "text", "Copy without leading/trailing spaces.", "input.trim()",
           [](CallContext& c) { return Value::string(str::trim(c.arg(0).str())); });
    method(r, S, "starts_with", {{"prefix", S}}, B, "text", "Whether the text starts with a prefix.", "name.starts_with(\"Enemy\")",
           [](CallContext& c) { return Value::boolean(str::startsWith(c.arg(0).str(), c.string(1))); });
    method(r, S, "ends_with", {{"suffix", S}}, B, "text", "Whether the text ends with a suffix.", "file.ends_with(\".png\")",
           [](CallContext& c) {
               const std::string& s = c.arg(0).str();
               const std::string& x = c.string(1);
               return Value::boolean(s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0);
           });
    method(r, S, "split", {{"separator", S, true}}, L, "text", "Parts of the text between separators (default: spaces).",
           "for word in line.split(\" \") ... end", [](CallContext& c) {
               const std::string& s = c.arg(0).str();
               std::vector<Value> out;
               if (c.argc() < 2 || c.string(1).empty()) {
                   std::string cur;
                   for (char ch : s) {
                       if (std::isspace(static_cast<unsigned char>(ch))) {
                           if (!cur.empty()) out.push_back(Value::string(cur));
                           cur.clear();
                       } else {
                           cur.push_back(ch);
                       }
                   }
                   if (!cur.empty()) out.push_back(Value::string(cur));
                   return Value::list(std::move(out));
               }
               const std::string& sep = c.string(1);
               size_t start = 0;
               while (true) {
                   size_t p = s.find(sep, start);
                   if (p == std::string::npos) {
                       out.push_back(Value::string(s.substr(start)));
                       break;
                   }
                   out.push_back(Value::string(s.substr(start, p - start)));
                   start = p + sep.size();
               }
               return Value::list(std::move(out));
           });
    method(r, S, "replace", {{"old", S}, {"new", S}}, S, "text", "Copy with every occurrence of old replaced.",
           "msg.replace(\"{name}\", player.name)", [](CallContext& c) {
               std::string s = c.arg(0).str();
               const std::string& from = c.string(1);
               const std::string& to = c.string(2);
               if (from.empty()) return Value::string(s);
               size_t p = 0;
               while ((p = s.find(from, p)) != std::string::npos) {
                   s.replace(p, from.size(), to);
                   p += to.size();
               }
               return Value::string(std::move(s));
           });

    // --- statement helpers (move/rotate/look/emit/destroy/log and test input) ------------------------
    hidden(r, "__move_by", {{"entity", E}, {"offset", kTPoint}}, [](CallContext& c) {
        Vec3 delta = c.point(1);
        transformArg(c, 0).position += delta;
        c.scene().markDirty();
        return Value();
    });
    hidden(r, "__move_toward", {{"entity", E}, {"destination", kTPoint}, {"speed", N}}, [](CallContext& c) {
        EntityRef id = c.entity(0);
        Vec3 goal = c.point(1);
        double speed = c.number(2);
        Transform& t = c.scene().add<Transform>(id);
        Vec3 from = worldPosition(c.scene(), id);
        Vec3 delta = goal - from;
        float dist = length(delta);
        float step = static_cast<float>(speed) * c.dt();
        if (dist > 1e-6f) t.position += delta * (std::min(step, dist) / dist);
        c.scene().markDirty();
        return Value();
    });
    hidden(r, "__rotate", {{"entity", E}, {"degrees", kTPoint}}, [](CallContext& c) {
        Vec3 delta = c.point(1);
        Transform& t = transformArg(c, 0);
        t.rotation += delta;
        // Keep angles bounded so long sessions don't lose float precision.
        for (float* a : {&t.rotation.x, &t.rotation.y, &t.rotation.z}) *a = std::remainder(*a, 360.f);
        c.scene().markDirty();
        return Value();
    });
    hidden(r, "__look", {{"entity", E}, {"point", kTPoint}}, [](CallContext& c) {
        EntityRef id = c.entity(0);
        Vec3 at = c.point(1);
        Vec3 d = at - worldPosition(c.scene(), id);
        Transform& t = c.scene().add<Transform>(id);
        if (length(d) > 1e-6f) {
            float horizontal = std::sqrt(d.x * d.x + d.z * d.z);
            t.rotation = {degrees(std::atan2(d.y, horizontal)), degrees(std::atan2(-d.x, -d.z)), 0.f};
        }
        c.scene().markDirty();
        return Value();
    });
    hidden(r, "__emit", {{"name", S}, {"payload", A}}, [](CallContext& c) {
        c.runtime().emit(c.string(0), kNoEntity, c.arg(1), c.self());
        return Value();
    });
    hidden(r, "__emit_to", {{"name", S}, {"payload", A}, {"receiver", E}}, [](CallContext& c) {
        if (!c.arg(2).isEntity()) {
            c.fail(std::string("the event receiver must be an entity, got ") + typeName(c.arg(2).type()) +
                   (c.arg(2).isNone() ? " (find() found nothing?)" : ""));
        }
        c.runtime().emit(c.string(0), c.entity(2), c.arg(1), c.self());
        return Value();
    });
    hidden(r, "__destroy", {{"entity", A}}, [](CallContext& c) {
        const Value& v = c.arg(0);
        if (v.isEntity() && c.scene().exists(v.e())) CallContextAccess(c).impl.toDestroy.push_back(v.e());
        return Value();
    });
    hidden(r, "__log", {{"value", A}}, [](CallContext& c) {
        ExecState& st = CallContextAccess(c);
        st.impl.messages.push_back({RuntimeMessage::Kind::Log, st.self, st.scriptName ? *st.scriptName : std::string(),
                                    c.loc().line, c.display(c.arg(0)), {}});
        return Value();
    });
    hidden(r, "__press", {{"key", S}}, [](CallContext& c) {
        ExecState& st = CallContextAccess(c);
        if (st.test && st.test->press) st.test->press(str::lower(c.string(0)));
        return Value();
    });
    hidden(r, "__hold", {{"key", S}}, [](CallContext& c) {
        ExecState& st = CallContextAccess(c);
        if (st.test && st.test->hold) st.test->hold(str::lower(c.string(0)));
        return Value();
    });
    hidden(r, "__release", {{"key", S}}, [](CallContext& c) {
        ExecState& st = CallContextAccess(c);
        if (st.test && st.test->release) st.test->release(str::lower(c.string(0)));
        return Value();
    });
    hidden(r, "__click", {{"entity", E}}, [](CallContext& c) {
        ExecState& st = CallContextAccess(c);
        if (st.test && st.test->click) st.test->click(c.entity(0));
        return Value();
    });
}

}  // namespace sky::wander
