#include "music/adam_client.hpp"
#include "music/config.hpp"
#include "music/lrclib_client.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace music_plugin {
namespace {

constexpr size_t kMaxHttpBytes = 16 * 1024 * 1024;

void ensureCurlInit() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t curlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    if (out->size() + total > kMaxHttpBytes) return 0;
    out->append(ptr, total);
    return total;
}

std::string toLowerCopy(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string formatArtworkUrl(std::string tpl, int size = 500) {
    if (tpl.empty()) return "";
    std::string s = std::to_string(size);
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = tpl.find(from, pos)) != std::string::npos) {
            tpl.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("{w}", s);
    replaceAll("{h}", s);
    return tpl;
}

int connectLoopbackTimeout(uint16_t port, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    if (rc != 0) {
        struct pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

std::vector<uint8_t> getPeerDerCert(SSL* ssl) {
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return {};
    int len = i2d_X509(cert, nullptr);
    std::vector<uint8_t> der;
    if (len > 0 && len <= 65536) {
        der.resize(static_cast<size_t>(len));
        uint8_t* p = der.data();
        i2d_X509(cert, &p);
    }
    X509_free(cert);
    return der;
}


int nosigpipeBioWrite(BIO* b, const char* data, int len) {
    int fd = 0;
    BIO_get_fd(b, &fd);
    BIO_clear_retry_flags(b);
    ssize_t n = ::send(fd, data, static_cast<size_t>(len), MSG_NOSIGNAL);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            BIO_set_retry_write(b);
        }
        return -1;
    }
    return static_cast<int>(n);
}

int nosigpipeBioRead(BIO* b, char* data, int len) {
    int fd = 0;
    BIO_get_fd(b, &fd);
    BIO_clear_retry_flags(b);
    ssize_t n = ::recv(fd, data, static_cast<size_t>(len), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            BIO_set_retry_read(b);
        }
        return -1;
    }
    return static_cast<int>(n);
}


BIO* makeNosigpipeSocketBio(int fd) {
    static BIO_METHOD* method = []() {
        BIO_METHOD* m = BIO_meth_new(BIO_TYPE_SOCKET, "nosigpipe_socket");
        BIO_meth_set_write(m, nosigpipeBioWrite);
        BIO_meth_set_read(m, nosigpipeBioRead);
        BIO_meth_set_ctrl(m, [](BIO* b, int cmd, long /*num*/, void* ptr) -> long {
            int cur_fd = -1;
            switch (cmd) {
                case BIO_C_SET_FD:
                    BIO_set_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(*static_cast<int*>(ptr))));
                    BIO_set_init(b, 1);
                    return 1;
                case BIO_C_GET_FD:
                    cur_fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(b)));
                    if (ptr) *static_cast<int*>(ptr) = cur_fd;
                    return cur_fd;
                case BIO_CTRL_FLUSH:
                case BIO_CTRL_DUP:
                    return 1;
                default:
                    return 0;
            }
        });
        BIO_meth_set_create(m, [](BIO* b) -> int {
            BIO_set_init(b, 0);
            BIO_set_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
            return 1;
        });
        BIO_meth_set_destroy(m, [](BIO*) -> int { return 1; });
        return m;
    }();
    BIO* bio = BIO_new(method);
    if (bio) {
        BIO_set_fd(bio, fd, BIO_NOCLOSE);
    }
    return bio;
}

DrmStatus parseDrmSnapshot(const nlohmann::json& j) {
    DrmStatus st;
    if (!j.is_object()) return st;
    st.engine_reachable = true;
    st.available = j.value("available", true);
    if (j.contains("backend") && j["backend"].is_object()) {
        st.backend = j["backend"].value("selected", "");
    }
    if (j.contains("state") && j["state"].is_object()) {
        const auto& s = j["state"];
        st.manager_state = s.value("manager", "unknown");
        st.process_state = s.value("process", "unknown");
        st.auth_state = s.value("authentication", "unknown");
        st.fairplay_state = s.value("fairplay", "unknown");
        st.session_state = s.value("session", "unknown");
    }
    if (j.contains("capabilities") && j["capabilities"].is_object()) {
        const auto& c = j["capabilities"];
        st.cap_cbcs = c.value("cbcs", false);
        st.cap_alac = c.value("alac", false);
        st.cap_atmos = c.value("atmos", false);
        st.cap_hi_res = c.value("hiRes", false);
    }
    if (j.contains("challenge") && j["challenge"].is_object()) {
        st.challenge_prompt = j["challenge"].value("prompt", j["challenge"].value("message", ""));
    }
    st.message = j.value("message", "");
    return st;
}

void applyAudioTraits(const nlohmann::json& traits, MusicTrack& tr) {
    if (!traits.is_array()) return;
    for (const auto& t : traits) {
        if (!t.is_string()) continue;
        std::string s = t.get<std::string>();
        if (s == "hi-res-lossless") tr.is_hires = true;
        else if (s == "lossless") tr.is_lossless = true;
        else if (s == "atmos" || s == "spatial") tr.is_atmos = true;
    }
    if (tr.is_hires) tr.quality_label = "Hi-Res ALAC";
    else if (tr.is_lossless) tr.quality_label = "ALAC Lossless";
    else if (tr.is_atmos) tr.quality_label = "Dolby Atmos";
    else tr.quality_label = "AAC 256kbps";
}

} // namespace

std::string PlaybackSessionInfo::formattedQuality() const {
    if (type == "mv") return "Apple Music MV";
    std::string c = codec.empty() ? "ALAC" : codec;
    for (char& ch : c) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (bit_depth > 0 && sample_rate > 0) {
        std::ostringstream oss;
        oss << c << " " << bit_depth << "bit/";
        if (sample_rate % 1000 == 0) oss << (sample_rate / 1000) << "kHz";
        else oss << (sample_rate / 1000.0) << "kHz";
        return oss.str();
    }
    if (bit_rate > 0) {
        return c + " " + std::to_string(bit_rate / 1000) + "kbps";
    }
    return c;
}

