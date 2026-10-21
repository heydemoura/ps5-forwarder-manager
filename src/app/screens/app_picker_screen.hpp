// ps5fwdgen - Picks one entry of a target app's library (a PC's apps, a list
// of games), each with its box art.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"
#include "fwd/stream_catalog.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fwd
{
struct Context;

struct PickItem
{
    std::string title;
    std::string value;    // shown at the end of the row ("no box art yet")
    std::string subtitle; // e.g. a ROM path, to distinguish games sharing a title
    // The box art, encoded (PNG, JPEG or TGA); unset when there is none. Called a
    // picture at a time around the focus.
    std::function<std::vector<unsigned char>()> art;
};

struct PickList
{
    std::string heading;          // "Pick an app"
    std::string caption;          // under the heading
    std::string fallback_title;   // the first row, for anything not in the list
    std::string fallback_caption; // its subtitle
    std::string no_art;           // shown in place of missing box art
    std::vector<PickItem> items;
};

// Lists the items with their box art. on_pick gets the chosen item's index, or
// -1 for the first row (the player would rather choose another way).
std::unique_ptr<Screen> make_list_picker_screen(Context &context, PickList list,
                                                std::function<void(int)> on_pick);

// The PC's apps (fwd/stream_catalog.hpp). on_pick gets the chosen app, or
// nullptr when the player would rather type a name.
std::unique_ptr<Screen> make_app_picker_screen(Context &context, StreamPc pc,
                                               std::function<void(const StreamApp *)> on_pick);
} // namespace fwd
