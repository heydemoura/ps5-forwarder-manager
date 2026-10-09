// ps5fwdgen - Settings: where forwarders live, plus input and sound options.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SteamGridDB key is injected at build time from an environment value
// (a GitHub secret in CI), so it is not editable here. The theme is fixed to
// match the home screen's design language, so there is no theme option.

#include "platform/launcher_inject.hpp"
#include "app/screens/settings_screen.hpp"

#include "app/context.hpp"
#include "app/tile.hpp"
#include "ui/components/form.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/fonts.hpp"

#include <memory>
#include <span>
#include <string>

namespace fwd
{

namespace
{

enum Row
{
    RowRoot = 1,
    RowSwap,
    RowSounds,
    RowMotion,
};

constexpr hui::gfx::Rect kFormBounds{96.0f, 300.0f, 1200.0f, 620.0f};

class SettingsScreen final : public Screen
{
  public:
    explicit SettingsScreen(Context &context)
    {
        restyle(context);
        form_.set_bounds(kFormBounds);
        form_.set_active(true);
        {
            auto kb = hui::ui::KeyboardBindings::standard();
            kb.done = hui::Action::page_next; // R1 confirms
            prompt_.keyboard.style.bindings = kb;
        }
        prompt_.style.buttons = false; // single Done
        build(context);
    }

    void restyle(Context &context) override
    {
        form_.style.theme = context.theme;
        form_.style.panel = true;
        prompt_.style.theme = context.theme;
    }

    void build(Context &context)
    {
        const int keep = form_.focus_id();
        form_.clear();
        form_.add_header("Storage");
        form_.add_action(RowRoot, "Forwarders folder").text = context.settings.forwarders_root;
        form_.add_header("Input & sound");
        form_.add_toggle(RowSwap, "Circle confirms (swap X/O)", context.settings.swap_confirm);
        form_.add_toggle(RowSounds, "Interface sounds", context.settings.sounds);
        form_.add_toggle(RowMotion, "Reduced motion", context.settings.reduced_motion);
        if (keep != 0)
            form_.focus_row(keep, true);
    }

    void backdrop(Context &context, hui::gfx::BackdropSpec &spec) const override
    {
        (void)context;
        spec = app_backdrop();
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (prompt_.is_open())
        {
            const hui::ui::Event event = prompt_.handle(input, feedback);
            if (event == hui::ui::Event::activated && !prompt_.text().empty())
            {
                context.settings.forwarders_root = prompt_.text();
                context.save_settings();
                build(context);
            }
            prompt_.update(dt);
            return;
        }
        prompt_.update(dt);
        if (input.is_pressed(hui::Action::back))
        {
            context.save_settings();
            context.pop();
            return;
        }
        const hui::ui::Event event = form_.handle(input, feedback);
        if (event == hui::ui::Event::changed)
        {
            switch (form_.changed_id())
            {
            case RowSwap:
                context.settings.swap_confirm = form_.toggle_value(RowSwap);
                context.settings_changed = true;
                break;
            case RowSounds:
                context.settings.sounds = form_.toggle_value(RowSounds);
                break;
            case RowMotion:
                context.settings.reduced_motion = form_.toggle_value(RowMotion);
                break;
            default:
                break;
            }
            context.save_settings();
        }
        else if (event == hui::ui::Event::activated && form_.focus_id() == RowRoot)
        {
            prompt_.set_title("Forwarders folder (/data/homebrew)");
            prompt_.style.max_length = 120;
            prompt_.open(feedback, context.settings.forwarders_root);
        }
        form_.update(dt);
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        if (!prompt_.is_open())
            return false;
        if (!text.empty())
            context.settings.forwarders_root = text;
        context.save_settings();
        prompt_.dismiss();
        build(context);
        return true;
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::gfx::Color white = hui::gfx::Color::rgb(0xffffff);
        hui::ui::text(scene.list, context.fonts.display, "Settings", 96.0f, 150.0f, 46.0f, white);
        hui::ui::text(scene.list, context.fonts.regular,
                      "SteamGridDB uses a key built into the app.", 96.0f, 206.0f, 24.0f,
                      white.with_alpha(0.55f));
        // Which launcher serves the forwarder tiles (platform/launcher_inject).
        const launcher::State state = launcher::state();
        const bool bad = state == launcher::State::no_elfldr || state == launcher::State::failed;
        hui::ui::text(scene.list, context.fonts.regular, launcher::describe(state), 1824.0f,
                      206.0f, 24.0f,
                      bad ? context.theme.warning : white.with_alpha(0.55f), hui::gfx::Align::right);
        form_.draw(scene);
        const bool modal = prompt_.visible();
        prompt_.draw(overlay);
        return modal;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {
            {hui::ui::Button::cross, "Change"},
            {hui::ui::Button::circle, "Back"},
        };
        return kHints;
    }

  private:
    mutable hui::ui::Form form_;
    mutable hui::ui::InputPrompt prompt_;
};

} // namespace

std::unique_ptr<Screen> make_settings_screen(Context &context)
{
    return std::make_unique<SettingsScreen>(context);
}

} // namespace fwd
