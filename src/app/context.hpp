// ps5fwdgen - State every screen shares.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/settings.hpp"
#include "gfx/renderer.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fwd
{

class Screen;

// Shared by the app and its screens. Screens push and pop other screens
// through it; the app owns the stack and runs the top one.
struct Context
{
    const hui::ui::Fonts &fonts;
    hui::gfx::Renderer &renderer;
    hui::ui::Theme theme;
    Settings settings;
    std::string data_root;         // /download0/ps5fwdgen
    std::string app_template_root; // <app>/assets/forwarder-template
    bool elevated = false;   // /data is reachable (Lapy said yes)
    std::string elevation_status;
    std::string version;
    bool settings_changed = false; // main.cpp re-reads input settings
    bool theme_changed = false;    // the app restyles every screen
    bool quit = false;

    // Navigation requests, applied by the app between frames.
    std::vector<std::unique_ptr<Screen>> push_requests;
    int pop_requests = 0;

    void push(std::unique_ptr<Screen> screen)
    {
        push_requests.push_back(std::move(screen));
    }
    void pop()
    {
        ++pop_requests;
    }
    void save_settings();
};

} // namespace fwd
