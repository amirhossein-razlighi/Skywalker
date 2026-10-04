#pragma once
// Save games: the running game's state written to named slots and restored into a running game
// (docs/SAVE_GAMES.md). Scene files are for authoring and the editor's play snapshot is for the
// editor; this is what a game uses for checkpoints, autosaves and "Continue".
//
// What a save holds: every entity with a `persist` component (its whole state or chosen fields, its
// Wander vars, and its subtree when it was spawned at run time), tombstones for persisted scene
// entities destroyed before the save, the global game variables (game_var), the current scene,
// the exact Wander play state (clocks, random generator, state machines, timers, waiting handlers),
// the play time and a user metadata object (slot title, chapter, thumbnail...).
//
// Format: versioned JSON ("format": "skywalker.save", formatVersion) with the game's own schema
// version (game.json saves.version) and migrations (C++ migrators or a Wander `fn migrate(from,
// data)`), an FNV-1a integrity hash, optional gzip (zlib), atomic writes (temp file + rename).
// Slots live in <project>/.skywalker/saves in the editor and tests, and in the platform user-data
// folder in a shipped game (userSaveDir).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky {
class Engine;
}

namespace sky::game {

/// Engine save format version (the layout of the file). Games version their own data with
/// game.json `saves.version`.
constexpr int kSaveFormatVersion = 1;
constexpr const char* kSaveFormat = "skywalker.save";

/// game.json `saves`: {"version": 2, "maxSlots": 20, "compress": false, "migrate": "scripts/save_migrate.wander"}.
struct SaveSettings {
    int version = 1;      // the game's save schema version; older saves are migrated up to it
    int maxSlots = 20;    // named slots (autosave and quicksave do not count)
    bool compress = false;  // gzip the files (zlib)
    std::string migrate;  // project-relative Wander file with `fn migrate(from: number, data: map) -> map`

    static Result<SaveSettings> fromJson(const Json& j);
    Json toJson() const;
};

/// What a slot listing shows about one save file.
struct SaveInfo {
    std::string slot;
    std::string file;  // absolute path
    uint64_t bytes = 0;
    bool compressed = false;
    int formatVersion = 0;
    int version = 0;  // game schema version it was written with
    double playTime = 0;
    std::string scene;
    std::string savedAt;  // UTC, ISO 8601 (metadata only: never read by the simulation)
    uint64_t tick = 0;
    Json meta = Json::object();
    std::string error;  // unreadable / corrupted file: why (the other fields are empty)
    Json toJson() const;
};

// --- Format -----------------------------------------------------------------------------------

/// "fnv1a64:<16 hex digits>" over the canonical text of `doc` without its "hash" member.
std::string saveHash(const Json& doc);
/// Bytes of a file (gunzipped when it starts with the gzip magic) -> parsed, integrity-checked document.
/// Errors: save_corrupted (unreadable, truncated, hash mismatch), invalid_save (not a save), save_too_new.
Result<Json> parseSave(const std::string& bytes);
/// Slot names: 1-64 of [a-z0-9_-] ("slot1", "autosave", "quicksave", "chapter-2").
Status validateSlotName(const std::string& slot);
bool isReservedSlot(const std::string& slot);  // autosave, quicksave

/// The folder a shipped game keeps its saves in: ~/Library/Application Support/<gameId>/saves on macOS,
/// $XDG_DATA_HOME/<gameId>/saves (default ~/.local/share/<gameId>/saves) elsewhere.
std::string userSaveDir(const std::string& gameId);

// --- Slot store: one folder of <slot>.save.json (or .save.json.gz) files --------------------------

class SaveStore {
public:
    explicit SaveStore(std::string dir) : dir_(std::move(dir)) {}
    const std::string& dir() const { return dir_; }
    /// Atomic: written to a temp file in the same folder, flushed, then renamed over the old slot.
    Status write(const std::string& slot, const Json& sealedDoc, bool compress) const;
    Result<Json> read(const std::string& slot) const;
    bool exists(const std::string& slot) const;
    Status remove(const std::string& slot) const;
    /// Every save in the folder, sorted by slot name (corrupted files are listed with `error`).
    std::vector<SaveInfo> list() const;
    /// The existing file of a slot (compressed or not), or "" when there is none.
    std::string fileOf(const std::string& slot) const;

private:
    std::string dir_;
};

// --- Migrations -------------------------------------------------------------------------------

/// Upgrades a save document from version `from` to `from + 1` in place (change "entities",
/// "globals", "meta"...). Return an error to refuse the save.
using SaveMigrator = std::function<Status(Json& doc)>;

// --- The engine's save system -----------------------------------------------------------------

class SaveSystem {
public:
    explicit SaveSystem(Engine& engine);
    ~SaveSystem();
    SaveSystem(const SaveSystem&) = delete;
    SaveSystem& operator=(const SaveSystem&) = delete;

    /// Where slots live (default <project>/.skywalker/saves; the standalone player sets userSaveDir()).
    void setDirectory(std::string dir);
    std::string directory() const;
    /// game.json `saves` (re-read when the file changes); defaults when absent or invalid.
    SaveSettings settings() const;
    /// Registers a C++ migration from `fromVersion` to `fromVersion + 1` (runs before the Wander one).
    void addMigrator(int fromVersion, SaveMigrator migrator);

    // Play lifecycle (Engine::play / stop / step).
    void beginPlay();
    void endPlay();
    /// End of a tick: runs saves and loads that scripts requested during it, then emits `saved` / `loaded`.
    void endTick();

    /// Global game variables (Wander game_var): kept per play session, saved with every slot. Exact values.
    const Json& globalsJson() const;
    Status setGlobal(const std::string& name, const Json& taggedValue);
    /// Seconds of play in this save line: the loaded save's play time plus real time since.
    double playTime() const;
    /// The scene flow swapped scenes: the new scene's persisted entities become the base tombstones refer to.
    void sceneChanged();

    struct Outcome {
        SaveInfo info;
        std::vector<std::string> warnings;
        int migratedFrom = 0;    // load: the version the save was written with (when it was migrated)
        size_t entities = 0;     // persisted entities written / restored
        size_t spawned = 0;      // load: entities recreated
        size_t destroyed = 0;    // load: entities removed (tombstones, spawned after the save)
        std::string sceneChanged;  // load: the scene the save switched to ("" = same scene)
        Json toJson() const;
    };
    /// Writes a slot now (between ticks, while playing). `meta` is the user object (title, chapter, thumbnail).
    Result<Outcome> save(const std::string& slot, const Json& meta, const std::string& actor = "game");
    /// Restores a slot into the running game now (between ticks, while playing).
    Result<Outcome> load(const std::string& slot, const std::string& actor = "game");
    /// Compares a slot with the running game: per entity, the fields that would change on load.
    Result<Json> inspect(const std::string& slot, size_t maxDiffs = 200);
    Status remove(const std::string& slot);
    std::vector<SaveInfo> list() const;
    bool has(const std::string& slot) const;

    /// From scripts: run at the end of the current tick (a save mid-tick would see half a tick).
    void requestSave(const std::string& slot, const Json& meta);
    void requestLoad(const std::string& slot);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sky::game
