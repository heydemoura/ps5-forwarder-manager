// ps5fwdgen - Development-only scripted input, to drive the real UI remotely.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// There is no controller on the build host, so this reads a script file over
// the console's filesystem and turns each line into a synthetic InputFrame (or
// a text-injection), letting a run exercise the actual screens. Off unless the
// script file exists; it is consumed once.
#pragma once

#include "core/input.hpp"

#include <string>

namespace dev_input
{

enum class Kind
{
    none,   // queue empty
    button, // feed `frame` to the app this frame
    text,   // inject `text` into the active prompt
    quit,   // close the app
};

struct Command
{
    Kind kind = Kind::none;
    hui::InputFrame frame;
    std::string text;
};

// Reload the script from /data/ps5fwdgen-dev/input.txt when present (consumes
// the file). Safe to call every frame; it only touches disk when a file is
// there.
void poll();

// Pop the next scripted command. Returns {Kind::none} when nothing is queued.
Command next();

} // namespace dev_input
