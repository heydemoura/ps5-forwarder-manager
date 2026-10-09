// ps5fwdgen - Search SteamGridDB and pick art (icon grids or hero backgrounds).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Laid out after ps5-homebrew-ui's "Paper Library" concept (src/concepts/
// paper.cpp): a warm, tactile shelf of cream paper cards on a night-blue
// table lit by bokeh.
//   - the header carries the title and a count, the matched games as filter
//     chips with one paper tab sliding under the labels (L2/R2 or Left/Right
//     step them);
//   - each piece of art is a paper card holding its picture like a photo
//     print, with the style and size printed under it; cards are dealt onto
//     the table a few milliseconds apart, the focused one lifts with a second
//     sheet turning out from under it, and one gold ring glides between them;
//   - the grid scrolls with a spring and dissolves at the edges it scrolls
//     past; an empty shelf is a note left on the table, never a blank screen,
//     and the loading state is a note too, with the kit's spinner and
//     thumbnail progress bar on it;
//   - Cross lays a sheet of paper over the blurred screen with the picture as
//     a print, its details, three stat tiles and two actions under one ink
//     highlight: "Use this image" fetches the full picture, "Close" puts the
//     sheet away.
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

// ---- the design language (Paper Library) --------------------------------------
const Color kNightTop = Color::rgb(0x0c1330);
const Color kNightBottom = Color::rgb(0x1d1240);
const Color kPaper = Color::rgb(0xf4f1ea);
const Color kPaperShade = Color::rgb(0xe6e1d6);
const Color kPrint = Color::rgb(0xfcfbf7); // the white border of a photo print
const Color kInk = Color::rgb(0x1b1d2b);
const Color kInkMuted = Color::rgb(0x6b6f82);
const Color kOnDark = Color::rgb(0xf5f3ff);
const Color kGold = Color::rgb(0xffd166);
const Color kBlack = Color::rgb(0x000000);
const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kWidth = hui::gfx::kVirtualWidth;
constexpr float kHeight = hui::gfx::kVirtualHeight;
constexpr float kMargin = 96.0f;
constexpr float kRight = kWidth - kMargin;
// The grid: six cards across the safe area, two rows on screen.
constexpr int kColumns = 6;
constexpr float kCardW = 268.0f;
constexpr float kCardH = 344.0f;
constexpr float kCardGap = 24.0f;
constexpr float kRowPitch = kCardH + kCardGap;
constexpr float kCardRadius = 14.0f;
constexpr float kCoverInset = 12.0f; // paper margin around the cover
constexpr float kLift = 0.08f;       // how much the focused card grows
constexpr float kTilt = 0.042f;      // radians the sheet under a lifted card turns
constexpr float kRingGap = 12.0f;    // from the lifted card to the ring's outer edge
constexpr float kGridTop = 210.0f;   // the clipped grid area
constexpr float kGridBottom = 990.0f;
constexpr float kGridView = kGridBottom - kGridTop;
constexpr float kRowInset = 30.0f; // room above the first row for a lifted card and its ring
constexpr float kReveal = 38.0f;   // clearance scrolling keeps around the focused row
constexpr float kFeather = 14.0f;  // height of the dissolve at a scrolled edge
constexpr float kEdgeCover = 8.0f; // solid part of the top edge, over the row scrolled past it
// Header.
constexpr float kTitleBaseline = 126.0f;
constexpr float kChipY = 150.0f;
constexpr float kChipH = 46.0f;
constexpr float kChipGap = 10.0f;
constexpr float kShoulder = 34.0f; // L2 / R2 glyph height beside the chips
// The details dialog.
constexpr float kDialogW = 1360.0f;
constexpr float kDialogH = 664.0f;
constexpr float kDialogX = (kWidth - kDialogW) * 0.5f;
constexpr float kDialogY = 190.0f;
constexpr float kDialogPad = 56.0f;
constexpr float kDialogCover = 404.0f; // the print, border included
constexpr float kActionH = 64.0f;
constexpr float kActionGap = 16.0f;
constexpr int kActions = 2;

enum class Phase
{
    query, // the search prompt is up
    shelf, // the library
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
    chips,
    grid,
    dialog,
};

// One piece of art on the table, with the springs that move it.
struct Card
{
    std::uint32_t texture = 0;
    float aspect = 1.0f;
    std::string style;
    std::string size; // "1024x1024"
    std::string author;
    Color mid = kGold;        // a tone from the picture, for the kicker and the rule
    hui::tween::Spring x, y;  // top-left corner in grid (content) coordinates
    hui::tween::Spring shown; // 0 gone, 1 on the table
    hui::tween::Spring lift;  // 0 resting, 1 focused
    int slot = 0;
};

// A card's place in the grid, in content coordinates.
Rect slot_rect(int slot)
{
    const float x = kMargin + static_cast<float>(slot % kColumns) * (kCardW + kCardGap);
    const float y = kRowInset + static_cast<float>(slot / kColumns) * kRowPitch;
    return {x, y, kCardW, kCardH};
}

Rect scaled(const Rect &r, float scale)
{
    const float w = r.w * scale;
    const float h = r.h * scale;
    return {r.cx() - w * 0.5f, r.cy() - h * 0.5f, w, h};
}

