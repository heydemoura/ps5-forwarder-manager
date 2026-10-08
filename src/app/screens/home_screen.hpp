// ps5fwdgen - Home: the forwarders on the console, and where everything starts.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"

#include <memory>

namespace fwd
{

struct Context;

std::unique_ptr<Screen> make_home_screen(Context &context);

} // namespace fwd
