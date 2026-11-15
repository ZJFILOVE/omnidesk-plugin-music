#pragma once

#include "music/types.hpp"
#include <chrono>
#include <mutex>
#include <string>

namespace music_plugin {

struct LrclibLookupResult {
    bool ok = false;
    bool found = false;
    bool instrumental = false;
    std::string synced_lyrics;
    std::string plain_lyrics;
    std::string error;
};

class LrclibClient {
public:
    LrclibLookupResult fetchExact(const MusicTrack& track, const std::string& proxy = "");

    static bool isExactMatch(const MusicTrack& query, const nlohmann::json& record);
    static std::string urlEncode(const std::string& value);

private:
    std::mutex net_mutex_;
    std::chrono::steady_clock::time_point next_allowed_{};
};

} // namespace music_plugin
