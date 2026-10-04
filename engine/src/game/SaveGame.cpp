// Save game files: game.json `saves` settings, the integrity hash, gzip, atomic slot writes and the
// platform save folder. The engine-facing capture / restore lives in SaveSystem.cpp.

#include "skywalker/game/SaveGame.h"

#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"

namespace sky::game {

namespace fs = std::filesystem;

namespace {

constexpr const char* kPlainExt = ".save.json";
constexpr const char* kGzipExt = ".save.json.gz";

Error badSettings(const std::string& message, const std::string& hint = {}) {
    return Error::make("invalid_game_json", "game.json saves: " + message, hint);
}

uint64_t fnv1a(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

bool isGzip(const std::string& bytes) {
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1f && static_cast<unsigned char>(bytes[1]) == 0x8b;
}

Result<std::string> gzipBytes(const std::string& in) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return Error::make("io_error", "cannot start gzip compression");
    }
    std::string out(deflateBound(&zs, static_cast<uLong>(in.size())) + 32, '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return Error::make("io_error", "gzip compression failed");
    return out;
}

Result<std::string> gunzipBytes(const std::string& in) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return Error::make("save_corrupted", "cannot start gzip decompression");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    char buf[1 << 15];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > (size_t{512} << 20)) rc = Z_MEM_ERROR;  // a save is never half a gigabyte
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        return Error::make("save_corrupted", "the compressed save is truncated or damaged",
                           "the file cannot be repaired: load another slot (save_list shows them)");
    }
    return out;
}

Result<std::string> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string isoNowUtc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

// --- settings ---------------------------------------------------------------------------------

Result<SaveSettings> SaveSettings::fromJson(const Json& j) {
    SaveSettings s;
    if (j.isNull()) return s;
    if (!j.isObject()) return badSettings("must be an object, e.g. {\"version\": 1, \"maxSlots\": 20}");
    static const std::vector<std::string> keys{"version", "maxSlots", "compress", "migrate"};
    for (const auto& [k, v] : j.members()) {
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
        std::string guess = str::closest(k, keys, 3);
        return badSettings("unknown field '" + k + "'",
                           (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") + "valid fields: version, maxSlots, compress, migrate");
    }
    auto whole = [&](const char* key, int lo, int hi, int& out) -> Status {
        const Json* v = j.find(key);
        if (!v || v->isNull()) return {};
        double n = v->asNumber(-1e300);
        if (!v->isNumber() || n < lo || n > hi || n != static_cast<double>(static_cast<int>(n))) {
            return badSettings(std::string(key) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
        }
        out = static_cast<int>(n);
        return {};
    };
    if (Status st = whole("version", 1, 1000000, s.version); !st) return st.error();
    if (Status st = whole("maxSlots", 1, 1000, s.maxSlots); !st) return st.error();
    if (const Json* c = j.find("compress"); c && !c->isNull()) {
        if (!c->isBool()) return badSettings("compress must be true or false");
        s.compress = c->asBool();
    }
    if (const Json* m = j.find("migrate"); m && !m->isNull()) {
        if (!m->isString()) return badSettings("migrate must be a project-relative .wander path");
        s.migrate = m->asString();
    }
    return s;
}

Json SaveSettings::toJson() const {
    Json j = Json::object({{"version", version}, {"maxSlots", maxSlots}, {"compress", compress}});
    if (!migrate.empty()) j["migrate"] = migrate;
    return j;
}

Json SaveInfo::toJson() const {
    if (!error.empty()) return Json::object({{"slot", slot}, {"file", file}, {"bytes", bytes}, {"error", error}});
    return Json::object({{"slot", slot},
                         {"file", file},
                         {"bytes", bytes},
                         {"compressed", compressed},
                         {"formatVersion", formatVersion},
                         {"version", version},
                         {"playTime", playTime},
                         {"scene", scene},
                         {"savedAt", savedAt},
                         {"tick", tick},
                         {"meta", meta}});
}

// --- format -----------------------------------------------------------------------------------

std::string saveHash(const Json& doc) {
    Json body = doc;
    body.erase("hash");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "fnv1a64:%016llx", static_cast<unsigned long long>(fnv1a(body.dump())));
    return buf;
}

Result<Json> parseSave(const std::string& raw) {
    std::string text = raw;
    if (isGzip(raw)) {
        auto unpacked = gunzipBytes(raw);
        if (!unpacked) return unpacked.error();
        text = std::move(unpacked.value());
    }
    auto doc = Json::parse(text);
    if (!doc) {
        return Error::make("save_corrupted", "the save is not valid JSON (" + doc.error().message + ")",
                           "the file was truncated or edited by hand; load another slot (save_list shows them)");
    }
    Json& d = doc.value();
    if (!d.isObject() || d.get("format").asString() != kSaveFormat) {
        return Error::make("invalid_save", "not a Skywalker save (missing \"format\": \"skywalker.save\")");
    }
    const int formatVersion = static_cast<int>(d.get("formatVersion").asInt());
    if (formatVersion > kSaveFormatVersion) {
        return Error::make("save_too_new",
                           "the save uses format " + std::to_string(formatVersion) + "; this engine reads up to " +
                               std::to_string(kSaveFormatVersion),
                           "update the game to load it");
    }
    const std::string& hash = d.get("hash").asString();
    if (hash.empty() || hash != saveHash(d)) {
        return Error::make("save_corrupted", "the save's integrity hash does not match its content",
                           "the file was damaged or edited by hand; load another slot (save_list shows them)");
    }
    return std::move(d);
}

Status validateSlotName(const std::string& slot) {
    bool ok = !slot.empty() && slot.size() <= 64;
    for (char c : slot) {
        ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
    }
    if (ok) return {};
    return Error::make("invalid_slot", "invalid save slot name '" + slot + "'",
                       "use 1-64 lowercase letters, digits, '_' or '-' (e.g. \"slot1\", \"autosave\", \"chapter-2\")");
}

bool isReservedSlot(const std::string& slot) { return slot == "autosave" || slot == "quicksave"; }

std::string userSaveDir(const std::string& gameId) {
    const std::string id = gameId.empty() ? "skywalker-game" : gameId;
    const char* home = std::getenv("HOME");
    const std::string h = home && *home ? home : ".";
#if defined(__APPLE__)
    return (fs::path(h) / "Library" / "Application Support" / id / "saves").string();
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(h) / ".local" / "share";
    return (base / id / "saves").string();
#endif
}

// --- store ------------------------------------------------------------------------------------

std::string SaveStore::fileOf(const std::string& slot) const {
    std::error_code ec;
    for (const char* ext : {kPlainExt, kGzipExt}) {
        fs::path p = fs::path(dir_) / (slot + ext);
        if (fs::is_regular_file(p, ec)) return p.string();
    }
    return {};
}

bool SaveStore::exists(const std::string& slot) const { return validateSlotName(slot).ok() && !fileOf(slot).empty(); }

Status SaveStore::write(const std::string& slot, const Json& doc, bool compress) const {
    if (Status s = validateSlotName(slot); !s) return s;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return Error::make("io_error", "cannot create the save folder " + dir_ + ": " + ec.message());
    std::string bytes = doc.dump(compress ? -1 : 1);
    if (compress) {
        auto packed = gzipBytes(bytes);
        if (!packed) return packed.error();
        bytes = std::move(packed.value());
    }
    const fs::path target = fs::path(dir_) / (slot + (compress ? kGzipExt : kPlainExt));
    const fs::path temp = fs::path(dir_) / ("." + slot + "." + std::to_string(::getpid()) + ".tmp");
    FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) return Error::make("io_error", "cannot write " + temp.string());
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = std::fflush(f) == 0 && ok;
    ok = ::fsync(::fileno(f)) == 0 && ok;  // the bytes are on disk before the rename makes them the slot
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        fs::remove(temp, ec);
        return Error::make("io_error", "writing the save failed (disk full?)", "free some disk space and save again");
    }
    fs::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return Error::make("io_error", "cannot replace " + target.string() + ": " + ec.message());
    }
    // A slot is one file: drop the other variant when the compression setting changed.
    fs::remove(fs::path(dir_) / (slot + (compress ? kPlainExt : kGzipExt)), ec);
    return {};
}

