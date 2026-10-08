// ps5fwdgen - Search SteamGridDB and pick art (icon grids or hero backgrounds).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "ui/fonts.hpp"

#include <atomic>
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

enum class Phase
{
    query,   // the search prompt is up
    games,   // choose a matched game
    thumbs,  // choose an image
    busy,    // a worker is running
};

enum class Job
{
    none,
    search,
    assets,
    thumbs,
    full,
};

struct Thumb
{
    std::vector<unsigned char> bytes; // decoded? no: still-encoded jpeg from CDN
};

constexpr hui::gfx::Rect kListBounds{96.0f, 300.0f, 1200.0f, 640.0f};
constexpr hui::gfx::Rect kGridBounds{96.0f, 300.0f, 1728.0f, 640.0f};

class SteamGridScreen final : public Screen
{
  public:
    SteamGridScreen(Context &context, ArtKind kind, std::string query,
                    std::function<void(std::vector<unsigned char>)> on_image)
        : key_(context.settings.steamgriddb_key), kind_(kind), query_(std::move(query)),
          on_image_(std::move(on_image))
    {
        restyle(context);
        games_.set_bounds(kListBounds);
        games_.set_active(true);
        grid_.set_bounds(kGridBounds);
        grid_.set_active(true);
        grid_.style.columns = kind_ == ArtKind::icon ? 6 : 4;
        grid_.style.card.art_aspect = kind_ == ArtKind::icon ? 1.0f : 16.0f / 9.0f;
        prompt_.keyboard.style.bindings = hui::ui::KeyboardBindings::standard();
        prompt_.set_title("Search SteamGridDB");
        prompt_.style.max_length = 80;
    }

    ~SteamGridScreen() override
    {
        join_worker();
    }

    void restyle(Context &context) override
    {
        games_.style.theme = context.theme;
        games_.style.cards = true;
        grid_.style.theme = context.theme;
        prompt_.style.theme = context.theme;
    }

    void enter(Context &context) override
    {
        if (!started_)
        {
            started_ = true;
            if (key_.empty())
            {
                message_ = "Set a SteamGridDB API key in Settings first.";
                phase_ = Phase::query; // still allow typing; the worker will error
            }
            hui::ui::Feedback discard;
            prompt_.open(discard, query_);
            phase_ = Phase::query;
        }
    }

