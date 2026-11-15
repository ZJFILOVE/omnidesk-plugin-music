#pragma once

#include "music/lyrics_cache.hpp"
#include "music/types.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace music_plugin {

struct HttpResponse {
    bool ok = false;
    int status = 0;
    std::string content_type;
    std::string body;
    std::string error;
};

class AdamClient {
public:
    explicit AdamClient(LyricsCacheStore* cache = nullptr);
    ~AdamClient();

    void configure(const MusicConfig& cfg);
    void setCacheStore(LyricsCacheStore* cache) { cache_ = cache; }

    // Headless Go engine process lifecycle
    bool ensureEngineRunning(std::string& error);
    void stopManagedEngine();
    bool isManagedByUs() const { return managed_pid_ > 0; }
    bool isPortReachable() const;

    // Developer JWT & Music-User-Token
    std::string getDeveloperToken(bool force_refresh = false);
    void setMediaUserToken(const std::string& mut);
    std::string mediaUserToken() const;
    bool hasDeveloperToken() const;
    bool pushTokensToEngine(std::string& error);

    // Native Apple ID + 2FA DRM authentication
    DrmStatus getDrmStatus();
    bool authenticateDrm(const std::string& email, const std::string& password, DrmStatus& status_out, std::string& error);
    bool submitDrmChallenge(const std::string& code, DrmStatus& status_out, std::string& error);
    bool resendDrmChallenge(DrmStatus& status_out, std::string& error);
    bool logoutDrm(std::string& error);

    // Playback session (ALAC / Hi-Res / AAC / MV)
    PlaybackSessionInfo createPlaybackSession(const std::string& asset_id,
                                              const std::string& storefront,
                                              bool lossless,
                                              bool hires,
                                              bool video,
                                              bool atmos);
    bool deletePlaybackSession(const std::string& session_id);

    // Metadata & native lyrics
    bool fetchMetadata(const std::string& asset_id, const std::string& storefront, MusicTrack& out, std::string& error);
    bool fetchNativeLyrics(const std::string& asset_id, const std::string& storefront, std::string& lrc_out, std::string& error);

    // Library (local SQLite playlists & tracks)
    bool fetchLibraryStatus(nlohmann::json& out, std::string& error);
    std::vector<ContainerItem> fetchLibraryPlaylists(std::string& error);
    std::vector<MusicTrack> fetchLibraryPlaylistTracks(const std::string& playlist_id, std::string& error);
    std::vector<MusicTrack> fetchLibraryAlbumTracks(const std::string& album_id, std::string& error);
    bool syncLibraryFromApple(nlohmann::json& stats_out, std::string& error);

    // Catalog & Personalised feeds
    FeedOutcome fetchRecommendations(const std::string& kind, int limit = 20);
    bool fetchCatalogAlbum(const std::string& album_id, const std::string& sf,
                           ContainerItem& album_out, std::vector<MusicTrack>& tracks_out, std::string& error);
    bool fetchCatalogPlaylist(const std::string& playlist_id, const std::string& sf,
                              ContainerItem& pl_out, std::vector<MusicTrack>& tracks_out, std::string& error);
    bool fetchCatalogArtist(const std::string& artist_id, const std::string& sf,
                            ContainerItem& artist_out, std::vector<ContainerItem>& albums_out,
                            std::vector<MusicTrack>& top_songs_out, std::string& error);
    FeedOutcome searchCatalog(const std::string& query, const std::string& sf = "",
                              const std::string& types = "songs,albums,playlists,artists,music-videos");

    // Helpers exposed for testing
    static bool isLoopbackPortOwnedByUid(uint16_t port, uint32_t expected_uid);
    static std::string decodeChunkedBody(const std::string& raw);
    static void parseResourceNode(const nlohmann::json& node,
                                  const std::string& default_sf,
                                  std::vector<ContainerItem>& containers,
                                  std::vector<MusicTrack>& tracks);

private:
    HttpResponse engineRequest(const std::string& method,
                               const std::string& path,
                               const std::string& body = "",
                               const std::string& content_type = "application/json",
                               const std::string& accept = "application/json",
                               int timeout_ms = 8000);
    HttpResponse ampApiGet(const std::string& path_and_query, bool require_mut = false);
    std::string fetchDeveloperJwtFromApple();
    std::string effectiveStorefront(const std::string& override_sf = "") const;

    LyricsCacheStore* cache_ = nullptr;
    mutable std::mutex cfg_mutex_;
    MusicConfig cfg_;

    mutable std::mutex token_mutex_;
    std::string dev_token_;
    std::string media_user_token_;
    std::string api_token_env_;

    std::mutex proc_mutex_;
    pid_t managed_pid_ = -1;
};

} // namespace music_plugin
