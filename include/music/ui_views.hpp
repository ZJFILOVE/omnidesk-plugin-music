#pragma once

#include "music/music_engine.hpp"
#include <atomic>
#include <borealis.hpp>
#include <memory>
#include <omnidesk/mpv_core.hpp>
#include <string>
#include <vector>

namespace music_plugin {

class MusicWorkspaceView : public brls::Box {
public:
    explicit MusicWorkspaceView(std::shared_ptr<MusicEngine> engine);
    ~MusicWorkspaceView() override;

private:
    enum class SubTab {
        ForYou,
        Recent,
        Library,
        Search,
        NowPlaying,
    };

    void buildUi();
    void setSubTab(SubTab tab);
    void refreshHeader();
    void renderActiveTab();
    void triggerFeedLoad(const std::string& feed_id, const std::string& query = "", bool force = false);
    void openContainerView(const ContainerItem& container);
    void renderFeedBody(const FeedOutcome& outcome);
    void renderLibraryBody();
    void renderNowPlayingBody();
    void updateActiveLyricHighlight(double position_sec);

    void promptAppleIdLogin();
    void promptTwoFactorCode();
    void promptSearch();
    void promptImportLyrics();
    void promptMediaUserToken();
    void explainLyricAtIndex(size_t index, const std::string& focus_phrase = "");

    std::shared_ptr<MusicEngine> engine_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    uint64_t engine_sub_ = 0;
    uint64_t mpv_sub_ = 0;

    SubTab active_tab_ = SubTab::ForYou;
    std::string search_query_;
    FeedOutcome current_feed_;
    bool feed_loading_ = false;

    bool showing_container_detail_ = false;
    ContainerItem active_container_;
    std::vector<MusicTrack> container_tracks_;
    std::vector<ContainerItem> library_playlists_;

    bool show_auth_panel_ = false;
    std::string pending_email_;

    std::string rendered_session_id_;
    std::string rendered_lyrics_sig_;
    size_t rendered_lyrics_count_ = 0;
    bool rendered_lyrics_translated_ = false;
    std::string rendered_display_mode_;
    int active_lyric_idx_ = -1;

    PhraseExplanation last_explanation_;
    bool has_explanation_ = false;

    brls::Label* status_label_ = nullptr;
    brls::Label* drm_pill_label_ = nullptr;
    brls::Box* sub_tabs_box_ = nullptr;
    brls::Box* auth_panel_box_ = nullptr;
    brls::Box* stage_box_ = nullptr;
    omnidesk::MpvStageView* mv_stage_ = nullptr;

    std::vector<brls::Box*> lyric_cards_;
    std::vector<brls::Label*> lyric_orig_labels_;
    std::vector<double> lyric_times_;
};

class MusicCompanionDockView : public brls::Box {
public:
    explicit MusicCompanionDockView(std::shared_ptr<MusicEngine> engine);
    ~MusicCompanionDockView() override;

private:
    void refreshSummary();

    std::shared_ptr<MusicEngine> engine_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    uint64_t engine_sub_ = 0;
    uint64_t mpv_sub_ = 0;

    brls::Label* track_label_ = nullptr;
    brls::Label* lyric_orig_label_ = nullptr;
    brls::Label* lyric_trans_label_ = nullptr;
    brls::Label* note_label_ = nullptr;
};

} // namespace music_plugin