bool AdamClient::isLoopbackPortOwnedByUid(uint16_t port, uint32_t expected_uid) {
    char expected[32];
    std::snprintf(expected, sizeof(expected), "0100007F:%04X", static_cast<unsigned>(port));
    std::ifstream ifs("/proc/net/tcp");
    if (!ifs.is_open()) return false;
    std::string line;
    std::getline(ifs, line);
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string sl, local_addr, rem_addr, st, tx_rx, tr_tm, retrnsmt, uid_str;
        if (!(iss >> sl >> local_addr >> rem_addr >> st >> tx_rx >> tr_tm >> retrnsmt >> uid_str)) {
            continue;
        }
        std::string upper = local_addr;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (upper == expected && st == "0A") {
            try {
                if (std::stoul(uid_str) == expected_uid) return true;
            } catch (...) {
            }
        }
    }
    return false;
}

std::string AdamClient::decodeChunkedBody(const std::string& raw) {
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        std::string hex_len = raw.substr(pos, line_end - pos);
        size_t semi = hex_len.find(';');
        if (semi != std::string::npos) hex_len = hex_len.substr(0, semi);
        size_t chunk_len = 0;
        try {
            chunk_len = std::stoul(trimCopy(hex_len), nullptr, 16);
        } catch (...) {
            break;
        }
        pos = line_end + 2;
        if (chunk_len == 0) break;
        if (pos + chunk_len > raw.size()) {
            out.append(raw.data() + pos, raw.size() - pos);
            break;
        }
        out.append(raw.data() + pos, chunk_len);
        pos += chunk_len + 2;
    }
    return out;
}

AdamClient::AdamClient(LyricsCacheStore* cache) : cache_(cache) {
    if (const char* tok = std::getenv("ADAM_API_TOKEN"); tok && *tok) {
        api_token_env_ = tok;
    }
}

AdamClient::~AdamClient() {
    stopManagedEngine();
}

void AdamClient::configure(const MusicConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        cfg_ = cfg;
    }
    if (!cfg.media_user_token.empty()) {
        std::lock_guard<std::mutex> lk(token_mutex_);
        media_user_token_ = cfg.media_user_token;
    }
}

bool AdamClient::isPortReachable() const {
    uint16_t port = 20025;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        port = static_cast<uint16_t>(cfg_.engine_port);
    }
    if (!isLoopbackPortOwnedByUid(port, static_cast<uint32_t>(::geteuid()))) {
        return false;
    }
    int fd = connectLoopbackTimeout(port, 400);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

bool AdamClient::ensureEngineRunning(std::string& error) {
    if (isPortReachable()) return true;

    MusicConfig cfg;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        cfg = cfg_;
    }
    if (!cfg.auto_launch_engine) {
        error = "Adam Go 引擎未运行且已关闭自动拉起";
        return false;
    }
    if (cfg.engine_bin.empty()) {
        error = "未找到 Adam Go 引擎二进制文件（请检查设置中的 engine_bin 或 /home/zhu/Projects/apple-music-online/electron/dist/resources/engine）";
        return false;
    }

    std::error_code ec;
    if (!fs::exists(cfg.engine_bin, ec)) {
        error = "Adam Go 引擎路径不存在: " + cfg.engine_bin;
        return false;
    }

    std::lock_guard<std::mutex> lk(proc_mutex_);
    if (isPortReachable()) return true;

    // Clean up exited child if any
    if (managed_pid_ > 0) {
        int st = 0;
        pid_t w = ::waitpid(managed_pid_, &st, WNOHANG);
        if (w == managed_pid_ || w < 0) managed_pid_ = -1;
        else return true; // still starting
    }

    std::string data_dir = cfg.engine_data_dir;
    if (data_dir.empty()) data_dir = expandUserPath("~/.config/adam/engine-data");
    fs::create_directories(data_dir, ec);

    // Ensure config.yaml exists inside engine_data_dir
    fs::path cfg_yaml = fs::path(data_dir) / "config.yaml";
    if (!fs::exists(cfg_yaml, ec)) {
        std::string drm_base = expandUserPath("~/.config/adam/drm/files");
        std::string repo_drm = "/home/zhu/Projects/apple-music-online/drm/files";
        if (!fs::exists(drm_base, ec) && fs::exists(repo_drm, ec)) {
            drm_base = repo_drm;
        }
        fs::create_directories(drm_base, ec);
        std::ofstream ofs(cfg_yaml);
        if (ofs.is_open()) {
            ofs << "storefront: " << (cfg.storefront.empty() ? "us" : cfg.storefront) << "\n"
                << "media-user-token: \"\"\n"
                << "authorization-token: \"\"\n"
                << "decrypt-m3u8-port: 127.0.0.1:10020\n"
                << "get-m3u8-port: 127.0.0.1:20020\n"
                << "get-m3u8-from-device: true\n"
                << "get-m3u8-mode: " << (cfg.hires_enabled ? "hires" : "lossless") << "\n"
                << "aac-type: aac-lc\n"
                << "alac-max: 192000\n"
                << "drm-base-dir: \"" << drm_base << "\"\n";
        }
    }

    std::string log_path = cfg.data_dir.empty()
                               ? "/tmp/omnidesk-adam-engine.log"
                               : (fs::path(cfg.data_dir) / "adam-engine.log").string();
    fs::create_directories(fs::path(log_path).parent_path(), ec);

    std::string engine_dir = fs::path(cfg.engine_bin).parent_path().string();
    std::string port_str = std::to_string(cfg.engine_port);

    pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork Adam Go 引擎失败";
        return false;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        if (::chdir(data_dir.c_str()) != 0) {
            ::_exit(126);
        }
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        int logfd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (logfd >= 0) {
            ::dup2(logfd, STDOUT_FILENO);
            ::dup2(logfd, STDERR_FILENO);
            ::close(logfd);
        }
        ::setenv("GODEBUG", "netdns=go", 1);
        const char* cur_ld = ::getenv("LD_LIBRARY_PATH");
        std::string new_ld = engine_dir + (cur_ld && *cur_ld ? (std::string(":") + cur_ld) : "");
        ::setenv("LD_LIBRARY_PATH", new_ld.c_str(), 1);

        char* argv[] = {
            const_cast<char*>(cfg.engine_bin.c_str()),
            const_cast<char*>("--api"),
            const_cast<char*>(port_str.c_str()),
            nullptr
        };
        ::execvp(argv[0], argv);
        ::_exit(127);
    }

    managed_pid_ = pid;
    for (int i = 0; i < 35; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (isPortReachable()) return true;
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid) {
            managed_pid_ = -1;
            error = "Adam Go 引擎启动后提前退出，请查看日志: " + log_path;
            return false;
        }
    }
    error = "等待 Adam Go 引擎监听端口超时 (" + port_str + ")";
    return false;
}

