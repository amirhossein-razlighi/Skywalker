#include "skywalker/assets/AssetDatabase.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"

namespace sky {

namespace fs = std::filesystem;

const char* toString(AssetType t) {
    switch (t) {
        case AssetType::Unknown: return "unknown";
        case AssetType::Mesh: return "mesh";
        case AssetType::Texture: return "texture";
        case AssetType::Material: return "material";
        case AssetType::Prefab: return "prefab";
        case AssetType::Scene: return "scene";
        case AssetType::Audio: return "audio";
        case AssetType::Video: return "video";
        case AssetType::Script: return "script";
        case AssetType::Agent: return "agent";
        case AssetType::Animation: return "animation";
        case AssetType::Controller: return "controller";
        case AssetType::Sequence: return "sequence";
    }
    return "unknown";
}

AssetType assetTypeFromString(std::string_view s) {
    for (auto t : {AssetType::Mesh, AssetType::Texture, AssetType::Material, AssetType::Prefab, AssetType::Scene,
                   AssetType::Audio, AssetType::Video, AssetType::Script, AssetType::Agent, AssetType::Animation,
                   AssetType::Controller, AssetType::Sequence}) {
        if (s == toString(t)) return t;
    }
    return AssetType::Unknown;
}

AssetType assetTypeForPath(std::string_view path) {
    std::string p = str::lower(path);
    auto ends = [&](std::string_view suffix) {
        return p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (ends(".mat.json")) return AssetType::Material;
    if (ends(".prefab.json")) return AssetType::Prefab;
    if (ends(".sky.json")) return AssetType::Scene;
    if (ends(".agent.json")) return AssetType::Agent;
    if (ends(".anim")) return AssetType::Animation;
    if (ends(".animctl.json")) return AssetType::Controller;
    if (ends(".sequence.json")) return AssetType::Sequence;
    if (ends(".obj") || ends(".glb") || ends(".gltf") || ends(".ply") || ends(".stl")) return AssetType::Mesh;
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".hdr")) return AssetType::Texture;
    if (ends(".wav") || ends(".mp3") || ends(".ogg") || ends(".m4a") || ends(".flac")) return AssetType::Audio;
    if (ends(".mp4") || ends(".mov") || ends(".webm")) return AssetType::Video;
    if (ends(".wander")) return AssetType::Script;
    return AssetType::Unknown;
}

Json AssetRecord::toJson() const {
    Json t = Json::array();
    for (const auto& tag : tags) t.push(tag);
    Json j = Json::object({{"guid", guid}, {"path", path}, {"type", toString(type)}, {"tags", t}, {"size", size}});
    if (!description.empty()) j["description"] = description;
    if (source.size()) j["source"] = source;
    if (importSettings.size()) j["import"] = importSettings;
    return j;
}

AssetDatabase::AssetDatabase(std::string root) : root_(fs::path(root).lexically_normal().string()) {}

namespace {
/// "kit/a/b.png" -> ("kit", "a/b.png"); ("", path) without a separator.
std::pair<std::string_view, std::string_view> firstComponent(std::string_view p) {
    size_t slash = p.find_first_of("/\\");
    if (slash == std::string_view::npos) return {p, {}};
    return {p.substr(0, slash), p.substr(slash + 1)};
}

/// `abs` relative to `root` ("" when outside it), lexically first, canonically as a fallback.
std::string relativeTo(const fs::path& abs, const std::string& root) {
    std::error_code ec;
    std::string s = abs.lexically_normal().lexically_relative(fs::path(root).lexically_normal()).generic_string();
    if (s.empty() || s.rfind("..", 0) == 0) {
        fs::path rel = fs::relative(abs.lexically_normal(), fs::path(root), ec);
        s = ec ? std::string() : rel.generic_string();
    }
    if (s.empty() || s == "." || s.rfind("..", 0) == 0) return {};
    return s;
}
}  // namespace

