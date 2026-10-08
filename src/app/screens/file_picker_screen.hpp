// ps5fwdgen - A file browser used to pick a ROM (or any file) off /data.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fwd
{
struct Context;

// Opens at start_dir. extensions is a set of lower-case suffixes (".nsp",
// ".iso"); empty means every file. on_pick is called with the chosen absolute
// path, then the picker pops itself.
std::unique_ptr<Screen> make_file_picker_screen(Context &context, std::string start_dir,
                                                std::vector<std::string> extensions,
                                                std::function<void(const std::string &)> on_pick);
} // namespace fwd
