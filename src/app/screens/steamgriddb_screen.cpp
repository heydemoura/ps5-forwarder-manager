// ps5fwdgen - Search SteamGridDB and pick art (icon grids or hero backgrounds).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Laid out after ps5-homebrew-ui's "Storefront" concept (src/concepts/
// store.cpp), a shop window on deep charcoal:
//   - a top bar with the accent mark, the tracked name and the query as a pill;
//   - a featured banner: the focused artwork on the right in a wide crop that
//     drifts slowly, its words and a "Use this image" call to action on the
//     left, cross-fading when the focus moves;
//   - filter chips: the matched games, one ink pill gliding between them;
//     the active game's art fills the grid;
//   - a grid of product cards (cover, style, size in the mono face), scrolled
//     as one sheet with the chips sticking under the top bar, one focus
//     plate gliding between banner, chips and cards;
//   - Cross on a card opens the product page: the cover grows from the card
//     into the page's preview over the blurred shop, with the words and a
//     frosted box holding the primary button that fetches the full image.
//
// Network and image decoding run on one worker thread at a time; the UI polls
// an atomic phase and a mutex-guarded payload each frame, and uploads GL
// textures on the main thread (the only place the GL context is current).

#include "app/screens/steamgriddb_screen.hpp"

#include "app/context.hpp"
#include "core/tween.hpp"
#include "fwd/image.hpp"
#include "gfx/renderer.hpp"
#include "net/steamgriddb.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/progress.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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

// ---- the design language (Storefront) ----------------------------------------
const Color kWhite = Color::rgb(0xffffff);
const Color kBlack = Color::rgb(0x000000);
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kInk = Color::rgb(0xf4efe6);      // warm white: all text
const Color kAccent = Color::rgb(0xff9f43);   // calls to action
const Color kOnAccent = Color::rgb(0x201407); // text on the accent
const Color kCoal = Color::rgb(0x0e0f12);     // backdrop, top
const Color kCoalLow = Color::rgb(0x17181d);  // backdrop, bottom
const Color kPanel = Color::rgb(0x202228);    // plates and panels

constexpr float kWidth = hui::gfx::kVirtualWidth;
constexpr float kHeight = hui::gfx::kVirtualHeight;
constexpr float kMargin = 96.0f;
constexpr float kRight = kWidth - kMargin;
constexpr float kBarY = 78.0f;
constexpr float kPageTop = 104.0f;
constexpr float kBannerY = 116.0f;
constexpr float kBannerH = 412.0f;
constexpr float kBannerArtW = 820.0f;
constexpr float kBannerRadius = 28.0f;
constexpr float kChipsY = 552.0f;
constexpr float kChipH = 48.0f;
constexpr float kStickY = 116.0f;
constexpr float kGridY = 628.0f;
constexpr int kColumns = 5;
constexpr float kCardGap = 24.0f;
constexpr float kCardW = (kWidth - 2.0f * kMargin - (kColumns - 1) * kCardGap) / kColumns;
constexpr float kCoverH = 184.0f;
constexpr float kCardH = 272.0f;
constexpr float kCardRadius = 14.0f;
constexpr float kRowPitch = 300.0f;
constexpr float kLift = 0.05f;
constexpr float kViewBottom = 968.0f;
constexpr float kFadeFoot = 72.0f;
constexpr float kStuckTop = kStickY + kChipH + 28.0f;
// The product page.
constexpr Rect kPreview{96.0f, 128.0f, 600.0f, 600.0f};
constexpr float kPreviewRadius = 28.0f;
constexpr float kInfoX = 768.0f;
constexpr Rect kBuyBox{1336.0f, 392.0f, 488.0f, 360.0f};
constexpr Rect kBuyButton{1368.0f, 600.0f, 424.0f, 72.0f};
constexpr float kButtonRadius = 18.0f;

// A baseline that centres `size` type on `y`.
float centred(float y, float size)
{
    return y + size * 0.36f;
}

enum class Phase
{
    query, // the search prompt is up
    shop,  // the storefront
    busy,  // a worker is running
};

enum class Job
{
    none,
    search,
    assets,
    thumbs,
    full,
};

enum class Zone
{
    banner,
    chips,
    grid,
    page,
};

struct Card
{
    std::uint32_t texture = 0;
    float aspect = 1.0f;
    std::string style;
    std::string size; // "1024x1024"
    std::string author;
    Color accent = kAccent;
};

class SteamGridScreen final : public Screen
{
  public:
    SteamGridScreen(Context &context, ArtKind kind, std::string query,
                    std::function<void(std::vector<unsigned char>)> on_image)
        : key_(context.settings.steamgriddb_key), kind_(kind), query_(std::move(query)),
          on_image_(std::move(on_image))
    {
        restyle(context);
        {
            auto kb = hui::ui::KeyboardBindings::standard();
            kb.done = hui::Action::page_next; // R1 confirms (a console keyboard nicety)
            prompt_.keyboard.style.bindings = kb;
        }
        prompt_.style.buttons = false; // single Done: the keyboard's own key (closes on press)
        prompt_.set_title("Search SteamGridDB");
        prompt_.style.max_length = 80;
        palette_[0].snap(kCoal);
        palette_[1].snap(kCoalLow);
        palette_[2].snap(kPanel);
        tone_.snap(kPanel);
        ring_.snap(banner_action());
        ring_radius_.snap(26.0f);
        chip_pill_.snap({kMargin, kChipsY, 0.0f, kChipH});
        hui::sys::log("[FWD] sgdb screen key_len=%zu", key_.size());
    }

    ~SteamGridScreen() override
    {
        join_worker();
        release_cards();
    }

