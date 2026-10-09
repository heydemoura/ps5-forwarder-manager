// ps5fwdgen - Choose where a tile icon or background comes from.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two cards side by side, one per source, built from the kit's components:
//   - a GridView of two cells, each drawn through its content slot as a card
//     with an illustration, a title, a one-line explanation and Chips that
//     say what the source needs ("Online", "PNG or JPEG"); a Badge marks the
//     recommended source;
//   - under them a DetailList titled "How it works" that follows the focus
//     and explains the focused source in three steps;
//   - a Banner warns, and the SteamGridDB card is disabled, when the build
//     has no SteamGridDB key.
// The screen's own subtitle says what the picture becomes on the console.

#include "app/screens/art_screen.hpp"

#include "app/context.hpp"
#include "app/screens/file_picker_screen.hpp"
#include "app/screens/steamgriddb_screen.hpp"
#include "fwd/image.hpp"
#include "ui/components/badge.hpp"
#include "ui/components/banner.hpp"
#include "ui/components/detail_list.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/toast.hpp"
#include "ui/fonts.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

using hui::gfx::Color;
using hui::gfx::Rect;

bool read_whole(const std::string &path, std::vector<unsigned char> &out)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    out.clear();
    unsigned char buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.insert(out.end(), buffer, buffer + got);
    std::fclose(file);
    return true;
}

enum Source
{
    kSteamGridDb = 0,
    kFile = 1,
};

constexpr float kMargin = 96.0f;
constexpr Rect kCards{kMargin, 268.0f, 1728.0f, 380.0f};
constexpr float kCardHeight = 340.0f;
constexpr Rect kHow{kMargin, 690.0f, 1728.0f, 132.0f};
constexpr Rect kBanner{kMargin, 268.0f, 1728.0f, 96.0f};
constexpr float kBannerShift = 120.0f; // everything below moves down while it shows

class ArtScreen final : public Screen
{
  public:
    ArtScreen(Context &context, ArtKind kind, std::string suggested,
              std::function<void(std::vector<unsigned char>)> on_ready)
        : kind_(kind), suggested_(std::move(suggested)), on_ready_(std::move(on_ready)),
          has_key_(!context.settings.steamgriddb_key.empty())
    {
        restyle(context);
        const float shift = has_key_ ? 0.0f : kBannerShift;

        // The two source cards.
        std::vector<hui::ui::CardItem> items(2);
        items[kSteamGridDb].title = "Search SteamGridDB";
        items[kSteamGridDb].disabled = !has_key_;
        items[kFile].title = "Choose an image file";
        cards_.set_items(std::move(items));
        cards_.set_bounds({kCards.x, kCards.y + shift, kCards.w, kCards.h});
        cards_.set_focus(has_key_ ? kSteamGridDb : kFile, true);
        cards_.set_active(true);
        cards_.content = [this, &context](hui::ui::Canvas &canvas, const Rect &cell,
                                          const hui::ui::CardItem &item, int index, float focus)
        { draw_source_card(context, canvas, cell, item, index, focus); };

        how_.set_bounds({kHow.x, kHow.y + shift, kHow.w, kHow.h});
        how_.set_active(false);
        refresh_how();

        banner_.title = "SteamGridDB is unavailable";
        banner_.body = "This build has no SteamGridDB key, so online search is off. Pick an "
                       "image file from the console instead.";
        banner_.set_bounds(kBanner);
        if (!has_key_)
        {
            hui::ui::Feedback quiet;
            banner_.show(quiet);
        }

        recommended_.set_text("RECOMMENDED");
    }

