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
    long status = 0;   // HTTP status, or 0 on a transport error
    int curl_code = 0; // CURLcode (0 = ok)
    std::string error; // human-readable, when status == 0
    std::vector<unsigned char> body;
};

// A blocking HTTPS GET. bearer, when non-empty, is sent as
// "Authorization: Bearer <bearer>". Follows redirects (https only).
Response get(const std::string &url, const std::string &bearer = {}, long timeout_ms = 20000);

// A multipart/form-data POST of one file field. field is the form field name,
// filename the upload filename, data the file bytes. For the forwarder site's
// /api/convert-at9 endpoint. Blocking; worker thread only.
Response post_file(const std::string &url, const std::string &field, const std::string &filename,
                   const std::vector<unsigned char> &data, long timeout_ms = 60000);

// A raw POST of a byte body (http or https). Dev-only: used to ship debug
// screenshots to the build host, which has no TLS. Blocking; worker/dev use.
Response post_bytes(const std::string &url, const std::vector<unsigned char> &data,
                    const std::string &content_type = "application/octet-stream",
                    long timeout_ms = 15000);

} // namespace net
