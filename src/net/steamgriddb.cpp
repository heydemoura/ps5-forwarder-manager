// ps5fwdgen - SteamGridDB API v2 client (search, grids, heroes, download).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "net/steamgriddb.hpp"

#include "core/json.hpp"
#include "net/http.hpp"

#include <string>

namespace sgdb
{

namespace
{

constexpr const char *kBase = "https://www.steamgriddb.com/api/v2";

// Percent-encode a path segment (the search term).
std::string encode_segment(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved)
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// Parse a response body, verify "success": true, hand back the "data" value.
bool parse_envelope(const std::vector<unsigned char> &body, json::Value &data, std::string &error)
{
    std::string text(body.begin(), body.end());
    json::Value root;
    if (!json::parse(text, root, nullptr) || !root.is_object())
    {
        error = "unexpected response";
        return false;
    }
    if (!root.get("success").as_bool())
    {
        const json::Value &errors = root.get("errors");
        error = errors.size() > 0 ? std::string(errors.at(0).as_string("request failed"))
                                  : "request failed";
        return false;
    }
    const json::Value *found = root.find_mutable("data");
    if (found == nullptr)
    {
        error = "no data";
        return false;
    }
    data = *found;
    return true;
}

Result request_json(const std::string &url, const std::string &key, json::Value &data)
{
    if (key.empty())
        return {false, "set a SteamGridDB API key in Settings"};
    const net::Response response = net::get(url, key);
    if (response.status == 0)
        return {false, response.error.empty() ? "network error" : response.error};
    if (response.status == 401)
        return {false, "SteamGridDB rejected the API key"};
    if (response.status == 429)
        return {false, "rate limited; try again shortly"};
    std::string error;
    if (!parse_envelope(response.body, data, error))
    {
        if (response.status >= 400)
            return {false, "HTTP " + std::to_string(response.status)};
        return {false, error};
    }
    return {true, {}};
}

void collect_assets(const json::Value &data, std::vector<Asset> &out)
{
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const json::Value &item = data.at(i);
        Asset asset;
        asset.id = item.get("id").as_int();
        asset.url = std::string(item.get("url").as_string());
        asset.thumb = std::string(item.get("thumb").as_string());
        asset.width = static_cast<int>(item.get("width").as_int());
        asset.height = static_cast<int>(item.get("height").as_int());
        asset.style = std::string(item.get("style").as_string());
        asset.author = std::string(item.get("author").get("name").as_string());
        if (!asset.url.empty())
            out.push_back(std::move(asset));
    }
}

} // namespace

Result search(const std::string &key, const std::string &term, std::vector<Game> &out)
{
    out.clear();
    json::Value data;
    const Result result = request_json(
        std::string(kBase) + "/search/autocomplete/" + encode_segment(term), key, data);
    if (!result.ok)
        return result;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        Game game;
        game.id = data.at(i).get("id").as_int();
        game.name = std::string(data.at(i).get("name").as_string());
        if (game.id != 0)
            out.push_back(std::move(game));
    }
    return {true, {}};
}

Result assets(const std::string &key, Kind kind, long game_id, std::vector<Asset> &out)
{
    out.clear();
    const std::string id = std::to_string(game_id);
    if (kind == Kind::background)
    {
        json::Value data;
        const Result result =
            request_json(std::string(kBase) + "/heroes/game/" + id +
                             "?dimensions=3840x1240,1920x620&types=static&nsfw=false&humor=false",
                         key, data);
        if (!result.ok)
            return result;
        collect_assets(data, out);
        return {true, {}};
    }

    // Icon: prefer square grids, then fall back to large square icons.
    json::Value grids;
    const Result grid_result =
        request_json(std::string(kBase) + "/grids/game/" + id +
                         "?dimensions=1024x1024,512x512&types=static&nsfw=false&humor=false",
                     key, grids);
    if (!grid_result.ok)
        return grid_result;
    collect_assets(grids, out);

    json::Value icons;
    const Result icon_result =
        request_json(std::string(kBase) + "/icons/game/" + id +
                         "?mimes=image/png&types=static&nsfw=false&humor=false",
                     key, icons);
    if (icon_result.ok)
        collect_assets(icons, out);
    return {true, {}};
}

Result download(const std::string &url, std::vector<unsigned char> &out)
{
    const net::Response response = net::get(url, {}, 30000);
    if (response.status == 0)
        return {false, response.error.empty() ? "download failed" : response.error};
    if (response.status != 200)
        return {false, "HTTP " + std::to_string(response.status)};
    out = response.body;
    if (out.empty())
        return {false, "empty download"};
    return {true, {}};
}

} // namespace sgdb
