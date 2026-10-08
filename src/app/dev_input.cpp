// ps5fwdgen - Development-only scripted input, to drive the real UI remotely.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/dev_input.hpp"

#include <cstdio>
#include <deque>
#include <string>
#include <unistd.h>

namespace dev_input
{

namespace
{

constexpr char kScript[] = "/data/ps5fwdgen-dev/input.txt";

std::deque<Command> g_queue;

hui::InputFrame button(hui::Action action, hui::Direction nav)
{
    hui::InputFrame frame;
    frame.connected = true;
    frame.pressed = hui::action_bit(action);
    frame.held = frame.pressed;
    frame.nav = nav;
    return frame;
}

Command parse_line(const std::string &line)
{
    Command command;
    if (line.empty() || line[0] == '#')
        return command; // Kind::none, skipped
    if (line.rfind("type:", 0) == 0)
    {
        command.kind = Kind::text;
        command.text = line.substr(5);
        return command;
    }
    if (line == "quit")
    {
        command.kind = Kind::quit;
        return command;
    }
    if (line == "shot")
    {
        command.kind = Kind::shot;
        return command;
    }
    struct Map
    {
        const char *name;
        hui::Action action;
        hui::Direction nav;
    };
    static const Map kMap[] = {
        {"up", hui::Action::up, hui::Direction::up},
        {"down", hui::Action::down, hui::Direction::down},
        {"left", hui::Action::left, hui::Direction::left},
        {"right", hui::Action::right, hui::Direction::right},
        {"cross", hui::Action::confirm, hui::Direction::none},
        {"circle", hui::Action::back, hui::Direction::none},
        {"triangle", hui::Action::north, hui::Direction::none},
        {"square", hui::Action::west, hui::Direction::none},
        {"l1", hui::Action::page_prev, hui::Direction::none},
        {"r1", hui::Action::page_next, hui::Direction::none},
        {"l2", hui::Action::jump_prev, hui::Direction::none},
        {"r2", hui::Action::jump_next, hui::Direction::none},
        {"options", hui::Action::menu, hui::Direction::none},
    };
    for (const Map &entry : kMap)
    {
        if (line == entry.name)
        {
            command.kind = Kind::button;
            command.frame = button(entry.action, entry.nav);
            return command;
        }
    }
    // "wait N": enqueue N empty frames so the UI can animate/settle.
    if (line.rfind("wait", 0) == 0)
    {
        int frames = 1;
        (void)std::sscanf(line.c_str() + 4, "%d", &frames);
        if (frames < 1)
            frames = 1;
        if (frames > 600)
            frames = 600;
        command.kind = Kind::button; // an empty (neutral) frame
        command.frame.connected = true;
        for (int i = 1; i < frames; ++i)
        {
            Command pad;
            pad.kind = Kind::button;
            pad.frame.connected = true;
            g_queue.push_back(pad);
        }
        return command;
    }
    return command; // unknown -> skipped
}

} // namespace

void poll()
{
    std::FILE *file = std::fopen(kScript, "rb");
    if (file == nullptr)
        return;
    std::string text;
    char buffer[8192];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, got);
    std::fclose(file);
    (void)::unlink(kScript); // consume it

    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        std::size_t lead = 0;
        while (lead < line.size() && line[lead] == ' ')
            ++lead;
        line = line.substr(lead);
        if (!line.empty())
            g_queue.push_back(parse_line(line));
        if (end == text.size())
            break;
        start = end + 1;
    }
}

Command next()
{
    while (!g_queue.empty())
    {
        Command command = g_queue.front();
        g_queue.pop_front();
        if (command.kind != Kind::none)
            return command;
    }
    return Command{};
}

} // namespace dev_input
