#pragma once
// Runtime internals shared by the VM (Runtime.cpp), the builtin library and the native
// (AOT) bridge. Not part of the public API.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/wander/Runtime.h"

namespace sky::wander {

struct RuntimeError {
    SourceLoc loc;
    std::string message;
    std::string file;
};

/// How a proto run ended.
struct Outcome {
    enum class Kind { Done, Wait, GoTo, Stop } kind = Kind::Done;
    Value ret;                 // Done (functions)
    int state = -1;            // GoTo
    bool waitFrames = false;   // Wait
    double waitAmount = 0;     // Wait: seconds or frames
    size_t pc = 0;             // Wait: where to resume
};

struct VarSlot {
    uint32_t sym = 0;
    const std::string* name = nullptr;  // interned (stable) name
    Value value;
    bool inScene = false;  // EntityRecord::vars holds this name (we imported or mirrored it)
    size_t sceneIndex = 0; // position hint inside EntityRecord::vars
    bool dirty = false;
};

struct VarTable {
    EntityId entity = kNoEntity;
    std::vector<VarSlot> slots;
    bool anyDirty = false;

    int find(uint32_t sym) const {
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].sym == sym) return static_cast<int>(i);
        }
        return -1;
    }
};

struct BehaviorRun {
    std::vector<int> varSlots;  // declared var -> VarTable slot
    std::vector<double> timers;
    std::vector<uint8_t> fired;
    int state = -1;
    double stateTime = 0;
    bool exiting = false;  // running `on exit` (transitions are ignored)
};

struct Coroutine {
    int behavior = -1;
    int handler = -1;  // index in BehaviorInfo::handlers
    int state = -1;    // owning state (-1: behavior-level handler)
    int proto = -1;
    size_t pc = 0;
    std::vector<Value> regs;
    bool byFrames = false;
    double remaining = 0;  // seconds or frames
    EntityId other = kNoEntity;
    std::optional<Runtime::Contact> contact;  // collide/trigger handlers keep their contact across waits
};

struct PendingEvent {
    uint32_t sym = 0;
    std::string name;
    EntityId target = kNoEntity;
    EntityId other = kNoEntity;
    Value payload;
};

struct Instance {
    std::shared_ptr<const Program> program;
    std::string scriptName;
    size_t scriptIndex = 0;
    EntityId entity = kNoEntity;
    bool started = false;
    VarTable* vars = nullptr;
    std::vector<BehaviorRun> behaviors;
    std::vector<Coroutine> coroutines;
    int transitionsThisTick = 0;
    const NativeProgram* native = nullptr;  // AOT code for this program (refreshed every tick)
    bool dead = false;  // script disabled or replaced during this tick: stop running handlers
    /// Events that arrived while the entity was paused (process mode): delivered when it runs again.
    std::vector<PendingEvent> deferred;
    std::vector<Runtime::Contact> deferredContacts;  // contacts of the tick the pause began, delivered on resume
    bool frameFailed = false;  // an `on frame` handler failed: frame handlers are off for this play session
};

struct InstanceKeyHash {
    size_t operator()(const std::pair<EntityId, size_t>& k) const {
        return std::hash<uint64_t>()(k.first * 1000003ULL + k.second);
    }
};

/// Everything a running proto needs. One per handler run (and per test driver step).
struct ExecState {
    Runtime& rt;
    Runtime::Impl& impl;
    Scene& scene;
    const Program* prog = nullptr;
    const NativeProgram* native = nullptr;
    Instance* inst = nullptr;  // null for test drivers
    int behavior = -1;
    EntityId self = kNoEntity;
    EntityId other = kNoEntity;
    float dt = 0;
    int64_t budget = Runtime::kBudget;
    int depth = 0;
    const InputState* input = nullptr;
    TestHooks* test = nullptr;
    const Runtime::Contact* contact = nullptr;  // collide/trigger handlers
    std::optional<RayHitInfo> lastHit;          // the last raycast() in this run
    const std::string* scriptName = nullptr;  // for log messages
    /// An `on frame` handler: display rate, writes are recorded and undone after the frame,
    /// anything that would change the simulation (vars, waits, spawning, emitting...) is an error.
    bool cosmetic = false;
    double displayTime = 0;  // `time` inside a cosmetic run (the interpolated game time)

    ExecState(Runtime& r, Runtime::Impl& i, Scene& s) : rt(r), impl(i), scene(s) {}
};

struct Runtime::Impl {
    std::unordered_map<std::pair<EntityId, size_t>, Instance, InstanceKeyHash> instances;
    std::unordered_map<EntityId, VarTable> vars;
    std::vector<PendingEvent> pending;
    std::vector<PendingEvent> nextPending;
    std::vector<Runtime::Contact> contacts;      // physics contacts delivered this tick (sorted by receiver)
    std::vector<Runtime::Contact> nextContacts;  // queued by the physics step for the next tick
    std::vector<RuntimeMessage> messages;
    std::vector<EntityId> toDestroy;
    int spawnedThisTick = 0;
    uint64_t lastRevision = 0;
    bool revisionValid = false;
    const InputState* input = nullptr;
    float dt = 1.f / 60.f;