    // ---- worker plumbing ---------------------------------------------------
    struct Shared
    {
        std::mutex mutex;
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
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
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            shared_.error.clear();
        }
        phase_ = Phase::busy;
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
            for (std::size_t i = 0; i < assets_copy.size(); ++i)
            {
                const std::string &url =
                    assets_copy[i].thumb.empty() ? assets_copy[i].url : assets_copy[i].thumb;
                sgdb::download(url, thumbs[i]);
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
            phase_ = job == Job::search ? Phase::query : previous_phase_;
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
            games_.set_items(std::move(items));
            games_.set_focus(0, true);
            message_ = games_.items().empty() ? "No matches. Press Triangle to search again." : "";
            phase_ = Phase::games;
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
                phase_ = Phase::games;
                return;
            }
            start_job(Job::thumbs); // download thumbnails next
        }
        else if (job == Job::thumbs)
        {
            upload_thumbs(context);
            phase_ = Phase::thumbs;
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

    void upload_thumbs(Context &context)
    {
        std::vector<sgdb::Asset> assets_copy;
        std::vector<std::vector<unsigned char>> thumbs;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            assets_copy = shared_.assets;
            thumbs = std::move(shared_.thumbs);
        }
        std::vector<hui::ui::CardItem> cards;
        cards.reserve(assets_copy.size());
        for (std::size_t i = 0; i < assets_copy.size(); ++i)
        {
            hui::ui::CardItem card;
            card.title = assets_copy[i].style.empty() ? "grid" : assets_copy[i].style;
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
                }
            }
            cards.push_back(std::move(card));
        }
        hui::sys::log("[FWD] sgdb thumbs=%zu", cards.size());
        grid_.set_items(std::move(cards));
        grid_.set_focus(0, true);
        message_ = "";
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (finished_)
        {
            context.pop();
            return;
        }
        if (phase_ == Phase::busy)
        {
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
                {
                    previous_phase_ = Phase::query;
                    start_job(Job::search);
                }
            }
            else if (event == hui::ui::Event::cancelled)
            {
                context.pop();
                return;
            }
            prompt_.update(dt);
            return;
        }

        if (input.is_pressed(hui::Action::back))
        {
            if (phase_ == Phase::thumbs)
                phase_ = Phase::games;
            else
                context.pop();
            return;
        }
        if (input.is_pressed(hui::Action::north)) // Triangle: search again
        {
            prompt_.open(feedback, query_);
            return;
        }

        if (phase_ == Phase::games)
        {
            const hui::ui::Event event = games_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                const int index = games_.focus();
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (index >= 0 && index < static_cast<int>(shared_.games.size()))
                {
                    selected_game_ = shared_.games[static_cast<std::size_t>(index)].id;
                    previous_phase_ = Phase::games;
                    // unlock before starting the worker
                }
            }
            games_.update(dt);
            if (event == hui::ui::Event::activated && selected_game_ != 0)
                start_job(Job::assets);
        }
        else if (phase_ == Phase::thumbs)
        {
            const hui::ui::Event event = grid_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                const int index = grid_.focus();
                std::lock_guard<std::mutex> lock(shared_.mutex);
                if (index >= 0 && index < static_cast<int>(shared_.assets.size()))
                {
                    full_url_ = shared_.assets[static_cast<std::size_t>(index)].url;
                }
            }
            grid_.update(dt);
            if (event == hui::ui::Event::activated && !full_url_.empty())
            {
                previous_phase_ = Phase::thumbs;
                start_job(Job::full);
            }
        }
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        if (!prompt_.is_open())
            return false;
        query_ = text;
        prompt_.dismiss();
        if (!query_.empty())
        {
            previous_phase_ = Phase::query;
            start_job(Job::search);
        }
        return true;
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        const hui::gfx::Color muted =
            theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        hui::ui::text(scene.list, context.fonts.display, "SteamGridDB", 96.0f, 150.0f, 46.0f, text);
        if (!message_.empty())
            hui::ui::text(scene.list, context.fonts.regular, message_, 96.0f, 240.0f, 26.0f, muted);

        if (phase_ == Phase::busy)
        {
            hui::ui::text(scene.list, context.fonts.regular, "Working...", 96.0f, 400.0f, 30.0f,
                          muted);
        }
        else if (phase_ == Phase::games)
        {
            games_.draw(scene);
        }
        else if (phase_ == Phase::thumbs)
        {
            grid_.draw(scene);
        }
        const bool modal = prompt_.visible();
        prompt_.draw(overlay);
        return modal;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {
            {hui::ui::Button::cross, "Select"},
            {hui::ui::Button::triangle, "Search"},
            {hui::ui::Button::circle, "Back"},
        };
        return kHints;
    }

  private:
    std::string key_;
    ArtKind kind_;
    std::string query_;
    std::function<void(std::vector<unsigned char>)> on_image_;

    Phase phase_ = Phase::query;
    Phase previous_phase_ = Phase::query;
    Job pending_ = Job::none;
    bool started_ = false;
    bool finished_ = false;
    long selected_game_ = 0;
    std::string full_url_;
    mutable std::string message_;

    Shared shared_;
    pthread_t worker_{};
    bool worker_running_ = false;

    mutable hui::ui::ListView games_;
    mutable hui::ui::GridView grid_;
    mutable hui::ui::InputPrompt prompt_;
};

} // namespace

std::unique_ptr<Screen>
make_steamgriddb_screen(Context &context, ArtKind kind, std::string query,
                        std::function<void(std::vector<unsigned char>)> on_image)
{
    return std::make_unique<SteamGridScreen>(context, kind, std::move(query), std::move(on_image));
}

} // namespace fwd
