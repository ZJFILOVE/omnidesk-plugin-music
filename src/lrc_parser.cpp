#include "music/lrc_parser.hpp"
#include "music/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>

namespace music_plugin {
namespace {

constexpr size_t kMaxLyricsBytes = 1024 * 1024;

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool nextUtf8Codepoint(const std::string& s, size_t& pos, uint32_t& cp) {
    if (pos >= s.size()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[pos]);
    if (c0 < 0x80) {
        cp = c0;
        pos += 1;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && pos + 1 < s.size()) {
        cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3Fu);
        pos += 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0 && pos + 2 < s.size()) {
        cp = ((c0 & 0x0Fu) << 12) |
             ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(s[pos + 2]) & 0x3Fu);
        pos += 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0 && pos + 3 < s.size()) {
        cp = ((c0 & 0x07u) << 18) |
             ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 12) |
             ((static_cast<unsigned char>(s[pos + 2]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(s[pos + 3]) & 0x3Fu);
        pos += 4;
        return true;
    }
    cp = c0;
    pos += 1;
    return true;
}

bool isHanCodepoint(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

bool isKanaOrHangulCodepoint(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x31F0 && cp <= 0x31FF) ||
           (cp >= 0xFF66 && cp <= 0xFF9F) ||
           (cp >= 0x1100 && cp <= 0x11FF) ||
           (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

double parseSrtTimestamp(const std::string& raw) {
    std::string s = trimCopy(raw);
    int hh = 0, mm = 0, ss = 0, ms = 0;
    char sep = ',';
    if (std::sscanf(s.c_str(), "%d:%d:%d%c%d", &hh, &mm, &ss, &sep, &ms) >= 3) {
        return hh * 3600.0 + mm * 60.0 + ss + ms / 1000.0;
    }
    return -1.0;
}

} // namespace

std::string normalizeMetaField(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool prev_space = true;
    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        } else {
            out.push_back(static_cast<char>(std::tolower(ch)));
            prev_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool containsKanaOrHangul(const std::string& text) {
    size_t pos = 0;
    uint32_t cp = 0;
    while (nextUtf8Codepoint(text, pos, cp)) {
        if (isKanaOrHangulCodepoint(cp)) return true;
    }
    return false;
}

bool containsChineseOnlyScript(const std::string& text) {
    bool has_han = false;
    size_t pos = 0;
    uint32_t cp = 0;
    while (nextUtf8Codepoint(text, pos, cp)) {
        if (isKanaOrHangulCodepoint(cp)) return false;
        if (isHanCodepoint(cp)) has_han = true;
    }
    return has_han;
}

bool hasAlphabeticText(const std::string& text) {
    size_t pos = 0;
    uint32_t cp = 0;
    while (nextUtf8Codepoint(text, pos, cp)) {
        if (cp < 0x80) {
            if (std::isalpha(static_cast<unsigned char>(cp))) return true;
        } else if (isHanCodepoint(cp) || isKanaOrHangulCodepoint(cp) ||
                   (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x0400 && cp <= 0x04FF)) {
            return true;
        }
    }
    return false;
}

void assignStableLineIds(std::vector<LyricLineEx>& lines) {
    for (size_t i = 0; i < lines.size(); ++i) {
        nlohmann::json payload = nlohmann::json::array({
            i,
            lines[i].has_time ? nlohmann::json(std::round(lines[i].time_sec * 1000.0)) : nlohmann::json(nullptr),
            lines[i].original,
        });
        lines[i].id = sha256Hex(payload.dump()).substr(0, 20);
    }
}

std::vector<LyricLineEx> parseLyricsText(const std::string& raw_text) {
    if (raw_text.empty() || raw_text.size() > kMaxLyricsBytes) return {};
    std::string text = raw_text;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text = text.substr(3);
    std::string norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            norm.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            norm.push_back(text[i]);
        }
    }

    static const std::regex kStampRe(R"(\[(?:(\d{1,3}):)?(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\])");
    static const std::regex kInlineWordStampRe(R"(<\d+:\d{2}(?:[.:]\d{1,3})?>)");
    static const std::regex kOffsetRe(R"(\[offset:\s*([+-]?\d{1,9})\s*\])", std::regex::icase);
    static const std::regex kMetaTagRe(R"(^\s*\[[A-Za-z]+:.*\]\s*$)");

    double offset_sec = 0.0;
    {
        auto begin = std::sregex_iterator(norm.begin(), norm.end(), kOffsetRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            try { offset_sec = std::stoll((*it)[1].str()) / 1000.0; } catch (...) {}
        }
    }

    const bool has_any_stamp = std::regex_search(norm, kStampRe);
    std::vector<LyricLineEx> raw_lines;

    std::istringstream iss(norm);
    std::string line;
    while (std::getline(iss, line)) {
        std::vector<double> stamps;
        auto begin = std::sregex_iterator(line.begin(), line.end(), kStampRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const std::smatch& m = *it;
            try {
                int hours = m[1].matched ? std::stoi(m[1].str()) : 0;
                int minutes = std::stoi(m[2].str());
                int seconds = std::stoi(m[3].str());
                if (seconds >= 60 || (m[1].matched && minutes >= 60)) continue;
                std::string frac = m[4].matched ? m[4].str() : "0";
                while (frac.size() < 3) frac.push_back('0');
                if (frac.size() > 3) frac.resize(3);
                int millis = std::stoi(frac);
                double t = hours * 3600.0 + minutes * 60.0 + seconds + millis / 1000.0 + offset_sec;
                t = std::round(std::max(0.0, t) * 1000.0) / 1000.0;
                stamps.push_back(t);
            } catch (...) {
            }
        }

        if (has_any_stamp && stamps.empty()) continue;
        if (stamps.empty() && std::regex_match(line, kMetaTagRe)) continue;

        std::string body = std::regex_replace(line, kStampRe, "");
        body = std::regex_replace(body, kInlineWordStampRe, "");
        body = trimCopy(body);

        if (!stamps.empty()) {
            for (double t : stamps) {
                if (body.empty()) continue;
                LyricLineEx item;
                item.time_sec = t;
                item.has_time = true;
                item.original = body;
                raw_lines.push_back(std::move(item));
            }
        } else if (!body.empty()) {
            LyricLineEx item;
            item.time_sec = -1.0;
            item.has_time = false;
            item.original = body;
            raw_lines.push_back(std::move(item));
        }
    }

    if (has_any_stamp) {
        std::stable_sort(raw_lines.begin(), raw_lines.end(),
                         [](const LyricLineEx& a, const LyricLineEx& b) {
                             return a.time_sec < b.time_sec;
                         });
    }

    std::vector<LyricLineEx> merged;
    size_t idx = 0;
    while (idx < raw_lines.size()) {
        size_t end_idx = idx + 1;
        if (raw_lines[idx].has_time) {
            while (end_idx < raw_lines.size() &&
                   raw_lines[end_idx].has_time &&
                   std::abs(raw_lines[end_idx].time_sec - raw_lines[idx].time_sec) < 0.001) {
                ++end_idx;
            }
        }
        if (end_idx - idx > 1) {
            std::vector<std::string> origs;
            std::vector<std::string> zh;
            for (size_t k = idx; k < end_idx; ++k) {
                if (containsChineseOnlyScript(raw_lines[k].original)) zh.push_back(raw_lines[k].original);
                else origs.push_back(raw_lines[k].original);
            }
            if (!origs.empty() && !zh.empty()) {
                LyricLineEx combined;
                combined.has_time = true;
                combined.time_sec = raw_lines[idx].time_sec;
                for (size_t j = 0; j < origs.size(); ++j) {
                    if (j) combined.original += "\n";
                    combined.original += origs[j];
                }
                for (size_t j = 0; j < zh.size(); ++j) {
                    if (j) combined.translation += "\n";
                    combined.translation += zh[j];
                }
                merged.push_back(std::move(combined));
            } else {
                for (size_t k = idx; k < end_idx; ++k) merged.push_back(std::move(raw_lines[k]));
            }
        } else {
            merged.push_back(std::move(raw_lines[idx]));
        }
        idx = end_idx;
    }

    if (!has_any_stamp) {
        std::vector<LyricLineEx> paired;
        size_t i = 0;
        while (i < merged.size()) {
            if (i + 1 < merged.size() &&
                !containsChineseOnlyScript(merged[i].original) &&
                containsChineseOnlyScript(merged[i + 1].original)) {
                LyricLineEx l = std::move(merged[i]);
                l.translation = merged[i + 1].original;
                paired.push_back(std::move(l));
                i += 2;
            } else {
                paired.push_back(std::move(merged[i]));
                ++i;
            }
        }
        merged = std::move(paired);
    }

    static const std::regex kSepRe(R"(^(.+?)\s+(?://|/|\|)\s+(.+)$)");
    for (auto& l : merged) {
        if (!l.translation.empty()) continue;
        std::smatch m;
        if (std::regex_match(l.original, m, kSepRe)) {
            std::string left = trimCopy(m[1].str());
            std::string right = trimCopy(m[2].str());
            if (!containsChineseOnlyScript(left) && containsChineseOnlyScript(right)) {
                l.original = left;
                l.translation = right;
            }
        }
    }

    assignStableLineIds(merged);
    return merged;
}

std::vector<LyricLineEx> parseSrtText(const std::string& raw_srt) {
    if (raw_srt.empty() || raw_srt.size() > kMaxLyricsBytes) return {};
    std::string text = raw_srt;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text = text.substr(3);
    std::string norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            norm.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            norm.push_back(text[i]);
        }
    }

    std::vector<LyricLineEx> out;
    std::istringstream iss(norm);
    std::string line;
    double cur_start = -1.0;
    std::vector<std::string> block_lines;

    auto flush = [&]() {
        if (cur_start >= 0.0 && !block_lines.empty()) {
            std::vector<std::string> origs;
            std::vector<std::string> zh;
            for (const auto& bl : block_lines) {
                if (containsChineseOnlyScript(bl)) zh.push_back(bl);
                else origs.push_back(bl);
            }
            LyricLineEx item;
            item.time_sec = cur_start;
            item.has_time = true;
            if (!origs.empty()) {
                for (size_t i = 0; i < origs.size(); ++i) {
                    if (i) item.original += "\n";
                    item.original += origs[i];
                }
                for (size_t i = 0; i < zh.size(); ++i) {
                    if (i) item.translation += "\n";
                    item.translation += zh[i];
                }
            } else {
                for (size_t i = 0; i < zh.size(); ++i) {
                    if (i) item.original += "\n";
                    item.original += zh[i];
                }
            }
            out.push_back(std::move(item));
        }
        cur_start = -1.0;
        block_lines.clear();
    };

    while (std::getline(iss, line)) {
        std::string t = trimCopy(line);
        if (t.empty()) { flush(); continue; }
        size_t arrow = t.find("-->");
        if (arrow != std::string::npos) {
            cur_start = parseSrtTimestamp(t.substr(0, arrow));
            block_lines.clear();
            continue;
        }
        if (cur_start < 0.0) {
            bool all_digits = !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) {
                return std::isdigit(c);
            });
            if (all_digits) continue;
        }
        if (cur_start >= 0.0) block_lines.push_back(t);
    }
    flush();
    assignStableLineIds(out);
    return out;
}

