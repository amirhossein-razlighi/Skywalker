// Exact Wander play state for save games (docs/SAVE_GAMES.md): Runtime::saveState / loadState and
// the type-tagged value codec they use. Everything a tick depends on is captured between two ticks:
// clocks, the random generator, queued events and contacts, and per instance its var table, state
// machines, timers and waiting coroutines (program counter + registers). A coroutine resumes only in
// the program it was saved from (same hash); otherwise its instance starts fresh.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "RuntimeInternal.h"

namespace sky::wander {

namespace {

// --- type-tagged values ---------------------------------------------------------------------

Json floats(const float* f, int n) {
    Json a = Json::array();
    for (int i = 0; i < n; ++i) a.push(static_cast<double>(f[i]));
    return a;
}

bool readFloats(const Json& j, float* out, size_t n) {
    if (!j.isArray() || j.size() != n) return false;
    for (size_t i = 0; i < n; ++i) out[i] = j[i].asFloat();
    return true;
}

// 64-bit values (random state, program hashes) do not fit a JSON double: hex strings.
std::string hex64(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

uint64_t parseHex64(const Json& j) { return std::strtoull(j.asString().c_str(), nullptr, 16); }

}  // namespace

Json toTaggedJson(const Value& v) {
    switch (v.type()) {
        case VType::None: return {};
        case VType::Bool: return v.b();
        case VType::Number: {
            double n = v.num();
            if (std::isfinite(n)) return n;
            return Json::object({{"$num", std::isnan(n) ? "nan" : (n > 0 ? "inf" : "-inf")}});
        }
        case VType::String: return v.str();
        case VType::Vec: {
            Vec3 x = v.v();
            float f[3] = {x.x, x.y, x.z};
            return Json::object({{"$vec", floats(f, 3)}});
        }
        case VType::Color: {
            Vec4 c = v.c();
            float f[4] = {c.x, c.y, c.z, c.w};
            return Json::object({{"$color", floats(f, 4)}});
        }
        case VType::Entity: return Json::object({{"$entity", v.e()}});
        case VType::List: {
            Json a = Json::array();
            for (const auto& item : v.items()) a.push(toTaggedJson(item));
            return a;
        }
        case VType::Map: {
            Json o = Json::object();
            bool escape = false;
            for (const auto& [k, item] : v.mapObj().entries) {
                if (!k.empty() && k[0] == '$') escape = true;
                o[k] = toTaggedJson(item);
            }
            return escape ? Json::object({{"$map", std::move(o)}}) : o;
        }
    }
    return {};
}

Value fromTaggedJson(const Json& j) {
    switch (j.type()) {
        case Json::Type::Null: return {};
        case Json::Type::Bool: return Value::boolean(j.asBool());
        case Json::Type::Number: return Value::number(j.asNumber());
        case Json::Type::String: return Value::string(j.asString());
        case Json::Type::Array: {
            std::vector<Value> items;
            items.reserve(j.size());
            for (const auto& e : j.elements()) items.push_back(fromTaggedJson(e));
            return Value::list(std::move(items));
        }
        case Json::Type::Object: break;
    }
    if (j.size() == 1) {
        const auto& [tag, body] = j.members().front();
        if (tag == "$vec") {
            float f[3] = {};
            if (readFloats(body, f, 3)) return Value::vec({f[0], f[1], f[2]});
        } else if (tag == "$color") {
            float f[4] = {};
            if (readFloats(body, f, 4)) return Value::color({f[0], f[1], f[2], f[3]});
        } else if (tag == "$entity") {
            return Value::entity(static_cast<EntityRef>(body.asInt()));
        } else if (tag == "$num") {
            const std::string& s = body.asString();
            if (s == "nan") return Value::number(std::nan(""));
            return Value::number(s == "-inf" ? -HUGE_VAL : HUGE_VAL);
        } else if (tag == "$map") {
            Value m = Value::map();
            for (const auto& [k, item] : body.members()) m.mutMap().set(k, fromTaggedJson(item));
            return m;
        }
    }
    Value m = Value::map();
    for (const auto& [k, item] : j.members()) m.mutMap().set(k, fromTaggedJson(item));
    return m;
}

namespace {

Json contactToJson(const Runtime::Contact& c) {
    float p[3] = {c.point.x, c.point.y, c.point.z}, n[3] = {c.normal.x, c.normal.y, c.normal.z};
    return Json::object({{"trigger", static_cast<int>(c.trigger)},
                         {"self", c.self},
                         {"other", c.other},
                         {"point", floats(p, 3)},
                         {"normal", floats(n, 3)},
                         {"speed", static_cast<double>(c.speed)}});
}

Runtime::Contact contactFromJson(const Json& j) {
    Runtime::Contact c;
    c.trigger = static_cast<Trigger>(j.get("trigger").asInt());
    c.self = static_cast<EntityId>(j.get("self").asInt());
    c.other = static_cast<EntityId>(j.get("other").asInt());
    float p[3] = {}, n[3] = {};
    if (readFloats(j.get("point"), p, 3)) c.point = {p[0], p[1], p[2]};
    if (readFloats(j.get("normal"), n, 3)) c.normal = {n[0], n[1], n[2]};
    c.speed = j.get("speed").asFloat();
    return c;
}

Json eventToJson(const PendingEvent& ev) {
    return Json::object({{"name", ev.name}, {"target", ev.target}, {"other", ev.other}, {"payload", toTaggedJson(ev.payload)}});
}

PendingEvent eventFromJson(const Json& j) {
    PendingEvent ev;
    ev.name = j.get("name").asString();
    ev.sym = intern(ev.name);
    ev.target = static_cast<EntityId>(j.get("target").asInt());
    ev.other = static_cast<EntityId>(j.get("other").asInt());
    ev.payload = fromTaggedJson(j.get("payload"));
    return ev;
}

Json registers(const std::vector<Value>& regs) {
    Json a = Json::array();
    for (const auto& r : regs) a.push(toTaggedJson(r));
    return a;
}

}  // namespace

Json Runtime::saveState(const std::function<bool(EntityId)>& include) const {
    const Impl& impl = *impl_;
    Json events = Json::array();
    for (const auto& ev : impl.nextPending) {
        if (ev.target == kNoEntity || include(ev.target)) events.push(eventToJson(ev));
    }
    Json contacts = Json::array();
    for (const auto& c : impl.nextContacts) {
        if (include(c.self)) contacts.push(contactToJson(c));
    }

    // Instances in (entity, script) order: the file is the same bytes for the same state.
    std::vector<const Instance*> list;
    for (const auto& [key, inst] : impl.instances) {
        if (include(key.first)) list.push_back(&inst);
    }
    std::sort(list.begin(), list.end(), [](const Instance* a, const Instance* b) {
        return a->entity != b->entity ? a->entity < b->entity : a->scriptIndex < b->scriptIndex;
    });
    Json instances = Json::array();
    for (const Instance* inst : list) {
        if (!inst->program) continue;
        Json behaviors = Json::array();
        for (const auto& br : inst->behaviors) {
            Json timers = Json::array(), fired = Json::array();
            for (double t : br.timers) timers.push(t);
            for (uint8_t f : br.fired) fired.push(static_cast<int>(f));
            behaviors.push(Json::object({{"state", br.state}, {"stateTime", br.stateTime}, {"timers", timers}, {"fired", fired}}));
        }
        Json coroutines = Json::array();
        for (const auto& co : inst->coroutines) {
            Json cj = Json::object({{"behavior", co.behavior},
                                    {"handler", co.handler},
                                    {"state", co.state},
                                    {"proto", co.proto},
                                    {"pc", static_cast<uint64_t>(co.pc)},
                                    {"regs", registers(co.regs)},
                                    {"byFrames", co.byFrames},
                                    {"remaining", co.remaining},
                                    {"other", co.other}});
            if (co.contact) cj["contact"] = contactToJson(*co.contact);
            coroutines.push(std::move(cj));
        }
        Json deferred = Json::array(), deferredContacts = Json::array();
        for (const auto& ev : inst->deferred) deferred.push(eventToJson(ev));
        for (const auto& c : inst->deferredContacts) deferredContacts.push(contactToJson(c));
        Json ij = Json::object({{"entity", inst->entity},
                                {"script", static_cast<uint64_t>(inst->scriptIndex)},
                                {"name", inst->scriptName},
                                {"program", hex64(inst->program->hash)},
                                {"started", inst->started},
                                {"behaviors", behaviors},
                                {"coroutines", coroutines}});
        if (!deferred.elements().empty()) ij["deferred"] = deferred;
        if (!deferredContacts.elements().empty()) ij["deferredContacts"] = deferredContacts;
        if (inst->frameFailed) ij["frameFailed"] = true;
        instances.push(std::move(ij));
    }

    // Var tables: exact values (EntityRecord::vars holds the lossy JSON mirror).
    std::vector<EntityId> varEntities;
    for (const auto& [id, table] : impl.vars) {
        if (include(id)) varEntities.push_back(id);
    }
    std::sort(varEntities.begin(), varEntities.end());
    Json vars = Json::array();
    for (EntityId id : varEntities) {
        Json slots = Json::array();
        for (const auto& slot : impl.vars.at(id).slots) {
            slots.push(Json::object({{"name", *slot.name}, {"value", toTaggedJson(slot.value)}, {"inScene", slot.inScene}}));
        }
        vars.push(Json::object({{"entity", id}, {"slots", slots}}));
    }

    Json state = Json::object({{"time", time_},
                               {"frame", frame_},
                               {"unscaledTime", realTime_},
                               {"rng", Json::object({{"state", hex64(rng_.state())}, {"inc", hex64(rng_.increment())}})},
                               {"paused", paused_},
                               {"timeScale", timeScale_}});
    if (requestedPause_) state["requestedPause"] = *requestedPause_;
    if (requestedScale_) state["requestedScale"] = *requestedScale_;
    state["events"] = std::move(events);
    state["contacts"] = std::move(contacts);
    state["vars"] = std::move(vars);
    state["instances"] = std::move(instances);
    return state;
}

Status Runtime::loadState(const Json& state, std::vector<std::string>& warnings) {
    if (!state.isObject()) return Error::make("invalid_save", "the save has no runtime state");
    if (ticking_) return Error::make("invalid_state", "runtime state can only be restored between ticks");
    Impl& impl = *impl_;
    compileScripts();  // instances bind to the programs the scene runs now

    // Forget everything the saved entities had: their instances and var tables are replaced.
    std::vector<EntityId> restored;
    for (const auto& v : state.get("vars").elements()) restored.push_back(static_cast<EntityId>(v.get("entity").asInt()));
    for (const auto& ij : state.get("instances").elements()) restored.push_back(static_cast<EntityId>(ij.get("entity").asInt()));
    std::sort(restored.begin(), restored.end());
    restored.erase(std::unique(restored.begin(), restored.end()), restored.end());
    auto isRestored = [&](EntityId id) { return std::binary_search(restored.begin(), restored.end(), id); };
    std::erase_if(impl.instances, [&](const auto& kv) { return isRestored(kv.first.first); });
    for (EntityId id : restored) impl.vars.erase(id);

    for (const auto& vj : state.get("vars").elements()) {
        EntityId id = static_cast<EntityId>(vj.get("entity").asInt());
        if (!scene_.exists(id)) continue;
        VarTable& table = varTable(impl, scene_, id);
        table.slots.clear();
        for (const auto& sj : vj.get("slots").elements()) {
            VarSlot slot = makeSlot(intern(sj.get("name").asString()));
            slot.value = fromTaggedJson(sj.get("value"));
            slot.inScene = sj.get("inScene").asBool();
            table.slots.push_back(std::move(slot));
        }
    }

    for (const auto& ij : state.get("instances").elements()) {
        EntityId id = static_cast<EntityId>(ij.get("entity").asInt());
        size_t si = static_cast<size_t>(ij.get("script").asInt());
        const std::string& name = ij.get("name").asString();
        const Behavior* b = scene_.get<Behavior>(id);
        if (!b || si >= b->scripts.size() || !b->scripts[si].program) {
            warnings.push_back("behavior '" + name + "' of #" + std::to_string(id) + " no longer exists: its saved state was dropped");
            continue;
        }
        const Script& script = b->scripts[si];
        if (hex64(script.program->hash) != ij.get("program").asString()) {
            warnings.push_back("behavior '" + name + "' of #" + std::to_string(id) +
                               " changed since the save: it starts fresh (its vars are kept)");
            continue;
        }
        const Program& prog = *script.program;
        Instance inst;
        inst.program = script.program;
        inst.entity = id;
        inst.scriptIndex = si;
        inst.scriptName = script.name;
        inst.started = ij.get("started").asBool();
        inst.frameFailed = ij.get("frameFailed").asBool();
        if (inst.started) {
            inst.vars = &varTable(impl, scene_, id);
            const Json& runs = ij.get("behaviors");
            inst.behaviors.assign(prog.behaviors.size(), {});
            for (size_t bi = 0; bi < prog.behaviors.size(); ++bi) {
                BehaviorRun& br = inst.behaviors[bi];
                const Json& rj = bi < runs.size() ? runs[bi] : Json::null();
                const auto timerCount = static_cast<size_t>(prog.behaviors[bi].timerCount);
                br.timers.assign(timerCount, 0.0);
                br.fired.assign(timerCount, 0);
                for (size_t t = 0; t < timerCount && t < rj.get("timers").size(); ++t) br.timers[t] = rj.get("timers")[t].asNumber();
                for (size_t t = 0; t < timerCount && t < rj.get("fired").size(); ++t) {
                    br.fired[t] = static_cast<uint8_t>(rj.get("fired")[t].asInt());
                }
                br.state = static_cast<int>(rj.get("state").asInt(-1));
                br.stateTime = rj.get("stateTime").asNumber();
                for (const auto& v : prog.behaviors[bi].vars) {
                    int slot = inst.vars->find(v.sym);
                    if (slot < 0) {
                        inst.vars->slots.push_back(makeSlot(v.sym));
                        slot = static_cast<int>(inst.vars->slots.size()) - 1;
                    }
                    br.varSlots.push_back(slot);
                }
            }
            for (const auto& cj : ij.get("coroutines").elements()) {
                Coroutine co;
                co.behavior = static_cast<int>(cj.get("behavior").asInt(-1));
                co.handler = static_cast<int>(cj.get("handler").asInt(-1));
                co.state = static_cast<int>(cj.get("state").asInt(-1));
                co.proto = static_cast<int>(cj.get("proto").asInt(-1));
                co.pc = static_cast<size_t>(cj.get("pc").asInt());
                if (co.proto < 0 || co.proto >= static_cast<int>(prog.protos.size()) || co.behavior < 0 ||
                    co.behavior >= static_cast<int>(prog.behaviors.size())) {
                    return Error::make("invalid_save", "a saved waiting handler of '" + name + "' does not fit its program");
                }
                for (const auto& r : cj.get("regs").elements()) co.regs.push_back(fromTaggedJson(r));
                co.regs.resize(std::max(co.regs.size(), static_cast<size_t>(prog.protos[co.proto].numRegs)));
                co.byFrames = cj.get("byFrames").asBool();
                co.remaining = cj.get("remaining").asNumber();
                co.other = static_cast<EntityId>(cj.get("other").asInt());
                if (const Json* c = cj.find("contact")) co.contact = contactFromJson(*c);
                inst.coroutines.push_back(std::move(co));
            }
            for (const auto& ev : ij.get("deferred").elements()) inst.deferred.push_back(eventFromJson(ev));
            for (const auto& c : ij.get("deferredContacts").elements()) inst.deferredContacts.push_back(contactFromJson(c));
        }
        impl.instances[{id, si}] = std::move(inst);
    }

    // Clocks and the random generator: the next tick is the one that followed the save.
    time_ = state.get("time").asNumber();
    frame_ = static_cast<uint64_t>(state.get("frame").asInt());
    realTime_ = state.get("unscaledTime").asNumber();
    rng_.restore(parseHex64(state.get("rng").get("state")), parseHex64(state.get("rng").get("inc")));
    paused_ = state.get("paused").asBool();
    timeScale_ = state.get("timeScale").asNumber(1.0);
    requestedPause_.reset();
    requestedScale_.reset();
    if (const Json* p = state.find("requestedPause")) requestedPause_ = p->asBool();
    if (const Json* s = state.find("requestedScale")) requestedScale_ = s->asNumber(1.0);
    preparedFrame_ = ~0ull;
    impl.nextPending.clear();
    for (const auto& ev : state.get("events").elements()) impl.nextPending.push_back(eventFromJson(ev));
    impl.nextContacts.clear();
    for (const auto& c : state.get("contacts").elements()) impl.nextContacts.push_back(contactFromJson(c));
    impl.cosmeticUndo.clear();
    impl.toDestroy.clear();
    // The scene's vars were restored together with these tables: nothing to re-import on the next tick.
    impl.lastRevision = scene_.revision();
    impl.revisionValid = true;
    return {};
}

}  // namespace sky::wander
