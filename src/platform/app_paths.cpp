// ps5fwdgen - Where the app's own files live, before and after elevation.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/app_paths.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace paths
{

namespace
{

std::string g_root;
std::string g_title;

bool readable(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::fclose(file);
    return true;
}

// Reads "titleId": "PPSAxxxxx" out of a param.json without a JSON parser,
// because this runs before anything else is set up.
std::string read_title(const std::string &param_path)
{
    std::FILE *file = std::fopen(param_path.c_str(), "rb");
    if (file == nullptr)
        return {};
    char buffer[4096];
    const std::size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[length] = '\0';
    const char *key = std::strstr(buffer, "\"titleId\"");
    if (key == nullptr)
        return {};
    const char *quote = std::strchr(key + 9, '"');
    if (quote == nullptr)
        return {};
    const char *end = std::strchr(quote + 1, '"');
    if (end == nullptr || end - quote != 10)
        return {};
    return std::string(quote + 1, static_cast<std::size_t>(end - quote - 1));
}

void resolve()
{
    g_root.clear();
    // The sandboxed view: /app0 is the title image. It is also where the
    // title ID is read first, so the other candidates can be formed.
    if (readable("/app0/sce_sys/param.json"))
    {
        g_root = "/app0";
        if (g_title.empty())
            g_title = read_title("/app0/sce_sys/param.json");
        return;
    }
    if (g_title.empty())
        return;
    const std::string candidates[] = {
        "/mnt/sandbox/" + g_title + "_000/app0",
        "/data/homebrew/" + g_title,
    };
    for (const std::string &candidate : candidates)
    {
        if (readable(candidate + "/sce_sys/param.json"))
        {
            g_root = candidate;
            return;
        }
    }
}

} // namespace

const std::string &app_root()
{
    if (g_root.empty())
        resolve();
    return g_root;
}

void refresh()
{
    g_root.clear();
    resolve();
}

std::string assets()
{
    return app_root() + "/assets";
}

const std::string &title_id()
{
    if (g_title.empty())
        (void)app_root();
    return g_title;
}

} // namespace paths
