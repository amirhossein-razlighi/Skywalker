// skywalker-player: runs a Skywalker game, from a project folder or from inside a built Name.app.
// See docs/SHIPPING.md.

#include <cstdio>

#include "Player.h"
#include "skywalker/core/Log.h"

using namespace sky;

int main(int argc, char** argv) {
    auto options = player::parseOptions(argc, argv);
    if (!options) {
        std::fprintf(stderr, "skywalker-player: %s\n  hint: %s\n", options.error().message.c_str(), options.error().hint.c_str());
        return 2;
    }
    if (options->help) {
        std::printf("%s", player::usageText());
        return 0;
    }
    if (options->version) {
        std::printf("skywalker-player %s\n", SKY_VERSION_STRING);
        return 0;
    }
    log::setMinLevel(options->verbose ? LogLevel::Info : LogLevel::Warn);
    if (options->check) return player::runCheck(*options, argv[0]);

    auto session = player::openSession(*options, argv[0], audio::AudioMode::Auto);
    if (!session) {
        const std::string detail = session.error().message + (session.error().hint.empty() ? "" : "\n\n" + session.error().hint);
        std::fprintf(stderr, "skywalker-player: %s\n", detail.c_str());
        if (!options->automated()) player::showStartupError("The game cannot start", detail);
        return 1;
    }
    return player::runWindowed(**session, *options);
}
