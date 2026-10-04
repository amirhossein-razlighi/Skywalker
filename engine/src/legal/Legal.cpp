#include "skywalker/legal/Legal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "skywalker/agent/Resources.h"
#include "skywalker/core/Strings.h"

namespace sky::legal {

namespace fs = std::filesystem;

namespace {

constexpr const char* kLicenseText =
#include "skywalker_license.inc"
    ;

constexpr const char* kLicenseId = "BUSL-1.1";
constexpr const char* kLicenseName = "Business Source License 1.1";

std::string firstHeading(std::string_view text) {
    for (const auto& line : str::split(text, '\n')) {
        if (str::startsWith(line, "# ")) return str::trim(std::string_view(line).substr(2));
    }
    return {};
}

Document fromEmbedded(std::string id, const char* path, std::string resource) {
    Document d;
    d.id = std::move(id);
    d.source = path;
    d.resource = std::move(resource);
    if (const EmbeddedAsset* a = findEmbeddedAsset(path)) d.text = a->content;
    d.title = firstHeading(d.text);
    d.version = headerField(d.text, "Version");
    d.effectiveDate = headerField(d.text, "Effective date");
    return d;
}

std::string requireVersion(const Document& d) { return d.version.empty() ? "unknown" : d.version; }

Json acceptanceJson(const Acceptance& a) {
    return Json::object({{"terms_version", a.termsVersion},
                         {"privacy_version", a.privacyVersion},
                         {"accepted_at", a.acceptedAt},
                         {"via", a.via},
                         {"app_version", a.appVersion}});
}

Acceptance acceptanceFrom(const Json& j) {
    return Acceptance{j.get("terms_version").asString(), j.get("privacy_version").asString(), j.get("accepted_at").asString(),
                      j.get("via").asString(), j.get("app_version").asString()};
}

/// Where data goes, as verified in the source of this version (kept in step with docs/legal/PRIVACY.md).
Json dataPractices() {
    auto flow = [](const char* what, const char* when, const char* to, const char* contains) {
        return Json::object({{"what", what}, {"when", when}, {"to", to}, {"contains", contains}});
    };
    return Json::object(
        {{"telemetry", false},
         {"analytics", false},
         {"sent_to_licensor", "nothing: no telemetry, crash reports, update or license checks, accounts or ads"},
         {"outbound",
          Json::array({flow("AI provider requests", "an in-editor agent or `skywalker studio run` works",
                            "the provider endpoint the human configured (cloud or local model)",
                            "agent instructions, messages, tool calls and results (scene and file contents), viewport "
                            "screenshots if the model sees images, the API key, IP address and client identifier"),
                       flow("asset downloads", "`asset_download`, after the human approves", "the URL requested",
                            "IP address, client identifier Skywalker/<version>, the URL")})},
         {"local_only",
          Json::array({"projects, provider settings (Application Support/Skywalker/crew.json), API keys (macOS Keychain)",
                       "agent socket ~/.skywalker/editor.sock (owner-only Unix socket)",
                       "design-app bridge on 127.0.0.1", "game crash logs in ~/Library/Logs/SkywalkerGames",
                       "the acceptance record"})},
         {"games", "skywalker-player and packaged games collect and send nothing and show no terms; the developer is "
                   "the controller for anything a game adds (docs/legal/PRIVACY.md, last section)"}});
}

Status writeAtomically(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) return Error::make("io_error", "cannot write " + tmp.string(), "check that the folder is writable, or set SKY_LEGAL_DIR");
    }
    fs::rename(tmp, path, ec);
    if (ec) return Error::make("io_error", "cannot replace " + path.string() + ": " + ec.message());
    return {};
}

