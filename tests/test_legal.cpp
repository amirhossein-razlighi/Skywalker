// Terms of Use, Privacy Notice and acceptance (docs/legal/): embedded documents and their versions, the local
// acceptance record, the legal_info tool, `skywalker legal`, and the MCP resources for docs in subfolders.

#include <doctest/doctest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "skywalker/agent/McpServer.h"
#include "skywalker/dcc/Process.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/legal/Legal.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

/// A private SKY_LEGAL_DIR for one test, so nothing touches the real acceptance record.
struct LegalDir {
    LegalDir() {
        static int counter = 0;
        dir = fs::temp_directory_path() / ("sky_legal_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
        fs::remove_all(dir);
        fs::create_directories(dir);
        ::setenv("SKY_LEGAL_DIR", dir.c_str(), 1);
    }
    ~LegalDir() {
        ::unsetenv("SKY_LEGAL_DIR");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path record() const { return dir / legal::kRecordFileName; }
    fs::path dir;
};

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

}  // namespace

TEST_CASE("legal: the documents are embedded with versions, a plain-language summary and marked placeholders") {
    auto terms = legal::document("terms");
    auto privacy = legal::document("privacy");
    REQUIRE(terms);
    REQUIRE(privacy);
    CHECK(terms->title == "Skywalker Terms of Use");
    CHECK(privacy->title == "Skywalker Privacy Notice");
    CHECK_FALSE(terms->version.empty());
    CHECK_FALSE(privacy->version.empty());
    CHECK(terms->resource == "skywalker://docs/legal/TERMS");
    CHECK(terms->text.find("Template prepared for legal review") != std::string::npos);
    CHECK(privacy->text.find("Template prepared for legal review") != std::string::npos);
    CHECK_FALSE(legal::plainSummary(terms->text).empty());
    CHECK_FALSE(legal::plainSummary(privacy->text).empty());
    auto ph = legal::placeholders(terms->text);
    CHECK(std::find(ph.begin(), ph.end(), "{{LICENSOR_LEGAL_NAME}}") != ph.end());
    CHECK(std::find(ph.begin(), ph.end(), "{{GOVERNING_LAW_COUNTRY}}") != ph.end());
    CHECK(std::find(ph.begin(), ph.end(), "{{EFFECTIVE_DATE}}") != ph.end());

    auto license = legal::document("license");
    REQUIRE(license);
    CHECK(license->text.find("Business Source License 1.1") != std::string::npos);
    CHECK(license->version == "BUSL-1.1");
    auto licensing = legal::document("licensing");
    REQUIRE(licensing);
    CHECK(licensing->text.find("## Third-party") != std::string::npos);

    auto typo = legal::document("term");
    REQUIRE_FALSE(typo);
    CHECK(typo.error().code == "unknown_document");
    CHECK(typo.error().hint.find("terms") != std::string::npos);
}

TEST_CASE("legal: header fields, summaries and placeholders are parsed from markdown") {
    const std::string text =
        "# Title\n\nVersion: 2.1 \nEffective date: {{EFFECTIVE_DATE}}\n\n## In plain language\n\n- one\n- two\n\n## 1. Next\n"
        "{{A_B}} {{A_B}} {{lower}} {{}} {{C1}}\n";
    CHECK(legal::headerField(text, "Version") == "2.1");
    CHECK(legal::headerField(text, "Effective date") == "{{EFFECTIVE_DATE}}");
    CHECK(legal::headerField(text, "Missing").empty());
    CHECK(legal::plainSummary(text) == "- one\n- two");
    CHECK(legal::plainSummary("# No summary\n").empty());
    auto ph = legal::placeholders(text);
    REQUIRE(ph.size() == 3);
    CHECK(ph[0] == "{{EFFECTIVE_DATE}}");
    CHECK(ph[1] == "{{A_B}}");
    CHECK(ph[2] == "{{C1}}");
}

TEST_CASE("legal: acceptance is recorded locally with a history and detected as outdated after a version change") {
    LegalDir d;
    CHECK(legal::defaultRecordPath() == d.record());
    auto empty = legal::loadRecord(d.record());
    REQUIRE(empty);
    CHECK_FALSE(empty->latest.has_value());

    auto first = legal::recordAcceptance(d.record(), "editor", "2026-01-01T00:00:00Z");
    REQUIRE(first);
    CHECK(first->via == "editor");
    CHECK(legal::isCurrent(*first));
    auto second = legal::recordAcceptance(d.record(), "cli");
    REQUIRE(second);
    auto record = legal::loadRecord(d.record());
    REQUIRE(record);
    REQUIRE(record->latest.has_value());
    CHECK(record->latest->via == "cli");
    CHECK(record->history.size() == 2);
    CHECK(record->history.front().acceptedAt == "2026-01-01T00:00:00Z");

    // A record for older versions (the shape the editor writes) is outdated.
    writeText(d.record(), R"({"schema":1,"terms_version":"0.9","privacy_version":"0.9","accepted_at":"2025-01-01T00:00:00Z","via":"editor",
                             "app_version":"0.0.1","history":[]})");
    auto old = legal::loadRecord(d.record());
    REQUIRE(old);
    REQUIRE(old->latest.has_value());
    CHECK_FALSE(legal::isCurrent(*old->latest));