    // Register stack shared by all frames (one activation chain at a time).
    std::vector<Value> stack;
    size_t stackTop = 0;

    // Compile cache: identical sources share one program.
    struct CacheEntry {
        CompileResult result;
        uint64_t epoch = 0;
    };
    std::unordered_map<std::string, CacheEntry> compileCache;

    // Modules read from the project.
    struct ModuleFile {
        std::string text;
        int64_t mtime = 0;
        bool exists = false;
    };
    std::map<std::string, ModuleFile> modules;
    uint64_t moduleRevision = 1;
    uint64_t scannedBehaviors = ~0ULL;  // Scene::behaviorsRevision() at the last compile scan
    uint64_t scannedEpoch = 0;

    // Component field resolution cache ("light.intensity" -> kind + field).
    struct ResolvedField {
        const ComponentKind* kind = nullptr;
        const FieldInfo* field = nullptr;
    };
    std::unordered_map<std::string, ResolvedField> fieldCache;

    // Undo log of the cosmetic (`on frame`) writes of the current frame (Runtime::revertFrame).
    struct CosmeticUndo {
        EntityId entity = kNoEntity;
        const ComponentKind* kind = nullptr;  // null: the whole Transform
        const FieldInfo* field = nullptr;
        Transform transform;
        unsigned char raw[16] = {};
        Json json;
        bool viaJson = false;
    };
    std::vector<CosmeticUndo> cosmeticUndo;
};

// --- shared helpers (Runtime.cpp) --------------------------------------------------
[[noreturn]] void raise(SourceLoc loc, std::string message);
inline bool truthy(const Scene& scene, const Value& v) {
    switch (v.type()) {
        case VType::Bool: return v.b();
        case VType::Number: return v.num() != 0;
        case VType::None: return false;
        case VType::Entity: return scene.exists(v.e());
        default: return v.truthyData();
    }
}
std::string displayValue(const Scene& scene, const Value& v);
Vec3 worldPosition(const Scene& scene, EntityId id);

VarTable& varTable(Runtime::Impl& impl, Scene& scene, EntityId id);
VarSlot makeSlot(uint32_t sym);
/// Instances of an entity (one per script), in script order.
template <typename Fn>
void forEachInstance(Runtime::Impl& impl, const Scene& scene, EntityId e, Fn&& fn);
/// Entity var by name (none if unset); imports outside edits lazily.
Value getEntityVar(Runtime::Impl& impl, Scene& scene, EntityId id, uint32_t sym);
void setEntityVar(Runtime::Impl& impl, Scene& scene, EntityId id, uint32_t sym, Value v);
/// Copies var values scripts changed into the scene's entity records (EntityRecord::vars).
void mirrorDirtyVars(Runtime::Impl& impl, Scene& scene);

/// Runs a proto (natively when compiled code is attached) from `pc`.
Outcome runProto(ExecState& st, int proto, Value* regs, size_t pc);
/// Executes one instruction in VM semantics (used by native code for its slow paths).
/// Returns false if the instruction ended the run (outcome filled in).
bool execOne(ExecState& st, int proto, Value* regs, size_t& pc, Outcome& out);
/// Executes the straight-line instructions [pc, end). False if the run ended.
bool execRange(ExecState& st, int proto, Value* regs, size_t& pc, size_t end, Outcome& out);

/// Native code entry (Aot.cpp): runs `proto` natively if compiled code is attached.
/// Returns false when there is no native code for it (the VM runs it instead).
bool aotRun(ExecState& st, int proto, Value* regs, size_t pc, Outcome& out);

ExecState& CallContextAccess(CallContext& c);

template <typename Fn>
void forEachInstance(Runtime::Impl& impl, const Scene& scene, EntityId e, Fn&& fn) {
    const Behavior* b = scene.get<Behavior>(e);
    if (!b) return;
    for (size_t si = 0; si < b->scripts.size(); ++si) {
        auto it = impl.instances.find({e, si});
        if (it != impl.instances.end()) fn(it->second);
    }
}

/// Records the current value of a component field (or the whole transform when `kind` is null) before
/// an `on frame` handler changes it; Runtime::revertFrame puts it back.
void recordCosmeticWrite(ExecState& st, EntityId e, const ComponentKind* kind, const FieldInfo* field);

/// Text for UTF-8 code points of a string (iteration, indexing, length).
std::vector<std::string> utf8Chars(const std::string& s);
size_t utf8Length(const std::string& s);

}  // namespace sky::wander