void AdamClient::stopManagedEngine() {
    std::lock_guard<std::mutex> lk(proc_mutex_);
    if (managed_pid_ <= 0) return;

    pid_t pid = managed_pid_;
    managed_pid_ = -1;
    ::kill(-pid, SIGTERM);
    ::kill(pid, SIGTERM);

    for (int i = 0; i < 20; ++i) {
        int st = 0;
        pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid || w < 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);
}

HttpResponse AdamClient::engineRequest(const std::string& method,
                                       const std::string& path,
                                       const std::string& body,
                                       const std::string& content_type,
                                       const std::string& accept,
                                       int timeout_ms) {
    HttpResponse resp;
    uint16_t port = 20025;
    std::string lang = "zh-CN";
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        port = static_cast<uint16_t>(cfg_.engine_port);
        lang = cfg_.language;
    }

    if (!isLoopbackPortOwnedByUid(port, static_cast<uint32_t>(::geteuid()))) {
        resp.error = "Adam Go 引擎未运行或端口归属当前用户校验失败";
        return resp;
    }

    auto doTlsConnect = [&](const std::vector<uint8_t>* pin_der,
                            std::vector<uint8_t>* captured_der,
                            int& fd_out,
                            SSL*& ssl_out,
                            SSL_CTX*& ctx_out) -> bool {
        fd_out = connectLoopbackTimeout(port, timeout_ms);
        if (fd_out < 0) return false;

        ctx_out = SSL_CTX_new(TLS_client_method());
        if (!ctx_out) { ::close(fd_out); return false; }
        SSL_CTX_set_min_proto_version(ctx_out, TLS1_2_VERSION);

        if (pin_der && !pin_der->empty()) {
            const uint8_t* p = pin_der->data();
            X509* trusted = d2i_X509(nullptr, &p, static_cast<long>(pin_der->size()));
            if (!trusted) {
                SSL_CTX_free(ctx_out);
                ::close(fd_out);
                return false;
            }
            X509_STORE* store = SSL_CTX_get_cert_store(ctx_out);
            X509_STORE_add_cert(store, trusted);
            X509_free(trusted);
            X509_VERIFY_PARAM* param = SSL_CTX_get0_param(ctx_out);
            X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_PARTIAL_CHAIN);
            X509_VERIFY_PARAM_clear_flags(param, X509_V_FLAG_X509_STRICT);
            SSL_CTX_set_verify(ctx_out, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ctx_out, SSL_VERIFY_NONE, nullptr);
        }

        ssl_out = SSL_new(ctx_out);
        if (!ssl_out) { SSL_CTX_free(ctx_out); ::close(fd_out); return false; }
        SSL_set_tlsext_host_name(ssl_out, "127.0.0.1");
        if (pin_der && !pin_der->empty()) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_out), "127.0.0.1");
        BIO* bio = makeNosigpipeSocketBio(fd_out);
        if (!bio) { SSL_free(ssl_out); SSL_CTX_free(ctx_out); ::close(fd_out); return false; }
        SSL_set_bio(ssl_out, bio, bio);
        if (SSL_connect(ssl_out) != 1) {
            SSL_free(ssl_out);
            SSL_CTX_free(ctx_out);
            ::close(fd_out);
            ssl_out = nullptr; ctx_out = nullptr; fd_out = -1;
            return false;
        }
        if (captured_der) *captured_der = getPeerDerCert(ssl_out);
        return true;
    };

    std::vector<uint8_t> leaf_der;
    {
        int fd = -1; SSL* ssl = nullptr; SSL_CTX* ctx = nullptr;
        if (!doTlsConnect(nullptr, &leaf_der, fd, ssl, ctx) || leaf_der.empty()) {
            resp.error = "无法捕获 Adam 回环 TLS 证书";
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
            if (ctx) SSL_CTX_free(ctx);
            if (fd >= 0) ::close(fd);
            return resp;
        }
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd);
    }

    int fd = -1; SSL* ssl = nullptr; SSL_CTX* ctx = nullptr;
    std::vector<uint8_t> actual_der;
    if (!doTlsConnect(&leaf_der, &actual_der, fd, ssl, ctx)) {
        resp.error = "Adam 回环 TLS 校验失败";
        return resp;
    }
    if (actual_der != leaf_der) {
        resp.error = "Adam 回环证书发生变更，请重试";
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd);
        return resp;
    }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1:" << port << "\r\n"
        << "Accept: " << accept << "\r\n"
        << "Accept-Language: " << lang << "\r\n"
        << "Accept-Encoding: identity\r\n"
        << "Connection: close\r\n";
    if (!api_token_env_.empty()) {
        req << "X-Api-Token: " << api_token_env_ << "\r\n";
    }
    if (!body.empty()) {
        req << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n" << body;
    std::string req_str = req.str();
    SSL_write(ssl, req_str.data(), static_cast<int>(req_str.size()));

    std::string raw;
    char buf[8192];
    while (raw.size() < kMaxHttpBytes) {
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd);

    size_t hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        resp.error = "Adam HTTP 响应不完整";
        return resp;
    }
    std::string hdr_block = raw.substr(0, hdr_end);
    std::string raw_body = raw.substr(hdr_end + 4);

    std::istringstream hss(hdr_block);
    std::string status_line;
    std::getline(hss, status_line);
    std::istringstream sls(status_line);
    std::string ver;
    sls >> ver >> resp.status;

    bool chunked = false;
    std::string hline;
    while (std::getline(hss, hline)) {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        size_t colon = hline.find(':');
        if (colon == std::string::npos) continue;
        std::string k = toLowerCopy(trimCopy(hline.substr(0, colon)));
        std::string v = trimCopy(hline.substr(colon + 1));
        if (k == "content-type") resp.content_type = v;
        if (k == "transfer-encoding" && toLowerCopy(v).find("chunked") != std::string::npos) {
            chunked = true;
        }
    }

    resp.body = chunked ? decodeChunkedBody(raw_body) : std::move(raw_body);
    resp.ok = (resp.status >= 200 && resp.status < 300);
    if (!resp.ok && resp.error.empty()) {
        resp.error = trimCopy(resp.body).empty()
                         ? ("HTTP " + std::to_string(resp.status))
                         : trimCopy(resp.body).substr(0, 240);
    }
    return resp;
}

