#pragma once

#include "music/types.hpp"
#include <omnidesk/plugin.hpp>
#include <string>

namespace music_plugin {

std::string expandUserPath(const std::string& path);
std::string resolveEngineBinary(const std::string& configured);
std::string resolveEngineDataDir(const std::string& configured);
MusicConfig loadConfigFromContext(const omnidesk::PluginContext& ctx);
std::string sha256Hex(const std::string& data);

} // namespace music_plugin
