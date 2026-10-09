// ps5fwdgen - Search SteamGridDB and pick art (icon grids or hero backgrounds).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Laid out with the kit's components: a SplitView whose left pane holds a
// SearchField (with recent searches) over the matched games, and whose right
// pane shows a large preview of the focused artwork over a GridView of the
// game's art. While art loads, Skeleton cards stand in for the grid and a
// ProgressBar counts the thumbnails; a Spinner covers the other jobs.
//
// Network and image decoding run on one worker thread at a time; the UI polls
// an atomic phase and a mutex-guarded payload each frame, and uploads GL
// textures on the main thread (the only place the GL context is current).

#include "app/screens/steamgriddb_screen.hpp"

#include "app/context.hpp"
#include "fwd/image.hpp"
#include "gfx/renderer.hpp"
#include "net/steamgriddb.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/list.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/search_field.hpp"
#include "ui/components/skeleton.hpp"
#include "ui/components/split_view.hpp"
#include "ui/fonts.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

extern "C" void glDeleteTextures(int n, const unsigned int *textures);

using hui::gfx::Color;
using hui::gfx::Rect;

enum class Phase
{
    query, // the search prompt is up
    browse, // the split view: games on the left, art on the right
    busy,   // a worker is running
};

enum class Job
{
    none,
    search,
    assets,
    thumbs,
    full,
};

// Which pane has the focus while browsing, and what inside the left one.
enum class Focus
{
    field,
    games,
    grid,
};

constexpr Rect kSplitBounds{96.0f, 236.0f, 1728.0f, 734.0f};
constexpr float kStageHeight = 320.0f; // the preview over the grid
constexpr int kSkeletonCards = 8;

class SteamGridScreen final : public Screen
{
  public:
    SteamGridScreen(Context &context, ArtKind kind, std::string query,
                    std::function<void(std::vector<unsigned char>)> on_image)
        : key_(context.settings.steamgriddb_key), kind_(kind), query_(std::move(query)),
          on_image_(std::move(on_image))
    {
        restyle(context);
        split_.set_bounds(kSplitBounds);
        split_.set_focus(0, true);

        search_.set_placeholder(kind_ == ArtKind::icon ? "Search a game for its icon"
                                                       : "Search a game for a background");
        search_.style.max_rows = 3;
        search_.style.pill = true;
        search_.style.empty_text = "";
        search_.style.max_length = 80;
        search_.set_text(query_);
        search_.set_active(false);

        games_.set_active(false);
        grid_.set_active(false);
        grid_.style.columns = kind_ == ArtKind::icon ? 5 : 3;
        grid_.style.card.art_aspect = kind_ == ArtKind::icon ? 1.0f : 16.0f / 9.0f;
        grid_.style.card.subtitle_size = 0.0f;
        grid_.style.card.title_size = 20.0f;
        grid_.style.card.glow = true;
        layout();

        {
            auto kb = hui::ui::KeyboardBindings::standard();
            kb.done = hui::Action::page_next; // R1 confirms (a console keyboard nicety)
            prompt_.keyboard.style.bindings = kb;
        }
        prompt_.style.buttons = false; // single Done: the keyboard's own key (closes on press)
        prompt_.set_title("Search SteamGridDB");
        prompt_.style.max_length = 80;
        hui::sys::log("[FWD] sgdb screen key_len=%zu", key_.size());
    }

    ~SteamGridScreen() override
    {
        join_worker();
        release_thumbs();
    }

    void restyle(Context &context) override
    {
        const hui::ui::Theme &theme = context.theme;
        split_.style.theme = theme;
        split_.style.ratio = 0.36f;
        split_.style.gap = 48.0f;
        split_.style.divider = true;
        split_.style.dim = 0.35f;
        search_.style.theme = theme;
        games_.style.theme = theme;
        games_.style.cards = false;
        games_.style.dividers = true;
        games_.style.row_height = 70.0f;
        games_.style.title_size = 25.0f;
        games_.style.subtitle_size = 18.0f;
        grid_.style.theme = theme;
        spinner_.style.theme = theme;
        spinner_.style.kind = hui::ui::SpinnerKind::arc;
        bar_.style.theme = theme;
        bar_.style.mode = hui::ui::ProgressMode::determinate;
        bar_.style.placement = hui::ui::LabelPlacement::above;
        bar_.style.sheen = true;
        bar_.label = "Thumbnails";
        skeleton_.style.theme = theme;
        skeleton_.style.kind = hui::ui::SkeletonKind::card;
        skeleton_.style.lines = 1;
        skeleton_.style.picture = kind_ == ArtKind::icon ? 0.78f : 0.72f;
        prompt_.style.theme = theme;
    }

