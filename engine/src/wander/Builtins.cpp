// The builtin registry and symbol interning.

#include "skywalker/wander/Builtins.h"

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace sky::wander {

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

namespace {
struct SymbolTable {
    std::mutex mutex;
    std::unordered_map<std::string, uint32_t> ids;
    std::deque<std::string> names;  // deque: references stay valid as it grows
};
SymbolTable& symbols() {
    static SymbolTable* t = new SymbolTable();  // never destroyed: ids outlive static teardown order
    return *t;
}
}  // namespace

uint32_t intern(std::string_view name) {
    SymbolTable& t = symbols();
    std::lock_guard lock(t.mutex);
    auto it = t.ids.find(std::string(name));
    if (it != t.ids.end()) return it->second;
    auto id = static_cast<uint32_t>(t.names.size() + 1);
    t.names.emplace_back(name);
    t.ids.emplace(std::string(name), id);
    return id;
}

const std::string& symbolName(uint32_t id) {
    SymbolTable& t = symbols();
    std::lock_guard lock(t.mutex);
    static const std::string empty;
    if (id == 0 || id > t.names.size()) return empty;
    return t.names[id - 1];
}

// ---------------------------------------------------------------------------
// BuiltinDef
// ---------------------------------------------------------------------------

int BuiltinDef::minArgs() const {
    int n = 0;
    for (const auto& p : params) {
        if (!p.optional) ++n;
    }
    return n;
}

int BuiltinDef::maxArgs() const { return variadic ? -1 : static_cast<int>(params.size()); }

std::string BuiltinDef::signature() const {
    std::string s;
    if (receiver) s += typeSetName(receiver) + ".";
    s += name + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) s += ", ";
        s += params[i].name;
        if (params[i].type != kTAny) s += ": " + typeSetName(params[i].type);
        if (params[i].optional) s += "?";
    }
    if (variadic) s += ", ...";
    s += ")";
    if (returns != kTNone) s += " -> " + typeSetName(returns);
    return s;
}

Json BuiltinDef::toJson() const {
    Json ps = Json::array();
    for (const auto& p : params) {
        Json j = Json::object({{"name", p.name}, {"type", typeSetName(p.type)}});
        if (p.optional) j["optional"] = true;
        ps.push(j);
    }
    Json j = Json::object({{"name", name},
                           {"signature", signature()},
                           {"category", category},
                           {"doc", doc},
                           {"params", ps},
                           {"returns", typeSetName(returns)}});
    if (receiver) j["receiver"] = typeSetName(receiver);
    if (!example.empty()) j["example"] = example;
    if (mutates) j["mutates"] = true;
    if (!owner.empty() && owner != "core") j["owner"] = owner;
    return j;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

BuiltinRegistry& BuiltinRegistry::global() {
    static BuiltinRegistry* reg = [] {
        auto* r = new BuiltinRegistry();
        registerCoreBuiltins(*r);
        registerSystemBuiltins(*r);
        return r;
    }();
    return *reg;
}

void BuiltinRegistry::add(BuiltinDef def) {
    std::lock_guard lock(mutex_);
    if (def.owner.empty()) def.owner = "engine";
    Key key{def.name, def.receiver};
    defs_[key] = std::make_shared<const BuiltinDef>(std::move(def));
    ++generation_;
}

bool BuiltinRegistry::remove(std::string_view name, TypeSet receiver) {
    std::lock_guard lock(mutex_);
    bool removed = defs_.erase(Key{std::string(name), receiver}) > 0;
    if (removed) ++generation_;
    return removed;
}

void BuiltinRegistry::removeOwner(std::string_view owner) {
    std::lock_guard lock(mutex_);
    size_t before = defs_.size();
    std::erase_if(defs_, [&](const auto& kv) { return kv.second->owner == owner; });
    if (defs_.size() != before) ++generation_;
}

void BuiltinRegistry::addTrigger(TriggerDef t) {
    std::lock_guard lock(mutex_);
    std::string name = t.name;
    triggers_[name] = std::move(t);
    ++generation_;
}

std::shared_ptr<const BuiltinDef> BuiltinRegistry::find(std::string_view name) const {
    {
        std::lock_guard lock(mutex_);
        auto it = defs_.find(Key{std::string(name), 0});
        if (it != defs_.end()) return it->second;
    }
    return parent_ ? parent_->find(name) : nullptr;
}

std::shared_ptr<const BuiltinDef> BuiltinRegistry::findMethod(std::string_view name, VType receiver) const {
    {
        std::lock_guard lock(mutex_);
        TypeSet bit = tbit(receiver);
        // Exact registrations are keyed by their receiver set; scan the entries with this name.
        for (auto it = defs_.lower_bound(Key{std::string(name), 0}); it != defs_.end() && it->first.name == name; ++it) {
            if (it->first.receiver & bit) return it->second;
        }
    }
    return parent_ ? parent_->findMethod(name, receiver) : nullptr;
}

bool BuiltinRegistry::hasMethodNamed(std::string_view name) const {
    {
        std::lock_guard lock(mutex_);
        for (auto it = defs_.lower_bound(Key{std::string(name), 0}); it != defs_.end() && it->first.name == name; ++it) {
            if (it->first.receiver) return true;
        }
    }
    return parent_ && parent_->hasMethodNamed(name);
}

const TriggerDef* BuiltinRegistry::trigger(std::string_view name) const {
    {
        std::lock_guard lock(mutex_);
        auto it = triggers_.find(name);
        if (it != triggers_.end()) return &it->second;
    }
    return parent_ ? parent_->trigger(name) : nullptr;
}

std::vector<std::shared_ptr<const BuiltinDef>> BuiltinRegistry::all(bool includeHidden) const {
    std::vector<std::shared_ptr<const BuiltinDef>> out;
    if (parent_) out = parent_->all(includeHidden);
    std::lock_guard lock(mutex_);
    for (const auto& [k, def] : defs_) {
        if (def->hidden && !includeHidden) continue;
        // Overrides replace the parent's entry.
        std::erase_if(out, [&](const auto& d) { return d->name == def->name && d->receiver == def->receiver; });
        out.push_back(def);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a->category != b->category) return a->category < b->category;
        if ((a->receiver != 0) != (b->receiver != 0)) return a->receiver == 0;
        return a->name < b->name;
    });
    return out;
}

