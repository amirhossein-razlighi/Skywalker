// DCC bridge tools: let design agents work in Blender (and Maya / Houdini / 3ds Max when they are
// installed) and bring the results back into the project.
//
//   headless jobs   dcc_list, dcc_run_script, dcc_convert, dcc_export, dcc_edit_asset, dcc_generate
//   live Blender    dcc_install_addon, dcc_session_start / _status / _exec / _pull_selection / _send / _stop
//   plumbing        dcc_receive (used by the Blender add-on's "Send" button), dcc_cancel
//
// Every tool that runs an external program is a two-step deferred call (see DeferredWork): the
// quick, engine-touching steps (argument checks, importing the produced files, placing entities)
// run on the main thread, the design app runs on the caller's thread. Nothing user-supplied is
// ever passed through a shell.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <set>

#include "ToolHelpers.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/dcc/Manager.h"
#include "skywalker/render/MeshData.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

constexpr int kMaxTimeoutS = 3600;

const std::vector<std::string>& recipeNames() {
    static const std::vector<std::string> names{"building", "tower", "wall", "rock", "stairs",
                                                "arch", "fence", "column", "barrel", "terrain_chunk"};
    return names;
}

std::string extOf(const std::string& name) {
    std::string l = str::lower(name);
    auto dot = l.rfind('.');
    return dot == std::string::npos ? "" : l.substr(dot);
}

bool isNativeMesh(const std::string& path) {
    std::string e = extOf(path);
    return e == ".glb" || e == ".gltf" || e == ".obj" || e == ".ply" || e == ".stl";
}

/// Mesh formats Blender converts to glb.
bool isConvertible(const std::string& path) {
    static const std::set<std::string> ok{".fbx", ".obj", ".dae", ".usd", ".usda", ".usdc", ".usdz",
                                          ".3ds", ".ply", ".stl", ".abc", ".blend"};
    return ok.count(extOf(path)) > 0;
}

std::string sanitize(const std::string& in, const std::string& fallback) {
    std::string out;
    for (char c : in) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') ? c : '_';
    while (!out.empty() && (out.front() == '.' || out.front() == '_')) out.erase(out.begin());
    return out.empty() ? fallback : out;
}

std::string shortHash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf).substr(0, 10);
}

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

/// Absolute path of a project location (relative paths are project-relative), refusing anything
/// outside the project.
Result<std::string> inProject(Engine& engine, const std::string& path, const char* what) {
    if (path.empty()) return Error::make("invalid_arguments", std::string(what) + " is empty");
    std::string abs = engine.resolvePath(path);
    if (engine.assets().relative(abs).empty() && fs::weakly_canonical(abs) != fs::weakly_canonical(engine.assets().root())) {
        return Error::make("invalid_path", std::string(what) + " must be inside the project: " + path,
                           "use a project-relative path like dcc/out");
    }
    return abs;
}

/// An existing file or folder, project-relative or absolute.
Result<std::string> existingPath(Engine& engine, const std::string& path, const char* what) {
    if (path.empty()) return Error::make("invalid_arguments", std::string(what) + " is empty");
    std::string abs = engine.resolvePath(path);
    std::error_code ec;
    if (!fs::exists(abs, ec)) {
        return Error::make("not_found", std::string(what) + " does not exist: " + path,
                           "paths are relative to the project, or absolute");
    }
    return abs;
}

int timeoutSeconds(const Json& a, int fallback) {
    int64_t t = a.get("timeout_s").asInt(fallback);
    return static_cast<int>(std::clamp<int64_t>(t, 5, kMaxTimeoutS));
}

std::string projectRel(Engine& engine, const std::string& abs) { return engine.assets().relative(abs); }

Json stringArray(const Json& v) {
    Json out = Json::array();
    for (const auto& e : v.elements()) {
        if (e.isString()) out.push(e.asString());
    }
    return out;
}

/// Everything an agent needs to see about a failed job, in one error.
ToolResult jobError(const dcc::JobResult& jr, const std::string& what) {
    std::string hint = jr.hint;
    if (!jr.log.empty()) hint += (hint.empty() ? "" : "\n") + std::string("app output:\n") + jr.log;
    return ToolResult::error(Error::make("dcc_failed", what + " failed: " + jr.error, hint));
}

Json producedJson(Engine& engine, const dcc::JobResult& jr) {
    Json files = Json::array();
    for (const auto& f : jr.files) {
        std::string rel = projectRel(engine, f.path);
        files.push(Json::object({{"path", rel.empty() ? f.path : rel}, {"bytes", f.size}}));
    }
    return files;
}

// --- placement & import -------------------------------------------------------------------------

struct PlaceSpec {
    bool enabled = false;
    std::string name;
    Json position;
    double spacing = 0;
};

PlaceSpec parsePlace(const Json& v, double defaultSpacing) {
    PlaceSpec p;
    p.spacing = defaultSpacing;
    if (v.isBool()) {
        p.enabled = v.asBool();
    } else if (v.isObject()) {
        p.enabled = true;
        p.name = v.get("name").asString();
        if (v.get("position").isArray()) p.position = v.get("position");
        p.spacing = v.get("spacing").asNumber(defaultSpacing);
    }
    return p;
}

struct ImportSpec {
    bool import = true;
    bool normalize = false;  // design apps work in meters: keep real-world size
    bool zUp = false;
    PlaceSpec place;
    std::string description;
    Json tags = Json::array();
    Json source = Json::object();
    std::string app;  // for tags
};

/// Imports mesh files as assets with provenance and optionally places them (one undo step).
/// Main thread only.
Json importMeshes(Engine& engine, const std::string& actor, const std::vector<std::string>& absFiles, const ImportSpec& spec,
                  std::vector<std::string>& warnings, bool& anyFailed) {
    Json imported = Json::array();
    engine.refreshAssets();
    std::vector<std::pair<std::string, Json>> placeable;  // (name, import result)
    for (const auto& abs : absFiles) {
        std::string rel = projectRel(engine, abs);
        if (rel.empty()) continue;
        // Re-imports keep the options the asset was first imported with unless the caller says otherwise.
        Engine::MeshImportOptions opts;
        opts.normalize = spec.normalize;
        opts.zUp = spec.zUp;
        auto r = engine.importMeshAsset(rel, opts);
        if (!r) {
            warnings.push_back("could not import " + rel + ": " + r.error().message);
            anyFailed = true;
            continue;
        }
        Json item = *r;
        item["file"] = rel;
        Json meta = Json::object();
        std::vector<std::string> tagList;
        if (const AssetRecord* existing = engine.assets().find(rel)) tagList = existing->tags;  // re-imports keep the human's tags
        auto addTag = [&](const std::string& t) {
            if (!t.empty() && std::find(tagList.begin(), tagList.end(), t) == tagList.end()) tagList.push_back(t);
        };
        addTag("dcc");
        addTag(spec.app);
        for (const auto& t : spec.tags.elements()) addTag(t.asString());
        Json tags = Json::array();
        for (const auto& t : tagList) tags.push(t);
        meta["tags"] = tags;
        if (!spec.description.empty()) meta["description"] = spec.description;
        Json source = spec.source;
        source["retrieved"] = today();
        source["by"] = actor;
        meta["source"] = source;
        (void)engine.assets().updateMeta(rel, meta);
        imported.push(item);
        if (spec.place.enabled) {
            std::string stem = fs::path(rel).stem().string();
            placeable.emplace_back(stem, item);
        }
    }
    if (!placeable.empty()) {
        Status st = engine.edit(actor, "Place " + placeable.front().first, [&]() -> Status {
            size_t i = 0;
            for (auto& [stem, item] : placeable) {
                Json pos = spec.place.position;
                if (pos.isArray() && pos.size() == 3 && spec.place.spacing != 0) {
                    const auto& pe = pos.elements();
                    pos = Json::array({pe[0].asNumber() + static_cast<double>(i) * spec.place.spacing, pe[1].asNumber(), pe[2].asNumber()});
                } else if (!pos.isArray() && spec.place.spacing != 0) {
                    pos = Json::array({static_cast<double>(i) * spec.place.spacing, 0.0, 0.0});
                }
                std::string name = (!spec.place.name.empty() && placeable.size() == 1) ? spec.place.name
                                   : !spec.place.name.empty() ? spec.place.name + "_" + stem : stem;
                EntityId id = kNoEntity;
                if (Status s = placeImportedMesh(engine, item, name, pos, id); !s) return s;
                item["entity"] = id;
                ++i;
            }
            return {};
        });
        if (!st) {
            warnings.push_back("could not place the models: " + st.error().message);
            anyFailed = true;
        } else {
            // `placeable` holds copies (one per imported model, same order): copy the entity ids back.
            for (size_t i = 0; i < imported.size() && i < placeable.size(); ++i) {
                imported.elements()[i]["entity"] = placeable[i].second.get("entity");
            }
        }
    }
    return imported;
}

ImportSpec parseImport(const Json& a, const std::string& app, bool importDefault) {
    ImportSpec s;
    s.import = a.get("import").asBool(importDefault);
    s.normalize = a.get("normalize").asBool(false);
    s.zUp = a.get("z_up").asBool(false);
    s.place = parsePlace(a.get("place"), 0);
    s.description = a.get("description").asString();
    if (a.get("tags").isArray()) s.tags = stringArray(a.get("tags"));
    s.app = app;
    return s;
}

Json importSchemaPlace() {
    return Json::object({{"description", "Also place the imported model(s) in the scene: true, or {\"name\": \"Tower\", "
                                          "\"position\": [x,y,z], \"spacing\": 4}. Several models are laid out along x by `spacing`."}});
}

