// String table files: CSV (key + one column per locale) and gettext .po catalogs.

#include <algorithm>
#include <cstdlib>

#include "skywalker/core/Strings.h"
#include "skywalker/locale/Localization.h"

namespace sky::loc {

Json Diagnostic::toJson() const {
    return Json::object({{"file", file}, {"line", line}, {"code", code}, {"message", message}});
}

void StringTable::merge(const StringTable& other) {
    locales.insert(other.locales.begin(), other.locales.end());
    problems.insert(problems.end(), other.problems.begin(), other.problems.end());
    for (const auto& [key, entry] : other.entries) {
        auto [it, fresh] = entries.emplace(key, entry);
        if (fresh) continue;
        StringEntry& mine = it->second;
        for (const auto& [loc, text] : entry.text) {
            auto [t, added] = mine.text.emplace(loc, text);
            if (!added && t->second != text) {
                problems.push_back({entry.file, entry.line, "duplicate_key",
                                    "'" + key + "' (" + loc + ") is also defined in " + mine.file + ":" + std::to_string(mine.line) +
                                        "; the first definition wins"});
            }
        }
        if (mine.comment.empty()) mine.comment = entry.comment;
        if (!mine.maxLength) mine.maxLength = entry.maxLength;
    }
}

// --- CSV ------------------------------------------------------------------------------------

namespace {

struct CsvRow {
    std::vector<std::string> cells;
    int line = 0;
};

std::vector<CsvRow> csvRows(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);  // UTF-8 BOM (spreadsheet exports)
    }
    std::vector<CsvRow> rows;
    CsvRow row;
    std::string cell;
    bool quoted = false, any = false;
    int line = 1;
    row.line = 1;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                if (c == '\n') ++line;
                if (c != '\r') cell += c;
            }
            continue;
        }
        if (c == '"' && cell.empty()) {
            quoted = true;
            any = true;
        } else if (c == ',') {
            row.cells.push_back(std::move(cell));
            cell.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !cell.empty()) {
                row.cells.push_back(std::move(cell));
                rows.push_back(std::move(row));
            }
            cell.clear();
            row = CsvRow{};
            any = false;
            row.line = ++line;
        } else {
            cell += c;
            any = true;
        }
    }
    if (any || !cell.empty()) {
        row.cells.push_back(std::move(cell));
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace

std::string csvField(std::string_view s) {
    if (s.find_first_of(",\"\n\r") == std::string_view::npos && (s.empty() || (s.front() != ' ' && s.back() != ' '))) {
        return std::string(s);
    }
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

StringTable parseCsv(std::string_view text, const std::string& file) {
    StringTable t;
    std::vector<CsvRow> rows = csvRows(text);
    size_t first = 0;
    while (first < rows.size() && !rows[first].cells.empty() && str::startsWith(str::trim(rows[first].cells[0]), "#")) ++first;
    if (first >= rows.size()) return t;
    const CsvRow& header = rows[first];
    if (header.cells.empty() || str::lower(str::trim(header.cells[0])) != "key") {
        t.problems.push_back({file, header.line, "invalid_csv", "the first column must be \"key\" (header: key,en,fr,...)"});
        return t;
    }
    enum class Col { Locale, Comment, Max, Ignore };
    std::vector<Col> kinds;
    std::vector<std::string> names;
    for (size_t c = 1; c < header.cells.size(); ++c) {
        std::string h = str::trim(header.cells[c]);
        std::string low = str::lower(h);
        if (low == "comment" || low == "comments" || low == "context" || low == "notes" || low == "description") {
            kinds.push_back(Col::Comment);
        } else if (low == "max" || low == "max_length" || low == "maxlength" || low == "limit") {
            kinds.push_back(Col::Max);
        } else if (h.empty() || h[0] == '#' || normalizeLocale(h).empty()) {
            kinds.push_back(Col::Ignore);
        } else {
            kinds.push_back(Col::Locale);
            h = normalizeLocale(h);
            t.locales.insert(h);
        }
        names.push_back(h);
    }
    for (size_t r = first + 1; r < rows.size(); ++r) {
        const CsvRow& row = rows[r];
        if (row.cells.empty()) continue;
        std::string key = str::trim(row.cells[0]);
        if (key.empty() || key[0] == '#') continue;
        if (row.cells.size() > header.cells.size()) {
            t.problems.push_back({file, row.line, "extra_cells", "'" + key + "' has more cells than the header (an unquoted comma?)"});
        }
        StringEntry e;
        e.file = file;
        e.line = row.line;
        for (size_t c = 1; c < row.cells.size() && c - 1 < kinds.size(); ++c) {
            const std::string& v = row.cells[c];
            switch (kinds[c - 1]) {
                case Col::Locale:
                    if (!v.empty()) e.text[names[c - 1]] = v;
                    break;
                case Col::Comment: e.comment = v; break;
                case Col::Max: e.maxLength = std::max(0, std::atoi(v.c_str())); break;
                case Col::Ignore: break;
            }
        }
        if (t.entries.count(key)) {
            t.problems.push_back({file, row.line, "duplicate_key", "'" + key + "' is defined twice in this file; the first row wins"});
            continue;
        }
        t.entries.emplace(key, std::move(e));
    }
    return t;
}

// --- PO -------------------------------------------------------------------------------------

namespace {

std::string poUnescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        char n = s[++i];
        switch (n) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            default: out += n; break;  // \" \\ and anything else
        }
    }
    return out;
}

