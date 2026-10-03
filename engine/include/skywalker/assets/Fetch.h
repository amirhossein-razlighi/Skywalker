#pragma once
// Fetching assets from the web (asset_download) and unpacking .zip archives.
// Downloads are user-approved actions: the tool is flagged open-world so MCP clients and
// the in-editor crew ask before running it.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky::net {

struct HttpResponse {
    long status = 0;
    std::vector<uint8_t> body;
    std::string contentType;
    std::string finalUrl;  // after redirects
};

/// HTTP(S) GET with redirects, a size cap and a timeout. Only http:// and https:// URLs.
Result<HttpResponse> get(const std::string& url, size_t maxBytes, long timeoutSeconds = 180);

/// Resolves `ref` (relative path or absolute URL) against `base`.
std::string resolveUrl(const std::string& base, const std::string& ref);

}  // namespace sky::net

namespace sky::zip {

struct Entry {
    std::string name;  // normalized relative path ("/" separators)
    std::vector<uint8_t> data;
};

/// Extracts stored / deflated entries. Rejects encrypted entries, zip64, path traversal
/// and archives that expand beyond `maxTotal` bytes (zip-bomb guard).
Result<std::vector<Entry>> extract(const std::vector<uint8_t>& archive, size_t maxTotal = size_t{512} << 20);

}  // namespace sky::zip
