// The Wander debugger: breakpoints, stepping, inspection and evaluation in paused frames
// (docs/WANDER.md "Debugging"). The VM calls statement() before each statement of debugged code
// (only while active(), see Runtime.cpp), enterFrame / leaveFrame around every proto run, and
// runtimeError() when a run fails.

#include "skywalker/wander/Debugger.h"

#include <algorithm>
#include <cstdlib>

#include "RuntimeInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/wander/Compiler.h"

namespace sky::wander {

struct Debugger::Stop {
    std::string reason;  // breakpoint | step | pause | error
    int breakpoint = -1;
    std::string message;
};

Json Breakpoint::toJson() const {
    Json j = Json::object({{"id", id},
                           {"script", script},
                           {"line", line},
                           {"enabled", enabled},
                           {"verified", verified},
                           {"hits", hits}});
    if (requestedLine && requestedLine != line) j["requestedLine"] = requestedLine;
    if (entity) j["entity"] = entity;
    if (!condition.empty()) j["condition"] = condition;
    if (!hitCondition.empty()) j["hitCondition"] = hitCondition;
    if (!log.empty()) j["log"] = log;
    if (!message.empty()) j["message"] = message;
    return j;
}

namespace {

const char* stepName(StepMode m) {
    switch (m) {
        case StepMode::Continue: return "continue";
        case StepMode::Over: return "over";
        case StepMode::Into: return "into";
        case StepMode::Out: return "out";
    }
    return "continue";
}

/// "5" -> the 5th hit, ">=5" / ">5" -> from then on, "%3" -> every 3rd. Unparsable conditions always pass.
bool hitPasses(const std::string& cond, int hits) {
    std::string c = str::trim(cond);
    if (c.empty()) return true;
    auto num = [](const std::string& s) { return std::atoi(s.c_str()); };
    if (str::startsWith(c, ">=")) return hits >= num(c.substr(2));
    if (str::startsWith(c, ">")) return hits > num(c.substr(1));
    if (str::startsWith(c, "%")) return num(c.substr(1)) > 0 && hits % num(c.substr(1)) == 0;
    if (str::startsWith(c, "==")) return hits == num(c.substr(2));
    return hits == num(c);
}

/// Raises the shared register stack's top above every frame of the paused VM (nested calls keep their
/// registers above the handler's without moving it), so evaluations can never overwrite them.
struct StackTopGuard {
    Runtime::Impl& impl;
    size_t saved;
    StackTopGuard(Runtime::Impl& i, const std::vector<Debugger::Frame>& frames) : impl(i), saved(i.stackTop) {
        for (const auto& f : frames) {
            const size_t top = static_cast<size_t>(f.regs - impl.stack.data()) + static_cast<size_t>(f.st->prog->protos[f.proto].numRegs);
            impl.stackTop = std::max(impl.stackTop, top);
        }
    }
    ~StackTopGuard() { impl.stackTop = saved; }
};

}  // namespace

Debugger::Debugger(Runtime& runtime) : rt_(runtime) {}
Debugger::~Debugger() = default;

// --- breakpoints --------------------------------------------------------------------------------

Result<Breakpoint> Debugger::setBreakpoint(Breakpoint bp) {
    if (bp.line <= 0) return Error::make("invalid_argument", "a breakpoint needs a line (1-based)");
    if (bp.log.empty() && !bp.condition.empty() && bp.condition.find('\n') != std::string::npos) {
        return Error::make("invalid_argument", "a condition is one expression");
    }
    bp.id = nextId_++;
    bp.requestedLine = bp.line;
    bp.hits = 0;
    // Verify against the scripts compiled so far: move to the first line with code at or after the request.
    rt_.compileScripts();
    int best = 0;
    std::set<std::string> scripts;
    rt_.scene().registry().each<Behavior>([&](ecs::Entity, Behavior& b) {
        for (const auto& s : b.scripts) {
            if (!s.program) continue;
            for (const auto& P : s.program->protos) {
                const std::string name = P.file.empty() ? s.name : P.file;
                scripts.insert(name);
                if (!bp.script.empty() && name != bp.script) continue;
                for (const auto& loc : P.locs) {
                    if (loc.line >= bp.line && (best == 0 || loc.line < best)) best = loc.line;
                }
            }
        }
    });
    if (best) {
        bp.line = best;
        bp.verified = true;
    } else {
        std::vector<std::string> names(scripts.begin(), scripts.end());
        std::string guess = bp.script.empty() ? std::string() : str::closest(bp.script, names, 3);
        if (!bp.script.empty() && !scripts.count(bp.script) && !guess.empty()) {
            return Error::make("unknown_script", "no script named '" + bp.script + "' is running", "did you mean '" + guess + "'?");
        }
        bp.message = scripts.count(bp.script) || bp.script.empty() ? "no code at or after line " + std::to_string(bp.line) + " yet"
                                                                    : "no script named '" + bp.script + "' is loaded yet (kept, checked again when it runs)";
    }
    breakpoints_.push_back(bp);
    refreshActive();
    return bp;
}

bool Debugger::clearBreakpoint(int id) {
    const size_t before = breakpoints_.size();
    std::erase_if(breakpoints_, [&](const Breakpoint& b) { return b.id == id; });
    refreshActive();
    return breakpoints_.size() != before;
}

size_t Debugger::clearBreakpoints(const std::string& script) {
    const size_t before = breakpoints_.size();
    std::erase_if(breakpoints_, [&](const Breakpoint& b) { return script.empty() || b.script == script; });
    refreshActive();
    return before - breakpoints_.size();
}

std::vector<Breakpoint> Debugger::breakpoints() const { return breakpoints_; }

void Debugger::setBreakOnError(bool on) {
    breakOnError_ = on;
    refreshActive();
}

void Debugger::refreshActive() {
    lines_.clear();
    for (const auto& b : breakpoints_) {
        if (b.enabled) lines_.insert(b.line);
    }
    active_ = !lines_.empty() || breakOnError_ || pauseRequested_ || step_ != StepMode::Continue;
}

// --- control ----------------------------------------------------------------------------------

void Debugger::requestPause() {
    pauseRequested_ = true;
    refreshActive();
}

Status Debugger::resume(StepMode how) {
    if (!paused_) return Error::make("not_paused", "the Wander debugger is not paused", "wander_debug_state shows where it is");
    step_ = how;
    if (!frames_.empty()) {
        stepRun_ = frames_.back().run;
        stepDepth_ = frames_.size();
    }
    stepOrphaned_ = false;
    refreshActive();
    if (host_.drive) host_.drive();
    return {};
}

void Debugger::abort() {
    if (!paused_) return;
    aborting_ = true;
    step_ = StepMode::Continue;
    if (host_.drive) host_.drive();
}

void Debugger::onReset() {
    pauseRequested_ = false;
    step_ = StepMode::Continue;
    stepOrphaned_ = false;
    errorReported_ = false;
    for (auto& b : breakpoints_) b.hits = 0;
    refreshActive();
}

bool Debugger::waitForStop(uint64_t after, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return stops_.load() > after; });
}

