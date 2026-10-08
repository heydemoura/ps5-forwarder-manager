// ps5fwdgen - Home: the forwarders on the console, and where everything starts.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/screens/home_screen.hpp"

#include "app/context.hpp"
#include "app/screens/edit_screen.hpp"
#include "app/screens/settings_screen.hpp"
#include "fwd/store.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/list.hpp"
#include "ui/components/toast.hpp"
#include "ui/fonts.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fwd
{

namespace
{

constexpr hui::gfx::Rect kListBounds{96.0f, 300.0f, 1728.0f, 660.0f};

class HomeScreen final : public Screen
{
  public:
    explicit HomeScreen(Context &context)
    {
        restyle(context);
        list_.set_bounds(kListBounds);
        list_.set_active(true);
    }

    void restyle(Context &context) override
    {
        list_.style.theme = context.theme;
        list_.style.cards = true;
        list_.style.dividers = false;
        dialog_.style.theme = context.theme;
        toasts_.style.theme = context.theme;
    }

    void enter(Context &context) override
    {
        rescan(context);
    }

    void rescan(Context &context)
    {
        forwarders_ = scan_forwarders(context.settings.forwarders_root);
        std::vector<hui::ui::ListItem> items;
        items.reserve(forwarders_.size());
        for (const Forwarder &forwarder : forwarders_)
        {
            hui::ui::ListItem item;
            item.title = forwarder.display_name.empty() ? forwarder.title_id
                                                        : forwarder.display_name;
            std::string sub = forwarder.title_id + "  ->  " + forwarder.target;
            if (!forwarder.rom.empty())
                sub += "   " + forwarder.rom;
            item.subtitle = sub;
            item.chevron = true;
            items.push_back(std::move(item));
        }
        list_.set_items(std::move(items));
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (dialog_.is_open())
        {
            const hui::ui::Event event = dialog_.handle(input, feedback);
            if (event == hui::ui::Event::activated && dialog_.choice() == 1 && pending_delete_ >= 0)
            {
                const Forwarder &target = forwarders_[static_cast<std::size_t>(pending_delete_)];
                const bool ok = remove_forwarder(context.settings.forwarders_root, target.title_id);
                toasts_.push(ok ? hui::ui::StatusKind::success : hui::ui::StatusKind::danger,
                             ok ? "Deleted " + target.title_id : "Could not delete",
                             ok ? "ShadowMountPlus will drop the tile on its next scan." : "");
                pending_delete_ = -1;
                rescan(context);
            }
            else if (event == hui::ui::Event::cancelled)
            {
                pending_delete_ = -1;
            }
            dialog_.update(dt);
            toasts_.update(dt, feedback);
            return;
        }

        if (!context.elevated)
        {
            if (input.is_pressed(hui::Action::menu))
                context.push(make_settings_screen(context));
            if (input.is_pressed(hui::Action::north))
                context.quit = true;
            toasts_.update(dt, feedback);
            return;
        }

        if (input.is_pressed(hui::Action::north)) // Triangle: new
        {
            feedback.play(hui::audio::Cue::open);
            context.push(make_edit_screen(context, Forwarder{}, false));
            return;
        }
        if (input.is_pressed(hui::Action::menu)) // Options: settings
        {
            context.push(make_settings_screen(context));
            return;
        }
        if (input.is_pressed(hui::Action::west) && !forwarders_.empty()) // Square: delete
        {
            pending_delete_ = list_.focus();
            if (pending_delete_ >= 0 &&
                pending_delete_ < static_cast<int>(forwarders_.size()))
            {
                const Forwarder &target = forwarders_[static_cast<std::size_t>(pending_delete_)];
                dialog_.open({hui::ui::StatusKind::danger, "Delete this forwarder?",
                              target.display_name + "\n" + target.title_id +
                                  "\nThis removes the tile folder from the console.",
                              {{"Cancel", hui::ui::ButtonKind::secondary, false},
                               {"Delete", hui::ui::ButtonKind::primary, true}},
                              0},
                             feedback);
            }
            return;
        }

        const hui::ui::Event event = list_.handle(input, feedback);
        if (event == hui::ui::Event::activated)
        {
            const int index = list_.focus();
            if (index >= 0 && index < static_cast<int>(forwarders_.size()))
                context.push(make_edit_screen(
                    context, forwarders_[static_cast<std::size_t>(index)], true));
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
        hui::ui::text(scene.list, context.fonts.display, "PS5 Forwarder Generator", 96.0f, 150.0f,
                      52.0f, text);

        if (!context.elevated)
        {
            hui::ui::text(scene.list, context.fonts.semibold,
                          "Lapy JB Daemon is not running", 96.0f, 360.0f, 34.0f, theme.danger);
            hui::ui::paragraph(
                scene.list, context.fonts.regular,
                "This app needs ArkSama's PS5-Lapy-JB-Daemon loaded (via your payload loader or "
                "autoload) to reach /data/homebrew. Load it, then relaunch this app. Elevation "
                "result: " +
                    context.elevation_status + ".",
                96.0f, 420.0f, 26.0f, 1500.0f, 1.5f, muted, 6);
            toasts_.draw(overlay);
            return false;
        }

        const std::string count = std::to_string(forwarders_.size()) + " forwarder" +
                                  (forwarders_.size() == 1 ? "" : "s") + " in " +
                                  context.settings.forwarders_root;
        hui::ui::text(scene.list, context.fonts.regular, count, 96.0f, 220.0f, 26.0f, muted);
        if (forwarders_.empty())
            hui::ui::text(scene.list, context.fonts.regular,
                          "No forwarders yet. Press Triangle to make one.", 96.0f, 400.0f, 28.0f,
                          muted);
        else
            list_.draw(scene);

        const bool modal = dialog_.visible();
        dialog_.draw(overlay);
        toasts_.draw(overlay);
        return modal;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kElevated[] = {
            {hui::ui::Button::cross, "Edit"},
            {hui::ui::Button::triangle, "New"},
            {hui::ui::Button::square, "Delete"},
            {hui::ui::Button::options, "Settings"},
        };
        static const hui::ui::Hint kSandboxed[] = {
            {hui::ui::Button::options, "Settings"},
            {hui::ui::Button::triangle, "Quit"},
        };
        return elevated_ ? std::span<const hui::ui::Hint>(kElevated)
                         : std::span<const hui::ui::Hint>(kSandboxed);
    }

    void set_elevated(bool value)
    {
        elevated_ = value;
    }

  private:
    std::vector<Forwarder> forwarders_;
    mutable hui::ui::ListView list_;
    mutable hui::ui::Dialog dialog_;
    mutable hui::ui::ToastStack toasts_;
    int pending_delete_ = -1;
    bool elevated_ = false;
};

} // namespace

std::unique_ptr<Screen> make_home_screen(Context &context)
{
    auto screen = std::make_unique<HomeScreen>(context);
    screen->set_elevated(context.elevated);
    return screen;
}

} // namespace fwd
