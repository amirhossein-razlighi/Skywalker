// Native module tools: build, list and scaffold project C++ (native/*.cpp) that registers
// Wander builtins and per-tick systems. Native code is trusted local code: these are
// "code" tools (building runs the system compiler; loading runs the code in-process).

#include "ToolHelpers.h"
#include "skywalker/native/NativeModules.h"

namespace sky::tools {

namespace {
using namespace schema;
}

void addNativeTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"native_build", "Build native module",
             "Compile the project's native C++ module (native/*.cpp against skywalker/native/sdk.h, plus flags, include "
             "dirs, libraries, frameworks and pkg-config packages from native/module.json) into a library with the system "
             "clang++, then load it: its builtins become callable from every behavior (they appear in wander_reference) "
             "and its systems run every tick while playing. Compiler errors come back as structured diagnostics "
             "{file, line, column, message} relative to the project. Unchanged sources load the cached build instantly. "
             "Security: this runs native code you or an agent wrote inside the engine; review it first.",
             "code",
             object({{"force", boolean("Rebuild even if nothing changed")},
                     {"load", boolean("Load the module after a successful build (default true)")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 auto r = engine.native().build(a.get("force").asBool());
                 if (!r) return ToolResult::error(r.error());
                 Json j = r->toJson();
                 if (!r->ok) {
                     ToolResult res = ToolResult::json(j, "the native module does not compile (" +
                                                              std::to_string(r->diagnostics.size()) + " diagnostics)");
                     res.isError = true;
                     return res;
                 }
                 if (a.get("load").asBool(true)) {
                     if (Status s = engine.native().load(); !s) return fail(s);
                     j["module"] = engine.native().list().get("loaded");
                 }
                 return ToolResult::json(j, r->upToDate ? "native module up to date" : "native module built");
             }});

    reg.add({"native_list", "Native module status",
             "Status of the project's native code: whether native sources exist, the C++ compiler, the last build and its "
             "diagnostics, the loaded module with its builtins (signatures) and systems, and behaviors running as "
             "AOT-compiled native code.",
             "code", object({}), false, false,
             [&engine](const Json&, ToolContext&) { return ToolResult::json(engine.native().list()); }});

    reg.add({"native_template", "Create native module",
             "Write a starter native module: native/<name>.cpp (a builtin `wave(t, freq)` and a per-tick system that spins "
             "entities tagged \"spinner\") and native/module.json (flags, include_dirs, lib_dirs, libs, frameworks, "
             "pkg_config). Edit it, then native_build. Use it for hot inner loops (flocking, procedural generation, custom "
             "physics) or to bring in any C++ library (e.g. Homebrew packages via pkg_config).",
             "code",
             object({{"name", string("File name without extension (default \"gameplay\")")},
                     {"overwrite", boolean("Replace an existing file")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 auto r = engine.native().writeTemplate(a.get("name").asString(), a.get("overwrite").asBool());
                 if (!r) return ToolResult::error(r.error());
                 return ToolResult::json(r.value(), "wrote the native module template; next: native_build");
             }});
}

}  // namespace sky::tools