std::string AdamClient::fetchDeveloperJwtFromApple() {
    ensureCurlInit();
    std::string proxy;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        proxy = cfg_.proxy;
    }

    auto curlGet = [&](const std::string& url, std::string& out) -> bool {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36");
        if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);
        return rc == CURLE_OK && code >= 200 && code < 300;
    };

    std::string html;
    if (!curlGet("https://music.apple.com", html)) return "";

    static const std::regex kIndexRe(R"(/assets/index~[^"'\s]+\.js)");
    std::smatch m;
    if (!std::regex_search(html, m, kIndexRe)) return "";

    std::string js;
    if (!curlGet("https://music.apple.com" + m[0].str(), js)) return "";

    static const std::regex kJwtRe(R"(eyJ[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,})");
    std::smatch jm;
    if (std::regex_search(js, jm, kJwtRe)) {
        return jm[0].str();
    }
    return "";
}

std::string AdamClient::getDeveloperToken(bool force_refresh) {
    {
        std::lock_guard<std::mutex> lk(token_mutex_);
        if (!force_refresh && !dev_token_.empty()) return dev_token_;
    }
    if (!force_refresh && cache_) {
        if (auto cached = cache_->loadCachedDevToken(); cached.has_value()) {
            std::lock_guard<std::mutex> lk(token_mutex_);
            dev_token_ = *cached;
            return dev_token_;
        }
    }
    std::string fetched = fetchDeveloperJwtFromApple();
    if (!fetched.empty()) {
        {
            std::lock_guard<std::mutex> lk(token_mutex_);
            dev_token_ = fetched;
        }
        if (cache_) cache_->saveCachedDevToken(fetched);
    }
    return fetched;
}

void AdamClient::setMediaUserToken(const std::string& mut) {
    std::lock_guard<std::mutex> lk(token_mutex_);
    media_user_token_ = trimCopy(mut);
}

std::string AdamClient::mediaUserToken() const {
    std::lock_guard<std::mutex> lk(token_mutex_);
    return media_user_token_;
}

bool AdamClient::hasDeveloperToken() const {
    std::lock_guard<std::mutex> lk(token_mutex_);
    return !dev_token_.empty();
}

bool AdamClient::pushTokensToEngine(std::string& error) {
    std::string dev = getDeveloperToken(false);
    std::string mut = mediaUserToken();
    if (dev.empty() && mut.empty()) {
        error = "暂无可推送的 Developer JWT / media-user-token";
        return false;
    }
    nlohmann::json body = nlohmann::json::object();
    if (!dev.empty()) body["developerToken"] = dev;
    if (!mut.empty()) body["musicUserToken"] = mut;
    HttpResponse r = engineRequest("POST", "/api/v1/library/token", body.dump());
    if (!r.ok) {
        error = r.error;
        return false;
    }
    return true;
}

std::string AdamClient::effectiveStorefront(const std::string& override_sf) const {
    if (!override_sf.empty()) return override_sf;
    std::lock_guard<std::mutex> lk(cfg_mutex_);
    return cfg_.storefront.empty() ? "us" : cfg_.storefront;
}

DrmStatus AdamClient::getDrmStatus() {
    HttpResponse r = engineRequest("GET", "/api/v1/drm/status");
    if (!r.ok) {
        DrmStatus st;
        st.engine_reachable = false;
        st.error = r.error;
        return st;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_discarded()) {
        DrmStatus st;
        st.engine_reachable = true;
        st.error = "DRM 状态响应 JSON 无效";
        return st;
    }
    return parseDrmSnapshot(j);
}

bool AdamClient::authenticateDrm(const std::string& email,
                                   const std::string& password,
                                   DrmStatus& status_out,
                                   std::string& error) {
    if (email.empty() || password.empty()) {
        error = "请输入 Apple ID 邮箱与密码";
        return false;
    }
    nlohmann::json body = {{"email", email}, {"password", password}};
    HttpResponse r = engineRequest("POST", "/api/v1/drm/authenticate", body.dump(),
                                   "application/json", "application/json", 12000);
    if (!r.ok) {
        error = r.error;
        return false;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_object()) status_out = parseDrmSnapshot(j);
    return true;
}

bool AdamClient::submitDrmChallenge(const std::string& code,
                                    DrmStatus& status_out,
                                    std::string& error) {
    if (code.empty()) {
        error = "请输入 6 位双重认证验证码";
        return false;
    }
    nlohmann::json body = {{"reply", trimCopy(code)}};
    HttpResponse r = engineRequest("POST", "/api/v1/drm/challenge", body.dump(),
                                   "application/json", "application/json", 12000);
    if (!r.ok) {
        error = r.error;
        return false;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_object()) status_out = parseDrmSnapshot(j);
    return true;
}