    void restyle(Context &context) override
    {
        const hui::ui::Theme &theme = context.theme;
        cards_.style.theme = theme;
        cards_.style.columns = 2;
        cards_.style.cell_height = kCardHeight;
        cards_.style.gap_x = 32.0f;
        cards_.style.entrance_step = 0.08f;
        cards_.style.scroll_thumb = false;
        cards_.style.card.focus_scale = 1.03f;
        cards_.style.card.lift = 6.0f;
        cards_.style.card.glow = true;
        cards_.style.card.dim = 0.25f;

        how_.style.theme = theme;
        how_.style.layout = hui::ui::DetailLayout::grid;
        how_.style.columns = 3;
        how_.style.panel = true;
        how_.style.dividers = false;
        how_.style.label_size = 18.0f;
        how_.style.value_size = 23.0f;
        how_.style.max_lines = 2;

        for (hui::ui::Chip &chip : chips_)
        {
            chip.style.theme = theme;
            chip.style.height = 36.0f;
            chip.style.text_size = 18.0f;
            chip.style.leading_dot = true;
        }
        recommended_.style.theme = theme;
        recommended_.style.kind = hui::ui::Status::accent;
        recommended_.style.fill = hui::ui::BadgeFill::tinted;
        recommended_.style.align = hui::gfx::Align::right;
        recommended_.style.text_size = 15.0f;
        recommended_.style.height = 28.0f;

        banner_.style.theme = theme;
        banner_.style.kind = hui::ui::StatusKind::warning;
        banner_.style.look = hui::ui::BannerLook::accent;
        toasts_.style.theme = theme;
    }

    // ---- what each source is -----------------------------------------------------
    const char *pitch(int index) const
    {
        if (index == kSteamGridDb)
            return kind_ == ArtKind::icon
                       ? "Community-made square grids for thousands of games, found by name."
                       : "Community-made hero images for thousands of games, found by name.";
        return "A PNG or JPEG you have already copied onto the console, for example over FTP.";
    }

    void refresh_how()
    {
        const int source = cards_.focus();
        if (source == shown_how_)
            return;
        shown_how_ = source;
        std::vector<hui::ui::DetailItem> rows;
        if (source == kSteamGridDb)
        {
            rows.push_back({"1  Search", "Type the game's name; the forwarder's name is filled in "
                                         "for you."});
            rows.push_back({"2  Browse", kind_ == ArtKind::icon
                                             ? "Pick a match, then look through its square grids."
                                             : "Pick a match, then look through its hero images."});
            rows.push_back({"3  Use", "The full picture downloads and is sized for the console."});
        }
        else
        {
            rows.push_back({"1  Copy", "Put the image anywhere under /data, for example with FTP."});
            rows.push_back({"2  Find", "Browse to it in the file picker; only images are listed."});
            rows.push_back({"3  Use", kind_ == ArtKind::icon
                                          ? "It is centre-cropped to a square, 512x512."
                                          : "It is scaled to cover 1920x1080, then cropped."});
        }
        how_.set_items(std::move(rows));
    }

    // ---- results -----------------------------------------------------------------------
    void finish_from_file(Context &context, const std::string &path)
    {
        std::vector<unsigned char> raw;
        if (!read_whole(path, raw) || raw.empty())
        {
            toasts_.push(hui::ui::StatusKind::danger, "Could not read the file");
            return;
        }
        finish_from_encoded(context, raw);
    }

    void finish_from_encoded(Context &context, const std::vector<unsigned char> &raw)
    {
        (void)context;
        std::vector<unsigned char> encoded =
            kind_ == ArtKind::icon ? make_icon_png(raw.data(), raw.size())
                                   : make_background_dds(raw.data(), raw.size());
        if (encoded.empty())
        {
            toasts_.push(hui::ui::StatusKind::danger, "That image could not be processed",
                         "Use a standard PNG or JPEG.");
            return;
        }
        if (on_ready_)
            on_ready_(std::move(encoded));
        done_ = true;
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (done_)
        {
            context.pop();
            return;
        }
        if (input.is_pressed(hui::Action::back))
        {
            context.pop();
            return;
        }
        const hui::ui::Event event = cards_.handle(input, feedback);
        if (event == hui::ui::Event::activated)
        {
            ArtScreen *self = this;
            if (cards_.focus() == kSteamGridDb)
            {
                context.push(make_steamgriddb_screen(
                    context, kind_, suggested_,
                    [self, &context](std::vector<unsigned char> raw)
                    { self->finish_from_encoded(context, raw); }));
            }
            else
            {
                context.push(make_file_picker_screen(
                    context, "/data", {".png", ".jpg", ".jpeg", ".bmp"},
                    [self, &context](const std::string &path)
                    { self->finish_from_file(context, path); }));
            }
        }
        refresh_how();
        cards_.update(dt);
        how_.update(dt);
        banner_.update(dt);
        recommended_.update(dt);
        for (hui::ui::Chip &chip : chips_)
            chip.update(dt);
        toasts_.update(dt, feedback);
    }

