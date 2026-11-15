#include "music/lyrics_cache.hpp"
#include "music/lrc_parser.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace music_plugin {
namespace {

constexpr size_t kMaxCacheFileBytes = 2 * 1024 * 1024;

std::string utf16ToUtf8(const std::string& raw) {
    if (raw.size() < 2) return "";
    bool little = (static_cast<unsigned char>(raw[0]) == 0xFF &&
                   static_cast<unsigned char>(raw[1]) == 0xFE);
    bool big = (static_cast<unsigned char>(raw[0]) == 0xFE &&
                static_cast<unsigned char>(raw[1]) == 0xFF);
    if (!little && !big) return "";
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        uint16_t u = little ? (uint16_t(static_cast<unsigned char>(raw[i])) |
                               (uint16_t(static_cast<unsigned char>(raw[i + 1])) << 8))
                            : ((uint16_t(static_cast<unsigned char>(raw[i])) << 8) |
                               uint16_t(static_cast<unsigned char>(raw[i + 1])));
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

} // namespace

LyricsCacheStore::LyricsCacheStore(std::string base_dir) : base_dir_(std::move(base_dir)) {}

void LyricsCacheStore::setBaseDir(std::string base_dir) {
    base_dir_ = std::move(base_dir);
}

std::string LyricsCacheStore::trackDir(const std::string& signature) const {
    if (base_dir_.empty() || signature.empty()) return "";
    return (fs::path(base_dir_) / "lyrics" / signature).string();
}

bool LyricsCacheStore::atomicWriteFile(const std::string& path, const std::string& content) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) return false;
    }
    std::string tmpl = (p.parent_path() / ("." + p.filename().string() + ".XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) return false;

    size_t written = 0;
    while (written < content.size()) {
        ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n <= 0) {
            ::close(fd);
            ::unlink(buf.data());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    ::fchmod(fd, 0600);
    ::fsync(fd);
    ::close(fd);

    fs::rename(buf.data(), p, ec);
    if (ec) {
        ::unlink(buf.data());
        return false;
    }
    return true;
}

std::string LyricsCacheStore::readTextAutoEncoding(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::string raw(kMaxCacheFileBytes + 1, '\0');
    ifs.read(&raw[0], static_cast<std::streamsize>(raw.size()));
    std::streamsize got = ifs.gcount();
    if (got <= 0 || static_cast<size_t>(got) > kMaxCacheFileBytes) return "";
    raw.resize(static_cast<size_t>(got));

    if (raw.size() >= 2 &&
        ((static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) ||
         (static_cast<unsigned char>(raw[0]) == 0xFE && static_cast<unsigned char>(raw[1]) == 0xFF))) {
        return utf16ToUtf8(raw);
    }
    if (raw.rfind("\xEF\xBB\xBF", 0) == 0) return raw.substr(3);
    return raw;
}

std::optional<LyricBundle> LyricsCacheStore::loadLyrics(const std::string& signature) const {
    std::string dir = trackDir(signature);
    if (dir.empty()) return std::nullopt;

    std::string text = readTextAutoEncoding((fs::path(dir) / "lyrics.json").string());
    if (text.empty()) return std::nullopt;

    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1) return std::nullopt;
    if (!j.contains("lines") || !j["lines"].is_array() || j["lines"].size() > 20000) {
        return std::nullopt;
    }

    LyricBundle bundle;
    bundle.signature = signature;
    bundle.source = j.value("source", "cache");
    bundle.instrumental = j.value("instrumental", false);
    bundle.cache_hit = true;

    if (j.contains("track") && j["track"].is_object()) {
        const auto& t = j["track"];
        bundle.track.id = t.value("id", "");
        bundle.track.title = t.value("title", "");
        bundle.track.artist = t.value("artist", "");
        bundle.track.album = t.value("album", "");
        bundle.track.cover_url = t.value("cover_url", "");
        bundle.track.storefront = t.value("storefront", "");
        bundle.track.duration_sec = t.value("duration", 0.0);
    }

    for (const auto& row : j["lines"]) {
        if (!row.is_object() || !row.contains("text") || !row["text"].is_string()) {
            return std::nullopt;
        }
        LyricLineEx l;
        if (row.contains("start") && row["start"].is_number()) {
            double st = row["start"].get<double>();
            if (!std::isfinite(st) || st < 0.0) return std::nullopt;
            l.time_sec = st;
            l.has_time = true;
        }
        l.original = row["text"].get<std::string>();
        l.translation = row.value("translation", "");
        bundle.lines.push_back(std::move(l));
    }

    assignStableLineIds(bundle.lines);
    bundle.synced = !bundle.lines.empty() &&
                    std::all_of(bundle.lines.begin(), bundle.lines.end(),
                                [](const LyricLineEx& l) { return l.has_time; });
    bundle.translated = std::any_of(bundle.lines.begin(), bundle.lines.end(),
                                    [](const LyricLineEx& l) { return !l.translation.empty(); });
    bundle.original_lrc_path = (fs::path(dir) / "original.lrc").string();
    bundle.zh_lrc_path = (fs::path(dir) / "zh-CN.lrc").string();
    bundle.bilingual_lrc_path = (fs::path(dir) / "bilingual.lrc").string();
    return bundle;
}

bool LyricsCacheStore::saveLyrics(LyricBundle& bundle) const {
    if (bundle.signature.empty()) bundle.signature = computeTrackSignature(bundle.track);
    std::string dir = trackDir(bundle.signature);
    if (dir.empty()) return false;

    bundle.original_lrc_path = (fs::path(dir) / "original.lrc").string();
    bundle.zh_lrc_path = (fs::path(dir) / "zh-CN.lrc").string();
    bundle.bilingual_lrc_path = (fs::path(dir) / "bilingual.lrc").string();

    if (!atomicWriteFile(bundle.original_lrc_path, composeLrc(bundle.lines, "original"))) return false;
    if (!atomicWriteFile(bundle.zh_lrc_path, composeLrc(bundle.lines, "translation"))) return false;
    if (!atomicWriteFile(bundle.bilingual_lrc_path, composeLrc(bundle.lines, "bilingual"))) return false;

    nlohmann::json lines_arr = nlohmann::json::array();
    for (const auto& l : bundle.lines) {
        lines_arr.push_back({
            {"id", l.id},
            {"start", l.has_time ? nlohmann::json(l.time_sec) : nlohmann::json(nullptr)},
            {"text", l.original},
            {"translation", l.translation},
        });
    }

    nlohmann::json record = {
        {"version", 1},
        {"source", bundle.source},
        {"instrumental", bundle.instrumental},
        {"track", {
            {"id", bundle.track.id},
            {"title", bundle.track.title},
            {"artist", bundle.track.artist},
            {"album", bundle.track.album},
            {"cover_url", bundle.track.cover_url},
            {"storefront", bundle.track.storefront},
            {"duration", bundle.track.duration_sec},
        }},
        {"lines", std::move(lines_arr)},
    };
    return atomicWriteFile((fs::path(dir) / "lyrics.json").string(), record.dump(2) + "\n");
}

std::vector<LyricBundle> LyricsCacheStore::listCachedBundles(size_t max_items) const {
    std::vector<LyricBundle> out;
    if (base_dir_.empty()) return out;
    std::error_code ec;
    fs::path p = fs::path(base_dir_) / "lyrics";
    if (!fs::is_directory(p, ec)) return out;

    struct Entry { fs::file_time_type mtime; std::string sig; };
    std::vector<Entry> entries;
    for (const auto& entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory(ec)) continue;
        fs::path json_p = entry.path() / "lyrics.json";
        if (!fs::exists(json_p, ec)) continue;
        entries.push_back({fs::last_write_time(json_p, ec), entry.path().filename().string()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.mtime > b.mtime;
    });
    for (const auto& e : entries) {
        if (out.size() >= max_items) break;
        if (auto b = loadLyrics(e.sig)) out.push_back(std::move(*b));
    }
    return out;
}

std::optional<PhraseExplanation> LyricsCacheStore::loadExplanation(const std::string& signature,
                                                                   const std::string& line_id) const {
    std::string dir = trackDir(signature);
    if (dir.empty() || line_id.empty()) return std::nullopt;
    std::string raw = readTextAutoEncoding((fs::path(dir) / "notes.json").string());
    if (raw.empty()) return std::nullopt;
    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (!j.is_object() || !j.contains(line_id) || !j[line_id].is_object()) return std::nullopt;
    const auto& item = j[line_id];
    PhraseExplanation note;
    note.line_id = line_id;
    note.original = item.value("original", "");
    note.translation = item.value("translation", "");
    note.focus_phrase = item.value("focus_phrase", "");
    note.explanation = item.value("explanation", "");
    note.degraded = item.value("degraded", false);
    note.provider = item.value("provider", "");
    if (note.explanation.empty()) return std::nullopt;
    return note;
}

bool LyricsCacheStore::saveExplanation(const std::string& signature, const PhraseExplanation& note) const {
    std::string dir = trackDir(signature);
    if (dir.empty() || note.line_id.empty()) return false;
    fs::path p = fs::path(dir) / "notes.json";
    nlohmann::json root = nlohmann::json::object();
    std::string raw = readTextAutoEncoding(p.string());
    if (!raw.empty()) {
        auto parsed = nlohmann::json::parse(raw, nullptr, false);
        if (parsed.is_object()) root = std::move(parsed);
    }
    root[note.line_id] = {
        {"original", note.original},
        {"translation", note.translation},
        {"focus_phrase", note.focus_phrase},
        {"explanation", note.explanation},
        {"degraded", note.degraded},
        {"provider", note.provider},
    };
    return atomicWriteFile(p.string(), root.dump(2) + "\n");
}

std::optional<std::string> LyricsCacheStore::loadCachedDevToken() const {
    if (base_dir_.empty()) return std::nullopt;
    std::string raw = readTextAutoEncoding((fs::path(base_dir_) / "dev_token.json").string());
    if (raw.empty()) return std::nullopt;
    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    int64_t saved_at = j.value("saved_at", int64_t{0});
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    if (now - saved_at > 12 * 3600) return std::nullopt;
    std::string tok = j.value("token", "");
    if (tok.empty()) return std::nullopt;
    return tok;
}

bool LyricsCacheStore::saveCachedDevToken(const std::string& token) const {
    if (base_dir_.empty() || token.empty()) return false;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json j = {{"token", token}, {"saved_at", now}};
    return atomicWriteFile((fs::path(base_dir_) / "dev_token.json").string(), j.dump() + "\n");
}

size_t LyricsCacheStore::countCachedTracks() const {
    if (base_dir_.empty()) return 0;
    std::error_code ec;
    fs::path p = fs::path(base_dir_) / "lyrics";
    if (!fs::is_directory(p, ec)) return 0;
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec) && fs::exists(entry.path() / "lyrics.json", ec)) ++count;
    }
    return count;
}

} // namespace music_plugin