void attachTranslationLines(std::vector<LyricLineEx>& base_lines,
                            const std::vector<LyricLineEx>& translated_lines) {
    if (base_lines.empty() || translated_lines.empty()) return;

    std::map<long long, std::vector<std::string>> timed_map;
    for (const auto& l : translated_lines) {
        if (!l.has_time) continue;
        long long ms = std::llround(l.time_sec * 1000.0);
        std::string val = !l.translation.empty() ? l.translation : l.original;
        if (!val.empty()) timed_map[ms].push_back(val);
    }

    if (!timed_map.empty()) {
        for (auto& b : base_lines) {
            if (!b.has_time) continue;
            long long ms = std::llround(b.time_sec * 1000.0);
            auto it = timed_map.find(ms);
            if (it == timed_map.end()) continue;
            std::string combined;
            for (const auto& candidate : it->second) {
                if (containsChineseOnlyScript(candidate)) {
                    if (!combined.empty()) combined += "\n";
                    combined += candidate;
                }
            }
            if (!combined.empty()) b.translation = combined;
        }
        return;
    }

    if (base_lines.size() == translated_lines.size()) {
        for (size_t i = 0; i < base_lines.size(); ++i) {
            std::string val = !translated_lines[i].translation.empty()
                                  ? translated_lines[i].translation
                                  : translated_lines[i].original;
            if (containsChineseOnlyScript(val)) base_lines[i].translation = val;
        }
    }
}

