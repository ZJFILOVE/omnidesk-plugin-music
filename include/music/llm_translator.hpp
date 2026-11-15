#pragma once

#include "music/types.hpp"
#include <omnidesk/capabilities.hpp>
#include <string>
#include <vector>

namespace music_plugin {

class LlmLyricsBridge {
public:
    explicit LlmLyricsBridge(omnidesk::CapabilityRegistry& caps);

    bool hasTranslator() const;
    bool hasChat() const;
    bool hasAgent() const;

    bool translateMissingLines(const MusicTrack& track,
                               std::vector<LyricLineEx>& lines,
                               const std::string& target_language,
                               int batch_size,
                               std::string& error,
                               bool* degraded_out = nullptr) const;

    PhraseExplanation explainLyricLine(const MusicTrack& track,
                                       const LyricLineEx& line,
                                       const std::string& prev_line,
                                       const std::string& next_line,
                                       const std::string& focus_phrase,
                                       const std::string& target_language) const;

    static bool parseTranslatedJsonBatch(const std::string& reply,
                                         const std::vector<std::pair<std::string, std::string>>& expected,
                                         std::vector<std::string>& out_translations);

private:
    omnidesk::CapabilityRegistry& caps_;
};

} // namespace music_plugin