bool AdamClient::resendDrmChallenge(DrmStatus& status_out, std::string& error) {
    HttpResponse r = engineRequest("POST", "/api/v1/drm/challenge/resend", "{}");
    if (!r.ok) {
        error = r.error;
        return false;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_object()) status_out = parseDrmSnapshot(j);
    return true;
}

bool AdamClient::logoutDrm(std::string& error) {
    HttpResponse r = engineRequest("POST", "/api/v1/drm/logout", "{}");
    if (!r.ok) {
        error = r.error;
        return false;
    }
    return true;
}

PlaybackSessionInfo AdamClient::createPlaybackSession(const std::string& asset_id,
                                                      const std::string& storefront,
                                                      bool lossless,
                                                      bool hires,
                                                      bool video,
                                                      bool atmos) {
    PlaybackSessionInfo info;
    if (asset_id.empty()) {
        info.error = "缺少曲目 assetId";
        return info;
    }

    std::string dev = getDeveloperToken(false);
    std::string mut = mediaUserToken();
    nlohmann::json req = {
        {"assetId", asset_id},
        {"capabilities", {
            {"lossless", lossless},
            {"hiRes", hires},
            {"video", video},
            {"atmos", atmos},
        }},
    };
    if (!storefront.empty()) req["storefront"] = storefront;
    if (!dev.empty()) req["token"] = dev;
    if (!mut.empty()) req["mediaUserToken"] = mut;

    HttpResponse r = engineRequest("POST", "/api/v1/playback", req.dump(),
                                   "application/json", "application/json", 15000);
    if (!r.ok) {
        info.error = r.error;
        return info;
    }

    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || j.value("sessionId", "").empty()) {
        info.error = "播放会话响应无效";
        return info;
    }

    uint16_t port = 20025;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        port = static_cast<uint16_t>(cfg_.engine_port);
    }
    std::string base = "https://127.0.0.1:" + std::to_string(port);

    info.ok = true;
    info.session_id = j.value("sessionId", "");
    info.asset_id = j.value("assetId", asset_id);
    info.storefront = j.value("storefront", "");
    info.type = j.value("type", video ? "mv" : "song");
    info.codec = j.value("codec", "");
    info.sample_rate = j.value("sampleRate", 0);
    info.bit_depth = j.value("bitDepth", 0);
    info.bit_rate = j.value("bitRate", 0);
    info.channel_count = j.value("channelCount", 0);
    info.spatial_audio = j.value("spatialAudio", "");
    info.title = j.value("title", "");
    info.artist = j.value("artistName", "");
    info.album = j.value("albumName", "");
    info.duration_sec = j.value("durationMs", 0) / 1000.0;

    std::string audio_rel = "/api/v1/playback/" + info.session_id + "/audio";
    if (j.contains("streams") && j["streams"].is_object()) {
        std::string sa = j["streams"].value("audio", "");
        if (!sa.empty()) audio_rel = sa;
    }
    info.audio_url = (audio_rel.rfind("http", 0) == 0) ? audio_rel : (base + audio_rel);
    if (info.type == "mv") {
        info.video_url = base + "/api/v1/playback/" + info.session_id + "/vseg/manifest";
    }
    return info;
}

bool AdamClient::deletePlaybackSession(const std::string& session_id) {
    if (session_id.empty()) return true;
    HttpResponse r = engineRequest("DELETE", "/api/v1/playback/" + session_id, "", "application/json", "application/json", 3000);
    return r.ok;
}

bool AdamClient::fetchMetadata(const std::string& asset_id,
                               const std::string& storefront,
                               MusicTrack& out,
                               std::string& error) {
    std::string dummy_err;
    pushTokensToEngine(dummy_err);
    std::string path = "/api/v1/metadata/" + asset_id;
    if (!storefront.empty()) path += "?sf=" + storefront;
    HttpResponse r = engineRequest("GET", path);
    if (!r.ok) {
        error = r.error;
        return false;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object()) {
        error = "metadata JSON invalid";
        return false;
    }
    out.id = j.value("id", asset_id);
    out.kind = j.value("type", "song");
    out.title = j.value("title", "");
    out.artist = j.value("artistName", "");
    out.album = j.value("albumName", "");
    out.duration_sec = j.value("durationMs", 0) / 1000.0;
    out.cover_url = j.value("artworkUrl", "");
    out.has_lyrics = j.value("hasLyrics", false);
    if (j.contains("availableStreams") && j["availableStreams"].is_array() &&
        !j["availableStreams"].empty()) {
        const auto& s0 = j["availableStreams"][0];
        std::string c = s0.value("codec", "");
        int sr = s0.value("sampleRate", 0);
        int bd = s0.value("bitDepth", 0);
        if (bd > 0 && sr > 0) {
            out.is_lossless = true;
            out.is_hires = (sr > 48000 || bd > 16);
            out.quality_label = c + " " + std::to_string(bd) + "bit/" + std::to_string(sr / 1000) + "kHz";
        } else if (!c.empty()) {
            out.quality_label = c;
        }
    }
    return true;
}

bool AdamClient::fetchNativeLyrics(const std::string& asset_id,
                                   const std::string& storefront,
                                   std::string& lrc_out,
                                   std::string& error) {
    if (asset_id.empty()) {
        error = "empty asset_id";
        return false;
    }
    std::string dummy_err;
    pushTokensToEngine(dummy_err);

    std::string sf = storefront;
    std::string path = "/api/v1/lyrics/" + asset_id + "?format=lrc";
    if (!sf.empty()) path += "&sf=" + sf;

    HttpResponse r = engineRequest("GET", path, "", "application/json", "text/plain");
    if (r.status == 404) {
        std::string fb_sf = (sf == "cn") ? "us" : "cn";
        r = engineRequest("GET", "/api/v1/lyrics/" + asset_id + "?format=lrc&sf=" + fb_sf,
                          "", "application/json", "text/plain");
    }
    if (!r.ok || trimCopy(r.body).empty()) {
        error = r.error.empty() ? "未找到 Adam 原生歌词" : r.error;
        return false;
    }
    lrc_out = r.body;
    return true;
}

