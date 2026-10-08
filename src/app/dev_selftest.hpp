// ps5fwdgen - A development-only self-test of the forwarder write path.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// There is no way to drive the controller from a host, so this lets a run
// exercise the real write_forwarder() path on hardware: drop a trigger file
// and the app generates a forwarder (icon encoded in-process), logs the
// result to the kernel log, and removes the trigger. Off by default; it does
// nothing unless the trigger file exists.
#pragma once

#include <string>

namespace fwd
{

// Runs once at startup when <forwarders_root sibling> trigger exists. Reads
// /data/ps5fwdgen-dev/selftest.txt (optional "name|target|rom" line), writes a
// forwarder, logs "[FWD] selftest ...", removes the trigger. template_root is
// the app's assets/forwarder-template path.
void run_dev_selftest(const std::string &forwarders_root, const std::string &template_root);

} // namespace fwd
