#include "skywalker/wander/Runtime.h"

#include <cmath>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/wander/Compiler.h"

namespace sky::wander {

Json RuntimeMessage::toJson() const {
    const char* k = kind == Kind::Log ? "log" : kind == Kind::Error ? "runtime_error" : "compile_error";
    return Json::object({{"kind", k}, {"entity", entity}, {"script", script}, {"line", line}, {"text", text}});
}

namespace {

struct Value {
    enum class Type { None, Number, Bool, String, Vec, Color, Entity };
    Type type = Type::None;
    double n = 0;
    std::string s;
    Vec3 v;
    Vec4 c;
    EntityId e = kNoEntity;

    static Value number(double x) { Value r; r.type = Type::Number; r.n = x; return r; }
    static Value boolean(bool b) { Value r; r.type = Type::Bool; r.n = b ? 1 : 0; return r; }
    static Value string(std::string x) { Value r; r.type = Type::String; r.s = std::move(x); return r; }
    static Value vec(Vec3 x) { Value r; r.type = Type::Vec; r.v = x; return r; }
    static Value color(Vec4 x) { Value r; r.type = Type::Color; r.c = x; return r; }
    static Value entity(EntityId x) { Value r; r.type = Type::Entity; r.e = x; return r; }
};

const char* typeName(Value::Type t) {
    switch (t) {
        case Value::Type::None: return "none";
        case Value::Type::Number: return "number";
        case Value::Type::Bool: return "boolean";
        case Value::Type::String: return "string";
        case Value::Type::Vec: return "vector";
        case Value::Type::Color: return "color";
        case Value::Type::Entity: return "entity";
    }
    return "?";
}

std::string fmt(double d) {
    std::ostringstream os;
    os << d;
    return os.str();
}

struct RuntimeError {
    SourceLoc loc;
    std::string message;
};
struct StopSignal {};

}  // namespace

// Executes handlers for one entity/script pair. Errors are thrown internally as
// RuntimeError and caught at the handler boundary (exceptions never escape the runtime).
class Exec {
public:
    // The script is addressed by index, never by reference: `spawn` can add Behavior
    // components mid-handler and reallocate their storage.
    Exec(Runtime& rt, EntityId self, size_t scriptIndex, Runtime::Instance& inst, float dt, const InputState& input)
        : rt_(rt), scene_(rt.scene_), self_(self), scriptIndex_(scriptIndex), inst_(inst), dt_(dt), input_(input) {
        if (Script* s = script()) scriptName_ = s->name;
    }

    void run(const Block& body) {
        locals_.clear();
        locals_.emplace_back();
        budget_ = Runtime::kBudget;
        try {
            block(body);
        } catch (const StopSignal&) {
        } catch (const RuntimeError& err) {
            rt_.messages_.push_back({RuntimeMessage::Kind::Error, self_, scriptName_, err.loc.line, err.message});
            Script* s = script();
            if (s && ++s->runtimeErrors >= 5) {
                s->enabled = false;
                rt_.messages_.push_back({RuntimeMessage::Kind::Error, self_, scriptName_, err.loc.line,
                                         "script disabled after repeated runtime errors"});
            }
        }
    }

    void initVars(const BehaviorDef& b) {
        locals_.assign(1, {});
        budget_ = Runtime::kBudget;
        EntityRecord* rec = scene_.record(self_);
        if (!rec) return;
        for (const auto& v : b.vars) {
            if (rec->vars.contains(v.name) || !v.initial) continue;
            try {
                rec->vars[v.name] = toJson(eval(*v.initial));
            } catch (const RuntimeError& err) {
                rt_.messages_.push_back({RuntimeMessage::Kind::Error, self_, scriptName_, err.loc.line, err.message});
            }
        }
    }

private:
    [[noreturn]] void fail(SourceLoc loc, std::string msg) { throw RuntimeError{loc, std::move(msg)}; }

    void spend(SourceLoc loc) {
        if (--budget_ < 0) fail(loc, "execution budget exceeded (too much work in one handler)");
    }

    // --- conversions -----------------------------------------------------------
    static Json toJson(const Value& v) {
        switch (v.type) {
            case Value::Type::None: return {};
            case Value::Type::Number: return v.n;
            case Value::Type::Bool: return v.n != 0;
            case Value::Type::String: return v.s;
            case Value::Type::Vec: return reflect::vec3ToJson(v.v);
            case Value::Type::Color: return reflect::colorToJson(v.c);
            case Value::Type::Entity: return Json::object({{"$entity", v.e}});
        }
        return {};
    }