    // A corrupt record is reported, and accepting again rewrites it.
    writeText(d.record(), "{not json");
    auto corrupt = legal::loadRecord(d.record());
    REQUIRE_FALSE(corrupt);
    CHECK(corrupt.error().code == "corrupt_record");
    REQUIRE(legal::recordAcceptance(d.record(), "cli"));
    CHECK(legal::loadRecord(d.record()));
}

TEST_CASE("legal: the history is capped") {
    LegalDir d;
    for (size_t i = 0; i < legal::kHistoryLimit + 5; ++i) REQUIRE(legal::recordAcceptance(d.record(), "cli", "2026-01-01T00:00:00Z"));
    auto record = legal::loadRecord(d.record());
    REQUIRE(record);
    CHECK(record->history.size() == legal::kHistoryLimit);
}

TEST_CASE("legal_info: versions, acceptance state, data practices and document text") {
    LegalDir d;
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);

    ToolResult r = engine.callTool("legal_info", Json::object(), "test");
    REQUIRE_FALSE(r.isError);
    const Json& s = r.structured;
    CHECK(s.get("documents").get("terms").get("version").asString() == legal::document("terms")->version);
    CHECK(s.get("documents").get("privacy").get("resource").asString() == "skywalker://docs/legal/PRIVACY");
    CHECK(s.get("documents").get("license").get("version").asString() == "BUSL-1.1");
    CHECK_FALSE(s.get("documents").get("terms").contains("text"));
    CHECK(s.get("acceptance").get("status").asString() == "not_recorded");
    CHECK(s.get("data").get("telemetry").asBool(true) == false);
    CHECK(s.get("data").get("outbound").size() == 2);
    CHECK(s.get("placeholders").size() > 0);
    CHECK(s.get("warnings").size() > 0);  // the templates still have placeholders

    REQUIRE(legal::recordAcceptance(d.record(), "cli"));
    Json withText = engine.callTool("legal_info", Json::object({{"document", "privacy"}}), "test").structured;
    CHECK(withText.get("acceptance").get("status").asString() == "current");
    CHECK(withText.get("acceptance").get("latest").get("via").asString() == "cli");
    CHECK(withText.get("documents").get("privacy").get("text").asString().find("For developers who ship games") != std::string::npos);

    ToolResult bad = engine.callTool("legal_info", Json::object({{"document", "privacy_notice"}}), "test");
    CHECK(bad.isError);

    const ToolDef* def = engine.tools().find("legal_info");
    REQUIRE(def != nullptr);
    CHECK_FALSE(def->mutates);
    CHECK_FALSE(def->openWorld);
}

