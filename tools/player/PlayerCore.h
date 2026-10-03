#pragma once
// skywalker-player, platform-independent part: command line, finding the game, opening a session (project +
// settings + scene + engine) and the headless `--check`. The window, input and display loop are in Player.mm.

#include <memory>
#include <optional>
#include <string>

#include "skywalker/core/Result.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/game/Packager.h"

namespace sky::player {

struct Options {
    std::string target;  // project folder or built .app ("" = the game bundled around this executable)
    std::string scene;   // start scene override (project-relative)
    std::optional<bool> fullscreen;
    std::optional<bool> vsync;
    std::optional<int> width, height;  // window content size in points
    std::string quality;               // low | medium | high | ultra
    float renderScale = 0.f;
    bool quitOnEscape = false;
    bool noAudio = false;
    bool verbose = false;
    bool check = false;          // headless validation, prints a JSON report
    int checkTicks = 120;
    std::string capture;         // write one rendered frame to this PNG, then quit
    int frames = 90;             // frames to run before the capture
    double quitAfter = 0;        // seconds; 0 = never
    std::string agentSocket;     // serve MCP on this Unix socket (development)
    bool help = false;
    bool version = false;

    /// Runs without a human: fixed time step, no error dialogs, no display link (captures, smoke tests, CI).
    bool automated() const { return !capture.empty() || quitAfter > 0; }
};

Result<Options> parseOptions(int argc, char** argv);
const char* usageText();

/// A game ready to play: where it lives, its settings and an engine with the start scene loaded (not yet playing).
struct Session {
    game::GameLocation location;
    game::GameSettings settings;
    std::string scene;
    std::unique_ptr<Engine> engine;
};

/// `executable` is argv[0] (used to find the game when the player runs from inside a built app).
Result<std::unique_ptr<Session>> openSession(const Options& options, const std::string& executable, audio::AudioMode audio);

/// `--check`: opens the game headlessly, verifies a bundle against its manifest, plays `checkTicks` ticks and renders a
/// test frame. Prints a JSON report to stdout. Returns the exit code (0 = the game works).
int runCheck(const Options& options, const std::string& executable);

}  // namespace sky::player
