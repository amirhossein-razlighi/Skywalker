// Native modules (project C++ against skywalker/native/sdk.h) and AOT-compiled behaviors.

#include "skywalker/native/NativeModules.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "Process.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/native/sdk.h"
#include "skywalker/wander/Aot.h"
#include "skywalker/wander/Runtime.h"

namespace sky {

namespace fs = std::filesystem;
using wander::Value;

namespace {
const char* kSdkHeader =
#include "native_sdk_h.inc"
    ;
const char* kValueHeader =
#include "native_value_h.inc"
    ;

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeIfChanged(const fs::path& p, const std::string& text) {
    std::error_code ec;
    if (fs::exists(p, ec) && readText(p) == text) return true;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}

}  // namespace

}  // namespace sky

// ---------------------------------------------------------------------------
// The SDK host API
// ---------------------------------------------------------------------------

struct SkyWorld {
    sky::Engine* engine = nullptr;
    sky::Scene* scene = nullptr;
    sky::wander::Runtime* runtime = nullptr;
    const sky::wander::InputState* input = nullptr;
    double dt = 1.0 / 60.0;
    std::string scratch;
    std::string lastError;
};

struct SkyCall {
    sky::wander::CallContext* ctx = nullptr;
    SkyWorld* world = nullptr;
    std::string failure;
};

namespace sky {

struct NativeBuiltinRecord {
    SkyBuiltinFn fn = nullptr;
    void* user = nullptr;
    std::string name;
    bool alive = true;
};

struct SystemRecord {
    std::string name;
    SkySystemFn fn = nullptr;
    void* user = nullptr;
};

}  // namespace sky

struct SkyModule {
    std::string name;
    std::vector<sky::wander::BuiltinDef> builtins;
    std::vector<std::shared_ptr<sky::NativeBuiltinRecord>> records;
    std::vector<sky::SystemRecord> systems;
    bool initializing = true;
    std::string error;
};