/// The quoted string starting at `s` (after the keyword), or nullopt when there is none.
std::optional<std::string> quotedValue(std::string_view raw) {
    const std::string s = str::trim(raw);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return std::nullopt;
    return poUnescape(std::string_view(s).substr(1, s.size() - 2));
}

struct PoEntry {
    std::string ctxt, id, idPlural, comment;
    std::map<int, std::string> str;
    bool fuzzy = false, any = false, hasId = false;
    int line = 0;
};

std::string headerField(const std::string& header, const std::string& name) {
    for (const auto& l : str::split(header, '\n')) {
        if (str::startsWith(str::lower(l), str::lower(name) + ":")) return str::trim(l.substr(name.size() + 1));
    }
    return {};
}

/// gettext "%d" (and "%i") -> the ICU plural '#'.
std::string poPluralText(std::string s) {
    for (const char* f : {"%d", "%i", "%u"}) {
        for (size_t p = s.find(f); p != std::string::npos; p = s.find(f, p + 1)) s.replace(p, 2, "#");
    }
    return s;
}

std::string pluralMessage(const std::vector<Plural>& order, const std::map<int, std::string>& forms) {
    std::string out = "{count, plural,";
    bool other = false;
    for (const auto& [idx, text] : forms) {
        if (idx < 0 || static_cast<size_t>(idx) >= order.size()) continue;
        other = other || order[static_cast<size_t>(idx)] == Plural::Other;
        out += std::string(" ") + toString(order[static_cast<size_t>(idx)]) + " {" + poPluralText(text) + "}";
    }
    if (!other && !forms.empty()) out += " other {" + poPluralText(forms.rbegin()->second) + "}";  // fractions: the last form
    return out + "}";
}

}  // namespace

