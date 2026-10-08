// ps5fwdgen - Where the app's own files live, before and after elevation.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A sandboxed title reads itself at /app0. Once Lapy has lifted the sandbox
// the process root is the console's, so the same files are reached through
// the install folder instead. Nothing else in the app hard-codes /app0.
#pragma once

#include <string>

namespace paths
{

// Resolves the root that holds eboot.bin, sce_sys/ and assets/ for this
// title. Probes /app0 first, then the sandbox mount, then /data/homebrew.
// The result is cached; call refresh() after elevation changes the root.
const std::string &app_root();
void refresh();

// Convenience: app_root() + "/assets".
std::string assets();

// The title ID read from the app's own param.json (empty when unreadable).
const std::string &title_id();

} // namespace paths