    // ---- drawing -----------------------------------------------------------------------
    // A small picture of the source: a magnifier over a grid of covers for
    // SteamGridDB, a folder holding a picture for a file.
    void draw_illustration(hui::gfx::DrawList &list, const Rect &box, int index, Color ink,
                           Color tint) const
    {
        list.rounded_rect(box, 24.0f, tint);
        const float cx = box.cx();
        const float cy = box.cy();
        if (index == kSteamGridDb)
        {
            // Four covers...
            const float s = 30.0f;
            const float g = 8.0f;
            for (int i = 0; i < 4; ++i)
            {
                const float x = cx - s - g * 0.5f + static_cast<float>(i % 2) * (s + g) - 10.0f;
                const float y = cy - s - g * 0.5f + static_cast<float>(i / 2) * (s + g) - 10.0f;
                list.rounded_rect({x, y, s, s}, 6.0f, ink.with_alpha(i == 0 ? 0.9f : 0.35f));
            }
            // ...and a magnifier over them.
            list.circle(cx + 18.0f, cy + 18.0f, 22.0f, tint);
            list.arc(cx + 18.0f, cy + 18.0f, 17.0f, 6.0f, 0.0f, 6.2831853f, ink);
            list.rotated_rect({cx + 28.0f, cy + 38.0f, 24.0f, 8.0f}, 4.0f, 0.7854f, ink);
        }
        else
        {
            // A folder...
            list.rounded_rect({cx - 46.0f, cy - 34.0f, 40.0f, 16.0f}, 5.0f, ink.with_alpha(0.55f));
            list.rounded_rect({cx - 46.0f, cy - 24.0f, 92.0f, 64.0f}, 9.0f, ink.with_alpha(0.55f));
            // ...holding a picture: a sun over hills.
            const Rect photo{cx - 30.0f, cy - 16.0f, 60.0f, 44.0f};
            list.rounded_rect(photo, 6.0f, tint);
            list.circle(photo.x + 16.0f, photo.y + 14.0f, 6.0f, ink);
            list.rotated_rect({photo.x + 22.0f, photo.y + 24.0f, 30.0f, 30.0f}, 3.0f, 0.7854f,
                              ink.with_alpha(0.9f));
            list.rounded_rect({cx - 46.0f, cy + 4.0f, 92.0f, 36.0f}, 9.0f, ink.with_alpha(0.8f));
        }
    }