namespace sky {

namespace {

Value& V(SkyValue* v) { return Value::fromAbi(v); }
const Value& V(const SkyValue* v) { return Value::fromAbi(v); }

SkyValue toAbi(Value v) {
    SkyValue out;
    new (&out) Value(std::move(v));  // Value is layout-identical to SkyValue
    return out;
}

void fail(SkyWorld* w, std::string msg) { w->lastError = std::move(msg); }

bool entityOk(SkyWorld* w, SkyEntity e) {
    if (w->scene->exists(e)) return true;
    fail(w, "no entity #" + std::to_string(e));
    return false;
}

int getVec(SkyWorld* w, SkyEntity e, float out[3], int which) {
    if (!entityOk(w, e)) return -1;
    const Transform* t = w->scene->get<Transform>(e);
    Vec3 v = t ? (which == 0 ? t->position : which == 1 ? t->rotation : t->scale) : (which == 2 ? Vec3{1, 1, 1} : Vec3{});
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    return 0;
}

int setVec(SkyWorld* w, SkyEntity e, const float v[3], int which) {
    if (!entityOk(w, e)) return -1;
    Transform& t = w->scene->add<Transform>(e);
    (which == 0 ? t.position : which == 1 ? t.rotation : t.scale) = {v[0], v[1], v[2]};
    w->scene->markDirty();
    return 0;
}

wander::TypeSet parseParamType(const std::string& t) {
    wander::TypeSet ts = wander::parseTypeName(str::trim(t));
    return ts ? ts : wander::kTAny;
}

std::vector<wander::BuiltinParam> parseParams(const char* spec) {
    std::vector<wander::BuiltinParam> out;
    if (!spec) return out;
    for (const auto& part : str::split(spec, ',')) {
        std::string p = str::trim(part);
        if (p.empty()) continue;
        wander::BuiltinParam bp;
        auto colon = p.find(':');
        bp.name = str::trim(p.substr(0, colon));
        if (!bp.name.empty() && bp.name.back() == '?') {
            bp.optional = true;
            bp.name.pop_back();
        }
        if (colon != std::string::npos) bp.type = parseParamType(p.substr(colon + 1));
        out.push_back(std::move(bp));
    }
    return out;
}

Value nativeTrampoline(wander::CallContext& c);

int apiRegisterBuiltin(SkyModule* m, const SkyBuiltinDesc* d) {
    if (!m || !d || !d->name || !d->fn) return -1;
    if (!m->initializing) {
        m->error = "register_builtin can only be called inside sky_module_init";
        return -1;
    }
    std::string name = d->name;
    bool valid = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
    for (char ch : name) valid = valid && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
    if (!valid) {
        m->error = "invalid builtin name '" + name + "' (use snake_case letters, digits and _)";
        return -1;
    }
    auto rec = std::make_shared<NativeBuiltinRecord>();
    rec->fn = d->fn;
    rec->user = d->user;
    rec->name = name;
    wander::BuiltinDef def;
    def.name = name;
    def.params = parseParams(d->params);
    int declared = static_cast<int>(def.params.size());
    // min/max from the descriptor win over the params string.
    if (d->max_args < 0) def.variadic = true;
    while (static_cast<int>(def.params.size()) < std::max(d->min_args, d->max_args)) {
        def.params.push_back({"arg" + std::to_string(def.params.size() + 1), wander::kTAny, false});
    }
    for (int i = 0; i < static_cast<int>(def.params.size()); ++i) def.params[i].optional = i >= d->min_args;
    if (d->max_args >= 0 && static_cast<int>(def.params.size()) > d->max_args) def.params.resize(static_cast<size_t>(d->max_args));
    (void)declared;
    def.returns = d->returns ? parseParamType(d->returns) : wander::kTAny;
    def.category = d->category ? d->category : "native";
    def.doc = d->doc ? d->doc : "";
    def.example = d->example ? d->example : "";
    def.fn = nativeTrampoline;
    def.user = rec.get();
    def.keepAlive = rec;
    def.owner = "native:" + m->name;
    m->builtins.push_back(std::move(def));
    m->records.push_back(std::move(rec));
    return 0;
}

int apiRegisterSystem(SkyModule* m, const char* name, SkySystemFn fn, void* user) {
    if (!m || !fn) return -1;
    if (!m->initializing) {
        m->error = "register_system can only be called inside sky_module_init";
        return -1;
    }
    m->systems.push_back({name ? name : "system", fn, user});
    return 0;
}

void apiLog(SkyWorld* w, const char* text) {
    if (w->runtime) w->runtime->log(kNoEntity, text ? text : "", "native");
}
const char* apiLastError(SkyWorld* w) { return w->lastError.c_str(); }
SkyWorld* apiCallWorld(SkyCall* c) { return c->world; }
SkyEntity apiCallSelf(SkyCall* c) { return c->ctx ? c->ctx->self() : kNoEntity; }
void apiCallFail(SkyCall* c, const char* message) {
    if (c->failure.empty()) c->failure = message && *message ? message : "native builtin failed";
}
double apiTime(SkyWorld* w) { return w->runtime ? w->runtime->time() : 0.0; }
double apiDt(SkyWorld* w) { return w->dt; }
uint64_t apiFrame(SkyWorld* w) { return w->runtime ? w->runtime->frame() : 0; }
double apiRandom(SkyWorld* w) { return w->runtime ? w->runtime->rng().nextFloat() : 0.0; }
int apiKeyHeld(SkyWorld* w, const char* key) {
    if (!w->input || !key) return 0;
    return w->input->held.count(str::lower(key)) ? 1 : 0;
}

SkyEntity apiFind(SkyWorld* w, const char* name) { return name ? w->scene->find(name) : kNoEntity; }
int apiExists(SkyWorld* w, SkyEntity e) { return w->scene->exists(e) ? 1 : 0; }
size_t apiEntityCount(SkyWorld* w) { return w->scene->entities().size(); }
SkyEntity apiEntityAt(SkyWorld* w, size_t i) {
    const auto& es = w->scene->entities();
    return i < es.size() ? es[i] : kNoEntity;
}
size_t apiFindTagged(SkyWorld* w, const char* tag, SkyEntity* out, size_t cap) {
    if (!tag) return 0;
    auto ids = w->scene->findTagged(tag);
    for (size_t i = 0; i < ids.size() && i < cap && out; ++i) out[i] = ids[i];
    return ids.size();
}
int apiHasTag(SkyWorld* w, SkyEntity e, const char* tag) {
    const EntityRecord* r = w->scene->record(e);
    if (!r || !tag) return 0;
    return std::find(r->tags.begin(), r->tags.end(), tag) != r->tags.end() ? 1 : 0;
}
const char* apiName(SkyWorld* w, SkyEntity e) {
    const EntityRecord* r = w->scene->record(e);
    w->scratch = r ? r->name : "";
    return w->scratch.c_str();
}
SkyEntity apiSpawn(SkyWorld* w, const char* what, const float pos[3], const char* name) {
    std::string mesh = what ? what : "cube";
    Vec3 p = pos ? Vec3{pos[0], pos[1], pos[2]} : Vec3{};
    if (str::startsWith(mesh, "prefab:")) {
        if (!w->runtime || !w->runtime->spawnPrefab) {
            fail(w, "prefabs are not available here");
            return kNoEntity;
        }
        auto id = w->runtime->spawnPrefab(mesh.substr(7), p, name ? name : "");
        if (!id) {
            fail(w, id.error().message);
            return kNoEntity;
        }
        return *id;
    }
    const auto& prims = MeshRenderer::primitives();
    if (std::find(prims.begin(), prims.end(), mesh) == prims.end() && !str::startsWith(mesh, "asset:")) {
        fail(w, "unknown mesh '" + mesh + "'");
        return kNoEntity;
    }
    EntityId id = w->scene->create(name && *name ? name : mesh);
    w->scene->add<MeshRenderer>(id).mesh = mesh;
    w->scene->add<Transform>(id).position = p;
    return id;
}
void apiDestroy(SkyWorld* w, SkyEntity e) {
    if (w->runtime) {
        w->runtime->destroyEntity(e);
    } else if (w->scene->exists(e)) {
        w->scene->destroy(e);
    }
}
int apiGetPosition(SkyWorld* w, SkyEntity e, float out[3]) { return getVec(w, e, out, 0); }
int apiSetPosition(SkyWorld* w, SkyEntity e, const float v[3]) { return setVec(w, e, v, 0); }
int apiGetRotation(SkyWorld* w, SkyEntity e, float out[3]) { return getVec(w, e, out, 1); }
int apiSetRotation(SkyWorld* w, SkyEntity e, const float v[3]) { return setVec(w, e, v, 1); }
int apiGetScale(SkyWorld* w, SkyEntity e, float out[3]) { return getVec(w, e, out, 2); }
int apiSetScale(SkyWorld* w, SkyEntity e, const float v[3]) { return setVec(w, e, v, 2); }
int apiWorldPosition(SkyWorld* w, SkyEntity e, float out[3]) {
    if (!entityOk(w, e)) return -1;
    Vec3 p = w->scene->worldMatrix(e).translation();
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    return 0;
}

const ComponentKind* kindOf(SkyWorld* w, const char* component) {
    const ComponentKind* k = component ? w->scene->componentKind(component) : nullptr;
    if (!k) {
        std::string guess = str::closest(component ? component : "", w->scene->componentNames());
        fail(w, std::string("unknown component '") + (component ? component : "") + "'" +
                    (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
    }
    return k;
}

int apiHasComponent(SkyWorld* w, SkyEntity e, const char* component) {
    const ComponentKind* k = kindOf(w, component);
    return k && w->scene->exists(e) && k->has(*w->scene, e) ? 1 : 0;
}
int apiGetField(SkyWorld* w, SkyEntity e, const char* component, const char* field, SkyValue* out) {
    if (!out) return -1;
    *out = sky_none();
    if (!entityOk(w, e)) return -1;
    const ComponentKind* k = kindOf(w, component);
    if (!k) return -1;
    if (!k->has(*w->scene, e)) {
        fail(w, std::string("the entity has no ") + component + " component");
        return -1;
    }
    Json j = k->toJson(*w->scene, e);
    const Json* f = field ? j.find(field) : nullptr;
    if (!f) {
        fail(w, std::string(component) + " has no field '" + (field ? field : "") + "'");
        return -1;
    }
    *out = toAbi(wander::fromJson(*f));
    return 0;
}
int apiSetField(SkyWorld* w, SkyEntity e, const char* component, const char* field, const SkyValue* v) {
    if (!entityOk(w, e) || !field || !v) return -1;
    const ComponentKind* k = kindOf(w, component);
    if (!k) return -1;
    Status s = k->apply(*w->scene, e, Json::object({{field, wander::toJson(V(v))}}));
    if (!s) {
        fail(w, s.error().message);
        return -1;
    }
    w->scene->markDirty();
    return 0;
}
int apiGetComponentJson(SkyWorld* w, SkyEntity e, const char* component, char* buf, size_t cap) {
    if (!entityOk(w, e)) return -1;
    const ComponentKind* k = kindOf(w, component);
    if (!k) return -1;
    std::string text = k->has(*w->scene, e) ? k->toJson(*w->scene, e).dump() : "null";
    if (buf && cap > 0) {
        size_t n = std::min(cap - 1, text.size());
        std::memcpy(buf, text.data(), n);
        buf[n] = 0;
    }
    return static_cast<int>(text.size());
}
int apiPatchComponentJson(SkyWorld* w, SkyEntity e, const char* component, const char* patch) {
    if (!entityOk(w, e) || !patch) return -1;
    auto j = Json::parse(patch);
    if (!j) {
        fail(w, "invalid JSON: " + j.error().message);
        return -1;
    }
    const ComponentKind* k = kindOf(w, component);
    if (!k) return -1;
    Status s = k->apply(*w->scene, e, j.value());
    if (!s) {
        fail(w, s.error().message);
        return -1;
    }
    w->scene->markDirty();
    return 0;
}
int apiGetVar(SkyWorld* w, SkyEntity e, const char* name, SkyValue* out) {
    if (!out) return -1;
    *out = sky_none();
    if (!entityOk(w, e) || !name) return -1;
    if (w->runtime) {
        *out = toAbi(w->runtime->getVar(e, name));
    } else if (const Json* j = w->scene->record(e)->vars.find(name)) {
        *out = toAbi(wander::fromJson(*j));
    }
    return 0;
}
int apiSetVar(SkyWorld* w, SkyEntity e, const char* name, const SkyValue* v) {
    if (!entityOk(w, e) || !name || !v) return -1;
    if (w->runtime) {
        w->runtime->setVar(e, name, V(v));
    } else {
        w->scene->record(e)->vars[name] = wander::toJson(V(v));
    }
    return 0;
}
void apiEmit(SkyWorld* w, const char* name, SkyEntity target, const SkyValue* payload, SkyEntity other) {
    if (!w->runtime || !name) return;
    w->runtime->emit(name, target, payload ? V(payload) : Value(), other);
}
SkyValue apiStringNew(const char* s) { return toAbi(Value::string(s ? s : "")); }
const char* apiStringChars(const SkyValue* v) { return v && v->type == SKY_STRING ? V(v).str().c_str() : nullptr; }
SkyValue apiListNew() { return toAbi(Value::list()); }
size_t apiListLength(const SkyValue* l) { return l && l->type == SKY_LIST ? V(l).items().size() : 0; }
SkyValue apiListGet(const SkyValue* l, size_t i) {
    if (!l || l->type != SKY_LIST || i >= V(l).items().size()) return sky_none();
    return toAbi(V(l).items()[i]);
}
void apiListPush(SkyValue* l, const SkyValue* item) {
    if (!l || l->type != SKY_LIST || !item) return;
    V(l).mutItems().push_back(V(item));
}
SkyValue apiMapNew() { return toAbi(Value::map()); }
SkyValue apiMapGet(const SkyValue* m, const char* key) {
    if (!m || m->type != SKY_MAP || !key) return sky_none();
    const Value* v = V(m).mapObj().find(key);
    return v ? toAbi(*v) : sky_none();
}
void apiMapSet(SkyValue* m, const char* key, const SkyValue* item) {
    if (!m || m->type != SKY_MAP || !key || !item) return;
    V(m).mutMap().set(key, V(item));
}
SkyValue apiCopy(const SkyValue* v) { return v ? toAbi(V(v)) : sky_none(); }
void apiRelease(SkyValue* v) {
    if (v) V(v) = Value();
}

const SkyApi kSdkApi = {
    SKY_SDK_VERSION, sizeof(SkyApi), apiRegisterBuiltin, apiRegisterSystem, apiLog, apiLastError, apiCallWorld, apiCallSelf,
    apiCallFail, apiTime, apiDt, apiFrame, apiRandom, apiKeyHeld, apiFind, apiExists, apiEntityCount, apiEntityAt,
    apiFindTagged, apiHasTag, apiName, apiSpawn, apiDestroy, apiGetPosition, apiSetPosition, apiGetRotation,
    apiSetRotation, apiGetScale, apiSetScale, apiWorldPosition, apiHasComponent, apiGetField, apiSetField,
    apiGetComponentJson, apiPatchComponentJson, apiGetVar, apiSetVar, apiEmit, apiStringNew, apiStringChars,
    apiListNew, apiListLength, apiListGet, apiListPush, apiMapNew, apiMapGet, apiMapSet, apiCopy, apiRelease};

Value nativeTrampoline(wander::CallContext& c) {
    auto* rec = static_cast<NativeBuiltinRecord*>(c.def().user);
    if (!rec || !rec->alive) c.fail(c.def().name + "() belongs to a native module that is no longer loaded");
    SkyWorld world;
    world.scene = &c.scene();
    world.runtime = &c.runtime();
    world.engine = c.service<Engine>();
    world.input = &c.input();
    world.dt = c.dt();
    SkyCall call;
    call.ctx = &c;
    call.world = &world;
    Value result;
    rec->fn(&call, c.args()->abi(), c.argc(), result.abi(), rec->user);
    if (!call.failure.empty()) c.fail(c.def().name + "(): " + call.failure);
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// NativeModules
// ---------------------------------------------------------------------------

struct NativeModules::Impl {
    void* handle = nullptr;
    std::unique_ptr<SkyModule> module;
    std::string loadedLibrary;
    std::string lastBuilt;  // most recent successful build
    std::string lastError;
    BuildResult lastBuild;
    std::unordered_map<uint64_t, std::shared_ptr<const wander::NativeProgram>> aot;
};

NativeModules::NativeModules(Engine& engine, wander::BuiltinRegistry& registry)
    : engine_(engine), registry_(registry), impl_(std::make_unique<Impl>()) {}

NativeModules::~NativeModules() { unload(); }

std::string NativeModules::projectDir() const { return engine_.config().projectDir; }
std::string NativeModules::cacheDir() const { return (fs::path(projectDir()) / ".skywalker" / "cache").string(); }

Json NativeModules::BuildResult::toJson() const {
    Json src = Json::array();
    for (const auto& s : sources) src.push(s);
    Json j = Json::object({{"ok", ok}, {"up_to_date", upToDate}, {"library", library}, {"sources", src},
                           {"diagnostics", diagnostics}, {"ms", ms}});
    if (!ok && !output.empty()) j["output"] = output;
    return j;
}

bool NativeModules::hasSources() const {
    std::error_code ec;
    fs::path dir = fs::path(projectDir()) / "native";
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        auto ext = e.path().extension().string();
        if (ext == ".cpp" || ext == ".cc" || ext == ".c") return true;
    }
    return false;
}

namespace {

struct ModuleConfig {
    std::string name;
    std::vector<std::string> flags, includeDirs, libDirs, libs, frameworks, pkgConfig;
};

Result<ModuleConfig> readConfig(const fs::path& nativeDir, const std::string& fallbackName) {
    ModuleConfig c;
    c.name = fallbackName;
    fs::path p = nativeDir / "module.json";
    std::error_code ec;
    if (!fs::exists(p, ec)) return c;
    auto j = Json::parse(readText(p));
    if (!j) return Error::make("invalid_module_json", "native/module.json: " + j.error().message);
    const Json& m = j.value();
    auto strings = [&](const char* key, std::vector<std::string>& out) -> Status {
        const Json& a = m.get(key);
        if (a.isNull()) return {};
        if (!a.isArray()) return Error::make("invalid_module_json", std::string("native/module.json: \"") + key + "\" must be an array of strings");
        for (const auto& s : a.elements()) {
            if (!s.isString()) return Error::make("invalid_module_json", std::string("native/module.json: \"") + key + "\" must contain strings");
            out.push_back(s.asString());
        }
        return {};
    };
    if (m.get("name").isString() && !m.get("name").asString().empty()) c.name = m.get("name").asString();
    for (auto [key, out] : std::initializer_list<std::pair<const char*, std::vector<std::string>*>>{
             {"flags", &c.flags}, {"include_dirs", &c.includeDirs}, {"lib_dirs", &c.libDirs}, {"libs", &c.libs},
             {"frameworks", &c.frameworks}, {"pkg_config", &c.pkgConfig}}) {
        if (Status s = strings(key, *out); !s) return s.error();
    }
    for (char& ch : c.name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
    }
    return c;
}

}  // namespace

Result<NativeModules::BuildResult> NativeModules::build(bool force) {
    BuildResult r;
    fs::path project = projectDir();
    fs::path nativeDir = project / "native";
    std::error_code ec;
    if (!hasSources()) {
        return Error::make("no_native_sources", "the project has no native/*.cpp files",
                           "create one with native_template, then call native_build");
    }
    std::string folder = fs::absolute(project, ec).filename().string();
    if (folder.empty() || folder == ".") folder = "module";
    auto cfg = readConfig(nativeDir, folder);
    if (!cfg) return cfg.error();

    std::vector<fs::path> sources;
    for (const auto& e : fs::directory_iterator(nativeDir, ec)) {
        auto ext = e.path().extension().string();
        if (ext == ".cpp" || ext == ".cc" || ext == ".c") sources.push_back(e.path());
    }
    std::sort(sources.begin(), sources.end());
    // SDK headers live in the project cache (embedded in the engine; never stale).
    fs::path sdkRoot = fs::path(cacheDir()) / "sdk";
    if (!writeIfChanged(sdkRoot / "skywalker" / "native" / "sdk.h", kSdkHeader) ||
        !writeIfChanged(sdkRoot / "skywalker" / "native" / "value.h", kValueHeader)) {
        return Error::make("io_error", "cannot write the SDK headers to " + sdkRoot.string());
    }
    std::string cxx = wander::findCxxCompiler();
    if (cxx.empty()) {
        return Error::make("no_compiler", "no C++ compiler found (clang++)", "install the Xcode command line tools: xcode-select --install");
    }

    std::vector<std::string> pkgFlags;
    for (const auto& pkg : cfg->pkgConfig) {
        auto pr = native::runProcess({"pkg-config", "--cflags", "--libs", pkg}, project.string(), 60);
        if (!pr || pr->exitCode != 0) {
            return Error::make("pkg_config_failed", "pkg-config could not find '" + pkg + "'",
                               pr ? str::trim(pr->output) : pr.error().message);
        }
        for (const auto& f : str::split(str::trim(pr->output), ' ')) {
            if (!str::trim(f).empty()) pkgFlags.push_back(str::trim(f));
        }
    }

    // The library name hashes everything that affects the build.
    uint64_t h = wander::fnv1a(std::to_string(SKY_SDK_VERSION) + cxx);
    for (const auto& s : sources) h = wander::fnv1a(readText(s), wander::fnv1a(s.filename().string(), h));
    h = wander::fnv1a(readText(nativeDir / "module.json"), h);
    for (const auto& f : pkgFlags) h = wander::fnv1a(f, h);
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(h));
    fs::path outDir = fs::path(cacheDir()) / "native";
    fs::create_directories(outDir, ec);
    r.library = (outDir / (cfg->name + "_" + hash + ".dylib")).string();
    for (const auto& s : sources) r.sources.push_back(fs::relative(s, project, ec).string());

    if (!force && fs::exists(r.library, ec)) {
        r.ok = true;
        r.upToDate = true;
        impl_->lastBuilt = r.library;
        impl_->lastBuild = r;
        return r;
    }
    std::vector<std::string> argv{cxx, "-std=c++20", "-O2", "-shared", "-fPIC", "-fvisibility=hidden",
                                  "-I" + sdkRoot.string(), "-I" + nativeDir.string()};
    for (const auto& d : cfg->includeDirs) argv.push_back("-I" + (fs::path(d).is_absolute() ? d : (project / d).string()));
    for (const auto& f : cfg->flags) argv.push_back(f);
    for (const auto& s : sources) argv.push_back(s.string());
    std::string tmp = r.library + ".tmp" + std::to_string(::getpid());
    argv.insert(argv.end(), {"-o", tmp});
    for (const auto& d : cfg->libDirs) argv.push_back("-L" + (fs::path(d).is_absolute() ? d : (project / d).string()));
    for (const auto& l : cfg->libs) argv.push_back(l.rfind("-", 0) == 0 || l.find('/') != std::string::npos ? l : "-l" + l);
    for (const auto& fw : cfg->frameworks) argv.insert(argv.end(), {"-framework", fw});
    argv.insert(argv.end(), pkgFlags.begin(), pkgFlags.end());

    auto t0 = std::chrono::steady_clock::now();
    auto pr = native::runProcess(argv, project.string(), 600);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!pr) return pr.error();
    r.output = pr->output.size() > 8000 ? pr->output.substr(0, 8000) + "\n..." : pr->output;
    for (auto d : native::parseCompilerOutput(pr->output)) {
        std::error_code rec;
        fs::path rel = fs::relative(d.file, project, rec);
        if (!rec && !rel.empty() && rel.native()[0] != '.') d.file = rel.string();
        r.diagnostics.push(d.toJson());
    }
    if (pr->exitCode != 0 || pr->timedOut) {
        fs::remove(tmp, ec);
        r.ok = false;
        impl_->lastBuild = r;
        return r;
    }
    fs::rename(tmp, r.library, ec);
    if (ec) return Error::make("io_error", "cannot write " + r.library + ": " + ec.message());
    // Keep the cache small: older builds of this module go.
    for (const auto& e : fs::directory_iterator(outDir, ec)) {
        std::string n = e.path().filename().string();
        if (str::startsWith(n, cfg->name + "_") && e.path().string() != r.library && e.path().string() != impl_->loadedLibrary) {
            fs::remove(e.path(), ec);
        }
    }
    r.ok = true;
    impl_->lastBuilt = r.library;
    impl_->lastBuild = r;
    return r;
}

void NativeModules::unload() {
    if (!impl_->handle) return;
    if (auto shutdown = reinterpret_cast<SkyModuleShutdownFn>(dlsym(impl_->handle, "sky_module_shutdown"))) shutdown();
    if (impl_->module) {
        for (auto& rec : impl_->module->records) {
            rec->alive = false;
            rec->fn = nullptr;
        }
        registry_.removeOwner("native:" + impl_->module->name);
    }
    impl_->module.reset();
    dlclose(impl_->handle);
    impl_->handle = nullptr;
    impl_->loadedLibrary.clear();
}

Status NativeModules::load() {
    if (impl_->lastBuilt.empty()) return Error::make("not_built", "no native build to load", "call native_build first");
    if (impl_->lastBuilt == impl_->loadedLibrary) return {};
    unload();
    void* h = dlopen(impl_->lastBuilt.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        impl_->lastError = std::string("cannot load ") + impl_->lastBuilt + ": " + (e ? e : "?");
        return Error::make("load_failed", impl_->lastError);
    }
    auto init = reinterpret_cast<SkyModuleInitFn>(dlsym(h, "sky_module_init"));
    if (!init) {
        dlclose(h);
        impl_->lastError = "the native module has no sky_module_init (declare it with SKY_MODULE_EXPORT)";
        return Error::make("load_failed", impl_->lastError);
    }
    auto module = std::make_unique<SkyModule>();
    std::string file = fs::path(impl_->lastBuilt).stem().string();
    module->name = file.substr(0, file.rfind('_'));
    int rc = init(&kSdkApi, module.get());
    module->initializing = false;
    if (rc != 0 || !module->error.empty()) {
        for (auto& rec : module->records) rec->alive = false;
        dlclose(h);
        impl_->lastError = "sky_module_init failed (returned " + std::to_string(rc) + ")" +
                           (module->error.empty() ? "" : ": " + module->error);
        return Error::make("init_failed", impl_->lastError);
    }
    for (auto& def : module->builtins) registry_.add(def);
    impl_->handle = h;
    impl_->module = std::move(module);
    impl_->loadedLibrary = impl_->lastBuilt;
    impl_->lastError.clear();
    log::info("native", "loaded native module " + impl_->module->name + " (" + std::to_string(impl_->module->builtins.size()) +
                            " builtins, " + std::to_string(impl_->module->systems.size()) + " systems)");
    return {};
}

Status NativeModules::usePrebuilt(const std::string& library) {
    std::error_code ec;
    if (!fs::exists(library, ec)) return Error::make("not_found", "no precompiled native module at " + library);
    prebuiltOnly_ = true;
    impl_->lastBuilt = library;
    return load();
}

Status NativeModules::reloadIfChanged() {
    if (prebuiltOnly_) return load();  // shipped: the library is final, never rebuilt
    if (!hasSources()) {
        unload();
        return {};
    }
    auto b = build(false);
    if (!b) return b.error();
    if (!b->ok) {
        std::string first;
        for (const auto& d : b->diagnostics.elements()) {
            if (d.get("severity").asString() == "error") {
                first = d.get("file").asString() + ":" + std::to_string(d.get("line").asInt()) + ": " + d.get("message").asString();
                break;
            }
        }
        return Error::make("native_build_failed", "the native module does not compile" + (first.empty() ? "" : ": " + first),
                           "call native_build for all diagnostics; the previous module stays loaded");
    }
    return load();
}

void NativeModules::tick(float dt) {
    if (!impl_->module || impl_->module->systems.empty()) return;
    SkyWorld world;
    world.engine = &engine_;
    world.scene = &engine_.scene();
    world.runtime = &engine_.runtime();
    world.input = &engine_.input();
    world.dt = dt;
    for (const auto& s : impl_->module->systems) s.fn(&world, dt, s.user);
}

Json NativeModules::list() const {
    Json out = Json::object({{"sources", hasSources()}, {"compiler", wander::findCxxCompiler()}});
    if (!impl_->lastBuild.library.empty()) out["last_build"] = impl_->lastBuild.toJson();
    if (impl_->module) {
        Json b = Json::array();
        for (const auto& d : impl_->module->builtins) b.push(Json::object({{"name", d.name}, {"signature", d.signature()}, {"doc", d.doc}}));
        Json s = Json::array();
        for (const auto& sys : impl_->module->systems) s.push(sys.name);
        out["loaded"] = Json::object({{"name", impl_->module->name}, {"library", impl_->loadedLibrary}, {"builtins", b}, {"systems", s}});
    }
    if (!impl_->lastError.empty()) out["error"] = impl_->lastError;
    Json aot = Json::array();
    for (const auto& [hash, np] : impl_->aot) aot.push(Json::object({{"program", std::to_string(hash)}, {"library", np->library}}));
    out["aot_programs"] = aot;
    out["aot_auto"] = autoAot_;
    return out;
}

Result<Json> NativeModules::writeTemplate(const std::string& rawName, bool overwrite) {
    std::string name = rawName.empty() ? "gameplay" : rawName;
    for (char ch : name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            return Error::make("invalid_name", "module file names use letters, digits and _ (got \"" + name + "\")");
        }
    }
    fs::path dir = fs::path(projectDir()) / "native";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path cpp = dir / (name + ".cpp");
    if (fs::exists(cpp, ec) && !overwrite) {
        return Error::make("exists", cpp.string() + " already exists", "pass overwrite=true to replace it");
    }
    std::string code = R"CPP(// Native module for Skywalker: C++ that Wander behaviors call like builtins.
// Build: native_build. Reloads when play starts. API: skywalker/native/sdk.h.
#include <cmath>

#include "skywalker/native/sdk.h"

namespace {

// wave(t, frequency) -> number: a builtin every behavior can call: wave(time, 2)
void wave(SkyCall* call, const SkyValue* args, int argc, SkyValue* result, void*) {
    double t = sky_arg_number(call, args, 0);
    double f = argc > 1 ? sky_arg_number(call, args, 1) : 1.0;
    *result = sky_number(std::sin(t * f * 6.283185307179586));
}

// A per-tick system: spins every entity tagged "spinner" (runs after behaviors).
void spinners(SkyWorld* w, double dt, void*) {
    SkyEntity ids[256];
    size_t n = sky_sdk_api->find_tagged(w, "spinner", ids, 256);
    for (size_t i = 0; i < n && i < 256; ++i) {
        float r[3];
        if (sky_sdk_api->get_rotation(w, ids[i], r) == 0) {
            r[1] = std::fmod(r[1] + static_cast<float>(90.0 * dt), 360.f);
            sky_sdk_api->set_rotation(w, ids[i], r);
        }
    }
}

}  // namespace

SKY_MODULE_EXPORT int sky_module_init(const SkyApi* api, SkyModule* module) {
    SKY_SDK_INIT(api);
    SkyBuiltinDesc d = {"wave", "Sine wave in -1..1 for time t and a frequency (Hz).", "math", "wave(time, 2)",
                        1, 2, "t: number, frequency?: number", "number", wave, nullptr};
    if (api->register_builtin(module, &d) != 0) return -1;
    return api->register_system(module, "spinners", spinners, nullptr);
}
)CPP";
    {
        std::ofstream f(cpp, std::ios::binary | std::ios::trunc);
        f << code;
        if (!f) return Error::make("io_error", "cannot write " + cpp.string());
    }
    fs::path cfg = dir / "module.json";
    bool wroteConfig = false;
    if (!fs::exists(cfg, ec)) {
        std::ofstream f(cfg, std::ios::binary);
        f << "{\n  \"flags\": [],\n  \"include_dirs\": [],\n  \"lib_dirs\": [],\n  \"libs\": [],\n  \"frameworks\": [],\n"
             "  \"pkg_config\": []\n}\n";
        wroteConfig = true;
    }
    Json files = Json::array({fs::relative(cpp, projectDir(), ec).string()});
    if (wroteConfig) files.push(fs::relative(cfg, projectDir(), ec).string());
    return Json::object({{"files", files}});
}

