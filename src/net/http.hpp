// ps5fwdgen - A tiny blocking HTTPS GET, for use on a worker thread.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app elevates, so sceHttp/sceSsl reject public certificates; this uses
// libcurl + OpenSSL through console_curl, as the boilerplate's CURL guide
// prescribes. Never call it on the render thread.
#pragma once

#include <string>
#include <vector>

namespace net
{

// Call once, from one thread, before any get().
void global_init();

struct Response
{
    long status = 0;      // HTTP status, or 0 on a transport error
    int curl_code = 0;    // CURLcode (0 = ok)
    std::string error;    // human-readable, when status == 0
    std::vector<unsigned char> body;
};

// A blocking HTTPS GET. bearer, when non-empty, is sent as
// "Authorization: Bearer <bearer>". Follows redirects (https only).
Response get(const std::string &url, const std::string &bearer = {}, long timeout_ms = 20000);

} // namespace net