    void draw_source_card(Context &context, hui::ui::Canvas &canvas, const Rect &cell,
                          const hui::ui::CardItem &item, int index, float focus) const
    {
        const hui::ui::Theme &theme = context.theme;
        const hui::ui::Fonts &fonts = context.fonts;
        hui::gfx::DrawList &list = canvas.list;
        const bool disabled = item.disabled;
        const float radius = theme.radius_card;

        // The plate: the theme's surface, a hairline that lights with the focus.
        list.rounded_rect(cell, radius, theme.surface);
        list.bordered_rect(cell, radius, Color::rgb(0x000000, 0.0f), 1.5f,
                           hui::gfx::mix(theme.outline.with_alpha(0.35f), theme.primary, focus));

        const float pad = 36.0f;
        const Color ink = disabled ? theme.text_muted : theme.primary;
        const Rect art{cell.x + pad, cell.y + pad, 140.0f, 140.0f};
        draw_illustration(list, art, index, ink, theme.surface_high);

        if (index == kSteamGridDb && !disabled)
        {
            recommended_.set_bounds({cell.x + cell.w - pad - 200.0f, cell.y + pad, 200.0f, 28.0f});
            recommended_.draw(canvas);
        }

        const float tx = art.x + art.w + 32.0f;
        const float tw = cell.x + cell.w - pad - tx;
        hui::ui::text(list, fonts.semibold, index == kSteamGridDb ? "ONLINE" : "ON THE CONSOLE",
                      tx, cell.y + pad + 22.0f, 16.0f, disabled ? theme.text_muted : theme.accent,
                      hui::gfx::Align::left, 3.0f);
        hui::ui::text(list, fonts.display, item.title, tx - 2.0f, cell.y + pad + 72.0f, 38.0f,
                      disabled ? theme.text_muted : theme.text);
        hui::ui::paragraph(list, fonts.regular, pitch(index), tx, cell.y + pad + 114.0f, 22.0f, tw,
                           32.0f, theme.text_muted, 3);

        // What the source needs, as chips along the bottom.
        const std::array<const char *, 2> labels =
            index == kSteamGridDb
                ? std::array<const char *, 2>{"Needs internet", "Searches by game name"}
                : std::array<const char *, 2>{"Works offline", "PNG, JPEG or BMP"};
        float x = cell.x + pad;
        const float y = cell.y + cell.h - pad - 36.0f;
        for (int c = 0; c < 2; ++c)
        {
            hui::ui::Chip &chip = chips_[static_cast<std::size_t>(index * 2 + c)];
            chip.label = labels[static_cast<std::size_t>(c)];
            chip.style.dot = c == 0 ? (index == kSteamGridDb ? hui::ui::Status::primary
                                                             : hui::ui::Status::success)
                                    : hui::ui::Status::neutral;
            chip.style.leading_dot = c == 0;
            const hui::ui::Painter paint(list, fonts, theme);
            const float w = chip.width(paint);
            chip.set_bounds({x, y, w, 36.0f});
            chip.draw(canvas);
            x += w + 12.0f;
        }

        if (disabled)
            hui::ui::text(list, fonts.semibold, "Unavailable in this build", cell.x + cell.w - pad,
                          y + 26.0f, 18.0f, theme.warning, hui::gfx::Align::right);
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        const Color muted = theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        hui::ui::text(scene.list, context.fonts.display,
                      kind_ == ArtKind::icon ? "Where should the icon come from?"
                                             : "Where should the background come from?",
                      kMargin, 150.0f, 46.0f, text);
        hui::ui::text(scene.list, context.fonts.regular,
                      kind_ == ArtKind::icon
                          ? "The tile's picture on the home screen, saved as a 512x512 PNG."
                          : "Shown behind the tile and while it launches, saved as a 1920x1080 "
                            "BC7 image.",
                      kMargin, 206.0f, 26.0f, muted);
        if (banner_.visible())
            banner_.draw(scene);
        cards_.draw(scene);
        hui::ui::text(scene.list, context.fonts.semibold, "HOW IT WORKS", kMargin,
                      how_.bounds().y - 14.0f, 16.0f, muted, hui::gfx::Align::left, 3.0f);
        how_.draw(scene);
        toasts_.draw(overlay);
        return false;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {
            {hui::ui::Button::cross, "Choose"},
            {hui::ui::Button::circle, "Back"},
        };
        return kHints;
    }

  private:
    ArtKind kind_;
    std::string suggested_;
    std::function<void(std::vector<unsigned char>)> on_ready_;
    bool has_key_;
    bool done_ = false;
    int shown_how_ = -1;
    mutable hui::ui::GridView cards_;
    mutable hui::ui::DetailList how_;
    mutable hui::ui::Banner banner_;
    mutable hui::ui::Badge recommended_;
    mutable std::array<hui::ui::Chip, 4> chips_;
    mutable hui::ui::ToastStack toasts_;
};

} // namespace

std::unique_ptr<Screen>
make_art_screen(Context &context, ArtKind kind, std::string suggested_query,
                std::function<void(std::vector<unsigned char>)> on_ready)
{
    return std::make_unique<ArtScreen>(context, kind, std::move(suggested_query),
                                      std::move(on_ready));
}

} // namespace fwd
