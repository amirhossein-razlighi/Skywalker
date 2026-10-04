// Wander runtime: instance scheduling, entity vars, events, coroutines, state machines,
// and the bytecode interpreter.

#include "skywalker/wander/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "RuntimeInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/wander/Aot.h"

namespace sky::wander {

namespace fs = std::filesystem;

namespace {
constexpr size_t kStackSize = 1u << 16;
constexpr int kMaxTransitionsPerTick = 32;
constexpr size_t kMaxPendingEvents = 100000;
constexpr int kMaxErrorsBeforeDisable = 5;
}  // namespace

Json RuntimeMessage::toJson() const {
    const char* k = kind == Kind::Log ? "log" : kind == Kind::Error ? "runtime_error" : "compile_error";
    Json j = Json::object({{"kind", k}, {"entity", entity}, {"script", script}, {"line", line}, {"text", text}});
    if (!file.empty()) j["file"] = file;
    return j;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void raise(SourceLoc loc, std::string message) { throw RuntimeError{loc, std::move(message), {}}; }

namespace {

void appendDisplay(const Scene& scene, std::string& out, const Value& v, bool quoteStrings) {
    switch (v.type()) {
        case VType::Entity: {
            const EntityRecord* r = scene.record(v.e());
            out += r ? r->name : "<destroyed entity>";
            return;
        }
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
                appendDisplay(scene, out, item, true);
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
                appendDisplay(scene, out, item, true);
            }
            out += "}";
            return;
        }
        default: out += toDisplayString(v); return;
    }
}

}  // namespace

std::string displayValue(const Scene& scene, const Value& v) {
    if (v.isString()) return v.str();
    std::string out;
    appendDisplay(scene, out, v, false);
    return out;
}

Vec3 worldPosition(const Scene& scene, EntityId id) { return scene.worldMatrix(id).translation(); }

std::vector<std::string> utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
        n = std::min(n, s.size() - i);
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (char ch : s) {
        if ((static_cast<unsigned char>(ch) & 0xc0) != 0x80) ++n;
    }
    return n;
}

VarSlot makeSlot(uint32_t sym) {
    VarSlot s;
    s.sym = sym;
    s.name = &symbolName(sym);
    return s;
}

VarTable& varTable(Runtime::Impl& impl, Scene& scene, EntityId id) {
    auto it = impl.vars.find(id);
    if (it != impl.vars.end()) return it->second;
    VarTable& t = impl.vars[id];
    t.entity = id;
    if (const EntityRecord* rec = scene.record(id)) {
        for (const auto& [k, v] : rec->vars.members()) {
            VarSlot s = makeSlot(intern(k));
            s.value = fromJson(v);
            s.inScene = true;
            t.slots.push_back(std::move(s));
        }
    }
    return t;
}

Value getEntityVar(Runtime::Impl& impl, Scene& scene, EntityId id, uint32_t sym) {
    VarTable& t = varTable(impl, scene, id);
    int i = t.find(sym);
    if (i >= 0) return t.slots[i].value;
    // Set from outside since the table was built (an agent or tool between ticks)?
    if (const EntityRecord* rec = scene.record(id)) {
        if (const Json* j = rec->vars.find(symbolName(sym))) {
            VarSlot s = makeSlot(sym);
            s.value = fromJson(*j);
            s.inScene = true;
            t.slots.push_back(s);
            return t.slots.back().value;
        }
    }
    return {};
}

void mirrorDirtyVars(Runtime::Impl& impl, Scene& scene) {
    for (auto& [id, table] : impl.vars) {
        if (!table.anyDirty) continue;
        table.anyDirty = false;
        EntityRecord* rec = scene.record(id);
        for (auto& slot : table.slots) {
            if (!slot.dirty) continue;
            slot.dirty = false;
            if (!rec) continue;
            auto& members = rec->vars.members();
            if (slot.sceneIndex < members.size() && members[slot.sceneIndex].first == *slot.name) {
                members[slot.sceneIndex].second = toJson(slot.value);
            } else {
                rec->vars[*slot.name] = toJson(slot.value);
                for (size_t k = 0; k < members.size(); ++k) {
                    if (members[k].first == *slot.name) slot.sceneIndex = k;
                }
            }
            slot.inScene = true;
        }
    }
}

void setEntityVar(Runtime::Impl& impl, Scene& scene, EntityId id, uint32_t sym, Value v) {
    VarTable& t = varTable(impl, scene, id);
    int i = t.find(sym);
    if (i < 0) {
        t.slots.push_back(makeSlot(sym));
        i = static_cast<int>(t.slots.size() - 1);
    }
    t.slots[i].value = std::move(v);
    t.slots[i].dirty = true;
    t.anyDirty = true;
}

// ---------------------------------------------------------------------------
// CallContext
// ---------------------------------------------------------------------------

namespace {
std::string argLabel(const CallContext& c, int i) {
    std::string name = c.def().name;
    const auto& ps = c.def().params;
    int pi = c.def().receiver ? i - 1 : i;
    std::string label = name + "(): ";
    if (c.def().receiver && i == 0) return label + "the receiver";
    if (pi >= 0 && pi < static_cast<int>(ps.size())) return label + "argument " + std::to_string(pi + 1) + " (" + ps[pi].name + ")";
    return label + "argument " + std::to_string(pi + 1);
}
}  // namespace

double CallContext::number(int i) const {
    const Value& v = args_[i];
    if (v.isNumber()) return v.num();
    if (v.isBool()) return v.b() ? 1 : 0;
    fail(argLabel(*this, i) + " must be a number, got " + typeName(v.type()));
}
bool CallContext::boolean(int i) const { return wander::truthy(scene(), args_[i]); }
const std::string& CallContext::string(int i) const {
    const Value& v = args_[i];
    if (!v.isString()) fail(argLabel(*this, i) + " must be a string, got " + typeName(v.type()));
    return v.str();
}
Vec3 CallContext::vec(int i) const {
    const Value& v = args_[i];
    if (!v.isVec()) fail(argLabel(*this, i) + " must be a vector (x, y, z), got " + typeName(v.type()));
    return v.v();
}
Vec3 CallContext::point(int i) const {
    const Value& v = args_[i];
    if (v.isVec()) return v.v();
    if (v.isEntity()) return worldPosition(scene(), entity(i));
    fail(argLabel(*this, i) + " must be a vector (x, y, z) or an entity, got " + typeName(v.type()));
}
Vec4 CallContext::color(int i) const {
    const Value& v = args_[i];
    if (!v.isColor()) fail(argLabel(*this, i) + " must be a color like #ff8800, got " + typeName(v.type()));
    return v.c();
}
EntityRef CallContext::entity(int i) const {
    const Value& v = args_[i];
    if (!v.isEntity()) fail(argLabel(*this, i) + " must be an entity, got " + typeName(v.type()));
    if (!state_.scene.exists(v.e())) fail(argLabel(*this, i) + " refers to an entity that no longer exists");
    return v.e();
}
const std::vector<Value>& CallContext::list(int i) const {
    const Value& v = args_[i];
    if (!v.isList()) fail(argLabel(*this, i) + " must be a list, got " + typeName(v.type()));
    return v.items();
}
const MapObj& CallContext::map(int i) const {
    const Value& v = args_[i];
    if (!v.isMap()) fail(argLabel(*this, i) + " must be a map, got " + typeName(v.type()));
    return v.mapObj();
}
Runtime& CallContext::runtime() const { return state_.rt; }
Scene& CallContext::scene() const { return state_.scene; }
EntityRef CallContext::self() const { return state_.self; }
EntityRef CallContext::other() const { return state_.other; }
double CallContext::time() const { return state_.rt.time(); }
float CallContext::dt() const { return state_.dt; }
uint64_t CallContext::frame() const { return state_.rt.frame(); }
Random& CallContext::rng() const { return state_.rt.rng(); }
const InputState& CallContext::input() const {
    static const InputState empty;
    return state_.input ? *state_.input : empty;
}
std::string CallContext::display(const Value& v) const { return displayValue(state_.scene, v); }
bool CallContext::truthy(const Value& v) const { return wander::truthy(state_.scene, v); }
void CallContext::charge(int64_t units) const {
    state_.budget -= units;
    if (state_.budget < 0) {
        fail("execution budget exceeded in " + def_.name + "() (more than " + std::to_string(Runtime::kBudget) +
             " steps in one handler run)");
    }
}
void CallContext::fail(std::string message) const { throw RuntimeError{loc_, std::move(message), {}}; }

void CallContext::setLastHit(EntityRef entity, Vec3 point, Vec3 normal, float distance) const {
    state_.lastHit = RayHitInfo{entity, point, normal, distance};
}

void CallContext::clearLastHit() const { state_.lastHit.reset(); }
void* CallContext::serviceById(std::type_index t) const { return state_.rt.serviceById(t); }
ExecState& CallContextAccess(CallContext& c) { return c.state_; }

// ---------------------------------------------------------------------------
// The interpreter
// ---------------------------------------------------------------------------

