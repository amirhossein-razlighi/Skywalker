// The documentation site (website/): its generated reference must match the engine, and every sample on it must be
// real. Like the skills check in test_integrations.cpp, this fails when a tool, component, Wander builtin or CLI flag
// changes without regenerating the site:
//   python3 website/scripts/dump_data.py --cli build/<preset>/bin/skywalker && python3 website/scripts/gen_reference.py

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef SKY_CLI_PATH

namespace fs = std::filesystem;

namespace {

bool pythonAvailable() { return std::system("python3 --version > /dev/null 2>&1") == 0; }

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

TEST_CASE("website: the generated reference matches the engine and every sample compiles") {
    if (!pythonAvailable()) {
        MESSAGE("python3 not available: skipping");
        return;
    }
    const std::string root = SKY_SOURCE_DIR;
    const fs::path fresh =
        fs::temp_directory_path() / ("sky_site_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string dump = "python3 " + quoted(root + "/website/scripts/dump_data.py") + " --cli " + quoted(SKY_CLI_PATH) +
                             " --out " + quoted(fresh.string()) + " > /dev/null";
    REQUIRE_MESSAGE(std::system(dump.c_str()) == 0, "website/scripts/dump_data.py failed");
    const std::string check = "python3 " + quoted(root + "/website/scripts/gen_reference.py") + " --check --fresh " +
                              quoted(fresh.string()) + " > /dev/null";
    CHECK_MESSAGE(std::system(check.c_str()) == 0,
                  "the site's reference is stale: run `python3 website/scripts/dump_data.py --cli build/<preset>/bin/skywalker"
                  " && python3 website/scripts/gen_reference.py` and commit website/data and website/docs/reference");
    std::error_code ec;
    fs::remove_all(fresh, ec);

    const std::string snippets = "python3 " + quoted(root + "/website/scripts/check_snippets.py") + " --cli " +
                                 quoted(SKY_CLI_PATH) + " > /dev/null";
    CHECK_MESSAGE(std::system(snippets.c_str()) == 0,
                  "a sample on the documentation site no longer matches the engine: run `python3 "
                  "website/scripts/check_snippets.py --cli build/<preset>/bin/skywalker` to see the problems");
}
#endif