std::string textList(const Json& imported) {
    std::string out;
    for (const auto& i : imported.elements()) {
        out += "\n  " + i.get("file").asString() + " -> " + i.get("mesh").asString();
        if (i.contains("entity")) out += " (entity #" + std::to_string(i.get("entity").asInt()) + ")";
    }
    return out;
}

// --- job plumbing ------------------------------------------------------------------------------

using ManagerPtr = std::shared_ptr<dcc::Manager>;

/// RAII: removes the job from the manager's running list even if the work throws.
struct JobScope {
    ManagerPtr mgr;
    int id;
    ~JobScope() { mgr->endJob(id); }
};

/// The worker half of a typical tool: run the job and, if asked, convert the formats the engine
/// cannot read natively (FBX, Collada, USD...) with Blender.
struct RunState {
    dcc::AppInfo app;
    dcc::JobSpec spec;
    dcc::JobResult job;
    std::vector<std::string> convertedFrom;
    std::string convertNote;
    std::shared_ptr<dcc::CancelToken> cancel = std::make_shared<dcc::CancelToken>();
    int jobId = 0;
    // Tool-specific scratch.
    ImportSpec import;
    Json extra = Json::object();
    std::vector<std::string> alreadyConverted;  // convert: up-to-date outputs that were skipped
};

std::string seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f s", s);
    return buf;
}

std::string baseLabel(const std::string& tool, const dcc::AppInfo& app) { return tool + " (" + app.name + ")"; }

void convertForeign(const ManagerPtr& mgr, RunState& st) {
    std::vector<std::string> foreign;
    for (const auto& f : st.job.files) {
        std::string e = extOf(f.path);
        if (e == ".fbx" || e == ".dae" || e == ".usd" || e == ".usda" || e == ".usdc" || e == ".usdz" || e == ".3ds" || e == ".abc") {
            foreign.push_back(f.path);
        }
    }
    if (foreign.empty()) return;
    auto blender = mgr->pick("blender", "convert");
    if (!blender) {
        st.convertNote = "produced " + std::to_string(foreign.size()) + " file(s) the engine cannot read directly (" +
                         extOf(foreign.front()) + "); install Blender so dcc_convert can turn them into glTF";
        return;
    }
    dcc::JobSpec cs;
    cs.label = "convert";
    cs.task = "convert";
    cs.projectDir = st.spec.projectDir;
    cs.outDir = st.spec.outDir;
    cs.timeout = std::chrono::minutes(10);
    Json inputs = Json::array();
    for (const auto& f : foreign) {
        std::string rel = fs::relative(f, st.spec.outDir).generic_string();
        rel = rel.substr(0, rel.size() - extOf(rel).size());
        inputs.push(Json::object({{"path", f}, {"name", rel}}));
    }
    cs.params = Json::object({{"inputs", inputs}});
    dcc::JobResult cr = mgr->run(cs, *blender, st.cancel);
    if (!cr.ok) {
        st.convertNote = "conversion with Blender failed: " + cr.error;
        return;
    }
    for (const auto& f : cr.files) st.job.files.push_back(f);
    st.convertedFrom = foreign;
}

/// Files of a finished job that should be imported (glTF first, then native mesh formats).
std::vector<std::string> importable(const dcc::JobResult& jr) {
    std::vector<std::string> glb, other;
    std::set<std::string> glbStems;
    for (const auto& f : jr.files) {
        std::string e = extOf(f.path);
        if (e == ".glb" || e == ".gltf") {
            glb.push_back(f.path);
            glbStems.insert(fs::path(f.path).replace_extension().string());
        }
    }
    for (const auto& f : jr.files) {
        std::string e = extOf(f.path);
        // An .obj/.ply/.stl that has a converted .glb twin is not imported twice.
        if ((e == ".obj" || e == ".ply" || e == ".stl") && !glbStems.count(fs::path(f.path).replace_extension().string())) {
            other.push_back(f.path);
        }
    }
    glb.insert(glb.end(), other.begin(), other.end());
    return glb;
}

std::string appTag(const dcc::AppInfo& app) { return dcc::toString(app.id); }

Json provenance(const dcc::AppInfo& app, const std::string& tool) {
    return Json::object({{"generator", appTag(app) + (app.version.empty() ? "" : " " + app.version)}, {"tool", tool}});
}