namespace {

[[noreturn]] void budgetExceeded(SourceLoc loc) {
    raise(loc, "execution budget exceeded: this handler ran more than " + std::to_string(Runtime::kBudget) +
                   " steps (an endless loop?); spread long work over several ticks with `wait`");
}

inline void charge(ExecState& st, int64_t units, SourceLoc loc) {
    st.budget -= units;
    if (st.budget < 0) budgetExceeded(loc);
}

double asNumber(const Value& v, SourceLoc loc, const char* what) {
    if (v.isNumber()) return v.num();
    if (v.isBool()) return v.b() ? 1 : 0;
    raise(loc, std::string(what) + " must be a number, got " + typeName(v.type()));
}

const char* opSymbol(Op op) {
    switch (op) {
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Mod: return "%";
        case Op::Lt: return "<";
        case Op::Le: return "<=";
        case Op::Gt: return ">";
        case Op::Ge: return ">=";
        default: return "?";
    }
}

Value arith(const Scene& scene, Op op, const Value& a, const Value& b, SourceLoc loc) {
    using T = VType;
    auto numeric = [](const Value& v) { return v.isNumber() || v.isBool(); };
    auto n = [](const Value& v) { return v.isBool() ? (v.b() ? 1.0 : 0.0) : v.num(); };
    if (op == Op::Add && (a.isString() || b.isString())) return Value::string(displayValue(scene, a) + displayValue(scene, b));
    if (numeric(a) && numeric(b)) {
        double x = n(a), y = n(b);
        switch (op) {
            case Op::Add: return Value::number(x + y);
            case Op::Sub: return Value::number(x - y);
            case Op::Mul: return Value::number(x * y);
            case Op::Div:
                if (y == 0) raise(loc, "division by zero");
                return Value::number(x / y);
            case Op::Mod:
                if (y == 0) raise(loc, "modulo by zero");
                return Value::number(std::fmod(x, y));
            default: break;
        }
    }
    if (a.type() == T::Vec && b.type() == T::Vec) {
        if (op == Op::Add) return Value::vec(a.v() + b.v());
        if (op == Op::Sub) return Value::vec(a.v() - b.v());
        if (op == Op::Mul) return Value::vec(a.v() * b.v());
    }
    if (a.type() == T::Vec && numeric(b)) {
        if (op == Op::Mul) return Value::vec(a.v() * static_cast<float>(n(b)));
        if (op == Op::Div) {
            if (n(b) == 0) raise(loc, "division by zero");
            return Value::vec(a.v() / static_cast<float>(n(b)));
        }
    }
    if (numeric(a) && b.type() == T::Vec && op == Op::Mul) return Value::vec(b.v() * static_cast<float>(n(a)));
    if (a.type() == T::Color && numeric(b) && op == Op::Mul) {
        auto f = static_cast<float>(n(b));
        Vec4 c = a.c();
        return Value::color({c.x * f, c.y * f, c.z * f, c.w});
    }
    if (numeric(a) && b.type() == T::Color && op == Op::Mul) {
        auto f = static_cast<float>(n(a));
        Vec4 c = b.c();
        return Value::color({c.x * f, c.y * f, c.z * f, c.w});
    }
    if (a.type() == T::Color && b.type() == T::Color && op == Op::Add) {
        Vec4 x = a.c(), y = b.c();
        return Value::color({x.x + y.x, x.y + y.y, x.z + y.z, std::max(x.w, y.w)});
    }
    if (a.type() == T::List && b.type() == T::List && op == Op::Add) {
        std::vector<Value> items = a.items();
        items.insert(items.end(), b.items().begin(), b.items().end());
        return Value::list(std::move(items));
    }
    std::string hint;
    if (op == Op::Add && (a.isNone() || b.isNone())) hint = " (a value is none: was a var never set, or did find() return none?)";
    raise(loc, std::string("cannot apply '") + opSymbol(op) + "' to " + typeName(a.type()) + " and " + typeName(b.type()) + hint);
}

bool compare(Op op, const Value& a, const Value& b, SourceLoc loc) {
    auto numeric = [](const Value& v) { return v.isNumber() || v.isBool(); };
    auto n = [](const Value& v) { return v.isBool() ? (v.b() ? 1.0 : 0.0) : v.num(); };
    int c;
    if (numeric(a) && numeric(b)) {
        double x = n(a), y = n(b);
        switch (op) {
            case Op::Lt: return x < y;
            case Op::Le: return x <= y;
            case Op::Gt: return x > y;
            default: return x >= y;
        }
    }
    if (a.isString() && b.isString()) {
        c = a.str().compare(b.str());
        switch (op) {
            case Op::Lt: return c < 0;
            case Op::Le: return c <= 0;
            case Op::Gt: return c > 0;
            default: return c >= 0;
        }
    }
    std::string hint;
    if (a.isNone() || b.isNone()) hint = " (a value is none: was a var never set?)";
    raise(loc, std::string("cannot compare ") + typeName(a.type()) + " and " + typeName(b.type()) + " with '" + opSymbol(op) + "'" + hint);
}

bool contains(const Value& item, const Value& coll, SourceLoc loc) {
    if (coll.isList()) {
        for (const auto& v : coll.items()) {
            if (v == item) return true;
        }
        return false;
    }
    if (coll.isMap()) {
        if (!item.isString()) return false;
        return coll.mapObj().find(item.str()) != nullptr;
    }
    if (coll.isString()) {
        if (!item.isString()) raise(loc, std::string("'in' on a string needs a string on the left, got ") + typeName(item.type()));
        return coll.str().find(item.str()) != std::string::npos;
    }
    raise(loc, std::string("'in' needs a list, map or string on the right, got ") + typeName(coll.type()));
}

int64_t listIndex(const Value& index, size_t size, SourceLoc loc, const char* what) {
    if (!index.isNumber()) raise(loc, std::string(what) + " indexes are numbers, got " + typeName(index.type()));
    double d = index.num();
    if (d != std::floor(d)) raise(loc, std::string(what) + " index must be a whole number, got " + formatNumber(d));
    auto i = static_cast<int64_t>(d);
    if (i < 0) i += static_cast<int64_t>(size);
    if (i < 0 || i >= static_cast<int64_t>(size)) {
        raise(loc, "index " + formatNumber(d) + " is out of range (the " + what + " has " + std::to_string(size) +
                       (size == 1 ? " item" : " items") + ")");
    }
    return i;
}

Value indexValue(const Value& obj, const Value& index, SourceLoc loc) {
    if (obj.isList()) return obj.items()[static_cast<size_t>(listIndex(index, obj.items().size(), loc, "list"))];
    if (obj.isMap()) {
        if (!index.isString()) raise(loc, std::string("map keys are strings, got ") + typeName(index.type()));
        const Value* v = obj.mapObj().find(index.str());
        return v ? *v : Value();
    }
    if (obj.isString()) {
        auto chars = utf8Chars(obj.str());
        return Value::string(chars[static_cast<size_t>(listIndex(index, chars.size(), loc, "string"))]);
    }
    std::string hint = obj.isNone() ? " (the value is none)" : "";
    raise(loc, std::string("cannot index a ") + typeName(obj.type()) + " with []" + hint);
}

void setIndexValue(Value& obj, const Value& index, const Value& v, SourceLoc loc) {
    if (obj.isList()) {
        auto i = listIndex(index, obj.items().size(), loc, "list");
        obj.mutItems()[static_cast<size_t>(i)] = v;
        return;
    }
    if (obj.isMap()) {
        if (!index.isString()) raise(loc, std::string("map keys are strings, got ") + typeName(index.type()));
        obj.mutMap().set(index.str(), v);
        return;
    }
    if (obj.isString()) raise(loc, "strings cannot be changed in place; build a new one");
    raise(loc, std::string("cannot set [] on a ") + typeName(obj.type()));
}

// --- entity properties --------------------------------------------------------------

EntityId requireEntity(const Scene& scene, const Value& v, SourceLoc loc, const char* what) {
    if (!v.isEntity()) raise(loc, std::string(what) + " must be an entity, got " + typeName(v.type()));
    if (!scene.exists(v.e())) raise(loc, std::string(what) + " refers to an entity that no longer exists");
    return v.e();
}

std::string currentStateOf(Runtime::Impl& impl, const Scene& scene, EntityId e) {
    std::string out;
    forEachInstance(impl, scene, e, [&](const Instance& inst) {
        if (!out.empty() || !inst.program) return;
        for (size_t b = 0; b < inst.behaviors.size() && b < inst.program->behaviors.size(); ++b) {
            int s = inst.behaviors[b].state;
            if (s >= 0) {
                out = inst.program->behaviors[b].states[s].name;
                return;
            }
        }
    });
    return out;
}

Value getEntityMember(ExecState& st, EntityId id, const MemberRef& m, SourceLoc loc) {
    using K = MemberRef::Kind;
    Scene& scene = st.scene;
    switch (m.kind) {
        case K::Position:
        case K::Rotation:
        case K::Scale: {
            const Transform* t = scene.get<Transform>(id);
            if (!t) return Value::vec(m.kind == K::Scale ? Vec3{1, 1, 1} : Vec3{0, 0, 0});
            return Value::vec(m.kind == K::Position ? t->position : m.kind == K::Rotation ? t->rotation : t->scale);
        }
        case K::Color:
            if (const auto* mr = scene.get<MeshRenderer>(id)) return Value::color(mr->color);
            if (const auto* l = scene.get<Light>(id)) return Value::color(l->color);
            return {};
        case K::Name: return Value::string(scene.record(id)->name);
        case K::Id: return Value::number(static_cast<double>(id));
        case K::Enabled: return Value::boolean(scene.record(id)->enabled);
        case K::Tags: {
            std::vector<Value> tags;
            for (const auto& t : scene.record(id)->tags) tags.push_back(Value::string(t));
            return Value::list(std::move(tags));
        }
        case K::Parent: {
            EntityId p = scene.record(id)->parent;
            return p ? Value::entity(p) : Value();
        }
        case K::State: {
            std::string s = currentStateOf(st.impl, scene, id);
            return s.empty() ? Value() : Value::string(s);
        }
        default: break;
    }
    Value v = getEntityVar(st.impl, scene, id, m.sym);
    if (v.isNone() && scene.componentKind(m.name)) {
        raise(loc, "'" + m.name + "' is a component; read one of its fields like ." + m.name + ".<field>");
    }
    return v;
}

std::string cosmeticComponentList() {
    std::string out;
    for (const auto& c : cosmeticComponents()) out += (out.empty() ? "" : ", ") + c;
    return out;
}

[[noreturn]] void cosmeticError(SourceLoc loc, const std::string& what) {
    raise(loc, "on frame is cosmetic: it cannot " + what +
                   " (it runs at display rate and its writes are undone after the frame; change simulation state in on "
                   "tick, then style it in on frame)");
}

void setEntityMember(ExecState& st, EntityId id, const MemberRef& m, const Value& v, SourceLoc loc) {
    using K = MemberRef::Kind;
    if (!st.rt.sceneWriteGuard().empty()) raise(loc, st.rt.sceneWriteGuard());
    Scene& scene = st.scene;
    if (st.cosmetic) [[unlikely]] {
        if (m.kind == K::Position || m.kind == K::Rotation || m.kind == K::Scale) {
            if (!scene.get<Transform>(id)) cosmeticError(loc, "add a transform");
            recordCosmeticWrite(st, id, nullptr, nullptr);
        } else if (m.kind == K::Color) {
            const char* comp = scene.get<MeshRenderer>(id) ? "mesh" : scene.get<Light>(id) ? "light" : nullptr;
            if (!comp) cosmeticError(loc, "add a mesh (.color needs a mesh or a light)");
            const ComponentKind* kind = scene.componentKind(comp);
            recordCosmeticWrite(st, id, kind, kind->info->field("color"));
        } else {
            cosmeticError(loc, "set ." + m.name + " (only position, rotation, scale, color and fields of " +
                                   cosmeticComponentList() + ")");
        }
    }
    if (st.rt.recordSceneWrites() && scene.observer()) scene.observer()->beforeEntityChange(id);
    scene.markDirty();
    switch (m.kind) {
        case K::Position:
        case K::Rotation:
        case K::Scale: {
            Vec3 x;
            if (v.isVec()) {
                x = v.v();
            } else if (v.isEntity()) {
                x = worldPosition(scene, requireEntity(scene, v, loc, m.name.c_str()));
            } else {
                raise(loc, m.name + " must be a vector like (0, 1, 0), got " + typeName(v.type()));
            }
            Transform& t = scene.add<Transform>(id);
            (m.kind == K::Position ? t.position : m.kind == K::Rotation ? t.rotation : t.scale) = x;
            return;
        }
        case K::Color: {
            if (!v.isColor()) raise(loc, std::string("color must be a color like #ff8800, got ") + typeName(v.type()));
            if (auto* mr = scene.get<MeshRenderer>(id)) {
                mr->color = v.c();
            } else if (auto* l = scene.get<Light>(id)) {
                l->color = v.c();
            } else {
                scene.add<MeshRenderer>(id).color = v.c();
            }
            return;
        }
        case K::Name: scene.record(id)->name = displayValue(scene, v); return;
        case K::Enabled: scene.record(id)->enabled = truthy(scene, v); return;
        case K::Id: raise(loc, "id is read-only");
        case K::Tags: raise(loc, "tags is read-only (use add_tag / remove_tag)");
        case K::Parent: raise(loc, "parent is read-only (use set_parent)");
        case K::State: raise(loc, "state is read-only (use go to inside the behavior)");
        default: break;
    }
    if (scene.componentKind(m.name)) {
        raise(loc, "'" + m.name + "' is a component; assign one of its fields like ." + m.name + ".<field> = ...");
    }
    setEntityVar(st.impl, scene, id, m.sym, v);
}

Value getMember(ExecState& st, const Value& obj, const MemberRef& m, SourceLoc loc) {
    using K = MemberRef::Kind;
    switch (obj.type()) {
        case VType::Entity: {
            if (m.kind == K::Position || m.kind == K::Rotation || m.kind == K::Scale) {
                // Hot path: one record lookup, then the transform by handle.
                const EntityRecord* rec = st.scene.record(obj.e());
                if (!rec) raise(loc, "the property owner refers to an entity that no longer exists");
                const Transform* t = st.scene.registry().get<Transform>(rec->handle);
                if (!t) return Value::vec(m.kind == K::Scale ? Vec3{1, 1, 1} : Vec3{0, 0, 0});
                return Value::vec(m.kind == K::Position ? t->position : m.kind == K::Rotation ? t->rotation : t->scale);
            }
            return getEntityMember(st, requireEntity(st.scene, obj, loc, "the property owner"), m, loc);
        }
        case VType::Vec:
            switch (m.kind) {
                case K::X: return Value::number(obj.v().x);
                case K::Y: return Value::number(obj.v().y);
                case K::Z: return Value::number(obj.v().z);
                case K::Length: return Value::number(length(obj.v()));
                default: break;
            }
            break;
        case VType::Color:
            switch (m.kind) {
                case K::R: return Value::number(obj.c().x);
                case K::G: return Value::number(obj.c().y);
                case K::B: return Value::number(obj.c().z);
                case K::A: return Value::number(obj.c().w);
                default: break;
            }
            break;
        case VType::Map: {
            const Value* v = obj.mapObj().find(m.name);
            return v ? *v : Value();
        }
        case VType::List:
            if (m.kind == K::Length) return Value::number(static_cast<double>(obj.items().size()));
            break;
        case VType::String:
            if (m.kind == K::Length) return Value::number(static_cast<double>(utf8Length(obj.str())));
            break;
        case VType::None:
            raise(loc, "cannot read '." + m.name + "' of none (is the entity missing, or was a var never set?)");
        default: break;
    }
    raise(loc, std::string("a ") + typeName(obj.type()) + " has no property '" + m.name + "'");
}

void setMember(ExecState& st, Value& obj, const MemberRef& m, const Value& v, SourceLoc loc) {
    using K = MemberRef::Kind;
    switch (obj.type()) {
        case VType::Entity: setEntityMember(st, requireEntity(st.scene, obj, loc, "the assignment target"), m, v, loc); return;
        case VType::Vec: {
            float* f = obj.floats();
            float x = static_cast<float>(asNumber(v, loc, ("." + m.name).c_str()));
            if (m.kind == K::X) f[0] = x;
            else if (m.kind == K::Y) f[1] = x;
            else if (m.kind == K::Z) f[2] = x;
            else raise(loc, "cannot set '." + m.name + "' on a vector");
            return;
        }
        case VType::Color: {
            float* f = obj.floats();
            float x = static_cast<float>(asNumber(v, loc, ("." + m.name).c_str()));
            if (m.kind == K::R) f[0] = x;
            else if (m.kind == K::G) f[1] = x;
            else if (m.kind == K::B) f[2] = x;
            else if (m.kind == K::A) f[3] = x;
            else raise(loc, "cannot set '." + m.name + "' on a color");
            return;
        }
        case VType::Map: obj.mutMap().set(m.name, v); return;
        case VType::None: raise(loc, "cannot set '." + m.name + "' on none (is the entity missing, or was a var never set?)");
        default: raise(loc, std::string("cannot set '.") + m.name + "' on a " + typeName(obj.type()));
    }
}

const Runtime::Impl::ResolvedField& resolveField(ExecState& st, const FieldRef& f, SourceLoc loc) {
    std::string key = f.component + "." + f.field;
    auto it = st.impl.fieldCache.find(key);
    if (it != st.impl.fieldCache.end()) return it->second;
    const ComponentKind* kind = st.scene.componentKind(f.component);
    if (!kind || !kind->info) raise(loc, "unknown component '" + f.component + "'");
    const FieldInfo* field = kind->info->field(f.field);
    if (!field) {
        std::string guess = str::closest(f.field, kind->info->fieldNames());
        raise(loc, f.component + " has no field '" + f.field + "'" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
    }
    return st.impl.fieldCache[key] = {kind, field};
}

Value getField(ExecState& st, const Value& obj, const FieldRef& f, SourceLoc loc) {
    EntityId id = requireEntity(st.scene, obj, loc, "the component owner");
    const auto& r = resolveField(st, f, loc);
    void* c = r.kind->ptr ? r.kind->ptr(st.scene, id) : nullptr;
    if (!c) {
        if (!r.kind->has(st.scene, id)) return {};
        return fromJson(r.kind->toJson(st.scene, id).get(f.field));
    }
    const auto* base = static_cast<const char*>(c) + r.field->offset;
    switch (r.field->type) {
        case FieldType::Float: return Value::number(*reinterpret_cast<const float*>(base));
        case FieldType::Int: return Value::number(*reinterpret_cast<const int*>(base));
        case FieldType::Bool: return Value::boolean(*reinterpret_cast<const bool*>(base));
        case FieldType::String:
        case FieldType::Enum: return Value::string(*reinterpret_cast<const std::string*>(base));
        case FieldType::Vec3: return Value::vec(*reinterpret_cast<const Vec3*>(base));
        case FieldType::Color: return Value::color(*reinterpret_cast<const Vec4*>(base));
        case FieldType::Json: return fromJson(*reinterpret_cast<const Json*>(base));
        case FieldType::Vec2: {
            const Vec2 v = *reinterpret_cast<const Vec2*>(base);
            return Value::vec({v.x, v.y, 0.f});
        }
        case FieldType::Vec4: {
            const Vec4 v = *reinterpret_cast<const Vec4*>(base);
            return Value::list({Value::number(v.x), Value::number(v.y), Value::number(v.z), Value::number(v.w)});
        }
        case FieldType::Entity: {  // the linked entity, or none (unset / dangling)
            EntityId t = st.scene.resolve(*reinterpret_cast<const EntityLink*>(base), id);
            return t ? Value::entity(t) : Value();
        }
        case FieldType::EntityList: {
            std::vector<Value> out;
            for (const auto& l : *reinterpret_cast<const std::vector<EntityLink>*>(base)) {
                if (EntityId t = st.scene.resolve(l, id)) out.push_back(Value::entity(t));
            }
            return Value::list(std::move(out));
        }
    }
    return {};
}

void setField(ExecState& st, const Value& obj, const FieldRef& f, const Value& v, SourceLoc loc) {
    if (!st.rt.sceneWriteGuard().empty()) raise(loc, st.rt.sceneWriteGuard());
    EntityId id = requireEntity(st.scene, obj, loc, "the component owner");
    if (st.rt.recordSceneWrites() && st.scene.observer()) st.scene.observer()->beforeEntityChange(id);
    const auto& r = resolveField(st, f, loc);
    if (st.cosmetic) [[unlikely]] {
        if (!cosmeticComponent(f.component)) {
            cosmeticError(loc, "set " + f.component + "." + f.field + " (frame handlers may set fields of " +
                                   cosmeticComponentList() + ")");
        }
        if (!r.kind->has(st.scene, id)) cosmeticError(loc, "add a " + f.component + " component");
        recordCosmeticWrite(st, id, r.kind, r.field);
    }
    st.scene.markDirty();
    void* c = r.kind->ptr ? r.kind->ptr(st.scene, id) : nullptr;
    const FieldInfo& fi = *r.field;
    std::string what = f.component + "." + f.field;
    if (c) {
        char* base = static_cast<char*>(c) + fi.offset;
        switch (fi.type) {
            case FieldType::Float:
                if (!v.isNumber()) raise(loc, what + " must be a number, got " + typeName(v.type()));
                *reinterpret_cast<float*>(base) = std::clamp(static_cast<float>(v.num()), fi.minValue, fi.maxValue);
                return;
            case FieldType::Int:
                if (!v.isNumber()) raise(loc, what + " must be a whole number, got " + typeName(v.type()));
                *reinterpret_cast<int*>(base) = static_cast<int>(v.num());
                return;
            case FieldType::Bool:
                if (!v.isBool()) raise(loc, what + " must be true or false, got " + typeName(v.type()));
                *reinterpret_cast<bool*>(base) = v.b();
                return;
            case FieldType::Vec3:
                if (!v.isVec()) raise(loc, what + " must be a vector (x, y, z), got " + typeName(v.type()));
                *reinterpret_cast<Vec3*>(base) = v.v();
                return;
            case FieldType::Color:
                if (!v.isColor()) raise(loc, what + " must be a color like #ff8800, got " + typeName(v.type()));
                *reinterpret_cast<Vec4*>(base) = v.c();
                return;
            case FieldType::Entity:
                // An entity value links by id (rename-proof); a string is a name ("Player", "%Muzzle", "#12").
                if (v.isEntity()) {
                    EntityLink l;
                    l.id = v.e();
                    if (const EntityRecord* t = st.scene.record(v.e())) l.name = t->name;
                    *reinterpret_cast<EntityLink*>(base) = l;
                    return;
                }
                if (!v.isString() && !v.isNone()) {
                    raise(loc, what + " must be an entity, a name or none, got " + typeName(v.type()));
                }
                break;  // names and none: parsed by reflection, then bound
            case FieldType::String:
            case FieldType::Enum:
            case FieldType::Vec2:  // vectors (z ignored), lists and numbers: parsed by reflection
            case FieldType::Vec4:
            case FieldType::EntityList:  // lists of entities / names: {"$entity"} values parse as links
            case FieldType::Json: break;  // validated through reflection below
        }
    }
    // Missing component (added on assignment, like the inspector) or validated string fields.
    Status s = r.kind->apply(st.scene, id, Json::object({{f.field, toJson(v)}}));
    if (!s) raise(loc, s.error().message + (s.error().hint.empty() ? "" : " (" + s.error().hint + ")"));
    if (fi.type == FieldType::Entity || fi.type == FieldType::EntityList) st.scene.bindLinks(id);  // names -> ids
}

void iterSet(Value* R, int a, bool two, const Value& coll, size_t i) {
    if (coll.isList()) {
        if (two) {
            R[a + 2] = Value::number(static_cast<double>(i));
            R[a + 3] = coll.items()[i];
        } else {
            R[a + 2] = coll.items()[i];
        }
    } else {  // map
        const auto& e = coll.mapObj().entries[i];
        R[a + 2] = Value::string(e.first);
        if (two) R[a + 3] = e.second;
    }
}

size_t iterSize(const Value& coll) { return coll.isList() ? coll.items().size() : coll.mapObj().entries.size(); }

const BehaviorRun* behaviorRunFor(ExecState& st) {
    if (st.behavior < 0) return nullptr;
    if (st.inst && st.inst->program.get() == st.prog) return &st.inst->behaviors[st.behavior];
    // Test drivers: the instance of this program on `self`.
    const BehaviorRun* found = nullptr;
    forEachInstance(st.impl, st.scene, st.self, [&](Instance& inst) {
        if (!found && inst.program.get() == st.prog && st.behavior < static_cast<int>(inst.behaviors.size())) {
            found = &inst.behaviors[st.behavior];
        }
    });
    return found;
}

Value& varRef(ExecState& st, int var) {
    if (st.inst && st.inst->program.get() == st.prog) {
        VarTable& t = *st.inst->vars;
        return t.slots[st.inst->behaviors[st.behavior].varSlots[var]].value;
    }
    // Slow path (test drivers): by name on self.
    uint32_t sym = st.prog->behaviors[st.behavior].vars[var].sym;
    VarTable& t = varTable(st.impl, st.scene, st.self);
    int i = t.find(sym);
    if (i < 0) {
        (void)getEntityVar(st.impl, st.scene, st.self, sym);
        i = t.find(sym);
        if (i < 0) {
            t.slots.push_back(makeSlot(sym));
            i = static_cast<int>(t.slots.size() - 1);
        }
    }
    return t.slots[i].value;
}

void markVarDirty(ExecState& st, int var) {
    if (st.inst && st.inst->program.get() == st.prog) {
        VarTable& t = *st.inst->vars;
        t.slots[st.inst->behaviors[st.behavior].varSlots[var]].dirty = true;
        t.anyDirty = true;
        return;
    }
    uint32_t sym = st.prog->behaviors[st.behavior].vars[var].sym;
    VarTable& t = varTable(st.impl, st.scene, st.self);
    int i = t.find(sym);
    if (i >= 0) {
        t.slots[i].dirty = true;
        t.anyDirty = true;
    }
}

#define RK(x) (isK(x) ? K[(x)&0x7fff] : R[x])

// Single = true executes from pc until it leaves [pc, stopPc) (one instruction, or a
// straight-line range for native code); false runs until the proto ends.
template <bool Single>
bool interpret(ExecState& st, int protoIndex, Value* R, size_t& pc, Outcome& out, size_t stopPc = 0) {
    [[maybe_unused]] const size_t startPc = pc;
    const Program& prog = *st.prog;
    const Proto& P = prog.protos[protoIndex];
    const Ins* code = P.code.data();
    const Value* K = prog.constants.data();
    const SourceLoc* locs = P.locs.data();
#define LOC (locs[pc - 1])
    for (;;) {
        const Ins in = code[pc++];
        switch (in.op) {
            case Op::Nop: break;
            case Op::Move: R[in.a] = R[in.b]; break;
            case Op::LoadK:
                if (K[in.b].isNumber()) R[in.a].setNumber(K[in.b].num());
                else R[in.a] = K[in.b];
                break;
            case Op::LoadNone: R[in.a] = Value(); break;
            case Op::LoadBool: R[in.a] = Value::boolean(in.b != 0); break;
            case Op::LoadSelf: R[in.a] = Value::entity(st.self); break;
            case Op::LoadEnv:
                switch (static_cast<Env>(in.b)) {
                    case Env::Dt: R[in.a] = Value::number(st.dt); break;
                    case Env::Time: R[in.a] = Value::number(st.cosmetic ? st.displayTime : st.rt.time()); break;
                    case Env::Frame: R[in.a] = Value::number(static_cast<double>(st.rt.frame())); break;
                    case Env::Other: R[in.a] = st.other ? Value::entity(st.other) : Value(); break;
                    case Env::State: {
                        const BehaviorRun* br = behaviorRunFor(st);
                        if (br && br->state >= 0) {
                            R[in.a] = Value::string(prog.behaviors[st.behavior].states[br->state].name);
                        } else {
                            R[in.a] = Value();
                        }
                        break;
                    }
                    case Env::StateTime: {
                        const BehaviorRun* br = behaviorRunFor(st);
                        R[in.a] = Value::number(br ? br->stateTime : 0.0);
                        break;
                    }
                    case Env::ContactPoint: R[in.a] = st.contact ? Value::vec(st.contact->point) : Value(); break;
                    case Env::ContactNormal: R[in.a] = st.contact ? Value::vec(st.contact->normal) : Value(); break;
                    case Env::Impact: R[in.a] = Value::number(st.contact ? st.contact->speed : 0.0); break;
                    case Env::HitPoint: R[in.a] = st.lastHit ? Value::vec(st.lastHit->point) : Value(); break;
                    case Env::HitNormal: R[in.a] = st.lastHit ? Value::vec(st.lastHit->normal) : Value(); break;
                    case Env::HitDistance: R[in.a] = st.lastHit ? Value::number(st.lastHit->distance) : Value(); break;
                }
                break;
            case Op::GetVar:
                if (st.inst && st.inst->program.get() == st.prog) [[likely]] {
                    R[in.a] = st.inst->vars->slots[st.inst->behaviors[st.behavior].varSlots[in.b]].value;
                } else {
                    R[in.a] = varRef(st, in.b);
                }
                break;
            case Op::SetVar:
                if (st.cosmetic) [[unlikely]] cosmeticError(LOC, "change vars");
                if (st.inst && st.inst->program.get() == st.prog) [[likely]] {
                    VarTable& t = *st.inst->vars;
                    VarSlot& slot = t.slots[st.inst->behaviors[st.behavior].varSlots[in.a]];
                    slot.value = RK(in.b);
                    slot.dirty = true;
                    t.anyDirty = true;
                } else {
                    varRef(st, in.a) = RK(in.b);
                    markVarDirty(st, in.a);
                }
                break;
            case Op::GetMember: R[in.a] = getMember(st, R[in.b], prog.members[in.c], LOC); break;
            case Op::SetMember: setMember(st, R[in.a], prog.members[in.b], RK(in.c), LOC); break;
            case Op::GetField: R[in.a] = getField(st, R[in.b], prog.fields[in.c], LOC); break;
            case Op::SetField: setField(st, R[in.a], prog.fields[in.b], RK(in.c), LOC); break;
            case Op::Index: R[in.a] = indexValue(R[in.b], RK(in.c), LOC); break;
            case Op::SetIndex: setIndexValue(R[in.a], RK(in.b), RK(in.c), LOC); break;
            case Op::Add: {
                const Value& b = RK(in.b);
                const Value& c = RK(in.c);
                if (b.isNumber() && c.isNumber()) {
                    R[in.a].setNumber(b.num() + c.num());
                } else {
                    R[in.a] = arith(st.scene, Op::Add, b, c, LOC);
                }
                break;
            }
            case Op::Sub: {
                const Value& b = RK(in.b);
                const Value& c = RK(in.c);
                if (b.isNumber() && c.isNumber()) {
                    R[in.a].setNumber(b.num() - c.num());
                } else {
                    R[in.a] = arith(st.scene, Op::Sub, b, c, LOC);
                }
                break;
            }
            case Op::Mul: {
                const Value& b = RK(in.b);
                const Value& c = RK(in.c);
                if (b.isNumber() && c.isNumber()) {
                    R[in.a].setNumber(b.num() * c.num());
                } else {
                    R[in.a] = arith(st.scene, Op::Mul, b, c, LOC);
                }
                break;
            }
            case Op::Div: {
                const Value& b = RK(in.b);
                const Value& c = RK(in.c);
                if (b.isNumber() && c.isNumber() && c.num() != 0) {
                    R[in.a].setNumber(b.num() / c.num());
                } else {
                    R[in.a] = arith(st.scene, Op::Div, b, c, LOC);
                }
                break;
            }
            case Op::Mod: R[in.a] = arith(st.scene, Op::Mod, RK(in.b), RK(in.c), LOC); break;
            case Op::Neg: {
                const Value& b = R[in.b];
                if (b.isNumber()) R[in.a].setNumber(-b.num());
                else if (b.isVec()) R[in.a] = Value::vec(-b.v());
                else if (b.isBool()) R[in.a] = Value::number(b.b() ? -1 : 0);
                else raise(LOC, std::string("cannot negate a ") + typeName(b.type()));
                break;
            }
            case Op::Not: {
                bool t = truthy(st.scene, R[in.b]);
                R[in.a].setBool(in.x ? t : !t);
                break;
            }
            case Op::Eq: R[in.a] = Value::boolean(RK(in.b) == RK(in.c)); break;
            case Op::Ne: R[in.a] = Value::boolean(!(RK(in.b) == RK(in.c))); break;
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge: {
                const Value& b = RK(in.b);
                const Value& c = RK(in.c);
                bool r;
                if (b.isNumber() && c.isNumber()) {
                    double x = b.num(), y = c.num();
                    r = in.op == Op::Lt ? x < y : in.op == Op::Le ? x <= y : in.op == Op::Gt ? x > y : x >= y;
                } else {
                    r = compare(in.op, b, c, LOC);
                }
                R[in.a].setBool(r);
                break;
            }
            case Op::In: R[in.a] = Value::boolean(contains(RK(in.b), RK(in.c), LOC)); break;
            case Op::Jmp: {
                int32_t off = in.sbx();
                if (off < 0) charge(st, -off, LOC);
                pc = static_cast<size_t>(static_cast<int64_t>(pc) + off);
                break;
            }
            case Op::JmpIf:
                if (truthy(st.scene, R[in.a])) pc = static_cast<size_t>(static_cast<int64_t>(pc) + in.sbx());
                break;
            case Op::JmpIfNot:
                if (!truthy(st.scene, R[in.a])) pc = static_cast<size_t>(static_cast<int64_t>(pc) + in.sbx());
                break;
            case Op::JmpCmp: {
                const Value& b = RK(in.a);
                const Value& c = RK(in.b);
                int kind = in.x & 7;
                bool r;
                if (b.isNumber() && c.isNumber()) {
                    double x = b.num(), y = c.num();
                    switch (kind) {
                        case 0: r = x < y; break;
                        case 1: r = x <= y; break;
                        case 2: r = x > y; break;
                        case 3: r = x >= y; break;
                        case 4: r = x == y; break;
                        default: r = x != y; break;
                    }
                } else if (kind >= 4) {
                    r = (b == c) == (kind == 4);
                } else {
                    static const Op ops[] = {Op::Lt, Op::Le, Op::Gt, Op::Ge};
                    r = compare(ops[kind], b, c, LOC);
                }
                if (r != ((in.x & 8) != 0)) {
                    auto off = static_cast<int16_t>(in.c);
                    if (off < 0) charge(st, -off, LOC);
                    pc = static_cast<size_t>(static_cast<int64_t>(pc) + off);
                }
                break;
            }
            case Op::NewList: {
                std::vector<Value> items;
                items.reserve(in.c);
                for (int i = 0; i < in.c; ++i) items.push_back(R[in.b + i]);
                R[in.a] = Value::list(std::move(items));
                break;
            }
            case Op::NewMap: {
                Value m = Value::map();
                auto& mo = m.mutMap();
                for (int i = 0; i < in.c; ++i) mo.set(R[in.b + 2 * i].str(), R[in.b + 2 * i + 1]);
                R[in.a] = std::move(m);
                break;
            }
            case Op::MakeVec: {
                float c[4] = {0, 0, 0, 1};
                for (int i = 0; i < in.c && i < 4; ++i) {
                    c[i] = static_cast<float>(asNumber(R[in.b + i], LOC, "a vector component"));
                }
                R[in.a] = in.c == 4 ? Value::color({c[0], c[1], c[2], c[3]}) : Value::vec({c[0], c[1], c[2]});
                break;
            }
            case Op::Concat: {
                std::string s;
                for (int i = 0; i < in.c; ++i) s += displayValue(st.scene, R[in.b + i]);
                R[in.a] = Value::string(std::move(s));
                break;
            }
            case Op::Call: {
                const BuiltinDef& def = *prog.builtins[in.b];
                if (st.cosmetic && !cosmeticBuiltin(def)) [[unlikely]] {
                    cosmeticError(LOC, "call " + (def.hidden ? std::string("this statement") : def.name + "()") +
                                           " (only pure functions and read-only queries)");
                }
                CallContext ctx(st, def, &R[in.a], in.c, LOC);
                Value r = def.fn(ctx);
                R[in.a] = std::move(r);
                break;
            }
            case Op::CallM: {
                Value& recv = R[in.a];
                const auto& def = prog.methods[in.b][static_cast<uint32_t>(recv.type())];
                const std::string& name = prog.methodNames[in.b];
                if (!def) {
                    std::vector<std::string> names = st.rt.registry().methodNames(recv.type());
                    std::string guess = str::closest(name, names);
                    std::string hint = guess.empty() ? "" : " (did you mean '" + guess + "'?)";
                    if (recv.isNone()) hint = " (the value is none)";
                    raise(LOC, std::string("a ") + typeName(recv.type()) + " has no method '" + name + "'" + hint);
                }
                int argc = in.c;
                if (argc < def->minArgs() || (def->maxArgs() >= 0 && argc > def->maxArgs())) {
                    raise(LOC, name + "() takes " + std::to_string(def->minArgs()) +
                                   (def->maxArgs() == def->minArgs() ? "" : "-" + std::to_string(def->maxArgs())) +
                                   " argument(s) but was given " + std::to_string(argc));
                }
                CallContext ctx(st, *def, &R[in.a], argc + 1, LOC);
                Value r = def->fn(ctx);
                R[in.a + argc + 1] = std::move(r);
                break;
            }
            case Op::CallF: {
                const FnInfo& f = prog.functions[in.b];
                const Proto& callee = prog.protos[f.proto];
                charge(st, static_cast<int64_t>(callee.code.size()), LOC);
                if (st.depth + 1 > Runtime::kMaxDepth) {
                    raise(LOC, "recursion too deep: more than " + std::to_string(Runtime::kMaxDepth) +
                                   " nested calls (calling " + f.name + "())");
                }
                Value* CR = R + P.numRegs;
                if (CR + callee.numRegs > st.impl.stack.data() + st.impl.stack.size()) {
                    raise(LOC, "out of stack space calling " + f.name + "()");
                }
                for (int i = 0; i < in.c; ++i) CR[i] = std::move(R[in.a + i]);
                ++st.depth;
                Outcome o = runProto(st, f.proto, CR, 0);
                --st.depth;
                for (int i = 0; i < callee.numRegs; ++i) CR[i] = Value();
                if (o.kind != Outcome::Kind::Done) {
                    out = std::move(o);
                    return false;
                }
                R[in.a] = std::move(o.ret);
                break;
            }
            case Op::Ret:
                out.kind = Outcome::Kind::Done;
                out.ret = std::move(R[in.a]);
                return false;
            case Op::RetNone:
                out.kind = Outcome::Kind::Done;
                out.ret = Value();
                return false;
            case Op::Every:
            case Op::After: {
                if (st.cosmetic) [[unlikely]] cosmeticError(LOC, "use every/after timers");
                if (!st.inst || st.behavior < 0) raise(LOC, "timers need a running behavior");
                double interval = asNumber(RK(in.b), LOC, in.op == Op::Every ? "the every interval" : "the after delay");
                BehaviorRun& br = st.inst->behaviors[st.behavior];
                bool fire = false;
                if (in.op == Op::Every) {
                    if (interval <= 0) raise(LOC, "the every interval must be positive");
                    double& t = br.timers[in.c];
                    t += st.dt;
                    if (t >= interval) {
                        t = std::fmod(t, interval);  // at most once per tick, no burst catch-up
                        fire = true;
                    }
                } else if (!br.fired[in.c]) {
                    double& t = br.timers[in.c];
                    t += st.dt;
                    if (t >= interval) {
                        br.fired[in.c] = 1;
                        fire = true;
                    }
                }
                R[in.a] = Value::boolean(fire);
                break;
            }
            case Op::ForPrep: {
                double s = asNumber(R[in.a], LOC, "the loop start");
                double l = asNumber(R[in.a + 1], LOC, "the loop end");
                double d = asNumber(R[in.a + 2], LOC, "the loop step");
                if (d == 0) raise(LOC, "the loop step cannot be 0");
                bool run = d > 0 ? (in.x ? s <= l : s < l) : (in.x ? s >= l : s > l);
                R[in.a] = Value::number(s);
                R[in.a + 1] = Value::number(l);
                R[in.a + 2] = Value::number(d);
                if (!run) {
                    pc = static_cast<size_t>(static_cast<int64_t>(pc) + in.sbx());
                } else {
                    R[in.a + 3] = Value::number(s);
                }
                break;
            }
            case Op::ForLoop: {
                double v = R[in.a].num() + R[in.a + 2].num();
                double l = R[in.a + 1].num();
                double d = R[in.a + 2].num();
                bool run = d > 0 ? (in.x ? v <= l : v < l) : (in.x ? v >= l : v > l);
                if (run) {
                    R[in.a].setNumber(v);
                    R[in.a + 3].setNumber(v);
                    int32_t off = in.sbx();
                    charge(st, -off, LOC);
                    pc = static_cast<size_t>(static_cast<int64_t>(pc) + off);
                }
                break;
            }
            case Op::IterPrep: {
                Value& coll = R[in.a];
                if (coll.isString()) {
                    std::vector<Value> chars;
                    for (auto& ch : utf8Chars(coll.str())) chars.push_back(Value::string(std::move(ch)));
                    coll = Value::list(std::move(chars));
                }
                if (!coll.isList() && !coll.isMap()) {
                    raise(LOC, std::string("cannot loop over a ") + typeName(coll.type()) +
                                   (coll.isNone() ? " (the value is none)" : "") + "; loop over a list, map, string or range");
                }
                if (iterSize(coll) == 0) {
                    pc = static_cast<size_t>(static_cast<int64_t>(pc) + in.sbx());
                    break;
                }
                R[in.a + 1] = Value::number(0);
                iterSet(R, in.a, in.x != 0, coll, 0);
                break;
            }
            case Op::IterNext: {
                auto i = static_cast<size_t>(R[in.a + 1].num()) + 1;
                const Value& coll = R[in.a];
                if (i < iterSize(coll)) {
                    R[in.a + 1] = Value::number(static_cast<double>(i));
                    iterSet(R, in.a, in.x != 0, coll, i);
                    int32_t off = in.sbx();
                    charge(st, -off, LOC);
                    pc = static_cast<size_t>(static_cast<int64_t>(pc) + off);
                }
                break;
            }
            case Op::Wait: {
                if (st.cosmetic) [[unlikely]] cosmeticError(LOC, "wait");
                double amount = asNumber(RK(in.b), LOC, in.x ? "the frame count" : "the wait time");
                if (!std::isfinite(amount)) raise(LOC, "the wait time must be a finite number");
                out.kind = Outcome::Kind::Wait;
                out.waitFrames = in.x != 0;
                out.waitAmount = amount;
                out.pc = pc;
                return false;
            }
            case Op::GoTo:
                if (st.cosmetic) [[unlikely]] cosmeticError(LOC, "change state (go to)");
                out.kind = Outcome::Kind::GoTo;
                out.state = in.b;
                return false;
            case Op::Stop:
                out.kind = Outcome::Kind::Stop;
                return false;
            case Op::Expect: {
                if (!st.test) raise(LOC, "expect can only run inside tests");
                ++st.test->expectations;
                if (!truthy(st.scene, R[in.a])) {
                    std::string detail;
                    if (in.x) {
                        detail = "left side was " + displayValue(st.scene, R[in.c]) + ", right side was " +
                                 displayValue(st.scene, R[in.c + 1]);
                        if (R[in.c].isString()) detail = "left side was \"" + R[in.c].str() + "\", right side was " + displayValue(st.scene, R[in.c + 1]);
                    }
                    if (st.test->fail) st.test->fail(LOC, "expected " + K[in.b].str(), detail);
                }
                break;
            }
            case Op::Count: break;
        }
        if constexpr (Single) {
            if (pc >= stopPc || pc < startPc) return true;
        }
    }
#undef LOC
}

#undef RK

}  // namespace

Outcome runProto(ExecState& st, int proto, Value* regs, size_t pc) {
    Outcome out;
    if (st.native) {
        if (aotRun(st, proto, regs, pc, out)) return out;
    }
    interpret<false>(st, proto, regs, pc, out);
    return out;
}

bool execOne(ExecState& st, int proto, Value* regs, size_t& pc, Outcome& out) {
    return interpret<true>(st, proto, regs, pc, out, pc + 1);
}

bool execRange(ExecState& st, int proto, Value* regs, size_t& pc, size_t end, Outcome& out) {
    return interpret<true>(st, proto, regs, pc, out, end);
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

Runtime::Runtime(Scene& scene, const BuiltinRegistry* registry)
    : scene_(scene),
      registry_(registry ? registry : &BuiltinRegistry::global()),
      impl_(std::make_unique<Impl>()),
      rng_(scene.seed) {}

Runtime::~Runtime() = default;

void Runtime::reset(bool keepQueuedEvents) {
    rng_.reseed(scene_.seed);
    time_ = 0;
    frame_ = 0;
    realTime_ = 0;
    paused_ = false;
    timeScale_ = 1.0;
    requestedPause_.reset();
    requestedScale_.reset();
    preparedFrame_ = ~0ull;
    gate_.reset();
    impl_->cosmeticUndo.clear();
    impl_->pending.clear();
    if (!keepQueuedEvents) impl_->nextPending.clear();
    impl_->contacts.clear();
    impl_->nextContacts.clear();
    impl_->instances.clear();
    impl_->vars.clear();
    impl_->toDestroy.clear();
    impl_->revisionValid = false;
    impl_->fieldCache.clear();
    scene_.registry().each<Behavior>([](ecs::Entity, Behavior& b) {
        for (auto& s : b.scripts) s.runtimeErrors = 0;
    });
}

CompileOptions Runtime::compileOptions() const {
    CompileOptions o;
    o.registry = registry_;
    for (const auto& k : scene_.componentKinds()) {
        o.components.push_back(k.name);
        if (k.info) o.componentTypes.push_back(k.info);
    }
    if (!projectDir_.empty()) {
        Impl* impl = impl_.get();
        std::string root = projectDir_;
        o.loadModule = [impl, root](const std::string& path) -> Result<std::string> {
            auto it = impl->modules.find(path);
            if (it == impl->modules.end()) {
                Impl::ModuleFile mf;
                std::error_code ec;
                fs::path p = fs::path(root) / path;
                auto t = fs::last_write_time(p, ec);
                if (!ec) {
                    std::ifstream f(p, std::ios::binary);
                    std::ostringstream ss;
                    ss << f.rdbuf();
                    mf.text = ss.str();
                    mf.exists = static_cast<bool>(f) || !mf.text.empty();
                    mf.mtime = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
                }
                it = impl->modules.emplace(path, std::move(mf)).first;
            }
            if (!it->second.exists) {
                return Error::make("not_found", "no file " + path + " in the project",
                                   "create it (e.g. with file_write) or fix the path");
            }
            return it->second.text;
        };
    }
    return o;
}

void Runtime::setProjectDir(std::string dir) {
    projectDir_ = std::move(dir);
    impl_->modules.clear();
    ++impl_->moduleRevision;
}

bool Runtime::refreshModules() {
    bool changed = false;
    for (auto it = impl_->modules.begin(); it != impl_->modules.end();) {
        std::error_code ec;
        fs::path p = fs::path(projectDir_) / it->first;
        auto t = fs::last_write_time(p, ec);
        int64_t mtime = ec ? -1 : std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        if (mtime != it->second.mtime || (ec && it->second.exists)) {
            it = impl_->modules.erase(it);  // reloaded on next use
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) ++impl_->moduleRevision;
    return changed;
}

void Runtime::compileScripts() {
    uint64_t epoch = impl_->moduleRevision * 1000003ULL + registry_->generation();
    if (impl_->scannedBehaviors == scene_.behaviorsRevision() && impl_->scannedEpoch == epoch) return;
    impl_->scannedBehaviors = scene_.behaviorsRevision();
    impl_->scannedEpoch = epoch;
    std::optional<CompileOptions> opts;
    for (EntityId id : scene_.entities()) {
        Behavior* b = scene_.get<Behavior>(id);
        if (!b) continue;
        for (auto& s : b->scripts) {
            if (s.compiledSource == s.source && s.compiledEpoch == epoch && (s.program || s.hasErrors)) continue;
            auto& entry = impl_->compileCache[s.source];
            if (entry.epoch != epoch) {
                if (!opts) opts = compileOptions();
                entry.result = compile(s.source, *opts);
                entry.epoch = epoch;
            }
            const CompileResult& r = entry.result;
            s.compiledSource = s.source;
            s.compiledEpoch = epoch;
            s.program = r.program;
            s.hasErrors = !r.ok();
            for (const auto& d : r.diagnostics) {
                if (d.severity != Severity::Error) continue;
                impl_->messages.push_back({RuntimeMessage::Kind::Compile, id, s.name, d.loc.line, d.message, d.file});
            }
        }
    }
    if (impl_->compileCache.size() > 4096) impl_->compileCache.clear();  // bound memory in long sessions
}

void Runtime::queueContact(const Contact& contact) { impl_->nextContacts.push_back(contact); }

void Runtime::emit(std::string name, EntityId target, Value payload, EntityId other) {
    if (onEmit) onEmit(name, target, other);  // Studio hook (playtests)
    if (impl_->nextPending.size() >= kMaxPendingEvents) return;
    PendingEvent e;
    e.sym = intern(name);
    e.name = std::move(name);
    e.target = target;
    e.other = other;
    e.payload = std::move(payload);
    impl_->nextPending.push_back(std::move(e));
}

void Runtime::emitJson(std::string name, EntityId target, const Json& payload, EntityId other) {
    emit(std::move(name), target, fromJson(payload), other);
}

std::vector<RuntimeMessage> Runtime::drainMessages() {
    std::vector<RuntimeMessage> out;
    out.swap(impl_->messages);
    return out;
}

void Runtime::attachNative(uint64_t programHash, std::shared_ptr<const NativeProgram> native) {
    native_[programHash] = std::move(native);
}
void Runtime::detachNative(uint64_t programHash) { native_.erase(programHash); }
void Runtime::clearNative() { native_.clear(); }

std::string Runtime::currentState(EntityId e) const { return currentStateOf(*impl_, scene_, e); }

void Runtime::log(EntityId entity, std::string text, std::string script) {
    impl_->messages.push_back({RuntimeMessage::Kind::Log, entity, std::move(script), 0, std::move(text), {}});
}

Value Runtime::getVar(EntityId entity, std::string_view name) {
    if (!scene_.exists(entity)) return {};
    return getEntityVar(*impl_, scene_, entity, intern(name));
}

void Runtime::setVar(EntityId entity, std::string_view name, Value value) {
    if (!scene_.exists(entity)) return;
    setEntityVar(*impl_, scene_, entity, intern(name), std::move(value));
    if (!ticking_) {
        // Outside a tick, mirror right away so the scene shows it.
        if (EntityRecord* rec = scene_.record(entity)) rec->vars[std::string(name)] = toJson(getVar(entity, name));
    }
}

void Runtime::destroyEntity(EntityId entity) {
    if (!scene_.exists(entity)) return;
    if (ticking_) {
        impl_->toDestroy.push_back(entity);
    } else {
        scene_.destroy(entity);
    }
}

// --- scheduling ------------------------------------------------------------------------

namespace {

struct Scheduler {
    Runtime& rt;
    Runtime::Impl& impl;
    Scene& scene;
    float dt;
    const InputState& input;

    Script* script(const Instance& inst) {
        Behavior* b = scene.get<Behavior>(inst.entity);
        return b && inst.scriptIndex < b->scripts.size() ? &b->scripts[inst.scriptIndex] : nullptr;
    }
    // Scripts only change between ticks (tools) or when the runtime disables one after
    // repeated errors, so the full check runs once per instance per tick.
    bool alive(const Instance& inst) const { return !inst.dead; }
    void checkAlive(Instance& inst) {
        Script* s = script(inst);
        inst.dead = !(s && s->enabled && s->program == inst.program);
    }

    void report(Instance& inst, const RuntimeError& err, const std::string& file) {
        impl.messages.push_back({RuntimeMessage::Kind::Error, inst.entity, inst.scriptName, err.loc.line, err.message, file});
        if (cosmetic) {  // a failing `on frame` handler: frame handlers of this instance stop until the next play
            inst.frameFailed = true;
            impl.messages.push_back({RuntimeMessage::Kind::Error, inst.entity, inst.scriptName, err.loc.line,
                                     "on frame handlers of this script are off until the game restarts", file});
            return;
        }
        Script* s = script(inst);
        if (s && ++s->runtimeErrors >= kMaxErrorsBeforeDisable && s->enabled) {
            s->enabled = false;
            inst.dead = true;
            impl.messages.push_back({RuntimeMessage::Kind::Error, inst.entity, inst.scriptName, err.loc.line,
                                     "script disabled after repeated runtime errors", file});
        }
    }

    ExecState state(Instance& inst, int behavior, EntityId other) {
        ExecState st(rt, impl, scene);
        st.prog = inst.program.get();
        st.native = inst.native;
        st.inst = &inst;
        st.scriptName = &inst.scriptName;
        st.behavior = behavior;
        st.self = inst.entity;
        st.other = other;
        st.dt = dt;
        st.input = &input;
        st.contact = contact;
        if (cosmetic) {
            st.cosmetic = true;
            st.displayTime = displayTime;
            st.native = nullptr;  // the interpreter enforces the cosmetic rules
        }
        return st;
    }

    const Runtime::Contact* contact = nullptr;  // the contact being delivered (collide/trigger handlers)
    bool cosmetic = false;                      // running `on frame` handlers
    double displayTime = 0;

    // Runs a proto as the top frame; handles errors, waits and state changes.
    void run(Instance& inst, int behavior, int handler, int proto, std::vector<Value>* resumeRegs, size_t pc,
             Value payload, EntityId other) {
        const Program& prog = *inst.program;
        const Proto& P = prog.protos[proto];
        Value* R = impl.stack.data() + impl.stackTop;
        if (impl.stackTop + static_cast<size_t>(P.numRegs) > impl.stack.size()) return;
        size_t savedTop = impl.stackTop;
        impl.stackTop += static_cast<size_t>(P.numRegs);
        if (resumeRegs) {
            for (int i = 0; i < P.numRegs && i < static_cast<int>(resumeRegs->size()); ++i) R[i] = std::move((*resumeRegs)[i]);
        } else if (P.numParams > 0) {
            R[0] = std::move(payload);
        }
        ExecState st = state(inst, behavior, other);
        Outcome out;
        bool failed = false;
        try {
            out = runProto(st, proto, R, pc);
        } catch (const RuntimeError& err) {
            failed = true;
            report(inst, err, P.file);
        }
        if (!failed && out.kind == Outcome::Kind::Wait) {
            if (inst.coroutines.size() >= Runtime::kMaxCoroutines) {
                report(inst, RuntimeError{P.locs.empty() ? SourceLoc{} : P.locs[std::min(out.pc, P.locs.size()) - 1],
                                          "too many waiting handler runs (more than " +
                                              std::to_string(Runtime::kMaxCoroutines) + "): events arrive faster than they finish",
                                          {}},
                       P.file);
            } else {
                Coroutine co;
                co.behavior = behavior;
                co.handler = handler;
                co.state = handler >= 0 ? prog.behaviors[behavior].handlers[handler].state : -1;
                co.proto = proto;
                co.pc = out.pc;
                co.regs.resize(static_cast<size_t>(P.numRegs));
                for (int i = 0; i < P.numRegs; ++i) co.regs[i] = std::move(R[i]);
                co.byFrames = out.waitFrames;
                co.remaining = out.waitAmount;
                co.other = other;
                if (contact) co.contact = *contact;
                inst.coroutines.push_back(std::move(co));
            }
        }
        for (int i = 0; i < P.numRegs; ++i) R[i] = Value();
        impl.stackTop = savedTop;
        if (!failed && out.kind == Outcome::Kind::GoTo) transition(inst, behavior, out.state, P.file);
    }

    void invoke(Instance& inst, int behavior, int handler, Value payload = {}, EntityId other = kNoEntity) {
        if (!alive(inst)) return;
        const HandlerInfo& h = inst.program->behaviors[behavior].handlers[handler];
        run(inst, behavior, handler, h.proto, nullptr, 0, std::move(payload), other);
    }

    void transition(Instance& inst, int behavior, int newState, const std::string& file) {
        if (!alive(inst)) return;
        const BehaviorInfo& info = inst.program->behaviors[behavior];
        BehaviorRun& br = inst.behaviors[behavior];
        if (br.exiting) {
            report(inst, RuntimeError{info.states[newState].loc, "go to inside 'on exit' is ignored (the state is already changing)", {}},
                   file);
            return;
        }
        if (++inst.transitionsThisTick > kMaxTransitionsPerTick) {
            report(inst, RuntimeError{info.states[newState].loc, "too many state changes in one tick (more than " +
                                                                     std::to_string(kMaxTransitionsPerTick) +
                                                                     "): states keep switching with go to",
                                      {}},
                   file);
            return;
        }
        int old = br.state;
        // Waiting runs of the old state's handlers are cancelled.
        if (old >= 0) {
            auto ownedByOld = [&](const Coroutine& c) { return c.behavior == behavior && c.state == old; };
            std::erase_if(inst.coroutines, ownedByOld);
            if (int exitH = info.states[old].exit; exitH >= 0) {
                br.exiting = true;
                invoke(inst, behavior, exitH);
                br.exiting = false;
                if (!alive(inst)) return;
                std::erase_if(inst.coroutines, ownedByOld);
            }
        }
        br.state = newState;
        br.stateTime = 0;
        if (int enterH = info.states[newState].enter; enterH >= 0) invoke(inst, behavior, enterH);
    }

    void start(Instance& inst) {
        const Program& prog = *inst.program;
        inst.started = true;
        inst.vars = &varTable(impl, scene, inst.entity);
        inst.behaviors.assign(prog.behaviors.size(), {});
        for (size_t b = 0; b < prog.behaviors.size(); ++b) {
            const BehaviorInfo& info = prog.behaviors[b];
            BehaviorRun& br = inst.behaviors[b];
            br.timers.assign(static_cast<size_t>(info.timerCount), 0.0);
            br.fired.assign(static_cast<size_t>(info.timerCount), 0);
            for (const auto& v : info.vars) {
                VarTable& t = *inst.vars;
                int slot = t.find(v.sym);
                if (slot < 0) {
                    (void)getEntityVar(impl, scene, inst.entity, v.sym);  // imports a value set from outside
                    slot = t.find(v.sym);
                }
                if (slot < 0) {
                    // Initialize: the var did not exist on the entity.
                    Value init;
                    if (v.init >= 0) {
                        Value* R = impl.stack.data() + impl.stackTop;
                        ExecState st = state(inst, static_cast<int>(b), kNoEntity);
                        const Proto& P = prog.protos[v.init];
                        try {
                            Outcome o = runProto(st, v.init, R, 0);
                            init = std::move(o.ret);
                        } catch (const RuntimeError& err) {
                            report(inst, err, P.file);
                        }
                        for (int i = 0; i < P.numRegs; ++i) R[i] = Value();
                    }
                    setEntityVar(impl, scene, inst.entity, v.sym, std::move(init));
                    slot = t.find(v.sym);
                }
                br.varSlots.push_back(slot);
            }
        }
        for (size_t b = 0; b < prog.behaviors.size(); ++b) {
            const auto& hs = prog.behaviors[b].handlers;
            for (size_t h = 0; h < hs.size(); ++h) {
                if (hs[h].trigger == Trigger::Start && hs[h].state < 0) invoke(inst, static_cast<int>(b), static_cast<int>(h));
            }
        }
        for (size_t b = 0; b < prog.behaviors.size(); ++b) {
            if (!prog.behaviors[b].states.empty() && inst.behaviors[b].state == -1 && alive(inst)) {
                transition(inst, static_cast<int>(b), 0, std::string());
            }
        }
    }

    void resumeCoroutines(Instance& inst) {
        // Snapshot: coroutines created while resuming wait for the next tick.
        size_t n = inst.coroutines.size();
        size_t i = 0;
        while (i < n && i < inst.coroutines.size()) {
            Coroutine& c = inst.coroutines[i];
            c.remaining -= c.byFrames ? 1.0 : dt;
            if (c.remaining > 1e-9) {
                ++i;
                continue;
            }
            Coroutine co = std::move(c);
            inst.coroutines.erase(inst.coroutines.begin() + static_cast<std::ptrdiff_t>(i));
            --n;
            if (!alive(inst)) return;
            // A state change since it started cancels state-owned coroutines (erased above).
            const Runtime::Contact* saved = contact;
            contact = co.contact ? &*co.contact : nullptr;
            run(inst, co.behavior, co.handler, co.proto, &co.regs, co.pc, {}, co.other);
            contact = saved;
        }
    }

    bool busy(const Instance& inst, int behavior, int handler) const {
        for (const auto& c : inst.coroutines) {
            if (c.behavior == behavior && c.handler == handler) return true;
        }
        return false;
    }

    bool contactMatches(EntityId other, const std::string& filter) const {
        if (filter.empty()) return true;
        const EntityRecord* r = scene.record(other);
        if (!r) return false;
        if (str::lower(r->name) == str::lower(filter)) return true;
        return std::find(r->tags.begin(), r->tags.end(), filter) != r->tags.end();
    }

    template <typename Pred>
    void fire(Instance& inst, Pred matches, const Value& payload = {}, EntityId other = kNoEntity) {
        const Program& prog = *inst.program;
        for (size_t b = 0; b < prog.behaviors.size(); ++b) {
            const auto& hs = prog.behaviors[b].handlers;
            // Behavior-level handlers first, then the current state's.
            for (int pass = 0; pass < 2; ++pass) {
                for (size_t h = 0; h < hs.size(); ++h) {
                    if (!alive(inst)) return;
                    const HandlerInfo& hi = hs[h];
                    if (pass == 0 ? hi.state >= 0 : hi.state < 0) continue;
                    if (hi.state >= 0 && hi.state != inst.behaviors[b].state) continue;
                    if (!matches(hi)) continue;
                    invoke(inst, static_cast<int>(b), static_cast<int>(h), payload, other);
                }
            }
        }
    }

    void tickInstance(Instance& inst) {
        inst.transitionsThisTick = 0;
        checkAlive(inst);
        if (!alive(inst)) return;
        // Coroutines created this tick (by `on start` too) first resume next tick.
        if (!inst.started) {
            start(inst);
        } else {
            resumeCoroutines(inst);
        }
        if (!inst.deferred.empty()) {  // events that arrived while the entity was paused
            std::vector<PendingEvent> deferred = std::move(inst.deferred);
            inst.deferred.clear();
            for (const auto& ev : deferred) {
                fire(inst, [&](const HandlerInfo& h) { return h.trigger == Trigger::Event && h.sym == ev.sym; }, ev.payload,
                     ev.other);
            }
        }
        if (!inst.deferredContacts.empty()) {
            std::vector<Runtime::Contact> held = std::move(inst.deferredContacts);
            inst.deferredContacts.clear();
            for (const auto& c : held) {
                contact = &c;
                fire(inst, [&](const HandlerInfo& h) { return h.trigger == c.trigger && contactMatches(c.other, h.argument); },
                     Value(), c.other);
                contact = nullptr;
            }
        }
        for (const auto& ev : impl.pending) {
            if (ev.target != kNoEntity && ev.target != inst.entity) continue;
            fire(inst, [&](const HandlerInfo& h) { return h.trigger == Trigger::Event && h.sym == ev.sym; }, ev.payload,
                 ev.other);
        }
        // Physics contacts, filtered by the other entity's name or tag.
        Runtime::Contact probe;
        probe.self = inst.entity;
        auto [cBegin, cEnd] = std::equal_range(impl.contacts.begin(), impl.contacts.end(), probe,
                                               [](const Runtime::Contact& x, const Runtime::Contact& y) { return x.self < y.self; });
        for (auto c = cBegin; c != cEnd; ++c) {
            contact = &*c;
            fire(inst, [&](const HandlerInfo& h) { return h.trigger == c->trigger && contactMatches(c->other, h.argument); }, Value(),
                 c->other);
            contact = nullptr;
        }
        for (const auto& key : input.pressed) {
            fire(inst, [&](const HandlerInfo& h) { return h.trigger == Trigger::Key && h.argument == key; });
        }
        for (const auto& [name, action] : input.actions) {
            if (!action.pressed) continue;
            fire(inst, [&](const HandlerInfo& h) { return h.trigger == Trigger::Action && h.argument == name; });
        }
        for (EntityId clicked : input.clicked) {
            if (clicked == inst.entity) fire(inst, [](const HandlerInfo& h) { return h.trigger == Trigger::Click; });
        }
        const Program& prog = *inst.program;
        for (size_t b = 0; b < prog.behaviors.size(); ++b) {
            const auto& hs = prog.behaviors[b].handlers;
            for (int pass = 0; pass < 2; ++pass) {
                for (size_t h = 0; h < hs.size(); ++h) {
                    if (!alive(inst)) return;
                    const HandlerInfo& hi = hs[h];
                    if (hi.trigger != Trigger::Tick) continue;
                    if (pass == 0 ? hi.state >= 0 : hi.state < 0) continue;
                    if (hi.state >= 0 && hi.state != inst.behaviors[b].state) continue;
                    // A tick handler that is waiting does not start again until it finishes.
                    if (prog.protos[hi.proto].canWait && busy(inst, static_cast<int>(b), static_cast<int>(h))) continue;
                    invoke(inst, static_cast<int>(b), static_cast<int>(h));
                }
            }
            if (b < inst.behaviors.size() && inst.behaviors[b].state >= 0) inst.behaviors[b].stateTime += dt;
        }
    }

    bool handlesEvent(const Instance& inst, uint32_t sym) const {
        for (const auto& b : inst.program->behaviors) {
            for (const auto& h : b.handlers) {
                if (h.trigger == Trigger::Event && h.sym == sym) return true;
            }
        }
        return false;
    }

    /// An instance whose entity does not run this tick (process mode while the game is paused, or
    /// `disabled`): time stands still for it. It still hears `pause` / `resume`. While the GAME is paused,
    /// events it has handlers for (and the contacts of the step before the pause) wait until it runs
    /// again, so a pause is invisible to the simulation; an entity stopped by its own mode (`disabled`,
    /// `when_paused` during play) is off and drops them. Input and clicks are never for it.
    void park(Instance& inst) {
        checkAlive(inst);
        if (!alive(inst) || !inst.started) return;
        const bool hold = rt.gamePaused();
        static const uint32_t kPause = intern("pause");
        static const uint32_t kResume = intern("resume");
        constexpr size_t kMaxDeferred = 256;
        for (const auto& ev : impl.pending) {
            if (ev.target != kNoEntity && ev.target != inst.entity) continue;
            if (ev.sym == kPause || ev.sym == kResume) {
                fire(inst, [&](const HandlerInfo& h) { return h.trigger == Trigger::Event && h.sym == ev.sym; }, ev.payload,
                     ev.other);
                if (!alive(inst)) return;
                continue;
            }
            if (!hold || !handlesEvent(inst, ev.sym)) continue;
            if (inst.deferred.size() >= kMaxDeferred) inst.deferred.erase(inst.deferred.begin());  // keep the newest
            inst.deferred.push_back(ev);
        }
        // Contacts from the physics step before the pause (physics holds while paused, so no new ones).
        if (!hold) return;
        Runtime::Contact probe;
        probe.self = inst.entity;
        auto [cBegin, cEnd] = std::equal_range(impl.contacts.begin(), impl.contacts.end(), probe,
                                               [](const Runtime::Contact& x, const Runtime::Contact& y) { return x.self < y.self; });
        for (auto c = cBegin; c != cEnd && inst.deferredContacts.size() < kMaxDeferred; ++c) inst.deferredContacts.push_back(*c);
    }

    /// Runs the instance's `on frame` handlers (behavior-level, then the current state's). Returns how many ran.
    size_t frame(Instance& inst) {
        size_t runs = 0;
        const Program& prog = *inst.program;
        for (size_t b = 0; b < prog.behaviors.size(); ++b) {
            const auto& hs = prog.behaviors[b].handlers;
            for (int pass = 0; pass < 2; ++pass) {
                for (size_t h = 0; h < hs.size(); ++h) {
                    if (!alive(inst) || inst.frameFailed) return runs;
                    const HandlerInfo& hi = hs[h];
                    if (hi.trigger != Trigger::Frame) continue;
                    if (pass == 0 ? hi.state >= 0 : hi.state < 0) continue;
                    if (hi.state >= 0 && (b >= inst.behaviors.size() || hi.state != inst.behaviors[b].state)) continue;
                    invoke(inst, static_cast<int>(b), static_cast<int>(h));
                    ++runs;
                }
            }
        }
        return runs;
    }
};

}  // namespace

void Runtime::requestTimeScale(double scale) {
    if (!std::isfinite(scale)) scale = 1.0;
    requestedScale_ = std::clamp(scale, 0.0, kMaxTimeScale);
}

void Runtime::prepareTick() {
    preparedFrame_ = frame_;
    if (requestedScale_) {
        timeScale_ = *requestedScale_;
        requestedScale_.reset();
    }
    if (requestedPause_) {
        const bool p = *requestedPause_;
        requestedPause_.reset();
        if (p != paused_) {
            paused_ = p;
            emit(p ? "pause" : "resume");  // delivered this tick (also to the entities the pause stops)
        }
    }
    gate_.update(scene_, paused_, static_cast<float>(timeScale_));
}

void Runtime::tick(float dt, const InputState& input) {
    Impl& impl = *impl_;
    if (preparedFrame_ != frame_) prepareTick();  // the embedding did not (bare runtimes, tests)
    if (impl.stack.empty()) impl.stack.resize(kStackSize);
    compileScripts();
    ticking_ = true;
    impl.dt = dt;
    impl.input = &input;
    impl.pending = std::move(impl.nextPending);
    impl.nextPending.clear();
    // Contacts from the last physics step, grouped by receiver (stable: the producer's order).
    impl.contacts = std::move(impl.nextContacts);
    impl.nextContacts.clear();
    std::stable_sort(impl.contacts.begin(), impl.contacts.end(),
                     [](const Contact& x, const Contact& y) { return x.self < y.self; });
    impl.spawnedThisTick = 0;

    // Pick up var edits made between ticks (tools, agents, the inspector).
    if (impl.revisionValid && scene_.revision() != impl.lastRevision) {
        for (auto& [id, table] : impl.vars) {
            const EntityRecord* rec = scene_.record(id);
            if (!rec) continue;
            for (auto& slot : table.slots) {
                // The scene holds what we last mirrored unless someone edited it since.
                const Json* j = rec->vars.find(*slot.name);
                if (!j) {
                    if (slot.inScene) {
                        slot.value = Value();
                        slot.inScene = false;
                    }
                } else if (!slot.inScene || *j != toJson(slot.value)) {
                    slot.value = fromJson(*j);
                    slot.inScene = true;
                }
            }
        }
    }

    Scheduler sched{*this, impl, scene_, dt, input};
    // Snapshot the order: entities spawned this tick start running next tick. `process.priority`
    // reorders it (lower first; ties keep scene order, so it stays deterministic).
    std::vector<EntityId> order = scene_.entities();
    if (gate_.ordered()) {
        std::stable_sort(order.begin(), order.end(), [&](EntityId a, EntityId b) { return gate_.priority(a) < gate_.priority(b); });
    }
    for (EntityId id : order) {
        if (!scene_.exists(id) || !scene_.isActive(id)) continue;
        if (!scene_.get<Behavior>(id)) continue;
        const bool runs = gate_.runs(id);
        sched.dt = runs ? dt * gate_.scale(id) : 0.f;
        // Never hold Behavior pointers across handler runs: `spawn` may reallocate storage.
        for (size_t si = 0;; ++si) {
            Behavior* b = scene_.get<Behavior>(id);
            if (!b || si >= b->scripts.size()) break;
            const Script& script = b->scripts[si];
            if (!script.enabled || !script.program) continue;
            auto found = impl.instances.find({id, si});
            if (found == impl.instances.end()) found = impl.instances.emplace(std::make_pair(id, si), Instance{}).first;
            Instance& inst = found->second;
            if (inst.program != script.program) {  // behavior replaced (e.g. live edit): start fresh
                inst = Instance{};
                inst.program = script.program;
                inst.entity = id;
                inst.scriptIndex = si;
                inst.scriptName = script.name;
            }
            if (native_.empty()) {
                inst.native = nullptr;
            } else {
                auto nit = native_.find(inst.program->hash);
                inst.native = nit == native_.end() ? nullptr : nit->second.get();
            }
            if (runs) {
                sched.tickInstance(inst);
            } else {
                sched.park(inst);
            }
        }
    }
    for (EntityId id : impl.toDestroy) {
        if (!scene_.exists(id)) continue;
        scene_.destroy(id);
    }
    if (!impl.toDestroy.empty()) {
        std::erase_if(impl.instances, [&](const auto& kv) { return !scene_.exists(kv.first.first); });
        for (auto it = impl.vars.begin(); it != impl.vars.end();) {
            it = scene_.exists(it->first) ? std::next(it) : impl.vars.erase(it);
        }
    }
    impl.toDestroy.clear();

    mirrorDirtyVars(impl, scene_);  // so tools and agents see current values
    impl.lastRevision = scene_.revision();
    impl.revisionValid = true;
    impl.input = nullptr;
    ticking_ = false;
    time_ += static_cast<double>(dt) * (paused_ ? 0.0 : timeScale_);  // game time
    realTime_ += dt;
    ++frame_;
}

// ---------------------------------------------------------------------------
// Cosmetic `on frame` handlers
// ---------------------------------------------------------------------------

namespace {
size_t fieldBytes(FieldType t) {
    switch (t) {
        case FieldType::Float:
        case FieldType::Int: return 4;
        case FieldType::Bool: return sizeof(bool);
        case FieldType::Vec3: return sizeof(Vec3);
        case FieldType::Color: return sizeof(Vec4);
        default: return 0;  // strings, enums, JSON, vec2/vec4: through reflection
    }
}
}  // namespace

void recordCosmeticWrite(ExecState& st, EntityId e, const ComponentKind* kind, const FieldInfo* field) {
    auto& log = st.impl.cosmeticUndo;
    for (const auto& u : log) {
        if (u.entity == e && u.kind == kind && u.field == field) return;  // the first value is the one to restore
    }
    if (log.size() >= 100000) raise(SourceLoc{}, "on frame wrote too many fields in one frame (more than 100000)");
    Runtime::Impl::CosmeticUndo u;
    u.entity = e;
    u.kind = kind;
    u.field = field;
    if (!kind) {
        const Transform* t = st.scene.get<Transform>(e);
        if (!t) return;
        u.transform = *t;
    } else {
        void* c = kind->ptr ? kind->ptr(st.scene, e) : nullptr;
        const size_t n = field ? fieldBytes(field->type) : 0;
        if (c && n > 0) {
            std::memcpy(u.raw, static_cast<const char*>(c) + field->offset, n);
        } else {
            u.viaJson = true;
            u.json = kind->toJson(st.scene, e).get(field ? field->name : std::string());
        }
    }
    log.push_back(std::move(u));
}

bool Runtime::hasFrameHandlers() const {
    for (const auto& [key, inst] : impl_->instances) {
        if (inst.program && inst.program->hasFrameHandlers) return true;
    }
    return false;
}

size_t Runtime::runFrameHandlers(const FrameInfo& info) {
    Impl& impl = *impl_;
    if (ticking_ || !hasFrameHandlers()) return 0;
    if (impl.stack.empty()) impl.stack.resize(kStackSize);
    static const InputState kNoInput;
    Scheduler sched{*this, impl, scene_, info.dt, info.input ? *info.input : kNoInput};
    sched.cosmetic = true;
    sched.displayTime = info.time;
    const bool gated = preparedFrame_ != ~0ull;  // the gate is valid once a tick ran
    size_t runs = 0;
    for (EntityId id : scene_.entities()) {
        const Behavior* b = scene_.get<Behavior>(id);
        if (!b || !scene_.isActive(id)) continue;
        if (gated && !gate_.runs(id)) continue;
        sched.dt = info.dt * (gated ? gate_.scale(id) : 1.f);
        for (size_t si = 0; si < b->scripts.size(); ++si) {
            auto found = impl.instances.find({id, si});
            if (found == impl.instances.end()) continue;
            Instance& inst = found->second;
            if (!inst.started || inst.dead || inst.frameFailed || !inst.program || !inst.program->hasFrameHandlers) continue;
            if (b->scripts[si].program != inst.program || !b->scripts[si].enabled) continue;
            runs += sched.frame(inst);
            b = scene_.get<Behavior>(id);  // never hold component pointers across handler runs
            if (!b) break;
        }
    }
    return runs;
}

void Runtime::revertFrame() {
    auto& log = impl_->cosmeticUndo;
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (!scene_.exists(it->entity)) continue;
        if (!it->kind) {
            if (Transform* t = scene_.get<Transform>(it->entity)) *t = it->transform;
            continue;
        }
        if (it->viaJson) {
            (void)it->kind->apply(scene_, it->entity, Json::object({{it->field->name, it->json}}));
            continue;
        }
        if (void* c = it->kind->ptr ? it->kind->ptr(scene_, it->entity) : nullptr) {
            std::memcpy(static_cast<char*>(c) + it->field->offset, it->raw, fieldBytes(it->field->type));
        }
    }
    log.clear();
}

Json Runtime::inspect(EntityId e) const {
    Json out = Json::array();
    forEachInstance(*impl_, scene_, e, [&](const Instance& inst) {
        if (!inst.program) return;
        Json behaviors = Json::array();
        for (size_t b = 0; b < inst.program->behaviors.size(); ++b) {
            const BehaviorInfo& info = inst.program->behaviors[b];
            Json j = Json::object({{"name", info.name}});
            if (b < inst.behaviors.size()) {
                const BehaviorRun& br = inst.behaviors[b];
                if (br.state >= 0) {
                    j["state"] = info.states[br.state].name;
                    j["state_time"] = br.stateTime;
                }
                Json waits = Json::array();
                for (const auto& c : inst.coroutines) {
                    if (c.behavior != static_cast<int>(b)) continue;
                    const Proto& p = inst.program->protos[c.proto];
                    int line = c.pc > 0 && c.pc <= p.locs.size() ? p.locs[c.pc - 1].line : 0;
                    waits.push(Json::object({{"handler", p.name},
                                             {"line", line},
                                             {"remaining", c.remaining},
                                             {"unit", c.byFrames ? "frames" : "seconds"}}));
                }
                if (waits.size()) j["waiting"] = waits;
            }
            behaviors.push(j);
        }
        out.push(Json::object({{"script", inst.scriptName}, {"started", inst.started}, {"behaviors", behaviors}}));
    });
    return out;
}

// --- tests ------------------------------------------------------------------------------

namespace {

void driveTest(Runtime& rt, Runtime::Impl& impl, Scene& scene, Runtime::TestDriver& d, float dt) {
    const Proto& P = d.program->protos[d.proto];
    if (impl.stack.empty()) impl.stack.resize(kStackSize);
    Value* R = impl.stack.data() + impl.stackTop;
    size_t savedTop = impl.stackTop;
    impl.stackTop += static_cast<size_t>(P.numRegs);
    for (int i = 0; i < P.numRegs && i < static_cast<int>(d.regs.size()); ++i) R[i] = std::move(d.regs[i]);
    ExecState st(rt, impl, scene);
    st.prog = d.program.get();
    static const std::string kTestScript = "test";
    st.behavior = P.behavior;
    st.self = d.self;
    st.dt = dt;
    st.test = d.hooks;
    st.scriptName = &kTestScript;
    Outcome out;
    try {
        out = runProto(st, d.proto, R, d.pc);
    } catch (const RuntimeError& err) {
        out.kind = Outcome::Kind::Stop;
        if (d.hooks && d.hooks->fail) d.hooks->fail(err.loc, "runtime error in the test: " + err.message, "");
    }
    d.regs.assign(static_cast<size_t>(P.numRegs), Value());
    if (out.kind == Outcome::Kind::Wait) {
        for (int i = 0; i < P.numRegs; ++i) d.regs[i] = std::move(R[i]);
        d.pc = out.pc;
        d.byFrames = out.waitFrames;
        d.remaining = out.waitAmount;
    } else {
        d.finished = true;
    }
    for (int i = 0; i < P.numRegs; ++i) R[i] = Value();
    impl.stackTop = savedTop;
}

}  // namespace

std::unique_ptr<Runtime::TestDriver> Runtime::startTest(std::shared_ptr<const Program> program, int proto, EntityId self,
                                                        TestHooks* hooks) {
    auto d = std::make_unique<TestDriver>();
    d->program = std::move(program);
    d->proto = proto;
    d->self = self;
    d->hooks = hooks;
    driveTest(*this, *impl_, scene_, *d, impl_->dt);
    return d;
}

bool Runtime::stepTest(TestDriver& d, float dt) {
    if (d.finished) return true;
    d.remaining -= d.byFrames ? 1.0 : dt;
    if (d.remaining > 1e-9) return false;
    driveTest(*this, *impl_, scene_, d, dt);
    return d.finished;
}

bool Runtime::testFinished(const TestDriver& d) const { return d.finished; }

}  // namespace sky::wander
