// ps5fwdgen - Host tests for feature flags and the targets they hide.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built twice: with the default flags, and with FWD_FEATURE_PAYLOAD_TARGET=1
// (EXPECT_PAYLOAD=1).

#include "fwd/features.hpp"
#include "fwd/forwarder.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef EXPECT_PAYLOAD
#define EXPECT_PAYLOAD 0
#endif

namespace
{

int failures = 0;

void expect(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "failed: %s\n", what);
        ++failures;
    }
}

bool listed(const char *title_id)
{
    for (const fwd::Target &target : fwd::known_targets())
        if (std::strcmp(target.title_id, title_id) == 0)
            return true;
    return false;
}

} // namespace

int main()
{
    constexpr bool payload = EXPECT_PAYLOAD != 0;
    static_assert(fwd::enabled(fwd::Feature::none), "unflagged parts are always on");
    static_assert(fwd::enabled(fwd::Feature::payload_target) == payload, "payload default");
    expect(std::string(fwd::feature_name(fwd::Feature::payload_target)) == "PAYLOAD_TARGET",
           "flag name");

    expect(listed("PPSA00000") == payload, "Payload listed only with its flag");
    expect(fwd::target_display_name("PPSA00000") == (payload ? "Payload" : "PPSA00000"),
           "Payload's display name follows its flag");
    // Unflagged targets and the generic entry (last, as the picker expects) stay.
    expect(listed("PPSA99008") && listed("PPSA99002"), "unflagged targets listed");
    expect(fwd::known_targets().back().title_id[0] == '\0', "generic stays last");
    expect(fwd::target_display_name("PPSA99008") == "ProsperoEden", "unflagged display name");
    // ProsperoEden ships under two title IDs; the mirror is listed as such.
    expect(listed("PPSA99016"), "mirror ProsperoEden title ID listed");
    expect(fwd::target_display_name("PPSA99016") == "ProsperoEden ( Mirror )",
           "mirror ProsperoEden display name");

    if (failures != 0)
        return 1;
    std::printf("Feature flag checks passed (payload=%d).\n", payload ? 1 : 0);
    return 0;
}
