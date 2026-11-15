#pragma once

#include "music/types.hpp"
#include <omnidesk/types.hpp>
#include <string>
#include <vector>

namespace music_plugin {

std::string normalizeMetaField(const std::string& value);
bool containsChineseOnlyScript(const std::string& text);
bool containsKanaOrHangul(const std::string& text);
bool hasAlphabeticText(const std::string& text);

std::vector<LyricLineEx> parseLyricsText(const std::string& raw_text);
std::vector<LyricLineEx> parseSrtText(const std::string& raw_srt);
void attachTranslationLines(std::vector<LyricLineEx>& base_lines,
                            const std::vector<LyricLineEx>& translated_lines);
void assignStableLineIds(std::vector<LyricLineEx>& lines);

std::string composeLrc(const std::vector<LyricLineEx>& lines, const std::string& variant = "bilingual");
std::string computeTrackSignature(const MusicTrack& track);
std::vector<omnidesk::LyricLine> toSdkLyricLines(const std::vector<LyricLineEx>& lines);

} // namespace music_plugin