bool AdamClient::fetchLibraryStatus(nlohmann::json& out, std::string& error) {
    HttpResponse r = engineRequest("GET", "/api/v1/library/status");
    if (!r.ok) { error = r.error; return false; }
    out = nlohmann::json::parse(r.body, nullptr, false);
    if (!out.is_object()) { error = "library status JSON invalid"; return false; }
    return true;
}

std::vector<ContainerItem> AdamClient::fetchLibraryPlaylists(std::string& error) {
    std::vector<ContainerItem> out;
    HttpResponse r = engineRequest("GET", "/api/v1/library/playlists");
    if (!r.ok) { error = r.error; return out; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("playlists") || !j["playlists"].is_array()) return out;
    for (const auto& p : j["playlists"]) {
        if (!p.is_object()) continue;
        ContainerItem c;
        c.id = p.value("lid", "");
        c.type = "library-playlists";
        c.name = p.value("name", c.id);
        c.track_count = p.value("trackCount", 0);
        c.subtitle = std::to_string(c.track_count) + " 首歌曲";
        if (!c.id.empty()) out.push_back(std::move(c));
    }
    return out;
}

std::vector<MusicTrack> AdamClient::fetchLibraryPlaylistTracks(const std::string& playlist_id, std::string& error) {
    std::string dummy_err;
    pushTokensToEngine(dummy_err);
    std::vector<MusicTrack> out;
    HttpResponse r = engineRequest("GET", "/api/v1/library/playlists/" + playlist_id + "/tracks");
    if (!r.ok) { error = r.error; return out; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("tracks") || !j["tracks"].is_array()) return out;
    for (const auto& t : j["tracks"]) {
        if (!t.is_object()) continue;
        MusicTrack tr;
        tr.library_id = t.value("lid", "");
        tr.id = t.value("cid", "");
        if (tr.id.empty()) tr.id = tr.library_id;
        tr.title = t.value("name", tr.id);
        tr.artist = t.value("artist", "资料库曲目");
        tr.album = t.value("album", "");
        tr.duration_sec = t.value("ms", 0) / 1000.0;
        tr.quality_label = "资料库";
        if (!tr.id.empty()) out.push_back(std::move(tr));
    }
    return out;
}

std::vector<MusicTrack> AdamClient::fetchLibraryAlbumTracks(const std::string& album_id, std::string& error) {
    std::vector<MusicTrack> out;
    HttpResponse r = engineRequest("GET", "/api/v1/library/albums/" + album_id + "/tracks");
    if (!r.ok) { error = r.error; return out; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("tracks") || !j["tracks"].is_array()) return out;
    for (const auto& t : j["tracks"]) {
        if (!t.is_object()) continue;
        MusicTrack tr;
        tr.library_id = t.value("lid", "");
        tr.id = t.value("cid", "");
        if (tr.id.empty()) tr.id = tr.library_id;
        tr.title = t.value("name", tr.id);
        tr.artist = t.value("artist", "资料库曲目");
        tr.album = t.value("album", "");
        tr.duration_sec = t.value("ms", 0) / 1000.0;
        tr.quality_label = "资料库";
        if (!tr.id.empty()) out.push_back(std::move(tr));
    }
    return out;
}

HttpResponse AdamClient::ampApiGet(const std::string& path_and_query, bool require_mut) {
    HttpResponse res;
    std::string dev = getDeveloperToken(false);
    if (dev.empty()) {
        res.error = "无法获取 Apple Music Developer JWT";
        return res;
    }
    std::string mut = mediaUserToken();
    if (require_mut && mut.empty()) {
        // Fallback: try reading MUSIC_TOKEN from drm_base_dir if configured
        MusicConfig cfg;
        {
            std::lock_guard<std::mutex> lk(cfg_mutex_);
            cfg = cfg_;
        }
        std::string candidate = expandUserPath("~/.config/adam/drm/files/MUSIC_TOKEN");
        std::string repo_cand = "/home/zhu/Projects/apple-music-online/drm/files/MUSIC_TOKEN";
        std::error_code ec;
        std::string tok_file = fs::exists(candidate, ec) ? candidate : (fs::exists(repo_cand, ec) ? repo_cand : "");
        if (!tok_file.empty()) {
            std::ifstream ifs(tok_file);
            std::string raw;
            std::getline(ifs, raw);
            mut = trimCopy(raw);
        }
    }
    if (require_mut && mut.empty()) {
        res.error = "需要 Music-User-Token（请先登录 Apple ID 或导入 media-user-token）";
        return res;
    }

    ensureCurlInit();
    std::string proxy;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        proxy = cfg_.proxy;
    }

    std::string url = "https://amp-api.music.apple.com" + path_and_query;
    CURL* curl = curl_easy_init();
    if (!curl) { res.error = "curl_easy_init failed"; return res; }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + dev).c_str());
    headers = curl_slist_append(headers, "Origin: https://music.apple.com");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!mut.empty()) {
        headers = curl_slist_append(headers, ("Music-User-Token: " + mut).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 12000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/131.0.0.0 Safari/537.36");
    if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    res.status = static_cast<int>(code);
    res.ok = (rc == CURLE_OK && code >= 200 && code < 300);
    if (!res.ok) {
        res.error = (rc != CURLE_OK) ? curl_easy_strerror(rc) : ("AMP API HTTP " + std::to_string(code));
    }
    return res;
}

bool AdamClient::syncLibraryFromApple(nlohmann::json& stats_out, std::string& error) {
    nlohmann::json playlists = nlohmann::json::array();
    nlohmann::json songs = nlohmann::json::array();
    nlohmann::json albums = nlohmann::json::array();

    std::string lang;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        lang = cfg_.language;
    }

    // Fetch playlists (up to 3 pages of 100)
    std::string next_pl = "/v1/me/library/playlists?limit=100&l=" + LrclibClient::urlEncode(lang);
    for (int page = 0; page < 3 && !next_pl.empty(); ++page) {
        HttpResponse r = ampApiGet(next_pl, true);
        if (!r.ok) { error = r.error; return false; }
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) break;
        for (const auto& item : j["data"]) {
            if (!item.is_object()) continue;
            const auto& attr = item.value("attributes", nlohmann::json::object());
            playlists.push_back({
                {"lid", item.value("id", "")},
                {"name", attr.value("name", "")},
                {"trackCount", attr.value("trackCount", 0)},
            });
        }
        next_pl = j.value("next", "");
    }

    // Fetch library songs (up to 5 pages of 100)
    std::string next_songs = "/v1/me/library/songs?limit=100&l=" + LrclibClient::urlEncode(lang);
    for (int page = 0; page < 5 && !next_songs.empty(); ++page) {
        HttpResponse r = ampApiGet(next_songs, true);
        if (!r.ok) break;
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) break;
        for (const auto& item : j["data"]) {
            if (!item.is_object()) continue;
            const auto& attr = item.value("attributes", nlohmann::json::object());
            std::string cid;
            if (attr.contains("playParams") && attr["playParams"].is_object()) {
                cid = attr["playParams"].value("catalogId", "");
            }
            songs.push_back({
                {"lid", item.value("id", "")},
                {"cid", cid},
                {"name", attr.value("name", "")},
                {"artist", attr.value("artistName", "")},
                {"album", attr.value("albumName", "")},
                {"ms", attr.value("durationInMillis", 0)},
                {"track", attr.value("trackNumber", 0)},
                {"disc", attr.value("discNumber", 0)},
            });
        }
        next_songs = j.value("next", "");
    }

    nlohmann::json ingest = {
        {"playlists", std::move(playlists)},
        {"songs", std::move(songs)},
        {"albums", std::move(albums)},
    };
    HttpResponse ir = engineRequest("POST", "/api/v1/library/ingest", ingest.dump(),
                                    "application/json", "application/json", 15000);
    if (!ir.ok) {
        error = ir.error;
        return false;
    }
    stats_out = nlohmann::json::parse(ir.body, nullptr, false);
    return true;
}