// --- AOT ----------------------------------------------------------------------------------

Result<Json> NativeModules::compileBehaviors(const std::vector<EntityId>& entities, bool force) {
    Scene& scene = engine_.scene();
    wander::Runtime& rt = engine_.runtime();
    rt.compileScripts();
    std::vector<std::pair<EntityId, std::shared_ptr<const wander::Program>>> programs;
    std::set<uint64_t> seen;
    auto add = [&](EntityId e) {
        const Behavior* b = scene.get<Behavior>(e);
        if (!b) return;
        for (const auto& s : b->scripts) {
            if (s.program && seen.insert(s.program->hash).second) programs.emplace_back(e, s.program);
        }
    };
    if (entities.empty()) {
        for (EntityId e : scene.entities()) add(e);
    } else {
        for (EntityId e : entities) add(e);
    }
    Json results = Json::array();
    wander::AotOptions o;
    o.cacheDir = (fs::path(cacheDir()) / "aot").string();
    o.force = force;
    int ok = 0, failed = 0;
    for (const auto& [e, prog] : programs) {
        auto r = wander::compileNative(*prog, o);
        Json j = Json::object({{"entity", e}, {"program", std::to_string(prog->hash)}});
        if (r) {
            impl_->aot[prog->hash] = r->native;
            rt.attachNative(prog->hash, r->native);
            j["ok"] = true;
            j["result"] = r->toJson();
            ++ok;
        } else {
            j["ok"] = false;
            j["error"] = r.error().message;
            if (!r.error().hint.empty()) j["hint"] = r.error().hint;
            ++failed;
        }
        results.push(j);
    }
    return Json::object({{"compiled", ok}, {"failed", failed}, {"programs", results}});
}

void NativeModules::onPlay() {
    if (Status s = reloadIfChanged(); !s) {
        engine_.runtime().log(kNoEntity, "native module: " + s.error().message, "native");
        log::warn("native", s.error().message);
    }
    if (autoAot_ && !prebuiltOnly_) {
        auto r = compileBehaviors({}, false);
        if (r && r->get("failed").asInt() > 0) {
            engine_.runtime().log(kNoEntity, "AOT: " + std::to_string(r->get("failed").asInt()) +
                                                 " behavior(s) stay interpreted (see wander_compile_native)", "native");
        }
    }
}

}  // namespace sky