StringTable parsePo(std::string_view text, const std::string& file, const std::string& localeHint, const std::string& source) {
    StringTable t;
    std::vector<PoEntry> entries;
    PoEntry cur;
    std::string* last = nullptr;  // the field continuation lines append to
    int lineNo = 0;
    auto finish = [&] {
        if (cur.any) entries.push_back(std::move(cur));
        cur = PoEntry{};
        last = nullptr;
    };
    for (const auto& raw : str::split(text, '\n')) {
        ++lineNo;
        std::string line = str::trim(raw);
        if (line.empty()) {
            finish();
            continue;
        }
        if (line[0] == '#') {
            if (!cur.str.empty()) finish();  // comments open the next entry (catalogs without blank lines)
            if (str::startsWith(line, "#,") && line.find("fuzzy") != std::string::npos) cur.fuzzy = true;
            if (str::startsWith(line, "#.")) cur.comment += (cur.comment.empty() ? "" : " ") + str::trim(line.substr(2));
            continue;
        }
        if (line[0] == '"') {
            if (auto v = quotedValue(line); v && last) *last += *v;
            continue;
        }
        size_t sp = line.find_first_of(" \t");
        std::string kw = sp == std::string::npos ? line : line.substr(0, sp);
        auto value = quotedValue(sp == std::string::npos ? std::string_view() : std::string_view(line).substr(sp));
        if (!value) {
            t.problems.push_back({file, lineNo, "invalid_po", "expected a quoted string after " + kw});
            continue;
        }
        // A new msgctxt, or a msgid after a complete entry, starts the next entry.
        if ((kw == "msgctxt" && (cur.hasId || !cur.str.empty())) || (kw == "msgid" && (cur.hasId || !cur.str.empty()))) finish();
        if (!cur.any) cur.line = lineNo;
        cur.any = true;
        if (kw == "msgctxt") {
            cur.ctxt = *value;
            last = &cur.ctxt;
        } else if (kw == "msgid") {
            cur.id = *value;
            cur.hasId = true;
            last = &cur.id;
        } else if (kw == "msgid_plural") {
            cur.idPlural = *value;
            last = &cur.idPlural;
        } else if (kw == "msgstr") {
            cur.str[0] = *value;
            last = &cur.str[0];
        } else if (str::startsWith(kw, "msgstr[") && kw.back() == ']') {
            int idx = std::atoi(kw.c_str() + 7);
            cur.str[idx] = *value;
            last = &cur.str[idx];
        } else {
            t.problems.push_back({file, lineNo, "invalid_po", "unknown keyword " + kw});
        }
    }
    finish();

    std::string locale = normalizeLocale(localeHint);
    for (const auto& e : entries) {
        if (e.ctxt.empty() && e.id.empty()) {  // the header
            std::string lang = normalizeLocale(headerField(e.str.count(0) ? e.str.at(0) : std::string(), "Language"));
            if (!lang.empty()) locale = lang;
        }
    }
    if (locale.empty()) {
        t.problems.push_back({file, 1, "unknown_locale", "no Language header and the file name is not a locale (name it fr.po or pt_BR.po)"});
        return t;
    }
    t.locales.insert(locale);
    const std::string src = normalizeLocale(source);
    for (const auto& e : entries) {
        if (e.ctxt.empty() && e.id.empty()) continue;
        const std::string key = e.ctxt.empty() ? e.id : e.ctxt;
        StringEntry se;
        se.file = file;
        se.line = e.line;
        se.comment = e.comment;
        if (!e.ctxt.empty() && !e.id.empty() && locale != src) {
            // msgctxt = key, msgid = the source text: it is the source language's string too.
            se.text[src] = e.idPlural.empty() ? e.id : pluralMessage(gettextPluralOrder(src), {{0, e.id}, {1, e.idPlural}});
        }
        if (e.fuzzy) {
            t.problems.push_back({file, e.line, "fuzzy", "'" + key + "' is marked fuzzy (needs review): not used"});
        } else if (!e.idPlural.empty()) {
            bool any = false;
            for (const auto& [i, s] : e.str) any = any || !s.empty();
            if (any) se.text[locale] = pluralMessage(gettextPluralOrder(locale), e.str);
        } else if (e.str.count(0) && !e.str.at(0).empty()) {
            se.text[locale] = e.str.at(0);
        }
        if (t.entries.count(key)) {
            t.problems.push_back({file, e.line, "duplicate_key", "'" + key + "' is defined twice in this catalog"});
            continue;
        }
        t.entries.emplace(key, std::move(se));
    }
    return t;
}

}  // namespace sky::loc
