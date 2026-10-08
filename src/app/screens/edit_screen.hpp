// ps5fwdgen - Create or edit one forwarder.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/screen.hpp"
#include "fwd/forwarder.hpp"

#include <memory>

namespace fwd
{
struct Context;
std::unique_ptr<Screen> make_edit_screen(Context &context, Forwarder initial, bool editing);
} // namespace fwd
