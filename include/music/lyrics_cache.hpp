#pragma once

#include "music/types.hpp"
#include <optional>
#include <string>
#include <vector>

namespace music_plugin {

class LyricsCacheStore {
public:
    explicit LyricsCacheStore(std::string base_dir = "");

    void setBaseDir(std::string base_dir);
    const std::string& baseDir() const { return base_dir_; }

    std::string trackDir(const std::string& signature) const;
    std::optional<LyricBundle> loadLyrics(const std::string& signature) const;
    bool saveLyrics(LyricBundle& bundle) const;
    std::vector<LyricBundle> listCachedBundles(size_t max_items = 40) const;

    std::optional<PhraseExplanation> loadExplanation(const std::string& signature,
                                                     const std::string& line_id) const;
    bool saveExplanation(const std::string& signature, const PhraseExplanation& note) const;

    std::optional<std::string> loadCachedDevToken() const;
    bool saveCachedDevToken(const std::string& token) const;

    size_t countCachedTracks() const;

    static bool atomicWriteFile(const std::string& path, const std::string& content);
    static std::string readTextAutoEncoding(const std::string& path);

private:
    std::string base_dir_;
};

} // namespace music_plugin
