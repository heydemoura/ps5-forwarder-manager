// ps5fwdgen - Settings: where forwarders live, the SteamGridDB key, the theme.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"

#include <memory>

namespace fwd
{
struct Context;
std::unique_ptr<Screen> make_settings_screen(Context &context);
} // namespace fwd
