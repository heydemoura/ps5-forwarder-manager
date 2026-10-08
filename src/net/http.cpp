// ps5fwdgen - A tiny blocking HTTPS GET, for use on a worker thread.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "net/http.hpp"

#include "net/console_curl.h"

#include <curl/curl.h>

#include <mutex>

namespace net
{

namespace
{

std::once_flag g_once;

std::size_t on_body(char *data, std::size_t size, std::size_t count, void *user)
{
    const std::size_t total = size * count;
    auto *out = static_cast<std::vector<unsigned char> *>(user);
    out->insert(out->end(), reinterpret_cast<unsigned char *>(data),
                reinterpret_cast<unsigned char *>(data) + total);
    return total;
}

} // namespace

void global_init()
{
    std::call_once(g_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Response get(const std::string &url, const std::string &bearer, long timeout_ms)
{
    global_init();
    Response response;
    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        response.error = "curl_easy_init failed";
        return response;
    }
    console_curl_setup(easy);
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ps5fwdgen/1.0");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body);

    struct curl_slist *headers = nullptr;
    std::string auth;
    if (!bearer.empty())
    {
        auth = "Authorization: Bearer " + bearer;
        headers = curl_slist_append(headers, auth.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    }

    char error_buffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer);

    const CURLcode code = curl_easy_perform(easy);
    response.curl_code = static_cast<int>(code);
    if (code == CURLE_OK)
    {
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response.status);
    }
    else
    {
        response.status = 0;
        response.error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(code);
    }

    if (headers != nullptr)
        curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    return response;
}

} // namespace net
