#include "skywalker/agent/Resources.h"

#include <algorithm>
#include <cstring>

#include "skywalker/core/Json.h"

namespace sky {

const std::vector<EmbeddedAsset>& embeddedAssets() {
    static const std::vector<EmbeddedAsset> assets = [] {
        std::vector<EmbeddedAsset> v{
#include "AgentEmbedded.inc"
        };
        std::sort(v.begin(), v.end(), [](const EmbeddedAsset& a, const EmbeddedAsset& b) { return std::strcmp(a.path, b.path) < 0; });
        return v;
    }();
    return assets;
}

const EmbeddedAsset* findEmbeddedAsset(std::string_view path) {
    for (const auto& a : embeddedAssets()) {
        if (path == a.path) return &a;
    }
    return nullptr;
}

std::vector<const EmbeddedAsset*> embeddedAssetsUnder(std::string_view prefix) {
    std::vector<const EmbeddedAsset*> out;
    for (const auto& a : embeddedAssets()) {
        if (std::string_view(a.path).substr(0, prefix.size()) == prefix) out.push_back(&a);
    }
    return out;
}

std::string MarkdownDoc::get(std::string_view key) const {
    for (const auto& [k, v] : fields) {
        if (k == key) return v;
    }
    return {};
}

MarkdownDoc parseMarkdownDoc(std::string_view text) {
    MarkdownDoc doc;
    if (text.substr(0, 4) != "---\n") {
        doc.body = std::string(text);
        return doc;
    }
    size_t end = text.find("\n---\n", 4);
    if (end == std::string_view::npos) {
        doc.body = std::string(text);
        return doc;
    }
    std::string_view head = text.substr(4, end - 4);
    size_t pos = 0;
    while (pos <= head.size()) {
        size_t nl = head.find('\n', pos);
        std::string_view line = head.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? head.size() + 1 : nl + 1;
        size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string key(line.substr(0, colon));
        std::string value(line.substr(colon + 1));
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        if (value.size() >= 2 && value.front() == '"') {
            if (auto parsed = Json::parse(value); parsed && parsed->isString()) value = parsed->asString();
        }
        doc.fields.emplace_back(std::move(key), std::move(value));
    }
    size_t bodyStart = end + 5;
    while (bodyStart < text.size() && text[bodyStart] == '\n') ++bodyStart;
    doc.body = std::string(text.substr(bodyStart));
    return doc;
}

}  // namespace sky
