#include "music/plugin.hpp"
#include <omnidesk/plugin_abi.h>

OMNIDESK_DECLARE_PLUGIN(
    music_plugin::MusicPlugin,
    "plugin-music",
    "Apple Music (Adam FairPlay Lossless)",
    "0.1.0",
    "workspace",
    "media"
)
