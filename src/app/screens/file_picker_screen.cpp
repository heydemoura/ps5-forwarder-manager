// ps5fwdgen - A file browser used to pick a ROM (or any file) off /data.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ps5-homebrew-ui kit's files.cpp design is a demo over an invented tree,
// so this is a real browser: POSIX opendir/readdir/stat over the console
// filesystem (reachable because the app elevated), presented with the kit's
// ListView and Breadcrumb.

#include "app/screens/file_picker_screen.hpp"

#include "app/context.hpp"
#include "ui/components/list.hpp"
#include "ui/fonts.hpp"

#include <algorithm>
#include <cstdint>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

constexpr hui::gfx::Rect kListBounds{96.0f, 300.0f, 1728.0f, 660.0f};

struct Entry
{
    std::string name;
    bool is_dir = false;
    std::uint64_t size = 0;
};

std::string human_size(std::uint64_t bytes)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4)
    {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    (void)std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value,
                        units[unit]);
    return buffer;
}

bool lower_ends_with(const std::string &name, const std::string &suffix)
{
    if (name.size() < suffix.size())
        return false;
    for (std::size_t i = 0; i < suffix.size(); ++i)
    {
        char c = name[name.size() - suffix.size() + i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

class FilePickerScreen final : public Screen
{
  public:
    FilePickerScreen(Context &context, std::string start_dir,
                     std::vector<std::string> extensions,
                     std::function<void(const std::string &)> on_pick)
        : dir_(std::move(start_dir)), extensions_(std::move(extensions)),
          on_pick_(std::move(on_pick))
    {
        if (dir_.empty())
            dir_ = "/data";
        restyle(context);
        list_.set_bounds(kListBounds);
        list_.set_active(true);
        read_dir();
    }

    void restyle(Context &context) override
    {
        list_.style.theme = context.theme;
        list_.style.dividers = true;
    }

    void read_dir()
    {
        entries_.clear();
        DIR *dir = ::opendir(dir_.c_str());
        if (dir != nullptr)
        {
            struct dirent *de;
            while ((de = ::readdir(dir)) != nullptr)
            {
                const std::string name = de->d_name;
                if (name == "." || name == "..")
                    continue;
                const std::string path = dir_ + "/" + name;
                struct stat st;
                if (::stat(path.c_str(), &st) != 0)
                    continue;
                Entry entry;
                entry.name = name;
                entry.is_dir = S_ISDIR(st.st_mode);
                entry.size = static_cast<std::uint64_t>(st.st_size);
                if (!entry.is_dir && !extensions_.empty())
                {
                    bool matched = false;
                    for (const std::string &suffix : extensions_)
                        matched = matched || lower_ends_with(name, suffix);
                    if (!matched)
                        continue;
                }
                entries_.push_back(std::move(entry));
            }
            ::closedir(dir);
        }
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry &a, const Entry &b)
                  {
                      if (a.is_dir != b.is_dir)
                          return a.is_dir;
                      return a.name < b.name;
                  });

        std::vector<hui::ui::ListItem> items;
        if (dir_ != "/")
        {
            hui::ui::ListItem up;
            up.title = "..";
            up.subtitle = "parent folder";
            up.chevron = true;
            items.push_back(std::move(up));
        }
        for (const Entry &entry : entries_)
        {
            hui::ui::ListItem item;
            item.title = entry.name;
            item.chevron = entry.is_dir;
            item.value = entry.is_dir ? "folder" : human_size(entry.size);
            items.push_back(std::move(item));
        }
        list_.set_items(std::move(items));
        list_.set_focus(0, true);
    }

    bool has_parent_row() const
    {
        return dir_ != "/";
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        if (input.is_pressed(hui::Action::back))
        {
            context.pop();
            return;
        }
        const hui::ui::Event event = list_.handle(input, feedback);
        if (event == hui::ui::Event::activated)
        {
            int index = list_.focus();
            if (has_parent_row())
            {
                if (index == 0)
                {
                    go_up();
                    return;
                }
                --index;
            }
            if (index < 0 || index >= static_cast<int>(entries_.size()))
                return;
            const Entry &entry = entries_[static_cast<std::size_t>(index)];
            if (entry.is_dir)
            {
                dir_ = dir_ == "/" ? "/" + entry.name : dir_ + "/" + entry.name;
                read_dir();
            }
            else if (on_pick_)
            {
                on_pick_(dir_ + "/" + entry.name);
                feedback.play(hui::audio::Cue::select);
                context.pop();
            }
        }
        list_.update(dt);
    }

    void go_up()
    {
        const std::size_t slash = dir_.find_last_of('/');
        if (slash == 0)
            dir_ = "/";
        else if (slash != std::string::npos)
            dir_ = dir_.substr(0, slash);
        read_dir();
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        (void)overlay;
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        const hui::gfx::Color muted =
            theme.page_text_muted.a > 0.0f ? theme.page_text_muted : theme.text_muted;
        hui::ui::text(scene.list, context.fonts.display, "Pick a file", 96.0f, 150.0f, 46.0f, text);
        hui::ui::text(scene.list, context.fonts.mono, dir_, 96.0f, 220.0f, 24.0f, muted);
        if (entries_.empty())
            hui::ui::text(scene.list, context.fonts.regular, "(empty)", 96.0f, 400.0f, 28.0f,
                          muted);
        list_.draw(scene);
        return false;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {
            {hui::ui::Button::cross, "Open / Pick"},
            {hui::ui::Button::circle, "Back"},
        };
        return kHints;
    }

  private:
    std::string dir_;
    std::vector<std::string> extensions_;
    std::function<void(const std::string &)> on_pick_;
    std::vector<Entry> entries_;
    mutable hui::ui::ListView list_;
};

} // namespace

std::unique_ptr<Screen> make_file_picker_screen(Context &context, std::string start_dir,
                                                std::vector<std::string> extensions,
                                                std::function<void(const std::string &)> on_pick)
{
    return std::make_unique<FilePickerScreen>(context, std::move(start_dir), std::move(extensions),
                                              std::move(on_pick));
}

} // namespace fwd