void AdamClient::parseResourceNode(const nlohmann::json& node,
                                   const std::string& default_sf,
                                   std::vector<ContainerItem>& containers,
                                   std::vector<MusicTrack>& tracks) {
    if (!node.is_object()) return;
    std::string type = node.value("type", "");
    std::string id = node.value("id", "");
    const auto attr = node.value("attributes", nlohmann::json::object());

    if (type == "songs" || type == "music-videos" || type == "library-songs") {
        MusicTrack tr;
        tr.id = id;
        if (attr.contains("playParams") && attr["playParams"].is_object()) {
            std::string cid = attr["playParams"].value("catalogId", "");
            if (!cid.empty()) {
                tr.library_id = id;
                tr.id = cid;
            }
        }
        tr.kind = (type == "music-videos") ? "mv" : "song";
        tr.title = attr.value("name", "");
        tr.artist = attr.value("artistName", "");
        tr.album = attr.value("albumName", "");
        tr.duration_sec = attr.value("durationInMillis", 0) / 1000.0;
        tr.has_lyrics = attr.value("hasLyrics", false);
        tr.storefront = default_sf;
        if (attr.contains("artwork") && attr["artwork"].is_object()) {
            tr.cover_url = formatArtworkUrl(attr["artwork"].value("url", ""), 500);
        }
        if (attr.contains("audioTraits")) applyAudioTraits(attr["audioTraits"], tr);
        if (tr.kind == "mv") tr.quality_label = "MV";
        if (!tr.id.empty() && !tr.title.empty()) tracks.push_back(std::move(tr));
        return;
    }

    if (type == "albums" || type == "playlists" || type == "library-playlists" ||
        type == "library-albums" || type == "artists") {
        ContainerItem c;
        c.id = id;
        c.type = type;
        c.name = attr.value("name", "");
        c.subtitle = attr.value("artistName", attr.value("curatorName", ""));
        if (c.subtitle.empty() && attr.contains("genreNames") && attr["genreNames"].is_array() &&
            !attr["genreNames"].empty() && attr["genreNames"][0].is_string()) {
            c.subtitle = attr["genreNames"][0].get<std::string>();
        }
        c.track_count = attr.value("trackCount", 0);
        c.storefront = default_sf;
        if (attr.contains("artwork") && attr["artwork"].is_object()) {
            c.cover_url = formatArtworkUrl(attr["artwork"].value("url", ""), 500);
        }
        if (!c.id.empty() && !c.name.empty()) containers.push_back(std::move(c));
        return;
    }

    // Personal recommendation containers wrap nested albums/playlists/songs
    if (node.contains("relationships") && node["relationships"].is_object()) {
        const auto& rels = node["relationships"];
        for (const char* rel_key : {"contents", "tracks"}) {
            if (rels.contains(rel_key) && rels[rel_key].is_object() &&
                rels[rel_key].contains("data") && rels[rel_key]["data"].is_array()) {
                for (const auto& child : rels[rel_key]["data"]) {
                    parseResourceNode(child, default_sf, containers, tracks);
                }
            }
        }
    }
}

FeedOutcome AdamClient::fetchRecommendations(const std::string& kind, int limit) {
    FeedOutcome out;
    out.feed_id = kind;
    std::string dummy_err;
    pushTokensToEngine(dummy_err);

    std::string path = "/api/v1/recommendations";
    if (kind == "heavy-rotation") path = "/api/v1/recommendations/heavy-rotation";
    else if (kind == "recently-played") path = "/api/v1/recommendations/recently-played";
    path += "?limit=" + std::to_string(std::clamp(limit, 1, 50));

    HttpResponse r = engineRequest("GET", path, "", "application/json", "application/json", 12000);
    if (!r.ok) {
        out.error = r.error;
        return out;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) {
        out.error = "推荐响应格式无效";
        return out;
    }
    std::string sf = effectiveStorefront();
    for (const auto& item : j["data"]) {
        parseResourceNode(item, sf, out.containers, out.tracks);
    }
    out.ok = true;
    return out;
}

