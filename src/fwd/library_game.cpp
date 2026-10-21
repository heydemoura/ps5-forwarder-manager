// ps5fwdgen - One game in an emulator's library, as a forwarder can start it.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fwd/library_game.hpp"

#include "fwd/forwarder.hpp"
#include "fwd/eden_library.hpp"
#include "fwd/psx_library.hpp"
#include "fwd/x360_library.hpp"

namespace fwd
{

std::string library_game_name(const std::string &target, const std::string &path)
{
    switch (library_of(target))
    {
    case GameLibrary::eden:
        return eden_game_title(path);
    case GameLibrary::x360:
        return x360_game_name(path);
    case GameLibrary::psx:
    {
        const std::size_t slash = path.find_last_of('/');
        return psx_game_title(slash == std::string::npos ? path : path.substr(slash + 1));
    }
    case GameLibrary::none:
        break;
    }
    return path;
}

} // namespace fwd