std::vector<TriggerDef> BuiltinRegistry::triggers() const {
    std::vector<TriggerDef> out;
    if (parent_) out = parent_->triggers();
    std::lock_guard lock(mutex_);
    for (const auto& [n, t] : triggers_) out.push_back(t);
    return out;
}

std::vector<std::string> BuiltinRegistry::functionNames() const {
    std::vector<std::string> out;
    for (const auto& d : all(false)) {
        if (!d->receiver) out.push_back(d->name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> BuiltinRegistry::methodNames(VType receiver) const {
    std::vector<std::string> out;
    for (const auto& d : all(false)) {
        if (d->receiver & tbit(receiver)) out.push_back(d->name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uint64_t BuiltinRegistry::generation() const {
    std::lock_guard lock(mutex_);
    return generation_ + (parent_ ? parent_->generation() * 1315423911ULL : 0);
}

// ---------------------------------------------------------------------------
// Cosmetic (`on frame`) allowances
// ---------------------------------------------------------------------------

const std::vector<std::string>& cosmeticComponents() {
    static const std::vector<std::string> names{"transform", "mesh", "light", "camera", "sprite", "text", "ui", "light2d"};
    return names;
}

bool cosmeticComponent(std::string_view component) {
    const auto& names = cosmeticComponents();
    return std::find(names.begin(), names.end(), component) != names.end();
}

bool cosmeticBuiltin(const BuiltinDef& def) {
    if (def.pure) return true;
    // Read-only queries: they look at the world without changing it (random() advances the seeded
    // generator, so it is not here: use noise()).
    static const std::set<std::string, std::less<>> reads{
        "distance", "direction", "forward",  "right",   "up",          "world_position", "find",     "find_all",
        "nearest",  "count",     "tagged",   "exists",  "children",    "has",            "key",      "key_pressed",
        "str",      "num",       "action",   "pressed", "released",    "axis",           "grounded", "arrived",
        "raycast",  "overlap_sphere", "anim_state", "water_height", "is_paused", "tile_at", "dialogue_var", "__log"};
    return reads.count(def.name) > 0;
}

}  // namespace sky::wander