// --- VM hooks -----------------------------------------------------------------------------------

void Debugger::enterFrame(ExecState& st, int proto, Value* regs, const size_t* pc) {
    if (frames_.empty()) {
        ++runs_;
        errorReported_ = false;
    }
    frames_.push_back({&st, proto, regs, pc, frames_.empty() ? runs_ : frames_.back().run});
}

void Debugger::leaveFrame() {
    if (frames_.empty()) return;
    const uint64_t run = frames_.back().run;
    frames_.pop_back();
    // A step whose run ended stops at the next statement anywhere.
    if ((step_ == StepMode::Over || step_ == StepMode::Out) && run == stepRun_ && (frames_.empty() || frames_.back().run != run)) {
        stepOrphaned_ = true;
    }
}

std::string Debugger::scriptOf(const Frame& f) const {
    const Proto& P = f.st->prog->protos[f.proto];
    if (!P.file.empty()) return P.file;
    if (f.st->inst) return f.st->inst->scriptName;
    if (f.st->scriptName) return *f.st->scriptName;
    return {};
}

int Debugger::lineOf(const Frame& f) const {
    const Proto& P = f.st->prog->protos[f.proto];
    if (P.locs.empty()) return 0;
    size_t at = *f.pc;
    if (&f != &frames_.back() && at > 0) --at;  // outer frames: their pc is past the call
    return P.locs[std::min(at, P.locs.size() - 1)].line;
}

bool Debugger::breakpointHit(Breakpoint& bp, const Frame& f) {
    if (!bp.condition.empty()) {
        auto v = evaluate(f, bp.condition, true);
        if (!v) {
            bp.message = "condition failed: " + v.error().message;
            return true;  // stop and show it, rather than silently never stopping
        }
        if (!truthy(f.st->scene, *v)) return false;
    }
    ++bp.hits;
    return hitPasses(bp.hitCondition, bp.hits);
}

