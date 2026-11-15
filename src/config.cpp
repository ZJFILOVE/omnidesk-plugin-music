#include "music/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>

namespace fs = std::filesystem;

namespace music_plugin {
namespace {

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool parseBoolValue(const nlohmann::json& j, const std::string& str_val, bool fallback) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_integer()) return j.get<int>() != 0;
    std::string s = j.is_string() ? j.get<std::string>() : str_val;
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return fallback;
}

int parseIntValue(const nlohmann::json& j, const std::string& str_val, int fallback) {
    try {
        if (j.is_number_integer()) return j.get<int>();
        if (j.is_string() && !j.get<std::string>().empty()) return std::stoi(j.get<std::string>());
        if (!str_val.empty()) return std::stoi(str_val);
    } catch (...) {
    }
    return fallback;
}

} // namespace

std::string expandUserPath(const std::string& path) {
    if (path.empty()) return "";
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

std::string resolveEngineBinary(const std::string& configured) {
    std::error_code ec;
    std::vector<std::string> candidates;
    if (!configured.empty()) candidates.push_back(expandUserPath(configured));
    if (const char* env = std::getenv("ADAM_ENGINE_BIN"); env && *env) {
        candidates.emplace_back(env);
    }
    candidates.push_back("/home/zhu/Projects/apple-music-online/electron/dist/resources/engine");
    candidates.push_back(expandUserPath("~/Projects/apple-music-online/electron/dist/resources/engine"));
    candidates.push_back("/usr/lib/adam/resources/engine");
    candidates.push_back("/opt/adam/resources/engine");

    for (const auto& c : candidates) {
        if (!c.empty() && fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
            return c;
        }
    }
    return "";
}

std::string resolveEngineDataDir(const std::string& configured) {
    if (!configured.empty()) return expandUserPath(configured);
    const char* xdg_cfg = std::getenv("XDG_CONFIG_HOME");
    if (xdg_cfg && *xdg_cfg) {
        return (fs::path(xdg_cfg) / "adam" / "engine-data").string();
    }
    return expandUserPath("~/.config/adam/engine-data");
}

MusicConfig loadConfigFromContext(const omnidesk::PluginContext& ctx) {
    MusicConfig cfg;
    const nlohmann::json raw = ctx.loadConfig();

    auto getStr = [&](const char* key, const std::string& def) {
        if (raw.is_object() && raw.contains(key) && raw[key].is_string()) {
            std::string val = trimCopy(raw[key].get<std::string>());
            if (!val.empty()) return val;
        }
        std::string from_ctx = trimCopy(ctx.getConfigString(key, def));
        return from_ctx.empty() ? def : from_ctx;
    };
    auto field = [&](const char* key) -> nlohmann::json {
        return (raw.is_object() && raw.contains(key)) ? raw[key] : nlohmann::json{};
    };

    cfg.engine_bin = resolveEngineBinary(getStr("engine_bin", ""));
    cfg.engine_data_dir = resolveEngineDataDir(getStr("engine_data_dir", ""));
    cfg.engine_port = std::clamp(
        parseIntValue(field("engine_port"), ctx.getConfigString("engine_port", ""), 20025),
        1024, 65535);
    cfg.auto_launch_engine = parseBoolValue(field("auto_launch_engine"),
                                            ctx.getConfigString("auto_launch_engine", ""), true);
    cfg.storefront = getStr("storefront", "us");
    for (char& c : cfg.storefront) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (cfg.storefront.size() != 2) cfg.storefront = "us";

    cfg.language = getStr("language", "zh-CN");
    cfg.lossless_enabled = parseBoolValue(field("lossless_enabled"),
                                          ctx.getConfigString("lossless_enabled", ""), true);
    cfg.hires_enabled = parseBoolValue(field("hires_enabled"),
                                       ctx.getConfigString("hires_enabled", ""), true);
    cfg.enable_lrclib = parseBoolValue(field("enable_lrclib"),
                                       ctx.getConfigString("enable_lrclib", ""), true);
    cfg.auto_translate_lyrics = parseBoolValue(field("auto_translate_lyrics"),
                                               ctx.getConfigString("auto_translate_lyrics", ""), true);
    cfg.target_language = getStr("target_language", "zh-CN");
    cfg.lyrics_display_mode = getStr("lyrics_display_mode", "bilingual");
    if (cfg.lyrics_display_mode != "bilingual" &&
        cfg.lyrics_display_mode != "original" &&
        cfg.lyrics_display_mode != "translation") {
        cfg.lyrics_display_mode = "bilingual";
    }
    cfg.translation_batch_size = std::clamp(
        parseIntValue(field("translation_batch_size"),
                      ctx.getConfigString("translation_batch_size", ""), 24),
        4, 60);

    if (auto mut = ctx.getSecret("media_user_token"); mut.has_value() && !mut->empty()) {
        cfg.media_user_token = trimCopy(*mut);
    }
    cfg.proxy = getStr("proxy", "");
    cfg.data_dir = ctx.dataDir();
    return cfg;
}

std::string sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
        EVP_MD_CTX_free(ctx);
        std::ostringstream oss;
        for (unsigned int i = 0; i < len; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        }
        return oss.str();
    }
    EVP_MD_CTX_free(ctx);
    return "";
}

} // namespace music_plugin