    void restyle(Context &context) override
    {
        spinner_.style.theme = context.theme;
        spinner_.style.kind = hui::ui::SpinnerKind::arc;
        spinner_.style.color = kAccent;
        bar_.style.theme = context.theme;
        bar_.style.mode = hui::ui::ProgressMode::determinate;
        bar_.style.placement = hui::ui::LabelPlacement::above;
        bar_.style.color = kAccent;
        bar_.style.sheen = true;
        bar_.label = "Thumbnails";
        prompt_.style.theme = context.theme;
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

    // The backdrop stays charcoal; only its undertone follows the focused art.
    void backdrop(Context &context, hui::gfx::BackdropSpec &spec) const override
    {
        (void)context;
        spec.mode = hui::gfx::BackdropMode::gradient;
        spec.colors[0] = palette_[0].value();
        spec.colors[1] = palette_[1].value();
        spec.colors[2] = palette_[2].value();
        spec.params[0] = 0.72f;
        spec.params[1] = 0.3f;
        spec.params[2] = 0.26f;
        spec.time = clock_;
    }

    // ---- worker plumbing ---------------------------------------------------
    struct Shared
    {
        mutable std::mutex mutex;
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
        hui::sys::log("[FWD] sgdb job=%d ok=%d", static_cast<int>(job), ok ? 1 : 0);
        if (!ok)
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            message_ = shared_.error.empty() ? "Request failed" : shared_.error;
            hui::sys::log("[FWD] sgdb error: %s", message_.c_str());
            phase_ = Phase::shop;
            return;
        }
        if (job == Job::search)
        {
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                games_ = shared_.games;
            }
            hui::sys::log("[FWD] sgdb games=%zu", games_.size());
            release_cards();
            loaded_game_ = 0;
            chip_ = 0;
            counts_.assign(games_.size(), -1);
            message_ = games_.empty() ? "No matches. Press Triangle to search again." : "";
            phase_ = Phase::shop;
            scroll_.snap(0.0f);
            if (!games_.empty())
            {
                zone_ = Zone::chips;
                chip_pill_pending_snap_ = true;
                selected_game_ = games_[0].id;
                start_job(Job::assets); // the first match's art fills the shelf
            }
            else
            {
                zone_ = Zone::banner;
            }
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
                set_count(selected_game_, 0);
                release_cards();
                loaded_game_ = selected_game_;
                phase_ = Phase::shop;
                return;
            }
            start_job(Job::thumbs); // download thumbnails next
        }
        else if (job == Job::thumbs)
        {
            upload_cards(context);
            loaded_game_ = selected_game_;
            set_count(selected_game_, static_cast<int>(cards_.size()));
            phase_ = Phase::shop;
            focus_ = 0;
            shelf_age_ = 0.0f;
            banner_set_ = false;
            show_banner(0, 1.0f);
            if (zone_ == Zone::chips && !cards_.empty())
                zone_ = Zone::grid; // the shelf is the thing to look at now
            apply_palette(false);
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

    void set_count(long game, int count)
    {
        for (std::size_t i = 0; i < games_.size(); ++i)
            if (games_[i].id == game)
                counts_[i] = count;
    }

    void release_cards()
    {
        std::vector<std::uint32_t> textures;
        for (const Card &card : cards_)
            if (card.texture != 0)
                textures.push_back(card.texture);
        if (!textures.empty())
            glDeleteTextures(static_cast<int>(textures.size()), textures.data());
        cards_.clear();
        lift_.clear();
    }

    void upload_cards(Context &context)
    {
        std::vector<sgdb::Asset> assets_copy;
        std::vector<std::vector<unsigned char>> thumbs;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            assets_copy = shared_.assets;
            thumbs = std::move(shared_.thumbs);
        }
        release_cards();
        cards_.reserve(assets_copy.size());
        char text[64];
        for (std::size_t i = 0; i < assets_copy.size(); ++i)
        {
            Card card;
            card.style = assets_copy[i].style.empty() ? "grid" : assets_copy[i].style;
            card.author = assets_copy[i].author;
            if (assets_copy[i].width > 0 && assets_copy[i].height > 0)
            {
                (void)std::snprintf(text, sizeof(text), "%dx%d", assets_copy[i].width,
                                    assets_copy[i].height);
                card.size = text;
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
                    card.aspect = static_cast<float>(w) / static_cast<float>(h);
                    card.accent = average_accent(rgba);
                }
            }
            cards_.push_back(std::move(card));
        }
        lift_.assign(cards_.size(), hui::tween::Spring{});
        hui::sys::log("[FWD] sgdb thumbs=%zu", cards_.size());
        message_ = "";
    }

    // A lively colour from the picture, for its glow: the mean, pushed toward
    // saturation; the accent when the picture is too grey to light anything.
    static Color average_accent(const std::vector<unsigned char> &rgba)
    {
        double r = 0.0, g = 0.0, b = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4 * 7)
        {
            r += rgba[i];
            g += rgba[i + 1];
            b += rgba[i + 2];
            ++n;
        }
        if (n == 0)
            return kAccent;
        const Color c{static_cast<float>(r / static_cast<double>(n) / 255.0),
                      static_cast<float>(g / static_cast<double>(n) / 255.0),
                      static_cast<float>(b / static_cast<double>(n) / 255.0), 1.0f};
        const float mx = std::max({c.r, c.g, c.b});
        const float mn = std::min({c.r, c.g, c.b});
        if (mx - mn < 0.08f)
            return kAccent;
        const float scale = 0.95f / std::max(mx, 0.01f);
        return {std::min(1.0f, c.r * scale), std::min(1.0f, c.g * scale),
                std::min(1.0f, c.b * scale), 1.0f};
    }

    // ---- geometry -------------------------------------------------------------
    static Rect banner_rect()
    {
        return {kMargin, kBannerY, kWidth - 2.0f * kMargin, kBannerH};
    }
    static Rect banner_action()
    {
        const Rect b = banner_rect();
        return {b.x + 56.0f, b.y + 324.0f, 214.0f, 52.0f};
    }
    static Rect card_rect(int index)
    {
        return {kMargin + static_cast<float>(index % kColumns) * (kCardW + kCardGap),
                kGridY + static_cast<float>(index / kColumns) * kRowPitch, kCardW, kCardH};
    }
    float chips_y() const
    {
        return std::max(kChipsY - scroll_.value, kStickY);
    }
    std::string chip_label(const hui::ui::Fonts &fonts, std::size_t i) const
    {
        return fonts.semibold.font->fit(games_[i].name, 22.0f, 260.0f);
    }
    // Chip rectangles on the row, left to right; widths follow their labels.
    void chip_layout(const hui::ui::Fonts &fonts, std::vector<Rect> &out) const
    {
        out.clear();
        char number[8];
        float x = kMargin + hui::ui::button_width(hui::ui::Button::l2, 30.0f) + 16.0f;
        for (std::size_t i = 0; i < games_.size(); ++i)
        {
            const std::string label = chip_label(fonts, i);
            (void)std::snprintf(number, sizeof(number), "%d", std::max(0, counts_[i]));
            const float w = 48.0f + fonts.semibold.measure(label, 22.0f) + 10.0f +
                            (counts_[i] >= 0 ? fonts.mono.measure(number, 18.0f) : 0.0f);
            out.push_back({x, chips_y(), w, kChipH});
            x += w + 12.0f;
        }
    }
    // The row scrolls sideways so the active chip stays in view.
    float chips_offset(const hui::ui::Fonts &fonts) const
    {
        std::vector<Rect> chips;
        chip_layout(fonts, chips);
        if (chips.empty() || chip_ >= static_cast<int>(chips.size()))
            return 0.0f;
        const Rect &c = chips[static_cast<std::size_t>(chip_)];
        const float limit = kRight - hui::ui::button_width(hui::ui::Button::r2, 30.0f) - 16.0f;
        return c.x + c.w > limit ? c.x + c.w - limit : 0.0f;
    }
    float window_top() const
    {
        return chips_y() + kChipH + 24.0f;
    }
    Rect focus_target(const hui::ui::Fonts &fonts) const
    {
        if (zone_ == Zone::banner)
            return banner_action();
        if (zone_ == Zone::chips)
        {
            std::vector<Rect> chips;
            chip_layout(fonts, chips);
            if (chip_ < static_cast<int>(chips.size()))
            {
                Rect c = chips[static_cast<std::size_t>(chip_)];
                c.x -= chips_offset(fonts);
                return c.inset(-4.0f);
            }
        }
        if (!cards_.empty())
            return card_rect(focus_).inset(-10.0f); // the scroll is applied at draw time
        return banner_action();
    }
    float focus_radius() const
    {
        return zone_ == Zone::banner ? 26.0f : zone_ == Zone::chips ? kChipH * 0.5f + 4.0f : 22.0f;
    }
    float scroll_target() const
    {
        if (zone_ != Zone::grid || cards_.empty() || focus_ / kColumns == 0)
            return 0.0f;
        // The focused row sits at the top of the window, under the stuck chips.
        return card_rect(focus_).y - kStuckTop;
    }

    // ---- input ------------------------------------------------------------------
    void refuse(hui::ui::Feedback &feedback, float dx, float dy)
    {
        nudge_.trigger();
        nudge_x_ = dx;
        nudge_y_ = dy;
        feedback.play(hui::audio::Cue::error);
    }

    void show_banner(int index, float direction)
    {
        if (index == banner_ && banner_set_)
            return;
        banner_previous_ = banner_;
        banner_ = index;
        banner_set_ = true;
        banner_direction_ = direction;
        banner_fade_.start(0.55f);
        drift_previous_ = drift_;
        drift_ = 0.0f;
    }

    void apply_palette(bool snap)
    {
        Color accent = kAccent;
        if (!cards_.empty() && banner_ >= 0 && banner_ < static_cast<int>(cards_.size()))
            accent = cards_[static_cast<std::size_t>(banner_)].accent;
        const Color dark = hui::gfx::mix(accent, kBlack, 0.7f);
        const Color targets[3] = {hui::gfx::mix(kCoal, dark, 0.3f),
                                  hui::gfx::mix(kCoalLow, dark, 0.12f),
                                  hui::gfx::mix(dark, accent, 0.15f)};
        for (int i = 0; i < 3; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
        const Color tone = hui::gfx::mix(kPanel, dark, 0.5f);
        if (snap)
            tone_.snap(tone);
        else
            tone_.target(tone);
    }

    void step_chip(int delta, hui::ui::Feedback &feedback)
    {
        const int count = static_cast<int>(games_.size());
        if (count == 0)
            return;
        const int next = chip_ + delta;
        if (next < 0 || next >= count)
        {
            refuse(feedback, static_cast<float>(delta), 0.0f);
            return;
        }
        chip_ = next;
        feedback.play(hui::audio::Cue::tab);
        selected_game_ = games_[static_cast<std::size_t>(chip_)].id;
        if (selected_game_ != loaded_game_)
            start_job(Job::assets);
    }

    void open_page(hui::ui::Feedback &feedback)
    {
        if (cards_.empty())
            return;
        page_item_ = focus_;
        zone_ = Zone::page;
        page_open_ = true;
        page_.start(0.55f);
        feedback.play(hui::audio::Cue::open);
    }

    void close_page(hui::ui::Feedback &feedback)
    {
        page_open_ = false;
        page_.start(0.4f);
        zone_ = Zone::grid;
        feedback.play(hui::audio::Cue::back);
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (finished_)
        {
            context.pop();
            return;
        }
        animate(context, dt);
        if (phase_ == Phase::busy)
        {
            spinner_.update(dt);
            const int total = shared_.total.load();
            if (pending_ == Job::thumbs && total > 0)
                bar_.set_value(static_cast<float>(shared_.progress.load()) /
                               static_cast<float>(total));
            bar_.update(dt);
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
                if (!query_.empty())
                    start_job(Job::search);
                else
                    phase_ = Phase::shop;
            }
            else if (event == hui::ui::Event::cancelled)
            {
                if (games_.empty())
                {
                    context.pop();
                    return;
                }
                phase_ = Phase::shop;
            }
            prompt_.update(dt);
            return;
        }
        prompt_.update(dt);

        if (zone_ == Zone::page)
        {
            update_page(input, feedback);
            return;
        }
        if (input.is_pressed(hui::Action::back))
        {
            context.pop();
            return;
        }
        if (input.is_pressed(hui::Action::north)) // Triangle: search again
        {
            prompt_.open(feedback, query_);
            return;
        }
        if (input.is_pressed(hui::Action::jump_prev)) // L2 / R2 step the game chips
            step_chip(-1, feedback);
        if (input.is_pressed(hui::Action::jump_next))
            step_chip(1, feedback);

        switch (zone_)
        {
        case Zone::banner:
            if (input.nav == hui::Direction::down)
            {
                if (games_.empty())
                    refuse(feedback, 0.0f, 1.0f);
                else
                {
                    zone_ = Zone::chips;
                    feedback.play(hui::audio::Cue::focus);
                }
            }
            else if (input.nav != hui::Direction::none)
                refuse(feedback,
                       input.nav == hui::Direction::left    ? -1.0f
                       : input.nav == hui::Direction::right ? 1.0f
                                                            : 0.0f,
                       input.nav == hui::Direction::up ? -1.0f : 0.0f);
            if (input.is_pressed(hui::Action::confirm) && !cards_.empty())
            {
                focus_ = banner_;
                open_page(feedback);
            }
            break;
        case Zone::chips:
            if (input.nav == hui::Direction::left)
                step_chip(-1, feedback);
            else if (input.nav == hui::Direction::right)
                step_chip(1, feedback);
            else if (input.nav == hui::Direction::up)
            {
                zone_ = Zone::banner;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (input.nav == hui::Direction::down)
            {
                if (cards_.empty())
                    refuse(feedback, 0.0f, 1.0f);
                else
                {
                    zone_ = Zone::grid;
                    feedback.play(hui::audio::Cue::focus);
                }
            }
            if (input.is_pressed(hui::Action::confirm))
            {
                if (cards_.empty())
                    refuse(feedback, 0.0f, 0.0f);
                else
                {
                    zone_ = Zone::grid;
                    feedback.play(hui::audio::Cue::select);
                }
            }
            break;
        case Zone::grid:
            navigate_grid(input, feedback);
            break;
        case Zone::page:
            break;
        }
    }

    void navigate_grid(const hui::InputFrame &input, hui::ui::Feedback &feedback)
    {
        const int count = static_cast<int>(cards_.size());
        if (count == 0)
        {
            zone_ = Zone::chips;
            return;
        }
        int next = focus_;
        float dx = 0.0f;
        float dy = 0.0f;
        switch (input.nav)
        {
        case hui::Direction::left:
            dx = -1.0f;
            if (focus_ % kColumns > 0)
                next = focus_ - 1;
            break;
        case hui::Direction::right:
            dx = 1.0f;
            if (focus_ % kColumns < kColumns - 1 && focus_ + 1 < count)
                next = focus_ + 1;
            break;
        case hui::Direction::up:
            dy = -1.0f;
            if (focus_ >= kColumns)
                next = focus_ - kColumns;
            else
            {
                zone_ = Zone::chips;
                feedback.play(hui::audio::Cue::focus);
                return;
            }
            break;
        case hui::Direction::down:
            dy = 1.0f;
            if (focus_ + kColumns < count)
                next = focus_ + kColumns;
            else if (focus_ / kColumns < (count - 1) / kColumns)
                next = count - 1;
            break;
        case hui::Direction::none:
            break;
        }
        if (input.nav != hui::Direction::none)
        {
            if (next == focus_)
                refuse(feedback, dx, dy);
            else
            {
                focus_ = next;
                feedback.play(hui::audio::Cue::focus, 1.0f,
                              -0.4f + 0.8f * static_cast<float>(focus_ % kColumns) /
                                           static_cast<float>(kColumns - 1));
                show_banner(focus_, dx != 0.0f ? dx : 1.0f);
                apply_palette(false);
            }
        }
        if (input.is_pressed(hui::Action::confirm))
            open_page(feedback);
    }

    void update_page(const hui::InputFrame &input, hui::ui::Feedback &feedback)
    {
        if (input.is_pressed(hui::Action::back))
        {
            close_page(feedback);
            return;
        }
        if (input.nav != hui::Direction::none)
            page_nudge_.trigger();
        if (input.is_pressed(hui::Action::confirm))
        {
            press_.trigger();
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (page_item_ >= 0 && page_item_ < static_cast<int>(shared_.assets.size()))
                    full_url_ = shared_.assets[static_cast<std::size_t>(page_item_)].url;
            }
            if (!full_url_.empty())
            {
                feedback.play(hui::audio::Cue::select);
                start_job(Job::full);
            }
        }
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        (void)context;
        if (!prompt_.is_open())
            return false;
        query_ = text;
        prompt_.dismiss();
        if (!query_.empty())
            start_job(Job::search);
        return true;
    }

    // ---- animation ----------------------------------------------------------------
    void animate(Context &context, float dt)
    {
        clock_ += dt;
        age_ += dt;
        shelf_age_ += dt;
        const float omega = context.settings.reduced_motion ? 40.0f : 14.0f;
        banner_fade_.update(dt);
        page_.update(dt);
        nudge_.update(dt);
        page_nudge_.update(dt);
        press_.update(dt, 10.0f);
        // The banner's crop drifts while it is not being looked at.
        const float drift_rate = zone_ == Zone::banner || page_open_ ? 0.0f : 0.012f;
        drift_ = std::min(1.0f, drift_ + drift_rate * dt);
        drift_previous_ = std::min(1.0f, drift_previous_ + drift_rate * dt);
        for (hui::ui::SpringColor &c : palette_)
            c.update(dt, 6.0f);
        tone_.update(dt, 6.0f);
        scroll_.target = scroll_target();
        scroll_.update(dt, omega);
        // The focus plate, its radius and the chip pill.
        ring_.target(focus_target(context.fonts));
        ring_.update(dt, 18.0f);
        ring_radius_.target = focus_radius();
        ring_radius_.update(dt, 18.0f);
        plate_.target = zone_ == Zone::grid && !cards_.empty() ? 1.0f : 0.0f;
        plate_.update(dt, 16.0f);
        std::vector<Rect> chips;
        chip_layout(context.fonts, chips);
        if (chip_ < static_cast<int>(chips.size()))
        {
            Rect pill = chips[static_cast<std::size_t>(chip_)];
            pill.x -= chips_offset(context.fonts);
            if (chip_pill_pending_snap_)
            {
                chip_pill_.snap(pill);
                chip_pill_pending_snap_ = false;
            }
            else
                chip_pill_.target(pill);
        }
        chip_pill_.update(dt, 18.0f);
        for (std::size_t i = 0; i < lift_.size(); ++i)
        {
            lift_[i].target = zone_ == Zone::grid && static_cast<int>(i) == focus_ ? 1.0f : 0.0f;
            lift_[i].update(dt, 16.0f);
        }
        banner_focus_.target = zone_ == Zone::banner ? 1.0f : 0.0f;
        banner_focus_.update(dt, 16.0f);
    }

    // ---- drawing ------------------------------------------------------------------
    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        hui::gfx::DrawList &list = scene.list;
        draw_top_bar(context, list);
        draw_banner(context, list);
        draw_grid(context, list);
        draw_chips(context, list);
        draw_hints(context, list);
        const bool page = page_.running || page_open_;
        if (page)
            draw_page(context, overlay.list, overlay.glass);
        const bool modal = prompt_.visible();
        prompt_.draw(overlay);
        return page || modal;
    }

    void draw_top_bar(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = hui::tween::stagger(age_, 0, 0.07f, 0.5f);
        list.push_opacity(in);
        const float y = kBarY - 8.0f * (1.0f - in);
        list.rotated_rect({kMargin + 2.0f, y - 11.0f, 22.0f, 22.0f}, 5.0f, 0.7854f, kAccent);
        list.rotated_rect({kMargin + 8.0f, y - 5.0f, 10.0f, 10.0f}, 2.0f, 0.7854f, kCoal);
        hui::ui::text(list, fonts.semibold, "STEAMGRIDDB", kMargin + 42.0f, centred(y, 22.0f),
                      22.0f, kInk, hui::gfx::Align::left, 5.0f);
        hui::ui::text(list, fonts.regular,
                      kind_ == ArtKind::icon ? "Square grids for the tile icon"
                                             : "Hero images for the background",
                      kMargin + 330.0f, centred(y, 20.0f), 20.0f, kInk.with_alpha(0.55f));

        // The query as a pill at the right, where the concept keeps its cart.
        if (!query_.empty())
        {
            const std::string label = fonts.semibold.font->fit(query_, 20.0f, 420.0f);
            const float w = fonts.semibold.measure(label, 20.0f) + 64.0f;
            const Rect pill{kRight - w, y - 22.0f, w, 44.0f};
            list.rounded_rect(pill, 22.0f, kInk.with_alpha(0.08f));
            list.bordered_rect(pill, 22.0f, kClear, 1.5f, kInk.with_alpha(0.2f));
            // A small magnifier.
            list.arc(pill.x + 22.0f, pill.cy() - 1.0f, 6.0f, 2.0f, 0.0f, 6.2831853f,
                     kInk.with_alpha(0.8f));
            list.rounded_rect({pill.x + 26.0f, pill.cy() + 3.0f, 7.0f, 2.5f}, 1.0f,
                              kInk.with_alpha(0.8f));
            hui::ui::text(list, fonts.semibold, label, pill.x + 42.0f, centred(pill.cy(), 20.0f),
                          20.0f, kInk.with_alpha(0.9f));
        }
        if (!message_.empty())
            hui::ui::text(list, fonts.regular, message_, kRight, kBarY + 46.0f, 20.0f,
                          kAccent.with_alpha(0.9f), hui::gfx::Align::right);
        list.pop_opacity();
    }

    const std::string &active_game_name() const
    {
        return chip_ < static_cast<int>(games_.size()) ? games_[static_cast<std::size_t>(chip_)].name
                                                       : query_;
    }

    // One artwork's words in the banner, at an opacity and an offset so two
    // of them can cross-fade.
    void draw_banner_text(Context &context, hui::gfx::DrawList &list, int index, const Rect &b,
                          float alpha, float slide) const
    {
        if (alpha <= 0.01f || index < 0 || index >= static_cast<int>(cards_.size()))
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        const Card &card = cards_[static_cast<std::size_t>(index)];
        const float x = b.x + 56.0f + slide;
        list.push_opacity(alpha);
        list.rounded_rect({x, b.y + 71.0f, 28.0f, 3.0f}, 1.5f, kAccent);
        hui::ui::text(list, fonts.semibold, kind_ == ArtKind::icon ? "TILE ICON" : "BACKGROUND",
                      x + 40.0f, b.y + 80.0f, 18.0f, kAccent, hui::gfx::Align::left, 4.0f);
        hui::ui::text(list, fonts.display, fonts.display.font->fit(active_game_name(), 66.0f, 900.0f),
                      x - 3.0f, b.y + 164.0f, 66.0f, kInk);
        std::string pitch = card.style;
        if (!card.author.empty())
            pitch += "  \xC2\xB7  by " + card.author;
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(pitch, 27.0f, 860.0f), x,
                      b.y + 212.0f, 27.0f, kInk.with_alpha(0.78f));
        // The "price line": the picture's size, in the mono face.
        if (!card.size.empty())
        {
            hui::ui::text(list, fonts.mono, card.size, x, b.y + 296.0f, 40.0f, kInk);
            const float w = fonts.mono.measure(card.size, 40.0f);
            hui::ui::text(list, fonts.regular, "pixels", x + w + 14.0f, b.y + 296.0f, 22.0f,
                          kInk.with_alpha(0.6f));
        }
        list.pop_opacity();
    }

    // A wide crop of the cover that drifts slowly across it.
    Rect banner_uv(int index, float drift) const
    {
        if (index < 0 || index >= static_cast<int>(cards_.size()))
            return hui::gfx::kFullUv;
        const Card &card = cards_[static_cast<std::size_t>(index)];
        const float want = kBannerArtW / kBannerH; // the art slot's aspect
        if (card.aspect >= want)
        {
            const float w = want / card.aspect; // a full-height window drifting sideways
            return {(1.0f - w) * (0.3f + 0.4f * drift), 0.0f, w, 1.0f};
        }
        const float h = card.aspect / want; // a full-width window drifting up and down
        return {0.0f, (1.0f - h) * (0.35f + 0.3f * drift), 1.0f, h};
    }

    void draw_banner(Context &context, hui::gfx::DrawList &list) const
    {
        const float scroll = scroll_.value;
        const float in = hui::tween::stagger(age_, 1, 0.07f, 0.6f);
        const float visible = in * hui::tween::clamp01(1.0f - scroll / 360.0f);
        if (visible <= 0.004f)
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        Rect b = banner_rect();
        b.y += 28.0f * (1.0f - in) - scroll;
        const float fade = banner_fade_.running ? banner_fade_.progress() : 1.0f;
        const Color tone = tone_.value();
        const bool loading_art =
            phase_ == Phase::busy && (pending_ == Job::assets || pending_ == Job::thumbs);
        const bool searching = phase_ == Phase::busy && pending_ == Job::search;
        const bool featured = !cards_.empty() && !loading_art;

        list.push_opacity(visible);
        list.shadow({b.x, b.y + 18.0f, b.w, b.h}, kBannerRadius, 44.0f, kBlack.with_alpha(0.5f));
        list.rounded_rect(b, kBannerRadius, tone);
        const Rect art{b.x + b.w - kBannerArtW, b.y, kBannerArtW, b.h};
        if (featured)
        {
            const int now_index = std::clamp(banner_, 0, static_cast<int>(cards_.size()) - 1);
            const Card &now = cards_[static_cast<std::size_t>(now_index)];
            if (fade < 1.0f && banner_previous_ >= 0 &&
                banner_previous_ < static_cast<int>(cards_.size()))
            {
                const Card &before = cards_[static_cast<std::size_t>(banner_previous_)];
                if (before.texture != 0)
                    list.image(before.texture, art, banner_uv(banner_previous_, drift_previous_),
                               kWhite, kBannerRadius);
            }
            if (now.texture != 0)
                list.image(now.texture, art, banner_uv(now_index, drift_),
                           kWhite.with_alpha(hui::tween::smoothstep(fade)), kBannerRadius);
            // The plate's own colour runs into the picture (the words may run
        // under that run-in), so they sit
            // on a calm surface and the artwork has no left edge.
            list.gradient_rect_h({art.x - 1.0f, b.y, 360.0f, b.h}, 0.0f, tone,
                                 tone.with_alpha(0.0f));
            list.glow({b.x + 180.0f, b.y + 170.0f, 420.0f, 80.0f}, 40.0f, 130.0f,
                      now.accent.with_alpha(0.07f * hui::tween::smoothstep(fade)));
        }
        else
        {
            list.gradient_rect_h(art, kBannerRadius, tone.with_alpha(0.0f), kInk.with_alpha(0.04f));
        }
        list.bordered_rect(b, kBannerRadius, kClear, 1.5f, kInk.with_alpha(0.1f));

        if (featured)
        {
            if (fade < 1.0f)
            {
                // The old words leave quickly; the new ones arrive a beat later.
                const float side = banner_direction_;
                draw_banner_text(context, list, banner_previous_, b,
                                 1.0f - hui::tween::smoothstep(fade * 2.4f),
                                 -40.0f * side *
                                     hui::tween::cubic_in(hui::tween::clamp01(fade * 2.4f)));
                const float arrive = hui::tween::clamp01((fade - 0.3f) / 0.7f);
                draw_banner_text(context, list, banner_, b, hui::tween::smoothstep(arrive),
                                 48.0f * side * (1.0f - hui::tween::quint_out(arrive)));
            }
            else
            {
                draw_banner_text(context, list, banner_, b, 1.0f, 0.0f);
            }

            // The call to action lights up when the banner has the focus.
            const float lit = banner_focus_.value;
            const Rect action = banner_action();
            const Rect at{action.x, action.y - scroll + 28.0f * (1.0f - in), action.w, action.h};
            list.bordered_rect(at, 26.0f, hui::gfx::mix(kInk.with_alpha(0.06f), kAccent, lit), 1.5f,
                               hui::gfx::mix(kInk.with_alpha(0.3f), kAccent, lit));
            hui::ui::text(list, fonts.semibold, "Use this image", at.cx(), centred(at.cy(), 22.0f),
                          22.0f, hui::gfx::mix(kInk.with_alpha(0.86f), kOnAccent, lit),
                          hui::gfx::Align::center);

            // Where the banner's picture sits among the results.
            char text[32];
            (void)std::snprintf(text, sizeof(text), "%d / %d", banner_ + 1,
                                static_cast<int>(cards_.size()));
            const float tw = fonts.mono.measure(text, 18.0f);
            const float cy = b.y + b.h - 34.0f;
            list.rounded_rect({b.x + b.w - 44.0f - tw - 28.0f, cy - 13.0f, tw + 28.0f, 26.0f},
                              13.0f, kBlack.with_alpha(0.38f));
            hui::ui::text(list, fonts.mono, text, b.x + b.w - 44.0f - 14.0f, centred(cy, 18.0f),
                          18.0f, kInk, hui::gfx::Align::right);
        }
        else
        {
            // Nothing to feature yet: the loading state, or an invitation.
            const float x = b.x + 56.0f;
            list.rounded_rect({x, b.y + 71.0f, 28.0f, 3.0f}, 1.5f, kAccent);
            hui::ui::text(list, fonts.semibold, kind_ == ArtKind::icon ? "TILE ICON" : "BACKGROUND",
                          x + 40.0f, b.y + 80.0f, 18.0f, kAccent, hui::gfx::Align::left, 4.0f);
            const char *title = searching       ? "Searching..."
                                : loading_art   ? "Stocking the shelf..."
                                : games_.empty() ? "Nothing here yet"
                                                 : "No art for that game";
            hui::ui::text(list, fonts.display, title, x - 3.0f, b.y + 164.0f, 66.0f, kInk);
            const char *pitch =
                searching     ? "Asking SteamGridDB for matching games."
                : loading_art ? (pending_ == Job::assets ? "Finding its artwork."
                                                         : "Fetching the thumbnails.")
                : games_.empty() ? "Press Triangle and type a game's name."
                                 : "Try another game from the chips.";
            hui::ui::text(list, fonts.regular, pitch, x, b.y + 212.0f, 27.0f,
                          kInk.with_alpha(0.78f));
            if (phase_ == Phase::busy)
            {
                hui::ui::Canvas canvas{list, fonts, 0, clock_};
                spinner_.set_bounds({x, b.y + 262.0f, 48.0f, 48.0f});
                spinner_.draw(canvas);
                if (pending_ == Job::thumbs && shared_.total.load() > 0)
                {
                    bar_.set_bounds({x + 72.0f, b.y + 262.0f, 520.0f, 44.0f});
                    bar_.draw(canvas);
                }
            }
        }
        list.pop_opacity();
    }

    void draw_chips(Context &context, hui::gfx::DrawList &list) const
    {
        if (games_.empty())
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        const hui::ui::GlyphStyle glyphs = hui::ui::GlyphStyle::dark();
        const float in = hui::tween::stagger(age_, 3, 0.07f, 0.5f);
        std::vector<Rect> chips;
        chip_layout(fonts, chips);
        const float offset = chips_offset(fonts);
        const float cy = chips_y() + kChipH * 0.5f;
        char number[8];
        list.push_opacity(in);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 16.0f * (1.0f - in));
        // The row sits on a plate once it is stuck under the top bar.
        const float stuck = hui::tween::clamp01((kChipsY - chips_y()) / 80.0f);
        if (stuck > 0.01f)
            list.gradient_rect({0.0f, 0.0f, kWidth, chips_y() + kChipH + 24.0f}, 0.0f,
                               kCoal.with_alpha(0.95f * stuck), kCoal.with_alpha(0.0f));
        hui::ui::draw_button(list, fonts, glyphs, hui::ui::Button::l2, kMargin, cy, 30.0f);
        const float left = kMargin + hui::ui::button_width(hui::ui::Button::l2, 30.0f) + 8.0f;
        const float right = kRight - hui::ui::button_width(hui::ui::Button::r2, 30.0f) - 8.0f;
        list.push_clip({left, chips_y() - 8.0f, right - left, kChipH + 16.0f});

        // The active game is one ink pill that glides between the chips.
        Rect pill = chip_pill_.value();
        pill.x += hui::ui::shake(nudge_.value, clock_) * (zone_ == Zone::chips ? nudge_x_ : 0.0f);
        list.rounded_rect(pill, kChipH * 0.5f, kInk);
        // Outlines, then labels, then counts.
        for (int pass = 0; pass < 3; ++pass)
        {
            for (std::size_t i = 0; i < chips.size(); ++i)
            {
                Rect chip = chips[i];
                chip.x -= offset;
                if (chip.x + chip.w < left || chip.x > right)
                    continue;
                const float overlap =
                    std::min(pill.x + pill.w, chip.x + chip.w) - std::max(pill.x, chip.x);
                const float on = hui::tween::clamp01(overlap / chip.w);
                const Color ink = hui::gfx::mix(kInk.with_alpha(0.78f), kCoal, on);
                const std::string label = chip_label(fonts, i);
                const float lw = fonts.semibold.measure(label, 22.0f);
                if (pass == 0)
                    list.bordered_rect(chip, kChipH * 0.5f, kClear, 1.5f,
                                       kInk.with_alpha(0.2f * (1.0f - on)));
                else if (pass == 1)
                    hui::ui::text(list, fonts.semibold, label, chip.x + 24.0f, centred(cy, 22.0f),
                                  22.0f, ink);
                else if (counts_[i] >= 0)
                {
                    (void)std::snprintf(number, sizeof(number), "%d", counts_[i]);
                    hui::ui::text(list, fonts.mono, number, chip.x + 34.0f + lw, centred(cy, 18.0f),
                                  18.0f, ink.with_alpha(0.6f));
                }
            }
        }
        list.pop_clip();
        hui::ui::draw_button(list, fonts, glyphs, hui::ui::Button::r2,
                             kRight - hui::ui::button_width(hui::ui::Button::r2, 30.0f), cy, 30.0f);
        list.pop_transform();
        list.pop_opacity();
    }

    // How visible a y on the page is: faded under the chips and above the hints.
    float fade_at(float y) const
    {
        const float top = window_top();
        if (y < top)
            return 0.0f;
        if (y < top + 24.0f)
            return (y - top) / 24.0f;
        if (y > kViewBottom)
            return 0.0f;
        if (y > kViewBottom - kFadeFoot)
            return (kViewBottom - y) / kFadeFoot;
        return 1.0f;
    }

    // The part of the picture a card's cover shows: a centred band of the
    // cover's aspect.
    static Rect cover_uv(const Card &card)
    {
        const float want = kCardW / kCoverH;
        if (card.aspect >= want)
        {
            const float w = want / card.aspect;
            return {(1.0f - w) * 0.5f, 0.0f, w, 1.0f};
        }
        const float h = card.aspect / want;
        return {0.0f, (1.0f - h) * 0.5f, 1.0f, h};
    }

    // One product card: cover, style, size. The cover's tint runs from the
    // visibility at its top to the one at its bottom, which gives the grid a
    // per-pixel soft edge without painting over the backdrop.
    void draw_card(Context &context, hui::gfx::DrawList &list, int k, const Rect &r,
                   float alpha) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const Card &card = cards_[static_cast<std::size_t>(k)];
        const float top = alpha * fade_at(r.y);
        const float foot = alpha * fade_at(r.y + kCoverH);
        if (top <= 0.01f && foot <= 0.01f)
            return;
        const float lift = lift_[static_cast<std::size_t>(k)].value;
        const Rect cover{r.x, r.y, r.w, kCoverH};
        // While the page is open the cover has left its card: an empty slot.
        const bool travelling = (page_.running || page_open_) && k == page_item_;
        if (travelling)
        {
            list.gradient_rect(cover, kCardRadius, kInk.with_alpha(0.05f * top),
                               kInk.with_alpha(0.05f * foot));
        }
        else if (card.texture != 0)
        {
            const float light = 0.84f + 0.16f * lift; // resting covers sit back a little
            list.image_gradient(card.texture, cover, cover_uv(card),
                                Color{light, light, light, top}, Color{light, light, light, foot},
                                kCardRadius);
        }
        else
        {
            list.gradient_rect(cover, kCardRadius, kPanel.with_alpha(top), kPanel.with_alpha(foot));
        }
        // On the focus plate the words step in from its edge.
        const float x = r.x + 2.0f + 10.0f * lift;
        const float title = alpha * fade_at(r.y + kCoverH + 38.0f);
        if (title > 0.01f)
            hui::ui::text(list, fonts.semibold,
                          fonts.semibold.font->fit(card.style, 24.0f, r.w - 8.0f), x,
                          r.y + kCoverH + 38.0f, 24.0f,
                          kInk.with_alpha(title * (0.84f + 0.16f * lift)));
        const float price = alpha * fade_at(r.y + kCoverH + 74.0f);
        if (price > 0.01f && !card.size.empty())
            hui::ui::text(list, fonts.mono, card.size, x, r.y + kCoverH + 74.0f, 22.0f,
                          kInk.with_alpha(0.7f * price));
    }

    void draw_grid_card(Context &context, hui::gfx::DrawList &list, int k, float scroll) const
    {
        Rect r = card_rect(k);
        r.y -= scroll;
        if (r.y > kViewBottom || r.y + r.h < kPageTop)
            return;
        const float in =
            hui::tween::stagger(shelf_age_, k % kColumns + 2 * (k / kColumns), 0.05f, 0.45f);
        const float lift = lift_[static_cast<std::size_t>(k)].value;
        const float nudge =
            k == focus_ && zone_ == Zone::grid ? hui::ui::shake(nudge_.value, clock_) : 0.0f;
        list.push_transform(1.0f + kLift * lift, r.cx(), r.cy(), nudge * nudge_x_,
                            nudge * nudge_y_ + 26.0f * (1.0f - in));
        draw_card(context, list, k, r, in);
        list.pop_transform();
    }

    void draw_grid(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float scroll = scroll_.value;
        const Rect window{0.0f, window_top(), kWidth, kViewBottom - window_top()};
        const int count = static_cast<int>(cards_.size());
        const int focused = zone_ == Zone::grid && count > 0 ? focus_ : -1;
        const bool loading_art =
            phase_ == Phase::busy && (pending_ == Job::assets || pending_ == Job::thumbs);

        list.push_clip(window);
        if (loading_art)
        {
            // Plates where the cards will be, a band of light crossing them.
            const float band = std::fmod(clock_ * 0.6f, 1.6f) - 0.3f;
            for (int k = 0; k < kColumns * 2; ++k)
            {
                Rect r = card_rect(k);
                r.y -= scroll;
                const float u = (r.cx() - kMargin) / (kWidth - 2.0f * kMargin);
                const float shimmer = std::max(0.0f, 1.0f - std::fabs(u - band) * 4.0f);
                list.rounded_rect({r.x, r.y, r.w, kCoverH}, kCardRadius,
                                  kInk.with_alpha((0.05f + 0.05f * shimmer) * fade_at(r.y)));
                list.rounded_rect({r.x, r.y + kCoverH + 22.0f, r.w * 0.6f, 14.0f}, 6.0f,
                                  kInk.with_alpha(0.06f * fade_at(r.y + kCoverH + 22.0f)));
                list.rounded_rect({r.x, r.y + kCoverH + 58.0f, r.w * 0.35f, 12.0f}, 6.0f,
                                  kInk.with_alpha(0.05f * fade_at(r.y + kCoverH + 58.0f)));
            }
        }
        for (int k = 0; k < count; ++k)
            if (k != focused) // that one is drawn last, on top of its neighbours
                draw_grid_card(context, list, k, scroll);
        if (count == 0 && !loading_art && !games_.empty() && phase_ != Phase::busy)
        {
            const float in = hui::tween::stagger(age_, 5, 0.07f, 0.5f);
            hui::ui::text(list, fonts.semibold, "Nothing on this shelf", 960.0f, 760.0f, 30.0f,
                          kInk.with_alpha(0.86f * in), hui::gfx::Align::center);
            hui::ui::text(list, fonts.regular,
                          "SteamGridDB has no art of this kind for that game.", 960.0f, 802.0f,
                          24.0f, kInk.with_alpha(0.6f * in), hui::gfx::Align::center);
        }
        list.pop_clip();

        // The focus highlight: one object that glides between the banner, the
        // chips and the cards. On a card it is also the plate, the shadow and
        // the light under it.
        if (zone_ == Zone::page)
            return;
        const float in = hui::tween::stagger(age_, 4, 0.07f, 0.5f);
        Rect ring = ring_.value();
        const float shake = hui::ui::shake(nudge_.value, clock_);
        ring.x += shake * nudge_x_;
        ring.y += shake * nudge_y_ - (zone_ == Zone::grid ? scroll : 0.0f);
        const float radius = ring_radius_.value;
        const float plate = plate_.value * in;
        if (plate > 0.01f && focused >= 0)
        {
            const Color accent = cards_[static_cast<std::size_t>(focused)].accent;
            list.shadow({ring.x, ring.y + 16.0f, ring.w, ring.h}, radius, 38.0f,
                        kBlack.with_alpha(0.55f * plate));
            list.glow(ring, radius, 30.0f,
                      accent.with_alpha((0.26f + 0.12f * hui::ui::breathe(clock_)) * plate));
            list.rounded_rect(ring, radius, kPanel.with_alpha(plate));
        }
        list.bordered_rect(ring, radius, kClear, 3.0f, kInk.with_alpha(0.94f * in));
        if (focused >= 0)
        {
            list.push_clip(window);
            draw_grid_card(context, list, focused, scroll);
            list.pop_clip();
        }
    }

    // ---- the product page --------------------------------------------------------
    void draw_page(Context &context, hui::gfx::DrawList &list, std::uint32_t glass) const
    {
        const float t = page_open_ ? page_.progress() : 1.0f - page_.progress();
        if (t <= 0.004f || page_item_ < 0 || page_item_ >= static_cast<int>(cards_.size()))
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        const Card &card = cards_[static_cast<std::size_t>(page_item_)];
        const Color dark = hui::gfx::mix(card.accent, kBlack, 0.7f);

        // The whole shop, blurred and darkened, is the page's wall.
        const Rect full{0.0f, 0.0f, kWidth, kHeight};
        const float veil = hui::tween::clamp01(t * 1.7f);
        if (glass != 0)
            list.glass(glass, full, 0.0f, kWhite.with_alpha(veil));
        list.rounded_rect(full, 0.0f, hui::gfx::mix(kCoal, dark, 0.3f).with_alpha(0.87f * veil));
        list.glow(kPreview.inset(60.0f), 60.0f, 220.0f, card.accent.with_alpha(0.1f * veil));

        // The shared element: the card's cover grows into the preview, its
        // crop opening up to the whole picture.
        const float grow = hui::tween::quint_out(t);
        Rect from = card_rect(page_item_);
        from.y -= scroll_.value;
        from.h = kCoverH;
        Rect preview = kPreview;
        if (kind_ != ArtKind::icon)
        {
            // A hero keeps its shape: as wide as the square, 16:9, centred on it.
            preview.h = kPreview.w * 9.0f / 16.0f;
            preview.y = kPreview.y + (kPreview.h - preview.h) * 0.5f;
        }
        const Rect at{from.x + (preview.x - from.x) * grow, from.y + (preview.y - from.y) * grow,
                      from.w + (preview.w - from.w) * grow, from.h + (preview.h - from.h) * grow};
        const Rect uv0 = cover_uv(card);
        const Rect uv{uv0.x * (1.0f - grow), uv0.y * (1.0f - grow), uv0.w + (1.0f - uv0.w) * grow,
                      uv0.h + (1.0f - uv0.h) * grow};
        const float radius = kCardRadius + (kPreviewRadius - kCardRadius) * grow;
        list.shadow({at.x, at.y + 20.0f, at.w, at.h}, radius, 50.0f,
                    kBlack.with_alpha(0.5f * grow));
        if (card.texture != 0)
            list.image(card.texture, at, uv, kWhite, radius);
        else
            list.rounded_rect(at, radius, kPanel);
        list.bordered_rect(at, radius, kClear, 1.5f, kInk.with_alpha(0.14f * grow));

        // Everything but the cover fades in once the cover is well on its way.
        const float content = hui::tween::smoothstep((t - 0.45f) / 0.55f);
        if (content <= 0.01f)
            return;
        const float slide = 56.0f * (1.0f - hui::tween::cubic_out(t));
        list.push_opacity(content);
        list.push_transform(1.0f, 0.0f, 0.0f, slide, 0.0f);
        hui::ui::text(list, fonts.semibold, kind_ == ArtKind::icon ? "TILE ICON" : "BACKGROUND",
                      kInfoX, 170.0f, 20.0f, card.accent, hui::gfx::Align::left, 4.0f);
        hui::ui::text(list, fonts.display,
                      fonts.display.font->fit(active_game_name(), 76.0f, kRight - kInfoX),
                      kInfoX - 4.0f, 252.0f, 76.0f, kInk);
        std::string line = card.style;
        if (!card.author.empty())
            line += "  \xC2\xB7  by " + card.author;
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(line, 26.0f, 520.0f), kInfoX,
                      302.0f, 26.0f, kInk.with_alpha(0.72f));
        // Tags as chips.
        const std::string tags[3] = {card.style, card.size,
                                     kind_ == ArtKind::icon ? "Square" : "16:9"};
        for (const bool words : {false, true}) // the pills, then their labels
        {
            float x = kInfoX;
            for (const std::string &tag : tags)
            {
                if (tag.empty())
                    continue;
                const float w = fonts.semibold.measure(tag, 20.0f) + 36.0f;
                if (words)
                    hui::ui::text(list, fonts.semibold, tag, x + 18.0f, centred(380.0f, 20.0f),
                                  20.0f, kInk.with_alpha(0.86f));
                else
                    list.bordered_rect({x, 360.0f, w, 40.0f}, 20.0f, kInk.with_alpha(0.07f), 1.5f,
                                       kInk.with_alpha(0.2f));
                x += w + 10.0f;
            }
        }
        hui::ui::paragraph(list, fonts.regular,
                           kind_ == ArtKind::icon
                               ? "Used as the tile's icon: centre-cropped to a square and "
                                 "scaled to 512 pixels."
                               : "Used as the tile's background and launch screen: scaled to "
                                 "cover 1920x1080 and encoded as BC7.",
                           kInfoX, 440.0f, 24.0f, 520.0f, 34.0f, kInk.with_alpha(0.7f), 3);
        list.pop_transform();
        list.pop_opacity();
        draw_buy_box(context, list, glass, content);
    }

    void draw_buy_box(Context &context, hui::gfx::DrawList &list, std::uint32_t glass,
                      float content) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const Card &card = cards_[static_cast<std::size_t>(page_item_)];
        const float rise = 30.0f * (1.0f - content);
        Rect box = kBuyBox;
        box.y += rise;
        const bool downloading = phase_ == Phase::busy && pending_ == Job::full;

        list.push_opacity(content);
        list.shadow({box.x, box.y + 20.0f, box.w, box.h}, 30.0f, 50.0f, kBlack.with_alpha(0.45f));
        // Frosted glass: the blurred screen, a tint, then a hairline of light.
        if (glass != 0)
            list.glass(glass, box, 30.0f, kWhite);
        list.rounded_rect(box, 30.0f, hui::gfx::mix(kPanel, card.accent, 0.12f).with_alpha(0.6f));
        list.bordered_rect(box, 30.0f, kClear, 1.5f, kInk.with_alpha(0.2f));

        const float x = box.x + 32.0f;
        hui::ui::text(list, fonts.semibold, "SIZE", x, box.y + 52.0f, 16.0f, kInk.with_alpha(0.55f),
                      hui::gfx::Align::left, 3.5f);
        hui::ui::text(list, fonts.display, card.size.empty() ? "Unknown" : card.size, x - 2.0f,
                      box.y + 130.0f, 48.0f, kInk);
        hui::ui::text(list, fonts.regular, "Fetched in full from the SteamGridDB CDN", x,
                      box.y + 172.0f, 21.0f, kInk.with_alpha(0.62f));

        // The primary button: a press dips it; a refused press shakes it.
        Rect button = kBuyButton;
        button.y += rise;
        button.x += hui::ui::shake(page_nudge_.value, clock_, 10.0f);
        list.push_transform(1.0f - 0.035f * press_.value, button.cx(), button.cy(), 0.0f, 0.0f);
        list.glow(button, kButtonRadius, 22.0f,
                  kAccent.with_alpha(0.22f + 0.1f * hui::ui::breathe(clock_)));
        list.rounded_rect(button, kButtonRadius, kAccent);
        if (downloading)
        {
            hui::ui::Canvas canvas{list, fonts, 0, clock_};
            spinner_.set_bounds({button.x + 26.0f, button.cy() - 18.0f, 36.0f, 36.0f});
            spinner_.style.color = kOnAccent;
            spinner_.draw(canvas);
            spinner_.style.color = kAccent;
            hui::ui::text(list, fonts.semibold, "Downloading...", button.x + 78.0f,
                          centred(button.cy(), 24.0f), 24.0f, kOnAccent);
        }
        else
        {
            hui::ui::draw_button(list, fonts, hui::ui::GlyphStyle::light(), hui::ui::Button::cross,
                                 button.x + 40.0f, button.cy(), 30.0f);
            hui::ui::text(list, fonts.semibold, "Use this image", button.x + 78.0f,
                          centred(button.cy(), 24.0f), 24.0f, kOnAccent);
        }
        list.pop_transform();
        hui::ui::text(list, fonts.regular, "Circle goes back to the shelf", box.cx(),
                      box.y + box.h - 40.0f, 20.0f, kInk.with_alpha(0.5f), hui::gfx::Align::center);
        list.pop_opacity();
    }

    void draw_hints(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const hui::ui::Hint shop[] = {
            {hui::ui::Button::cross, zone_ == Zone::chips ? "Browse" : "Details"},
            {hui::ui::Button::triangle, "Search"},
            {hui::ui::Button::circle, "Back"}};
        list.push_opacity(hui::tween::stagger(age_, 2, 0.07f, 0.5f));
        hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), shop, 3, 1824.0f, true);
        list.pop_opacity();
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        return {}; // drawn here, in the concept's own manner
    }

  private:
    std::string key_;
    ArtKind kind_;
    std::string query_;
    std::function<void(std::vector<unsigned char>)> on_image_;

    Phase phase_ = Phase::query;
    Zone zone_ = Zone::banner;
    Job pending_ = Job::none;
    bool started_ = false;
    bool finished_ = false;
    long selected_game_ = 0; // the game whose art is being fetched
    long loaded_game_ = 0;   // the game whose art the shelf holds
    std::string full_url_;
    mutable std::string message_;

    std::vector<sgdb::Game> games_;
    std::vector<int> counts_; // art per game once known, else -1
    std::vector<Card> cards_;
    int chip_ = 0;
    int focus_ = 0;
    int banner_ = 0;
    int banner_previous_ = 0;
    bool banner_set_ = false;
    float banner_direction_ = 1.0f;
    float drift_ = 0.0f;
    float drift_previous_ = 0.0f;
    int page_item_ = -1;
    bool page_open_ = false;

    float clock_ = 0.0f;
    float age_ = 0.0f;
    float shelf_age_ = 0.0f;
    hui::tween::Timer banner_fade_;
    hui::tween::Timer page_;
    hui::tween::Spring scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring ring_radius_;
    hui::tween::Spring plate_;
    hui::ui::SpringRect chip_pill_;
    bool chip_pill_pending_snap_ = false;
    std::vector<hui::tween::Spring> lift_;
    hui::tween::Spring banner_focus_;
    hui::ui::SpringColor palette_[3];
    hui::ui::SpringColor tone_;
    hui::ui::Pulse nudge_;
    hui::ui::Pulse page_nudge_;
    hui::ui::Pulse press_;
    float nudge_x_ = 0.0f;
    float nudge_y_ = 0.0f;

    Shared shared_;
    pthread_t worker_{};
    bool worker_running_ = false;

    mutable hui::ui::InputPrompt prompt_;
    mutable hui::ui::Spinner spinner_;
    mutable hui::ui::ProgressBar bar_;
};

} // namespace

std::unique_ptr<Screen>
make_steamgriddb_screen(Context &context, ArtKind kind, std::string query,
                        std::function<void(std::vector<unsigned char>)> on_image)
{
    return std::make_unique<SteamGridScreen>(context, kind, std::move(query), std::move(on_image));
}

} // namespace fwd