std::string Debugger::interpolate(const Frame& f, const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') {
            out += text[i];
            continue;
        }
        size_t close = text.find('}', i);
        if (close == std::string::npos) {
            out += text.substr(i);
            break;
        }
        auto v = evaluate(f, text.substr(i + 1, close - i - 1), true);
        out += v ? displayValue(f.st->scene, *v) : "<" + v.error().message + ">";
        i = close;
    }
    return out;
}

void Debugger::statement(ExecState& st, int proto, size_t pc) {
    if (frames_.empty()) return;
    const Frame& f = frames_.back();
    std::string reason;
    int bpId = -1;
    if (pauseRequested_) {
        reason = "pause";
    } else if (step_ != StepMode::Continue) {
        const size_t depth = frames_.size();
        const bool same = f.run == stepRun_;
        if (step_ == StepMode::Into || stepOrphaned_ || (step_ == StepMode::Over && same && depth <= stepDepth_) ||
            (step_ == StepMode::Out && same && depth < stepDepth_)) {
            reason = "step";
        }
    }
    const Proto& P = st.prog->protos[proto];
    if (!lines_.empty() && pc < P.locs.size() && lines_.count(P.locs[pc].line)) {
        const int line = P.locs[pc].line;
        const std::string script = scriptOf(f);
        for (auto& bp : breakpoints_) {
            if (!bp.enabled || bp.line != line || (!bp.script.empty() && bp.script != script) || (bp.entity && bp.entity != st.self)) continue;
            if (!breakpointHit(bp, f)) continue;
            bp.verified = true;
            if (!bp.log.empty()) {  // a logpoint never stops
                rt_.log(st.self, interpolate(f, bp.log), "logpoint");
                continue;
            }
            if (reason.empty() || reason == "step") {
                reason = "breakpoint";
                bpId = bp.id;
            }
        }
    }
    if (reason.empty()) return;
    stopHere(reason, bpId, bpId >= 0 ? [&] {
        for (const auto& b : breakpoints_) {
            if (b.id == bpId) return b.message;
        }
        return std::string();
    }() : std::string());
}

void Debugger::runtimeError(ExecState&, int, size_t, const std::string& message) {
    if (!breakOnError_ || errorReported_ || frames_.empty()) return;
    errorReported_ = true;
    stopHere("error", -1, message);
}

void Debugger::stopHere(const std::string& reason, int breakpoint, const std::string& message) {
    const bool canWait = onStop || (host_.canPause && host_.canPause() && host_.waitWhilePaused);
    if (!canWait) return;  // nowhere to wait (a tool running a function on the main thread): run on, stop later
    pauseRequested_ = false;
    step_ = StepMode::Continue;
    stepOrphaned_ = false;
    refreshActive();
    stop_ = std::make_unique<Stop>(Stop{reason, breakpoint, message});
    StackTopGuard top(*rt_.impl_, frames_);
    {
        std::lock_guard lock(mutex_);
        paused_ = true;
        ++stops_;
    }
    cv_.notify_all();
    if (host_.event) {
        Json e = state();
        e["type"] = "wander.paused";
        host_.event(e);
    }
    if (onStop) {
        const StepMode next = onStop(state());
        paused_ = false;
        step_ = next;
        stepRun_ = frames_.back().run;
        stepDepth_ = frames_.size();
        refreshActive();
    } else {
        host_.waitWhilePaused();  // returns after resume() or abort() on the main thread
        paused_ = false;
    }
    if (host_.event) host_.event(Json::object({{"type", "wander.resumed"}, {"action", stepName(step_)}, {"aborted", aborting_}}));
    if (aborting_) {
        aborting_ = false;
        throw DebugAbort{};
    }
}

// --- inspection -------------------------------------------------------------------------------

Json Debugger::state() const {
    Json bps = Json::array();
    for (const auto& b : breakpoints_) bps.push(b.toJson());
    Json j = Json::object({{"paused", paused_.load()},
                           {"breakOnError", breakOnError_},
                           {"pauseRequested", pauseRequested_},
                           {"stops", stops_.load()},
                           {"breakpoints", bps}});
    if (paused_ && stop_ && !frames_.empty()) {
        j["reason"] = stop_->reason;
        if (stop_->breakpoint >= 0) j["breakpoint"] = stop_->breakpoint;
        if (!stop_->message.empty()) j["message"] = stop_->message;
        j["where"] = frameJson(frames_.size() - 1, false);
    }
    return j;
}

