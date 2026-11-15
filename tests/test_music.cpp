#include "music/adam_client.hpp"
#include "music/config.hpp"
#include "music/llm_translator.hpp"
#include "music/lrc_parser.hpp"
#include "music/lrclib_client.hpp"
#include "music/lyrics_cache.hpp"
#include "music/music_engine.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <omnidesk/capabilities.hpp>
#include <omnidesk/config_store.hpp>
#include <omnidesk/domain_registry.hpp>
#include <omnidesk/entity_bus.hpp>
#include <omnidesk/plugin.hpp>
#include <omnidesk/plugin_abi.h>
#include <poll.h>
#include <sstream>
#include <csignal>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

#define REQUIRE(cond)                                                                      \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n"; \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

namespace {

class MockAdamTlsServer {
public:
    MockAdamTlsServer() = default;
    ~MockAdamTlsServer() { stop(); }

    bool start() {
        sandbox_blocked_ = false;
        EVP_PKEY* pkey = EVP_RSA_gen(2048);
        if (!pkey) { std::cerr << "EVP_RSA_gen failed\n"; return false; }

        X509* x509 = X509_new();
        X509_set_version(x509, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509), -60);
        X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
        X509_set_pubkey(x509, pkey);
        X509_NAME* name = X509_NAME_new();
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
        X509_set_subject_name(x509, name);
        X509_set_issuer_name(x509, name);
        X509_NAME_free(name);

        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name,
                                                  const_cast<char*>("IP:127.0.0.1"));
        if (ext) {
            X509_add_ext(x509, ext, -1);
            X509_EXTENSION_free(ext);
        }
        X509_sign(x509, pkey, EVP_sha256());

