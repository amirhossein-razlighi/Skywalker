// Calling a Wander function directly, outside the tick loop: the entry point of tools defined in
// Wander (docs/CUSTOM_TOOLS.md). Same VM, same budget accounting as handler runs.

#include <algorithm>
#include <exception>

#include "RuntimeInternal.h"
#include "skywalker/wander/Runtime.h"

namespace sky::wander {

namespace {
constexpr size_t kCallStackSize = 1u << 16;
}

Runtime::FunctionResult Runtime::callFunction(const std::shared_ptr<const Program>& program, std::string_view name,
                                              std::vector<Value> args, const FunctionCall& options) {
    FunctionResult result;
    if (!program) {
        result.error = "the program did not compile";
        return result;
    }
    const FnInfo* fn = nullptr;
    for (const auto& f : program->functions) {
        if (f.name == name && f.behavior < 0) fn = &f;
    }
    if (!fn) {
        result.error = "no file-level function named " + std::string(name);
        return result;
    }
    if (args.size() != fn->params.size()) {
        result.error = std::string(name) + "() takes " + std::to_string(fn->params.size()) + " argument(s), got " +
                       std::to_string(args.size());
        result.loc = fn->loc;
        return result;
    }
    Impl& impl = *impl_;
    if (impl.stack.empty()) impl.stack.resize(kCallStackSize);
    // A fresh view of entity vars: the scene is the source of truth between calls.
    if (!ticking_) impl.vars.clear();
    const Proto& P = program->protos[fn->proto];
    if (impl.stackTop + static_cast<size_t>(P.numRegs) > impl.stack.size()) {
        result.error = "out of stack space calling " + std::string(name) + "()";
        return result;
    }
    Value* R = impl.stack.data() + impl.stackTop;
    const size_t savedTop = impl.stackTop;
    impl.stackTop += static_cast<size_t>(P.numRegs);
    for (size_t i = 0; i < args.size(); ++i) R[i] = std::move(args[i]);

    ExecState st(*this, impl, scene_);
    st.prog = program.get();
    st.behavior = -1;
    st.self = options.self;
    st.dt = 0.f;
    st.budget = options.budget;
    st.scriptName = &options.scriptName;
    try {
        Outcome out = runProto(st, fn->proto, R, 0);
        result.ok = true;
        if (out.kind == Outcome::Kind::Done) result.value = std::move(out.ret);
    } catch (const RuntimeError& err) {
        result.error = err.message;
        result.loc = err.loc;
        result.file = err.file;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    result.instructions = options.budget - std::max<int64_t>(st.budget, 0);
    for (int i = 0; i < P.numRegs; ++i) R[i] = Value();
    impl.stackTop = savedTop;

    if (!ticking_) {
        // What the function changed becomes visible in the scene right away.
        mirrorDirtyVars(impl, scene_);
        std::vector<EntityId> doomed;
        doomed.swap(impl.toDestroy);
        for (EntityId id : doomed) {
            if (scene_.exists(id)) scene_.destroy(id);
        }
        impl.spawnedThisTick = 0;
    }
    return result;
}

}  // namespace sky::wander
