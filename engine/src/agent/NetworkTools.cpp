// Network tools: download openly licensed assets from the web into the project, with
// provenance, license tracking (CREDITS.md) and optional import. Open-world: MCP clients and
// the in-editor crew ask the human before every download.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/assets/Fetch.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/MeshData.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

constexpr size_t kMaxDownload = size_t{300} << 20;

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string extOf(const std::string& name) {
    std::string l = str::lower(name);
    auto dot = l.rfind('.');
    return dot == std::string::npos ? "" : l.substr(dot);
}

/// File kinds we accept from the web: meshes and their side files, images, audio, license texts.
bool allowedFile(const std::string& name) {
    static const std::set<std::string> ok{".glb", ".gltf", ".bin", ".obj", ".mtl", ".ply", ".stl", ".png", ".jpg",
                                          ".jpeg", ".hdr", ".wav", ".mp3", ".ogg", ".m4a", ".txt", ".md",
                                          // Convertible with Blender (dcc_convert); not imported directly.
                                          ".fbx", ".dae", ".3ds", ".usd", ".usda", ".usdc", ".usdz", ".abc", ".blend"};
    return ok.count(extOf(name)) > 0;
}

std::string sanitize(std::string name) {
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
    }
    return name.empty() ? std::string("download") : name;
}

std::string fileNameFor(const net::HttpResponse& r, const std::string& url) {
    std::string u = (r.finalUrl.empty() ? url : r.finalUrl);
    u = u.substr(0, u.find_first_of("?#"));
    std::string name = sanitize(u.substr(u.rfind('/') + 1));
    if (extOf(name).empty()) {
        std::string ct = str::lower(r.contentType);
        if (ct.find("gltf-binary") != std::string::npos) name += ".glb";
        else if (ct.find("gltf") != std::string::npos) name += ".gltf";
        else if (ct.find("zip") != std::string::npos) name += ".zip";
        else if (ct.find("png") != std::string::npos) name += ".png";
        else if (ct.find("jpeg") != std::string::npos) name += ".jpg";
    }
    return name;
}

/// Relative URI inside a downloaded file (glTF buffers/images, OBJ mtllib, MTL maps) → safe
/// relative path, or "" if it would escape the folder.
std::string safeRelative(std::string uri) {
    if (uri.empty() || str::startsWith(uri, "data:") || uri.find("://") != std::string::npos) return "";
    std::replace(uri.begin(), uri.end(), '\\', '/');
    fs::path p = fs::path(uri).lexically_normal();
    std::string s = p.generic_string();
    if (s.empty() || s[0] == '/' || str::startsWith(s, "..")) return "";
    return s;
}

}  // namespace

