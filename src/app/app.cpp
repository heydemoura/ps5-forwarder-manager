// ps5fwdgen - The application: screens, frame composition, shared state.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "app/context.hpp"
#include "app/screen.hpp"
#include "app/screens/home_screen.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/component.hpp"
#include "ui/glyphs.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

constexpr float kHintsRight = 1824.0f;

const hui::ui::Theme &theme_at(int index)
{
    const std::span<const hui::ui::Theme> all = hui::ui::themes();
    if (index < 0 || static_cast<std::size_t>(index) >= all.size())
        return hui::ui::default_theme();
    return all[static_cast<std::size_t>(index)];
}

} // namespace

void Context::save_settings()
{
    settings.save(data_root + "/settings.txt");
}

struct App::Stack
{
    std::vector<std::unique_ptr<Screen>> screens;
};

App::App(const hui::ui::Fonts &fonts, hui::gfx::Renderer &renderer, std::string data_root,
         bool elevated, const char *elevation_status)
    : context_(new Context{fonts, renderer, hui::ui::default_theme(), Settings{}}), stack_(new Stack)
{
    context_->data_root = std::move(data_root);
    context_->settings = Settings::load(context_->data_root + "/settings.txt");
    context_->theme = theme_at(context_->settings.theme);
    context_->elevated = elevated;
    context_->elevation_status = elevation_status;
    frame_.glass_texture = renderer.glass_texture();
    stack_->screens.push_back(make_home_screen(*context_));
    stack_->screens.back()->enter(*context_);
}

App::~App() = default;

void App::set_version(std::string version)
{
    context_->version = std::move(version);
}

hui::audio::SoundSet App::sound_set() const
{
    return context_->theme.sounds;
}

hui::gfx::Color App::accent() const
{
    return context_->theme.accent;
}

bool App::take_settings_changed()
{
    const bool changed = context_->settings_changed;
    context_->settings_changed = false;
    return changed;
}

bool App::swap_confirm() const
{
    return context_->settings.swap_confirm;
}

bool App::quit_requested() const
{
    return context_->quit;
}

void App::apply_navigation()
{
    Context &context = *context_;
    bool changed = false;
    while (context.pop_requests > 0 && stack_->screens.size() > 1)
    {
        stack_->screens.pop_back();
        --context.pop_requests;
        changed = true;
    }
    context.pop_requests = 0;
    for (std::unique_ptr<Screen> &screen : context.push_requests)
    {
        stack_->screens.push_back(std::move(screen));
        changed = true;
    }
    context.push_requests.clear();
    if (changed && !stack_->screens.empty())
        stack_->screens.back()->enter(context);
}

void App::update(const hui::InputFrame &input, float dt)
{
    feedback_.clear();
    clock_ += dt;
    apply_navigation();
    Context &context = *context_;
    if (context.theme_changed)
    {
        context.theme = theme_at(context.settings.theme);
        for (std::unique_ptr<Screen> &screen : stack_->screens)
            screen->restyle(context);
        context.theme_changed = false;
    }
    if (!stack_->screens.empty())
        stack_->screens.back()->update(context, input, dt, feedback_);
    apply_navigation();
    if (!context.settings.sounds)
        feedback_.cues.clear();
}

void App::compose(hui::gfx::Renderer &renderer)
{
    Context &context = *context_;
    renderer.begin();
    frame_.scene.clear();
    frame_.overlay.clear();
    frame_.backdrop = context.theme.backdrop;
    frame_.backdrop.time = clock_;
    frame_.glass = false;
    if (!stack_->screens.empty())
    {
        const Screen &screen = *stack_->screens.back();
        hui::ui::Canvas scene{frame_.scene, context.fonts, 0, clock_};
        hui::ui::Canvas overlay{frame_.overlay, context.fonts, frame_.glass_texture, clock_};
        frame_.glass = screen.draw(context, scene, overlay);
        chrome_.clear();
        const std::span<const hui::ui::Hint> hints = screen.hints();
        if (!hints.empty())
        {
            const hui::ui::GlyphStyle style =
                context.theme.dark ? hui::ui::GlyphStyle::dark() : hui::ui::GlyphStyle::light();
            hui::ui::draw_hints(chrome_, context.fonts, style, hints.data(),
                                static_cast<int>(hints.size()), kHintsRight, true);
        }
    }
    renderer.backdrop(frame_.backdrop);
    renderer.draw(frame_.scene);
    renderer.draw(chrome_);
    if (frame_.glass)
        renderer.glass();
    renderer.draw(frame_.overlay);
}

} // namespace fwd