/// Summary object shared by the job tools.
Json jobJson(Engine& engine, const RunState& st) {
    Json j = Json::object({{"app", appTag(st.app)}, {"version", st.app.version}, {"seconds", st.job.seconds}});
    j["files"] = producedJson(engine, st.job);
    if (st.job.result.isObject() && st.job.result.size()) j["result"] = st.job.result;
    if (!st.job.log.empty()) j["log"] = st.job.log;
    return j;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------

std::shared_ptr<dcc::Manager> addDccTools(Engine& engine, ToolRegistry& reg, std::shared_ptr<dcc::Manager> manager) {
    ManagerPtr mgr = manager ? std::move(manager) : std::make_shared<dcc::Manager>();

    // --- dcc_list -----------------------------------------------------------------------------
    reg.add({"dcc_list", "List design apps",
             "Which professional design apps are installed on this computer and what the bridge can do with them: "
             "Blender (fully supported and tested), plus Maya (mayapy), Houdini (hython) and 3ds Max (3dsmaxbatch) "
             "adapters that follow the vendors' documented headless interfaces but are untested. Also lists running "
             "jobs and the live Blender session (if any) and the procedural recipes of dcc_generate. Detection checks "
             "SKY_BLENDER / SKY_MAYAPY / SKY_HOUDINI_HYTHON / SKY_3DSMAX_BATCH, ~/.skywalker/dcc/paths.json, the "
             "standard install folders and PATH. Call this first to know what you can use. Example: {}.",
             "dcc", object({{"refresh", boolean("Rescan the machine instead of using the cached result")}}),
             false, false, [mgr](const Json& a, ToolContext&) -> ToolResult {
                 // Detection may start Blender once to read its version: do it off the main thread.
                 auto apps = std::make_shared<std::vector<dcc::AppInfo>>();
                 bool refresh = a.get("refresh").asBool(false);
                 return ToolResult::defer(
                     [mgr, apps, refresh] { *apps = mgr->apps(refresh); },
                     [mgr, apps]() -> ToolResult {
                         Json list = Json::array();
                         std::string text;
                         for (const auto& app : *apps) {
                             list.push(app.toJson());
                             text += app.name + " " + app.version + " — " + app.executable + (app.tested ? "" : " [untested adapter]") + "\n";
                         }
                         if (apps->empty()) {
                             text = "no design apps found. Install Blender (free), or set SKY_BLENDER to its executable.\n";
                         }
                         Json recipes = Json::array();
                         for (const auto& r : recipeNames()) recipes.push(r);
                         Json out = Json::object({{"apps", list}, {"jobs", mgr->runningJobs()}, {"recipes", recipes}});
                         if (auto s = mgr->session(); s) {
                             out["session"] = Json::object({{"port", s->port}, {"pid", static_cast<double>(s->pid)}, {"version", s->version}, {"mode", s->mode}});
                             text += "live Blender session: " + s->mode + " (pid " + std::to_string(s->pid) + ")\n";
                         }
                         return ToolResult::json(out, str::trim(text));
                     });
             }});

    // --- dcc_run_script -------------------------------------------------------------------------
    {
        ToolDef def{
            "dcc_run_script", "Run a design-app script",
            "Run a Python script headless inside a design app on this computer (Blender bpy by default; Maya via mayapy, "
            "Houdini via hython, 3ds Max via 3dsmaxbatch — those untested) and get back its log, structured result and the "
            "files it produced. The script runs with the environment SKY_PROJECT (project folder), SKY_OUT (output folder: "
            "everything new or changed there is reported), SKY_INPUT, and the helper library already imported as `sky` "
            "(sky.out_path('tower.glb'), sky.log(), sky.result(vertices=n), sky.project_path('assets/x.png')). Inside "
            "Blender `from skywalker_dcc import blender as B, procedural` gives import/export/decimate/bevel/UV/bake helpers "
            "and ready-made generators (see dcc_generate). Write models to SKY_OUT with B.export_glb(sky.out_path('x.glb')). "
            "With import:true produced .glb/.gltf (and .obj/.ply/.stl) are imported as project assets with provenance; "
            "FBX/Collada/USD files are converted through Blender first; `place` puts them in the scene. Units are meters; "
            "glTF export converts Blender's Z-up to the engine's Y-up. The script is executable code: the human is asked "
            "to approve in the editor. Example: {\"script\": \"import bpy; bpy.ops.mesh.primitive_cube_add(size=2); "
            "B.export_glb(sky.out_path('cube.glb'))\", \"import\": true, \"place\": {\"name\": \"Cube\", \"position\": [0,0,0]}}.",
            "dcc",
            object({{"script", string("Python source to run inside the app")},
                    {"app", enumeration({"blender", "maya", "houdini", "3dsmax"}, "Which app (default: Blender, else the first installed)")},
                    {"input", string("Scene to open first (.blend for Blender, .ma/.mb Maya, .hip Houdini, .max 3ds Max), also in SKY_INPUT; project-relative or absolute")},
                    {"args", array(Json::object({{"type", "string"}}), "Script arguments (sky.ARGS)")},
                    {"name", string("Label for this run; the default output folder is dcc/<name> (default \"script\")")},
                    {"out_dir", string("Project-relative output folder (SKY_OUT), default dcc/<name>")},
                    {"timeout_s", integer("Give up after this many seconds (default 300, max 3600)")},
                    {"import", boolean("Import produced meshes as assets (default false)")},
                    {"normalize", boolean("When importing: scale to fit 1 m (default false = keep real-world size)")},
                    {"place", importSchemaPlace()},
                    {"description", string("Asset description for imported models (helps future searches)")},
                    {"tags", array(Json::object({{"type", "string"}}), "Extra asset tags")},
                    {"user_prefs", boolean("Blender: load the user's preferences and add-ons instead of factory settings")}},
                   {"script"}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext& ctx) -> ToolResult {
                std::string appName = a.get("app").asString();
                auto app = mgr->pick(appName, "script");
                if (!app) return ToolResult::error(app.error());
                if (a.get("script").asString().size() > (size_t{256} << 10)) {
                    return ToolResult::error(Error::make("invalid_arguments", "script is larger than 256 KB",
                                                         "put helpers in files under the project and import them"));
                }
                auto st = std::make_shared<RunState>();
                st->app = *app;
                std::string name = sanitize(a.get("name").asString("script"), "script");
                auto out = inProject(engine, a.get("out_dir").asString("dcc/" + name), "out_dir");
                if (!out) return ToolResult::error(out.error());
                st->spec.label = "run_script";
                st->spec.task = "script";
                st->spec.script = a.get("script").asString();
                st->spec.projectDir = engine.assets().root();
                st->spec.outDir = *out;
                st->spec.timeout = std::chrono::seconds(timeoutSeconds(a, 300));
                st->spec.userPrefs = a.get("user_prefs").asBool(false);
                for (const auto& s : a.get("args").elements()) st->spec.args.push_back(s.asString());
                if (a.contains("input")) {
                    auto in = existingPath(engine, a.get("input").asString(), "input");
                    if (!in) return ToolResult::error(in.error());
                    st->spec.inputFile = *in;
                }
                st->import = parseImport(a, appTag(*app), false);
                st->import.source = provenance(*app, "dcc_run_script");
                st->import.source["scriptSha"] = shortHash(st->spec.script);
                st->jobId = mgr->beginJob(baseLabel("run_script", *app), st->cancel);
                std::string actor = ctx.actor;
                return ToolResult::defer(
                    [mgr, st] {
                        JobScope scope{mgr, st->jobId};
                        st->job = mgr->run(st->spec, st->app, st->cancel);
                        if (st->job.ok && st->import.import) convertForeign(mgr, *st);
                    },
                    [&engine, mgr, st, actor]() -> ToolResult {
                        if (!st->job.ok) return jobError(st->job, "script");
                        Json out = jobJson(engine, *st);
                        std::vector<std::string> warnings;
                        if (!st->convertNote.empty()) warnings.push_back(st->convertNote);
                        std::string summary = "script finished in " + seconds(st->job.seconds) + ", " +
                                              std::to_string(st->job.files.size()) + " file(s) produced";
                        engine.refreshAssets();
                        if (st->import.import) {
                            bool failed = false;
                            Json imported = importMeshes(engine, actor, importable(st->job), st->import, warnings, failed);
                            out["imported"] = imported;
                            summary += "; imported " + std::to_string(imported.size()) + " model(s)" + textList(imported);
                        }
                        if (!warnings.empty()) {
                            Json w = Json::array();
                            for (const auto& s : warnings) {
                                w.push(s);
                                summary += "\nwarning: " + s;
                            }
                            out["warnings"] = w;
                        }
                        if (st->job.result.get("warnings").isArray()) {
                            for (const auto& s : st->job.result.get("warnings").elements()) summary += "\nscript warning: " + s.asString();
                        }
                        return ToolResult::json(out, summary);
                    },
                    [st] { st->cancel->cancel(); });
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    // --- dcc_convert ----------------------------------------------------------------------------
    {
        ToolDef def{
            "dcc_convert", "Convert models to glTF with Blender",
            "Convert FBX, OBJ, Collada (.dae), USD/USDZ, 3DS, PLY, STL, Alembic or .blend files to glTF binary (.glb) with "
            "Blender — materials and textures are kept and embedded — then import them as assets. This unlocks the huge free "
            "FBX libraries (Kenney, Quaternius, Mixamo props, Sketchfab downloads...). `path` is one file or a whole folder "
            "(every supported file in it; recursive:true to descend); outputs mirror the folder structure next to the sources "
            "(or in out_dir). Use `scale` for packs authored in other units (0.01 = centimeters to meters if the importer did "
            "not already convert), `axis_forward`/`axis_up` for odd orientations ('-Z','Y'), `recenter` ('bottom' puts the "
            "origin at the feet), and `ops` for cleanup while converting (see dcc_edit_asset: decimate, merge_by_distance, "
            "shade_smooth...). Real-world size is kept (normalize:false). Typical flow after dcc_run_script or asset_download: "
            "{\"path\": \"downloads/kenney_town\", \"recursive\": true}. Needs Blender; asks for approval in the editor.",
            "dcc",
            object({{"path", string("File or folder to convert (project-relative or absolute)")},
                    {"recursive", boolean("Folder: include sub-folders (default false)")},
                    {"out_dir", string("Project-relative output folder (default: next to the sources, or dcc/converted outside the project)")},
                    {"scale", number("Uniform scale applied on import (default 1)")},
                    {"axis_forward", string("FBX/OBJ forward axis override, e.g. \"-Z\"")},
                    {"axis_up", string("FBX/OBJ up axis override, e.g. \"Y\"")},
                    {"recenter", enumeration({"bottom", "center"}, "Move the geometry so its origin is at the bottom / middle center")},
                    {"ops", array(Json::object({{"type", "object"}}), "Cleanup operations, e.g. [{\"op\":\"decimate\",\"ratio\":0.5}]")},
                    {"skip_existing", boolean("Skip files whose .glb is newer than the source (resume a big batch)")},
                    {"timeout_s", integer("Give up after this many seconds (default 600, max 3600)")},
                    {"import", boolean("Import the converted models as assets (default true)")},
                    {"normalize", boolean("When importing: scale to fit 1 m (default false = real-world size)")},
                    {"place", importSchemaPlace()},
                    {"tags", array(Json::object({{"type", "string"}}), "Asset tags")},
                    {"description", string("Asset description")}},
                   {"path"}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext& ctx) -> ToolResult {
                auto blender = mgr->pick("blender", "convert");
                if (!blender) return ToolResult::error(blender.error());
                auto in = existingPath(engine, a.get("path").asString(), "path");
                if (!in) return ToolResult::error(in.error());
                std::error_code ec;
                const bool isDir = fs::is_directory(*in, ec);

                // Inputs and where each output goes.
                struct Item {
                    std::string in;
                    std::string name;  // path under outDir without extension
                };
                std::vector<Item> items;
                std::string inRel = engine.assets().relative(*in);
                std::string outAbs;
                if (a.contains("out_dir")) {
                    auto o = inProject(engine, a.get("out_dir").asString(), "out_dir");
                    if (!o) return ToolResult::error(o.error());
                    outAbs = *o;
                } else if (!inRel.empty()) {
                    outAbs = isDir ? *in : fs::path(*in).parent_path().string();
                } else {
                    outAbs = engine.resolvePath("dcc/converted/" + sanitize(fs::path(*in).stem().string(), "converted"));
                }
                if (isDir) {
                    auto addFile = [&](const fs::path& p) {
                        if (!isConvertible(p.string())) return;
                        std::string rel = fs::relative(p, *in).generic_string();
                        items.push_back({p.string(), rel.substr(0, rel.size() - extOf(rel).size())});
                    };
                    if (a.get("recursive").asBool(false)) {
                        for (fs::recursive_directory_iterator it(*in, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                            if (it->is_directory() && it->path().filename().string().rfind('.', 0) == 0) it.disable_recursion_pending();
                            else if (it->is_regular_file()) addFile(it->path());
                            if (items.size() > 500) break;
                        }
                    } else {
                        for (fs::directory_iterator it(*in, ec), end; !ec && it != end; it.increment(ec)) {
                            if (it->is_regular_file()) addFile(it->path());
                        }
                    }
                } else {
                    if (!isConvertible(*in)) {
                        return ToolResult::error(Error::make("unsupported", "cannot convert '" + extOf(*in) + "' files",
                                                             "supported: .fbx .obj .dae .usd .usda .usdc .usdz .3ds .ply .stl .abc .blend"));
                    }
                    items.push_back({*in, fs::path(*in).stem().string()});
                }
                if (items.empty()) {
                    return ToolResult::error(Error::make("nothing_to_convert", "no convertible files in " + a.get("path").asString(),
                                                         "folders: set recursive:true to include sub-folders"));
                }
                if (items.size() > 500) {
                    return ToolResult::error(Error::make("too_many_files", "more than 500 files; convert sub-folders separately"));
                }
                // Same stem, different source format (tree.fbx + tree.obj): keep both.
                std::map<std::string, int> seen;
                for (auto& it : items) seen[str::lower(it.name)]++;
                for (auto& it : items) {
                    if (seen[str::lower(it.name)] > 1) it.name += "_" + extOf(it.in).substr(1);
                }

                auto st = std::make_shared<RunState>();
                st->app = *blender;
                st->spec.label = "convert";
                st->spec.task = "convert";
                st->spec.projectDir = engine.assets().root();
                st->spec.outDir = outAbs;
                st->spec.timeout = std::chrono::seconds(timeoutSeconds(a, 600));
                Json inputs = Json::array();
                Json skipped = Json::array();
                std::vector<std::string> alreadyDone;
                for (const auto& it : items) {
                    std::string outFile = (fs::path(outAbs) / (it.name + ".glb")).string();
                    if (a.get("skip_existing").asBool(false) && fs::exists(outFile, ec) &&
                        fs::last_write_time(outFile, ec) >= fs::last_write_time(it.in, ec)) {
                        skipped.push(it.name + ".glb");
                        alreadyDone.push_back(outFile);
                        continue;
                    }
                    inputs.push(Json::object({{"path", it.in}, {"name", it.name}}));
                }
                st->spec.params = Json::object({{"inputs", inputs}});
                for (const char* k : {"scale", "axis_forward", "axis_up", "recenter", "ops"}) {
                    if (a.contains(k)) st->spec.params[k] = a.get(k);
                }
                st->import = parseImport(a, "blender", true);
                if (items.size() > 1 && st->import.place.enabled) st->import.place.spacing = st->import.place.spacing ? st->import.place.spacing : 4;
                st->import.source = provenance(*blender, "dcc_convert");
                st->import.source["convertedFrom"] = inRel.empty() ? a.get("path").asString() : inRel;
                st->extra = Json::object({{"skipped", skipped}});
                st->alreadyConverted = alreadyDone;
                st->jobId = mgr->beginJob(baseLabel("convert", *blender), st->cancel);
                const bool nothingToRun = inputs.size() == 0;
                std::string actor = ctx.actor;
                return ToolResult::defer(
                    [mgr, st, nothingToRun] {
                        JobScope scope{mgr, st->jobId};
                        if (nothingToRun) {
                            st->job.ok = true;
                            return;
                        }
                        st->job = mgr->run(st->spec, st->app, st->cancel);
                    },
                    [&engine, st, actor]() -> ToolResult {
                        if (!st->job.ok) return jobError(st->job, "conversion");
                        Json out = jobJson(engine, *st);
                        out["skipped"] = st->extra.get("skipped");
                        std::vector<std::string> warnings;
                        std::string summary;
                        Json files = st->job.result.get("files");
                        size_t okCount = 0;
                        for (const auto& f : files.elements()) {
                            if (f.get("ok").asBool()) {
                                ++okCount;
                            } else {
                                warnings.push_back(fs::path(f.get("input").asString()).filename().string() + ": " + f.get("error").asString());
                            }
                        }
                        summary = "converted " + std::to_string(okCount) + " of " + std::to_string(files.size()) + " file(s)";
                        if (st->extra.get("skipped").size()) summary += ", skipped " + std::to_string(st->extra.get("skipped").size()) + " up-to-date";
                        if (st->import.import) {
                            std::vector<std::string> glbs;
                            for (const auto& f : st->job.files) {
                                if (extOf(f.path) == ".glb") glbs.push_back(f.path);
                            }
                            for (const auto& f : st->alreadyConverted) glbs.push_back(f);
                            bool failed = false;
                            Json imported = importMeshes(engine, actor, glbs, st->import, warnings, failed);
                            out["imported"] = imported;
                            summary += "; imported " + std::to_string(imported.size()) + " model(s)";
                            if (imported.size() <= 12) summary += textList(imported);
                        }
                        if (!warnings.empty()) {
                            Json w = Json::array();
                            for (const auto& s : warnings) {
                                w.push(s);
                                summary += "\nwarning: " + s;
                            }
                            out["warnings"] = w;
                        }
                        return ToolResult::json(out, summary);
                    },
                    [st] { st->cancel->cancel(); });
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    // --- dcc_export -----------------------------------------------------------------------------
    {
        ToolDef def{
            "dcc_export", "Export assets from a .blend file",
            "Export objects or collections of a Blender file to glTF (.glb) and import them: the way to use a hand-made "
            "or downloaded .blend asset library. By default every top-level collection that contains meshes becomes one "
            "asset (collection-per-asset; loose objects form one more asset named after the file). Select explicitly with "
            "`collections` or `objects` (children come along). Modifiers are applied, the scene's unit scale is honored "
            "(a centimeter-scale scene is exported in meters) and hidden objects are skipped unless include_hidden. "
            "`origin`: keep (default, as placed in the scene), bottom_center / center (re-centered per asset), collection "
            "(the collection's instance offset). Example: {\"blend\": \"downloads/dungeon_kit.blend\", \"origin\": "
            "\"bottom_center\", \"place\": {\"spacing\": 5}}. Needs Blender.",
            "dcc",
            object({{"blend", string(".blend file (project-relative or absolute)")},
                    {"collections", array(Json::object({{"type", "string"}}), "Collections to export, one asset each (default: every top-level collection with meshes)")},
                    {"objects", array(Json::object({{"type", "string"}}), "Object names to export, one asset each (with their children)")},
                    {"origin", enumeration({"keep", "bottom_center", "center", "collection"}, "Where each asset's origin ends up (default keep)")},
                    {"apply_modifiers", boolean("Bake modifiers into the exported mesh (default true)")},
                    {"include_hidden", boolean("Export hidden objects too (default false)")},
                    {"real_units", boolean("Honor the scene's unit scale (default true)")},
                    {"out_dir", string("Project-relative output folder (default dcc/<blend name>)")},
                    {"timeout_s", integer("Give up after this many seconds (default 300, max 3600)")},
                    {"import", boolean("Import the exported models (default true)")},
                    {"normalize", boolean("When importing: scale to fit 1 m (default false)")},
                    {"place", importSchemaPlace()},
                    {"tags", array(Json::object({{"type", "string"}}), "Asset tags")},
                    {"description", string("Asset description")}},
                   {"blend"}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext& ctx) -> ToolResult {
                auto blender = mgr->pick("blender", "export_glb");
                if (!blender) return ToolResult::error(blender.error());
                auto blend = existingPath(engine, a.get("blend").asString(), "blend");
                if (!blend) return ToolResult::error(blend.error());
                if (extOf(*blend) != ".blend") {
                    return ToolResult::error(Error::make("invalid_arguments", "blend must be a .blend file", "use dcc_convert for other formats"));
                }
                auto out = inProject(engine, a.get("out_dir").asString("dcc/" + sanitize(fs::path(*blend).stem().string(), "export")), "out_dir");
                if (!out) return ToolResult::error(out.error());
                auto st = std::make_shared<RunState>();
                st->app = *blender;
                st->spec.label = "export";
                st->spec.task = "export";
                st->spec.projectDir = engine.assets().root();
                st->spec.outDir = *out;
                st->spec.inputFile = *blend;
                st->spec.timeout = std::chrono::seconds(timeoutSeconds(a, 300));
                st->spec.params = Json::object({{"blend", *blend}});
                for (const char* k : {"collections", "objects", "origin", "apply_modifiers", "include_hidden", "real_units"}) {
                    if (a.contains(k)) st->spec.params[k] = a.get(k);
                }
                st->import = parseImport(a, "blender", true);
                if (st->import.place.enabled && st->import.place.spacing == 0) st->import.place.spacing = 5;
                st->import.source = provenance(*blender, "dcc_export");
                st->import.source["blend"] = projectRel(engine, *blend).empty() ? *blend : projectRel(engine, *blend);
                st->jobId = mgr->beginJob(baseLabel("export", *blender), st->cancel);
                std::string actor = ctx.actor;
                return ToolResult::defer(
                    [mgr, st] {
                        JobScope scope{mgr, st->jobId};
                        st->job = mgr->run(st->spec, st->app, st->cancel);
                    },
                    [&engine, st, actor]() -> ToolResult {
                        if (!st->job.ok) return jobError(st->job, "export");
                        Json out = jobJson(engine, *st);
                        std::vector<std::string> warnings;
                        size_t okCount = 0;
                        for (const auto& f : st->job.result.get("files").elements()) {
                            if (f.get("ok").asBool()) ++okCount;
                            else warnings.push_back(f.get("asset").asString() + ": " + f.get("error").asString());
                        }
                        std::string summary = "exported " + std::to_string(okCount) + " asset(s)";
                        if (st->import.import) {
                            std::vector<std::string> glbs;
                            for (const auto& f : st->job.files) {
                                if (extOf(f.path) == ".glb") glbs.push_back(f.path);
                            }
                            bool failed = false;
                            Json imported = importMeshes(engine, actor, glbs, st->import, warnings, failed);
                            out["imported"] = imported;
                            summary += "; imported " + std::to_string(imported.size()) + textList(imported);
                        }
                        if (!warnings.empty()) {
                            Json w = Json::array();
                            for (const auto& s : warnings) {
                                w.push(s);
                                summary += "\nwarning: " + s;
                            }
                            out["warnings"] = w;
                        }
                        return ToolResult::json(out, summary);
                    },
                    [st] { st->cancel->cancel(); });
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    // --- dcc_edit_asset ----------------------------------------------------------------------------
    {
        ToolDef def{
            "dcc_edit_asset", "Edit an asset in Blender (round trip)",
            "Round-trip a mesh asset of the project through Blender: it is loaded, edited by preset `ops` and/or your Python "
            "`script`, exported and re-imported as a NEW VERSION (<name>_v2.glb, provenance recorded in its .meta: source "
            "asset, ops, version), so the original stays untouched and the change can be compared or reverted. Preset ops "
            "(run in order): decimate {ratio}, bevel {width,segments,angle}, smart_uv {angle,margin}, merge_by_distance "
            "{distance}, triangulate, shade_smooth {angle}, recenter {where: bottom|center}, apply_modifiers, join {name}, "
            "bake_ao {samples,distance,target: vertex|texture,size} (ambient occlusion into vertex colors or a texture). Your "
            "`script` runs after the ops with `objects` (the asset's objects), `bpy`, `B` (skywalker_dcc.blender) and `sky` in "
            "scope. mode:'replace' overwrites a .glb in place (a backup goes to .skywalker/dcc-backups) so every scene use "
            "updates live; update_references:true re-points scene entities from the old file to the new version. Example: "
            "{\"asset\": \"downloads/scan/statue.glb\", \"ops\": [{\"op\":\"decimate\",\"ratio\":0.25},{\"op\":\"shade_smooth\"}]}. "
            "Needs Blender.",
            "dcc",
            object({{"asset", string("Mesh asset path or guid")},
                    {"ops", array(Json::object({{"type", "object"}}), "Preset operations, e.g. [{\"op\":\"decimate\",\"ratio\":0.3}]")},
                    {"script", string("Extra Python run after the ops; `objects` holds the asset's objects")},
                    {"mode", enumeration({"new_version", "replace"}, "new_version (default) writes <name>_vN.glb; replace overwrites a .glb in place")},
                    {"update_references", boolean("Re-point scene entities from the old asset to the new version (new_version mode)")},
                    {"timeout_s", integer("Give up after this many seconds (default 300, max 3600)")},
                    {"description", string("Asset description for the new version")}},
                   {"asset"}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext& ctx) -> ToolResult {
                auto blender = mgr->pick("blender", "edit_asset");
                if (!blender) return ToolResult::error(blender.error());
                const AssetRecord* rec = engine.assets().find(a.get("asset").asString());
                if (!rec) {
                    std::vector<std::string> names;
                    for (const auto* r : engine.assets().query({AssetType::Mesh, "", "", 500})) names.push_back(r->path);
                    std::string guess = str::closest(a.get("asset").asString(), names, 6);
                    return ToolResult::error(Error::make("not_found", "no asset " + a.get("asset").asString(),
                                                         guess.empty() ? "use asset_list type=mesh" : "did you mean '" + guess + "'?"));
                }
                if (rec->type != AssetType::Mesh) {
                    return ToolResult::error(Error::make("invalid_arguments", rec->path + " is a " + toString(rec->type) + ", not a mesh"));
                }
                if (!a.contains("ops") && !a.contains("script")) {
                    return ToolResult::error(Error::make("invalid_arguments", "give ops and/or script", "e.g. ops: [{\"op\":\"decimate\",\"ratio\":0.5}]"));
                }
                const std::string mode = a.get("mode").asString("new_version");
                fs::path srcRel(rec->path);
                const std::string ext = extOf(rec->path);
                if (mode == "replace" && ext != ".glb") {
                    return ToolResult::error(Error::make("invalid_arguments", "replace mode works on .glb assets (this is " + ext + ")",
                                                         "use mode new_version, or convert the asset to .glb first"));
                }
                // Next free version: statue.glb -> statue_v2.glb -> statue_v3.glb
                std::string stem = srcRel.stem().string();
                std::string baseStem = stem;
                if (size_t v = stem.rfind("_v"); v != std::string::npos && v + 2 < stem.size() &&
                    std::all_of(stem.begin() + static_cast<long>(v) + 2, stem.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                    baseStem = stem.substr(0, v);
                }
                int version = 2;
                std::string outName;
                std::error_code ec;
                if (mode == "replace") {
                    outName = srcRel.stem().string() + ".glb";
                } else {
                    while (fs::exists(engine.resolvePath((srcRel.parent_path() / (baseStem + "_v" + std::to_string(version) + ".glb")).string()), ec)) ++version;
                    outName = baseStem + "_v" + std::to_string(version) + ".glb";
                }

                auto st = std::make_shared<RunState>();
                st->app = *blender;
                st->spec.label = "edit_asset";
                st->spec.task = "edit";
                st->spec.projectDir = engine.assets().root();
                st->spec.outDir = engine.resolvePath(srcRel.parent_path().string());
                if (srcRel.parent_path().empty()) st->spec.outDir = engine.assets().root();
                st->spec.timeout = std::chrono::seconds(timeoutSeconds(a, 300));
                st->spec.script = a.get("script").asString();
                st->spec.params = Json::object({{"input", engine.resolvePath(rec->path)}, {"output", outName}});
                if (a.contains("ops")) st->spec.params["ops"] = a.get("ops");
                if (!st->spec.script.empty()) st->spec.params["has_script"] = true;
                st->import.import = true;
                st->import.normalize = rec->importSettings.get("normalize").asBool(false);
                st->import.zUp = rec->importSettings.get("zUp").asBool(false);
                st->import.app = "blender";
                st->import.description = a.get("description").asString();
                st->import.source = provenance(*blender, "dcc_edit_asset");
                st->import.source["derivedFrom"] = rec->path;
                st->import.source["derivedFromGuid"] = rec->guid;
                st->import.source["version"] = mode == "replace" ? 1 : version;
                if (a.contains("ops")) st->import.source["ops"] = a.get("ops");
                if (!st->spec.script.empty()) st->import.source["scriptSha"] = shortHash(st->spec.script);
                st->import.tags = Json::array();
                for (const auto& t : rec->tags) {
                    if (t != "downloaded") st->import.tags.push(t);
                }
                st->import.tags.push("edited");
                // A derived model keeps the original's attribution and license trail.
                for (const char* k : {"license", "author", "attribution", "source_page", "url"}) {
                    if (rec->source.contains(k)) st->import.source[k] = rec->source.get(k);
                }
                st->extra = Json::object({{"from", rec->path},
                                          {"output", (srcRel.parent_path() / outName).generic_string()},
                                          {"mode", mode},
                                          {"updateReferences", a.get("update_references").asBool(false) && mode != "replace"}});

                // replace: keep a copy of the original so the edit can be reverted
                if (mode == "replace") {
                    std::string backup = ".skywalker/dcc-backups/" + today() + "-" + shortHash(rec->path + std::to_string(rec->mtime)) + "-" + srcRel.filename().string();
                    fs::create_directories(fs::path(engine.resolvePath(backup)).parent_path(), ec);
                    fs::copy_file(engine.resolvePath(rec->path), engine.resolvePath(backup), fs::copy_options::overwrite_existing, ec);
                    if (ec) return ToolResult::error(Error::make("io_error", "cannot back up the original: " + ec.message()));
                    st->extra["backup"] = backup;
                }
                st->jobId = mgr->beginJob(baseLabel("edit_asset", *blender), st->cancel);
                std::string actor = ctx.actor;
                std::string srcPath = rec->path;
                return ToolResult::defer(
                    [mgr, st] {
                        JobScope scope{mgr, st->jobId};
                        st->job = mgr->run(st->spec, st->app, st->cancel);
                    },
                    [&engine, st, actor, srcPath]() -> ToolResult {
                        if (!st->job.ok) return jobError(st->job, "edit");
                        Json out = jobJson(engine, *st);
                        std::vector<std::string> warnings;
                        std::string outRel = st->extra.get("output").asString();
                        bool failed = false;
                        Json imported = importMeshes(engine, actor, {engine.resolvePath(outRel)}, st->import, warnings, failed);
                        if (imported.size() == 0) {
                            return ToolResult::error(Error::make("import_failed", "Blender produced " + outRel + " but it could not be imported",
                                                                 warnings.empty() ? "" : warnings.front()));
                        }
                        out["imported"] = imported;
                        out["from"] = srcPath;
                        out["asset"] = outRel;
                        if (st->extra.contains("backup")) out["backup"] = st->extra.get("backup");
                        out["before"] = st->job.result.get("before");
                        out["after"] = st->job.result.get("after");
                        size_t rewired = 0;
                        if (st->extra.get("updateReferences").asBool()) {
                            Status s = engine.edit(actor, "Use " + outRel, [&]() -> Status {
                                rewired = engine.rewriteAssetReferences(srcPath, outRel);
                                return {};
                            });
                            if (!s) warnings.push_back("could not update references: " + s.error().message);
                            out["rewiredReferences"] = rewired;
                        }
                        std::string summary = "edited " + srcPath + " -> " + outRel;
                        const Json& b = st->job.result.get("before");
                        const Json& af = st->job.result.get("after");
                        if (b.contains("triangles") && af.contains("triangles")) {
                            summary += ": " + std::to_string(b.get("triangles").asInt()) + " -> " + std::to_string(af.get("triangles").asInt()) + " triangles";
                        }
                        if (st->extra.get("updateReferences").asBool()) summary += ", " + std::to_string(rewired) + " scene reference(s) updated";
                        for (const auto& w : warnings) summary += "\nwarning: " + w;
                        return ToolResult::json(out, summary);
                    },
                    [st] { st->cancel->cancel(); });
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    // --- dcc_generate -------------------------------------------------------------------------------
    {
        ToolDef def{
            "dcc_generate", "Model procedurally in Blender",
            "Create a 3D model by running a procedural modeling recipe or your own Blender script, then import it as an "
            "asset (and optionally place it). Recipes are deterministic (same seed, same model), sized in meters, standing "
            "on the ground with the origin at the bottom center, with PBR materials and per-face color variation: "
            "building {floors,width,depth,floor_height,windows_per_side,roof: gabled|hip|flat,wall_style: plaster|brick|"
            "stone|wood,door,chimney}, tower {radius,height,sides,wall_thickness,battlements,ruin 0-1,door,window_slits,"
            "rubble}, wall {length,height,thickness,ruin}, rock {radius,roughness,flatten,detail,moss}, stairs {steps,"
            "width,rise,run,landing,style}, arch {width,height,depth,thickness,segments}, fence {length,height,"
            "post_spacing,style: picket|rail}, column {radius,height,flutes,base,capital}, barrel {radius,height,staves,"
            "hoops}, terrain_chunk {size,resolution,height,roughness,flat_radius}; every recipe also takes `seed`. Compose "
            "a scene piece by calling it several times with different seeds. For anything custom pass `script` (Blender "
            "Python; create objects in bpy.context.scene.collection; `procedural.generate(...)`, `B`, `sky` are in scope) — "
            "all mesh objects in the scene are exported. `ops` can post-process (decimate, bevel, shade_smooth, bake_ao...; "
            "see dcc_edit_asset). Example: {\"recipe\": \"tower\", \"params\": {\"height\": 14, \"ruin\": 0.6, \"seed\": 3}, "
            "\"name\": \"RuinedTower\", \"place\": {\"position\": [10, 0, 5]}}. Needs Blender.",
            "dcc",
            object({{"recipe", enumeration(recipeNames(), "Procedural recipe")},
                    {"params", Json::object({{"type", "object"}, {"description", "Recipe parameters (see the list above), e.g. {\"floors\": 3, \"seed\": 2}"}})},
                    {"script", string("Custom Blender Python; runs after the recipe (if any); `objects` holds the recipe's object")},
                    {"name", string("Asset / entity name (default: the recipe name)")},
                    {"out_dir", string("Project-relative output folder (default dcc/generated)")},
                    {"ops", array(Json::object({{"type", "object"}}), "Post-processing, e.g. [{\"op\":\"bevel\",\"width\":0.02}]")},
                    {"timeout_s", integer("Give up after this many seconds (default 300, max 3600)")},
                    {"import", boolean("Import the result as an asset (default true)")},
                    {"place", importSchemaPlace()},
                    {"description", string("Asset description")},
                    {"tags", array(Json::object({{"type", "string"}}), "Asset tags")}}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext& ctx) -> ToolResult {
                auto blender = mgr->pick("blender", "generate");
                if (!blender) return ToolResult::error(blender.error());
                std::string recipe = a.get("recipe").asString();
                if (recipe.empty() && !a.contains("script")) {
                    return ToolResult::error(Error::make("invalid_arguments", "give a recipe or a script",
                                                         "recipes: " + [] { std::string s; for (const auto& r : recipeNames()) s += (s.empty() ? "" : ", ") + r; return s; }()));
                }
                std::string name = sanitize(a.get("name").asString(recipe.empty() ? "generated" : recipe), "generated");
                auto out = inProject(engine, a.get("out_dir").asString("dcc/generated"), "out_dir");
                if (!out) return ToolResult::error(out.error());
                auto st = std::make_shared<RunState>();
                st->app = *blender;
                st->spec.label = "generate";
                st->spec.task = "generate";
                st->spec.projectDir = engine.assets().root();
                st->spec.outDir = *out;
                st->spec.script = a.get("script").asString();
                st->spec.timeout = std::chrono::seconds(timeoutSeconds(a, 300));
                st->spec.params = Json::object({{"recipe", recipe}, {"name", name}, {"output", name + ".glb"}});
                if (a.get("params").isObject()) st->spec.params["recipe_params"] = a.get("params");
                if (a.contains("ops")) st->spec.params["ops"] = a.get("ops");
                if (!st->spec.script.empty()) st->spec.params["has_script"] = true;
                st->import = parseImport(a, "blender", true);
                if (st->import.place.enabled && st->import.place.name.empty()) st->import.place.name = name;
                st->import.source = provenance(*blender, "dcc_generate");
                if (!recipe.empty()) {
                    st->import.source["recipe"] = recipe;
                    st->import.source["params"] = a.get("params").isObject() ? a.get("params") : Json::object();
                }
                if (!st->spec.script.empty()) st->import.source["scriptSha"] = shortHash(st->spec.script);
                st->jobId = mgr->beginJob(baseLabel("generate", *blender), st->cancel);
                std::string actor = ctx.actor;
                return ToolResult::defer(
                    [mgr, st] {
                        JobScope scope{mgr, st->jobId};
                        st->job = mgr->run(st->spec, st->app, st->cancel);
                    },
                    [&engine, st, actor]() -> ToolResult {
                        if (!st->job.ok) return jobError(st->job, "generation");
                        Json out = jobJson(engine, *st);
                        std::vector<std::string> warnings;
                        std::string summary = "generated";
                        std::vector<std::string> glbs;
                        for (const auto& f : st->job.files) {
                            if (extOf(f.path) == ".glb") glbs.push_back(f.path);
                        }
                        const Json& file0 = st->job.result.get("files")[0];
                        if (file0.contains("triangles")) {
                            summary += " " + std::to_string(file0.get("triangles").asInt()) + " triangles";
                            Json size = file0.get("size_m");
                            if (size.size() == 3) {
                                char buf[96];
                                std::snprintf(buf, sizeof(buf), ", %.1f x %.1f x %.1f m (w x h x d)", size.elements()[0].asNumber(), size.elements()[1].asNumber(), size.elements()[2].asNumber());
                                summary += buf;
                            }
                        }
                        if (st->import.import) {
                            bool failed = false;
                            Json imported = importMeshes(engine, actor, glbs, st->import, warnings, failed);
                            out["imported"] = imported;
                            summary += textList(imported);
                        }
                        for (const auto& w : warnings) summary += "\nwarning: " + w;
                        return ToolResult::json(out, summary);
                    },
                    [st] { st->cancel->cancel(); });
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    // --- dcc_install_addon -----------------------------------------------------------------------------
    {
        ToolDef def{
            "dcc_install_addon", "Install the Blender add-on",
            "Install (and enable) the Skywalker Bridge add-on in the user's Blender, so a Blender window the human works in "
            "can be driven live: it adds a 'Skywalker' tab to the 3D viewport sidebar (N) with Start Bridge and Send Selection "
            "buttons. After the human clicks Start Bridge (or you use dcc_session_start for a windowless session), the "
            "dcc_session_* tools work. The add-on is copied into Blender's user scripts folder and enabled in the user's "
            "preferences (other preferences are preserved). Safe to run again to update it. Example: {}.",
            "dcc",
            object({{"enable", boolean("Also enable it in Blender's preferences (default true)")},
                    {"scripts_dir", string("Blender user scripts folder to install into (default: the standard per-OS location of the detected version)")},
                    {"config_dir", string("Blender user config folder (BLENDER_USER_CONFIG) used while enabling; advanced / for tests")}}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext&) -> ToolResult {
                auto blender = mgr->pick("blender", "live_session");
                if (!blender) return ToolResult::error(blender.error());
                std::string scriptsDir = a.get("scripts_dir").asString();
                const dcc::Host& host = mgr->machine().host;
                if (scriptsDir.empty()) scriptsDir = host.getenv("BLENDER_USER_SCRIPTS");
                if (scriptsDir.empty()) {
                    std::string mm = blender->version;
                    size_t d1 = mm.find('.');
                    size_t d2 = d1 == std::string::npos ? d1 : mm.find('.', d1 + 1);
                    if (d1 == std::string::npos) {
                        return ToolResult::error(Error::make("unknown_version", "could not determine the Blender version",
                                                             "pass scripts_dir explicitly (Blender's user scripts folder)"));
                    }
                    mm = mm.substr(0, d2);
                    if (host.os == "macos") scriptsDir = host.home + "/Library/Application Support/Blender/" + mm + "/scripts";
                    else if (host.os == "windows") scriptsDir = host.getenv("APPDATA") + "\\Blender Foundation\\Blender\\" + mm + "\\scripts";
                    else {
                        std::string cfg = host.getenv("XDG_CONFIG_HOME");
                        scriptsDir = (cfg.empty() ? host.home + "/.config" : cfg) + "/blender/" + mm + "/scripts";
                    }
                }
                auto st = std::make_shared<RunState>();
                st->app = *blender;
                st->extra = Json::object({{"scripts_dir", scriptsDir}});
                bool enable = a.get("enable").asBool(true);
                bool customScripts = a.contains("scripts_dir");
                std::string configDir = a.get("config_dir").asString();
                st->jobId = mgr->beginJob("install_addon", st->cancel);
                std::string projectDir = engine.assets().root();
                return ToolResult::defer(
                    [mgr, st, scriptsDir, enable, customScripts, configDir, projectDir] {
                        JobScope scope{mgr, st->jobId};
                        auto where = mgr->installAddon(scriptsDir);
                        if (!where) {
                            st->job.error = where.error().message;
                            return;
                        }
                        st->extra["installed"] = *where;
                        st->job.ok = true;
                        if (!enable) return;
                        dcc::JobSpec spec;
                        spec.label = "enable_addon";
                        spec.script =
                            "import addon_utils, bpy\n"
                            "addon_utils.modules_refresh()\n"
                            "addon_utils.enable('skywalker_bridge', default_set=True, persistent=True)\n"
                            "bpy.ops.wm.save_userpref()\n"
                            "sky.result(enabled=bool(addon_utils.check('skywalker_bridge')[1]))\n";
                        spec.projectDir = projectDir;
                        spec.outDir = mgr->logPath("enable-out");
                        spec.userPrefs = true;  // keep the user's other preferences when saving
                        spec.timeout = std::chrono::minutes(2);
                        if (customScripts) spec.env.emplace_back("BLENDER_USER_SCRIPTS", scriptsDir);
                        if (!configDir.empty()) spec.env.emplace_back("BLENDER_USER_CONFIG", configDir);
                        dcc::JobResult jr = mgr->run(spec, st->app, st->cancel);
                        st->extra["enabled"] = jr.ok && jr.result.get("enabled").asBool();
                        if (!jr.ok) st->extra["enableError"] = jr.error + (jr.log.empty() ? "" : "\n" + jr.log);
                    },
                    [st]() -> ToolResult {
                        if (!st->job.ok) return ToolResult::error(Error::make("install_failed", "could not install the add-on: " + st->job.error));
                        std::string summary = "installed the Skywalker Bridge add-on into " + st->extra.get("installed").asString();
                        if (st->extra.get("enabled").asBool()) summary += " and enabled it";
                        else if (st->extra.contains("enableError")) summary += "; enabling failed: " + st->extra.get("enableError").asString();
                        summary += ". In Blender: press N in the 3D viewport, open the Skywalker tab and click Start Bridge.";
                        return ToolResult::json(st->extra, summary);
                    },
                    [st] { st->cancel->cancel(); });
            }};
        def.openWorld = true;
        reg.add(std::move(def));
    }

    // --- live sessions -----------------------------------------------------------------------------------
    {
        reg.add({"dcc_session_status", "Live Blender session status",
                 "Is a live Blender session available, and what is in it? Reports version, open file, scene, object counts and "
                 "the current selection, or why not connected and how to connect (dcc_session_start for a windowless session, "
                 "or dcc_install_addon and the Start Bridge button for a Blender window the human uses). Example: {}.",
                 "dcc", object({}), false, false, [mgr](const Json&, ToolContext&) -> ToolResult {
                     auto s = mgr->session();
                     if (!s) {
                         return ToolResult::json(Json::object({{"connected", false}, {"reason", s.error().message}, {"hint", s.error().hint}}),
                                                 "not connected: " + s.error().message + "\nhint: " + s.error().hint);
                     }
                     auto info = std::make_shared<Json>();
                     auto err = std::make_shared<Error>(Error::make("", ""));
                     dcc::Manager::SessionInfo si = *s;
                     return ToolResult::defer(
                         [mgr, si, info, err] {
                             auto ping = mgr->sessionCall(si, "ping", Json::object(), std::chrono::seconds(5));
                             if (!ping) {
                                 *err = ping.error();
                                 return;
                             }
                             auto status = mgr->sessionCall(si, "status", Json::object({{"timeout", 10}}), std::chrono::seconds(12));
                             *info = status ? *status : Json::object();
                             (*info)["idle_s"] = ping->get("main_thread_idle_s");
                             if (!status) (*info)["busy"] = status.error().message;
                         },
                         [si, info, err]() -> ToolResult {
                             if (!err->code.empty()) {
                                 return ToolResult::json(Json::object({{"connected", false}, {"reason", err->message}}), "not connected: " + err->message);
                             }
                             Json out = *info;
                             out["connected"] = true;
                             out["pid"] = static_cast<double>(si.pid);
                             out["port"] = si.port;
                             std::string text = "connected to Blender " + out.get("version").asString() + " (" + si.mode + ")";
                             if (out.contains("busy")) text += " — busy: " + out.get("busy").asString();
                             else text += ", " + std::to_string(out.get("objects").asInt()) + " object(s), " + std::to_string(out.get("selected").size()) + " selected";
                             return ToolResult::json(out, text);
                         });
                 }});

        ToolDef start{
            "dcc_session_start", "Start a live Blender session",
            "Launch Blender with the Skywalker bridge running, so you can work interactively across many calls (state persists: "
            "build a scene step by step with dcc_session_exec, inspect it, then pull the result back with "
            "dcc_session_pull_selection). headless:true (default) runs without a window; headless:false opens Blender's "
            "window so the human can watch and edit alongside you. `open` loads a project mesh asset into the session. "
            "Reuses a session that is already running. Example: {\"open\": \"props/statue.glb\", \"headless\": false}.",
            "dcc",
            object({{"headless", boolean("No window (default true); false opens Blender's UI")},
                    {"open", string("Project mesh asset to import into the session")},
                    {"timeout_s", integer("Wait this long for Blender to come up (default 90)")}}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext&) -> ToolResult {
                auto blender = mgr->pick("blender", "live_session");
                if (!blender) return ToolResult::error(blender.error());
                std::string open;
                if (a.contains("open")) {
                    const AssetRecord* rec = engine.assets().find(a.get("open").asString());
                    if (!rec) return ToolResult::error(Error::make("not_found", "no asset " + a.get("open").asString(), "use asset_list"));
                    open = engine.resolvePath(rec->path);
                }
                auto result = std::make_shared<Result<dcc::Manager::SessionInfo>>(Error::make("", ""));
                auto token = std::make_shared<dcc::CancelToken>();
                dcc::AppInfo app = *blender;
                bool headless = a.get("headless").asBool(true);
                auto wait = std::chrono::seconds(timeoutSeconds(a, 90));
                std::string projectDir = engine.assets().root();
                return ToolResult::defer(
                    [mgr, app, headless, open, projectDir, wait, result, token] {
                        *result = mgr->startSession(app, headless, open, projectDir, wait, token);
                    },
                    [result]() -> ToolResult {
                        if (!*result) return ToolResult::error((*result).error());
                        const auto& s = **result;
                        return ToolResult::json(Json::object({{"connected", true}, {"mode", s.mode}, {"version", s.version}, {"pid", static_cast<double>(s.pid)}, {"port", s.port}}),
                                                "Blender " + s.version + " session ready (" + s.mode + ")");
                    },
                    [token] { token->cancel(); });
            }};
        start.openWorld = true;
        reg.add(std::move(start));

        ToolDef stop{"dcc_session_stop", "Stop the live Blender session",
                     "Stop the bridge. A windowless session started by dcc_session_start exits; a Blender window stays open (only "
                     "the bridge stops). Example: {}.",
                     "dcc", object({}), true, false,
                     [mgr](const Json&, ToolContext&) -> ToolResult {
                         auto st = std::make_shared<Status>();
                         return ToolResult::defer([mgr, st] { *st = mgr->stopSession(); },
                                                  [st]() -> ToolResult {
                                                      if (!*st) return ToolResult::error(st->error());
                                                      return ToolResult::text("session stopped");
                                                  });
                     }};
        stop.openWorld = true;
        reg.add(std::move(stop));

        ToolDef exec{
            "dcc_session_exec", "Run Python in the live Blender session",
            "Execute Python inside the running Blender session (bpy, bmesh, `B` = skywalker_dcc.blender and `sky` are in scope; "
            "variables persist between calls, like a REPL). Returns stdout/stderr and the value of the last expression (or of "
            "a variable named `result`). In a Blender window the change is one undo step the human can Ctrl+Z. Errors come "
            "back with the Python traceback. Keep individual calls short (Blender's UI waits for them). Examples: "
            "{\"code\": \"[o.name for o in bpy.context.scene.objects]\"}, {\"code\": \"bpy.ops.mesh.primitive_uv_sphere_add(radius=1)\"}. "
            "The code is executable: the human is asked to approve in the editor.",
            "dcc",
            object({{"code", string("Python source")},
                    {"timeout_s", integer("Give up waiting after this many seconds (default 120)")},
                    {"reset", boolean("Start with a fresh namespace")},
                    {"undo", boolean("Push an undo step in a Blender window (default true)")}},
                   {"code"}),
            true, false,
            [mgr](const Json& a, ToolContext&) -> ToolResult {
                auto s = mgr->session();
                if (!s) return ToolResult::error(s.error());
                auto response = std::make_shared<Result<Json>>(Error::make("", ""));
                auto token = std::make_shared<dcc::CancelToken>();
                dcc::Manager::SessionInfo si = *s;
                Json params = Json::object({{"code", a.get("code")}, {"reset", a.get("reset").asBool(false)}, {"undo", a.get("undo").asBool(true)}});
                int timeout = timeoutSeconds(a, 120);
                params["timeout"] = timeout;
                return ToolResult::defer(
                    [mgr, si, params, timeout, response, token] {
                        *response = mgr->sessionCall(si, "exec", params, std::chrono::seconds(timeout + 5), token);
                    },
                    [response]() -> ToolResult {
                        if (!*response) return ToolResult::error((*response).error());
                        const Json& r = **response;
                        std::string text = r.get("stdout").asString();
                        if (!r.get("stderr").asString().empty()) text += (text.empty() ? "" : "\n") + r.get("stderr").asString();
                        if (!r.get("result").isNull()) text += (text.empty() ? "" : "\n") + std::string("=> ") + r.get("result").dump();
                        return ToolResult::json(r, text.empty() ? "ok" : text);
                    },
                    [token] { token->cancel(); });
            }};
        exec.openWorld = true;
        reg.add(std::move(exec));

        ToolDef pull{
            "dcc_session_pull_selection", "Bring Blender's selection into the project",
            "Export the objects currently selected in the live Blender session (or the named objects) to glTF under "
            "dcc/live/<name>.glb, import them as an asset and place them (a new asset is placed at the origin unless you pass "
            "`place`; place:false imports only). Calling it again with the same name "
            "UPDATES the asset in place: every scene entity that uses it refreshes live (the loop for iterating on a model in "
            "Blender while seeing it in the engine). Children of selected objects come along; modifiers are applied. Real-world "
            "size is kept. `origin`: keep | bottom_center | center. Example: {\"name\": \"Lantern\", \"origin\": "
            "\"bottom_center\", \"place\": {\"position\": [4,0,2]}}.",
            "dcc",
            object({{"names", array(Json::object({{"type", "string"}}), "Blender object names (default: the selection)")},
                    {"name", string("Asset name (default: the active object's name)")},
                    {"origin", enumeration({"keep", "bottom_center", "center"}, "Where the origin ends up (default keep)")},
                    {"apply_modifiers", boolean("Bake modifiers (default true)")},
                    {"timeout_s", integer("Give up after this many seconds (default 120)")},
                    {"import", boolean("Import as an asset (default true)")},
                    {"normalize", boolean("Scale to fit 1 m on import (default false)")},
                    {"place", importSchemaPlace()},
                    {"description", string("Asset description")},
                    {"tags", array(Json::object({{"type", "string"}}), "Asset tags")}}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext& ctx) -> ToolResult {
                auto s = mgr->session();
                if (!s) return ToolResult::error(s.error());
                struct Pull {
                    Result<Json> response = Error::make("", "");
                    std::shared_ptr<dcc::CancelToken> token = std::make_shared<dcc::CancelToken>();
                    std::string name, abs;
                    bool existed = false;
                    ImportSpec import;
                };
                auto st = std::make_shared<Pull>();
                st->import = parseImport(a, "blender", true);
                st->import.source = Json::object({{"generator", "blender " + s->version}, {"tool", "dcc_session_pull_selection"}, {"live", true}});
                st->name = a.get("name").asString();
                std::string dir = engine.resolvePath("dcc/live");
                dcc::Manager::SessionInfo si = *s;
                Json names = a.get("names").isArray() ? stringArray(a.get("names")) : Json();
                std::string origin = a.get("origin").asString("keep");
                bool applyMods = a.get("apply_modifiers").asBool(true);
                int timeout = timeoutSeconds(a, 120);
                bool placeExplicit = a.contains("place");
                std::string actor = ctx.actor;
                return ToolResult::defer(
                    [mgr, si, st, dir, names, origin, applyMods, timeout] {
                        if (st->name.empty()) {
                            Json p = names.isArray() ? Json::object({{"names", names}}) : Json::object();
                            auto sel = mgr->sessionCall(si, "selection", p, std::chrono::seconds(timeout), st->token);
                            if (!sel) {
                                st->response = sel.error();
                                return;
                            }
                            st->name = sel->get("active").asString();
                            if (st->name.empty() && sel->get("objects").size()) st->name = sel->get("objects")[0].get("name").asString();
                        }
                        st->name = sanitize(st->name, "selection");
                        st->abs = (fs::path(dir) / (st->name + ".glb")).string();
                        std::error_code ec;
                        st->existed = fs::exists(st->abs, ec);
                        fs::create_directories(dir, ec);
                        Json p = Json::object({{"path", st->abs}, {"origin", origin}, {"apply_modifiers", applyMods}, {"timeout", timeout}});
                        if (names.isArray()) p["names"] = names;
                        st->response = mgr->sessionCall(si, "export_selection", p, std::chrono::seconds(timeout + 5), st->token);
                    },
                    [&engine, st, actor, placeExplicit]() -> ToolResult {
                        if (!st->response) return ToolResult::error(st->response.error());
                        Json out = *st->response;
                        std::vector<std::string> warnings;
                        std::string summary = "exported " + std::to_string(out.get("objects").size()) + " object(s) from Blender";
                        if (st->import.import) {
                            // Updating an asset refreshes its entities by itself; only a new one is placed by default.
                            if (!placeExplicit) st->import.place.enabled = !st->existed;
                            if (st->import.place.enabled && st->import.place.name.empty()) st->import.place.name = st->name;
                            bool failed = false;
                            Json imported = importMeshes(engine, actor, {st->abs}, st->import, warnings, failed);
                            out["imported"] = imported;
                            out["updated"] = st->existed;
                            summary += std::string(st->existed ? "; updated " : "; imported ") + textList(imported);
                        }
                        for (const auto& w : warnings) summary += "\nwarning: " + w;
                        return ToolResult::json(out, summary);
                    },
                    [st] { st->token->cancel(); });
            }};
        pull.openWorld = true;
        reg.add(std::move(pull));

        ToolDef send{
            "dcc_session_send", "Send a project asset into Blender",
            "Import a project mesh asset into the live Blender session (it appears selected, and the view frames it) so you or "
            "the human can edit it there; bring it back with dcc_session_pull_selection. Any mesh format the bridge can import "
            "works (.glb .gltf .obj .fbx .ply .stl...). Example: {\"asset\": \"props/crate.glb\"}.",
            "dcc",
            object({{"asset", string("Mesh asset path or guid")},
                    {"scale", number("Uniform scale on import (default 1)")},
                    {"timeout_s", integer("Give up after this many seconds (default 120)")}},
                   {"asset"}),
            true, false,
            [&engine, mgr](const Json& a, ToolContext&) -> ToolResult {
                auto s = mgr->session();
                if (!s) return ToolResult::error(s.error());
                const AssetRecord* rec = engine.assets().find(a.get("asset").asString());
                if (!rec) return ToolResult::error(Error::make("not_found", "no asset " + a.get("asset").asString(), "use asset_list type=mesh"));
                if (rec->type != AssetType::Mesh) {
                    return ToolResult::error(Error::make("invalid_arguments", rec->path + " is not a mesh"));
                }
                auto response = std::make_shared<Result<Json>>(Error::make("", ""));
                auto token = std::make_shared<dcc::CancelToken>();
                dcc::Manager::SessionInfo si = *s;
                int timeout = timeoutSeconds(a, 120);
                Json params = Json::object({{"path", engine.resolvePath(rec->path)}, {"scale", a.get("scale").asNumber(1.0)}, {"timeout", timeout}});
                std::string path = rec->path;
                return ToolResult::defer(
                    [mgr, si, params, timeout, response, token] { *response = mgr->sessionCall(si, "import_file", params, std::chrono::seconds(timeout + 5), token); },
                    [response, path]() -> ToolResult {
                        if (!*response) return ToolResult::error((*response).error());
                        Json out = **response;
                        return ToolResult::json(out, "sent " + path + " to Blender: " + std::to_string(out.get("objects").size()) + " object(s)");
                    },
                    [token] { token->cancel(); });
            }};
        send.openWorld = true;
        reg.add(std::move(send));
    }

    // --- dcc_receive ----------------------------------------------------------------------------------------
    reg.add({"dcc_receive", "Receive a model exported by a design app",
             "Copy a model file a design app just exported (an absolute path, e.g. from the Blender add-on's Send Selection "
             "button) into the project under dcc/live/<name>, import it as an asset and place it. A file with the same name "
             "updates the existing asset in place (its entities refresh live) and is not placed again unless place:true. "
             "Agents normally use dcc_session_pull_selection instead; this is the entry point for the Blender add-on. "
             "Example: {\"file\": \"/tmp/lantern.glb\", \"name\": \"Lantern\"}.",
             "dcc",
             object({{"file", string("Absolute path of a .glb/.gltf/.obj/.ply/.stl file")},
                     {"name", string("Asset name (default: the file name)")},
                     {"place", importSchemaPlace()},
                     {"normalize", boolean("Scale to fit 1 m on import (default false)")},
                     {"description", string("Asset description")}},
                    {"file"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) -> ToolResult {
                 std::string src = a.get("file").asString();
                 std::error_code ec;
                 if (!fs::path(src).is_absolute() || !fs::is_regular_file(src, ec)) {
                     return ToolResult::error(Error::make("not_found", "file must be an existing absolute path: " + src));
                 }
                 std::string ext = extOf(src);
                 if (!isNativeMesh(src)) {
                     return ToolResult::error(Error::make("unsupported", "cannot receive '" + ext + "' files", "export glTF (.glb) from the design app"));
                 }
                 std::string name = sanitize(a.get("name").asString(fs::path(src).stem().string()), "model");
                 std::string rel = "dcc/live/" + name + ext;
                 std::string dest = engine.resolvePath(rel);
                 bool existed = fs::exists(dest, ec);
                 fs::create_directories(fs::path(dest).parent_path(), ec);
                 fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                 if (ec) return ToolResult::error(Error::make("io_error", "cannot copy into the project: " + ec.message()));
                 ImportSpec spec = parseImport(a, "blender", true);
                 spec.source = Json::object({{"generator", "blender"}, {"tool", "dcc_receive"}, {"live", true}});
                 if (!a.contains("place")) spec.place.enabled = !existed;  // updating refreshes existing entities by itself
                 if (spec.place.enabled && spec.place.name.empty()) spec.place.name = name;
                 std::vector<std::string> warnings;
                 bool failed = false;
                 Json imported = importMeshes(engine, ctx.actor, {dest}, spec, warnings, failed);
                 if (imported.size() == 0) {
                     return ToolResult::error(Error::make("import_failed", "could not import " + rel, warnings.empty() ? "" : warnings.front()));
                 }
                 Json out = imported.elements()[0];
                 out["updated"] = existed;
                 return ToolResult::json(out, std::string(existed ? "updated " : "imported ") + rel + (out.contains("entity") ? " and placed it" : ""));
             }});

    // --- dcc_cancel ------------------------------------------------------------------------------------
    reg.add({"dcc_cancel", "Cancel a design-app job",
             "Stop a running design-app job (its id is in dcc_list's `jobs`). The app is terminated and the call that "
             "started it returns a 'cancelled' error. Example: {\"job\": 3}.",
             "dcc", object({{"job", integer("Job id from dcc_list")}}, {"job"}),
             false, false, [mgr](const Json& a, ToolContext&) {
                 int id = static_cast<int>(a.get("job").asInt());
                 if (!mgr->cancelJob(id)) {
                     return ToolResult::error(Error::make("not_found", "no running job " + std::to_string(id), "see dcc_list -> jobs"));
                 }
                 return ToolResult::text("cancelling job " + std::to_string(id));
             }});

    return mgr;
}

}  // namespace sky::tools
