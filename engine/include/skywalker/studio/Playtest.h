#pragma once
// Playtests that actually play.
//
// A playtest copies the current scene into a sandbox engine (the editor scene is never
// touched), presses play, and drives the game for N seconds with a bot policy through the
// same `sim_input` tool any agent uses. It records what a human tester would notice:
// trajectory, deaths and fails, damage, stuck periods, objectives reached, time-to-goal,
// frame cost, screenshots at notable moments, and a top-down heatmap.
//
// Conventions the bot understands (documented in docs/STUDIO.md):
//   tags    player | goal / objective | hazard / enemy
//   events  death, died, killed, respawn  -> deaths
//           fail, lose, game_over         -> fails
//           damage, hurt, hit             -> damage
//           objective, goal, win, level_complete, victory -> objectives
//           *collected, pickup            -> pickups;  checkpoint -> checkpoints
//   input   WASD moves in world space (w = -Z); space jumps. Override with `controls`.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/Image.h"

namespace sky {
class Engine;
}

namespace sky::studio {

struct PlaytestPersona {
    float reactionTime = 0.25f;  // seconds between decisions
    float skill = 0.7f;          // 0..1: steering precision, hazard avoidance, jumping
    float curiosity = 0.3f;      // 0..1: detours toward unexplored space
    float patience = 15.f;       // seconds without progress before giving up (quit)
};

struct PlaytestControls {
    std::string up = "w", down = "s", left = "a", right = "d", jump = "space";
};

struct PlaytestConfig {
    std::string policy = "goal_seeker";  // scripted | random | explorer | goal_seeker
    double seconds = 30;
    int runs = 1;
    uint64_t seed = 1;
    PlaytestPersona persona;
    PlaytestControls controls;
    Json script = Json::array();  // scripted: [{t, press|hold|release|click|event|...sim_input args}]
    std::string player;           // entity id/name; default: the entity tagged "player"
    double goalRadius = 1.0;      // meters from a goal's bounds that count as reaching it
    bool screenshots = true;
    double shotInterval = 0;      // seconds between periodic shots (0 = notable moments only)
    int shotWidth = 480, shotHeight = 270, maxShots = 8;
    int heatmapSize = 320;
    std::string label;

    static Result<PlaytestConfig> fromJson(const Json& j);
    Json toJson() const;
};

struct PlaytestResult {
    Json report;  // {config, metrics, perf, runs, findings, summary, screenshots, warnings}
    Image heatmap;
    std::vector<std::pair<std::string, Image>> shots;  // file name -> image
};

/// Runs a playtest against a sandbox copy of `source`'s current scene.
Result<PlaytestResult> runPlaytest(Engine& source, const PlaytestConfig& config);

/// Runs a playtest from tool arguments (playtest_run) and records it in the studio:
/// studio/playtests/<id>/{report.json, heatmap.png, shot_*.png}; updates the studio's
/// baseline metrics. Uses the persona of the acting roster member (or `agent`) when the
/// arguments don't set one. Returns the full report (with id).
Result<Json> runAndRecordPlaytest(Engine& engine, const Json& args, const std::string& actor);

/// Findings a QA lead would file from a report: [{category, severity, summary, details,
/// target, fingerprint, evidence}]. Called by runPlaytest; exposed for tests.
Json playtestFindings(const Json& report);

}  // namespace sky::studio