Rect action_rect(float index)
{
    const float width = (kDialogW - 2.0f * kDialogPad - (kActions - 1) * kActionGap) / 2.0f;
    return {kDialogX + kDialogPad + index * (width + kActionGap),
            kDialogY + kDialogH - kDialogPad - kActionH, width, kActionH};
}

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
        ring_.snap(slot_rect(0));
        chip_tab_.snap({kMargin, kChipY, 0.0f, kChipH});
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
        spinner_.style.color = kInk;
        bar_.style.theme = context.theme;
        bar_.style.mode = hui::ui::ProgressMode::determinate;
        bar_.style.placement = hui::ui::LabelPlacement::none;
        bar_.style.track = hui::ui::TrackStyle::flat;
        bar_.style.color = kInk;
        bar_.style.height = 8.0f;
        bar_.style.sheen = true;
        bar_.style.finish_flash = false;
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

    // A night-blue table under lamp light.
    void backdrop(Context &context, hui::gfx::BackdropSpec &spec) const override
    {
        (void)context;
        spec.mode = hui::gfx::BackdropMode::bokeh;
        spec.colors[0] = kNightTop;
        spec.colors[1] = kNightBottom;
        spec.colors[2] = Color::rgb(0xffc978); // lamp light
        spec.colors[3] = Color::rgb(0x7d6bff);
        spec.time = clock_ * 0.5f;
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
        if (job == Job::search)
            note_.target = 1.0f; // "Looking it up", the moment the keyboard leaves
        if (job == Job::assets)
        {
            // The old shelf is cleared while the new one is fetched.
            for (Card &card : cards_)
                card.shown.target = 0.0f;
            note_.target = 1.0f;
        }
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
            phase_ = Phase::shelf;
            note_.target = cards_.empty() ? 1.0f : 0.0f;
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
            chip_tab_snap_ = true;
            counts_.assign(games_.size(), -1);
            message_ = "";
            phase_ = Phase::shelf;
            scroll_.reveal(0.0f, 0.0f, kGridView, 0.0f);
            zone_ = Zone::chips;
            if (!games_.empty())
            {
                selected_game_ = games_[0].id;
                start_job(Job::assets); // the first match's art fills the shelf
            }
            else
            {
                note_.target = 1.0f;
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
                set_count(selected_game_, 0);
                release_cards();
                loaded_game_ = selected_game_;
                phase_ = Phase::shelf;
                note_.target = 1.0f;
                return;
            }
            start_job(Job::thumbs); // download thumbnails next
        }
        else if (job == Job::thumbs)
        {
            upload_cards(context);
            loaded_game_ = selected_game_;
            set_count(selected_game_, static_cast<int>(cards_.size()));
            phase_ = Phase::shelf;
            focus_ = 0;
            deal_age_ = 0.0f;
            note_.target = 0.0f;
            scroll_.reveal(0.0f, 0.0f, kGridView, 0.0f);
            if (!cards_.empty())
            {
                ring_.snap(scaled(slot_rect(0), 1.0f + kLift));
                if (zone_ == Zone::chips)
                    zone_ = Zone::grid; // the shelf is the thing to look at now
            }
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
                    card.mid = mid_tone(rgba);
                }
            }
            card.slot = static_cast<int>(i);
            const Rect at = slot_rect(card.slot);
            card.x.snap(at.x);
            card.y.snap(at.y);
            card.shown.snap(0.0f);
            card.shown.target = 1.0f;
            cards_.push_back(std::move(card));
        }
        hui::sys::log("[FWD] sgdb thumbs=%zu", cards_.size());
        message_ = "";
    }

    // A mid tone from the picture (its mean, kept lively) for small marks;
    // the gold when the picture is too grey to colour anything.
    static Color mid_tone(const std::vector<unsigned char> &rgba)
    {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4 * 7)
        {
            r += rgba[i];
            g += rgba[i + 1];
            b += rgba[i + 2];
            ++n;
        }
        if (n == 0)
            return kGold;
        const Color c{static_cast<float>(r / static_cast<double>(n) / 255.0),
                      static_cast<float>(g / static_cast<double>(n) / 255.0),
                      static_cast<float>(b / static_cast<double>(n) / 255.0), 1.0f};
        const float mx = std::max({c.r, c.g, c.b});
        const float mn = std::min({c.r, c.g, c.b});
        if (mx - mn < 0.08f)
            return kGold;
        const float scale = 0.72f / std::max(mx, 0.01f); // a printable, not glaring, tone
        return {c.r * scale, c.g * scale, c.b * scale, 1.0f};
    }

    // ---- geometry -------------------------------------------------------------
    std::string chip_label(const hui::ui::Fonts &fonts, std::size_t i) const
    {
        return fonts.semibold.font->fit(games_[i].name, 22.0f, 240.0f);
    }
    void chip_layout(const hui::ui::Fonts &fonts, std::vector<Rect> &out) const
    {
        out.clear();
        char number[8];
        float x = kMargin + hui::ui::button_width(hui::ui::Button::l2, kShoulder) + 18.0f;
        for (std::size_t i = 0; i < games_.size(); ++i)
        {
            (void)std::snprintf(number, sizeof(number), "%d", std::max(0, counts_[i]));
            const float w = 48.0f + fonts.semibold.measure(chip_label(fonts, i), 22.0f) +
                            (counts_[i] >= 0 ? 10.0f + fonts.mono.measure(number, 18.0f) : 0.0f);
            out.push_back({x, kChipY, w, kChipH});
            x += w + kChipGap;
        }
    }
    // The row slides sideways so the active chip stays between the shoulders.
    float chips_offset(const hui::ui::Fonts &fonts) const
    {
        std::vector<Rect> chips;
        chip_layout(fonts, chips);
        if (chips.empty() || chip_ >= static_cast<int>(chips.size()))
            return 0.0f;
        const Rect &c = chips[static_cast<std::size_t>(chip_)];
        const float limit = kRight - hui::ui::button_width(hui::ui::Button::r2, kShoulder) - 18.0f;
        return c.x + c.w > limit ? c.x + c.w - limit : 0.0f;
    }
    float content_height() const
    {
        const int rows = (static_cast<int>(cards_.size()) + kColumns - 1) / kColumns;
        return kRowInset + static_cast<float>(std::max(rows, 1)) * kRowPitch - kCardGap + kReveal;
    }
    Rect ring_target() const
    {
        if (cards_.empty() || focus_ < 0 || focus_ >= static_cast<int>(cards_.size()))
            return ring_.value();
        const Card &card = cards_[static_cast<std::size_t>(focus_)];
        return scaled({card.x.value, card.y.value, kCardW, kCardH}, 1.0f + kLift);
    }
    const std::string &active_game_name() const
    {
        return chip_ < static_cast<int>(games_.size()) ? games_[static_cast<std::size_t>(chip_)].name
                                                       : query_;
    }

    // ---- input ------------------------------------------------------------------
    void refuse(hui::ui::Feedback &feedback, float dx, float dy)
    {
        nudge_.trigger();
        nudge_x_ = dx;
        nudge_y_ = dy;
        feedback.play(hui::audio::Cue::error);
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

    void open_dialog(hui::ui::Feedback &feedback)
    {
        if (cards_.empty())
            return;
        dialog_item_ = focus_;
        zone_ = Zone::dialog;
        dialog_open_ = true;
        action_ = 0;
        action_position_.snap(0.0f);
        feedback.play(hui::audio::Cue::open);
    }

    void close_dialog(hui::ui::Feedback &feedback)
    {
        dialog_open_ = false;
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
            // Keep the keyboard's fade-out running: it closed on submit and
            // must leave now, not when the search returns.
            prompt_.update(dt);
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
                    phase_ = Phase::shelf;
            }
            else if (event == hui::ui::Event::cancelled)
            {
                if (games_.empty())
                {
                    context.pop();
                    return;
                }
                phase_ = Phase::shelf;
            }
            prompt_.update(dt);
            return;
        }
        prompt_.update(dt);

        if (zone_ == Zone::dialog)
        {
            update_dialog(input, feedback);
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

        if (zone_ == Zone::chips)
        {
            if (input.nav == hui::Direction::left)
                step_chip(-1, feedback);
            else if (input.nav == hui::Direction::right)
                step_chip(1, feedback);
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
            else if (input.nav == hui::Direction::up)
                refuse(feedback, 0.0f, -1.0f);
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
        }
        else
        {
            navigate_grid(input, feedback);
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
                // Panned to where it happened, as the paper sound set does.
                feedback.play(hui::audio::Cue::focus, 1.0f,
                              -0.5f + static_cast<float>(focus_ % kColumns) /
                                          static_cast<float>(kColumns - 1));
            }
        }
        if (input.is_pressed(hui::Action::confirm))
            open_dialog(feedback);
    }

    void update_dialog(const hui::InputFrame &input, hui::ui::Feedback &feedback)
    {
        if (input.is_pressed(hui::Action::back))
        {
            close_dialog(feedback);
            return;
        }
        if (input.nav == hui::Direction::left || input.nav == hui::Direction::right)
        {
            const int next = std::clamp(action_ + (input.nav == hui::Direction::right ? 1 : -1), 0,
                                        kActions - 1);
            if (next == action_)
            {
                action_nudge_.trigger();
                feedback.play(hui::audio::Cue::error);
            }
            else
            {
                action_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
        }
        else if (input.nav == hui::Direction::up || input.nav == hui::Direction::down)
        {
            action_nudge_.trigger();
            feedback.play(hui::audio::Cue::error);
        }
        if (input.is_pressed(hui::Action::confirm))
        {
            if (action_ == 1)
            {
                close_dialog(feedback);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (dialog_item_ >= 0 && dialog_item_ < static_cast<int>(shared_.assets.size()))
                    full_url_ = shared_.assets[static_cast<std::size_t>(dialog_item_)].url;
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
        const bool reduced = context.settings.reduced_motion;
        clock_ += dt;
        age_ += dt;
        deal_age_ += dt;
        nudge_.update(dt);
        action_nudge_.update(dt);
        note_.update(dt, 12.0f);
        dialog_.target = dialog_open_ ? 1.0f : 0.0f;
        dialog_.update(dt, reduced ? 40.0f : 14.0f);
        action_position_.target = static_cast<float>(action_);
        action_position_.update(dt, 22.0f);

        // Cards: lift the focused one, settle the rest.
        for (std::size_t i = 0; i < cards_.size(); ++i)
        {
            Card &card = cards_[i];
            const Rect at = slot_rect(card.slot);
            card.x.target = at.x;
            card.y.target = at.y;
            card.x.update(dt, 11.0f);
            card.y.update(dt, 11.0f);
            card.shown.update(dt, 9.0f);
            card.lift.target =
                zone_ != Zone::chips && static_cast<int>(i) == focus_ && !dialog_open_ ? 1.0f : 0.0f;
            card.lift.update(dt, reduced ? 40.0f : 16.0f);
        }
        // Scroll keeps the focused row in view, with clearance for the lift.
        if (!cards_.empty() && zone_ != Zone::chips)
        {
            const Rect at = slot_rect(focus_);
            scroll_.reveal(at.y - kReveal, at.y + kCardH + kReveal, kGridView, 0.0f);
        }
        scroll_.update(dt, reduced ? 40.0f : 12.0f);
        // The gold ring and its light.
        ring_.target(ring_target());
        ring_.update(dt, 16.0f);
        ring_alpha_.target = zone_ == Zone::grid && !cards_.empty() && !dialog_open_ ? 1.0f : 0.0f;
        ring_alpha_.update(dt, 14.0f);
        // The paper tab under the chips.
        std::vector<Rect> chips;
        chip_layout(context.fonts, chips);
        if (chip_ < static_cast<int>(chips.size()))
        {
            Rect tab = chips[static_cast<std::size_t>(chip_)];
            tab.x -= chips_offset(context.fonts);
            if (chip_tab_snap_)
            {
                chip_tab_.snap(tab);
                chip_tab_snap_ = false;
            }
            else
                chip_tab_.target(tab);
        }
        chip_tab_.update(dt, 18.0f);
    }

    // ---- drawing ------------------------------------------------------------------
    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        hui::gfx::DrawList &list = scene.list;
        draw_grid(context, list);
        draw_edges(list);
        draw_note(context, list);
        draw_header(context, list);
        draw_hints(context, list);
        const bool dialog = dialog_.value > 0.01f;
        if (dialog)
            draw_dialog(context, overlay.list, overlay.glass);
        const bool modal = prompt_.visible();
        prompt_.draw(overlay);
        return dialog || modal;
    }

    void draw_header(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const bool reduced = context.settings.reduced_motion;
        char text[64];

        // Title and count.
        float in = hui::tween::stagger(age_, 0, 0.07f, 0.5f);
        float rise = reduced ? 0.0f : 14.0f * (1.0f - in);
        list.push_opacity(in);
        const float title_w = hui::ui::text(list, fonts.display, "SteamGridDB", kMargin - 3.0f,
                                            kTitleBaseline - rise, 60.0f, kOnDark);
        if (!cards_.empty())
            (void)std::snprintf(text, sizeof(text), "%zu %s", cards_.size(),
                                kind_ == ArtKind::icon ? "icons" : "backgrounds");
        else if (!games_.empty())
            (void)std::snprintf(text, sizeof(text), "%zu games", games_.size());
        else
            (void)std::snprintf(text, sizeof(text), "%s",
                                kind_ == ArtKind::icon ? "square grids" : "hero images");
        hui::ui::text(list, fonts.regular, text, kMargin + title_w + 24.0f, kTitleBaseline - rise,
                      26.0f, kOnDark.with_alpha(0.62f));
        list.pop_opacity();

        // The matched games as chips between the two shoulder glyphs. The
        // paper tab is one object that slides under the labels; the label
        // over it turns to ink.
        in = hui::tween::stagger(age_, 1, 0.07f, 0.5f);
        rise = reduced ? 0.0f : 14.0f * (1.0f - in);
        list.push_opacity(in);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -rise);
        const hui::ui::GlyphStyle glyphs = hui::ui::GlyphStyle::dark();
        const float chip_cy = kChipY + kChipH * 0.5f;
        if (!games_.empty())
        {
            std::vector<Rect> chips;
            chip_layout(fonts, chips);
            const float offset = chips_offset(fonts);
            hui::ui::draw_button(list, fonts, glyphs, hui::ui::Button::l2, kMargin, chip_cy,
                                 kShoulder);
            const float left = kMargin + hui::ui::button_width(hui::ui::Button::l2, kShoulder) + 8.0f;
            const float r2_x = kRight - hui::ui::button_width(hui::ui::Button::r2, kShoulder);
            const float right = r2_x - 8.0f;
            hui::ui::draw_button(list, fonts, glyphs, hui::ui::Button::r2, r2_x, chip_cy, kShoulder);
            list.push_clip({left, kChipY - 12.0f, right - left, kChipH + 24.0f});
            Rect tab = chip_tab_.value();
            tab.x += hui::ui::shake(nudge_.value, clock_, 12.0f, 8.0f) *
                     (zone_ == Zone::chips ? nudge_x_ : 0.0f);
            list.shadow({tab.x, tab.y + 5.0f, tab.w, tab.h}, kChipH * 0.5f, 12.0f,
                        kBlack.with_alpha(0.4f));
            list.gradient_rect(tab, kChipH * 0.5f, kPaper, hui::gfx::mix(kPaper, kPaperShade, 0.7f));
            for (std::size_t i = 0; i < chips.size(); ++i)
            {
                Rect chip = chips[i];
                chip.x -= offset;
                if (chip.x + chip.w < left || chip.x > right)
                    continue;
                const float away = std::fabs(tab.cx() - chip.cx()) / (chip.w * 0.6f);
                const float cover = hui::tween::clamp01(1.0f - away);
                if (cover < 0.99f)
                    list.bordered_rect(chip, kChipH * 0.5f,
                                       kOnDark.with_alpha(0.05f * (1.0f - cover)), 1.5f,
                                       kOnDark.with_alpha(0.2f * (1.0f - cover)));
                const std::string label = chip_label(fonts, i);
                const float label_w = fonts.semibold.measure(label, 22.0f);
                float count_w = 0.0f;
                if (counts_[i] >= 0)
                {
                    (void)std::snprintf(text, sizeof(text), "%d", counts_[i]);
                    count_w = 10.0f + fonts.mono.measure(text, 18.0f);
                }
                const float x = chip.cx() - (label_w + count_w) * 0.5f;
                hui::ui::text(list, fonts.semibold, label, x, chip_cy + 8.0f, 22.0f,
                              hui::gfx::mix(kOnDark.with_alpha(0.78f), kInk, cover));
                if (counts_[i] >= 0)
                    hui::ui::text(list, fonts.mono, text, x + label_w + 10.0f, chip_cy + 7.0f,
                                  18.0f, hui::gfx::mix(kOnDark.with_alpha(0.45f), kInkMuted, cover));
            }
            list.pop_clip();
        }
        // The chips say which game the shelf holds, so the query itself is
        // not repeated; only a problem is reported, up by the title.
        if (!message_.empty())
            hui::ui::text(list, fonts.regular, message_, kRight, kTitleBaseline - rise, 22.0f,
                          kGold.with_alpha(0.9f), hui::gfx::Align::right);
        list.pop_transform();
        list.pop_opacity();
    }

    // The print's picture area inside a card: a square for icons, 16:9 for
    // backgrounds, inset in the paper.
    static Rect card_art(const Rect &r, ArtKind kind)
    {
        const float w = r.w - 2.0f * kCoverInset;
        const float h = kind == ArtKind::icon ? w : w * 9.0f / 16.0f;
        return {r.x + kCoverInset, r.y + kCoverInset, w, h};
    }

    // The part of the picture the print shows: a centred band of its aspect.
    static Rect art_uv(const Card &card, float want)
    {
        if (card.aspect >= want)
        {
            const float w = want / card.aspect;
            return {(1.0f - w) * 0.5f, 0.0f, w, 1.0f};
        }
        const float h = card.aspect / want;
        return {0.0f, (1.0f - h) * 0.5f, 1.0f, h};
    }

    // One paper card at its animated place.
    void draw_card(Context &context, hui::gfx::DrawList &list, int index, bool focused) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const bool reduced = context.settings.reduced_motion;
        const Card &card = cards_[static_cast<std::size_t>(index)];

        // Entrance: cards are dealt onto the table in reading order, counted
        // from the first row on screen.
        const int first = kColumns * static_cast<int>(scroll_.offset() / kRowPitch);
        const int order = std::clamp(card.slot - first, 0, 17);
        const float in = hui::tween::stagger(deal_age_, order, 0.035f, 0.45f);
        const float alpha = hui::tween::clamp01(card.shown.value) * in;
        if (alpha <= 0.01f)
            return;

        const float lift = card.lift.value;
        Rect r{card.x.value, card.y.value + kGridTop - scroll_.offset(), kCardW, kCardH};
        if (r.y > kGridBottom + 60.0f || r.y + r.h < kGridTop - 60.0f)
            return;
        if (!reduced)
            r.y += 34.0f * (1.0f - in);
        if (focused)
        {
            r.x += hui::ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_x_;
            r.y += hui::ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_y_;
        }
        const float scale =
            (0.9f + 0.1f * hui::tween::clamp01(card.shown.value)) * (1.0f + kLift * lift);

        list.push_opacity(alpha);
        list.push_transform(scale, r.cx(), r.cy(), 0.0f, 0.0f);

        // Shadow: tight and dark on the table, wide and soft in the air.
        list.shadow({r.x, r.y + 5.0f + 17.0f * lift, r.w, r.h}, kCardRadius, 14.0f + 28.0f * lift,
                    kBlack.with_alpha(0.42f + 0.2f * lift));
        // The tilt: a second sheet turns out from under the lifted card.
        const float side = index % 2 == 0 ? 1.0f : -1.0f;
        const float tilt = reduced ? 0.0f : kTilt * lift * side;
        if (std::fabs(tilt) > 0.002f)
            list.rotated_rect(r, kCardRadius, tilt, kPaperShade);
        list.gradient_rect(r, kCardRadius, kPaper, hui::gfx::mix(kPaper, kPaperShade, 0.6f));

        // The picture, set into the paper like a photo print.
        const Rect art = card_art(r, kind_);
        if (card.texture != 0)
            list.image(card.texture, art, art_uv(card, art.w / art.h), kWhite, 6.0f);
        else
            list.rounded_rect(art, 6.0f, kPaperShade);
        list.bordered_rect(art, 6.0f, kClear, 1.0f, kInk.with_alpha(0.16f));

        const float text_x = r.x + kCoverInset + 2.0f;
        const float text_right = r.x + r.w - kCoverInset - 2.0f;
        const float base = art.y + art.h;
        hui::ui::text(list, fonts.semibold,
                      fonts.semibold.font->fit(card.style, 24.0f, art.w - 4.0f), text_x,
                      base + 32.0f, 24.0f, kInk);
        hui::ui::text(list, fonts.regular, card.size, text_x, base + 58.0f, 20.0f, kInkMuted);
        // Right of the size: who made it, as far as it fits.
        if (!card.author.empty())
        {
            const float used = fonts.regular.measure(card.size, 20.0f) + 12.0f;
            hui::ui::text(list, fonts.regular,
                          fonts.regular.font->fit(card.author, 18.0f, text_right - text_x - used),
                          text_right, base + 58.0f, 18.0f, kInkMuted.with_alpha(0.8f),
                          hui::gfx::Align::right);
        }
        // A thin rule in the picture's own tone, where the concept keeps its
        // progress bar; it fills as the card is lifted.
        const Rect bar{text_x, base + 70.0f, text_right - text_x, 6.0f};
        list.rounded_rect(bar, 3.0f, kInk.with_alpha(0.12f));
        list.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * (0.35f + 0.65f * lift)), bar.h},
                          3.0f, card.mid);
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_grid(Context &context, hui::gfx::DrawList &list) const
    {
        const int focused = zone_ != Zone::chips && !cards_.empty() ? focus_ : -1;
        list.push_clip({0.0f, kGridTop, kWidth, kGridView});
        for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
            if (i != focused)
                draw_card(context, list, i, false);

        // The gold ring: its own spring, so it glides from card to card. Its
        // light is drawn under the focused card and its line over it.
        const float ring_alpha = ring_alpha_.value * hui::tween::stagger(age_, 6, 0.07f, 0.4f);
        Rect ring = ring_.value();
        const Rect home = ring_target();
        const float away = std::fabs(ring.x - home.x) + std::fabs(ring.y - home.y);
        const float landed = hui::tween::clamp01(1.0f - away / 90.0f);
        ring.y += kGridTop - scroll_.offset();
        ring.x += hui::ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_x_;
        ring.y += hui::ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_y_;
        const float radius = kCardRadius * (1.0f + kLift);
        if (ring_alpha > 0.01f)
        {
            const float breath =
                context.settings.reduced_motion ? 0.5f : hui::ui::breathe(clock_);
            list.glow(ring.inset(-kRingGap), radius + kRingGap, 26.0f,
                      kGold.with_alpha((0.3f + 0.2f * breath) * ring_alpha * landed));
        }
        if (focused >= 0)
            draw_card(context, list, focused, true);
        if (ring_alpha > 0.01f)
            list.bordered_rect(ring.inset(-kRingGap), radius + kRingGap, kClear, 4.0f,
                               kGold.with_alpha(ring_alpha));
        list.pop_clip();
    }

    // The dissolve at an edge the grid has scrolled past: the backdrop's own
    // colour at that height, opaque on the clip line and clear on both sides.
    void draw_edges(hui::gfx::DrawList &list) const
    {
        const float offset = scroll_.offset();
        const float above = hui::tween::clamp01(offset / 40.0f);
        const float below = hui::tween::clamp01((content_height() - kGridView - offset) / 40.0f);
        if (above > 0.01f)
        {
            const Color night =
                hui::gfx::mix(kNightTop, kNightBottom, kGridTop / kHeight).with_alpha(above);
            list.gradient_rect({0.0f, kGridTop - kFeather, kWidth, kFeather}, 0.0f,
                               night.with_alpha(0.0f), night);
            list.rounded_rect({0.0f, kGridTop, kWidth, kEdgeCover}, 0.0f, night);
            list.gradient_rect({0.0f, kGridTop + kEdgeCover, kWidth, kFeather}, 0.0f, night,
                               night.with_alpha(0.0f));
        }
        if (below > 0.01f)
        {
            const Color night =
                hui::gfx::mix(kNightTop, kNightBottom, kGridBottom / kHeight).with_alpha(below);
            list.gradient_rect({0.0f, kGridBottom - kFeather, kWidth, kFeather}, 0.0f,
                               night.with_alpha(0.0f), night);
            list.gradient_rect({0.0f, kGridBottom, kWidth, kFeather}, 0.0f, night,
                               night.with_alpha(0.0f));
        }
    }

    // A note left on the table: for an empty shelf, and while one is fetched.
    void draw_note(Context &context, hui::gfx::DrawList &list) const
    {
        const float t = note_.value;
        if (t <= 0.01f)
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        const bool reduced = context.settings.reduced_motion;
        const float bob = reduced ? 0.0f : std::sin(clock_ * 0.9f) * 4.0f;
        const float shake = hui::ui::shake(nudge_.value, clock_, 10.0f, 8.0f);
        const Rect note{960.0f - 330.0f + shake, 400.0f + bob, 660.0f, 330.0f};
        const bool searching = phase_ == Phase::busy && pending_ == Job::search;
        const bool loading =
            phase_ == Phase::busy && (pending_ == Job::assets || pending_ == Job::thumbs);

        list.push_opacity(hui::tween::clamp01(t * 1.3f));
        list.push_transform(reduced ? 1.0f : 0.92f + 0.08f * t, note.cx(), note.cy(), 0.0f, 0.0f);
        list.shadow({note.x, note.y + 20.0f, note.w, note.h}, 22.0f, 44.0f, kBlack.with_alpha(0.5f));
        if (!reduced)
            list.rotated_rect(note, 22.0f, -0.045f, kPaperShade);
        list.gradient_rect(note, 22.0f, kPaper, hui::gfx::mix(kPaper, kPaperShade, 0.6f));

        const float icon_y = note.y + 92.0f;
        const char *headline;
        const char *line;
        if (searching || loading)
        {
            headline = searching ? "Looking it up" : "Stocking the shelf";
            line = searching             ? "Asking SteamGridDB for matching games."
                   : pending_ == Job::assets ? "Finding its artwork."
                                             : "Fetching the thumbnails.";
            hui::ui::Canvas canvas{list, fonts, 0, clock_};
            list.circle(note.cx(), icon_y, 44.0f, kPaperShade);
            spinner_.set_bounds({note.cx() - 24.0f, icon_y - 24.0f, 48.0f, 48.0f});
            spinner_.draw(canvas);
            if (pending_ == Job::thumbs && shared_.total.load() > 0)
            {
                bar_.set_bounds({note.x + 120.0f, note.y + 292.0f, note.w - 240.0f, 8.0f});
                bar_.draw(canvas);
            }
        }
        else if (games_.empty())
        {
            headline = "Nothing here yet";
            line = "Press Triangle and type a game's name.";
            list.circle(note.cx(), icon_y, 44.0f, kPaperShade);
            list.arc(note.cx() - 3.0f, icon_y - 3.0f, 16.0f, 5.0f, 0.0f, 6.2831853f, kInkMuted);
            list.rotated_rect({note.cx() + 8.0f, icon_y + 8.0f, 20.0f, 6.0f}, 3.0f, 0.7854f,
                              kInkMuted);
        }
        else
        {
            headline = "Nothing on this shelf";
            line = kind_ == ArtKind::icon ? "SteamGridDB has no icons for that game."
                                          : "SteamGridDB has no backgrounds for that game.";
            list.circle(note.cx(), icon_y, 44.0f, kPaperShade);
            list.arc(note.cx(), icon_y, 24.0f, 6.0f, 0.0f, 4.4f, kInkMuted);
        }
        hui::ui::text(list, fonts.display, headline, note.cx(), note.y + 204.0f, 40.0f, kInk,
                      hui::gfx::Align::center);
        hui::ui::text(list, fonts.regular, line, note.cx(), note.y + 262.0f, 26.0f, kInkMuted,
                      hui::gfx::Align::center);
        list.pop_transform();
        list.pop_opacity();
    }

    // ---- the details dialog: a sheet of paper over the blurred screen -------------
    void draw_dialog(Context &context, hui::gfx::DrawList &list, std::uint32_t glass) const
    {
        if (dialog_item_ < 0 || dialog_item_ >= static_cast<int>(cards_.size()))
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        const bool reduced = context.settings.reduced_motion;
        const Card &card = cards_[static_cast<std::size_t>(dialog_item_)];
        const float t = dialog_.value;

        // Everything behind is out of focus: the glass copy covers the screen.
        list.push_opacity(hui::tween::clamp01(t * 1.2f));
        if (glass != 0)
            list.glass(glass, {0.0f, 0.0f, kWidth, kHeight}, 0.0f, kWhite);
        list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, Color::rgb(0x0a0e26, 0.34f));
        list.pop_opacity();

        // The sheet is laid on top: it settles downward and grows to size.
        const Rect sheet{kDialogX, kDialogY, kDialogW, kDialogH};
        list.push_opacity(hui::tween::clamp01(t * 1.5f));
        if (!reduced)
            list.push_transform(0.94f + 0.06f * t, 960.0f, 540.0f, 0.0f, -36.0f * (1.0f - t));
        else
            list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        list.shadow({sheet.x, sheet.y + 28.0f, sheet.w, sheet.h}, 28.0f, 70.0f,
                    kBlack.with_alpha(0.6f));
        if (!reduced)
            list.rotated_rect(sheet, 28.0f, 0.012f, kPaperShade);
        list.gradient_rect(sheet, 28.0f, kPaper, hui::gfx::mix(kPaper, kPaperShade, 0.5f));
        list.bordered_rect(sheet, 28.0f, kClear, 1.5f, kWhite.with_alpha(0.7f));

        // The picture as a print lying on the sheet, slightly askew underneath.
        const float print_h = kind_ == ArtKind::icon
                                  ? kDialogCover
                                  : (kDialogCover - 24.0f) * 9.0f / 16.0f + 24.0f;
        const Rect print{sheet.x + kDialogPad, sheet.y + kDialogPad, kDialogCover, print_h};
        const Rect art = print.inset(12.0f);
        list.shadow({print.x, print.y + 12.0f, print.w, print.h}, 8.0f, 26.0f,
                    kBlack.with_alpha(0.38f));
        if (!reduced)
            list.rotated_rect(print, 8.0f, -0.04f, hui::gfx::mix(kPaperShade, kInkMuted, 0.18f));
        list.rounded_rect(print, 8.0f, kPrint);
        if (card.texture != 0)
            list.image(card.texture, art, art_uv(card, art.w / art.h), kWhite, 4.0f);
        else
            list.rounded_rect(art, 4.0f, kPaperShade);
        list.bordered_rect(art, 4.0f, kClear, 1.0f, kInk.with_alpha(0.16f));

        // Text column.
        const float x = sheet.x + kDialogPad + kDialogCover + 44.0f;
        const float width = sheet.x + sheet.w - kDialogPad - x;
        hui::ui::text(list, fonts.semibold, kind_ == ArtKind::icon ? "TILE ICON" : "BACKGROUND",
                      x, sheet.y + 90.0f, 18.0f, card.mid, hui::gfx::Align::left, 4.0f);
        hui::ui::text(list, fonts.display,
                      fonts.display.font->fit(active_game_name(), 58.0f, width), x - 3.0f,
                      sheet.y + 154.0f, 58.0f, kInk);
        std::string line = card.style;
        if (!card.author.empty())
            line += "  \xC2\xB7  by " + card.author;
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(line, 24.0f, width), x,
                      sheet.y + 196.0f, 24.0f, kInkMuted);
        hui::ui::paragraph(list, fonts.regular,
                           kind_ == ArtKind::icon
                               ? "Used as the tile's icon on the home screen: centre-cropped to "
                                 "a square and scaled to 512 pixels."
                               : "Used as the tile's background and launch screen: scaled to "
                                 "cover 1920x1080 and encoded as BC7.",
                           x, sheet.y + 250.0f, 26.0f, width, 38.0f, kInk.with_alpha(0.88f), 2);

        // Three stat tiles, arriving one after another.
        const char *labels[3] = {"STYLE", "SIZE", "SHAPE"};
        const float tile_w = (width - 2.0f * 16.0f) / 3.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float appear = hui::tween::stagger(t, i, 0.1f, 0.6f);
            const float drop = reduced ? 0.0f : 18.0f * (1.0f - appear);
            const Rect tile{x + static_cast<float>(i) * (tile_w + 16.0f), sheet.y + 346.0f + drop,
                            tile_w, 114.0f};
            list.push_opacity(appear);
            list.rounded_rect(tile, 14.0f, kPaperShade);
            hui::ui::text(list, fonts.semibold, labels[i], tile.x + 20.0f, tile.y + 34.0f, 15.0f,
                          kInkMuted, hui::gfx::Align::left, 3.0f);
            const std::string value =
                i == 0   ? card.style
                : i == 1 ? (card.size.empty() ? std::string("unknown") : card.size)
                         : (kind_ == ArtKind::icon ? std::string("square") : std::string("16 : 9"));
            const hui::ui::FontRef &face = i == 1 ? fonts.mono : fonts.semibold;
            hui::ui::text(list, face, face.font->fit(value, 30.0f, tile_w - 40.0f), tile.x + 20.0f,
                          tile.y + 86.0f, 30.0f, kInk);
            list.pop_opacity();
        }

        // What the focused action will do, in one line under the tiles.
        const bool downloading = phase_ == Phase::busy && pending_ == Job::full;
        hui::ui::text(list, fonts.regular,
                      downloading    ? "Fetching the full picture from SteamGridDB..."
                      : action_ == 0 ? "Fetches the full picture and uses it for this forwarder."
                                     : "Puts the sheet away and returns to the shelf.",
                      x, sheet.y + 500.0f, 22.0f, kInkMuted);

        // Actions: one ink highlight glides under the labels.
        const char *names[kActions] = {downloading ? "Fetching..." : "Use this image", "Close"};
        for (int i = 0; i < kActions; ++i)
            list.rounded_rect(action_rect(static_cast<float>(i)), kActionH * 0.5f, kPaperShade);
        Rect highlight = action_rect(action_position_.value);
        highlight.x += hui::ui::shake(action_nudge_.value, clock_, 10.0f, 9.0f);
        list.shadow({highlight.x, highlight.y + 8.0f, highlight.w, highlight.h}, kActionH * 0.5f,
                    16.0f, kBlack.with_alpha(0.35f));
        list.rounded_rect(highlight, kActionH * 0.5f, kInk);
        for (int i = 0; i < kActions; ++i)
        {
            const Rect r = action_rect(static_cast<float>(i));
            const float cover =
                hui::tween::clamp01(1.0f - std::fabs(highlight.cx() - r.cx()) / (r.w * 0.6f));
            const Color ink = hui::gfx::mix(kInk, kPaper, cover);
            const float label_w = fonts.semibold.measure(names[i], 25.0f);
            float lx = r.cx() - label_w * 0.5f;
            if (i == 0 && downloading)
            {
                hui::ui::Canvas canvas{list, fonts, 0, clock_};
                lx += 18.0f;
                spinner_.set_bounds({lx - 46.0f, r.cy() - 14.0f, 28.0f, 28.0f});
                spinner_.style.color = ink;
                spinner_.draw(canvas);
                spinner_.style.color = kInk;
            }
            hui::ui::text(list, fonts.semibold, names[i], lx, r.cy() + 9.0f, 25.0f, ink);
        }
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_hints(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const hui::ui::Hint shelf[] = {
            {hui::ui::Button::cross, zone_ == Zone::chips ? "Browse" : "Details"},
            {hui::ui::Button::triangle, "Search"},
            {hui::ui::Button::circle, "Back"}};
        const hui::ui::Hint sheet[] = {{hui::ui::Button::cross, "Choose"},
                                       {hui::ui::Button::circle, "Close"}};
        list.push_opacity(hui::tween::stagger(age_, 2, 0.07f, 0.5f));
        if (zone_ == Zone::dialog)
            hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), sheet, 2, 1824.0f, true);
        else
            hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), shelf, 3, 1824.0f, true);
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
    Zone zone_ = Zone::chips;
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
    int dialog_item_ = -1;
    bool dialog_open_ = false;
    int action_ = 0;

    float clock_ = 0.0f;
    float age_ = 0.0f;
    float deal_age_ = 0.0f;
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::tween::Spring ring_alpha_;
    hui::ui::SpringRect chip_tab_;
    bool chip_tab_snap_ = false;
    hui::tween::Spring note_;
    hui::tween::Spring dialog_;
    hui::tween::Spring action_position_;
    hui::ui::Pulse nudge_;
    hui::ui::Pulse action_nudge_;
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
