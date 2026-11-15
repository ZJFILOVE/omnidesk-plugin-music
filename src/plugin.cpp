#include "music/plugin.hpp"
#include "music/ui_views.hpp"

#include <omnidesk/domain_registry.hpp>

namespace music_plugin {
namespace {

class AppleMusicMediaSource final : public omnidesk::IMediaSource {
public:
    explicit AppleMusicMediaSource(std::shared_ptr<MusicEngine> engine)
        : engine_(std::move(engine)) {}

    std::string sourceId() const override { return "plugin-music"; }
    std::string displayName() const override { return "Apple Music (FairPlay ALAC)"; }

    std::vector<std::string> categories() const override {
        return {"music", "recommendations", "双语歌词"};
    }

    std::vector<omnidesk::MediaItem> fetchItems(const std::string& category,
                                                const std::string& query = "") override {
        if (!engine_ || !engine_->isAlive()) return {};
        return engine_->fetchSdkMediaItems(category, query);
    }

    std::vector<omnidesk::LyricLine> fetchLyrics(const std::string& item_id) override {
        if (!engine_ || !engine_->isAlive()) return {};
        return engine_->fetchSdkLyrics(item_id);
    }

private:
    std::shared_ptr<MusicEngine> engine_;
};

} // namespace

bool MusicPlugin::onLoad(omnidesk::PluginContext& ctx) {
    ctx_ = &ctx;
    engine_ = std::make_shared<MusicEngine>(ctx);
    engine_->initializeFast();

    ctx.registerCapability(
        "media.source.v1",
        [eng = engine_](const nlohmann::json& req) -> omnidesk::CapabilityResult {
            return eng->handleMediaSourceRpc(req);
        },
        {
            {"name", "Apple Music (Adam FairPlay ALAC)"},
            {"version", "0.1.0"},
            {"actions", {"categories", "feed", "search", "playurl", "lyrics", "drm_status"}},
        },
        95);

    ctx.registerCapability(
        "media.lyrics.v1",
        [eng = engine_](const nlohmann::json& req) -> omnidesk::CapabilityResult {
            return eng->handleLyricsRpc(req);
        },
        {
            {"name", "Apple Music & LRCLIB Bilingual Lyrics"},
            {"version", "0.1.0"},
            {"actions", {"status", "probe", "get", "fetch", "parse", "compose", "export", "import", "translate", "explain", "analyze_line"}},
        },
        100);

    ctx.registerMediaSource(std::make_shared<AppleMusicMediaSource>(engine_));

    omnidesk::WorkspaceTabSpec tab;
    tab.tab_id = "apple_music.workspace";
    tab.plugin_id = ctx.pluginId();
    tab.title = "Apple Music";
    tab.domain = omnidesk::ContentDomain::Media;
    tab.badge = "ALAC";
    tab.create_view = [weak = std::weak_ptr<MusicEngine>(engine_)]() -> brls::View* {
        auto eng = weak.lock();
        if (!eng || !eng->isAlive()) return nullptr;
        return new MusicWorkspaceView(eng);
    };
    ctx.registerWorkspaceTab(std::move(tab));

    omnidesk::DockWidgetSpec dock;
    dock.widget_id = "apple_music.companion";
    dock.plugin_id = ctx.pluginId();
    dock.title = "Apple Music 双语精讲";
    dock.supported_domains = {omnidesk::ContentDomain::Media, omnidesk::ContentDomain::Core};
    dock.create_view = [weak = std::weak_ptr<MusicEngine>(engine_)]() -> brls::View* {
        auto eng = weak.lock();
        if (!eng || !eng->isAlive()) return nullptr;
        return new MusicCompanionDockView(eng);
    };
    ctx.registerDockWidget(std::move(dock));

    ctx.subscribe("entity.media.open", [weak = std::weak_ptr<MusicEngine>(engine_)](
                                           const std::string& /*topic*/,
                                           const nlohmann::json& payload) {
        auto eng = weak.lock();
        if (!eng || !eng->isAlive() || !payload.is_object()) return;
        if (payload.value("origin", "") != "host_workbench") return;
        if (payload.value("source_id", "") != "plugin-music") return;
        std::string id = payload.value("id", "");
        if (id.empty() || id.rfind("cache:", 0) == 0) return;
        MusicTrack tr;
        tr.id = id;
        tr.title = payload.value("title", id);
        eng->playTrack(tr);
    });

    engine_->startBackgroundTasks();
    ctx.logInfo("Loaded plugin-music (Apple Music Adam FairPlay ALAC + Bilingual Lyrics)");
    return true;
}

void MusicPlugin::onUnload() {
    if (engine_) {
        engine_->shutdown();
    }
    if (ctx_) {
        const std::string pid = ctx_->pluginId();
        ctx_->logInfo("Unloading plugin-music");
        ctx_->bus().unsubscribeOwner(pid);
        ctx_->domains().unregisterPlugin(pid);
        ctx_->capabilities().unregisterPlugin(pid);
        if (ctx_->workers()) {
            ctx_->workers()->stopByPlugin(pid);
        }
    }
    engine_.reset();
    ctx_ = nullptr;
}

nlohmann::json MusicPlugin::onHealthCheck() const {
    if (!engine_) {
        return {{"ok", false}, {"plugin_id", "plugin-music"}, {"loaded", false}};
    }
    EngineStatusSnapshot st = engine_->statusSnapshot();
    return {
        {"ok", true},
        {"plugin_id", "plugin-music"},
        {"engine_reachable", st.engine_reachable},
        {"engine_managed_by_plugin", st.engine_managed_by_plugin},
        {"auth_state", st.drm.auth_state},
        {"fairplay_state", st.drm.fairplay_state},
        {"cap_alac", st.drm.cap_alac},
        {"cap_hi_res", st.drm.cap_hi_res},
        {"has_dev_token", st.has_dev_token},
        {"has_mut", st.has_mut},
        {"llm_translate_available", st.llm_translate_available},
        {"llm_chat_available", st.llm_chat_available},
        {"cached_lyrics_count", st.cached_lyrics_count},
        {"active_track_id", st.active_track_id},
        {"active_track_title", st.active_track_title},
        {"active_quality", st.active_quality},
    };
}

brls::View* MusicPlugin::createSettingsView(omnidesk::PluginContext& /*ctx*/) {
    return nullptr;
}

} // namespace music_plugin
