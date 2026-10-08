// ps5fwdgen - Choose a tile icon or background: from a file, or SteamGridDB.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/screens/art_screen.hpp"

#include "app/context.hpp"
#include "app/screens/file_picker_screen.hpp"
#include "app/screens/steamgriddb_screen.hpp"
#include "fwd/image.hpp"
#include "ui/components/list.hpp"
#include "ui/components/toast.hpp"
#include "ui/fonts.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

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

constexpr hui::gfx::Rect kListBounds{96.0f, 320.0f, 1200.0f, 400.0f};

class ArtScreen final : public Screen
{
  public:
    ArtScreen(Context &context, ArtKind kind, std::string suggested,
              std::function<void(std::vector<unsigned char>)> on_ready)
        : kind_(kind), suggested_(std::move(suggested)), on_ready_(std::move(on_ready))
    {
        restyle(context);
        list_.set_bounds(kListBounds);
        list_.set_active(true);
        std::vector<hui::ui::ListItem> items;
        items.push_back({"Search SteamGridDB", "online art for " +
                             (suggested_.empty() ? std::string("a game") : suggested_),
                         "", "", {}, true, false, false, 0});
        items.push_back({"Choose an image file", "PNG or JPEG from the console", "", "", {}, true,
                         false, false, 0});
        list_.set_items(std::move(items));
    }

    void restyle(Context &context) override
    {
        list_.style.theme = context.theme;
        list_.style.cards = true;
        toasts_.style.theme = context.theme;
    }

    // Decode an image file and encode it to the format this screen produces.
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

    // raw is an encoded image file (PNG/JPEG/...) in memory.
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
        const hui::ui::Event event = list_.handle(input, feedback);
        if (event == hui::ui::Event::activated)
        {
            ArtScreen *self = this;
            if (list_.focus() == 0)
            {
                context.push(make_steamgriddb_screen(
                    context, kind_, suggested_,
                    [self, &context](std::vector<unsigned char> raw)
                    { self->finish_from_encoded(context, raw); }));
            }
            else
            {
                context.push(make_file_picker_screen(
                    context, "/data",
                    {".png", ".jpg", ".jpeg", ".bmp"},
                    [self, &context](const std::string &path)
                    { self->finish_from_file(context, path); }));
            }
        }
        list_.update(dt);
        toasts_.update(dt, feedback);
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        const hui::gfx::Color muted =
            theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        hui::ui::text(scene.list, context.fonts.display,
                      kind_ == ArtKind::icon ? "Tile icon" : "Backgrounds", 96.0f, 150.0f, 46.0f,
                      text);
        hui::ui::text(scene.list, context.fonts.regular,
                      kind_ == ArtKind::icon ? "A 512x512 PNG, center-cropped from your image."
                                             : "A 4K background, encoded to BC7 DDS.",
                      96.0f, 220.0f, 26.0f, muted);
        list_.draw(scene);
        toasts_.draw(overlay);
        return false;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {
            {hui::ui::Button::cross, "Select"},
            {hui::ui::Button::circle, "Back"},
        };
        return kHints;
    }

  private:
    ArtKind kind_;
    std::string suggested_;
    std::function<void(std::vector<unsigned char>)> on_ready_;
    bool done_ = false;
    mutable hui::ui::ListView list_;
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