void AssetDatabase::setMounts(std::vector<AssetMount> mounts) {
    for (auto& m : mounts) m.root = fs::path(m.root).lexically_normal().string();
    while (true) {  // forget records of mounts that went away (they are rescanned on refresh)
        bool erased = false;
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            const AssetMount* old = mountOf(it->first);
            if (!old) continue;
            bool kept = std::any_of(mounts.begin(), mounts.end(), [&](const AssetMount& m) { return m.name == old->name && m.root == old->root; });
            if (kept) continue;
            byGuid_.erase(it->second.guid);
            records_.erase(it);
            erased = true;
            break;
        }
        if (!erased) break;
    }
    mounts_ = std::move(mounts);
}

const AssetMount* AssetDatabase::mountOf(std::string_view projectPath) const {
    if (mounts_.empty()) return nullptr;
    auto [head, rest] = firstComponent(projectPath);
    for (const auto& m : mounts_) {
        if (head == m.name) return &m;
    }
    return nullptr;
}

std::string AssetDatabase::absolute(std::string_view projectPath) const {
    fs::path p(projectPath);
    if (p.is_absolute()) return p.string();
    if (const AssetMount* m = mountOf(projectPath)) {
        return (fs::path(m->root) / std::string(firstComponent(projectPath).second)).lexically_normal().string();
    }
    return (fs::path(root_) / p).lexically_normal().string();
}

std::string AssetDatabase::relative(std::string_view path) const {
    // Lexical first: this runs for every material / prefab lookup of a frame, and the
    // canonicalizing fs::relative costs several file-system calls. Symlinked spellings of the
    // root (e.g. /tmp vs /private/tmp) fall back to it.
    fs::path abs = fs::path(path).is_absolute() ? fs::path(path) : fs::path(absolute(path));
    for (const auto& m : mounts_) {
        if (std::string s = relativeTo(abs, m.root); !s.empty()) return m.name + "/" + s;
    }
    return relativeTo(abs, root_);
}

