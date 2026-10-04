// legal_info: the Terms of Use, Privacy Notice and license versions, whether a person has accepted them on this
// computer, and what data the software sends where (docs/legal/). Read-only: agents cannot accept for a person.

#include "ToolHelpers.h"
#include "skywalker/legal/Legal.h"

namespace sky::tools {

using namespace schema;

void addLegalTools(Engine& /*engine*/, ToolRegistry& reg) {
    reg.add({"legal_info", "Legal documents and acceptance",
             "Skywalker's Terms of Use, Privacy Notice and license: their versions and effective dates, MCP resource URIs "
             "(skywalker://docs/legal/TERMS, skywalker://docs/legal/PRIVACY, skywalker://docs/LICENSING), whether a person "
             "has accepted the current versions on this computer (status current, outdated, not_recorded), and a verified "
             "summary of data practices (no telemetry; data leaves the computer only to the AI provider or download URL the "
             "human chose). Use it to answer licensing or privacy questions, or before advising on shipping. `document` "
             "(terms, privacy, license, licensing) adds that document's full text. Nothing waits for acceptance; a person "
             "accepts in the editor or with `skywalker legal --accept`, never an agent. Example: {\"document\": \"privacy\"}.",
             "files",
             object({{"document", enumeration(legal::documentIds(),
                                              "Include this document's full text: terms, privacy, license (the LICENSE "
                                              "file) or licensing (plain-language summary and third-party notices)")}}),
             false, false, [](const Json& a, ToolContext&) {
                 auto payload = legal::info(legal::defaultRecordPath(), a.get("document").asString());
                 if (!payload) return ToolResult::error(payload.error());
                 const Json& acc = payload->get("acceptance");
                 std::string summary = "Terms " + payload->get("documents").get("terms").get("version").asString() + ", Privacy Notice " +
                                       payload->get("documents").get("privacy").get("version").asString() + ", license " +
                                       payload->get("documents").get("license").get("version").asString() + "; acceptance " +
                                       acc.get("status").asString();
                 return ToolResult::json(std::move(payload.value()), summary);
             }});
}

}  // namespace sky::tools