        ssl_ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ssl_ctx_) { std::cerr << "SSL_CTX_new failed\n"; X509_free(x509); EVP_PKEY_free(pkey); return false; }
        SSL_CTX_use_certificate(ssl_ctx_, x509);
        SSL_CTX_use_PrivateKey(ssl_ctx_, pkey);
        X509_free(x509);
        EVP_PKEY_free(pkey);

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            if (errno == EPERM || errno == EACCES) sandbox_blocked_ = true;
            std::perror("socket");
            return false;
        }
        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 8) != 0) { std::perror("bind/listen");
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        running_ = true;
        th_ = std::thread([this]() { serveLoop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (th_.joinable()) th_.join();
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    int port() const { return port_; }
    bool sandboxBlocked() const { return sandbox_blocked_; }

private:
    void serveLoop() {
        while (running_) {
            struct pollfd pfd{listen_fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 100) <= 0) continue;
            int cfd = ::accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0) continue;
            SSL* ssl = SSL_new(ssl_ctx_);
            SSL_set_fd(ssl, cfd);
            if (SSL_accept(ssl) == 1) {
                char buf[8192];
                int n = SSL_read(ssl, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string req(buf, static_cast<size_t>(n));
                    std::istringstream iss(req);
                    std::string method, path;
                    iss >> method >> path;

                    int code = 404;
                    std::string ctype = "application/json";
                    std::string body = R"({"error":"not found"})";

                    if (path == "/api/v1/drm/status") {
                        code = 200;
                        body = nlohmann::json({
                            {"available", true},
                            {"backend", {{"selected", "adamplay"}}},
                            {"state", {
                                {"manager", "ready"},
                                {"process", "running"},
                                {"authentication", auth_state_},
                                {"fairplay", fairplay_state_},
                                {"session", "valid"},
                            }},
                            {"capabilities", {
                                {"cbcs", true}, {"alac", true}, {"hiRes", true}, {"atmos", false}
                            }},
                        }).dump();
                    } else if (method == "POST" && path == "/api/v1/drm/authenticate") {
                        auth_state_ = "challenging";
                        code = 202;
                        body = nlohmann::json({
                            {"available", true},
                            {"state", {{"authentication", auth_state_}, {"fairplay", "unknown"}}},
                            {"challenge", {{"prompt", "Enter 6-digit code"}}},
                        }).dump();
                    } else if (method == "POST" && path == "/api/v1/drm/challenge") {
                        auth_state_ = "logged_in";
                        fairplay_state_ = "ready";
                        code = 200;
                        body = nlohmann::json({
                            {"available", true},
                            {"state", {{"authentication", auth_state_}, {"fairplay", fairplay_state_}}},
                            {"capabilities", {{"cbcs", true}, {"alac", true}, {"hiRes", true}}},
                        }).dump();
                    } else if (method == "POST" && path == "/api/v1/library/token") {
                        code = 200;
                        body = R"({"ok":true})";
                    } else if (method == "POST" && path == "/api/v1/playback") {
                        code = 201;
                        body = nlohmann::json({
                            {"sessionId", "sess-mock-1"},
                            {"assetId", "1440857781"},
                            {"storefront", "us"},
                            {"type", "song"},
                            {"codec", "alac"},
                            {"sampleRate", 192000},
                            {"bitDepth", 24},
                            {"channelCount", 2},
                            {"title", "Midnight City"},
                            {"artistName", "M83"},
                            {"albumName", "Hurry Up, We're Dreaming"},
                            {"durationMs", 243000},
                            {"streams", {{"audio", "/api/v1/playback/sess-mock-1/audio"}}},
                        }).dump();
                    } else if (method == "DELETE" && path == "/api/v1/playback/sess-mock-1") {
                        code = 200;
                        body = R"({"ok":true})";
                    } else if (path.rfind("/api/v1/lyrics/1440857781", 0) == 0) {
                        code = 200;
                        ctype = "text/plain; charset=utf-8";
                        body = "[00:12.00]Waiting in a car\n[00:18.50]Waiting for a ride in the dark\n";
                    } else if (path == "/api/v1/library/playlists") {
                        code = 200;
                        body = R"({"playlists":[{"lid":"p.123","name":"Lossless Favorites","trackCount":12}]})";
                    } else if (path.rfind("/api/v1/recommendations", 0) == 0) {
                        code = 200;
                        body = nlohmann::json({
                            {"data", {{
                                {"id", "rec-1"},
                                {"type", "personal-recommendation"},
                                {"relationships", {
                                    {"contents", {
                                        {"data", {{
                                            {"id", "1440857781"},
                                            {"type", "songs"},
                                            {"attributes", {
                                                {"name", "Midnight City"},
                                                {"artistName", "M83"},
                                                {"albumName", "Hurry Up, We're Dreaming"},
                                                {"durationInMillis", 243000},
                                                {"hasLyrics", true},
                                                {"audioTraits", {"lossless", "hi-res-lossless"}},
                                                {"artwork", {{"url", "https://example.com/{w}x{h}.jpg"}}},
                                            }},
                                        }}}
                                    }}
                                }}
                            }}}
                        }).dump();
                    }

                    std::ostringstream resp;
                    resp << "HTTP/1.1 " << code << " OK\r\n"
                         << "Content-Type: " << ctype << "\r\n"
                         << "Content-Length: " << body.size() << "\r\n"
                         << "Connection: close\r\n\r\n"
                         << body;
                    std::string wire = resp.str();
                    SSL_write(ssl, wire.data(), static_cast<int>(wire.size()));
                }
            }
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ::close(cfd);
        }
    }

    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    int port_ = 0;
    bool sandbox_blocked_ = false;
    SSL_CTX* ssl_ctx_ = nullptr;
    std::thread th_;
    std::string auth_state_ = "logged_out";
    std::string fairplay_state_ = "unknown";
};

int testLrcAndBilingualParser() {
    std::string lrc =
        "[ti:Test]\n"
        "[offset:500]\n"
        "[00:10.00]Under silver moonlight\n"
        "[00:10.00]在银色月光下\n"
        "[00:20.250][00:40.000]<00:20.50>Echoes fade away\n";
    auto lines = music_plugin::parseLyricsText(lrc);
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0].has_time);
    REQUIRE(std::abs(lines[0].time_sec - 10.5) < 0.01);
    REQUIRE(lines[0].original == "Under silver moonlight");
    REQUIRE(lines[0].translation == "在银色月光下");
    REQUIRE(lines[1].original == "Echoes fade away");
    REQUIRE(std::abs(lines[1].time_sec - 20.75) < 0.01);
    REQUIRE(std::abs(lines[2].time_sec - 40.5) < 0.01);

    std::string composed = music_plugin::composeLrc(lines, "bilingual");
    REQUIRE(composed.find("[00:10.500]Under silver moonlight") != std::string::npos);
    REQUIRE(composed.find("[00:10.500]在银色月光下") != std::string::npos);

    REQUIRE(music_plugin::containsChineseOnlyScript("夜风吹过街角"));
    REQUIRE(!music_plugin::containsChineseOnlyScript("東京タワー"));
    return 0;
}