std::string AssetDatabase::newGuid() {
    static std::mt19937_64 rng{std::random_device{}() ^ static_cast<uint64_t>(
                                   std::chrono::steady_clock::now().time_since_epoch().count())};
    char buf[33];
    uint64_t a = rng(), b = rng();
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

namespace {

/// 128-bit FNV-1a style id from a project path (two independent 64-bit lanes).
std::string pathGuid(std::string_view path) {
    uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (unsigned char c : path) {
        a = (a ^ c) * 0x100000001b3ull;
        b = (b ^ (c + 0x9eu)) * 0x100000001b3ull;
    }
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    return buf;
}

bool skippedDir(const fs::path& p) {
    std::string name = p.filename().string();
    if (name == "playtests" && p.parent_path().filename() == "studio") return true;  // Studio reports, not assets
    return name.empty() || name[0] == '.' || name == "build" || name == "node_modules" || name == "DerivedData";
}

int64_t mtimeOf(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

Result<Json> readJsonFile(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return Error::make("io_error", "cannot read " + p.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return Json::parse(ss.str());
}

}  // namespace

Status AssetDatabase::writeMeta(const AssetRecord& r) {
    Json meta = Json::object({{"guid", r.guid}, {"type", toString(r.type)}});
    Json tags = Json::array();
    for (const auto& t : r.tags) tags.push(t);
    meta["tags"] = tags;
    meta["description"] = r.description;
    meta["source"] = r.source;
    meta["import"] = r.importSettings;
    std::ofstream f(absolute(r.path) + ".meta");
    if (!f) return Error::make("io_error", "cannot write meta for " + r.path);
    f << meta.dump(2) << "\n";
    if (!f) return Error::make("io_error", "cannot write meta for " + r.path);
    if (auto it = records_.find(r.path); it != records_.end()) it->second.persisted = true;
    return {};
}

std::vector<std::string> AssetDatabase::refresh() {
    std::vector<std::string> changed;
    std::unordered_map<std::string, bool> present;
    std::error_code ec;
    if (!fs::exists(root_, ec)) return changed;
    scanFolder(root_, present, changed, true);
    for (const auto& m : mounts_) scanFolder(m.root, present, changed, false);
    for (auto rit = records_.begin(); rit != records_.end();) {
        if (!present.count(rit->first)) {
            byGuid_.erase(rit->second.guid);
            rit = records_.erase(rit);
        } else {
            ++rit;
        }
    }
    return changed;
}

void AssetDatabase::scanFolder(const std::string& folder, std::unordered_map<std::string, bool>& present,
                               std::vector<std::string>& changed, bool skipMountNames) {
    std::error_code ec;
    if (!fs::exists(folder, ec)) return;
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
    size_t visited = 0;
    for (; it != end && visited < 50000; it.increment(ec)) {
        if (ec) break;
        ++visited;
        const fs::path& p = it->path();
        if (it->is_directory(ec)) {
            // A project folder named like a mount is shadowed by the mount.
            const bool shadowed = skipMountNames && it.depth() == 0 && mountOf(p.filename().string() + "/x");
            if (skippedDir(p) || shadowed) it.disable_recursion_pending();
            continue;
        }
        AssetType type = assetTypeForPath(p.filename().string());
        if (type == AssetType::Unknown) continue;
        std::string rel = relative(p.string());
        if (rel.empty()) continue;
        present[rel] = true;
        int64_t mtime = mtimeOf(p);
        auto existing = records_.find(rel);
        if (existing != records_.end() && existing->second.mtime == mtime) continue;

        AssetRecord r = existing != records_.end() ? existing->second : AssetRecord{};
        r.path = rel;
        r.type = type;
        r.mtime = mtime;
        r.size = static_cast<uint64_t>(fs::file_size(p, ec));
        if (existing == records_.end()) {
            // Load the sidecar if there is one. Without one, the GUID is derived from the path
            // (stable across sessions) and the sidecar is written only when the asset is
            // registered or annotated — browsing a folder never litters it with files.
            fs::path metaPath = p;
            metaPath += ".meta";
            if (auto meta = readJsonFile(metaPath); meta.ok() && meta->isObject()) {
                r.guid = meta->get("guid").asString();
                for (const auto& t : meta->get("tags").elements()) r.tags.push_back(t.asString());
                r.description = meta->get("description").asString();
                if (meta->get("source").isObject()) r.source = meta->get("source");
                if (meta->get("import").isObject()) r.importSettings = meta->get("import");
                r.persisted = !r.guid.empty();
            }
            if (r.guid.empty()) r.guid = pathGuid(rel);
            if (byGuid_.count(r.guid)) {  // a copied file (or its copied .meta): give it its own id
                r.guid = newGuid();
                r.persisted = false;
            }
        }
        byGuid_[r.guid] = rel;
        records_[rel] = std::move(r);
        changed.push_back(rel);
    }
}

const AssetRecord* AssetDatabase::find(std::string_view ref) const {
    return const_cast<AssetDatabase*>(this)->findMutable(ref);
}

AssetRecord* AssetDatabase::findMutable(std::string_view ref) {
    std::string_view r = ref;
    if (str::startsWith(r, "asset:")) r.remove_prefix(6);
    if (str::startsWith(r, "prefab:")) r.remove_prefix(7);
    if (str::startsWith(r, "guid:")) r.remove_prefix(5);
    if (size_t hash = r.find('#'); hash != std::string_view::npos) r = r.substr(0, hash);  // "model.gltf#2": part of a model
    if (auto g = byGuid_.find(std::string(r)); g != byGuid_.end()) return &records_[g->second];
    std::string rel = relative(std::string(r));
    if (auto it = records_.find(rel.empty() ? std::string(r) : rel); it != records_.end()) return &it->second;
    return nullptr;
}

std::vector<const AssetRecord*> AssetDatabase::query(const Query& q) const {
    std::vector<const AssetRecord*> out;
    for (const auto& [path, r] : records_) {
        if (q.type != AssetType::Unknown && r.type != q.type) continue;
        if (!q.tag.empty() && std::find(r.tags.begin(), r.tags.end(), q.tag) == r.tags.end()) continue;
        if (!q.text.empty()) {
            bool glob = q.text.find_first_of("*?") != std::string::npos;
            bool hit = glob ? str::globMatch(q.text, path)
                            : (str::lower(path).find(str::lower(q.text)) != std::string::npos ||
                               str::lower(r.description).find(str::lower(q.text)) != std::string::npos);
            for (const auto& t : r.tags) hit = hit || str::lower(t) == str::lower(q.text);
            if (!hit) continue;
        }
        out.push_back(&r);
    }
    std::sort(out.begin(), out.end(), [](const AssetRecord* a, const AssetRecord* b) { return a->path < b->path; });
    if (out.size() > q.limit) out.resize(q.limit);
    return out;
}

Result<const AssetRecord*> AssetDatabase::registerFile(std::string_view path) {
    std::string rel = relative(path);
    if (rel.empty()) return Error::make("invalid_path", "path is outside the project: " + std::string(path));
    std::error_code ec;
    if (!fs::exists(absolute(rel), ec)) return Error::make("not_found", "no such file: " + rel);
    if (assetTypeForPath(rel) == AssetType::Unknown) {
        return Error::make("unsupported", "unsupported asset type: " + rel,
                           "supported: .obj .glb .gltf .ply .stl .png .jpg .hdr .mat.json .prefab.json .sky.json .wav .mp3 .ogg .wander");
    }
    refresh();
    AssetRecord* r = findMutable(rel);
    if (!r) return Error::make("internal", "failed to register " + rel);
    if (!r->persisted) {
        if (Status s = writeMeta(*r); !s) return s.error();
    }
    return static_cast<const AssetRecord*>(r);
}

Status AssetDatabase::updateMeta(std::string_view ref, const Json& patch) {
    AssetRecord* r = findMutable(ref);
    if (!r) return Error::make("not_found", "no asset " + std::string(ref), "use asset_list to find assets");
    if (const Json* t = patch.find("tags"); t && t->isArray()) {
        r->tags.clear();
        for (const auto& v : t->elements()) r->tags.push_back(v.asString());
    }
    if (const Json* d = patch.find("description"); d && d->isString()) r->description = d->asString();
    if (const Json* s = patch.find("source"); s && s->isObject()) r->source.mergePatch(*s);
    if (const Json* i = patch.find("import"); i && i->isObject()) r->importSettings.mergePatch(*i);
    return writeMeta(*r);
}

Status AssetDatabase::move(std::string_view ref, std::string_view newPath) {
    AssetRecord* r = findMutable(ref);
    if (!r) return Error::make("not_found", "no asset " + std::string(ref));
    std::string to = relative(newPath);
    if (to.empty()) return Error::make("invalid_path", "destination is outside the project");
    if (assetTypeForPath(to) != r->type) {
        return Error::make("invalid_path", "the new name must keep the asset type's extension");
    }
    std::error_code ec;
    if (fs::exists(absolute(to), ec)) return Error::make("exists", "destination already exists: " + to);
    fs::create_directories(fs::path(absolute(to)).parent_path(), ec);
    fs::rename(absolute(r->path), absolute(to), ec);
    if (ec) return Error::make("io_error", "move failed: " + ec.message());
    fs::rename(absolute(r->path) + ".meta", absolute(to) + ".meta", ec);
    AssetRecord moved = *r;
    records_.erase(r->path);
    moved.path = to;
    byGuid_[moved.guid] = to;
    records_[to] = std::move(moved);
    return writeMeta(records_[to]);  // the GUID follows the file even if it had no sidecar yet
}

}  // namespace sky
