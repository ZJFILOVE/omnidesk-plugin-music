#pragma once

#include "music/music_engine.hpp"
#include <memory>
#include <omnidesk/plugin.hpp>

namespace music_plugin {

class MusicPlugin final : public omnidesk::IPlugin {
public:
    bool onLoad(omnidesk::PluginContext& ctx) override;
    void onUnload() override;
    nlohmann::json onHealthCheck() const override;
    brls::View* createSettingsView(omnidesk::PluginContext& ctx) override;

private:
    omnidesk::PluginContext* ctx_ = nullptr;
    std::shared_ptr<MusicEngine> engine_;
};

} // namespace music_plugin
