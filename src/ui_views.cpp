#include "music/ui_views.hpp"
#include "music/lrc_parser.hpp"

#include <algorithm>
#include <cstdio>
#include <omnidesk/image_cache.hpp>
#include <omnidesk/ui_theme.hpp>
#include <thread>

namespace music_plugin {
namespace {

std::string formatDuration(double sec) {
    if (sec <= 0.0) return "00:00";
    int total = static_cast<int>(sec);
    int m = total / 60;
    int s = total % 60;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

std::string clipText(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

std::string drmBadgeText(const DrmStatus& drm) {
    if (!drm.engine_reachable) return "引擎未连接";
    if (drm.auth_state == "challenging") return "等待 2FA 验证码";
    if (drm.auth_state == "logging_in") return "正在登录 Apple ID…";
    if (drm.fairplay_state == "ready") {
        if (drm.cap_hi_res) return "FairPlay Hi-Res ALAC 就绪";
        if (drm.cap_alac) return "FairPlay ALAC 就绪";
        return "FairPlay 就绪";
    }
    if (drm.auth_state == "logged_in") return "已登录 · 初始化 FairPlay…";
    return "未登录 Apple ID";
}

} // namespace

MusicWorkspaceView::MusicWorkspaceView(std::shared_ptr<MusicEngine> engine)
    : brls::Box(brls::Axis::COLUMN), engine_(std::move(engine)) {
    buildUi();

    if (engine_) {
        std::weak_ptr<std::atomic<bool>> alive_w = alive_;
        engine_sub_ = engine_->subscribeState([this, alive_w]() {
            brls::sync([this, alive_w]() {
                auto a = alive_w.lock();
                if (!a || !a->load(std::memory_order_acquire)) return;
                refreshHeader();
                if (active_tab_ == SubTab::NowPlaying) {
                    renderNowPlayingBody();
                }
            });
        });

        mpv_sub_ = omnidesk::MPVCore::instance().subscribeState(
            [this, alive_w](const omnidesk::MpvPlaybackState& st) {
                brls::sync([this, alive_w, pos = st.position_sec]() {
                    auto a = alive_w.lock();
                    if (!a || !a->load(std::memory_order_acquire)) return;
                    if (active_tab_ == SubTab::NowPlaying) {
                        updateActiveLyricHighlight(pos);
                    }
                });
            });

        // Initial async status & feed load
        std::thread([eng = engine_, this, alive_w]() {
            if (!eng || !eng->isAlive()) return;
            eng->refreshDrmStatus(true);
            FeedOutcome rec = eng->loadFeed("recommendations", "", false);
            brls::sync([this, alive_w, rec]() {
                auto a = alive_w.lock();
                if (!a || !a->load(std::memory_order_acquire)) return;
                current_feed_ = rec;
                refreshHeader();
                renderActiveTab();
            });
        }).detach();
    }
}

MusicWorkspaceView::~MusicWorkspaceView() {
    alive_->store(false, std::memory_order_release);
    if (engine_ && engine_sub_ != 0) {
        engine_->unsubscribeState(engine_sub_);
        engine_sub_ = 0;
    }
    if (mpv_sub_ != 0) {
        omnidesk::MPVCore::instance().unsubscribeState(mpv_sub_);
        mpv_sub_ = 0;
    }
    mv_stage_ = nullptr;
}

void MusicWorkspaceView::buildUi() {
    setGrow(1.0f);
    setPadding(20, 24, 16, 24);
    setBackgroundColor(omnidesk::ui::color("brls/background"));

    auto* header = new brls::Box(brls::Axis::ROW);
    header->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginBottom(10);

    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->addView(omnidesk::ui::makeLabel("Apple Music · FairPlay 无损工作室", 23.0f, "brls/text"));
    status_label_ = omnidesk::ui::makeLabel("直连无头 Go 引擎 · 24bit/192kHz ALAC 解密 · 双语歌词精讲", 14.5f, "font/grey");
    status_label_->setMarginTop(3);
    left->addView(status_label_);
    header->addView(left);

    auto* right = new brls::Box(brls::Axis::ROW);
    right->setAlignItems(brls::AlignItems::CENTER);

    auto* drm_pill = omnidesk::ui::makeCard("desktop/soft", 7.0f, 12.0f);
    drm_pill->setMarginRight(8);
    drm_pill_label_ = omnidesk::ui::makeLabel("检测引擎中…", 13.5f, "domain/media");
    drm_pill->addView(drm_pill_label_);
    right->addView(drm_pill);

    auto* search_btn = omnidesk::ui::makeButton("搜索 /", [this]() { promptSearch(); }, false, 34.0f);
    search_btn->setMarginRight(8);
    right->addView(search_btn);

    auto* auth_btn = omnidesk::ui::makeButton("Apple ID / 账号", [this]() {
        show_auth_panel_ = !show_auth_panel_;
        refreshHeader();
    }, false, 34.0f);
    right->addView(auth_btn);
    header->addView(right);
    addView(header);

    auth_panel_box_ = new brls::Box(brls::Axis::COLUMN);
    auth_panel_box_->setMarginBottom(10);
    auth_panel_box_->setVisibility(brls::Visibility::GONE);
    addView(auth_panel_box_);

    sub_tabs_box_ = new brls::Box(brls::Axis::ROW);
    sub_tabs_box_->setMarginBottom(12);
    addView(sub_tabs_box_);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setWidthPercentage(100);
    stage_box_ = new brls::Box(brls::Axis::COLUMN);
    stage_box_->setWidthPercentage(100);
    scroll->setContentView(stage_box_);
    addView(scroll);

    refreshHeader();
    renderActiveTab();
}

void MusicWorkspaceView::refreshHeader() {
    if (!engine_) return;
    DrmStatus drm = engine_->cachedDrmStatus();
    if (drm.auth_state == "challenging") show_auth_panel_ = true;

    if (drm_pill_label_) {
        drm_pill_label_->setText(drmBadgeText(drm));
    }

    if (sub_tabs_box_) {
        sub_tabs_box_->clearViews();
        struct TabDef { SubTab t; const char* label; };
        const TabDef tabs[] = {
            {SubTab::ForYou, "为你推荐"},
            {SubTab::Recent, "最近常听"},
            {SubTab::Library, "资料库歌单"},
            {SubTab::Search, "搜索发现"},
            {SubTab::NowPlaying, "正在播放 & 双语歌词"},
        };
        for (const auto& td : tabs) {
            auto* b = omnidesk::ui::makeButton(td.label, [this, tab = td.t]() {
                brls::sync([this, tab]() { setSubTab(tab); });
            }, active_tab_ == td.t, 34.0f);
            b->setMarginRight(8);
            sub_tabs_box_->addView(b);
        }
    }

    if (auth_panel_box_) {
        auth_panel_box_->clearViews();
        if (!show_auth_panel_) {
            auth_panel_box_->setVisibility(brls::Visibility::GONE);
        } else {
            auth_panel_box_->setVisibility(brls::Visibility::VISIBLE);
            auto* card = omnidesk::ui::makeCard("desktop/surface", 14.0f, 10.0f);
            card->addView(omnidesk::ui::makeLabel(
                "Apple ID 原生 FairPlay 无损认证与双端 Token 管理", 15.5f, "brls/text"));

            std::string desc = "当前状态: " + drmBadgeText(drm);
            if (!drm.challenge_prompt.empty()) desc += " · " + drm.challenge_prompt;
            if (!drm.error.empty()) desc += " (" + drm.error + ")";
            auto* info = omnidesk::ui::makeLabel(desc, 13.0f, "font/grey");
            info->setMarginTop(4);
            info->setMarginBottom(8);
            card->addView(info);

            auto* row1 = new brls::Box(brls::Axis::ROW);
            auto* login_btn = omnidesk::ui::makeButton("Apple ID 账号密码登录", [this]() {
                promptAppleIdLogin();
            }, true, 34.0f);
            login_btn->setMarginRight(8);
            row1->addView(login_btn);

            auto* code_btn = omnidesk::ui::makeButton("输入 6 位 2FA 验证码", [this]() {
                promptTwoFactorCode();
            }, drm.auth_state == "challenging", 34.0f);
            code_btn->setMarginRight(8);
            row1->addView(code_btn);

            auto* resend_btn = omnidesk::ui::makeButton("重发验证码", [this]() {
                std::weak_ptr<std::atomic<bool>> alive_w = alive_;
                std::thread([eng = engine_, alive_w]() {
                    DrmStatus st;
                    std::string err;
                    bool ok = eng->resendTwoFactorCode(st, err);
                    brls::sync([alive_w, ok, err]() {
                        auto a = alive_w.lock();
                        if (!a || !a->load(std::memory_order_acquire)) return;
                        brls::Application::notify(ok ? "已请求重发 2FA 验证码" : ("重发失败: " + err));
                    });
                }).detach();
            }, false, 34.0f);
            resend_btn->setMarginRight(8);
            row1->addView(resend_btn);

            auto* chrome_btn = omnidesk::ui::makeButton("从 Chrome 导入 Token", [this]() {
                std::weak_ptr<std::atomic<bool>> alive_w = alive_;
                std::thread([eng = engine_, alive_w]() {
                    std::string err;
                    bool ok = eng->tryImportMutFromChromeCookies(err);
                    brls::sync([alive_w, ok, err]() {
                        auto a = alive_w.lock();
                        if (!a || !a->load(std::memory_order_acquire)) return;
                        brls::Application::notify(ok ? "已从 Chrome 导入 media-user-token" : err);
                    });
                }).detach();
            }, false, 34.0f);
            chrome_btn->setMarginRight(8);
            row1->addView(chrome_btn);

            auto* mut_btn = omnidesk::ui::makeButton("手动填入 media-user-token", [this]() {
                promptMediaUserToken();
            }, false, 34.0f);
            mut_btn->setMarginRight(8);
            row1->addView(mut_btn);

            auto* logout_btn = omnidesk::ui::makeButton("退出 DRM 会话", [this]() {
                std::weak_ptr<std::atomic<bool>> alive_w = alive_;
                std::thread([eng = engine_, alive_w]() {
                    std::string err;
                    bool ok = eng->logoutAppleId(err);
                    brls::sync([alive_w, ok, err]() {
                        auto a = alive_w.lock();
                        if (!a || !a->load(std::memory_order_acquire)) return;
                        brls::Application::notify(ok ? "已退出 Apple ID DRM 会话" : err);
                    });
                }).detach();
            }, false, 34.0f);
            row1->addView(logout_btn);

            card->addView(row1);
            auth_panel_box_->addView(card);
        }
    }
}

void MusicWorkspaceView::setSubTab(SubTab tab) {
    active_tab_ = tab;
    showing_container_detail_ = false;
    refreshHeader();
    if (tab == SubTab::ForYou) {
        triggerFeedLoad("recommendations");
    } else if (tab == SubTab::Recent) {
        triggerFeedLoad("heavy-rotation");
    } else if (tab == SubTab::Library) {
        triggerFeedLoad("library-playlists");
    } else if (tab == SubTab::Search) {
        if (!search_query_.empty()) triggerFeedLoad("search", search_query_);
        else renderActiveTab();
    } else {
        rendered_session_id_.clear();
        rendered_lyrics_sig_.clear();
        renderActiveTab();
    }
}

void MusicWorkspaceView::triggerFeedLoad(const std::string& feed_id, const std::string& query, bool force) {
    if (!engine_) return;
    feed_loading_ = true;
    renderActiveTab();

    std::weak_ptr<std::atomic<bool>> alive_w = alive_;
    std::thread([this, eng = engine_, alive_w, feed_id, query, force]() {
        FeedOutcome out = eng->loadFeed(feed_id, query, force);
        if (feed_id == "heavy-rotation" && (!out.ok || (out.containers.empty() && out.tracks.empty()))) {
            out = eng->loadFeed("recently-played", "", force);
        }
        brls::sync([this, alive_w, feed_id, out]() {
            auto a = alive_w.lock();
            if (!a || !a->load(std::memory_order_acquire)) return;
            feed_loading_ = false;
            current_feed_ = out;
            if (feed_id == "library-playlists" && out.ok) {
                library_playlists_ = out.containers;
            }
            renderActiveTab();
        });
    }).detach();
}

void MusicWorkspaceView::openContainerView(const ContainerItem& container) {
    if (!engine_) return;
    showing_container_detail_ = true;
    active_container_ = container;
    container_tracks_.clear();
    feed_loading_ = true;
    renderActiveTab();

    std::weak_ptr<std::atomic<bool>> alive_w = alive_;
    std::thread([this, eng = engine_, alive_w, container]() {
        std::vector<MusicTrack> tracks;
        std::string err;
        bool ok = eng->openContainer(container, tracks, err);
        brls::sync([this, alive_w, ok, tracks = std::move(tracks), err]() mutable {
            auto a = alive_w.lock();
            if (!a || !a->load(std::memory_order_acquire)) return;
            feed_loading_ = false;
            if (!ok && !err.empty()) {
                brls::Application::notify(err);
            }
            container_tracks_ = std::move(tracks);
            renderActiveTab();
        });
    }).detach();
}

void MusicWorkspaceView::renderActiveTab() {
    if (!stage_box_) return;
    if (active_tab_ == SubTab::NowPlaying) {
        renderNowPlayingBody();
        return;
    }

    mv_stage_ = nullptr;
    lyric_cards_.clear();
    lyric_orig_labels_.clear();
    lyric_times_.clear();
    stage_box_->clearViews();

    if (showing_container_detail_) {
        auto* top = new brls::Box(brls::Axis::ROW);
        top->setAlignItems(brls::AlignItems::CENTER);
        top->setMarginBottom(12);
        auto* back_b = omnidesk::ui::makeButton("← 返回列表", [this]() {
            showing_container_detail_ = false;
            renderActiveTab();
        }, false, 32.0f);
        back_b->setMarginRight(12);
        top->addView(back_b);
        top->addView(omnidesk::ui::makeLabel(
            active_container_.name + (active_container_.subtitle.empty() ? "" : (" · " + active_container_.subtitle)),
            18.0f, "brls/text"));
        stage_box_->addView(top);

        if (feed_loading_) {
            stage_box_->addView(omnidesk::ui::makeLabel("正在加载曲目列表…", 14.5f, "font/grey"));
            return;
        }
        if (container_tracks_.empty()) {
            stage_box_->addView(omnidesk::ui::makeLabel("暂无曲目（可尝试先同步资料库或确认地区设置）", 14.5f, "font/grey"));
            return;
        }

        auto* play_all = omnidesk::ui::makeButton("▶ 顺序播放全部 (" + std::to_string(container_tracks_.size()) + ")",
            [this]() {
                engine_->setQueueAndPlay(container_tracks_, 0);
                setSubTab(SubTab::NowPlaying);
            }, true, 36.0f);
        play_all->setMarginBottom(10);
        stage_box_->addView(play_all);

        for (size_t i = 0; i < container_tracks_.size(); ++i) {
            const auto& tr = container_tracks_[i];
            auto* row = omnidesk::ui::makeCard("desktop/surface", 12.0f, 8.0f);
            row->setAxis(brls::Axis::ROW);
            row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setMarginBottom(8);
            row->setFocusable(true);

            auto* info = new brls::Box(brls::Axis::COLUMN);
            info->setGrow(1.0f);
            info->addView(omnidesk::ui::makeLabel(std::to_string(i + 1) + ". " + tr.title, 15.5f, "brls/text"));
            info->addView(omnidesk::ui::makeLabel(
                tr.artist + (tr.album.empty() ? "" : (" · " + tr.album)), 13.0f, "font/grey"));
            row->addView(info);

            auto* right = new brls::Box(brls::Axis::ROW);
            if (!tr.quality_label.empty()) {
                auto* q = omnidesk::ui::makePill(tr.quality_label, "status/info_bg", "status/info_fg");
                q->setMarginRight(8);
                right->addView(q);
            }
            right->addView(omnidesk::ui::makeLabel(formatDuration(tr.duration_sec), 13.0f, "font/grey"));
            row->addView(right);

            row->registerClickAction([this, i](brls::View*) {
                engine_->setQueueAndPlay(container_tracks_, i);
                brls::sync([this]() { setSubTab(SubTab::NowPlaying); });
                return true;
            });
            stage_box_->addView(row);
        }
        return;
    }

    if (active_tab_ == SubTab::Library) {
        renderLibraryBody();
        return;
    }

    if (active_tab_ == SubTab::Search) {
        auto* sbar = new brls::Box(brls::Axis::ROW);
        sbar->setMarginBottom(12);
        auto* sb = omnidesk::ui::makeButton(
            search_query_.empty() ? "输入歌曲 / 专辑 / 艺人关键词…" : ("搜索: " + search_query_),
            [this]() { promptSearch(); }, true, 36.0f);
        sb->setMarginRight(10);
        sbar->addView(sb);
        stage_box_->addView(sbar);
    }

    renderFeedBody(current_feed_);
}

void MusicWorkspaceView::renderLibraryBody() {
    auto* bar = new brls::Box(brls::Axis::ROW);
    bar->setMarginBottom(12);
    auto* sync_b = omnidesk::ui::makeButton("同步 Apple Music 资料库", [this]() {
        std::weak_ptr<std::atomic<bool>> alive_w = alive_;
        std::thread([this, eng = engine_, alive_w]() {
            nlohmann::json st;
            std::string err;
            bool ok = eng->syncLibrary(st, err);
            brls::sync([this, alive_w, ok, st, err]() {
                auto a = alive_w.lock();
                if (!a || !a->load(std::memory_order_acquire)) return;
                if (ok) {
                    brls::Application::notify("资料库同步完成");
                    triggerFeedLoad("library-playlists", "", true);
                } else {
                    brls::Application::notify("同步失败: " + err);
                }
            });
        }).detach();
    }, true, 34.0f);
    sync_b->setMarginRight(10);
    bar->addView(sync_b);

    auto* ref_b = omnidesk::ui::makeButton("刷新歌单", [this]() {
        triggerFeedLoad("library-playlists", "", true);
    }, false, 34.0f);
    bar->addView(ref_b);
    stage_box_->addView(bar);

    renderFeedBody(current_feed_);
}

void MusicWorkspaceView::renderFeedBody(const FeedOutcome& outcome) {
    if (feed_loading_) {
        stage_box_->addView(omnidesk::ui::makeLabel("正在请求 Apple Music 数据…", 14.5f, "font/grey"));
        return;
    }

    if (!outcome.ok && outcome.containers.empty() && outcome.tracks.empty()) {
        auto* card = omnidesk::ui::makeCard("desktop/surface", 16.0f, 10.0f);
        card->addView(omnidesk::ui::makeLabel(
            outcome.error.empty() ? "请先登录 Apple ID 或输入关键词搜索 Apple Music 目录" : outcome.error,
            14.5f, "font/grey", 1.5f));
        auto* btn_row = new brls::Box(brls::Axis::ROW);
        btn_row->setMarginTop(10);
        auto* login_b = omnidesk::ui::makeButton("登录 Apple ID / 2FA", [this]() {
            show_auth_panel_ = true;
            refreshHeader();
        }, true, 34.0f);
        login_b->setMarginRight(8);
        btn_row->addView(login_b);
        btn_row->addView(omnidesk::ui::makeButton("重试加载", [this]() {
            if (active_tab_ == SubTab::ForYou) triggerFeedLoad("recommendations", "", true);
            else if (active_tab_ == SubTab::Recent) triggerFeedLoad("heavy-rotation", "", true);
            else if (active_tab_ == SubTab::Library) triggerFeedLoad("library-playlists", "", true);
            else if (active_tab_ == SubTab::Search) promptSearch();
        }, false, 34.0f));
        card->addView(btn_row);
        stage_box_->addView(card);
        return;
    }

    if (!outcome.containers.empty()) {
        stage_box_->addView(omnidesk::ui::makeLabel("专辑与歌单", 17.0f, "brls/text"));
        brls::Box* row = nullptr;
        int col = 0;
        for (const auto& c : outcome.containers) {
            if (col % 3 == 0) {
                row = new brls::Box(brls::Axis::ROW);
                row->setWidthPercentage(100);
                row->setMarginTop(12);
                stage_box_->addView(row);
            }
            auto* card = omnidesk::ui::makeCard("desktop/surface", 14.0f, 12.0f);
            card->setWidth(0);
            card->setGrow(1.0f);
            card->setShrink(1.0f);
            if (col % 3 != 2) card->setMarginRight(14);
            card->setFocusable(true);

            if (!c.cover_url.empty()) {
                auto* iv = new omnidesk::CachedImageView(&omnidesk::ImageCache::instance());
                iv->setWidthPercentage(100);
                iv->setHeight(156);
                iv->setCornerRadius(8.0f);
                iv->setScalingType(brls::ImageScalingType::FILL);
                iv->setMarginBottom(8);
                iv->setImageUrl(c.cover_url);
                card->addView(iv);
            }
            card->addView(omnidesk::ui::makeLabel(clipText(c.name, 56), 15.5f, "brls/text", 1.35f));
            std::string sub = c.subtitle.empty() ? c.type : c.subtitle;
            auto* sl = omnidesk::ui::makeLabel(clipText(sub, 48), 13.5f, "font/grey");
            sl->setMarginTop(4);
            card->addView(sl);

            card->registerClickAction([this, c](brls::View*) {
                brls::sync([this, c]() { openContainerView(c); });
                return true;
            });
            row->addView(card);
            ++col;
        }
        if (row && (col % 3) != 0) {
            for (int s = col % 3; s < 3; ++s) {
                auto* spacer = new brls::Box(brls::Axis::COLUMN);
                spacer->setWidth(0);
                spacer->setGrow(1.0f);
                spacer->setShrink(1.0f);
                spacer->setFocusable(false);
                if (s != 2) spacer->setMarginRight(14);
                row->addView(spacer);
            }
        }
    }

    if (!outcome.tracks.empty()) {
        auto* hdr = omnidesk::ui::makeLabel("单曲与 MV", 17.0f, "brls/text");
        hdr->setMarginTop(16);
        hdr->setMarginBottom(8);
        stage_box_->addView(hdr);

        for (size_t i = 0; i < outcome.tracks.size(); ++i) {
            const auto& tr = outcome.tracks[i];
            auto* row = omnidesk::ui::makeCard("desktop/surface", 14.0f, 10.0f);
            row->setAxis(brls::Axis::ROW);
            row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setMarginBottom(8);
            row->setFocusable(true);

            auto* left = new brls::Box(brls::Axis::ROW);
            left->setAlignItems(brls::AlignItems::CENTER);
            left->setGrow(1.0f);
            if (!tr.cover_url.empty()) {
                auto* iv = new omnidesk::CachedImageView(&omnidesk::ImageCache::instance());
                iv->setWidth(48);
                iv->setHeight(48);
                iv->setCornerRadius(6.0f);
                iv->setScalingType(brls::ImageScalingType::FILL);
                iv->setMarginRight(10);
                iv->setImageUrl(tr.cover_url);
                left->addView(iv);
            }
            auto* meta = new brls::Box(brls::Axis::COLUMN);
            meta->addView(omnidesk::ui::makeLabel(tr.title, 15.5f, "brls/text"));
            meta->addView(omnidesk::ui::makeLabel(
                tr.artist + (tr.album.empty() ? "" : (" · " + tr.album)), 13.5f, "font/grey"));
            left->addView(meta);
            row->addView(left);

            auto* right = new brls::Box(brls::Axis::ROW);
            if (!tr.quality_label.empty()) {
                auto* q = omnidesk::ui::makePill(tr.quality_label, "status/info_bg", "status/info_fg");
                q->setMarginRight(8);
                right->addView(q);
            }
            right->addView(omnidesk::ui::makeLabel(formatDuration(tr.duration_sec), 13.5f, "font/grey"));
            row->addView(right);

            row->registerClickAction([this, tracks = outcome.tracks, i](brls::View*) {
                engine_->setQueueAndPlay(tracks, i);
                brls::sync([this]() { setSubTab(SubTab::NowPlaying); });
                return true;
            });
            stage_box_->addView(row);
        }
    }
}

void MusicWorkspaceView::renderNowPlayingBody() {
    if (!stage_box_ || !engine_) return;

    MusicTrack track;
    PlaybackSessionInfo sess;
    LyricBundle lyr;
    std::vector<MusicTrack> queue;
    size_t qidx = 0;
    bool has_track = engine_->getActivePlaybackState(track, sess, lyr, queue, qidx);
    std::string disp_mode = engine_->displayMode();

    mv_stage_ = nullptr;
    lyric_cards_.clear();
    lyric_orig_labels_.clear();
    lyric_times_.clear();
    active_lyric_idx_ = -1;
    stage_box_->clearViews();

    if (!has_track) {
        auto* empty = omnidesk::ui::makeCard("desktop/surface", 20.0f, 12.0f);
        empty->addView(omnidesk::ui::makeLabel("尚未播放曲目", 18.0f, "brls/text"));
        auto* hint = omnidesk::ui::makeLabel(
            "请在「为你推荐 / 资料库歌单 / 搜索发现」点击任意歌曲或 MV，插件将通过 Adam FairPlay 解密无损流并交由 MPVCore 播放。",
            14.0f, "font/grey", 1.5f);
        hint->setMarginTop(8);
        empty->addView(hint);
        stage_box_->addView(empty);
        return;
    }

    auto* split = new brls::Box(brls::Axis::ROW);
    split->setWidthPercentage(100);

    // Left: Cover / MV Stage + Transport + Queue
    auto* left_card = omnidesk::ui::makeCard("desktop/surface", 16.0f, 12.0f);
    left_card->setWidthPercentage(38);
    left_card->setMarginRight(16);

    if (track.kind == "mv" && sess.ok) {
        mv_stage_ = new omnidesk::MpvStageView(&omnidesk::MPVCore::instance());
        mv_stage_->setStageMinHeight(220.0f);
        mv_stage_->setTitle(track.title);
        mv_stage_->setSubtitles(toSdkLyricLines(lyr.lines));
        left_card->addView(mv_stage_);
    } else {
        auto* art = omnidesk::ui::makeCard("desktop/active", 12.0f, 10.0f);
        art->setHeight(230);
        art->setJustifyContent(brls::JustifyContent::CENTER);
        art->setAlignItems(brls::AlignItems::CENTER);
        if (!track.cover_url.empty()) {
            auto* cover = new omnidesk::CachedImageView(&omnidesk::ImageCache::instance());
            cover->setWidthPercentage(100);
            cover->setHeight(206);
            cover->setCornerRadius(8.0f);
            cover->setScalingType(brls::ImageScalingType::FILL);
            cover->setImageUrl(track.cover_url);
            art->addView(cover);
        } else {
            art->addView(omnidesk::ui::makePill("ALAC 无损舞台", "desktop/surface", "brls/accent"));
        }
        left_card->addView(art);
    }

    auto* title_lbl = omnidesk::ui::makeLabel(track.title, 18.0f, "brls/text");
    title_lbl->setMarginTop(12);
    left_card->addView(title_lbl);

    auto* sub_lbl = omnidesk::ui::makeLabel(
        track.artist + (track.album.empty() ? "" : (" · " + track.album)), 13.5f, "font/grey");
    sub_lbl->setMarginTop(4);
    left_card->addView(sub_lbl);

    std::string q_text = sess.ok ? sess.formattedQuality()
                                 : (!sess.error.empty() ? ("播放错误: " + sess.error)
                                                        : (track.quality_label.empty() ? "准备流中…" : track.quality_label));
    auto* q_pill = omnidesk::ui::makePill(
        q_text,
        sess.error.empty() ? "status/ok_bg" : "status/warn_bg",
        sess.error.empty() ? "status/ok_fg" : "status/warn_fg");
    q_pill->setMarginTop(8);
    left_card->addView(q_pill);

    auto mpv_st = omnidesk::MPVCore::instance().state();
    auto* pos_lbl = omnidesk::ui::makeLabel(
        formatDuration(mpv_st.position_sec) + " / " +
            formatDuration(mpv_st.duration_sec > 0.0 ? mpv_st.duration_sec : track.duration_sec),
        13.0f, "font/grey");
    pos_lbl->setMarginTop(8);
    left_card->addView(pos_lbl);

    auto* controls = new brls::Box(brls::Axis::ROW);
    controls->setMarginTop(10);
    controls->addView(omnidesk::ui::makeButton("上一曲", [this]() {
        engine_->playAdjacentTrack(-1);
    }, false, 34.0f));

    auto* pp = omnidesk::ui::makeButton(mpv_st.paused ? "播放" : "暂停", [this]() {
        engine_->togglePlayPause();
    }, true, 34.0f);
    pp->setMarginLeft(8);
    controls->addView(pp);

    auto* nxt = omnidesk::ui::makeButton("下一曲", [this]() {
        engine_->playAdjacentTrack(1);
    }, false, 34.0f);
    nxt->setMarginLeft(8);
    controls->addView(nxt);
    left_card->addView(controls);

    auto* seek_row = new brls::Box(brls::Axis::ROW);
    seek_row->setMarginTop(8);
    seek_row->addView(omnidesk::ui::makeButton("-10s", []() {
        omnidesk::MPVCore::instance().seekRelative(-10.0);
    }, false, 30.0f));
    auto* fwd = omnidesk::ui::makeButton("+10s", []() {
        omnidesk::MPVCore::instance().seekRelative(10.0);
    }, false, 30.0f);
    fwd->setMarginLeft(8);
    seek_row->addView(fwd);
    auto* ll = omnidesk::ui::makeButton(
        engine_->currentConfig().lossless_enabled ? "ALAC 无损: 开" : "ALAC 无损: 关",
        [this]() {
            engine_->setLosslessEnabled(!engine_->currentConfig().lossless_enabled);
        }, false, 30.0f);
    ll->setMarginLeft(8);
    seek_row->addView(ll);
    left_card->addView(seek_row);

    if (!queue.empty()) {
        auto* qh = omnidesk::ui::makeLabel("播放队列 (" + std::to_string(queue.size()) + ")", 14.0f, "brls/text");
        qh->setMarginTop(14);
        qh->setMarginBottom(6);
        left_card->addView(qh);
        size_t max_show = std::min<size_t>(queue.size(), 8);
        for (size_t i = 0; i < max_show; ++i) {
            bool cur = (i == qidx);
            auto* qrow = omnidesk::ui::makeCard(cur ? "desktop/active" : "desktop/soft", 8.0f, 6.0f);
            qrow->setMarginBottom(4);
            qrow->setFocusable(true);
            qrow->addView(omnidesk::ui::makeLabel(
                (cur ? "▶ " : "") + clipText(queue[i].title, 36), 13.0f,
                cur ? "brls/accent" : "brls/text"));
            qrow->registerClickAction([this, queue, i](brls::View*) {
                engine_->setQueueAndPlay(queue, i);
                return true;
            });
            left_card->addView(qrow);
        }
    }
    split->addView(left_card);

    // Right: Synced Bilingual Lyrics + AI Language Explanation
    auto* right_card = omnidesk::ui::makeCard("desktop/surface", 16.0f, 12.0f);
    right_card->setWidthPercentage(60);

    auto* lhdr = new brls::Box(brls::Axis::ROW);
    lhdr->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    lhdr->setAlignItems(brls::AlignItems::CENTER);
    lhdr->addView(omnidesk::ui::makeLabel("双语同步歌词 · LRC Studio", 17.5f, "brls/text"));

    auto* mode_row = new brls::Box(brls::Axis::ROW);
    for (const auto& [m, lbl] : std::vector<std::pair<std::string, std::string>>{
             {"bilingual", "双语"}, {"original", "原文"}, {"translation", "译文"}}) {
        auto* mb = omnidesk::ui::makeButton(lbl, [this, m]() {
            engine_->setDisplayMode(m);
            renderNowPlayingBody();
        }, disp_mode == m, 28.0f);
        mb->setMarginLeft(6);
        mode_row->addView(mb);
    }
    lhdr->addView(mode_row);
    right_card->addView(lhdr);

    auto* act_row = new brls::Box(brls::Axis::ROW);
    act_row->setMarginTop(8);
    act_row->setMarginBottom(6);
    act_row->addView(omnidesk::ui::makeButton("翻译中文", [this]() {
        engine_->triggerAsyncLyricsRefresh(false, true);
    }, true, 30.0f));
    auto* retry_b = omnidesk::ui::makeButton("重查歌词", [this]() {
        engine_->triggerAsyncLyricsRefresh(true, false);
    }, false, 30.0f);
    retry_b->setMarginLeft(8);
    act_row->addView(retry_b);
    auto* imp_b = omnidesk::ui::makeButton("导入 LRC", [this]() {
        promptImportLyrics();
    }, false, 30.0f);
    imp_b->setMarginLeft(8);
    act_row->addView(imp_b);
    right_card->addView(act_row);

    std::string st_txt = lyr.status_text;
    if (!lyr.error.empty()) st_txt += (st_txt.empty() ? "" : " · ") + lyr.error;
    if (!st_txt.empty()) {
        auto* sl = omnidesk::ui::makeLabel(st_txt, 12.5f, "font/grey");
        sl->setMarginBottom(6);
        right_card->addView(sl);
    }

    if (lyr.lines.empty()) {
        right_card->addView(omnidesk::ui::makeLabel("当前暂无歌词，可点击「重查歌词」或「导入 LRC」。", 14.0f, "font/grey"));
    } else {
        const double pos_sec = mpv_st.position_sec;
        int init_idx = -1;
        for (size_t i = 0; i < lyr.lines.size(); ++i) {
            if (lyr.lines[i].has_time && pos_sec >= lyr.lines[i].time_sec) init_idx = static_cast<int>(i);
        }
        active_lyric_idx_ = init_idx;

        for (size_t i = 0; i < lyr.lines.size(); ++i) {
            const auto& line = lyr.lines[i];
            bool active_line = (static_cast<int>(i) == init_idx);
            auto* lcard = omnidesk::ui::makeCard(active_line ? "desktop/active" : "desktop/soft", 12.0f, 10.0f);
            lcard->setMarginTop(8);
            lcard->setFocusable(true);

            auto* top_row = new brls::Box(brls::Axis::ROW);
            top_row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            top_row->setAlignItems(brls::AlignItems::CENTER);

            std::string main_txt = line.original;
            if (disp_mode == "translation" && !line.translation.empty()) main_txt = line.translation;
            std::string stamp = line.has_time ? (formatDuration(line.time_sec) + "  ") : "";
            auto* orig_lbl = omnidesk::ui::makeLabel(
                stamp + main_txt, 16.5f, active_line ? "brls/accent" : "brls/text");
            orig_lbl->setGrow(1.0f);
            top_row->addView(orig_lbl);

            auto* exp_btn = omnidesk::ui::makeButton("精讲", [this, i]() {
                explainLyricAtIndex(i);
            }, false, 30.0f);
            exp_btn->setMarginLeft(8);
            top_row->addView(exp_btn);
            lcard->addView(top_row);

            if (disp_mode == "bilingual" && !line.translation.empty()) {
                auto* tr_lbl = omnidesk::ui::makeLabel(line.translation, 14.5f, "font/grey");
                tr_lbl->setMarginTop(4);
                lcard->addView(tr_lbl);
            }

            if (line.has_time) {
                double seek_t = line.time_sec;
                lcard->registerClickAction([this, seek_t](brls::View*) {
                    engine_->seekSeconds(seek_t);
                    return true;
                });
            }

            lyric_cards_.push_back(lcard);
            lyric_orig_labels_.push_back(orig_lbl);
            lyric_times_.push_back(line.has_time ? line.time_sec : -1.0);
            right_card->addView(lcard);
        }
    }

    if (has_explanation_) {
        auto* exp_card = omnidesk::ui::makeCard("desktop/soft", 12.0f, 8.0f);
        exp_card->setMarginTop(14);
        exp_card->addView(omnidesk::ui::makeLabel(
            "AI 逐句精讲 (" + last_explanation_.provider + ")", 14.5f, "brls/accent"));
        auto* body = omnidesk::ui::makeLabel(last_explanation_.explanation, 13.5f, "brls/text", 1.45f);
        body->setMarginTop(6);
        exp_card->addView(body);
        right_card->addView(exp_card);
    }

    split->addView(right_card);
    stage_box_->addView(split);
}

void MusicWorkspaceView::updateActiveLyricHighlight(double position_sec) {
    if (lyric_cards_.empty() || lyric_cards_.size() != lyric_times_.size() ||
        lyric_orig_labels_.size() != lyric_times_.size()) {
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < lyric_times_.size(); ++i) {
        if (lyric_times_[i] >= 0.0 && position_sec >= lyric_times_[i]) idx = static_cast<int>(i);
    }
    if (idx == active_lyric_idx_) return;

    if (active_lyric_idx_ >= 0 && static_cast<size_t>(active_lyric_idx_) < lyric_cards_.size()) {
        lyric_cards_[active_lyric_idx_]->setBackgroundColor(omnidesk::ui::color("desktop/soft"));
        lyric_orig_labels_[active_lyric_idx_]->setTextColor(omnidesk::ui::color("brls/text"));
    }
    active_lyric_idx_ = idx;
    if (active_lyric_idx_ >= 0 && static_cast<size_t>(active_lyric_idx_) < lyric_cards_.size()) {
        lyric_cards_[active_lyric_idx_]->setBackgroundColor(omnidesk::ui::color("desktop/active"));
        lyric_orig_labels_[active_lyric_idx_]->setTextColor(omnidesk::ui::color("brls/accent"));
    }
}

void MusicWorkspaceView::explainLyricAtIndex(size_t index, const std::string& focus_phrase) {
    if (!engine_) return;
    std::weak_ptr<std::atomic<bool>> alive_w = alive_;
    std::thread([this, eng = engine_, alive_w, index, focus_phrase]() {
        PhraseExplanation note = eng->explainLine(index, focus_phrase);
        brls::sync([this, alive_w, note]() {
            auto a = alive_w.lock();
            if (!a || !a->load(std::memory_order_acquire)) return;
            last_explanation_ = note;
            has_explanation_ = true;
            renderNowPlayingBody();
        });
    }).detach();
}

void MusicWorkspaceView::promptAppleIdLogin() {
    auto* plat = brls::Application::getPlatform();
    if (!plat) return;
    plat->getImeManager()->openForText(
        [this](std::string email) {
            if (email.empty()) return;
            pending_email_ = email;
            brls::sync([this, email]() {
                auto* plat2 = brls::Application::getPlatform();
                if (!plat2) return;
                plat2->getImeManager()->openForText(
                    [this, email](std::string password) {
                        if (password.empty()) return;
                        std::weak_ptr<std::atomic<bool>> alive_w = alive_;
                        std::thread([this, eng = engine_, alive_w, email, password]() {
                            DrmStatus st;
                            std::string err;
                            bool ok = eng->authenticateAppleId(email, password, st, err);
                            brls::sync([this, alive_w, ok, st, err]() {
                                auto a = alive_w.lock();
                                if (!a || !a->load(std::memory_order_acquire)) return;
                                refreshHeader();
                                brls::Application::notify(ok ? "已提交 Apple ID 登录，若弹出受信任设备请填写 2FA 验证码"
                                                             : ("登录失败: " + err));
                            });
                        }).detach();
                    },
                    "输入 Apple ID 密码",
                    "仅提交至本机 127.0.0.1 Adam FairPlay 引擎，不经过任何第三方服务器",
                    128,
                    "");
            });
        },
        "输入 Apple ID 邮箱",
        "用于本机 FairPlay ALAC 无损解密认证",
        128,
        pending_email_);
}

void MusicWorkspaceView::promptTwoFactorCode() {
    auto* plat = brls::Application::getPlatform();
    if (!plat) return;
    plat->getImeManager()->openForText(
        [this](std::string code) {
            if (code.empty()) return;
            std::weak_ptr<std::atomic<bool>> alive_w = alive_;
            std::thread([this, eng = engine_, alive_w, code]() {
                DrmStatus st;
                std::string err;
                bool ok = eng->submitTwoFactorCode(code, st, err);
                brls::sync([this, alive_w, ok, err]() {
                    auto a = alive_w.lock();
                    if (!a || !a->load(std::memory_order_acquire)) return;
                    refreshHeader();
                    brls::Application::notify(ok ? "2FA 验证码已提交，正在初始化 FairPlay 会话"
                                                 : ("2FA 验证失败: " + err));
                });
            }).detach();
        },
        "输入 6 位 Apple ID 验证码",
        "受信任设备或短信收到的 6 位验证码",
        16,
        "");
}

void MusicWorkspaceView::promptSearch() {
    auto* plat = brls::Application::getPlatform();
    if (!plat) return;
    plat->getImeManager()->openForText(
        [this](std::string q) {
            if (q.empty()) return;
            brls::sync([this, q]() {
                search_query_ = q;
                active_tab_ = SubTab::Search;
                showing_container_detail_ = false;
                refreshHeader();
                triggerFeedLoad("search", q, true);
            });
        },
        "搜索 Apple Music 全站曲库",
        "输入歌曲、专辑、歌单或艺人名称",
        128,
        search_query_);
}

void MusicWorkspaceView::promptImportLyrics() {
    auto* plat = brls::Application::getPlatform();
    if (!plat) return;
    plat->getImeManager()->openForText(
        [this](std::string raw) {
            if (raw.empty()) return;
            std::string err;
            engine_->importLyricsForActiveOrTrack(nullptr, raw, err);
            brls::sync([err]() {
                brls::Application::notify(err.empty() ? "已导入歌词并保存至本地缓存" : err);
            });
        },
        "导入 LRC / 纯文本歌词",
        "粘贴 UTF-8 LRC（支持双语同时间戳合并）或纯文本歌词",
        8192,
        "");
}

void MusicWorkspaceView::promptMediaUserToken() {
    auto* plat = brls::Application::getPlatform();
    if (!plat) return;
    plat->getImeManager()->openForText(
        [this](std::string mut) {
            if (mut.empty()) return;
            std::string err;
            bool ok = engine_->importMediaUserToken(mut, err);
            brls::sync([ok, err]() {
                brls::Application::notify(ok ? "media-user-token 已保存至 0600 SecretVault 并推送引擎" : err);
            });
        },
        "填入 Apple Music media-user-token",
        "用于与网页端同步个人资料库与个性化推荐",
        2048,
        "");
}

// ---- MusicCompanionDockView (F8) ----

MusicCompanionDockView::MusicCompanionDockView(std::shared_ptr<MusicEngine> engine)
    : brls::Box(brls::Axis::COLUMN), engine_(std::move(engine)) {
    setPadding(12);
    setCornerRadius(10.0f);
    setBackgroundColor(omnidesk::ui::color("desktop/soft"));
    setMarginBottom(8);

    addView(omnidesk::ui::makeLabel("Apple Music 双语精讲", 15.0f, "brls/accent"));
    track_label_ = omnidesk::ui::makeLabel("尚未播放曲目", 14.0f, "brls/text");
    track_label_->setMarginTop(4);
    addView(track_label_);

    lyric_orig_label_ = omnidesk::ui::makeLabel("等待歌词同步…", 14.0f, "brls/text", 1.38f);
    lyric_orig_label_->setMarginTop(4);
    addView(lyric_orig_label_);

    lyric_trans_label_ = omnidesk::ui::makeLabel("", 13.5f, "font/grey", 1.38f);
    lyric_trans_label_->setMarginTop(2);
    addView(lyric_trans_label_);

    auto* btns = new brls::Box(brls::Axis::ROW);
    btns->setMarginTop(8);
    auto* exp_b = omnidesk::ui::makeButton("精讲当前句", [this]() {
        if (!engine_) return;
        std::weak_ptr<std::atomic<bool>> alive_w = alive_;
        std::thread([this, eng = engine_, alive_w]() {
            MusicTrack tr; PlaybackSessionInfo s; LyricBundle lyr; std::vector<MusicTrack> q; size_t qi = 0;
            eng->getActivePlaybackState(tr, s, lyr, q, qi);
            double pos = omnidesk::MPVCore::instance().state().position_sec;
            size_t idx = 0;
            for (size_t i = 0; i < lyr.lines.size(); ++i) {
                if (lyr.lines[i].has_time && pos >= lyr.lines[i].time_sec) idx = i;
            }
            PhraseExplanation note = eng->explainLine(idx);
            brls::sync([this, alive_w, note]() {
                auto a = alive_w.lock();
                if (!a || !a->load(std::memory_order_acquire)) return;
                if (note_label_) note_label_->setText(clipText(note.explanation, 360));
            });
        }).detach();
    }, true, 28.0f);
    exp_b->setMarginRight(6);
    btns->addView(exp_b);

    btns->addView(omnidesk::ui::makeButton("翻译缺失歌词", [this]() {
        if (engine_) engine_->triggerAsyncLyricsRefresh(false, true);
    }, false, 28.0f));
    addView(btns);

    note_label_ = omnidesk::ui::makeLabel("", 12.0f, "font/grey", 1.4f);
    note_label_->setMarginTop(6);
    addView(note_label_);

    if (engine_) {
        std::weak_ptr<std::atomic<bool>> alive_w = alive_;
        engine_sub_ = engine_->subscribeState([this, alive_w]() {
            brls::sync([this, alive_w]() {
                auto a = alive_w.lock();
                if (!a || !a->load(std::memory_order_acquire)) return;
                refreshSummary();
            });
        });
        mpv_sub_ = omnidesk::MPVCore::instance().subscribeState(
            [this, alive_w](const omnidesk::MpvPlaybackState&) {
                brls::sync([this, alive_w]() {
                    auto a = alive_w.lock();
                    if (!a || !a->load(std::memory_order_acquire)) return;
                    refreshSummary();
                });
            });
        refreshSummary();
    }
}

MusicCompanionDockView::~MusicCompanionDockView() {
    alive_->store(false, std::memory_order_release);
    if (engine_ && engine_sub_ != 0) {
        engine_->unsubscribeState(engine_sub_);
        engine_sub_ = 0;
    }
    if (mpv_sub_ != 0) {
        omnidesk::MPVCore::instance().unsubscribeState(mpv_sub_);
        mpv_sub_ = 0;
    }
}

void MusicCompanionDockView::refreshSummary() {
    if (!engine_) return;
    MusicTrack tr; PlaybackSessionInfo s; LyricBundle lyr; std::vector<MusicTrack> q; size_t qi = 0;
    if (!engine_->getActivePlaybackState(tr, s, lyr, q, qi)) {
        if (track_label_) track_label_->setText("尚未播放曲目");
        return;
    }
    if (track_label_) {
        track_label_->setText(tr.title + " · " + (s.ok ? s.formattedQuality() : tr.artist));
    }
    if (lyr.lines.empty()) {
        if (lyric_orig_label_) lyric_orig_label_->setText(lyr.status_text.empty() ? "暂无歌词" : lyr.status_text);
        if (lyric_trans_label_) lyric_trans_label_->setText("");
        return;
    }
    double pos = omnidesk::MPVCore::instance().state().position_sec;
    int idx = -1;
    for (size_t i = 0; i < lyr.lines.size(); ++i) {
        if (lyr.lines[i].has_time && pos >= lyr.lines[i].time_sec) idx = static_cast<int>(i);
    }
    if (idx < 0) idx = 0;
    const auto& line = lyr.lines[static_cast<size_t>(idx)];
    if (lyric_orig_label_) lyric_orig_label_->setText(line.original);
    if (lyric_trans_label_) lyric_trans_label_->setText(line.translation);
}

} // namespace music_plugin