std::string composeLrc(const std::vector<LyricLineEx>& lines, const std::string& variant) {
    std::ostringstream oss;
    for (const auto& l : lines) {
        std::string prefix;
        if (l.has_time && l.time_sec >= 0.0) {
            long long total_ms = std::llround(l.time_sec * 1000.0);
            long long mins = total_ms / 60000;
            long long rem_ms = total_ms % 60000;
            long long secs = rem_ms / 1000;
            long long ms = rem_ms % 1000;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "[%02lld:%02lld.%03lld]", mins, secs, ms);
            prefix = buf;
        }

        auto emitMultiline = [&](const std::string& val) {
            std::istringstream ls(val);
            std::string part;
            while (std::getline(ls, part)) oss << prefix << part << '\n';
        };

        if (variant == "original") {
            emitMultiline(l.original);
        } else if (variant == "translation") {
            std::string chosen = !l.translation.empty()
                                     ? l.translation
                                     : (containsChineseOnlyScript(l.original) ? l.original : "");
            if (!chosen.empty() || l.original.empty()) emitMultiline(chosen);
        } else {
            if (!l.original.empty()) emitMultiline(l.original);
            if (!l.translation.empty()) emitMultiline(l.translation);
        }
    }
    return oss.str();
}

std::string computeTrackSignature(const MusicTrack& track) {
    if (!track.id.empty() && track.title.empty()) {
        return sha256Hex("adam_id:" + track.id);
    }
    double rounded_dur = (std::isfinite(track.duration_sec) && track.duration_sec > 0.0)
                             ? std::round(track.duration_sec * 10.0) / 10.0
                             : 0.0;
    nlohmann::json sig = nlohmann::json::array({
        normalizeMetaField(track.title),
        normalizeMetaField(track.artist),
        normalizeMetaField(track.album),
        rounded_dur,
    });
    return sha256Hex(sig.dump());
}

std::vector<omnidesk::LyricLine> toSdkLyricLines(const std::vector<LyricLineEx>& lines) {
    std::vector<omnidesk::LyricLine> out;
    out.reserve(lines.size());
    for (const auto& l : lines) {
        omnidesk::LyricLine item;
        item.time_sec = l.has_time ? l.time_sec : 0.0;
        item.original = l.original;
        item.translation = l.translation;
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace music_plugin
