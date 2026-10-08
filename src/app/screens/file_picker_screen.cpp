// ps5fwdgen - A file browser used to pick a ROM (or any file) off /data.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ps5-homebrew-ui kit's files.cpp design is a demo over an invented tree,
// so this pairs real POSIX enumeration (reachable because the app elevated)
// with that design's presentation: a breadcrumb path and per-kind row icons
// drawn in the same style (see src/concepts/files.cpp draw_icon in the kit).

#include "app/screens/file_picker_screen.hpp"

#include "app/context.hpp"
#include "ui/components/breadcrumb.hpp"
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

enum class Kind
{
    folder,
    image,
    music,
    archive,
    rom,
    text,
    file,
};

constexpr hui::gfx::Rect kListBounds{96.0f, 360.0f, 1728.0f, 600.0f};

bool ends_with(const std::string &name, const char *suffix)
{
    const std::size_t n = std::char_traits<char>::length(suffix);
    if (name.size() < n)
        return false;
    for (std::size_t i = 0; i < n; ++i)
    {
        char c = name[name.size() - n + i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

Kind classify(const std::string &name)
{
    for (const char *s : {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".gif"})
        if (ends_with(name, s))
            return Kind::image;
    for (const char *s : {".at9", ".mp3", ".wav", ".ogg", ".flac", ".m4a"})
        if (ends_with(name, s))
            return Kind::music;
    for (const char *s : {".zip", ".7z", ".rar", ".gz"})
        if (ends_with(name, s))
            return Kind::archive;
    for (const char *s : {".nsp", ".xci", ".iso", ".pkg", ".nro", ".nca", ".rom", ".bin", ".chd",
                          ".cso", ".elf"})
        if (ends_with(name, s))
            return Kind::rom;
    for (const char *s : {".txt", ".json", ".ini", ".log", ".cfg", ".md"})
        if (ends_with(name, s))
            return Kind::text;
    return Kind::file;
}

hui::gfx::Color tint_of(Kind kind, const hui::ui::Theme &theme)
{
    switch (kind)
    {
    case Kind::folder:
        return theme.accent;
    case Kind::image:
        return theme.success;
    case Kind::music:
        return theme.primary;
    case Kind::archive:
        return theme.warning;
    case Kind::rom:
        return theme.accent;
    case Kind::text:
        return theme.text_muted;
    case Kind::file:
        return theme.text_muted;
    }
    return theme.text;
}

// A compact port of the kit's files.cpp draw_icon for the kinds we show.
void draw_file_icon(hui::gfx::DrawList &list, Kind kind, float cx, float cy, float s,
                    const hui::ui::Theme &theme)
{
    const hui::gfx::Color tint = tint_of(kind, theme);
    const hui::gfx::Color wash = tint.with_alpha(0.16f);
    const float h = s * 0.5f;
    const float stroke = std::max(1.5f, s * 0.065f);
    const hui::gfx::Rect page{cx - s * 0.36f, cy - h, s * 0.72f, s};
    switch (kind)
    {
    case Kind::folder:
    {
        const hui::gfx::Color back = hui::gfx::mix(tint, theme.text_muted, 0.38f);
        list.rounded_rect({cx - h, cy - s * 0.42f, s * 0.46f, s * 0.3f}, s * 0.09f, back);
        list.rounded_rect({cx - h, cy - s * 0.3f, s, s * 0.72f}, s * 0.1f, back);
        list.gradient_rect({cx - h, cy - s * 0.19f, s, s * 0.61f}, s * 0.1f, tint,
                           hui::gfx::mix(tint, theme.text_muted, 0.2f));
        break;
    }
    case Kind::image:
    {
        const hui::gfx::Rect frame{cx - h, cy - s * 0.4f, s, s * 0.8f};
        list.bordered_rect(frame, s * 0.1f, wash, stroke, tint);
        list.circle(cx + s * 0.2f, cy - s * 0.15f, s * 0.085f, tint);
        const float base = frame.y + frame.h - stroke - s * 0.05f;
        list.triangle({cx - s * 0.36f, base - s * 0.32f, s * 0.46f, s * 0.32f}, tint);
        list.triangle({cx - s * 0.02f, base - s * 0.2f, s * 0.36f, s * 0.2f},
                      tint.with_alpha(0.65f));
        break;
    }
    case Kind::music:
    {
        const float r = s * 0.13f;
        const float x1 = cx - s * 0.2f + r - stroke * 0.5f;
        const float x2 = cx + s * 0.24f + r - stroke * 0.5f;
        list.circle(cx - s * 0.2f, cy + s * 0.27f, r, tint);
        list.circle(cx + s * 0.24f, cy + s * 0.17f, r, tint);
        list.line(x1, cy + s * 0.27f, x1, cy - s * 0.28f, stroke, tint);
        list.line(x2, cy + s * 0.17f, x2, cy - s * 0.38f, stroke, tint);
        list.line(x1, cy - s * 0.28f, x2, cy - s * 0.38f, stroke * 1.9f, tint);
        break;
    }
    case Kind::archive:
    {
        list.bordered_rect(page, s * 0.1f, wash, stroke, tint);
        for (int i = 0; i < 5; ++i)
        {
            const float x = cx - s * 0.08f + static_cast<float>(i % 2) * s * 0.08f;
            list.rounded_rect({x, page.y + stroke + s * 0.05f + static_cast<float>(i) * s * 0.1f,
                               s * 0.08f, s * 0.08f},
                              1, tint);
        }
        break;
    }
    case Kind::rom:
    {
        // A cartridge: a rounded body with a label and two notches.
        list.gradient_rect({cx - h, cy - s * 0.42f, s, s * 0.84f}, s * 0.14f, tint,
                           hui::gfx::mix(tint, theme.text_muted, 0.25f));
        list.rounded_rect({cx - s * 0.28f, cy - s * 0.28f, s * 0.56f, s * 0.3f}, s * 0.05f,
                          theme.surface.with_alpha(0.85f));
        list.rounded_rect({cx - s * 0.18f, cy + s * 0.22f, s * 0.12f, s * 0.2f}, 1,
                          theme.page.with_alpha(0.6f));
        list.rounded_rect({cx + s * 0.06f, cy + s * 0.22f, s * 0.12f, s * 0.2f}, 1,
                          theme.page.with_alpha(0.6f));
        break;
    }
    case Kind::text:
    {
        list.bordered_rect(page, s * 0.1f, wash, stroke, tint);
        for (int i = 0; i < 4; ++i)
            list.rounded_rect({page.x + s * 0.12f, page.y + s * 0.2f + static_cast<float>(i) * s *
                                                                          0.16f,
                               s * 0.48f, stroke},
                              1, tint);
        break;
    }
    case Kind::file:
    {
        list.bordered_rect(page, s * 0.1f, wash, stroke, tint);
        list.triangle({cx + s * 0.08f, page.y, s * 0.28f, s * 0.28f},
                      hui::gfx::mix(tint, theme.page, 0.3f));
        break;
    }
    }
}

struct Entry
{
    std::string name;
    bool is_dir = false;
    Kind kind = Kind::file;
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

class FilePickerScreen final : public Screen
{
  public:
    FilePickerScreen(Context &context, std::string start_dir, std::vector<std::string> extensions,
                     std::function<void(const std::string &)> on_pick)
        : dir_(std::move(start_dir)), extensions_(std::move(extensions)),
          on_pick_(std::move(on_pick))
    {
        if (dir_.empty())
            dir_ = "/data";
        restyle(context);
        list_.set_bounds(kListBounds);
        list_.set_active(true);
        list_.style.leading_width = 64.0f;
        list_.leading = [this, &context](hui::ui::Canvas &canvas, const hui::gfx::Rect &row,
                                         const hui::ui::ListItem &item, int index, float focus)
        {
            (void)item;
            (void)focus;
            Kind kind = Kind::folder;
            const int base = has_parent_row() ? 1 : 0;
            if (index == 0 && has_parent_row())
                kind = Kind::folder;
            else if (index - base >= 0 && index - base < static_cast<int>(entries_.size()))
                kind = entries_[static_cast<std::size_t>(index - base)].kind;
            draw_file_icon(canvas.list, kind, row.x + 32.0f, row.y + row.h * 0.5f, 34.0f,
                           context.theme);
        };
        read_dir();
    }

    bool dev_inject_text(Context &context, const std::string &path) override
    {
        if (path.empty() || !on_pick_)
            return false;
        on_pick_(path);
        context.pop();
        return true;
    }

    void restyle(Context &context) override
    {
        list_.style.theme = context.theme;
        list_.style.dividers = true;
        crumbs_.style.theme = context.theme;
        crumbs_.style.chips = true;
    }

    void set_crumbs()
    {
        std::vector<std::string> parts;
        parts.push_back("/");
        std::string acc;
        for (std::size_t i = 1; i < dir_.size(); ++i)
        {
            if (dir_[i] == '/')
            {
                if (!acc.empty())
                    parts.push_back(acc);
                acc.clear();
            }
            else
            {
                acc.push_back(dir_[i]);
            }
        }
        if (!acc.empty())
            parts.push_back(acc);
        crumbs_.set_path(parts, true);
    }

    void read_dir()
    {
        set_crumbs();
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
                const std::string path = dir_ == "/" ? "/" + name : dir_ + "/" + name;
                struct stat st;
                if (::stat(path.c_str(), &st) != 0)
                    continue;
                Entry entry;
                entry.name = name;
                entry.is_dir = S_ISDIR(st.st_mode);
                entry.kind = entry.is_dir ? Kind::folder : classify(name);
                entry.size = static_cast<std::uint64_t>(st.st_size);
                if (!entry.is_dir && !extensions_.empty())
                {
                    bool matched = false;
                    for (const std::string &suffix : extensions_)
                        matched = matched || ends_with(name, suffix.c_str());
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
        if (has_parent_row())
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
                on_pick_(dir_ == "/" ? "/" + entry.name : dir_ + "/" + entry.name);
                feedback.play(hui::audio::Cue::select);
                context.pop();
            }
        }
        list_.update(dt);
        crumbs_.update(dt);
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
        crumbs_.set_bounds({96.0f, 250.0f, 1728.0f, 52.0f});
        crumbs_.draw(scene);
        if (entries_.empty())
            hui::ui::text(scene.list, context.fonts.regular, "(nothing to show here)", 96.0f,
                          440.0f, 28.0f, muted);
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
    mutable hui::ui::Breadcrumb crumbs_;
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
