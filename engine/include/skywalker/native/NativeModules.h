#pragma once
// Native code for a project: C++ modules (`native/*.cpp`, see skywalker/native/sdk.h) and
// AOT-compiled Wander behaviors (see skywalker/wander/Aot.h).
//
// Native modules are compiled with the system clang++ into
// <project>/.skywalker/cache/native/<name>_<hash>.dylib, loaded with dlopen, and register
// Wander builtins (into the engine's own builtin registry, layered on the global one) and
// per-tick systems. `native/module.json` adds compiler flags, include/library dirs,
// libraries, frameworks and pkg-config packages, so modules can use any C++ library.
// Modules hot-reload when play starts (rebuilt first if their sources changed).
//
// Security: native code is trusted, local code with the user's permissions; the tools
// that build it are mutating "code" tools that agents must be allowed to use.

#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

class Engine;

class NativeModules {
public:
    NativeModules(Engine& engine, wander::BuiltinRegistry& registry);
    ~NativeModules();
    NativeModules(const NativeModules&) = delete;
    NativeModules& operator=(const NativeModules&) = delete;

    // --- Native modules ---------------------------------------------------------------
    struct BuildResult {
        bool ok = false;
        bool upToDate = false;  // nothing changed since the last successful build
        std::string library;
        std::vector<std::string> sources;
        Json diagnostics = Json::array();  // [{file, line, column, severity, message}]
        std::string output;                // raw compiler output (trimmed)
        double ms = 0;
        Json toJson() const;
    };
    /// Whether the project has native sources (native/*.cpp).
    bool hasSources() const;
    /// Compiles native/*.cpp (or reports that the existing library is current).
    Result<BuildResult> build(bool force = false);
    /// Loads the most recent successful build (replacing the loaded module).
    Status load();
    void unload();
    /// Builds if sources changed, then (re)loads. Called when play starts.
    Status reloadIfChanged();
    /// Runs the per-tick systems registered by the loaded module.
    void tick(float dt);
    /// Status: sources, build, loaded module, its builtins and systems.
    Json list() const;
    /// Writes a starter module (native/<name>.cpp + module.json + .gitignore entry).
    Result<Json> writeTemplate(const std::string& name, bool overwrite);

    // --- AOT-compiled behaviors ----------------------------------------------------------
    /// Compiles the programs of these entities' behaviors (all scripts when empty) to native
    /// code and attaches them to the runtime. Per-program results (cached builds are instant).
    Result<Json> compileBehaviors(const std::vector<EntityId>& entities, bool force);
    /// Compile every behavior natively whenever play starts.
    void setAutoCompile(bool on) { autoAot_ = on; }
    bool autoCompile() const { return autoAot_; }
    /// Called by Engine::play().
    void onPlay();

    std::string cacheDir() const;
    std::string projectDir() const;

    struct Impl;

private:
    Engine& engine_;
    wander::BuiltinRegistry& registry_;
    std::unique_ptr<Impl> impl_;
    bool autoAot_ = false;
};

}  // namespace sky