const char* kUsage =
    "usage: skywalker legal [terms|privacy|license|licensing] [--accept] [--status] [--json]\n\n"
    "Prints Skywalker's Terms of Use, Privacy Notice and license summary (the copies embedded in this binary).\n"
    "Nothing is sent anywhere; automation never waits for acceptance.\n\n"
    "  terms|privacy|license|licensing   print one document (license = full LICENSE text,\n"
    "                                    licensing = plain-language summary and third-party notices)\n"
    "  --accept    record that you accept the Terms of Use and acknowledge the Privacy Notice\n"
    "              (for headless and CI use; the editor asks on first launch)\n"
    "  --status    versions and acceptance state only\n"
    "  --json      machine-readable output (the legal_info tool payload)\n";

std::string statusText(const fs::path& recordPath) {
    std::ostringstream o;
    o << "Skywalker legal documents (embedded copies; nothing is sent anywhere)\n";
    for (const char* id : {"terms", "privacy", "licensing"}) {
        auto d = document(id);
        if (!d) continue;
        o << "  " << d->title;
        if (!d->version.empty()) o << "  version " << d->version;
        if (!d->effectiveDate.empty()) o << "  effective " << d->effectiveDate;
        if (!d->resource.empty()) o << "  (" << d->resource << ")";
        o << "\n";
    }
    auto record = loadRecord(recordPath);
    if (!record) {
        o << "Acceptance: unreadable record: " << record.error().message << "\n";
    } else if (!record->latest) {
        o << "Acceptance: not recorded on this computer. Run `skywalker legal --accept` to accept for headless use.\n";
    } else {
        const Acceptance& a = *record->latest;
        o << "Acceptance: Terms " << a.termsVersion << " accepted, Privacy Notice " << a.privacyVersion << " acknowledged on "
          << a.acceptedAt << " via " << a.via << (isCurrent(a) ? " (current)" : " (outdated: newer versions exist)") << "\n";
    }
    o << "Record: " << recordPath.string() << "\n";
    return o.str();
}

}  // namespace

const std::vector<std::string>& documentIds() {
    static const std::vector<std::string> ids = {"terms", "privacy", "license", "licensing"};
    return ids;
}

Result<Document> document(std::string_view id) {
    if (id == "terms") return fromEmbedded("terms", "docs/legal/TERMS.md", "skywalker://docs/legal/TERMS");
    if (id == "privacy") return fromEmbedded("privacy", "docs/legal/PRIVACY.md", "skywalker://docs/legal/PRIVACY");
    if (id == "licensing") {
        Document d = fromEmbedded("licensing", "docs/LICENSING.md", "skywalker://docs/LICENSING");
        d.version = kLicenseId;
        return d;
    }
    if (id == "license") {
        Document d;
        d.id = "license";
        d.title = kLicenseName;
        d.version = kLicenseId;
        d.source = "LICENSE";
        d.text = kLicenseText;
        return d;
    }
    std::string near = str::closest(id, documentIds());
    return Error::make("unknown_document", "unknown legal document '" + std::string(id) + "'",
                       near.empty() ? "use one of: terms, privacy, license, licensing" : "did you mean '" + near + "'?");
}

std::string headerField(std::string_view text, std::string_view key) {
    std::string prefix = std::string(key) + ":";
    for (const auto& line : str::split(text, '\n')) {
        if (str::startsWith(line, prefix)) return str::trim(std::string_view(line).substr(prefix.size()));
    }
    return {};
}

std::string plainSummary(std::string_view text) {
    constexpr std::string_view heading = "## In plain language";
    size_t at = text.find(heading);
    if (at == std::string_view::npos) return {};
    size_t start = text.find('\n', at);
    if (start == std::string_view::npos) return {};
    size_t end = text.find("\n## ", start);
    return str::trim(text.substr(start + 1, end == std::string_view::npos ? std::string_view::npos : end - start - 1));
}

