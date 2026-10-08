// ps5fwdgen - Home: the forwarders on the console, and where everything starts.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/screens/home_screen.hpp"

#include "app/context.hpp"
#include "ui/fonts.hpp"

#include <memory>
#include <string>

namespace fwd
{

namespace
{

class HomeScreen final : public Screen
{
  public:
    explicit HomeScreen(Context &context)
    {
        (void)context;
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        (void)dt;
        (void)feedback;
        if (input.is_pressed(hui::Action::menu))
            context.quit = true;
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        (void)overlay;
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        const hui::gfx::Color muted =
            theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        hui::ui::text(scene.list, context.fonts.display, "PS5 Forwarder Generator", 96.0f, 140.0f,
                      52.0f, text);
        const std::string status = context.elevated
                                       ? "Lapy elevation ok: /data is reachable"
                                       : "Lapy elevation failed: " + context.elevation_status;
        hui::ui::text(scene.list, context.fonts.regular, status, 96.0f, 210.0f, 28.0f, muted);
        hui::ui::text(scene.list, context.fonts.regular,
                      "Version " + (context.version.empty() ? std::string("dev") : context.version),
                      96.0f, 250.0f, 24.0f, muted);
        return false;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {{hui::ui::Button::options, "Quit"}};
        return kHints;
    }
};

} // namespace

std::unique_ptr<Screen> make_home_screen(Context &context)
{
    return std::make_unique<HomeScreen>(context);
}

} // namespace fwd