Json Debugger::frameJson(size_t index, bool vars) const {
    const Frame& f = frames_[index];
    const Program& prog = *f.st->prog;
    const Proto& P = prog.protos[f.proto];
    const bool innermost = index + 1 == frames_.size();
    size_t at = *f.pc;
    if (!innermost && at > 0) --at;
    const SourceLoc loc = P.locs.empty() ? SourceLoc{} : P.locs[std::min(at, P.locs.size() - 1)];
    Scene& scene = f.st->scene;
    Json j = Json::object({{"frame", static_cast<int>(frames_.size() - 1 - index)},
                           {"function", P.name},
                           {"script", scriptOf(f)},
                           {"line", loc.line},
                           {"column", loc.column}});
    if (!P.file.empty()) j["file"] = P.file;
    if (f.st->self && scene.exists(f.st->self)) {
        j["entity"] = Json::object({{"id", f.st->self}, {"name", scene.record(f.st->self)->name}});
    }
    if (P.behavior >= 0) {
        j["behavior"] = prog.behaviors[P.behavior].name;
        if (f.st->inst && P.behavior < static_cast<int>(f.st->inst->behaviors.size())) {
            const BehaviorRun& br = f.st->inst->behaviors[P.behavior];
            if (br.state >= 0) j["state"] = prog.behaviors[P.behavior].states[br.state].name;
        }
    }
    if (!vars) return j;
    auto value = [&](const Value& v) { return Json::object({{"value", toJson(v)}, {"display", displayValue(scene, v)}}); };
    Json locals = Json::array(), args = Json::array();
    std::set<std::string> seen;
    std::vector<const Proto::LocalVar*> live;
    for (const auto& l : P.locals) {
        if (l.startPc <= at && at < l.endPc) live.push_back(&l);
    }
    std::sort(live.begin(), live.end(), [](auto* a, auto* b) { return a->startPc > b->startPc; });  // innermost first
    for (const auto* l : live) {
        if (!seen.insert(l->name).second) continue;
        Json e = value(f.regs[l->reg]);
        e["name"] = l->name;
        (l->reg < P.numParams ? args : locals).push(std::move(e));
    }
    j["args"] = std::move(args);
    j["locals"] = std::move(locals);
    if (f.st->self && scene.exists(f.st->self)) {
        Json self = Json::object();
        auto& impl = *rt_.impl_;
        if (auto it = impl.vars.find(f.st->self); it != impl.vars.end()) {
            for (const auto& slot : it->second.slots) self[*slot.name] = value(slot.value);
        }
        for (const auto& [k, v] : scene.record(f.st->self)->vars.members()) {
            if (!self.contains(k)) self[k] = Json::object({{"value", v}, {"display", v.isString() ? v.asString() : v.dump()}});
        }
        j["self"] = std::move(self);
    }
    return j;
}

Json Debugger::stack(bool vars) const {
    Json frames = Json::array();
    for (size_t i = frames_.size(); i-- > 0;) frames.push(frameJson(i, vars));
    Json globals = Json::object({{"time", rt_.time()}, {"frame", rt_.frame()}, {"unscaled_time", rt_.unscaledTime()}});
    if (!frames_.empty()) globals["dt"] = static_cast<double>(frames_.back().st->dt);
    return Json::object({{"paused", paused_.load()}, {"frames", frames}, {"globals", globals}});
}

