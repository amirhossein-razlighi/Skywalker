// The engine's link to external agent processes: a followable event log (EventLog) and tools
// served by another process (ToolHost). See EventLog.h, ToolHost.h and docs/PYTHON_AGENTS.md.

#include <algorithm>
#include <cctype>

#include "skywalker/agent/EventLog.h"
#include "skywalker/agent/ToolHost.h"
#include "skywalker/agent/ToolRegistry.h"

namespace sky {

namespace {

double wallClock() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// EventLog
// ---------------------------------------------------------------------------

EventLog::EventLog(size_t capacity) : capacity_(std::max<size_t>(capacity, 16)) {}

uint64_t EventLog::append(Json event) {
    if (!event.isObject()) event = Json::object({{"type", "value"}, {"value", std::move(event)}});
    uint64_t seq = 0;
    {
        std::lock_guard lock(mutex_);
        seq = ++lastSeq_;
        event["seq"] = seq;
        if (!event.contains("time")) event["time"] = wallClock();
        events_.push_back(std::move(event));
        while (events_.size() > capacity_) events_.pop_front();
    }
    changed_.notify_all();
    return seq;
}

EventLog::Batch EventLog::since(uint64_t cursor, size_t limit, const Filter& filter) const {
    std::lock_guard lock(mutex_);
    Batch out;
    if (cursor > lastSeq_) {  // a cursor from an earlier engine session: start over
        cursor = 0;
        out.reset = true;
    }
    out.next = cursor;
    if (events_.empty()) return out;
    uint64_t oldest = static_cast<uint64_t>(events_.front().get("seq").asInt());
    out.truncated = cursor > 0 && cursor + 1 < oldest;
    size_t start = cursor < oldest ? 0 : static_cast<size_t>(cursor + 1 - oldest);
    limit = std::max<size_t>(limit, 1);
    for (size_t i = start; i < events_.size(); ++i) {
        const Json& e = events_[i];
        uint64_t seq = static_cast<uint64_t>(e.get("seq").asInt());
        if (out.events.size() >= limit) {
            out.more = true;
            break;
        }
        out.next = seq;
        if (!filter || filter(e)) out.events.push_back(e);
    }
    return out;
}

EventLog::Batch EventLog::wait(uint64_t cursor, size_t limit, std::chrono::milliseconds timeout, const Filter& filter,
                               const std::atomic<bool>* cancel) const {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    Batch batch = since(cursor, limit, filter);
    while (batch.events.empty() && !batch.more && !(cancel && cancel->load())) {
        cursor = batch.next;
        bool interrupted = false;
        {
            std::unique_lock lock(mutex_);
            uint64_t generation = wakeGeneration_;
            bool woke = changed_.wait_until(lock, deadline, [&] {
                return lastSeq_ > cursor || wakeGeneration_ != generation || (cancel && cancel->load());
            });
            if (!woke) break;  // timed out
            interrupted = wakeGeneration_ != generation;
        }
        bool reset = batch.reset;
        batch = since(cursor, limit, filter);
        batch.reset = batch.reset || reset;
        if (interrupted) break;
    }
    return batch;
}

void EventLog::wakeAll() const {
    {
        std::lock_guard lock(mutex_);
        ++wakeGeneration_;
    }
    changed_.notify_all();
}

uint64_t EventLog::lastSeq() const {
    std::lock_guard lock(mutex_);
    return lastSeq_;
}

uint64_t EventLog::oldestSeq() const {
    std::lock_guard lock(mutex_);
    return events_.empty() ? 0 : static_cast<uint64_t>(events_.front().get("seq").asInt());
}

size_t EventLog::size() const {
    std::lock_guard lock(mutex_);
    return events_.size();
}

// ---------------------------------------------------------------------------
// ToolHost
// ---------------------------------------------------------------------------

Json ToolHost::Call::toJson() const {
    return Json::object({{"call", id}, {"tool", tool}, {"args", args}, {"actor", actor}, {"queued_at", queuedAt}});
}

ToolHost::ToolHost(ToolRegistry& registry, std::thread::id mainThread) : registry_(registry), mainThread_(mainThread) {}

ToolHost::~ToolHost() { shutdown(); }

double ToolHost::now() { return wallClock(); }

std::string ToolHost::publicName(const std::string& name) {
    return name.rfind(kPrefix, 0) == 0 ? name : std::string(kPrefix) + name;
}

Status ToolHost::validateName(const std::string& publicName) {
    bool ok = publicName.size() > std::string_view(kPrefix).size() && publicName.size() <= 64 &&
              std::all_of(publicName.begin(), publicName.end(),
                          [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
    if (ok) return {};
    return Error::make("invalid_arguments", "invalid tool name '" + publicName + "'",
                       "names use letters, digits, '_' and '-', at most 64 characters including the py_ prefix");
}

Result<std::string> ToolHost::registerTools(const std::string& hostId, const std::string& owner, const std::string& label,
                                            const std::vector<ToolSpec>& tools, double ttlSeconds, bool replace) {
    std::vector<std::string> names;
    for (const auto& t : tools) {
        std::string pub = publicName(t.name);
        if (Status s = validateName(pub); !s) return s.error();
        if (std::find(names.begin(), names.end(), pub) != names.end()) {
            return Error::make("invalid_arguments", "tool '" + pub + "' is listed twice");
        }
        if (!t.inputSchema.isNull() && !t.inputSchema.isObject()) {
            return Error::make("invalid_arguments", "input_schema of '" + pub + "' must be a JSON Schema object");
        }
        names.push_back(pub);
    }
    std::lock_guard lock(mutex_);
    if (shuttingDown_) return Error::make("unavailable", "the engine is shutting down");
    expireStaleLocked();
    std::string id = hostId;
    if (!id.empty() && !hosts_.count(id)) {
        return Error::make("not_found", "no tool host " + id, "it expired (stopped polling) or was unregistered: register again without host");
    }
    for (const auto& pub : names) {
        auto it = toolOwner_.find(pub);
        if (it != toolOwner_.end() && it->second != id && !replace) {
            return Error::make("name_taken", "'" + pub + "' is already served by host " + it->second,
                               "pick another name, or pass replace:true to take it over");
        }
    }
    if (id.empty()) id = "H-" + std::to_string(nextHost_++);
    Host& host = hosts_[id];
    host.id = id;
    host.owner = owner;
    host.label = label.empty() ? owner : label;
    host.ttl = ttlSeconds > 0 ? std::clamp(ttlSeconds, 30.0, 3600.0) : kDefaultTtl;
    host.lastSeen = now();
    // Drop tools this host no longer serves.
    for (const auto& old : host.tools) {
        if (std::find(names.begin(), names.end(), old) == names.end()) {
            registry_.removeDynamic(old);
            toolOwner_.erase(old);
        }
    }
    host.tools.clear();
    std::weak_ptr<ToolHost> weak = weak_from_this();
    for (size_t i = 0; i < tools.size(); ++i) {
        const ToolSpec& spec = tools[i];
        const std::string& pub = names[i];
        auto previous = toolOwner_.find(pub);
        if (previous != toolOwner_.end() && previous->second != id) {
            auto other = hosts_.find(previous->second);
            if (other != hosts_.end()) std::erase(other->second.tools, pub);
        }
        ToolDef def;
        def.name = pub;
        def.title = spec.title.empty() ? pub : spec.title;
        def.description = spec.description + (spec.description.empty() ? "" : " ") + "(Served by " + host.label +
                          " through the agent link; call it like any engine tool.)";
        def.category = "python";
        def.inputSchema = spec.inputSchema.isObject() ? spec.inputSchema : Json::object({{"type", "object"}});
        if (!def.inputSchema.contains("type")) def.inputSchema["type"] = "object";
        def.mutates = spec.mutates;
        def.handler = [weak, pub](const Json& args, ToolContext& ctx) -> ToolResult {
            auto self = weak.lock();
            if (!self) return ToolResult::error(Error::make("unavailable", "the tool host is gone"));
            auto queued = self->enqueue(pub, args, ctx.actor);
            if (!queued) return ToolResult::error(queued.error());
            uint64_t call = *queued;
            auto out = std::make_shared<Json>();
            return ToolResult::defer(
                [self, call, out] {
                    if (self->onMainThread()) {
                        // Nobody else can run tool_host_reply while this thread waits.
                        self->cancel(call, "unavailable", "py_* tools need an agent connection to wait on",
                                     "call it over the agent socket (the editor, or `skywalker serve`), not from the "
                                     "engine's own thread (stdio `skywalker mcp`, the CLI runner)");
                    }
                    auto r = self->waitReply(call, self->onMainThread() ? std::chrono::milliseconds(0) : kCallTimeout);
                    if (!r) {
                        self->cancel(call, "timeout", "the tool host did not answer in time", "is the host process busy or gone? (tool_host_list)");
                        r = self->waitReply(call, std::chrono::milliseconds(0));
                    }
                    *out = r ? *r : ToolResult::error(Error::make("timeout", "no reply")).toMcp();
                },
                [out] { return toolResultFromMcp(*out); },
                [self, call] { self->cancel(call, "cancelled", "the agent server is stopping"); });
        };
        if (Status s = registry_.addDynamic(std::move(def)); !s) return s.error();
        toolOwner_[pub] = id;
        host.tools.push_back(pub);
    }
    return id;
}

bool ToolHost::unregister(const std::string& hostId, const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (!hosts_.count(hostId)) return false;
    removeHostLocked(hostId, reason);
    changed_.notify_all();
    return true;
}

void ToolHost::expireStale() {
    std::lock_guard lock(mutex_);
    expireStaleLocked();
}

void ToolHost::expireStaleLocked() {
    double t = now();
    std::vector<std::string> stale;
    for (const auto& [id, h] : hosts_) {
        if (t - h.lastSeen > h.ttl) stale.push_back(id);
    }
    for (const auto& id : stale) removeHostLocked(id, "the tool host stopped polling (expired)");
    if (!stale.empty()) changed_.notify_all();
}

void ToolHost::removeHostLocked(const std::string& hostId, const std::string& reason) {
    auto it = hosts_.find(hostId);
    if (it == hosts_.end()) return;
    for (const auto& pub : it->second.tools) {
        auto owner = toolOwner_.find(pub);
        if (owner != toolOwner_.end() && owner->second == hostId) {
            registry_.removeDynamic(pub);
            toolOwner_.erase(owner);
        }
    }
    for (auto& [cid, p] : pending_) {
        if (p.call.host == hostId && !p.done) failLocked(p, "host_gone", reason, "tool_host_list shows the live hosts");
    }
    hosts_.erase(it);
}

void ToolHost::failLocked(Pending& p, const std::string& code, const std::string& message, const std::string& hint) {
    if (p.done) return;
    p.done = true;
    p.result = ToolResult::error(Error::make(code, message, hint)).toMcp();
    ++failed_;
}

Result<uint64_t> ToolHost::enqueue(const std::string& tool, const Json& args, const std::string& actor) {
    std::lock_guard lock(mutex_);
    if (shuttingDown_) return Error::make("unavailable", "the engine is shutting down");
    expireStaleLocked();
    auto owner = toolOwner_.find(tool);
    if (owner == toolOwner_.end()) return Error::make("host_gone", "nothing serves " + tool + " any more", "tool_host_list shows the live hosts");
    Pending p;
    p.call.id = nextCall_++;
    p.call.host = owner->second;
    p.call.tool = tool;
    p.call.args = args;
    p.call.actor = actor;
    p.call.queuedAt = now();
    uint64_t id = p.call.id;
    pending_.emplace(id, std::move(p));
    changed_.notify_all();
    return id;
}

std::optional<Json> ToolHost::waitReply(uint64_t call, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    auto ready = [&] {
        auto it = pending_.find(call);
        return it == pending_.end() || it->second.done || shuttingDown_;
    };
    changed_.wait_for(lock, timeout, ready);
    auto it = pending_.find(call);
    if (it == pending_.end()) return ToolResult::error(Error::make("cancelled", "the call was dropped")).toMcp();
    if (!it->second.done) {
        if (!shuttingDown_) return std::nullopt;
        failLocked(it->second, "unavailable", "the engine is shutting down", "");
    }
    Json result = std::move(it->second.result);
    pending_.erase(it);
    return result;
}

void ToolHost::cancel(uint64_t call, const std::string& code, const std::string& message, const std::string& hint) {
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(call);
        if (it == pending_.end()) return;
        failLocked(it->second, code, message, hint);
    }
    changed_.notify_all();
}

Result<std::vector<ToolHost::Call>> ToolHost::poll(const std::string& hostId, size_t max, std::chrono::milliseconds timeout,
                                                   const std::atomic<bool>* cancelFlag) {
    std::unique_lock lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    max = std::clamp<size_t>(max, 1, 64);
    while (true) {
        auto h = hosts_.find(hostId);
        if (h == hosts_.end()) {
            return Error::make("not_found", "no tool host " + hostId, "it expired or was unregistered: call tool_host_register again");
        }
        h->second.lastSeen = now();
        std::vector<Call> out;
        for (auto& [id, p] : pending_) {
            if (p.call.host != hostId || p.delivered || p.done) continue;
            p.delivered = true;
            out.push_back(p.call);
            if (out.size() >= max) break;
        }
        if (!out.empty() || shuttingDown_ || (cancelFlag && cancelFlag->load())) return out;
        if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
            auto again = hosts_.find(hostId);
            if (again != hosts_.end()) again->second.lastSeen = now();
            return std::vector<Call>{};
        }
    }
}

Status ToolHost::reply(const std::string& hostId, uint64_t call, Json mcpResult) {
    {
        std::lock_guard lock(mutex_);
        auto h = hosts_.find(hostId);
        if (h == hosts_.end()) return Error::make("not_found", "no tool host " + hostId, "register again");
        h->second.lastSeen = now();
        auto it = pending_.find(call);
        if (it == pending_.end() || it->second.call.host != hostId) {
            return Error::make("not_found", "call " + std::to_string(call) + " is not waiting any more",
                               "the caller timed out or was cancelled; nothing to do");
        }
        if (it->second.done) return Error::make("conflict", "call " + std::to_string(call) + " was already answered or failed");
        it->second.done = true;
        it->second.result = std::move(mcpResult);
        ++h->second.served;
        ++served_;
    }
    changed_.notify_all();
    return {};
}

void ToolHost::shutdown() {
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
        for (auto& [id, p] : pending_) failLocked(p, "unavailable", "the engine is shutting down", "");
    }
    changed_.notify_all();
}

void ToolHost::wakeAll() { changed_.notify_all(); }

Json ToolHost::info() const {
    std::lock_guard lock(mutex_);
    Json hosts = Json::array();
    double t = now();
    for (const auto& [id, h] : hosts_) {
        Json tools = Json::array();
        for (const auto& name : h.tools) tools.push(name);
        size_t queued = 0, inFlight = 0;
        for (const auto& [cid, p] : pending_) {
            if (p.call.host != id || p.done) continue;
            (p.delivered ? inFlight : queued)++;
        }
        hosts.push(Json::object({{"host", id},
                                 {"label", h.label},
                                 {"owner", h.owner},
                                 {"tools", tools},
                                 {"queued", queued},
                                 {"in_flight", inFlight},
                                 {"served", h.served},
                                 {"idle_seconds", std::max(0.0, t - h.lastSeen)},
                                 {"ttl", h.ttl}}));
    }
    return Json::object({{"hosts", hosts}, {"stats", Json::object({{"served", served_}, {"failed", failed_}})}});
}

Json ToolHost::stats() const {
    std::lock_guard lock(mutex_);
    size_t queued = 0, inFlight = 0;
    for (const auto& [cid, p] : pending_) {
        if (p.done) continue;
        (p.delivered ? inFlight : queued)++;
    }
    return Json::object({{"hosts", hosts_.size()},
                         {"tools", toolOwner_.size()},
                         {"queued", queued},
                         {"in_flight", inFlight},
                         {"served", served_},
                         {"failed", failed_}});
}

ToolResult toolResultFromMcp(const Json& mcp) {
    ToolResult r;
    for (const auto& block : mcp.get("content").elements()) {
        std::string type = block.get("type").asString();
        if (type == "text") {
            r.content.push_back({ContentBlock::Type::Text, block.get("text").asString(), {}, {}});
        } else if (type == "image") {
            r.content.push_back({ContentBlock::Type::Image, {}, block.get("data").asString(), block.get("mimeType").asString("image/png")});
        }
    }
    if (mcp.get("structuredContent").isObject()) r.structured = mcp.get("structuredContent");
    r.isError = mcp.get("isError").asBool();
    if (r.content.empty()) r.content.push_back({ContentBlock::Type::Text, r.structured.isObject() ? r.structured.dump() : "", {}, {}});
    return r;
}

}  // namespace sky
