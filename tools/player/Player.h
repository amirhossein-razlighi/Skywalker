#pragma once
// The windowed player (Player.mm; Objective-C++ / Cocoa).

#include <string>

#include "PlayerCore.h"

namespace sky::player {

/// Opens the window and runs the game until it quits. Returns the process exit code. Does not return when the
/// application terminates normally.
int runWindowed(Session& session, const Options& options);

/// A modal error dialog for failures before the game starts (a missing project, a broken scene).
void showStartupError(const std::string& title, const std::string& message);

}  // namespace sky::player
