#pragma once

#include "music/adam_client.hpp"
#include "music/llm_translator.hpp"
#include "music/lrclib_client.hpp"
#include "music/lyrics_cache.hpp"
#include "music/types.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <omnidesk/plugin.hpp>
#include <thread>
#include <unordered_map>
#include <vector>

namespace music_plugin {

class MusicEngine : public std::enable_shared_from_this<MusicEngine> {
public:
    using StateObserver = std::function<void()>;

    explicit MusicEngine(omnidesk::PluginContext& ctx);
    ~MusicEngine();

    void initializeFast();
    void startBackgroundTasks();
    void shutdown();
    bool isAlive() const { return alive_.load(std::memory_order_acquire); }

    void reloadConfig();
    MusicConfig currentConfig() const;

    // Engine & Auth
    DrmStatus refreshDrmStatus(bool ensure_started = true);
    bool authenticateAppleId(const std::string& email, const std::string& password, DrmStatus& out, std::string& error);
    bool submitTwoFactorCode(const std::string& code, DrmStatus& out, std::string& error);
    bool resendTwoFactorCode(DrmStatus& out, std::string& error);
    bool logoutAppleId(std::string& error);
    bool importMediaUserToken(const std::string& mut, std::string& error);
    bool tryImportMutFromChromeCookies(std::string& error);

    // Feeds & Search
    FeedOutcome loadFeed(const std::string& feed_id, const std::string& query = "", bool force_refresh = false);
    bool openContainer(const ContainerItem& container, std::vector<MusicTrack>& tracks_out, std::string& error);
    bool syncLibrary(nlohmann::json& stats_out, std::string& error);

    // Playback & Queue via MPVCore
    void setQueueAndPlay(std::vector<MusicTrack> queue, size_t start_index);
    void playTrack(const MusicTrack& track);
    void playAdjacentTrack(int delta);
    void togglePlayPause();
    void seekSeconds(double sec);
    void setVolumePercent(int vol);
    void setLosslessEnabled(bool enabled);

    // Lyrics & AI
    void setDisplayMode(const std::string& mode);
    std::string displayMode() const;
    LyricBundle getLyricsForTrack(const MusicTrack& track,
                                  bool force_refresh = false,
                                  bool force_translate = false,
                                  uint64_t expected_gen = 0);
    void triggerAsyncLyricsRefresh(bool force_refresh, bool force_translate);
    LyricBundle importLyricsForActiveOrTrack(const MusicTrack* explicit_track,
                                             const std::string& raw_lrc_or_text,
                                             std::string& error);
    PhraseExplanation explainLine(size_t line_index, const std::string& focus_phrase = "");
    std::string exportActiveLrc(const std::string& variant, std::string& error) const;

    // Snapshots & IMediaSource
    bool getActivePlaybackState(MusicTrack& track_out,
                                PlaybackSessionInfo& session_out,
                                LyricBundle& lyrics_out,
                                std::vector<MusicTrack>& queue_out,
                                size_t& queue_index_out) const;
    DrmStatus cachedDrmStatus() const;
    EngineStatusSnapshot statusSnapshot() const;

    std::vector<omnidesk::MediaItem> fetchSdkMediaItems(const std::string& category, const std::string& query);
    std::vector<omnidesk::LyricLine> fetchSdkLyrics(const std::string& item_id);

    uint64_t subscribeState(StateObserver cb);
    void unsubscribeState(uint64_t token);

    // Capability RPCs
    omnidesk::CapabilityResult handleMediaSourceRpc(const nlohmann::json& req);
    omnidesk::CapabilityResult handleLyricsRpc(const nlohmann::json& req);

    AdamClient& adamClient() { return adam_; }
    LyricsCacheStore& cacheStore() { return cache_; }

private:
    void workerLoop();
    void enqueueTask(std::function<void()> task);
    void notifyObservers();
    void startAsyncLyricLoadLocked(const MusicTrack& track, bool force_refresh, bool force_translate);
    void releasePlaybackSessionAsync(const std::string& session_id);
    std::string makeMpvUrlHeaders(std::unordered_map<std::string, std::string>& headers_out) const;

    omnidesk::PluginContext& ctx_;
    std::atomic<bool> alive_{true};
    std::atomic<bool> shutting_down_{false};

    mutable std::mutex cfg_mutex_;
    MusicConfig cfg_;

    LyricsCacheStore cache_;
    LrclibClient lrclib_;
    AdamClient adam_;
    LlmLyricsBridge llm_;

    mutable std::mutex state_mutex_;
    DrmStatus last_drm_;
    std::unordered_map<std::string, FeedOutcome> feed_cache_;
    std::vector<MusicTrack> queue_;
    size_t queue_index_ = 0;
    MusicTrack active_track_;
    bool has_active_track_ = false;
    PlaybackSessionInfo active_session_;
    LyricBundle active_lyrics_;
    uint64_t lyric_generation_ = 0;
    bool was_playing_in_mpv_ = false;
    bool auto_advancing_ = false;

    uint64_t mpv_sub_token_ = 0;

    std::mutex obs_mutex_;
    std::unordered_map<uint64_t, StateObserver> observers_;
    uint64_t next_obs_token_ = 1;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<std::function<void()>> worker_tasks_;
    std::thread worker_thread_;
};

} // namespace music_plugin
