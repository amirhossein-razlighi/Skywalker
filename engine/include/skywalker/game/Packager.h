#pragma once
// Shipping a game: which project files a build needs, the macOS app bundle layout, and the build itself
// (`skywalker build`, the `game_build` tool). See docs/SHIPPING.md.
//
//   Name.app/Contents/
//     Info.plist
//     MacOS/<Executable>         the standalone player (skywalker-player)
//     Resources/AppIcon.icns     from game.json "icon" (converted with sips + iconutil)
//     Resources/build.json       manifest: identity, start scene, every shipped file with its size
//     Resources/Game/            the project: game.json, scenes/, assets, scripts, input.json, audio.json, ...
//     Frameworks/<module>.dylib  the project's native C++ module, compiled at build time (if it has one)

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/game/GameSettings.h"

namespace sky {
class Engine;
}

namespace sky::game {

// --- What gets shipped -------------------------------------------------------------------------------

struct CollectOptions {
    bool allAssets = false;  // ship every runtime file in the project, not only what scenes and scripts reference
    std::string startScene;  // project-relative; "" resolves like the player does
};

struct CollectedFiles {
    std::set<std::string> files;                  // project-relative, '/' separators (includes .meta sidecars)
    std::map<std::string, std::string> missing;   // reference -> first file that mentions it: looks like an asset, does not exist
    std::vector<std::string> excludedReferenced;  // referenced files that game.json "exclude" keeps out (they will be missing at run time)
    std::string startScene;
    uint64_t bytes = 0;
    size_t referenced = 0;  // files reached through references (not roots)
    Json toJson() const;
};

/// Computes the set of project files a build ships: the roots (game.json, input.json, audio.json, scenes,
/// Wander scripts, credits, game.json "include") plus everything they reference, transitively: scene, prefab,
/// material, animation and `.meta` JSON strings, Wander string literals ("audio/hit.wav"), glTF/OBJ/MTL side files.
/// Editor-only folders (studio/, agents/, native sources, hidden folders, build outputs) are never shipped.
Result<CollectedFiles> collectGameFiles(const std::string& projectDir, const GameSettings& settings, const CollectOptions& options = {});

// --- Bundles -----------------------------------------------------------------------------------------

struct GameLocation {
    std::string projectDir;  // folder to run (Name.app/Contents/Resources/Game for a bundle)
    std::string appPath;     // the .app, when bundled
    bool bundled = false;
    Json manifest;           // build.json of a bundle (null otherwise)
};
/// `arg` is a project folder or a built `Name.app`.
Result<GameLocation> locateGame(const std::string& arg);
/// The game bundled around `executable` (Name.app/Contents/MacOS/Name): empty projectDir when it is not in a bundle.
GameLocation locateBundledGame(const std::string& executable);

struct BundleCheck {
    bool ok = true;
    std::vector<std::string> problems;
    size_t files = 0;
    uint64_t bytes = 0;
    Json toJson() const;
};
/// Checks a bundle's manifest against the files on disk (everything present, sizes match, executable, Info.plist).
BundleCheck verifyBundle(const GameLocation& game);

/// The player executable to put into bundles: $SKYWALKER_PLAYER, `skywalker-player` next to the running executable,
/// in ../bin (the build tree), or the path the build was configured with. Empty if none exists.
std::string findPlayerBinary();

// --- Building ----------------------------------------------------------------------------------------

struct PackageOptions {
    std::string projectDir;
    std::string outDir;
    std::string name;      // app name (default: game.json title)
    std::string icon;      // PNG (absolute, or project-relative); overrides game.json "icon"
    std::string player;    // player executable (default: findPlayerBinary())
    std::string version;   // overrides game.json
    std::string bundleId;  // overrides game.json
    std::string scene;     // start scene override
    bool release = false;  // strip the player's symbols, tag the manifest
    bool allAssets = false;
    bool sign = true;          // ad-hoc codesign
    bool buildNative = true;   // compile native/*.cpp and ship the library
    Engine* host = nullptr;    // a running engine on the same project reuses its native module build
};

struct PackageReport {
    std::string app;  // path of the bundle
    std::string name, bundleId, version, startScene;
    uint64_t appBytes = 0;
    size_t files = 0;
    bool signedOk = false;
    std::string signature;  // "ad-hoc" | "none"
    bool release = false;
    double seconds = 0;
    std::vector<std::string> warnings;
    Json contents;  // by type, largest files
    Json toJson() const;
};

/// Builds `<outDir>/<Name>.app`. Atomic: the old bundle is replaced only when the new one is complete.
Result<PackageReport> buildGame(const PackageOptions& options);

}  // namespace sky::game