    static Value fromJson(const Json& j) {
        switch (j.type()) {
            case Json::Type::Number: return Value::number(j.asNumber());
            case Json::Type::Bool: return Value::boolean(j.asBool());
            case Json::Type::String: {
                Vec4 c;
                if (j.asString().size() > 1 && j.asString()[0] == '#' && reflect::parseHexColor(j.asString(), c)) {
                    return Value::color(c);
                }
                return Value::string(j.asString());
            }
            case Json::Type::Array: {
                Vec3 v;
                if (reflect::jsonToVec3(j, v)) return Value::vec(v);
                Vec4 c;
                if (reflect::jsonToColor(j, c)) return Value::color(c);
                return {};
            }
            case Json::Type::Object:
                if (const Json* e = j.find("$entity")) return Value::entity(static_cast<EntityId>(e->asInt()));
                return {};
            case Json::Type::Null: return {};
        }
        return {};
    }

    bool truthy(const Value& v) const {
        switch (v.type) {
            case Value::Type::None: return false;
            case Value::Type::Number:
            case Value::Type::Bool: return v.n != 0;
            case Value::Type::String: return !v.s.empty();
            case Value::Type::Entity: return scene_.exists(v.e);
            default: return true;
        }
    }

    std::string toText(const Value& v) const {
        switch (v.type) {
            case Value::Type::None: return "none";
            case Value::Type::Number: return fmt(v.n);
            case Value::Type::Bool: return v.n != 0 ? "true" : "false";
            case Value::Type::String: return v.s;
            case Value::Type::Vec: return "(" + fmt(v.v.x) + ", " + fmt(v.v.y) + ", " + fmt(v.v.z) + ")";
            case Value::Type::Color: return reflect::toHexColor(v.c);
            case Value::Type::Entity: {
                const EntityRecord* r = scene_.record(v.e);
                return r ? r->name : "<destroyed entity>";
            }
        }
        return "";
    }

    double num(const Value& v, SourceLoc loc, std::string_view what) {
        if (v.type == Value::Type::Number || v.type == Value::Type::Bool) return v.n;
        fail(loc, std::string(what) + " must be a number, got " + typeName(v.type));
    }

    Vec3 vec(const Value& v, SourceLoc loc, std::string_view what) {
        if (v.type == Value::Type::Vec) return v.v;
        if (v.type == Value::Type::Entity) return worldPos(entity(v, loc, what));
        fail(loc, std::string(what) + " must be a vector (x, y, z) or an entity, got " + typeName(v.type));
    }

    EntityId entity(const Value& v, SourceLoc loc, std::string_view what) {
        if (v.type != Value::Type::Entity) fail(loc, std::string(what) + " must be an entity, got " + typeName(v.type));
        if (!scene_.exists(v.e)) fail(loc, std::string(what) + " refers to an entity that no longer exists");
        return v.e;
    }

    Vec3 worldPos(EntityId id) { return scene_.worldMatrix(id).translation(); }

    // --- statements --------------------------------------------------------------
    void block(const Block& b) {
        locals_.emplace_back();
        for (const auto& s : b) stmt(*s);
        locals_.pop_back();
    }