std::vector<std::string> placeholders(std::string_view text) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = text.find("{{", at)) != std::string_view::npos) {
        size_t end = text.find("}}", at + 2);
        if (end == std::string_view::npos) break;
        std::string name(text.substr(at, end + 2 - at));
        bool valid = end > at + 2;
        for (char c : name.substr(2, name.size() - 4)) valid = valid && (std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_');
        if (valid && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        at = end + 2;
    }
    return out;
}

fs::path defaultRecordPath() {
    if (const char* dir = std::getenv("SKY_LEGAL_DIR"); dir && *dir) return fs::path(dir) / kRecordFileName;
    const char* home = std::getenv("HOME");
    fs::path base = home && *home ? fs::path(home) : fs::temp_directory_path();
#ifdef __APPLE__
    return base / "Library" / "Application Support" / "Skywalker" / kRecordFileName;
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return fs::path(xdg) / "skywalker" / kRecordFileName;
    return base / ".local" / "share" / "skywalker" / kRecordFileName;
#endif
}

Result<AcceptanceRecord> loadRecord(const fs::path& path) {
    AcceptanceRecord record;
    std::error_code ec;
    if (!fs::exists(path, ec)) return record;
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    auto json = Json::parse(ss.str());
    if (!json || !json->isObject()) {
        return Error::make("corrupt_record", "the acceptance record " + path.string() + " is not valid JSON",
                           "accept again (the editor or `skywalker legal --accept`) to rewrite it");
    }
    for (const auto& h : json->get("history").elements()) {
        if (h.isObject()) record.history.push_back(acceptanceFrom(h));
    }
    if (!json->get("terms_version").asString().empty()) record.latest = acceptanceFrom(*json);
    else if (!record.history.empty()) record.latest = record.history.back();
    return record;
}

std::string nowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

Result<Acceptance> recordAcceptance(const fs::path& path, std::string via, std::string acceptedAt) {
    auto terms = document("terms");
    auto privacy = document("privacy");
    if (!terms || !privacy || terms->text.empty() || privacy->text.empty()) {
        return Error::make("missing_documents", "the legal documents are not embedded in this build");
    }
    auto record = loadRecord(path);
    AcceptanceRecord existing = record ? std::move(record.value()) : AcceptanceRecord{};  // a corrupt record is rewritten
    Acceptance a{requireVersion(*terms), requireVersion(*privacy), acceptedAt.empty() ? nowIso8601() : std::move(acceptedAt),
                 std::move(via), SKY_VERSION_STRING};
    existing.history.push_back(a);
    if (existing.history.size() > kHistoryLimit) {
        existing.history.erase(existing.history.begin(),
                               existing.history.begin() + static_cast<std::ptrdiff_t>(existing.history.size() - kHistoryLimit));
    }
    Json out = acceptanceJson(a);
    out["schema"] = 1;
    Json history = Json::array();
    for (const auto& h : existing.history) history.push(acceptanceJson(h));
    out["history"] = std::move(history);
    if (Status st = writeAtomically(path, out.dump(2) + "\n"); !st) return st.error();
    return a;
}

bool isCurrent(const Acceptance& a) {
    auto terms = document("terms");
    auto privacy = document("privacy");
    return terms && privacy && a.termsVersion == requireVersion(*terms) && a.privacyVersion == requireVersion(*privacy);
}

Result<Json> info(const fs::path& recordPath, std::string_view includeText) {
    Json warnings = Json::array();
    Json docs = Json::object();
    Json unfilled = Json::array();
    for (const auto& id : documentIds()) {
        auto d = document(id);
        if (!d) return d.error();
        Json j = Json::object({{"title", d->title}, {"version", d->version}, {"source", d->source}});
        if (!d->effectiveDate.empty()) j["effective_date"] = d->effectiveDate;
        if (!d->resource.empty()) j["resource"] = d->resource;
        if (d->text.empty()) warnings.push(d->source + " is not embedded in this build");
        if (id == "terms" || id == "privacy") {
            for (const auto& p : placeholders(d->text)) {
                bool seen = false;
                for (const auto& u : unfilled.elements()) seen = seen || u.asString() == p;
                if (!seen) unfilled.push(p);
            }
        }
        docs[id] = std::move(j);
    }
    if (!includeText.empty()) {
        auto d = document(includeText);
        if (!d) return d.error();
        docs[d->id]["text"] = d->text;
    }
    docs["license"]["change_license"] = "Apache-2.0 (four years after each version's first public release)";
    docs["license"]["packaged_apps"] = "skywalker build writes the license notices into Contents/Resources/Licenses/";

    Json acceptance = Json::object({{"record", recordPath.string()}});
    auto record = loadRecord(recordPath);
    if (!record) {
        acceptance["status"] = "unreadable";
        warnings.push(record.error().message);
    } else if (!record->latest) {
        acceptance["status"] = "not_recorded";
    } else {
        acceptance["status"] = isCurrent(*record->latest) ? "current" : "outdated";
        acceptance["latest"] = acceptanceJson(*record->latest);
        acceptance["history_count"] = static_cast<int64_t>(record->history.size());
        if (!isCurrent(*record->latest)) {
            warnings.push("the recorded acceptance is for older document versions; the human accepts the new ones in the editor or "
                          "with `skywalker legal --accept`");
        }
    }
    acceptance["how_to_accept"] =
        "A person accepts in the editor (first-launch sheet; Settings > Legal) or runs `skywalker legal --accept` for headless use. "
        "Agents cannot accept on a person's behalf, and nothing (CLI, MCP, tools) waits for acceptance.";
    if (unfilled.size() > 0) {
        warnings.push("the Terms and Privacy Notice are templates awaiting legal review: " + std::to_string(unfilled.size()) +
                      " placeholders are unfilled (see `placeholders`)");
    }
    return Json::object({{"documents", std::move(docs)},
                         {"acceptance", std::move(acceptance)},
                         {"data", dataPractices()},
                         {"placeholders", std::move(unfilled)},
                         {"warnings", std::move(warnings)}});
}

int runCommand(const std::vector<std::string>& args, const fs::path& recordPath, std::string& out, std::string& err) {
    bool accept = false, status = false, json = false;
    std::vector<std::string> docs;
    for (const auto& a : args) {
        if (a == "--accept") accept = true;
        else if (a == "--status") status = true;
        else if (a == "--json") json = true;
        else if (a == "--help" || a == "-h" || a == "help") {
            out += kUsage;
            return 0;
        } else if (str::startsWith(a, "-")) {
            std::string near = str::closest(a, {"--accept", "--status", "--json", "--help"});
            err += "error: unknown option " + a + (near.empty() ? "" : " (did you mean " + near + "?)") + "\n" + kUsage;
            return 2;
        } else {
            auto d = document(a);
            if (!d) {
                err += "error: " + d.error().message + "; " + d.error().hint + "\n";
                return 2;
            }
            docs.push_back(a);
        }
    }
    if (accept) {
        auto a = recordAcceptance(recordPath, "cli");
        if (!a) {
            err += "error: " + a.error().message + (a.error().hint.empty() ? "" : " (" + a.error().hint + ")") + "\n";
            return 1;
        }
        if (!json) {
            out += "Recorded: you accept the Skywalker Terms of Use " + a->termsVersion + " and acknowledge the Privacy Notice " +
                   a->privacyVersion + " (" + a->acceptedAt + ").\nRecord: " + recordPath.string() + "\n";
        }
        if (!json && !status && docs.empty()) return 0;
    }
    if (json) {
        auto payload = info(recordPath, docs.empty() ? std::string_view{} : std::string_view(docs.front()));
        if (!payload) {
            err += "error: " + payload.error().message + "\n";
            return 1;
        }
        out += payload->dump(2) + "\n";
        return 0;
    }
    if (status) {
        out += statusText(recordPath);
        if (docs.empty()) return 0;
    }
    if (docs.empty()) {
        out += statusText(recordPath);
        docs = {"terms", "privacy", "licensing"};
    }
    for (const auto& id : docs) {
        auto d = document(id);
        out += "\n" + std::string(78, '=') + "\n" + d->text;
        if (!d->text.empty() && d->text.back() != '\n') out += "\n";
    }
    return 0;
}

}  // namespace sky::legal