int testLrclibExactMatch() {
    music_plugin::MusicTrack tr;
    tr.title = "Midnight City";
    tr.artist = "M83";
    tr.album = "Hurry Up, We're Dreaming";
    tr.duration_sec = 243.0;

    nlohmann::json good = {
        {"trackName", "midnight city"},
        {"artistName", "M83"},
        {"albumName", "Hurry Up, We're Dreaming"},
        {"duration", 244.0},
    };
    REQUIRE(music_plugin::LrclibClient::isExactMatch(tr, good));

    nlohmann::json bad_dur = good;
    bad_dur["duration"] = 250.0;
    REQUIRE(!music_plugin::LrclibClient::isExactMatch(tr, bad_dur));
    return 0;
}

int testMockAdamAndLlmPipeline(const fs::path& tmp) {
    MockAdamTlsServer mock;
    if (!mock.start()) {
        if (mock.sandboxBlocked()) {
            std::cout << "[SKIP] MockAdamTlsServer skipped in seccomp sandbox\n";
            return 0;
        }
        REQUIRE(false);
    }

    omnidesk::CapabilityRegistry caps;
    omnidesk::EntityBus bus;
    omnidesk::DomainRegistry domains;
    omnidesk::ConfigStore cfg_store((tmp / "cfg").string());
    omnidesk::SecretVault vault((tmp / "cfg").string());

    cfg_store.savePluginConfig("plugin-music", {
        {"engine_port", mock.port()},
        {"auto_launch_engine", false},
        {"enable_lrclib", false},
        {"auto_translate_lyrics", true},
        {"storefront", "us"},
    });

    // Mock llm.chat.v1 provider
    caps.registerCapability("llm.chat.v1", "plugin-mock-llm",
        [](const nlohmann::json& req) -> omnidesk::CapabilityResult {
            omnidesk::CapabilityResult r;
            r.ok = true;
            std::string prompt = req.value("prompt", "");
            if (prompt.find("精讲") != std::string::npos) {
                r.data = {{"reply", "1. 意译：在车里等待。\n2. 词组：waiting for 表示等待。"}};
                return r;
            }
            size_t arr_pos = prompt.find('[');
            auto arr = nlohmann::json::parse(prompt.substr(arr_pos), nullptr, false);
            nlohmann::json out = nlohmann::json::array();
            for (const auto& item : arr) {
                std::string txt = item.value("text", "");
                std::string tr = (txt.find("car") != std::string::npos)
                                     ? "在车里静静等待"
                                     : "在黑夜中等候一程";
                out.push_back({{"id", item["id"]}, {"text", tr}});
            }
            r.data = {{"reply", out.dump()}};
            return r;
        });

    omnidesk::PluginContext ctx("plugin-music", tmp.string(), (tmp / "data").string(),
                                caps, bus, domains, cfg_store, vault);
    music_plugin::MusicEngine engine(ctx);
    engine.initializeFast();
    engine.adamClient().setMediaUserToken("mock-mut");

    auto drm0 = engine.refreshDrmStatus(false);
    REQUIRE(drm0.engine_reachable);
    REQUIRE(drm0.auth_state == "logged_out");

    std::string err;
    music_plugin::DrmStatus drm1;
    REQUIRE(engine.authenticateAppleId("user@example.com", "secret", drm1, err));
    REQUIRE(drm1.auth_state == "challenging");

    music_plugin::DrmStatus drm2;
    REQUIRE(engine.submitTwoFactorCode("123456", drm2, err));
    REQUIRE(drm2.auth_state == "logged_in");
    REQUIRE(drm2.fairplay_state == "ready");
    REQUIRE(drm2.cap_hi_res);

    auto feed = engine.loadFeed("recommendations", "", true);
    REQUIRE(feed.ok);
    REQUIRE(feed.tracks.size() == 1);
    REQUIRE(feed.tracks[0].id == "1440857781");
    REQUIRE(feed.tracks[0].is_hires);

    auto pl_feed = engine.loadFeed("library-playlists", "", true);
    REQUIRE(pl_feed.ok);
    REQUIRE(pl_feed.containers.size() == 1);
    REQUIRE(pl_feed.containers[0].name == "Lossless Favorites");

    auto sess = engine.adamClient().createPlaybackSession("1440857781", "us", true, true, false, false);
    REQUIRE(sess.ok);
    REQUIRE(sess.codec == "alac");
    REQUIRE(sess.sample_rate == 192000);
    REQUIRE(sess.bit_depth == 24);
    REQUIRE(sess.formattedQuality() == "ALAC 24bit/192kHz");
    REQUIRE(engine.adamClient().deletePlaybackSession(sess.session_id));

    auto lyr = engine.getLyricsForTrack(feed.tracks[0], false, true, 0);
    REQUIRE(lyr.source == "adam");
    REQUIRE(lyr.lines.size() == 2);
    REQUIRE(lyr.translated);
    REQUIRE(lyr.lines[0].translation == "在车里静静等待");
    REQUIRE(fs::exists(lyr.bilingual_lrc_path));

    // Verify media.lyrics.v1 explain RPC
    auto exp = engine.handleLyricsRpc({
        {"action", "explain"},
        {"original", "Waiting for a ride in the dark"},
        {"translation", "在黑夜中等候一程"},
        {"focus_phrase", "in the dark"},
    });
    REQUIRE(exp.ok);
    REQUIRE(!exp.data.value("degraded", true));
    REQUIRE(exp.data.value("explanation", "").find("waiting for") != std::string::npos);

    engine.shutdown();
    return 0;
}