    Value* findLocal(const std::string& name) {
        for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    void stmt(const Stmt& s) {
        spend(s.loc);
        switch (s.kind) {
            case Stmt::Kind::Let: locals_.back()[s.name] = eval(*s.value); break;
            case Stmt::Kind::Assign: assign(*s.target, eval(*s.value)); break;
            case Stmt::Kind::If:
                for (const auto& [cond, body] : s.branches) {
                    if (!cond || truthy(eval(*cond))) {
                        block(body);
                        break;
                    }
                }
                break;
            case Stmt::Kind::Every: {
                double interval = num(eval(*s.value), s.loc, "every interval");
                if (interval <= 0) fail(s.loc, "every interval must be positive");
                double& t = inst_.timers[s.nodeId];
                t += dt_;
                if (t >= interval) {
                    t = std::fmod(t, interval);  // at most once per tick, no burst catch-up
                    block(s.body);
                }
                break;
            }
            case Stmt::Kind::After: {
                if (inst_.fired.count(s.nodeId)) break;
                double delay = num(eval(*s.value), s.loc, "after delay");
                double& t = inst_.timers[s.nodeId];
                t += dt_;
                if (t >= delay) {
                    inst_.fired.insert(s.nodeId);
                    block(s.body);
                }
                break;
            }
            case Stmt::Kind::Repeat: {
                double n = num(eval(*s.value), s.loc, "repeat count");
                int count = static_cast<int>(std::clamp(n, 0.0, 1000.0));
                for (int i = 0; i < count; ++i) block(s.body);
                break;
            }
            case Stmt::Kind::Move: {
                EntityId id = entity(eval(*s.target), s.loc, "move target");
                Vec3 delta = vec(eval(*s.value), s.loc, "move offset");
                if (auto* t = scene_.get<Transform>(id)) t->position += delta;
                scene_.markDirty();
                break;
            }
            case Stmt::Kind::MoveToward: {
                EntityId id = entity(eval(*s.target), s.loc, "move target");
                Vec3 goal = vec(eval(*s.value), s.loc, "move destination");
                double speed = num(eval(*s.extra), s.loc, "move speed");
                if (auto* t = scene_.get<Transform>(id)) {
                    Vec3 from = worldPos(id);
                    Vec3 delta = goal - from;
                    float dist = length(delta);
                    float step = static_cast<float>(speed) * dt_;
                    if (dist > 1e-6f) t->position += delta * (std::min(step, dist) / dist);
                }
                scene_.markDirty();
                break;
            }
            case Stmt::Kind::Rotate: {
                EntityId id = entity(eval(*s.target), s.loc, "rotate target");
                Vec3 delta = vec(eval(*s.value), s.loc, "rotation");
                if (auto* t = scene_.get<Transform>(id)) {
                    t->rotation += delta;
                    // Keep angles bounded so long sessions don't lose float precision.
                    for (float* a : {&t->rotation.x, &t->rotation.y, &t->rotation.z}) *a = std::remainder(*a, 360.f);
                }
                scene_.markDirty();
                break;
            }
            case Stmt::Kind::Look: {
                EntityId id = entity(eval(*s.target), s.loc, "look target");
                Vec3 at = vec(eval(*s.value), s.loc, "look point");
                Vec3 d = at - worldPos(id);
                if (auto* t = scene_.get<Transform>(id); t && length(d) > 1e-6f) {
                    float horizontal = std::sqrt(d.x * d.x + d.z * d.z);
                    t->rotation = {degrees(std::atan2(d.y, horizontal)), degrees(std::atan2(-d.x, -d.z)), 0.f};
                }
                scene_.markDirty();
                break;
            }
            case Stmt::Kind::Emit: {
                EntityId target = s.extra ? entity(eval(*s.extra), s.loc, "emit receiver") : kNoEntity;
                rt_.nextPending_.push_back({s.name, target});
                break;
            }
            case Stmt::Kind::Destroy: {
                Value v = eval(*s.target);
                if (v.type == Value::Type::Entity && scene_.exists(v.e)) rt_.toDestroy_.push_back(v.e);
                break;
            }
            case Stmt::Kind::Log:
                rt_.messages_.push_back({RuntimeMessage::Kind::Log, self_, scriptName_, s.loc.line, toText(eval(*s.value))});
                break;
            case Stmt::Kind::Stop: throw StopSignal{};
            case Stmt::Kind::Call: (void)eval(*s.value); break;
        }
    }

    // --- property access -----------------------------------------------------------
    static void flatten(const Expr& e, const Expr*& base, std::vector<std::string>& path) {
        if (e.kind == Expr::Kind::Member) {
            flatten(*e.lhs, base, path);
            path.push_back(e.text);
        } else {
            base = &e;
        }
    }

    Value getProp(EntityId id, const std::string& name, SourceLoc loc) {
        EntityRecord* rec = scene_.record(id);
        if (name == "position" || name == "rotation" || name == "scale") {
            auto* t = scene_.get<Transform>(id);
            return Value::vec(name == "position" ? t->position : name == "rotation" ? t->rotation : t->scale);
        }
        if (name == "color") {
            auto* m = scene_.get<MeshRenderer>(id);
            if (m) return Value::color(m->color);
            if (auto* l = scene_.get<Light>(id)) return Value::color(l->color);
            return {};
        }
        if (name == "name") return Value::string(rec->name);
        if (name == "id") return Value::number(static_cast<double>(id));
        if (name == "enabled") return Value::boolean(rec->enabled);
        if (const ComponentKind* k = scene_.componentKind(name)) {
            (void)k;
            fail(loc, "'" + name + "' is a component; access a field like ." + name + ".<field>");
        }
        if (const Json* v = rec->vars.find(name)) return fromJson(*v);
        return {};
    }

    Value getComponentField(EntityId id, const std::string& comp, const std::string& field, SourceLoc loc) {
        const ComponentKind* k = scene_.componentKind(comp);
        if (!k->has(scene_, id)) return {};
        const FieldInfo* f = k->info->field(field);
        if (!f) {
            std::string guess = str::closest(field, k->info->fieldNames());
            fail(loc, comp + " has no field '" + field + "'" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
        }
        return fromJson(k->toJson(scene_, id).get(field));
    }

    static Value swizzle(const Value& v, const std::string& name, SourceLoc loc, Exec& ex) {
        if (v.type == Value::Type::Vec) {
            if (name == "x") return Value::number(v.v.x);
            if (name == "y") return Value::number(v.v.y);
            if (name == "z") return Value::number(v.v.z);
        }
        if (v.type == Value::Type::Color) {
            if (name == "r") return Value::number(v.c.x);
            if (name == "g") return Value::number(v.c.y);
            if (name == "b") return Value::number(v.c.z);
            if (name == "a") return Value::number(v.c.w);
        }
        ex.fail(loc, std::string("cannot read '.") + name + "' of a " + typeName(v.type));
    }

    Value member(const Expr& e) {
        const Expr* base = nullptr;
        std::vector<std::string> path;
        flatten(e, base, path);
        Value cur = eval(*base);
        for (size_t i = 0; i < path.size(); ++i) {
            if (cur.type == Value::Type::Entity) {
                EntityId id = entity(cur, e.loc, "property owner");
                if (scene_.componentKind(path[i]) && i + 1 < path.size()) {
                    cur = getComponentField(id, path[i], path[i + 1], e.loc);
                    ++i;
                } else {
                    cur = getProp(id, path[i], e.loc);
                }
            } else {
                cur = swizzle(cur, path[i], e.loc, *this);
            }
        }
        return cur;
    }

    static bool setSwizzle(Value& target, const std::string& name, double x) {
        auto f = static_cast<float>(x);
        if (target.type == Value::Type::Vec) {
            if (name == "x") { target.v.x = f; return true; }
            if (name == "y") { target.v.y = f; return true; }
            if (name == "z") { target.v.z = f; return true; }
        }
        if (target.type == Value::Type::Color) {
            if (name == "r") { target.c.x = f; return true; }
            if (name == "g") { target.c.y = f; return true; }
            if (name == "b") { target.c.z = f; return true; }
            if (name == "a") { target.c.w = f; return true; }
        }
        return false;
    }

    void setProp(EntityId id, const std::vector<std::string>& path, size_t start, const Value& value, SourceLoc loc) {
        const std::string& name = path[start];
        size_t rest = path.size() - start - 1;
        scene_.markDirty();
        if (scene_.componentKind(name)) {
            if (rest != 1) fail(loc, "assign a component field, e.g. self." + name + ".<field> = ...");
            const ComponentKind* k = scene_.componentKind(name);
            if (!k->info->field(path[start + 1])) {
                std::string guess = str::closest(path[start + 1], k->info->fieldNames());
                fail(loc, name + " has no field '" + path[start + 1] + "'" +
                              (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
            }
            Status st = k->apply(scene_, id, Json::object({{path[start + 1], toJson(value)}}));
            if (!st) fail(loc, st.error().message);
            return;
        }
        Value current = rest ? getProp(id, name, loc) : Value{};
        Value next = value;
        if (rest == 1) {
            current = getProp(id, name, loc);
            if (!setSwizzle(current, path[start + 1], num(value, loc, "component value"))) {
                fail(loc, "cannot set '." + path[start + 1] + "' on " + name + " (a " + typeName(current.type) + ")");
            }
            next = current;
        } else if (rest > 1) {
            fail(loc, "property path is too deep");
        }
        if (name == "position" || name == "rotation" || name == "scale") {
            Vec3 v = vec(next, loc, name);
            auto* t = scene_.get<Transform>(id);
            (name == "position" ? t->position : name == "rotation" ? t->rotation : t->scale) = v;
            return;
        }
        if (name == "color") {
            if (next.type != Value::Type::Color) fail(loc, "color must be a color like #ff8800");
            scene_.add<MeshRenderer>(id).color = next.c;
            return;
        }
        if (name == "name") {
            scene_.record(id)->name = toText(next);
            return;
        }
        if (name == "enabled") {
            scene_.record(id)->enabled = truthy(next);
            return;
        }
        if (name == "id") fail(loc, "id is read-only");
        scene_.record(id)->vars[name] = toJson(next);
    }

    void assign(const Expr& target, const Value& value) {
        if (target.kind == Expr::Kind::Ident) {
            if (Value* local = findLocal(target.text)) {
                *local = value;
                return;
            }
            fail(target.loc, "unknown variable '" + target.text + "'");
        }
        const Expr* base = nullptr;
        std::vector<std::string> path;
        flatten(target, base, path);
        if (base->kind == Expr::Kind::Ident) {
            if (Value* local = findLocal(base->text); local && local->type != Value::Type::Entity) {
                if (path.size() != 1 || !setSwizzle(*local, path[0], num(value, target.loc, "component value"))) {
                    fail(target.loc, "cannot assign that property of local '" + base->text + "'");
                }
                return;
            }
        }
        Value owner = eval(*base);
        // Walk through entity-valued vars (e.g. self.target.position = ...).
        size_t i = 0;
        EntityId id = entity(owner, target.loc, "assignment target");
        while (i + 1 < path.size() && !scene_.componentKind(path[i])) {
            Value v = getProp(id, path[i], target.loc);
            if (v.type != Value::Type::Entity) break;
            id = entity(v, target.loc, path[i]);
            ++i;
        }
        setProp(id, path, i, value, target.loc);
    }

    // --- expressions -------------------------------------------------------------
    Value eval(const Expr& e) {
        spend(e.loc);
        switch (e.kind) {
            case Expr::Kind::Number: return Value::number(e.number);
            case Expr::Kind::String: return Value::string(e.text);
            case Expr::Kind::Bool: return Value::boolean(e.number != 0);
            case Expr::Kind::None: return {};
            case Expr::Kind::Color: return Value::color(e.color);
            case Expr::Kind::Vector: {
                float c[4] = {0, 0, 0, 1};
                for (size_t i = 0; i < e.args.size() && i < 4; ++i) {
                    c[i] = static_cast<float>(num(eval(*e.args[i]), e.args[i]->loc, "vector component"));
                }
                if (e.args.size() == 4) return Value::color({c[0], c[1], c[2], c[3]});
                return Value::vec({c[0], c[1], c[2]});
            }
            case Expr::Kind::Ident: {
                if (Value* local = findLocal(e.text)) return *local;
                if (e.text == "self") return Value::entity(self_);
                if (e.text == "dt") return Value::number(dt_);
                if (e.text == "time") return Value::number(rt_.time_);
                if (e.text == "frame") return Value::number(static_cast<double>(rt_.frame_));
                if (e.text == "pi") return Value::number(kPi);
                fail(e.loc, "unknown name '" + e.text + "'");
            }
            case Expr::Kind::Member: return member(e);
            case Expr::Kind::Call: return call(e);
            case Expr::Kind::Unary: {
                Value v = eval(*e.lhs);
                if (e.text == "not") return Value::boolean(!truthy(v));
                if (v.type == Value::Type::Vec) return Value::vec(-v.v);
                return Value::number(-num(v, e.loc, "negation operand"));
            }
            case Expr::Kind::Binary: return binary(e);
        }
        return {};
    }

    Value binary(const Expr& e) {
        const std::string& op = e.text;
        if (op == "and") {
            Value l = eval(*e.lhs);
            return truthy(l) ? Value::boolean(truthy(eval(*e.rhs))) : Value::boolean(false);
        }
        if (op == "or") {
            Value l = eval(*e.lhs);
            return truthy(l) ? Value::boolean(true) : Value::boolean(truthy(eval(*e.rhs)));
        }
        Value a = eval(*e.lhs);
        Value b = eval(*e.rhs);
        using T = Value::Type;
        if (op == "==" || op == "!=") {
            bool eq = false;
            if ((a.type == T::Number || a.type == T::Bool) && (b.type == T::Number || b.type == T::Bool)) eq = a.n == b.n;
            else if (a.type != b.type) eq = false;
            else if (a.type == T::None) eq = true;
            else if (a.type == T::String) eq = a.s == b.s;
            else if (a.type == T::Vec) eq = a.v == b.v;
            else if (a.type == T::Color) eq = a.c == b.c;
            else if (a.type == T::Entity) eq = a.e == b.e;
            return Value::boolean(op == "==" ? eq : !eq);
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            double x = num(a, e.loc, "left side of " + op), y = num(b, e.loc, "right side of " + op);
            bool r = op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
            return Value::boolean(r);
        }
        if (op == "+" && (a.type == T::String || b.type == T::String)) return Value::string(toText(a) + toText(b));
        if (a.type == T::Vec && b.type == T::Vec) {
            if (op == "+") return Value::vec(a.v + b.v);
            if (op == "-") return Value::vec(a.v - b.v);
            if (op == "*") return Value::vec(a.v * b.v);
        }
        if (a.type == T::Vec && (b.type == T::Number)) {
            if (op == "*") return Value::vec(a.v * static_cast<float>(b.n));
            if (op == "/") {
                if (b.n == 0) fail(e.loc, "division by zero");
                return Value::vec(a.v / static_cast<float>(b.n));
            }
        }
        if (a.type == T::Number && b.type == T::Vec && op == "*") return Value::vec(b.v * static_cast<float>(a.n));
        if (a.type == T::Color && b.type == T::Number && op == "*") {
            auto f = static_cast<float>(b.n);
            return Value::color({a.c.x * f, a.c.y * f, a.c.z * f, a.c.w});
        }
        if (a.type == T::Color && b.type == T::Color && op == "+") {
            return Value::color({a.c.x + b.c.x, a.c.y + b.c.y, a.c.z + b.c.z, std::max(a.c.w, b.c.w)});
        }
        if ((a.type == T::Number || a.type == T::Bool) && (b.type == T::Number || b.type == T::Bool)) {
            if (op == "+") return Value::number(a.n + b.n);
            if (op == "-") return Value::number(a.n - b.n);
            if (op == "*") return Value::number(a.n * b.n);
            if (op == "/") {
                if (b.n == 0) fail(e.loc, "division by zero");
                return Value::number(a.n / b.n);
            }
            if (op == "%") {
                if (b.n == 0) fail(e.loc, "modulo by zero");
                return Value::number(std::fmod(a.n, b.n));
            }
        }
        fail(e.loc, std::string("cannot apply '") + op + "' to " + typeName(a.type) + " and " + typeName(b.type));
    }

    Value call(const Expr& e) {
        std::vector<Value> a;
        a.reserve(e.args.size());
        for (const auto& arg : e.args) a.push_back(eval(*arg));
        const std::string& f = e.text;
        auto n = [&](size_t i) { return num(a[i], e.args[i]->loc, f + "() argument " + std::to_string(i + 1)); };
        auto v = [&](size_t i) { return vec(a[i], e.args[i]->loc, f + "() argument " + std::to_string(i + 1)); };
        auto s = [&](size_t i) {
            if (a[i].type != Value::Type::String) fail(e.args[i]->loc, f + "() expects a string, got " + typeName(a[i].type));
            return a[i].s;
        };

        if (f == "find") {
            EntityId id = scene_.find(s(0));
            return id ? Value::entity(id) : Value{};
        }
        if (f == "nearest") {
            Vec3 me = worldPos(self_);
            EntityId best = kNoEntity;
            float bestD = 1e30f;
            for (EntityId id : scene_.findTagged(s(0))) {
                if (id == self_ || !scene_.isActive(id)) continue;
                float d = distance(me, worldPos(id));
                if (d < bestD) {
                    bestD = d;
                    best = id;
                }
            }
            return best ? Value::entity(best) : Value{};
        }
        if (f == "count") return Value::number(static_cast<double>(scene_.findTagged(s(0)).size()));
        if (f == "tagged") {
            if (a[0].type != Value::Type::Entity || !scene_.exists(a[0].e)) return Value::boolean(false);
            const auto& tags = scene_.record(a[0].e)->tags;
            return Value::boolean(std::find(tags.begin(), tags.end(), s(1)) != tags.end());
        }
        if (f == "exists") return Value::boolean(a[0].type == Value::Type::Entity && scene_.exists(a[0].e));
        if (f == "distance") return Value::number(distance(v(0), v(1)));
        if (f == "direction") return Value::vec(normalize(v(1) - v(0)));
        if (f == "forward") {
            EntityId id = entity(a[0], e.args[0]->loc, "forward() argument");
            return Value::vec(normalize(scene_.worldMatrix(id).transformDir({0, 0, -1})));
        }
        if (f == "length") return Value::number(length(v(0)));
        if (f == "normalize") return Value::vec(normalize(v(0)));
        if (f == "dot") return Value::number(dot(v(0), v(1)));
        if (f == "cross") return Value::vec(cross(v(0), v(1)));
        if (f == "vec") return Value::vec({static_cast<float>(n(0)), static_cast<float>(n(1)), static_cast<float>(n(2))});
        if (f == "color") {
            return Value::color({static_cast<float>(n(0)), static_cast<float>(n(1)), static_cast<float>(n(2)),
                                 a.size() > 3 ? static_cast<float>(n(3)) : 1.f});
        }
        if (f == "sin") return Value::number(std::sin(n(0)));
        if (f == "cos") return Value::number(std::cos(n(0)));
        if (f == "tan") return Value::number(std::tan(n(0)));
        if (f == "abs") return Value::number(std::fabs(n(0)));
        if (f == "sqrt") return Value::number(std::sqrt(std::max(0.0, n(0))));
        if (f == "floor") return Value::number(std::floor(n(0)));
        if (f == "ceil") return Value::number(std::ceil(n(0)));
        if (f == "round") return Value::number(std::round(n(0)));
        if (f == "sign") return Value::number(n(0) > 0 ? 1 : n(0) < 0 ? -1 : 0);
        if (f == "min") return Value::number(std::min(n(0), n(1)));
        if (f == "max") return Value::number(std::max(n(0), n(1)));
        if (f == "clamp") return Value::number(std::clamp(n(0), std::min(n(1), n(2)), std::max(n(1), n(2))));
        if (f == "lerp") {
            double t = n(2);
            if (a[0].type == Value::Type::Vec || a[1].type == Value::Type::Vec) {
                return Value::vec(lerp(v(0), v(1), static_cast<float>(t)));
            }
            return Value::number(n(0) + (n(1) - n(0)) * t);
        }
        if (f == "random") {
            double r = rt_.rng_.nextFloat();
            if (a.empty()) return Value::number(r);
            if (a.size() == 1) return Value::number(r * n(0));
            return Value::number(n(0) + (n(1) - n(0)) * r);
        }
        if (f == "chance") return Value::boolean(rt_.rng_.nextFloat() < n(0));
        if (f == "key") return Value::boolean(input_.held.count(str::lower(s(0))) != 0);
        if (f == "burst") {
            // burst(n) from self's particles, or burst(entity, n)
            EntityId target = self_;
            double count = 0;
            if (a.size() == 2) {
                target = entity(a[0], e.args[0]->loc, "burst() argument 1");
                count = n(1);
            } else {
                count = n(0);
            }
            if (!scene_.get<ParticleEmitter>(target)) fail(e.loc, "burst(): the entity has no particles component");
            if (rt_.burst) rt_.burst(target, static_cast<int>(std::clamp(count, 0.0, 20000.0)));
            return {};
        }
        if (f == "water_height") {
            // water_height(x, z) or water_height(position)
            float x, z;
            if (a.size() == 1) {
                Vec3 p = v(0);
                x = p.x;
                z = p.z;
            } else {
                x = static_cast<float>(n(0));
                z = static_cast<float>(n(1));
            }
            return Value::number(rt_.waterHeight ? rt_.waterHeight(x, z) : 0.0);
        }
        if (f == "str") return Value::string(toText(a[0]));
        // animation builtins (the engine's AnimationSystem does the work)
        if (f == "set_param" || f == "trigger" || f == "play_animation" || f == "anim_state" || f == "play_sequence") {
            if (!rt_.animation) fail(e.loc, f + "() is not available in this context");
            EntityId target = entity(a[0], e.args[0]->loc, f + "() argument 1");
            std::vector<Json> rest;
            for (size_t i = 1; i < a.size(); ++i) rest.push_back(toJson(a[i]));
            auto r = rt_.animation(f, target, rest);
            if (!r) fail(e.loc, f + "(): " + r.error().message + (r.error().hint.empty() ? "" : " (" + r.error().hint + ")"));
            return fromJson(r.value());
        }
        if (f == "spawn") {
            if (++rt_.spawnedThisTick_ > 256) fail(e.loc, "too many spawns in one tick (limit 256)");
            if (scene_.size() >= 20000) fail(e.loc, "entity limit reached (20000)");
            std::string mesh = s(0);
            if (str::startsWith(mesh, "prefab:")) {
                if (!rt_.spawnPrefab) fail(e.loc, "prefabs are not available in this context");
                Vec3 pos = a.size() > 1 ? v(1) : Vec3{0, 0, 0};
                auto id = rt_.spawnPrefab(mesh.substr(7), pos, a.size() > 2 ? toText(a[2]) : "");
                if (!id) fail(e.loc, id.error().message);
                return Value::entity(*id);
            }
            const auto& prims = MeshRenderer::primitives();
            if (std::find(prims.begin(), prims.end(), mesh) == prims.end() && !str::startsWith(mesh, "asset:")) {
                std::string guess = str::closest(mesh, prims, 3);
                fail(e.loc, "unknown mesh '" + mesh + "'" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
            }
            std::string name = a.size() > 2 ? toText(a[2]) : mesh;
            EntityId id = scene_.create(name);
            scene_.add<MeshRenderer>(id).mesh = mesh;
            if (a.size() > 1) scene_.get<Transform>(id)->position = v(1);
            return Value::entity(id);
        }
        fail(e.loc, "unknown function '" + f + "'");
    }

    Script* script() {
        Behavior* b = scene_.get<Behavior>(self_);
        return b && scriptIndex_ < b->scripts.size() ? &b->scripts[scriptIndex_] : nullptr;
    }

    Runtime& rt_;
    Scene& scene_;
    EntityId self_;
    size_t scriptIndex_;
    std::string scriptName_;
    Runtime::Instance& inst_;
    float dt_;
    const InputState& input_;
    int budget_ = Runtime::kBudget;
    std::vector<std::unordered_map<std::string, Value>> locals_;
};

Runtime::Runtime(Scene& scene) : scene_(scene), rng_(scene.seed) {}

void Runtime::reset(bool keepQueuedEvents) {
    rng_.reseed(scene_.seed);
    time_ = 0;
    frame_ = 0;
    pending_.clear();
    if (!keepQueuedEvents) nextPending_.clear();
    instances_.clear();
    toDestroy_.clear();
    scene_.registry().each<Behavior>([](ecs::Entity, Behavior& b) {
        for (auto& s : b.scripts) s.runtimeErrors = 0;
    });
}

void Runtime::compileScripts() {
    auto names = scene_.componentNames();
    for (EntityId id : scene_.entities()) {
        Behavior* b = scene_.get<Behavior>(id);
        if (!b) continue;
        for (auto& s : b->scripts) {
            if (s.compiledSource == s.source && (s.program || s.hasErrors)) continue;
            CompileResult r = compile(s.source, names);
            s.compiledSource = s.source;
            s.program = r.program;
            s.hasErrors = !r.ok();
            for (const auto& d : r.diagnostics) {
                if (d.severity != Severity::Error) continue;
                messages_.push_back({RuntimeMessage::Kind::Compile, id, s.name, d.loc.line, d.message});
            }
        }
    }
}

void Runtime::emit(std::string name, EntityId target) { nextPending_.push_back({std::move(name), target}); }

void Runtime::tick(float dt, const InputState& input) {
    compileScripts();
    pending_ = std::move(nextPending_);
    nextPending_.clear();
    spawnedThisTick_ = 0;

    // Snapshot the order: entities spawned this tick start running next tick.
    const std::vector<EntityId> order = scene_.entities();
    for (EntityId id : order) {
        if (!scene_.exists(id) || !scene_.isActive(id)) continue;
        // Never hold Behavior pointers across handler runs: `spawn` may reallocate storage.
        for (size_t si = 0;; ++si) {
            Behavior* b = scene_.get<Behavior>(id);
            if (!b || si >= b->scripts.size()) break;
            const Script& script = b->scripts[si];
            if (!script.enabled || !script.program) continue;
            std::shared_ptr<const Program> program = script.program;  // keep alive during execution
            Instance& inst = instances_[{id, si}];
            if (inst.program != program) {  // behavior replaced (e.g. live edit): start fresh
                inst = Instance{};
                inst.program = program;
            }
            Exec exec(*this, id, si, inst, dt, input);
            auto runAll = [&](Trigger trig, const std::string& arg) {
                for (const auto& beh : program->behaviors) {
                    for (const auto& h : beh.handlers) {
                        if (h.trigger == trig && (arg.empty() || h.argument == arg)) {
                            Behavior* cur = scene_.get<Behavior>(id);
                            if (!cur || si >= cur->scripts.size() || !cur->scripts[si].enabled) return;
                            Exec(*this, id, si, inst, dt, input).run(h.body);
                        }
                    }
                }
            };
            if (!inst.started) {
                inst.started = true;
                for (const auto& beh : program->behaviors) exec.initVars(beh);
                runAll(Trigger::Start, "");
            }
            for (const auto& ev : pending_) {
                if (ev.target == kNoEntity || ev.target == id) runAll(Trigger::Event, ev.name);
            }
            for (const auto& key : input.pressed) runAll(Trigger::Key, key);
            for (EntityId clicked : input.clicked) {
                if (clicked == id) runAll(Trigger::Click, "");
            }
            runAll(Trigger::Tick, "");
        }
    }
    for (EntityId id : toDestroy_) {
        scene_.destroy(id);
        std::erase_if(instances_, [&](const auto& kv) { return !scene_.exists(kv.first.first); });
    }
    toDestroy_.clear();
    time_ += dt;
    ++frame_;
}

std::vector<RuntimeMessage> Runtime::drainMessages() {
    std::vector<RuntimeMessage> out;
    out.swap(messages_);
    return out;
}

}  // namespace sky::wander
