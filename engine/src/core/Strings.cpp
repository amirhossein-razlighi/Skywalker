#include "skywalker/core/Strings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace sky::str {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        out.emplace_back(s.substr(start, pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool globMatch(std::string_view pattern, std::string_view text) {
    // Iterative wildcard matching with backtracking on the last '*'.
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t cost = std::tolower(static_cast<unsigned char>(a[i - 1])) ==
                                  std::tolower(static_cast<unsigned char>(b[j - 1]))
                              ? 0
                              : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string closest(std::string_view word, const std::vector<std::string>& candidates, size_t maxDistance) {
    std::string best;
    size_t bestDist = maxDistance + 1;
    for (const auto& c : candidates) {
        size_t d = editDistance(word, c);
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

std::string base64Encode(const void* data, size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        uint32_t n = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
    }
    if (i < size) {
        uint32_t n = uint32_t(bytes[i]) << 16;
        if (i + 1 < size) n |= uint32_t(bytes[i + 1]) << 8;
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? kTable[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

bool parseDouble(std::string_view text, double& out) {
    if (text.empty() || text.size() > 512) return false;
    std::string buf(text);  // NUL-terminated copy for strtod
    char* end = nullptr;
    // A dedicated "C" locale keeps parsing independent of the host app's LC_NUMERIC.
    static locale_t cLocale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    out = strtod_l(buf.c_str(), &end, cLocale);
    return end == buf.c_str() + buf.size();
}

}  // namespace sky::str
