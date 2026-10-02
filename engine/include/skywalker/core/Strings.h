#pragma once
// Small string helpers shared across the engine.

#include <string>
#include <string_view>
#include <vector>

namespace sky::str {

std::string lower(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
std::vector<std::string> split(std::string_view s, char sep);
std::string trim(std::string_view s);

/// Simple glob matching supporting '*' and '?' (case-insensitive).
bool globMatch(std::string_view pattern, std::string_view text);

/// Levenshtein edit distance, used for "did you mean ...?" hints.
size_t editDistance(std::string_view a, std::string_view b);

/// Returns the closest candidate within `maxDistance`, or empty string.
std::string closest(std::string_view word, const std::vector<std::string>& candidates, size_t maxDistance = 2);

std::string base64Encode(const void* data, size_t size);

}  // namespace sky::str