TEST_CASE("skywalker legal: prints the documents, records acceptance, never blocks") {
    LegalDir d;
    std::string out, err;
    CHECK(legal::runCommand({"--status"}, d.record(), out, err) == 0);
    CHECK(out.find("not recorded") != std::string::npos);
    CHECK(out.find(legal::kDeveloperCredit) != std::string::npos);

    out.clear();
    CHECK(legal::runCommand({}, d.record(), out, err) == 0);
    CHECK(out.find("# Skywalker Terms of Use") != std::string::npos);
    CHECK(out.find("# Skywalker Privacy Notice") != std::string::npos);
    CHECK(out.find("# Licensing") != std::string::npos);

    out.clear();
    CHECK(legal::runCommand({"--accept"}, d.record(), out, err) == 0);
    CHECK(out.find("Recorded") != std::string::npos);
    CHECK(fs::exists(d.record()));

    out.clear();
    CHECK(legal::runCommand({"--json"}, d.record(), out, err) == 0);
    auto j = Json::parse(out);
    REQUIRE(j);
    CHECK(j->get("acceptance").get("status").asString() == "current");

    out.clear();
    CHECK(legal::runCommand({"license"}, d.record(), out, err) == 0);
    CHECK(out.find("Additional Use Grant") != std::string::npos);

    err.clear();
    CHECK(legal::runCommand({"--acept"}, d.record(), out, err) == 2);
    CHECK(err.find("did you mean --accept") != std::string::npos);
    err.clear();
    CHECK(legal::runCommand({"privcy"}, d.record(), out, err) == 2);
    CHECK(err.find("privacy") != std::string::npos);
}

#ifdef SKY_CLI_PATH
TEST_CASE("skywalker legal: the CLI binary") {
    LegalDir d;
    dcc::ProcessSpec spec;
    spec.executable = SKY_CLI_PATH;
    spec.args = {"legal", "--accept"};
    spec.env = {{"SKY_LEGAL_DIR", d.dir.string()}};
    spec.timeout = std::chrono::seconds(60);
    auto r = dcc::runProcess(spec);
    REQUIRE(r.spawned);
    CHECK(r.exitCode == 0);
    CHECK(fs::exists(d.record()));

    spec.args = {"legal", "--status"};
    r = dcc::runProcess(spec);
    CHECK(r.exitCode == 0);
    CHECK(r.out.find("(current)") != std::string::npos);

    spec.args = {"legal", "--bogus"};
    r = dcc::runProcess(spec);
    CHECK(r.exitCode == 2);
}
#endif

TEST_CASE("mcp: docs in subfolders are listed and readable as resources") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine engine(cfg);
    McpSession session(engine.tools(), [&](const std::string& t, const Json& a, const std::string& actor) {
        return engine.callTool(t, a, actor).toMcp();
    });
    auto rpc = [&](const std::string& method, const Json& params) {
        auto r = session.handle(Json::object({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}).dump());
        REQUIRE(r.has_value());
        return Json::parse(*r).value();
    };
    (void)rpc("initialize", Json::object({{"protocolVersion", "2025-06-18"}, {"clientInfo", Json::object({{"name", "test"}})}}));
    Json list = rpc("resources/list", Json::object()).get("result").get("resources");
    bool terms = false, studio = false;
    for (const auto& r : list.elements()) {
        terms = terms || r.get("uri").asString() == "skywalker://docs/legal/TERMS";
        studio = studio || r.get("uri").asString() == "skywalker://docs/STUDIO";
    }
    CHECK(terms);
    CHECK(studio);
    Json doc = rpc("resources/read", Json::object({{"uri", "skywalker://docs/legal/PRIVACY"}}));
    CHECK(doc.get("result").get("contents")[0].get("text").asString().find("# Skywalker Privacy Notice") == 0);
    CHECK(rpc("resources/read", Json::object({{"uri", "skywalker://docs/legal/../legal/TERMS"}})).contains("error"));
    CHECK(rpc("resources/read", Json::object({{"uri", "skywalker://docs/"}})).contains("error"));
}