void addNetworkTools(Engine& engine, ToolRegistry& reg) {
    ToolDef def{
        "asset_download", "Download asset from the web",
        "Download an openly licensed asset (3D model .glb/.gltf/.obj/.ply/.stl, texture, .hdr sky panorama, audio, or a .zip pack of "
        "them) from a URL into the project, record its license and author, add it to CREDITS.md, and (for models) "
        "import it — optionally placing it in the scene. Multi-file glTF and OBJ+MTL dependencies are fetched "
        "automatically. RULES: only use assets whose license allows this project's use (CC0, CC-BY with "
        "attribution, MIT, public domain, or terms the human accepted); never bypass logins, paywalls or download "
        "limits; give the license exactly as stated on the source page. The human approves every download.",
        "network",
        object({{"url", string("Direct file URL (http/https)")},
                {"license", string("License as stated by the source, e.g. \"CC0-1.0\", \"CC-BY-4.0\", \"MIT\"")},
                {"author", string("Creator / copyright holder")},
                {"source_page", string("Page the asset was found on (for credits)")},
                {"attribution", string("Exact attribution text required by the license, if any")},
                {"folder", string("Project folder to save into (default downloads/<name>)")},
                {"include", Json::object({{"type", "object"},
                                          {"additionalProperties", Json::object({{"type", "string"}})},
                                          {"description",
                                           "Extra files of a multi-file asset as {\"relative/path\": \"url\"} — for libraries "
                                           "that host a model's textures/buffers elsewhere (e.g. Poly Haven's file API lists them "
                                           "under `include`)."}})},
                {"import", boolean("Import models after download (default true)")},
                {"normalize", boolean("Scale models to fit 1 m (default true); false keeps real-world units")},
                {"z_up", boolean("The model is Z-up (CAD, scans, some exporters) — rotate to Y-up")},
                {"create_entity", string("Also place the model as a new entity with this name")},
                {"position", vec3("Position for the new entity")}},
               {"url", "license"}),
        true, false,
        [&engine](const Json& a, ToolContext& ctx) {
            std::string url = str::trim(a.get("url").asString());
            std::string license = str::trim(a.get("license").asString());
            std::string licLower = str::lower(license);
            for (const char* blocked : {"unknown", "none", "all rights reserved", "proprietary", "no license", "?"}) {
                if (licLower.empty() || licLower == blocked) {
                    return ToolResult::error(Error::make("license_required",
                                                         "a usable license is required to download an asset (got \"" + license + "\")",
                                                         "find the license on the source page; if there is none, don't use the asset"));
                }
            }
            std::vector<std::string> warnings;
            if (licLower.find("nc") != std::string::npos && licLower.find("cc") != std::string::npos) {
                warnings.push_back("non-commercial license: the game cannot be sold with this asset");
            }
            if (licLower.find("nd") != std::string::npos && licLower.find("cc") != std::string::npos) {
                warnings.push_back("no-derivatives license: do not modify the asset");
            }
            if (licLower.find("by") != std::string::npos && a.get("author").asString().empty() && a.get("attribution").asString().empty()) {
                warnings.push_back("attribution license without an author: add author/attribution so CREDITS.md is correct");
            }

            auto resp = net::get(url, kMaxDownload);
            if (!resp) return ToolResult::error(resp.error());
            std::string name = fileNameFor(*resp, url);
            std::string stem = fs::path(name).stem().string();
            std::string folder = a.get("folder").asString("downloads/" + stem);
            std::string folderAbs = engine.resolvePath(folder);
            if (engine.assets().relative(folderAbs).empty()) {
                return ToolResult::error(Error::make("invalid_path", "folder must be inside the project"));
            }
            std::error_code ec;
            fs::create_directories(folderAbs, ec);

            std::vector<std::string> written, skipped;
            auto save = [&](const std::string& rel, const std::vector<uint8_t>& bytes) -> Status {
                fs::path p = fs::path(folderAbs) / rel;
                fs::create_directories(p.parent_path(), ec);
                std::ofstream f(p, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!f) return Error::make("io_error", "cannot write " + p.string());
                written.push_back(engine.assets().relative(p.string()));
                return {};
            };
            std::string baseUrl = resp->finalUrl.empty() ? url : resp->finalUrl;
            auto fetchSide = [&](const std::string& uri) {
                std::string rel = safeRelative(uri);
                if (rel.empty() || fs::exists(fs::path(folderAbs) / rel)) return;
                auto side = net::get(net::resolveUrl(baseUrl, uri), kMaxDownload);
                if (side && allowedFile(rel)) (void)save(rel, side->body);
                else skipped.push_back(uri);
            };

            if (extOf(name) == ".zip") {
                auto entries = zip::extract(resp->body);
                if (!entries) return ToolResult::error(entries.error());
                for (const auto& e : *entries) {
                    if (!allowedFile(e.name)) {
                        skipped.push_back(e.name);
                        continue;
                    }
                    if (Status s = save(e.name, e.data); !s) return fail(s);
                }
            } else {
                if (!allowedFile(name)) {
                    return ToolResult::error(Error::make("unsupported_file", "won't download '" + name + "' (unsupported type)",
                                                         "models: .glb .gltf .obj .ply .stl, or .fbx .dae .usd .blend to convert with dcc_convert "
                                                         "(or a .zip of them); images: .png .jpg .hdr; audio"));
                }
                if (Status s = save(name, resp->body); !s) return fail(s);
                // Explicitly listed side files first, so relative fetches below find them in place.
                for (const auto& [rel, src] : a.get("include").members()) {
                    std::string safe = safeRelative(rel);
                    if (safe.empty() || !allowedFile(safe) || !src.isString()) {
                        skipped.push_back(rel);
                        continue;
                    }
                    auto side = net::get(src.asString(), kMaxDownload);
                    if (!side) return ToolResult::error(side.error());
                    if (Status s = save(safe, side->body); !s) return fail(s);
                }
                if (extOf(name) == ".gltf") {
                    auto j = Json::parse(std::string_view(reinterpret_cast<const char*>(resp->body.data()), resp->body.size()));
                    if (j) {
                        for (const char* key : {"buffers", "images"}) {
                            for (const auto& item : j->get(key).elements()) fetchSide(item.get("uri").asString());
                        }
                    }
                } else if (extOf(name) == ".obj") {
                    std::string text(resp->body.begin(), resp->body.end());
                    std::istringstream in(text);
                    std::string line;
                    while (std::getline(in, line)) {
                        if (line.rfind("mtllib", 0) != 0) continue;
                        std::string mtl = str::trim(line.substr(6));
                        fetchSide(mtl);
                        std::ifstream mf(fs::path(folderAbs) / safeRelative(mtl));
                        std::string ml;
                        while (std::getline(mf, ml)) {
                            std::istringstream ls(ml);
                            std::string tag, tok, last;
                            ls >> tag;
                            if (tag.rfind("map_", 0) != 0 && tag != "bump" && tag != "norm") continue;
                            while (ls >> tok) last = tok;
                            if (!last.empty()) fetchSide(last);
                        }
                    }
                }
            }
            if (written.empty()) return ToolResult::error(Error::make("nothing_usable", "the download contained no usable files"));

            // Provenance on every file + CREDITS.md
            engine.refreshAssets();
            Json source = Json::object({{"url", url}, {"license", license}, {"retrieved", today()}, {"downloadedBy", ctx.actor}});
            for (const char* k : {"author", "source_page", "attribution"}) {
                if (a.contains(k)) source[k] = a.get(k);
            }
            for (const auto& f : written) {
                if (auto rec = engine.assets().registerFile(engine.resolvePath(f))) {
                    (void)engine.assets().updateMeta((*rec)->path, Json::object({{"source", source}, {"tags", Json::array({"downloaded"})}}));
                }
            }
            std::string credits = "- **" + stem + "**" + (a.contains("author") ? " by " + a.get("author").asString() : "") + " — " +
                                  license + " — " + a.get("source_page").asString(url) + " (`" + folder + "`, " + today() + ")";
            if (a.contains("attribution")) credits += "\n  " + a.get("attribution").asString();
            {
                std::string creditsPath = engine.resolvePath("CREDITS.md");
                bool exists = fs::exists(creditsPath, ec);
                std::string existing;
                if (exists) {
                    std::ifstream in(creditsPath);
                    std::stringstream ss;
                    ss << in.rdbuf();
                    existing = ss.str();
                }
                // Re-downloading the same asset (fresh checkout, rebuilt scene) doesn't duplicate its credit.
                std::string key = "**" + stem + "**";
                std::string where = a.get("source_page").asString(url);
                bool known = existing.find(key) != std::string::npos && existing.find(where) != std::string::npos;
                if (!known) {
                    std::ofstream cf(creditsPath, std::ios::app);
                    if (!exists) cf << "# Credits\n\nThird-party assets used in this project (maintained by `asset_download`).\n\n";
                    cf << credits << "\n";
                }
            }

            Json result = Json::object({{"license", license}, {"credits", credits}});
            Json files = Json::array();
            for (const auto& f : written) files.push(f);
            result["files"] = files;
            if (!skipped.empty()) {
                Json sk = Json::array();
                for (const auto& f : skipped) sk.push(f);
                result["skipped"] = sk;
            }

            // Formats the engine cannot read directly are converted through Blender (dcc_convert).
            size_t convertible = 0;
            for (const auto& f : written) {
                static const std::set<std::string> conv{".fbx", ".dae", ".3ds", ".usd", ".usda", ".usdc", ".usdz", ".abc", ".blend"};
                if (conv.count(extOf(f))) ++convertible;
            }
            if (convertible) {
                result["convert_hint"] = std::to_string(convertible) + " file(s) (FBX/Collada/USD/.blend) need converting to glTF: call dcc_convert {\"path\": \"" +
                                         folder + "\", \"recursive\": true}";
            }

            // Import the best model file: glb > gltf > obj > ply > stl
            if (a.get("import").asBool(true)) {
                std::string meshFile;
                for (const char* ext : {".glb", ".gltf", ".obj", ".ply", ".stl"}) {
                    for (const auto& f : written) {
                        if (meshFile.empty() && extOf(f) == ext) meshFile = f;
                    }
                }
                if (!meshFile.empty()) {
                    Engine::MeshImportOptions opts;
                    opts.normalize = a.get("normalize").asBool(true);
                    opts.zUp = a.get("z_up").asBool(false);
                    auto imp = engine.importMeshAsset(meshFile, opts);
                    if (!imp) return ToolResult::error(imp.error());
                    for (const auto& [k, v] : imp->members()) result[k] = v;
                    if (a.contains("create_entity")) {
                        EntityId id = kNoEntity;
                        Status st = engine.edit(ctx.actor, "Place " + a.get("create_entity").asString(), [&]() -> Status {
                            return placeImportedMesh(engine, *imp, a.get("create_entity").asString(), a.get("position"), id);
                        });
                        if (!st) return fail(st);
                        result["entity"] = id;
                    }
                }
            }
            std::string summary = "downloaded " + std::to_string(written.size()) + " file(s) into " + folder + " (" + license + ")";
            if (result.contains("mesh")) summary += "; imported " + result.get("mesh").asString();
            for (const auto& w : warnings) summary += "\nwarning: " + w;
            if (result.contains("convert_hint")) summary += "\nnext: " + result.get("convert_hint").asString();
            if (!warnings.empty()) {
                Json wj = Json::array();
                for (const auto& w : warnings) wj.push(w);
                result["warnings"] = wj;
            }
            return ToolResult::json(result, summary);
        }};
    def.openWorld = true;
    reg.add(std::move(def));
}

}  // namespace sky::tools
