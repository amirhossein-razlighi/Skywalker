#include "skywalker/assets/Fetch.h"

#include <curl/curl.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "skywalker/core/Strings.h"

namespace sky::net {

namespace {

struct Sink {
    std::vector<uint8_t>* out;
    size_t max;
    bool overflow = false;
};

size_t onData(char* ptr, size_t size, size_t n, void* user) {
    auto* s = static_cast<Sink*>(user);
    size_t bytes = size * n;
    if (s->out->size() + bytes > s->max) {
        s->overflow = true;
        return 0;  // abort the transfer
    }
    s->out->insert(s->out->end(), ptr, ptr + bytes);
    return bytes;
}

}  // namespace

Result<HttpResponse> get(const std::string& url, size_t maxBytes, long timeoutSeconds) {
    std::string lower = str::lower(url);
    if (!str::startsWith(lower, "https://") && !str::startsWith(lower, "http://")) {
        return Error::make("invalid_url", "only http(s) URLs can be downloaded: " + url);
    }
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (!c) return Error::make("network_error", "could not initialize HTTP client");
    HttpResponse r;
    Sink sink{&r.body, maxBytes};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Skywalker/" SKY_VERSION_STRING " (asset_download)");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    char* ct = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) r.contentType = ct;
    char* eff = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) r.finalUrl = eff;
    curl_easy_cleanup(c);
    if (sink.overflow) {
        return Error::make("too_large", "download exceeds " + std::to_string(maxBytes >> 20) + " MB", "pick a smaller file or LOD");
    }
    if (rc != CURLE_OK) return Error::make("network_error", std::string("download failed: ") + curl_easy_strerror(rc));
    if (r.status >= 400) {
        return Error::make("http_error", "server answered HTTP " + std::to_string(r.status) + " for " + url,
                           r.status == 401 || r.status == 403 ? "the asset needs a login or isn't public; ask the human to download it"
                                                              : "check the URL");
    }
    return r;
}

std::string resolveUrl(const std::string& base, const std::string& ref) {
    std::string lower = str::lower(ref);
    if (str::startsWith(lower, "http://") || str::startsWith(lower, "https://")) return ref;
    std::string b = base.substr(0, base.find_first_of("?#"));
    if (!ref.empty() && ref[0] == '/') {
        size_t scheme = b.find("://");
        size_t host = b.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        return (host == std::string::npos ? b : b.substr(0, host)) + ref;
    }
    return b.substr(0, b.rfind('/') + 1) + ref;
}

}  // namespace sky::net

namespace sky::zip {

namespace {

uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

/// "a/b/../c" -> "a/c"; rejects absolute paths and anything escaping the archive root.
bool safeName(std::string name, std::string& out) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    std::vector<std::string> parts;
    for (const auto& p : str::split(name, '/')) {
        if (p.empty() || p == ".") continue;
        if (p == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else {
            parts.push_back(p);
        }
    }
    out.clear();
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return !out.empty();
}

}  // namespace

Result<std::vector<Entry>> extract(const std::vector<uint8_t>& a, size_t maxTotal) {
    if (a.size() < 22) return Error::make("invalid_zip", "not a zip archive");
    // End of central directory: scan the last 64 KB for its signature.
    size_t eocd = std::string::npos;
    size_t stop = a.size() > 65557 ? a.size() - 65557 : 0;
    for (size_t i = a.size() - 22 + 1; i-- > stop;) {
        if (u32(&a[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return Error::make("invalid_zip", "not a zip archive (no central directory)");
    uint16_t count = u16(&a[eocd + 10]);
    uint32_t cdOffset = u32(&a[eocd + 16]);
    if (cdOffset == 0xFFFFFFFF || count == 0xFFFF) return Error::make("unsupported_zip", "zip64 archives are not supported");
    std::vector<Entry> out;
    size_t total = 0;
    size_t p = cdOffset;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > a.size() || u32(&a[p]) != 0x02014b50) return Error::make("invalid_zip", "corrupt central directory");
        uint16_t flags = u16(&a[p + 8]), method = u16(&a[p + 10]);
        uint32_t csize = u32(&a[p + 20]), usize = u32(&a[p + 24]);
        uint16_t nlen = u16(&a[p + 28]), xlen = u16(&a[p + 30]), clen = u16(&a[p + 32]);
        uint32_t local = u32(&a[p + 42]);
        if (p + 46 + nlen > a.size()) return Error::make("invalid_zip", "corrupt entry name");
        std::string rawName(reinterpret_cast<const char*>(&a[p + 46]), nlen);
        p += 46 + nlen + xlen + clen;
        if (!rawName.empty() && rawName.back() == '/') continue;  // directory
        if (flags & 1) return Error::make("unsupported_zip", "encrypted zip entries are not supported");
        std::string name;
        if (!safeName(rawName, name)) return Error::make("unsafe_zip", "zip entry escapes the archive: " + rawName);
        if (local + 30 > a.size() || u32(&a[local]) != 0x04034b50) return Error::make("invalid_zip", "corrupt local header");
        size_t data = local + 30 + u16(&a[local + 26]) + u16(&a[local + 28]);
        if (data + csize > a.size()) return Error::make("invalid_zip", "truncated entry " + name);
        total += usize;
        if (total > maxTotal) return Error::make("too_large", "archive expands beyond " + std::to_string(maxTotal >> 20) + " MB");
        Entry e{name, {}};
        if (method == 0) {
            e.data.assign(a.begin() + static_cast<std::ptrdiff_t>(data), a.begin() + static_cast<std::ptrdiff_t>(data + csize));
        } else if (method == 8) {
            e.data.resize(usize);
            z_stream zs{};
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return Error::make("zip_error", "inflate init failed");
            zs.next_in = const_cast<Bytef*>(&a[data]);
            zs.avail_in = csize;
            zs.next_out = e.data.data();
            zs.avail_out = usize;
            int rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END || zs.total_out != usize) return Error::make("zip_error", "corrupt deflate data in " + name);
        } else {
            return Error::make("unsupported_zip", "unsupported compression method " + std::to_string(method) + " in " + name);
        }
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace sky::zip