    // The panes' contents, against the split's settled geometry.
    void layout()
    {
        const Rect left = split_.target_pane_rect(0);
        const Rect right = split_.target_pane_rect(1);
        search_.set_bounds({left.x, left.y, left.w, search_.preferred_height()});
        const float list_y = search_.field_rect().y + search_.field_rect().h + 28.0f;
        games_.set_bounds({left.x, list_y, left.w, left.y + left.h - list_y});
        stage_ = {right.x, right.y, right.w, kStageHeight};
        const float grid_y = stage_.y + stage_.h + 28.0f;
        grid_.set_bounds({right.x, grid_y, right.w, right.y + right.h - grid_y});
    }

    void enter(Context &context) override
    {
        (void)context;
        if (!started_)
        {
            started_ = true;
            if (key_.empty())
                message_ = "Set a SteamGridDB API key in Settings first.";
            hui::ui::Feedback discard;
            prompt_.open(discard, query_);
            phase_ = Phase::query;
        }
    }

    // ---- worker plumbing ---------------------------------------------------
    struct Shared
    {
        mutable std::mutex mutex; // draw() reads the author under it
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        std::atomic<int> progress{0}; // thumbnails downloaded so far
        std::atomic<int> total{0};    // ... out of
        std::string error;
        std::vector<sgdb::Game> games;
        std::vector<sgdb::Asset> assets;
        std::vector<std::vector<unsigned char>> thumbs; // encoded jpeg, index-aligned to assets
        std::vector<unsigned char> image;               // the chosen full image
    };

    void join_worker()
    {
        if (worker_running_)
        {
            pthread_join(worker_, nullptr);
            worker_running_ = false;
        }
    }

    static void *worker_entry(void *arg)
    {
        auto *screen = static_cast<SteamGridScreen *>(arg);
        screen->run_job();
        screen->shared_.done.store(true);
        return nullptr;
    }

