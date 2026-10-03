#pragma once
// Documentation and agent-integration files embedded in the binary at build time.
//
// The engine ships its own docs (docs/*.md) and the generated agent integrations (skills, subagents,
// commands and config snippets for Claude Code, Codex, Gemini CLI and Cursor, see integrations/) so that
//   * the MCP server can serve them as resources and prompts (resources/list, prompts/list), and
//   * `skywalker setup` can install them without the source tree.
// The embedded keys are repo-relative paths: "docs/STUDIO.md", "integrations/claude-code/skills/skywalker-core/SKILL.md", ...
// Skills are identical for every tool, so only the Claude Code tree carries them (plus the
// codex-only workflow skills); `setup` maps them to each tool's directory layout.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sky {

struct EmbeddedAsset {
    const char* path;
    const char* content;  // NUL-terminated text
};

/// All embedded files, sorted by path.
const std::vector<EmbeddedAsset>& embeddedAssets();
/// Exact lookup by repo-relative path; nullptr if absent.
const EmbeddedAsset* findEmbeddedAsset(std::string_view path);
/// Embedded files whose path starts with `prefix`.
std::vector<const EmbeddedAsset*> embeddedAssetsUnder(std::string_view prefix);

/// A markdown file with a flat `key: value` frontmatter block (values may be JSON-quoted).
struct MarkdownDoc {
    std::vector<std::pair<std::string, std::string>> fields;
    std::string body;
    std::string get(std::string_view key) const;
};
MarkdownDoc parseMarkdownDoc(std::string_view text);

/// Markers delimiting the block `skywalker setup` manages inside AGENTS.md / GEMINI.md / CLAUDE.md.
inline constexpr const char* kManagedBegin = "<!-- BEGIN SKYWALKER (managed by `skywalker setup`; edit outside these markers) -->";
inline constexpr const char* kManagedEnd = "<!-- END SKYWALKER -->";

}  // namespace sky
