// ps5fwdgen - A screen: one page of the app that owns its focus and motion.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "ui/components/component.hpp"
#include "ui/feedback.hpp"
#include "ui/glyphs.hpp"

#include <span>
#include <string>

namespace fwd
{

struct Context;

class Screen
{
  public:
    virtual ~Screen() = default;

    // Became the top screen (first time, or again after the one above closed).
    virtual void enter(Context &context)
    {
        (void)context;
    }
    virtual void update(Context &context, const hui::InputFrame &input, float dt,
                        hui::ui::Feedback &feedback) = 0;
    // Draws the page into scene; anything modal (dialogs, prompts) into
    // overlay. Returns true when the overlay needs the frosted glass capture.
    virtual bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const = 0;
    // Button hints shown in the footer while this screen is on top.
    virtual std::span<const hui::ui::Hint> hints() const
    {
        return {};
    }
    // The theme was changed in settings: push it into every component.
    virtual void restyle(Context &context)
    {
        (void)context;
    }

    // Development-only: inject text into whatever prompt this screen has open.
    // Returns true when it was consumed. Default: ignored.
    virtual bool dev_inject_text(Context &context, const std::string &text)
    {
        (void)context;
        (void)text;
        return false;
    }
};

} // namespace fwd
