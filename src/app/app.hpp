// ps5fwdgen - The application: screens, frame composition, shared state.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/preview_music.hpp"
#include "audio/cues.hpp"
#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <string>

namespace hui::gfx
{
class Renderer;
}

namespace fwd
{

class Screen;
struct Context;

// One frame as the screens draw it, back to front.
struct Frame
{
    hui::gfx::BackdropSpec backdrop;
    hui::gfx::DrawList scene;
    hui::gfx::DrawList overlay;
    bool glass = false;
    std::uint32_t glass_texture = 0;
};

class App
{
  public:
    App(const hui::ui::Fonts &fonts, hui::gfx::Renderer &renderer, std::string data_root,
        hui::audio::Mixer &mixer, bool elevated, const char *elevation_status);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    void set_version(std::string version);

    void update(const hui::InputFrame &input, float dt);
    void compose(hui::gfx::Renderer &renderer);

    const hui::ui::Feedback &feedback() const
    {
        return feedback_;
    }
    hui::audio::SoundSet sound_set() const;
    hui::gfx::Color accent() const;
    bool take_settings_changed();
    bool swap_confirm() const;
    bool quit_requested() const;

    // Development-only scripted-input hooks.
    void dev_update(const hui::InputFrame &input, float dt)
    {
        update(input, dt);
    }
    bool dev_type(const std::string &text);
    void dev_quit();

  private:
    struct Stack;
    void apply_navigation();

    std::unique_ptr<Context> context_;
    std::unique_ptr<PreviewMusic> music_;
    std::unique_ptr<Stack> stack_;
    hui::ui::Feedback feedback_;
    Frame frame_;
    hui::gfx::DrawList chrome_;
    float clock_ = 0.0f;
};

} // namespace fwd