bool AdamClient::fetchCatalogAlbum(const std::string& album_id,
                                   const std::string& sf,
                                   ContainerItem& album_out,
                                   std::vector<MusicTrack>& tracks_out,
                                   std::string& error) {
    std::string dummy_err;
    pushTokensToEngine(dummy_err);
    std::string eff_sf = effectiveStorefront(sf);
    HttpResponse r = engineRequest("GET", "/api/v1/catalog/albums/" + album_id + "?sf=" + eff_sf);
    if (!r.ok) { error = r.error; return false; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
        error = "专辑数据为空";
        return false;
    }
    std::vector<ContainerItem> c_tmp;
    parseResourceNode(j["data"][0], eff_sf, c_tmp, tracks_out);
    if (!c_tmp.empty()) album_out = c_tmp.front();
    const auto& d0 = j["data"][0];
    if (d0.contains("relationships") && d0["relationships"].is_object() &&
        d0["relationships"].contains("tracks") && d0["relationships"]["tracks"].is_object() &&
        d0["relationships"]["tracks"].contains("data") && d0["relationships"]["tracks"]["data"].is_array()) {
        for (const auto& tr_node : d0["relationships"]["tracks"]["data"]) {
            parseResourceNode(tr_node, eff_sf, c_tmp, tracks_out);
        }
    }
    return true;
}

bool AdamClient::fetchCatalogPlaylist(const std::string& playlist_id,
                                      const std::string& sf,
                                      ContainerItem& pl_out,
                                      std::vector<MusicTrack>& tracks_out,
                                      std::string& error) {
    std::string dummy_err;
    pushTokensToEngine(dummy_err);
    std::string eff_sf = effectiveStorefront(sf);
    HttpResponse r = engineRequest("GET", "/api/v1/catalog/playlists/" + playlist_id + "?sf=" + eff_sf);
    if (!r.ok) { error = r.error; return false; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
        error = "歌单数据为空";
        return false;
    }
    std::vector<ContainerItem> c_tmp;
    parseResourceNode(j["data"][0], eff_sf, c_tmp, tracks_out);
    if (!c_tmp.empty()) pl_out = c_tmp.front();
    const auto& d0 = j["data"][0];
    if (d0.contains("relationships") && d0["relationships"].is_object() &&
        d0["relationships"].contains("tracks") && d0["relationships"]["tracks"].is_object() &&
        d0["relationships"]["tracks"].contains("data") && d0["relationships"]["tracks"]["data"].is_array()) {
        for (const auto& tr_node : d0["relationships"]["tracks"]["data"]) {
            parseResourceNode(tr_node, eff_sf, c_tmp, tracks_out);
        }
    }
    return true;
}

bool AdamClient::fetchCatalogArtist(const std::string& artist_id,
                                    const std::string& sf,
                                    ContainerItem& artist_out,
                                    std::vector<ContainerItem>& albums_out,
                                    std::vector<MusicTrack>& top_songs_out,
                                    std::string& error) {
    std::string dummy_err;
    pushTokensToEngine(dummy_err);
    std::string eff_sf = effectiveStorefront(sf);
    HttpResponse r = engineRequest("GET", "/api/v1/catalog/artists/" + artist_id + "?sf=" + eff_sf);
    if (!r.ok) { error = r.error; return false; }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
        error = "艺人数据为空";
        return false;
    }
    std::vector<ContainerItem> c_tmp;
    parseResourceNode(j["data"][0], eff_sf, c_tmp, top_songs_out);
    if (!c_tmp.empty()) artist_out = c_tmp.front();
    const auto& d0 = j["data"][0];
    if (d0.contains("relationships") && d0["relationships"].is_object()) {
        const auto& rels = d0["relationships"];
        if (rels.contains("albums") && rels["albums"].contains("data")) {
            for (const auto& a : rels["albums"]["data"]) {
                parseResourceNode(a, eff_sf, albums_out, top_songs_out);
            }
        }
        if (rels.contains("songs") && rels["songs"].contains("data")) {
            for (const auto& s : rels["songs"]["data"]) {
                parseResourceNode(s, eff_sf, albums_out, top_songs_out);
            }
        }
    }
    return true;
}

FeedOutcome AdamClient::searchCatalog(const std::string& query,
                                      const std::string& sf,
                                      const std::string& types) {
    FeedOutcome out;
    out.feed_id = "search";
    if (trimCopy(query).empty()) {
        out.ok = true;
        return out;
    }
    std::string eff_sf = effectiveStorefront(sf);
    std::string lang;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex_);
        lang = cfg_.language;
    }
    std::string path = "/v1/catalog/" + eff_sf + "/search?term=" + LrclibClient::urlEncode(query) +
                       "&types=" + LrclibClient::urlEncode(types) +
                       "&limit=15&l=" + LrclibClient::urlEncode(lang);
    HttpResponse r = ampApiGet(path, false);
    if (!r.ok) {
        out.error = r.error;
        return out;
    }
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("results") || !j["results"].is_object()) {
        out.error = "Apple Music 搜索响应格式无效";
        return out;
    }
    const auto& res = j["results"];
    for (const char* key : {"songs", "music-videos", "albums", "playlists", "artists"}) {
        if (res.contains(key) && res[key].is_object() &&
            res[key].contains("data") && res[key]["data"].is_array()) {
            for (const auto& item : res[key]["data"]) {
                parseResourceNode(item, eff_sf, out.containers, out.tracks);
            }
        }
    }
    out.ok = true;
    return out;
}

} // namespace music_plugin
