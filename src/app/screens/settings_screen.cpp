// ps5fwdgen - Settings: where forwarders live, the SteamGridDB key, the theme.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/screens/settings_screen.hpp"

#include "app/context.hpp"
#include "ui/components/form.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/fonts.hpp"
#include "platform/ps5/system.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fwd
{

namespace
{

enum Row
{
    RowRoot = 1,
    RowKey,
    RowTheme,
    RowSwap,
    RowSounds,
    RowMotion,
};

enum class Prompting
{
    none,
    root,
    key,
};

constexpr hui::gfx::Rect kFormBounds{96.0f, 300.0f, 1728.0f, 640.0f};

std::string mask_key(const std::string &key)
{
    if (key.empty())
        return "(not set)";
    if (key.size() <= 4)
        return "set";
    return "set, ..." + key.substr(key.size() - 4);
}

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
            kb.done = hui::Action::page_next; // R1 confirms (a console keyboard nicety)
            prompt_.keyboard.style.bindings = kb;
        }
        prompt_.style.buttons = false; // single Done: the keyboard's own key (closes on press)
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
        std::vector<std::string> theme_names;
        for (const hui::ui::Theme &theme : hui::ui::themes())
            theme_names.push_back(theme.name);
        const int theme_index =
            context.settings.theme >= 0 &&
                    context.settings.theme < static_cast<int>(theme_names.size())
                ? context.settings.theme
                : 0;

        form_.add_header("Storage");
        form_.add_action(RowRoot, "Forwarders folder").text = context.settings.forwarders_root;
        form_.add_header("SteamGridDB");
        form_.add_action(RowKey, "API key").text = mask_key(context.settings.steamgriddb_key);
        form_.add_header("Appearance & input");
        form_.add_choice(RowTheme, "Theme", theme_names, theme_index);
        form_.add_toggle(RowSwap, "Circle confirms (swap X/O)", context.settings.swap_confirm);
        form_.add_toggle(RowSounds, "Interface sounds", context.settings.sounds);
        form_.add_toggle(RowMotion, "Reduced motion", context.settings.reduced_motion);
        if (keep != 0)
            form_.focus_row(keep, true);
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (prompt_.is_open())
        {
            const hui::ui::Event event = prompt_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
            {
                if (prompting_ == Prompting::root && !prompt_.text().empty())
                    context.settings.forwarders_root = prompt_.text();
                else if (prompting_ == Prompting::key)
                    context.settings.steamgriddb_key = prompt_.text();
                context.save_settings();
                build(context);
            }
            if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
                prompting_ = Prompting::none;
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
            case RowTheme:
                context.settings.theme = form_.choice_index(RowTheme);
                context.theme_changed = true;
                break;
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
        else if (event == hui::ui::Event::activated)
        {
            if (form_.focus_id() == RowRoot)
            {
                prompting_ = Prompting::root;
                prompt_.set_title("Forwarders folder (/data/homebrew)");
                prompt_.style.max_length = 120;
                prompt_.open(feedback, context.settings.forwarders_root);
            }
            else if (form_.focus_id() == RowKey)
            {
                prompting_ = Prompting::key;
                prompt_.set_title("SteamGridDB API key");
                prompt_.style.max_length = 64;
                prompt_.open(feedback, context.settings.steamgriddb_key);
            }
        }
        form_.update(dt);
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        if (!prompt_.is_open())
            return false;
        if (prompting_ == Prompting::root && !text.empty())
            context.settings.forwarders_root = text;
        else if (prompting_ == Prompting::key)
            context.settings.steamgriddb_key = text;
        context.save_settings();
        hui::sys::log("[FWD] settings key set len=%zu", context.settings.steamgriddb_key.size());
        prompting_ = Prompting::none;
        prompt_.dismiss();
        build(context);
        return true;
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        hui::ui::text(scene.list, context.fonts.display, "Settings", 96.0f, 150.0f, 46.0f, text);
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
    Prompting prompting_ = Prompting::none;
};

} // namespace

std::unique_ptr<Screen> make_settings_screen(Context &context)
{
    return std::make_unique<SettingsScreen>(context);
}

} // namespace fwd