Result<Json> SaveStore::read(const std::string& slot) const {
    if (Status s = validateSlotName(slot); !s) return s.error();
    const std::string file = fileOf(slot);
    if (file.empty()) {
        std::vector<std::string> slots;
        for (const auto& info : list()) slots.push_back(info.slot);
        std::string guess = str::closest(slot, slots, 3);
        return Error::make("save_not_found", "no save in slot '" + slot + "'",
                           guess.empty() ? "save_list shows the existing slots" : "did you mean '" + guess + "'?");
    }
    auto bytes = readFile(file);
    if (!bytes) return bytes.error();
    auto doc = parseSave(bytes.value());
    if (!doc) return Error::make(doc.error().code, "slot '" + slot + "': " + doc.error().message, doc.error().hint);
    return doc;
}

Status SaveStore::remove(const std::string& slot) const {
    if (Status s = validateSlotName(slot); !s) return s;
    std::error_code ec;
    bool any = false;
    for (const char* ext : {kPlainExt, kGzipExt}) any = fs::remove(fs::path(dir_) / (slot + ext), ec) || any;
    if (!any) return Error::make("save_not_found", "no save in slot '" + slot + "'", "save_list shows the existing slots");
    return {};
}

std::vector<SaveInfo> SaveStore::list() const {
    std::vector<SaveInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::string slot;
        bool gz = false;
        if (name.size() > std::string(kGzipExt).size() && name.ends_with(kGzipExt)) {
            slot = name.substr(0, name.size() - std::string(kGzipExt).size());
            gz = true;
        } else if (name.size() > std::string(kPlainExt).size() && name.ends_with(kPlainExt)) {
            slot = name.substr(0, name.size() - std::string(kPlainExt).size());
        } else {
            continue;
        }
        if (!validateSlotName(slot)) continue;
        SaveInfo info;
        info.slot = slot;
        info.file = it->path().string();
        info.compressed = gz;
        info.bytes = static_cast<uint64_t>(fs::file_size(it->path(), ec));
        auto bytes = readFile(info.file);
        auto doc = bytes ? parseSave(bytes.value()) : Result<Json>(bytes.error());
        if (!doc) {
            info.error = doc.error().message;
        } else {
            const Json& d = doc.value();
            info.formatVersion = static_cast<int>(d.get("formatVersion").asInt());
            info.version = static_cast<int>(d.get("version").asInt());
            info.playTime = d.get("playTime").asNumber();
            info.scene = d.get("scene").asString();
            info.savedAt = d.get("savedAt").asString();
            info.tick = static_cast<uint64_t>(d.get("tick").asInt());
            info.meta = d.get("meta").isObject() ? d.get("meta") : Json::object();
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SaveInfo& a, const SaveInfo& b) { return a.slot < b.slot; });
    return out;
}

// Exposed to SaveSystem.cpp (same module).
std::string saveTimestamp() { return isoNowUtc(); }

}  // namespace sky::game