Result<Value> Debugger::evaluate(const Frame& f, const std::string& expression, bool guard) {
    const Program& prog = *f.st->prog;
    const Proto& P = prog.protos[f.proto];
    size_t at = *f.pc;
    if (&f != &frames_.back() && at > 0) --at;
    // In scope: live locals (innermost wins), then the behavior's vars.
    std::vector<std::string> names;
    std::vector<Value> values;
    std::vector<const Proto::LocalVar*> live;
    for (const auto& l : P.locals) {
        if (l.startPc <= at && at < l.endPc) live.push_back(&l);
    }
    std::sort(live.begin(), live.end(), [](auto* a, auto* b) { return a->startPc > b->startPc; });
    for (const auto* l : live) {
        if (std::find(names.begin(), names.end(), l->name) != names.end()) continue;
        names.push_back(l->name);
        values.push_back(f.regs[l->reg]);
    }
    auto& impl = *rt_.impl_;
    if (P.behavior >= 0 && f.st->self && f.st->scene.exists(f.st->self)) {
        for (const auto& v : prog.behaviors[P.behavior].vars) {
            if (std::find(names.begin(), names.end(), v.name) != names.end()) continue;
            names.push_back(v.name);
            values.push_back(getEntityVar(impl, f.st->scene, f.st->self, v.sym));
        }
    }
    std::string params;
    for (const auto& n : names) params += (params.empty() ? "" : ", ") + n;
    const std::string source = "fn __watch(" + params + ")\n  return (" + expression + ")\nend\n";
    std::shared_ptr<const Program> program;
    if (auto it = evalCache_.find(source); it != evalCache_.end()) {
        program = it->second;
    } else {
        CompileResult cr = compile(source, rt_.compileOptions());
        if (!cr.ok()) {
            std::string why = "does not compile";
            for (const auto& d : cr.diagnostics) {
                if (d.severity == Severity::Error) {
                    why = d.message;
                    break;
                }
            }
            return Error::make("eval_error", "'" + expression + "': " + why);
        }
        program = cr.program;
        if (evalCache_.size() > 256) evalCache_.clear();
        evalCache_[source] = program;
    }
    StackTopGuard top(impl, frames_);
    const std::string savedGuard = rt_.sceneWriteGuard();
    if (guard) rt_.setSceneWriteGuard("the debugger only evaluates (use wander_set_var to change a variable)");
    evaluating_ = true;
    Runtime::FunctionCall call;
    call.budget = 100000;
    call.self = f.st->self;
    call.scriptName = "debugger";
    Runtime::FunctionResult r = rt_.callFunction(program, "__watch", std::move(values), call);
    evaluating_ = false;
    rt_.setSceneWriteGuard(savedGuard);
    if (!r.ok) return Error::make("eval_error", "'" + expression + "': " + r.error);
    return std::move(r.value);
}

Result<Json> Debugger::eval(int frame, const std::string& expression) {
    if (!paused_) return Error::make("not_paused", "expressions are evaluated in a paused frame", "set a breakpoint or wander_pause first");
    if (frame < 0 || frame >= static_cast<int>(frames_.size())) {
        return Error::make("invalid_argument", "no frame " + std::to_string(frame) + " (0.." + std::to_string(frames_.size() - 1) + ")");
    }
    const Frame& f = frames_[frames_.size() - 1 - static_cast<size_t>(frame)];
    auto v = evaluate(f, expression, true);
    if (!v) return v.error();
    return Json::object({{"value", toJson(*v)}, {"display", displayValue(f.st->scene, *v)}, {"type", typeName(v->type())}});
}

Status Debugger::setVariable(int frame, const std::string& name, const Json& value) {
    if (!paused_) return Error::make("not_paused", "variables are set in a paused frame", "set a breakpoint or wander_pause first");
    if (frame < 0 || frame >= static_cast<int>(frames_.size())) return Error::make("invalid_argument", "no frame " + std::to_string(frame));
    const Frame& f = frames_[frames_.size() - 1 - static_cast<size_t>(frame)];
    const Proto& P = f.st->prog->protos[f.proto];
    size_t at = *f.pc;
    if (&f != &frames_.back() && at > 0) --at;
    const Proto::LocalVar* best = nullptr;
    std::vector<std::string> names;
    for (const auto& l : P.locals) {
        if (l.startPc <= at && at < l.endPc) {
            names.push_back(l.name);
            if (l.name == name && (!best || l.startPc > best->startPc)) best = &l;
        }
    }
    if (best) {
        f.regs[best->reg] = fromJson(value);
        return {};
    }
    Scene& scene = f.st->scene;
    if (f.st->self && scene.exists(f.st->self)) {
        bool known = scene.record(f.st->self)->vars.contains(name);
        auto& impl = *rt_.impl_;
        if (auto it = impl.vars.find(f.st->self); it != impl.vars.end()) known = known || it->second.find(intern(name)) >= 0;
        if (P.behavior >= 0) {
            for (const auto& v : f.st->prog->behaviors[P.behavior].vars) known = known || v.name == name;
        }
        if (known) {
            setEntityVar(impl, scene, f.st->self, intern(name), fromJson(value));
            mirrorDirtyVars(impl, scene);
            return {};
        }
        for (const auto& [k, v] : scene.record(f.st->self)->vars.members()) names.push_back(k);
    }
    std::string guess = str::closest(name, names, 3);
    return Error::make("unknown_variable", "no local, argument or var named '" + name + "' in this frame",
                       guess.empty() ? "wander_stack lists the frame's variables" : "did you mean '" + guess + "'?");
}

}  // namespace sky::wander
