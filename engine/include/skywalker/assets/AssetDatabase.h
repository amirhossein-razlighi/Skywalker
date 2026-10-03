#pragma once
// Project asset database.
//
// Every file in the project that the engine understands becomes an *asset* with:
//   * a stable GUID stored in a `<file>.meta` JSON sidecar (survives renames/moves),
//   * a type (mesh, texture, material, prefab, scene, audio, ...),
//   * agent-friendly metadata: tags, a description, and provenance ("source") — e.g.
//     which generator/agent produced it and from what prompt.
//
// Scenes reference assets by project-relative path ("asset:meshes/tree.glb",
// "materials/stone.mat.json") because paths are readable for people and models alike;
// `move()` rewrites those references so renames are safe. GUIDs give tools a rename-proof
// handle ("guid:3f2a...").

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {

enum class AssetType { Unknown, Mesh, Texture, Material, Prefab, Scene, Audio, Video, Script, Agent,
                       Animation, Controller, Sequence /* animation: *.anim, *.animctl.json, *.sequence.json */ };

const char* toString(AssetType t);
AssetType assetTypeFromString(std::string_view s);
/// Classifies by file name (".obj", ".glb", ".png", ".mat.json", ".prefab.json", ...).
AssetType assetTypeForPath(std::string_view path);

struct AssetRecord {
    std::string guid;
    std::string path;  // project-relative, '/' separators
    AssetType type = AssetType::Unknown;
    std::vector<std::string> tags;
    std::string description;
    Json source = Json::object();          // provenance: {"generator": "...", "prompt": "...", "by": "agent:..."}
    Json importSettings = Json::object();  // importer options / derived data (e.g. created material)
    uint64_t size = 0;
    int64_t mtime = 0;  // file-clock nanoseconds (change detection only)
    /// Whether a .meta sidecar exists. Scanning never writes files: unregistered assets get a
    /// path-derived GUID until they are registered, annotated or moved.
    bool persisted = false;

    Json toJson() const;
};

class AssetDatabase {
public:
    explicit AssetDatabase(std::string root);

    const std::string& root() const { return root_; }
    std::string absolute(std::string_view projectPath) const;
    /// Project-relative path for an absolute or relative path ("" if outside the project).
    std::string relative(std::string_view path) const;

    /// Rescans the project. Creates .meta files for new assets, forgets deleted ones.
    /// Returns the project paths of assets that were added or modified since the last scan.
    std::vector<std::string> refresh();

    /// Accepts "path", "asset:path", "guid:GUID" or a bare GUID.
    const AssetRecord* find(std::string_view ref) const;

    struct Query {
        AssetType type = AssetType::Unknown;  // Unknown = any
        std::string tag;
        std::string text;  // glob on path or substring of description
        size_t limit = 100;
    };
    std::vector<const AssetRecord*> query(const Query& q) const;
    size_t size() const { return records_.size(); }

    /// Ensures a file is registered (writes its .meta) and returns the record.
    Result<const AssetRecord*> registerFile(std::string_view path);
    /// Updates tags / description / source (only non-null fields of `patch`).
    Status updateMeta(std::string_view ref, const Json& patch);
    /// Moves/renames a file and its .meta. Callers rewrite scene references.
    Status move(std::string_view ref, std::string_view newPath);

    static std::string newGuid();

private:
    Status writeMeta(const AssetRecord& r);  // also marks the record persisted
    AssetRecord* findMutable(std::string_view ref);

    std::string root_;
    std::unordered_map<std::string, AssetRecord> records_;  // by path
    std::unordered_map<std::string, std::string> byGuid_;   // guid -> path
};

}  // namespace sky
