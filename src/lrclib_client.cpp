#include "music/lrclib_client.hpp"
#include "music/lrc_parser.hpp"

#include <cmath>
#include <curl/curl.h>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace music_plugin {
namespace {

constexpr size_t kMaxRespBytes = 1024 * 1024;

void ensureCurlInit() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    if (out->size() + total > kMaxRespBytes) return 0;
    out->append(ptr, total);
    return total;
}

} // namespace

std::string LrclibClient::urlEncode(const std::string& value) {
    std::ostringstream oss;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << static_cast<char>(c);
        } else if (c == ' ') {
            oss << "%20";
        } else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(c) << std::nouppercase << std::dec;
        }
    }
    return oss.str();
}

bool LrclibClient::isExactMatch(const MusicTrack& query, const nlohmann::json& record) {
    if (!record.is_object()) return false;
    if (!record.contains("duration") || !record["duration"].is_number()) return false;
    double rec_dur = record["duration"].get<double>();
    if (!std::isfinite(rec_dur) || std::abs(rec_dur - query.duration_sec) > 2.0) return false;
    std::string rec_title = normalizeMetaField(record.value("trackName", ""));
    std::string rec_artist = normalizeMetaField(record.value("artistName", ""));
    if (rec_title.empty() || rec_artist.empty()) return false;
    if (rec_title != normalizeMetaField(query.title)) return false;
    if (rec_artist != normalizeMetaField(query.artist)) return false;

    std::string q_album = normalizeMetaField(query.album);
    std::string r_album = normalizeMetaField(record.value("albumName", ""));
    if (!q_album.empty() && !r_album.empty() && q_album != r_album) return false;
    return true;
}

LrclibLookupResult LrclibClient::fetchExact(const MusicTrack& track, const std::string& proxy) {
    LrclibLookupResult res;
    if (track.title.empty() || track.artist.empty() ||
        track.duration_sec < 1.0 || track.duration_sec > 3600.0) {
        res.ok = true;
        res.found = false;
        return res;
    }

    ensureCurlInit();
    {
        std::lock_guard<std::mutex> lk(net_mutex_);
        auto now = std::chrono::steady_clock::now();
        if (next_allowed_ > now) {
            auto wait = next_allowed_ - now;
            if (wait > std::chrono::milliseconds(1000)) {
                res.error = "LRCLIB 请求过于频繁，请稍后重试";
                return res;
            }
            std::this_thread::sleep_for(wait);
        }
        next_allowed_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    }

    std::ostringstream url;
    url << "https://lrclib.net/api/get?track_name=" << urlEncode(track.title)
        << "&artist_name=" << urlEncode(track.artist)
        << "&duration=" << std::llround(track.duration_sec);
    if (!track.album.empty()) url << "&album_name=" << urlEncode(track.album);

    CURL* curl = curl_easy_init();
    if (!curl) {
        res.error = "curl_easy_init failed";
        return res;
    }

    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: OmniDeskMusic/0.1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        res.error = "暂时无法读取在线歌词，请检查网络后重试";
        return res;
    }
    if (status == 404) {
        res.ok = true;
        res.found = false;
        return res;
    }
    if (status < 200 || status >= 300) {
        res.error = "LRCLIB 返回状态异常 (" + std::to_string(status) + ")";
        return res;
    }

    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        res.error = "LRCLIB 响应 JSON 无效";
        return res;
    }
    if (!isExactMatch(track, j)) {
        res.ok = true;
        res.found = false;
        return res;
    }

    res.ok = true;
    res.found = true;
    res.instrumental = j.value("instrumental", false);
    if (j.contains("syncedLyrics") && j["syncedLyrics"].is_string()) {
        res.synced_lyrics = j["syncedLyrics"].get<std::string>();
    }
    if (j.contains("plainLyrics") && j["plainLyrics"].is_string()) {
        res.plain_lyrics = j["plainLyrics"].get<std::string>();
    }
    return res;
}

} // namespace music_plugin