int testDlopenPluginLifecycle(const std::string& so_path, const fs::path& tmp) {
    void* h = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != nullptr);
    auto info_fn = reinterpret_cast<OmniDeskAbiInfoFn>(dlsym(h, "omnidesk_plugin_abi_info"));
    auto create_fn = reinterpret_cast<OmniDeskPluginCreateFn>(dlsym(h, "omnidesk_plugin_create"));
    auto destroy_fn = reinterpret_cast<OmniDeskPluginDestroyFn>(dlsym(h, "omnidesk_plugin_destroy"));
    REQUIRE(info_fn && create_fn && destroy_fn);

    const OmniDeskPluginInfo* info = info_fn();
    REQUIRE(info && std::string(info->id) == "plugin-music");
    REQUIRE(std::string(info->sdk_abi_signature) == omnidesk::sdkAbiSignature());

    auto* plugin = static_cast<omnidesk::IPlugin*>(create_fn());
    REQUIRE(plugin != nullptr);

    {
        omnidesk::CapabilityRegistry caps;
        omnidesk::EntityBus bus;
        omnidesk::DomainRegistry domains;
        omnidesk::ConfigStore cfg_store((tmp / "dl_cfg").string());
        omnidesk::SecretVault vault((tmp / "dl_cfg").string());
        cfg_store.savePluginConfig("plugin-music", {
            {"auto_launch_engine", false},
            {"engine_port", 29998},
        });
        omnidesk::PluginContext ctx("plugin-music", tmp.string(), (tmp / "dl_data").string(),
                                    caps, bus, domains, cfg_store, vault);

        REQUIRE(plugin->onLoad(ctx));
        REQUIRE(caps.has("media.source.v1"));
        REQUIRE(caps.has("media.lyrics.v1"));
        REQUIRE(!domains.mediaSources().empty());
        REQUIRE(!domains.workspaceTabs().empty());
        REQUIRE(!domains.dockWidgets(omnidesk::ContentDomain::Media).empty());

        auto st_res = caps.call("media.lyrics.v1", {{"action", "status"}});
        REQUIRE(st_res.ok);
        REQUIRE(st_res.data.value("initialized", false));

        auto hc = plugin->onHealthCheck();
        REQUIRE(hc.value("ok", false));

        plugin->onUnload();
        REQUIRE(!caps.has("media.source.v1"));
        REQUIRE(!caps.has("media.lyrics.v1"));
        REQUIRE(domains.mediaSources().empty());
        destroy_fn(plugin);
    }

    dlclose(h);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        std::cerr << "Usage: test_music <plugin_so_path>\n";
        return 2;
    }

    fs::path tmp = fs::temp_directory_path() / ("omnidesk_music_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    int rc = 0;
    if (rc == 0) rc = testLrcAndBilingualParser();
    if (rc == 0) rc = testLrclibExactMatch();
    if (rc == 0) rc = testMockAdamAndLlmPipeline(tmp);
    if (rc == 0) rc = testDlopenPluginLifecycle(argv[1], tmp);

    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (rc == 0) {
        std::cout << "ALL plugin-music tests passed!\n";
    }
    return rc;
}
