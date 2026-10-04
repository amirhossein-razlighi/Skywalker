#pragma once
// Terms of Use, Privacy Notice, license texts and the local acceptance record (docs/legal/).
//
// The documents are embedded in the binary (docs/legal/*.md through Resources.h; the LICENSE text like the
// notices `skywalker build` ships). Their "Version:" and "Effective date:" lines are the single source of
// truth for what the editor asks people to accept; the editor reads the same lines from its bundled copies.
//
// Acceptance is recorded only on the user's computer, in legal-acceptance.json: by the editor's first-launch
// sheet and by `skywalker legal --accept`. Nothing here blocks automation, and nothing is sent anywhere.
// Agents read the state with the `legal_info` tool; they cannot accept on a person's behalf.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::legal {

struct Document {
    std::string id;             // terms, privacy, license, licensing
    std::string title;          // first markdown heading (or "Business Source License 1.1")
    std::string version;        // "Version:" line; the license documents carry the license id instead
    std::string effectiveDate;  // "Effective date:" line (may still be a {{PLACEHOLDER}})
    std::string source;         // repo-relative source file, e.g. docs/legal/TERMS.md
    std::string resource;       // MCP resource URI, e.g. skywalker://docs/legal/TERMS
    std::string text;
};

/// The document ids, in display order: terms, privacy, license (the full LICENSE), licensing (plain-language
/// license summary and third-party notices).
const std::vector<std::string>& documentIds();
/// One document by id, with a did-you-mean error for unknown ids.
Result<Document> document(std::string_view id);

/// Value of a "Key: value" line at the start of a line (e.g. "Version"), trimmed; empty if absent.
std::string headerField(std::string_view text, std::string_view key);
/// The body of the "## In plain language" section, without its heading; empty if absent.
std::string plainSummary(std::string_view text);
/// Every distinct {{PLACEHOLDER}} in the text, in order of first appearance.
std::vector<std::string> placeholders(std::string_view text);

struct Acceptance {
    std::string termsVersion;
    std::string privacyVersion;
    std::string acceptedAt;  // ISO 8601 UTC, e.g. 2026-10-04T12:00:00Z
    std::string via;         // "editor" or "cli"
    std::string appVersion;
};

struct AcceptanceRecord {
    std::optional<Acceptance> latest;
    std::vector<Acceptance> history;  // oldest first, capped (kHistoryLimit)
};

inline constexpr size_t kHistoryLimit = 100;
/// Shown by `skywalker legal` and in the editor's legal screens.
inline constexpr const char* kDeveloperCredit = "Developed by: AmirHossein (Amir) Razlighi";
inline constexpr const char* kRecordFileName = "legal-acceptance.json";

/// Where the acceptance record lives: $SKY_LEGAL_DIR if set, else ~/Library/Application Support/Skywalker on
/// macOS (shared with the editor) and $XDG_DATA_HOME/skywalker (or ~/.local/share/skywalker) elsewhere.
std::filesystem::path defaultRecordPath();
/// Reads a record. A missing file is an empty record; a corrupt file is an error.
Result<AcceptanceRecord> loadRecord(const std::filesystem::path& path);
/// Records acceptance of the current Terms and acknowledgement of the current Privacy Notice (appended to the
/// history, written atomically). `acceptedAt` defaults to now.
Result<Acceptance> recordAcceptance(const std::filesystem::path& path, std::string via, std::string acceptedAt = {});
/// True when the acceptance covers the current Terms and Privacy Notice versions.
bool isCurrent(const Acceptance& a);
/// Current UTC time as ISO 8601 (seconds precision).
std::string nowIso8601();

/// The `legal_info` payload: documents, versions, acceptance state, data practices, warnings. With a document
/// id, that document's full text is included.
Result<Json> info(const std::filesystem::path& recordPath, std::string_view includeText = {});

/// `skywalker legal [terms|privacy|license|licensing] [--accept] [--status] [--json]` (the CLI front end is
/// tools/cli/main.cpp). Returns the exit code.
int runCommand(const std::vector<std::string>& args, const std::filesystem::path& recordPath, std::string& out, std::string& err);

}  // namespace sky::legal