    void start_job(Job job)
    {
        join_worker();
        pending_ = job;
        shared_.done.store(false);
        shared_.ok.store(false);
        shared_.progress.store(0);
        shared_.total.store(0);
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.error.clear();
        }
        phase_ = Phase::busy;
        spinner_.set_spinning(true, true);
        bar_.set_value(0.0f, true);
        search_.set_busy(job == Job::search);
        if (job == Job::assets)
            skeleton_.set_loaded(false, true);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        if (pthread_create(&worker_, &attr, worker_entry, this) == 0)
            worker_running_ = true;
        else
            shared_.done.store(true);
        pthread_attr_destroy(&attr);
    }

    void run_job()
    {
        switch (pending_)
        {
        case Job::search:
        {
            std::vector<sgdb::Game> games;
            const sgdb::Result result = sgdb::search(key_, query_, games);
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.games = std::move(games);
            shared_.error = result.error;
            shared_.ok.store(result.ok);
            break;
        }
        case Job::assets:
        {
            std::vector<sgdb::Asset> assets;
            const sgdb::Result result =
                sgdb::assets(key_, kind_ == ArtKind::icon ? sgdb::Kind::icon : sgdb::Kind::background,
                             selected_game_, assets);
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.assets = std::move(assets);
            shared_.error = result.error;
            shared_.ok.store(result.ok);
            break;
        }
        case Job::thumbs:
        {
            std::vector<std::vector<unsigned char>> thumbs;
            std::vector<sgdb::Asset> assets_copy;
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                assets_copy = shared_.assets;
            }
            thumbs.resize(assets_copy.size());
            shared_.total.store(static_cast<int>(assets_copy.size()));
            for (std::size_t i = 0; i < assets_copy.size(); ++i)
            {
                const std::string &url =
                    assets_copy[i].thumb.empty() ? assets_copy[i].url : assets_copy[i].thumb;
                sgdb::download(url, thumbs[i]);
                shared_.progress.store(static_cast<int>(i + 1));
            }
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.thumbs = std::move(thumbs);
            shared_.ok.store(true);
            break;
        }
        case Job::full:
        {
            std::vector<unsigned char> image;
            const sgdb::Result result = sgdb::download(full_url_, image);
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.image = std::move(image);
            shared_.error = result.error;
            shared_.ok.store(result.ok);
            break;
        }
        case Job::none:
            break;
        }
    }

    // ---- after a worker finishes ------------------------------------------
    void on_job_done(Context &context)
    {
        join_worker();
        const Job job = pending_;
        pending_ = Job::none;
        const bool ok = shared_.ok.load();
        search_.set_busy(false);
        hui::sys::log("[FWD] sgdb job=%d ok=%d", static_cast<int>(job), ok ? 1 : 0);
        if (!ok)
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            message_ = shared_.error.empty() ? "Request failed" : shared_.error;
            hui::sys::log("[FWD] sgdb error: %s", message_.c_str());
            phase_ = Phase::browse;
            set_focus(job == Job::search ? Focus::field : Focus::games);
            return;
        }
        if (job == Job::search)
        {
            std::vector<hui::ui::ListItem> items;
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                for (const sgdb::Game &game : shared_.games)
                    items.push_back({game.name, "SteamGridDB #" + std::to_string(game.id), "", "",
                                     {}, true, false, false, 0});
            }
            hui::sys::log("[FWD] sgdb games=%zu", items.size());
            search_.add_recent(query_);
            search_.set_text(query_);
            release_thumbs();
            grid_.set_items({});
            loaded_game_ = 0;
            games_.set_items(std::move(items));
            games_.set_focus(0, true);
            message_ = games_.items().empty() ? "No matches. Try another name." : "";
            phase_ = Phase::browse;
            set_focus(games_.items().empty() ? Focus::field : Focus::games);
        }
        else if (job == Job::assets)
        {
            std::size_t count;
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                count = shared_.assets.size();
            }
            if (count == 0)
            {
                message_ = "No art for that game.";
                phase_ = Phase::browse;
                set_focus(Focus::games);
                return;
            }
            start_job(Job::thumbs); // download thumbnails next
        }
        else if (job == Job::thumbs)
        {
            upload_thumbs(context);
            loaded_game_ = selected_game_;
            skeleton_.set_loaded(true);
            grid_.enter();
            phase_ = Phase::browse;
            set_focus(Focus::grid);
        }
        else if (job == Job::full)
        {
            std::vector<unsigned char> image;
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                image = std::move(shared_.image);
            }
            if (on_image_)
                on_image_(std::move(image));
            finished_ = true;
        }
    }

    void release_thumbs()
    {
        if (!thumb_textures_.empty())
            glDeleteTextures(static_cast<int>(thumb_textures_.size()), thumb_textures_.data());
        thumb_textures_.clear();
    }

    void upload_thumbs(Context &context)
    {
        std::vector<sgdb::Asset> assets_copy;
        std::vector<std::vector<unsigned char>> thumbs;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            assets_copy = shared_.assets;
            thumbs = std::move(shared_.thumbs);
        }
        release_thumbs();
        std::vector<hui::ui::CardItem> cards;
        cards.reserve(assets_copy.size());
        char text[64];
        for (std::size_t i = 0; i < assets_copy.size(); ++i)
        {
            hui::ui::CardItem card;
            card.title = assets_copy[i].style.empty() ? "grid" : assets_copy[i].style;
            if (assets_copy[i].width > 0 && assets_copy[i].height > 0)
            {
                (void)std::snprintf(text, sizeof(text), "%dx%d", assets_copy[i].width,
                                    assets_copy[i].height);
                card.badge = text;
            }
            if (i < thumbs.size() && !thumbs[i].empty())
            {
                int w = 0;
                int h = 0;
                std::vector<unsigned char> rgba;
                if (fwd::decode_image(thumbs[i].data(), thumbs[i].size(), w, h, rgba) && w > 0 &&
                    h > 0)
                {
                    card.texture = context.renderer.batch().create_texture(w, h, rgba.data());
                    card.image_aspect = static_cast<float>(w) / static_cast<float>(h);
                    thumb_textures_.push_back(card.texture);
                }
            }
            cards.push_back(std::move(card));
        }
        hui::sys::log("[FWD] sgdb thumbs=%zu", cards.size());
        grid_.set_items(std::move(cards));
        grid_.set_focus(0, true);
        message_ = "";
    }

    // ---- focus ----------------------------------------------------------------
    void set_focus(Focus focus)
    {
        focus_ = focus;
        hui::sys::log("[FWD] sgdb focus=%d", static_cast<int>(focus));
        search_.set_active(focus == Focus::field);
        games_.set_active(focus == Focus::games);
        grid_.set_active(focus == Focus::grid);
        split_.set_focus(focus == Focus::grid ? 1 : 0);
    }

    void load_art_for_focused_game()
    {
        const int index = games_.focus();
        long id = 0;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            if (index >= 0 && index < static_cast<int>(shared_.games.size()))
                id = shared_.games[static_cast<std::size_t>(index)].id;
        }
        if (id == 0)
            return;
        if (id == loaded_game_ && !grid_.items().empty())
        {
            set_focus(Focus::grid); // already loaded: just go there
            return;
        }
        selected_game_ = id;
        start_job(Job::assets);
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (finished_)
        {
            context.pop();
            return;
        }
        split_.update(dt);
        search_.update(dt);
        skeleton_.update(dt);
        if (phase_ == Phase::busy)
        {
            spinner_.update(dt);
            const int total = shared_.total.load();
            if (pending_ == Job::thumbs && total > 0)
                bar_.set_value(static_cast<float>(shared_.progress.load()) /
                               static_cast<float>(total));
            bar_.update(dt);
            games_.update(dt);
            grid_.update(dt);
            if (shared_.done.load())
                on_job_done(context);
            return;
        }

        if (prompt_.is_open())
        {
            const hui::ui::Event event = prompt_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                query_ = prompt_.text();
                search_.set_text(query_);
                if (!query_.empty())
                    start_job(Job::search);
                else
                {
                    phase_ = Phase::browse;
                    set_focus(Focus::field);
                }
            }
            else if (event == hui::ui::Event::cancelled)
            {
                if (games_.items().empty())
                {
                    context.pop();
                    return;
                }
                phase_ = Phase::browse;
                set_focus(Focus::games);
            }
            prompt_.update(dt);
            return;
        }
        prompt_.update(dt);

        if (input.is_pressed(hui::Action::back))
        {
            if (focus_ == Focus::grid)
                set_focus(Focus::games);
            else
                context.pop();
            return;
        }
        if (input.is_pressed(hui::Action::north)) // Triangle: search again
        {
            prompt_.open(feedback, query_);
            return;
        }

        switch (focus_)
        {
        case Focus::field:
        {
            if (input.nav == hui::Direction::down && !games_.items().empty())
            {
                set_focus(Focus::games);
                feedback.play(hui::audio::Cue::focus);
                break;
            }
            const hui::ui::Event event = search_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                if (search_.has_pick())
                {
                    query_ = search_.picked().text;
                    search_.set_text(query_);
                    start_job(Job::search);
                }
                else
                    prompt_.open(feedback, search_.text());
            }
            break;
        }
        case Focus::games:
        {
            if (input.nav == hui::Direction::up && games_.focus() == 0)
            {
                set_focus(Focus::field);
                feedback.play(hui::audio::Cue::focus);
                break;
            }
            if (input.nav == hui::Direction::right && !grid_.items().empty())
            {
                load_art_for_focused_game();
                break;
            }
            const hui::ui::Event event = games_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
                load_art_for_focused_game();
            break;
        }
        case Focus::grid:
        {
            if (input.nav == hui::Direction::left && grid_.focus() % grid_.columns() == 0)
            {
                set_focus(Focus::games);
                feedback.play(hui::audio::Cue::focus);
                break;
            }
            const hui::ui::Event event = grid_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                const int index = grid_.focus();
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (index >= 0 && index < static_cast<int>(shared_.assets.size()))
                    full_url_ = shared_.assets[static_cast<std::size_t>(index)].url;
            }
            if (event == hui::ui::Event::activated && !full_url_.empty())
                start_job(Job::full);
            break;
        }
        }
        games_.update(dt);
        grid_.update(dt);
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        (void)context;
        if (!prompt_.is_open())
            return false;
        query_ = text;
        search_.set_text(query_);
        prompt_.dismiss();
        if (!query_.empty())
            start_job(Job::search);
        return true;
    }

    // ---- drawing --------------------------------------------------------------
    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        const Color muted = theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        hui::ui::text(scene.list, context.fonts.display, "SteamGridDB", 96.0f, 150.0f, 46.0f, text);
        hui::ui::text(scene.list, context.fonts.regular,
                      kind_ == ArtKind::icon ? "Pick a square grid for the tile icon."
                                             : "Pick a hero image for the background.",
                      96.0f, 200.0f, 24.0f, muted);
        if (!message_.empty())
            hui::ui::text(scene.list, context.fonts.regular, message_, 1824.0f, 200.0f, 24.0f,
                          theme.warning.a > 0.0f ? theme.warning : muted, hui::gfx::Align::right);

        split_.draw(scene);
        draw_left(context, scene);
        draw_right(context, scene);

        const bool modal = prompt_.visible();
        prompt_.draw(overlay);
        return modal;
    }

    void draw_left(Context &context, hui::ui::Canvas &scene) const
    {
        (void)context;
        scene.list.push_opacity(split_.pane_opacity(0));
        search_.draw(scene);
        if (!games_.items().empty())
            games_.draw(scene);
        else if (phase_ != Phase::busy)
            hui::ui::text(scene.list, context.fonts.regular, "Matched games appear here.",
                          games_.bounds().x + 26.0f, games_.bounds().y + 44.0f, 22.0f,
                          (context.theme.page_text_muted.a > 0.0f ? context.theme.page_text_muted
                                                                  : context.theme.text_muted));
        scene.list.pop_opacity();
    }

    void draw_right(Context &context, hui::ui::Canvas &scene) const
    {
        hui::gfx::DrawList &list = scene.list;
        const hui::ui::Fonts &fonts = context.fonts;
        const hui::ui::Theme &theme = context.theme;
        const Color muted = theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        const Color white = Color::rgb(0xffffff);
        list.push_opacity(split_.pane_opacity(1));

        const bool loading_art =
            phase_ == Phase::busy && (pending_ == Job::assets || pending_ == Job::thumbs);
        const bool downloading = phase_ == Phase::busy && pending_ == Job::full;

        // The preview stage: the focused artwork, large.
        const std::vector<hui::ui::CardItem> &cards = grid_.items();
        const int focus = grid_.focus();
        const hui::ui::CardItem *card =
            (!cards.empty() && focus >= 0 && focus < static_cast<int>(cards.size()))
                ? &cards[static_cast<std::size_t>(focus)]
                : nullptr;
        const float aspect = kind_ == ArtKind::icon ? 1.0f : 16.0f / 9.0f;
        const float h = stage_.h - 48.0f; // room under it for the caption
        const float w = h * aspect;
        const Rect frame{stage_.x, stage_.y, w, h};
        if (card != nullptr && card->texture != 0 && !loading_art)
        {
            list.shadow({frame.x, frame.y + 14.0f, frame.w, frame.h}, 22.0f, 30.0f,
                        Color::rgb(0x000000, 0.5f));
            list.image(card->texture, frame, hui::gfx::kFullUv, white, 22.0f);
            list.bordered_rect(frame, 22.0f, Color::rgb(0x000000, 0.0f), 1.5f,
                               white.with_alpha(0.18f));
            std::string caption = card->title;
            if (!card->badge.empty())
                caption += "  \xC2\xB7  " + card->badge;
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (focus < static_cast<int>(shared_.assets.size()) &&
                    !shared_.assets[static_cast<std::size_t>(focus)].author.empty())
                    caption += "  \xC2\xB7  by " + shared_.assets[static_cast<std::size_t>(focus)].author;
            }
            hui::ui::text(list, fonts.regular, fonts.regular.font->fit(caption, 22.0f, stage_.w),
                          stage_.x, stage_.y + stage_.h - 12.0f, 22.0f, muted);
            if (downloading)
            {
                list.rounded_rect(frame, 22.0f, Color::rgb(0x0b0d16, 0.55f));
                spinner_.set_bounds({frame.cx() - 28.0f, frame.cy() - 28.0f, 56.0f, 56.0f});
                spinner_.draw(scene);
                hui::ui::text(list, fonts.regular, "Downloading the image...", frame.cx(),
                              frame.cy() + 60.0f, 24.0f, white, hui::gfx::Align::center);
            }
        }
        else
        {
            // No artwork yet: a quiet panel across the pane with a word, or
            // the loading state.
            const Rect frame{stage_.x, stage_.y, stage_.w, h};
            list.rounded_rect(frame, 22.0f, white.with_alpha(0.05f));
            list.bordered_rect(frame, 22.0f, Color::rgb(0x000000, 0.0f), 1.5f, white.with_alpha(0.1f));
            if (loading_art)
            {
                spinner_.set_bounds({frame.cx() - 28.0f, frame.cy() - 52.0f, 56.0f, 56.0f});
                spinner_.draw(scene);
                hui::ui::text(list, fonts.regular,
                              pending_ == Job::assets ? "Finding artwork..." : "Loading artwork...",
                              frame.cx(), frame.cy() + 36.0f, 24.0f, muted, hui::gfx::Align::center);
                if (pending_ == Job::thumbs && shared_.total.load() > 0)
                {
                    bar_.set_bounds({frame.x + 48.0f, frame.y + frame.h - 72.0f, frame.w - 96.0f, 44.0f});
                    bar_.draw(scene);
                }
            }
            else
            {
                hui::ui::text(list, fonts.regular,
                              games_.items().empty() ? "Search for a game to browse its art."
                                                     : "Choose a game to see its art.",
                              frame.cx(), frame.cy() + 8.0f, 24.0f, muted, hui::gfx::Align::center);
            }
        }

        // The grid, or skeleton cards standing in for it while it loads.
        if (loading_art)
        {
            const Rect area = grid_.bounds();
            const int columns = grid_.style.columns;
            const float gap = grid_.style.gap_x;
            const float cw = (area.w - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
            const float ch = cw / aspect + 40.0f;
            for (int i = 0; i < kSkeletonCards; ++i)
            {
                const int c = i % columns;
                const int r = i / columns;
                const Rect cell{area.x + static_cast<float>(c) * (cw + gap),
                                area.y + static_cast<float>(r) * (ch + gap), cw, ch};
                if (cell.y + cell.h > area.y + area.h)
                    break;
                skeleton_.set_bounds(cell);
                skeleton_.draw(scene);
            }
        }
        else if (!cards.empty())
        {
            grid_.draw(scene);
        }
        list.pop_opacity();
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kBrowse[] = {
            {hui::ui::Button::cross, "Select"},
            {hui::ui::Button::triangle, "Search"},
            {hui::ui::Button::circle, "Back"},
        };
        return kBrowse;
    }

  private:
    std::string key_;
    ArtKind kind_;
    std::string query_;
    std::function<void(std::vector<unsigned char>)> on_image_;

    Phase phase_ = Phase::query;
    Focus focus_ = Focus::field;
    Job pending_ = Job::none;
    bool started_ = false;
    bool finished_ = false;
    long selected_game_ = 0; // the game whose art is being fetched
    long loaded_game_ = 0;   // the game whose art the grid holds
    std::string full_url_;
    mutable std::string message_;
    Rect stage_{0.0f, 0.0f, 0.0f, 0.0f};

    Shared shared_;
    pthread_t worker_{};
    bool worker_running_ = false;
    std::vector<std::uint32_t> thumb_textures_;

    mutable hui::ui::SplitView split_;
    mutable hui::ui::SearchField search_;
    mutable hui::ui::ListView games_;
    mutable hui::ui::GridView grid_;
    mutable hui::ui::InputPrompt prompt_;
    mutable hui::ui::Spinner spinner_;
    mutable hui::ui::ProgressBar bar_;
    mutable hui::ui::Skeleton skeleton_;
};

} // namespace

std::unique_ptr<Screen>
make_steamgriddb_screen(Context &context, ArtKind kind, std::string query,
                        std::function<void(std::vector<unsigned char>)> on_image)
{
    return std::make_unique<SteamGridScreen>(context, kind, std::move(query), std::move(on_image));
}

} // namespace fwd
