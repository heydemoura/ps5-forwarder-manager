// ps5fwdgen - Create or edit one forwarder, laid out as the Aurora Shelf
// concept (ps5-homebrew-ui src/concepts/aurora.cpp): the forwarder is the
// hero (floating artwork, display title, meta line, a completeness bar) over
// its own background, with the form on a frosted details sheet below, and
// its selection music playing on a mixer deck.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/keyboard_prompt.hpp"
#include "app/screens/edit_screen.hpp"

#include "app/context.hpp"
#include "app/preview_music.hpp"
#include "app/screens/app_picker_screen.hpp"
#include "app/screens/art_screen.hpp"
#include "app/screens/file_picker_screen.hpp"
#include "app/tile.hpp"
#include "audio/at9.hpp"
#include "core/tween.hpp"
#include "fwd/image.hpp"
#include "fwd/eden_library.hpp"
#include "fwd/library_game.hpp"
#include "fwd/psx_library.hpp"
#include "fwd/rom_name.hpp"
#include "fwd/store.hpp"
#include "fwd/stream_catalog.hpp"
#include "fwd/x360_library.hpp"
#include "gfx/gl_batch.hpp"
#include "gfx/renderer.hpp"
#include "net/at9_convert.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/form.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/toast.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <GL/glcorearb.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

using hui::gfx::Color;
using hui::gfx::Rect;

extern "C" void glDeleteTextures(int n, const unsigned int *textures);

bool read_whole_file(const std::string &path, std::vector<unsigned char> &out)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    out.clear();
    unsigned char buffer[65536];
    std::size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.insert(out.end(), buffer, buffer + got);
    std::fclose(file);
    return !out.empty();
}

enum Row
{
    RowName = 1,
    RowTarget,
    RowTargetId,
    RowTitleId,
    RowRom,
    RowPc,
    RowApp,
    RowExit,
    RowIcon,
    RowBackground,
    RowMusic,
    RowGenerate,
    RowDelete,
};

enum class Prompting
{
    none,
    name,
    target_id,
    title_id,
    pc,
    app,
};

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);
constexpr float kMargin = 96.0f;
// The concept's details sheet, 120 in from both sides at the bottom of the
// screen; a little shorter than the concept's 500 so the app's hint bar
// below it stays visible.
constexpr float kSheetHeight = 420.0f;
constexpr Rect kSheet{120.0f, hui::gfx::kVirtualHeight - kSheetHeight - 96.0f, 1680.0f,
                      kSheetHeight};
constexpr Rect kFormBounds{kSheet.x + 40.0f, kSheet.y + 24.0f, 840.0f, kSheetHeight - 48.0f};
constexpr float kTileW = 216.0f;
constexpr float kTileH = 120.0f;
constexpr float kTilePitch = 236.0f;

class EditScreen final : public Screen
{
  public:
    EditScreen(Context &context, Forwarder initial, bool editing)
        : context_(&context), forwarder_(std::move(initial)), editing_(editing)
    {
        if (!editing_ && forwarder_.title_id.empty())
            forwarder_.title_id = new_title_id(context.settings.forwarders_root);
        if (forwarder_.target.empty())
            forwarder_.target = "PPSA99008";
        target_index_ = index_for_target(forwarder_.target);
        sync_stream();
        restyle(context);
        form_.set_bounds(kFormBounds);
        form_.set_active(true);
        build(context);
        bind_keyboard_prompt(prompt_); // R2 confirms
        prompt_.style.buttons = false; // single Done: the keyboard's own key (closes on press)
    }

    ~EditScreen() override
    {
        join_convert();
        if (context_->music != nullptr)
            context_->music->stop();
        release_textures();
    }

    void restyle(Context &context) override
    {
        form_.style.theme = context.theme;
        form_.style.panel = false; // the frosted sheet is the panel
        form_.style.on_page = true;
        form_.style.dividers = false;
        dialog_.style.theme = context.theme;
        toasts_.style.theme = context.theme;
        style_keyboard_prompt(prompt_, context.theme);
    }

    static int index_for_target(const std::string &id)
    {
        const std::vector<Target> targets = known_targets();
        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            if (id == targets[i].title_id && targets[i].title_id[0] != '\0')
                return static_cast<int>(i);
        }
        return static_cast<int>(targets.size()) - 1; // Generic
    }

    bool target_is_generic() const
    {
        const std::vector<Target> targets = known_targets();
        return target_index_ == static_cast<int>(targets.size()) - 1;
    }

    const Target &current_target() const
    {
        static const std::vector<Target> targets = known_targets();
        return targets[static_cast<std::size_t>(target_index_)];
    }

    // Reads what the streaming client published and lines the PC choice up with
    // the PC this forwarder names. A new forwarder takes the first PC.
    void sync_stream()
    {
        catalog_.clear();
        pc_index_ = 0;
        pc_custom_ = false;
        if (!is_stream_target(forwarder_.target))
            return;
        catalog_ = read_stream_catalog();
        if (catalog_.empty())
            return;
        const int found = find_stream_pc(catalog_, forwarder_.pc);
        if (found >= 0)
            pc_index_ = found;
        else if (forwarder_.pc.empty() && !editing_)
            forwarder_.pc = stream_pc_argument(catalog_, catalog_[0]);
        else
        {
            // Typed earlier, or left to the last PC used: kept as one more choice.
            pc_custom_ = true;
            pc_custom_value_ = forwarder_.pc;
            pc_index_ = static_cast<int>(catalog_.size());
        }
    }

    // The PC chosen from the published list; nullptr when it is not one of them.
    const StreamPc *selected_pc() const
    {
        return pc_index_ >= 0 && pc_index_ < static_cast<int>(catalog_.size())
                   ? &catalog_[static_cast<std::size_t>(pc_index_)]
                   : nullptr;
    }

    std::vector<std::string> pc_options() const
    {
        std::vector<std::string> options;
        for (const StreamPc &pc : catalog_)
            options.push_back(stream_pc_argument(catalog_, pc) == pc.name
                                  ? pc.name
                                  : pc.name + " (" + pc.address + ")");
        if (pc_custom_)
            options.push_back(pc_custom_value_.empty() ? "The last one used" : pc_custom_value_);
        return options;
    }

    // The app picked from the list: its name, and its box art as the tile icon
    // unless the player chose an icon of their own.
    void pick_app(const StreamApp &app)
    {
        forwarder_.app = app.name;
        if (forwarder_.display_name.empty())
            forwarder_.display_name = app.name;
        if (!app.poster.empty() && (!has_icon() || icon_from_poster_))
        {
            const std::vector<unsigned char> art = read_stream_file(app.poster);
            std::vector<unsigned char> icon =
                art.empty() ? art : make_poster_icon_png(art.data(), art.size());
            if (!icon.empty())
            {
                assets_.icon_png = std::move(icon);
                icon_dirty_ = true;
                icon_from_poster_ = true;
            }
        }
        dirty_ = true;
        build(*context_);
    }

    // The game picked from the emulator's library: its path, its name, and its
    // cover as the tile icon unless the player chose an icon of their own.
    void pick_game(const LibraryGame &game)
    {
        forwarder_.rom = game.path;
        if (forwarder_.display_name.empty())
            forwarder_.display_name = game.name;
        if (!has_icon() || icon_from_poster_)
        {
            const std::vector<unsigned char> art = game_art(game);
            std::vector<unsigned char> icon =
                art.empty() ? art : make_poster_icon_png(art.data(), art.size());
            if (!icon.empty())
            {
                assets_.icon_png = std::move(icon);
                icon_dirty_ = true;
                icon_from_poster_ = true;
            }
        }
        dirty_ = true;
        build(*context_);
    }

    static std::vector<unsigned char> game_art(const LibraryGame &game)
    {
        if (!game.cover.empty())
            return read_stream_file(game.cover);
        return game.kind == "GOD/STFS" ? x360_package_thumbnail(game.location)
                                       : std::vector<unsigned char>();
    }

    // The emulator's games, where it looks for them (fwd/x360_library.hpp,
    // fwd/psx_library.hpp).
    void open_game_list(Context &context)
    {
        const bool x360 = current_target().library == GameLibrary::x360;
        const bool eden = current_target().library == GameLibrary::eden;
        if (x360)
        {
            x360_install_ = find_x360_install(context.settings.forwarders_root);
            games_ = read_x360_library(x360_install_, x360_game_paths(x360_install_));
        }
        else if (eden)
            games_ = read_eden_library();
        else
            games_ = read_psx_library(psx_game_paths(), std::string(kPsxDataRoot) + "/covers");
        PickList list;
        list.heading = "Pick a game";
        list.caption = std::to_string(games_.size()) + (games_.size() == 1 ? " game" : " games") +
                       " where " + target_display_name(forwarder_.target) + " looks for them";
        if (x360 && x360_install_.empty())
            list.caption += " (not found in " + context.settings.forwarders_root + ")";
        list.fallback_title = eden ? "Choose a ROM file" : "Choose a file or folder";
        list.fallback_caption = eden ? "Browse for a ROM outside this list"
                                     : "For a game in a folder added in the emulator's settings";
        list.no_art = "no cover";
        for (const LibraryGame &game : games_)
        {
            PickItem item;
            item.title = game.name;
            item.value = game.kind;
            if (eden)
                item.subtitle = game.path;
            item.art = [game]() { return game_art(game); };
            list.items.push_back(std::move(item));
        }
        EditScreen *self = this;
        context.push(make_list_picker_screen(
            context, std::move(list),
            [self](int index)
            {
                if (index >= 0)
                    self->pick_game(self->games_[static_cast<std::size_t>(index)]);
                else
                    self->choose_game_file_ = true;
            }));
    }

    // A game file, or for PS5X360 a folder with its .xex, given by full path
    // (as PS5X360 sees it).
    void open_game_file(Context &context)
    {
        EditScreen *self = this;
        if (current_target().library == GameLibrary::eden)
        {
            const std::string files = eden_game_files();
            const std::string roms = files + (files == "/" ? "roms" : "/roms");
            context.push(make_file_picker_screen(context, roms, {".nsp", ".xci", ".nsz", ".xcz"},
                                                 [self](const std::string &path)
                                                 { self->pick_game(eden_game_from_file(path)); }));
            return;
        }
        if (current_target().library == GameLibrary::psx)
        {
            context.push(make_file_picker_screen(context, std::string(kPsxDataRoot) + "/games", {},
                                                 [self](const std::string &path)
                                                 {
                                                     self->forwarder_.rom = path;
                                                     if (self->forwarder_.display_name.empty())
                                                         self->forwarder_.display_name =
                                                             library_game_name(
                                                                 self->forwarder_.target, path);
                                                     self->dirty_ = true;
                                                 }));
            return;
        }
        context.push(make_file_picker_screen(
            context, "/data", {},
            [self](const std::string &path)
            {
                std::string file = path;
                if (!path.empty() && path.back() == '/') // a game kept as a folder
                {
                    file = x360_folder_executable(path);
                    if (file.empty())
                    {
                        self->toasts_.push(hui::ui::StatusKind::warning, "No game in that folder",
                                           "It needs default.xex, or a single .xex.");
                        return;
                    }
                }
                self->forwarder_.rom = x360_path_for(self->x360_install_, file);
                if (self->forwarder_.display_name.empty())
                    self->forwarder_.display_name = name_from_rom(path);
                self->dirty_ = true;
            },
            true));
    }

    bool has_icon() const
    {
        return forwarder_.has_icon || !assets_.icon_png.empty();
    }
    bool has_background() const
    {
        return forwarder_.has_backgrounds || !assets_.pic0_dds.empty();
    }
    bool has_music() const
    {
        return forwarder_.has_music || !assets_.music_at9.empty();
    }

    void build(Context &context)
    {
        (void)context;
        const int keep = form_.focus_id();
        form_.clear();
        const std::vector<Target> targets = known_targets();
        std::vector<std::string> target_names;
        for (const Target &target : targets)
            target_names.push_back(target.name);

        form_.add_header(editing_ ? "Edit forwarder" : "New forwarder");
        // The ROM comes first: picking it also names an unnamed forwarder. A
        // streaming target has none; its PC and app follow under Launch.
        if (current_target().library != GameLibrary::none)
            form_.add_action(RowRom, "Game").text =
                forwarder_.rom.empty() ? "(none)"
                                       : library_game_name(forwarder_.target, forwarder_.rom);
        else if (!current_target().stream)
            form_.add_action(RowRom, "ROM file").text =
                forwarder_.rom.empty() ? "(none)" : forwarder_.rom;
        form_.add_action(RowName, "Display name").text =
            forwarder_.display_name.empty() ? "(required)" : forwarder_.display_name;
        form_.add_choice(RowTarget, "Target app", target_names, target_index_);
        if (target_is_generic())
            form_.add_action(RowTargetId, "Target title ID").text =
                forwarder_.target.empty() ? "(enter one)" : forwarder_.target;
        form_.add_action(RowTitleId, "Forwarder title ID").text = forwarder_.title_id;

        const bool stream = current_target().stream;
        if (stream || current_target().exit_after_game)
            form_.add_header("Launch");
        if (stream)
        {
            // A streaming client starts an app on a PC, not a ROM.
            // With what the client published, both are picked; without it, typed.
            if (!catalog_.empty())
                form_.add_choice(RowPc, "PC", pc_options(), pc_index_);
            else
                form_.add_action(RowPc, "PC (name or address)").text =
                    forwarder_.pc.empty() ? "(the last one used)" : forwarder_.pc;
            hui::ui::FormRow &app = form_.add_action(RowApp, "App on that PC");
            app.text = forwarder_.app.empty() ? "(required)" : forwarder_.app;
            app.chevron = selected_pc() != nullptr && !selected_pc()->apps.empty();
        }
        if (current_target().exit_after_game)
            form_.add_toggle(RowExit, stream ? "Exit after the stream" : "Exit after quitting game",
                             forwarder_.exit_after_game);

        form_.add_header("Presentation");
        form_.add_action(RowIcon, "Tile icon").text = has_icon() ? "set" : "(required)";
        form_.add_action(RowBackground, "Backgrounds").text =
            has_background() ? "set" : "(optional)";
        form_.add_action(RowMusic, "Selection music").text = has_music() ? "set" : "(optional)";

        form_.add_header("");
        form_.add_action(RowGenerate, editing_ ? "Save forwarder" : "Generate forwarder").chevron =
            false;
        if (editing_)
        {
            hui::ui::FormRow &del = form_.add_action(RowDelete, "Delete forwarder");
            del.danger = true;
        }
        if (keep != 0)
            form_.focus_row(keep, true);
    }

    void save(Context &context, hui::ui::Feedback &feedback)
    {
        forwarder_.exit_after_game = current_target().exit_after_game && forwarder_.exit_after_game;
        if (forwarder_.display_name.empty())
        {
            toasts_.push(hui::ui::StatusKind::warning, "A display name is required");
            return;
        }
        if (forwarder_.target.empty())
        {
            toasts_.push(hui::ui::StatusKind::warning, "A target title ID is required");
            return;
        }
        if (is_stream_target(forwarder_.target) && forwarder_.app.empty())
        {
            toasts_.push(hui::ui::StatusKind::warning, "The app to start is required",
                         "Its name as ProsperoLight's Games page shows it.");
            return;
        }
        if (!has_icon())
        {
            toasts_.push(hui::ui::StatusKind::warning, "A tile icon is required");
            return;
        }
        const WriteResult result = write_forwarder(context.settings.forwarders_root,
                                                   context.app_template_root, forwarder_, assets_);
        if (result.ok)
        {
            hui::sys::log("[FWD] ui-generate title=%s name=\"%s\" ok=1",
                          forwarder_.title_id.c_str(), forwarder_.display_name.c_str());
            feedback.play(hui::audio::Cue::complete);
            context.pop(); // back to home, which rescans on enter
        }
        else
        {
            toasts_.push(hui::ui::StatusKind::danger, "Could not write forwarder", result.error);
        }
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        poll_convert(context);
        refresh_preview(context);
        if (converting_)
        {
            toasts_.update(dt, feedback);
            return;
        }
        if (prompt_.is_open())
        {
            const hui::ui::Event event = prompt_.handle(input, feedback);
            if (event == hui::ui::Event::activated)
                apply_prompt(context, prompt_.text());
            if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
                prompting_ = Prompting::none;
            prompt_.update(dt);
            return;
        }
        // Keep animating the prompt's fade-out after it closes, so the
        // keyboard actually disappears (visible() depends on the fade).
        prompt_.update(dt);
        if (dialog_.is_open())
        {
            const hui::ui::Event event = dialog_.handle(input, feedback);
            if (event == hui::ui::Event::activated && dialog_.choice() == 1)
            {
                remove_forwarder(context.settings.forwarders_root, forwarder_.title_id);
                feedback.play(hui::audio::Cue::complete);
                context.pop();
                return;
            }
            dialog_.update(dt);
            toasts_.update(dt, feedback);
            return;
        }

        if (choose_game_file_) // asked for from the game list
        {
            choose_game_file_ = false;
            open_game_file(context);
            return;
        }
        if (type_app_) // asked for from the app list
        {
            type_app_ = false;
            open_prompt(Prompting::app, "App name, as ProsperoLight shows it", forwarder_.app,
                        feedback);
            return;
        }
        if (input.is_pressed(hui::Action::back))
        {
            context.pop();
            return;
        }
        if (input.is_pressed(hui::Action::north)) // Triangle: save from anywhere in the form
        {
            save(context, feedback);
            return;
        }

        const hui::ui::Event event = form_.handle(input, feedback);
        if (event == hui::ui::Event::changed && form_.changed_id() == RowTarget)
        {
            target_index_ = form_.choice_index(RowTarget);
            if (!target_is_generic())
                forwarder_.target = current_target().title_id;
            else if (forwarder_.target == "PPSA99008" || forwarder_.target == "PPSA99764" ||
                     forwarder_.target == "PPSA99203")
                forwarder_.target.clear();
            sync_stream();
            build(context);
        }
        else if (event == hui::ui::Event::changed && form_.changed_id() == RowPc)
        {
            pc_index_ = form_.choice_index(RowPc);
            const StreamPc *pc = selected_pc();
            forwarder_.pc = pc != nullptr ? stream_pc_argument(catalog_, *pc) : pc_custom_value_;
            // An app of the PC left behind means nothing on this one.
            bool listed = pc == nullptr;
            if (pc != nullptr)
                for (const StreamApp &app : pc->apps)
                    listed = listed || app.name == forwarder_.app;
            if (!listed)
                forwarder_.app.clear();
            dirty_ = true;
            build(context);
        }
        else if (event == hui::ui::Event::changed && form_.changed_id() == RowExit)
        {
            forwarder_.exit_after_game = form_.toggle_value(RowExit);
        }
        else if (event == hui::ui::Event::activated)
        {
            on_action(context, form_.focus_id(), feedback);
        }
        form_.update(dt);
        toasts_.update(dt, feedback);
    }

    void on_action(Context &context, int id, hui::ui::Feedback &feedback)
    {
        switch (id)
        {
        case RowName:
            open_prompt(Prompting::name, "Display name", forwarder_.display_name, feedback);
            break;
        case RowTargetId:
            open_prompt(Prompting::target_id, "Target title ID (PPSA99008)", forwarder_.target,
                        feedback);
            break;
        case RowTitleId:
            open_prompt(Prompting::title_id, "Forwarder title ID", forwarder_.title_id, feedback);
            break;
        case RowPc:
            open_prompt(Prompting::pc, "PC name or address (empty: the last one used)",
                        forwarder_.pc, feedback);
            break;
        case RowApp:
            if (const StreamPc *pc = selected_pc(); pc != nullptr && !pc->apps.empty())
            {
                EditScreen *self = this;
                context.push(make_app_picker_screen(context, *pc,
                                                    [self](const StreamApp *app)
                                                    {
                                                        if (app != nullptr)
                                                            self->pick_app(*app);
                                                        else
                                                            self->type_app_ = true;
                                                    }));
                break;
            }
            open_prompt(Prompting::app, "App name, as ProsperoLight shows it", forwarder_.app,
                        feedback);
            break;
        case RowRom:
        {
            if (current_target().library != GameLibrary::none)
            {
                open_game_list(context);
                break;
            }
            EditScreen *self = this;
            context.push(make_file_picker_screen(
                context, "/data", {},
                [self](const std::string &path)
                {
                    self->forwarder_.rom = rom_path_for(self->forwarder_.target, path);
                    if (self->forwarder_.display_name.empty())
                        self->forwarder_.display_name = name_from_rom(self->forwarder_.rom);
                    self->dirty_ = true;
                }));
            break;
        }
        case RowIcon:
        {
            EditScreen *self = this;
            context.push(make_art_screen(context, ArtKind::icon, forwarder_.display_name,
                                         [self](std::vector<unsigned char> bytes)
                                         {
                                             self->assets_.icon_png = std::move(bytes);
                                             self->icon_from_poster_ = false;
                                             self->dirty_ = true;
                                             self->icon_dirty_ = true;
                                         }));
            break;
        }
        case RowBackground:
        {
            EditScreen *self = this;
            context.push(make_art_screen(context, ArtKind::background, forwarder_.display_name,
                                         [self](std::vector<unsigned char> bytes)
                                         {
                                             self->assets_.pic0_dds = bytes;
                                             self->assets_.pic1_dds = std::move(bytes);
                                             self->dirty_ = true;
                                             self->background_dirty_ = true;
                                         }));
            break;
        }
        case RowMusic:
        {
            EditScreen *self = this;
            context.push(make_music_source_screen(
                context, forwarder_.display_name,
                [self](const std::string &path)
                {
                    std::vector<unsigned char> bytes;
                    if (!read_whole_file(path, bytes))
                        return;
                    const bool is_at9 =
                        path.size() > 4 && path.compare(path.size() - 4, 4, ".at9") == 0;
                    if (is_at9)
                    {
                        self->assets_.music_at9 = std::move(bytes);
                        self->dirty_ = true;
                        self->music_dirty_ = true;
                    }
                    else
                    {
                        // Non-AT9 audio is converted online, as the site does.
                        const std::size_t slash = path.find_last_of('/');
                        self->start_convert(std::move(bytes), slash == std::string::npos
                                                                  ? path
                                                                  : path.substr(slash + 1));
                    }
                },
                [self](std::vector<unsigned char> at9)
                {
                    // Theme music found online arrives already converted.
                    self->assets_.music_at9 = std::move(at9);
                    self->dirty_ = true;
                    self->music_dirty_ = true;
                }));
            break;
        }
        case RowGenerate:
            save(context, feedback);
            break;
        case RowDelete:
            dialog_.open({hui::ui::StatusKind::danger,
                          "Delete this forwarder?",
                          forwarder_.display_name + "\n" + forwarder_.title_id,
                          {{"Cancel", hui::ui::ButtonKind::secondary, false},
                           {"Delete", hui::ui::ButtonKind::primary, true}},
                          0},
                         feedback);
            break;
        default:
            break;
        }
    }

    // ---- online AT9 conversion (worker thread) ----------------------------
    void start_convert(std::vector<unsigned char> audio, std::string name)
    {
        join_convert();
        {
            std::lock_guard<std::mutex> lock(convert_mutex_);
            convert_in_ = std::move(audio);
            convert_name_ = std::move(name);
            convert_out_.clear();
            convert_error_.clear();
        }
        convert_done_.store(false);
        converting_ = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);
        if (pthread_create(&convert_thread_, &attr, &EditScreen::convert_entry, this) == 0)
            convert_running_ = true;
        else
            convert_done_.store(true);
        pthread_attr_destroy(&attr);
    }

    static void *convert_entry(void *arg)
    {
        auto *self = static_cast<EditScreen *>(arg);
        std::vector<unsigned char> audio;
        std::string name;
        {
            std::lock_guard<std::mutex> lock(self->convert_mutex_);
            audio = self->convert_in_;
            name = self->convert_name_;
        }
        std::vector<unsigned char> at9;
        std::string error;
        const bool ok = net::convert_to_at9(audio, name, at9, error);
        std::lock_guard<std::mutex> lock(self->convert_mutex_);
        if (ok)
            self->convert_out_ = std::move(at9);
        else
            self->convert_error_ = error;
        self->convert_done_.store(true);
        return nullptr;
    }

    void join_convert()
    {
        if (convert_running_)
        {
            pthread_join(convert_thread_, nullptr);
            convert_running_ = false;
        }
    }

    void poll_convert(Context &context)
    {
        if (!converting_ || !convert_done_.load())
            return;
        join_convert();
        converting_ = false;
        std::lock_guard<std::mutex> lock(convert_mutex_);
        if (!convert_out_.empty())
        {
            // The forwarder format holds at most ~87 s of selection music.
            assets_.music_at9 = at9::trim(convert_out_, at9::kMaxMusicSeconds);
            convert_out_.clear();
            dirty_ = true;
            music_dirty_ = true;
            toasts_.push(hui::ui::StatusKind::success, "Audio converted to ATRAC9");
            build(context);
        }
        else
        {
            toasts_.push(hui::ui::StatusKind::danger, "Could not convert audio",
                         convert_error_.empty() ? "Use a pre-made .at9 file." : convert_error_);
        }
    }

    bool dev_inject_text(Context &context, const std::string &text) override
    {
        if (!prompt_.is_open())
            return false;
        apply_prompt(context, text);
        prompting_ = Prompting::none;
        prompt_.dismiss();
        return true;
    }

    void open_prompt(Prompting kind, const std::string &title, const std::string &initial,
                     hui::ui::Feedback &feedback)
    {
        prompting_ = kind;
        prompt_.set_title(title);
        prompt_.style.max_length =
            kind == Prompting::name || kind == Prompting::pc || kind == Prompting::app ? 60 : 9;
        prompt_.open(feedback, initial);
    }

    void apply_prompt(Context &context, const std::string &value)
    {
        switch (prompting_)
        {
        case Prompting::name:
            forwarder_.display_name = value;
            break;
        case Prompting::target_id:
            forwarder_.target = value;
            break;
        case Prompting::pc:
            forwarder_.pc = value;
            break;
        case Prompting::app:
            forwarder_.app = value;
            break;
        case Prompting::title_id:
            if (valid_title_id(value))
                forwarder_.title_id = value;
            else
                toasts_.push(hui::ui::StatusKind::warning, "Title ID must be PPSA + 5 digits");
            break;
        case Prompting::none:
            break;
        }
        build(context);
    }

    // ---- live preview: icon, background and music ---------------------------

    std::string forwarder_dir(const Context &context) const
    {
        return context.settings.forwarders_root + "/" + forwarder_.title_id;
    }

    void enter(Context &context) override
    {
        if (dirty_)
        {
            build(context);
            dirty_ = false;
        }
        // An existing forwarder's own files, the first time in.
        if (editing_ && !existing_loaded_)
        {
            existing_loaded_ = true;
            const std::string dir = forwarder_dir(context);
            if (forwarder_.has_icon && assets_.icon_png.empty())
                icon_tex_ = load_icon_texture(context.renderer, dir);
            if (forwarder_.has_backgrounds && assets_.pic0_dds.empty())
            {
                std::vector<unsigned char> dds;
                if (read_whole_file(dir + "/sce_sys/pic0.dds", dds))
                    set_background_texture(context, dds);
            }
            if (forwarder_.has_music && assets_.music_at9.empty())
            {
                std::vector<unsigned char> at9;
                if (read_whole_file(dir + "/sce_sys/snd0.at9", at9))
                    play_music(context, at9);
            }
        }
        refresh_preview(context);
    }

    void refresh_preview(Context &context)
    {
        if (icon_dirty_)
        {
            icon_dirty_ = false;
            if (icon_tex_ != 0)
            {
                glDeleteTextures(1, &icon_tex_);
                icon_tex_ = 0;
            }
            if (!assets_.icon_png.empty())
                icon_tex_ = upload_image_texture(context.renderer, assets_.icon_png.data(),
                                                 assets_.icon_png.size());
        }
        if (background_dirty_)
        {
            background_dirty_ = false;
            set_background_texture(context, assets_.pic0_dds);
        }
        if (music_dirty_)
        {
            music_dirty_ = false;
            play_music(context, assets_.music_at9);
        }
    }

    void set_background_texture(Context &context, const std::vector<unsigned char> &dds)
    {
        if (background_tex_ != 0)
        {
            glDeleteTextures(1, &background_tex_);
            background_tex_ = 0;
        }
        int w = 0;
        int h = 0;
        std::vector<unsigned char> rgba;
        const std::int64_t started = hui::sys::monotonic_us();
        const bool ok = decode_bc7_dds(dds.data(), dds.size(), w, h, rgba, 1920);
        if (ok && w > 0 && h > 0)
            background_tex_ = context.renderer.batch().create_texture(w, h, rgba.data());
        hui::sys::log("[FWD] preview background ok=%d %dx%d in %lld ms", ok ? 1 : 0, w, h,
                      static_cast<long long>((hui::sys::monotonic_us() - started) / 1000));
    }

    void play_music(Context &context, const std::vector<unsigned char> &at9)
    {
        if (context.music == nullptr)
            return;
        const std::int64_t started = hui::sys::monotonic_us();
        const at9::Decoded clip = at9::decode(at9.data(), at9.size());
        const bool playing = clip.ok() && context.music->play(clip);
        hui::sys::log("[FWD] preview music ok=%d frames=%zu rate=%d ch=%d decode_ms=%lld err=%s",
                      playing ? 1 : 0, clip.frames, clip.sample_rate, clip.channels,
                      static_cast<long long>((hui::sys::monotonic_us() - started) / 1000),
                      clip.error.c_str());
        if (!clip.ok())
            toasts_.push(hui::ui::StatusKind::warning, "Could not play the music", clip.error);
    }

    void release_textures()
    {
        if (icon_tex_ != 0)
            glDeleteTextures(1, &icon_tex_);
        if (background_tex_ != 0)
            glDeleteTextures(1, &background_tex_);
        icon_tex_ = 0;
        background_tex_ = 0;
    }

    // ---- drawing --------------------------------------------------------------

    float stagger(int index, float step, float duration) const
    {
        return hui::tween::stagger(age_, index, step, duration);
    }

    Color accent() const
    {
        return accent_for(forwarder_.title_id.empty() ? std::string("__new__")
                                                      : forwarder_.title_id);
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        draw_background(scene.list);
        draw_hero(context, scene.list);
        // The sheet sits in the overlay so Frame::glass can blur the hero and
        // background behind it, as the concept's details sheet does.
        draw_sheet(context, overlay);
        form_.draw(overlay);
        dialog_.draw(overlay);
        prompt_.draw(overlay);
        toasts_.draw(overlay);
        return true;
    }

    // The forwarder's own background under everything, darkened toward the
    // bottom so the hero text and the sheet read; without one the aurora
    // backdrop shows through as on the home screen.
    void draw_background(hui::gfx::DrawList &list) const
    {
        const Rect screen{0.0f, 0.0f, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight};
        if (background_tex_ != 0)
        {
            const float in = stagger(0, 0.0f, 0.8f);
            list.image(background_tex_, screen, hui::gfx::kFullUv, kWhite.with_alpha(in), 0.0f);
        }
        list.gradient_rect(screen, 0.0f, kInk.with_alpha(background_tex_ != 0 ? 0.25f : 0.0f),
                           kInk.with_alpha(0.9f));
    }

    // The concept's hero block: artwork floating at the right with a glow in
    // its accent, a tracked label, the display title, a meta line, a blurb,
    // and a progress bar that here shows how complete the forwarder is.
    void draw_hero(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = stagger(1, 0.08f, 0.6f);
        const float slide = 30.0f * (1.0f - in);
        const Color tint = accent();
        char text[160];
        list.push_opacity(in);

        const float bob = context.settings.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 6.0f;
        // About a fifth smaller than the concept's 440 so it clears the sheet
        // below; it keeps the concept's right edge.
        const Rect art{1404.0f + slide * 1.6f, 132.0f + bob, 352.0f, 352.0f};
        list.glow(art.inset(30.0f), 60.0f, 90.0f, tint.with_alpha(0.3f));
        list.shadow({art.x, art.y + 26.0f, art.w, art.h}, 36.0f, 46.0f,
                    Color::rgb(0x000000, 0.55f));
        if (icon_tex_ != 0)
        {
            list.image(icon_tex_, art, hui::gfx::kFullUv, kWhite, 36.0f);
        }
        else
        {
            list.gradient_rect(art, 36.0f, hui::gfx::mix(tint, kInk, 0.4f), kInk);
            hui::ui::text(list, fonts.regular, "no icon yet", art.cx(), art.cy() + 8.0f, 26.0f,
                          kWhite.with_alpha(0.6f), hui::gfx::Align::center);
        }
        list.bordered_rect(art, 36.0f, Color::rgb(0x000000, 0.0f), 2.0f, kWhite.with_alpha(0.16f));

        const float x = kMargin + slide;
        hui::ui::text(list, fonts.semibold,
                      hui::ui::upper(editing_ ? "Edit forwarder" : "New forwarder"), x, 212.0f,
                      20.0f, tint, hui::gfx::Align::left, 4.0f);
        const bool untitled = forwarder_.display_name.empty();
        const std::string title =
            untitled ? std::string("Untitled forwarder") : forwarder_.display_name;
        hui::ui::text(list, fonts.display, fonts.display.font->fit(title, 88.0f, 1150.0f), x - 4.0f,
                      304.0f, 88.0f, untitled ? kWhite.with_alpha(0.45f) : kWhite);

        // "<system>  ·  <title id>  ·  <ROM>", as the concept's genre · year · studio.
        const bool stream = is_stream_target(forwarder_.target);
        const bool game = library_of(forwarder_.target) != GameLibrary::none;
        const std::string rom =
            stream ? (forwarder_.app.empty() ? std::string("no app") : forwarder_.app)
            : forwarder_.rom.empty() ? std::string(game ? "no game" : "no ROM")
            : game                   ? library_game_name(forwarder_.target, forwarder_.rom)
                                     : forwarder_.rom;
        (void)std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s  \xC2\xB7  %s",
                            target_display_name(forwarder_.target).c_str(),
                            forwarder_.title_id.c_str(), rom.c_str());
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(text, 26.0f, 1150.0f), x, 358.0f,
                      26.0f, kWhite.with_alpha(0.78f));

        // The blurb: what the tile will do, and what the preview is showing.
        std::string blurb =
            "Launches " + target_display_name(forwarder_.target) +
            (stream ? (forwarder_.app.empty() ? std::string(" and waits for an app to be named.")
                                              : std::string(" and starts streaming the app above."))
             : forwarder_.rom.empty()
                 ? std::string(game ? " and opens its library." : " without a ROM.")
             : game ? std::string(" and starts the game above.")
                    : std::string(" with the ROM above."));
        if (has_background() && background_tex_ != 0)
            blurb += " Its background is behind this screen";
        if (has_music() && context.music != nullptr && context.music->playing())
            blurb += (has_background() && background_tex_ != 0)
                         ? " and its selection music is playing."
                         : " Its selection music is playing.";
        else if (has_background() && background_tex_ != 0)
            blurb += ".";
        hui::ui::paragraph(list, fonts.regular, blurb, x, 414.0f, 28.0f, 820.0f, 40.0f,
                           kWhite.with_alpha(0.86f), 2);

        // Completeness, where the concept shows play progress.
        const int parts = 5;
        const int done = (forwarder_.display_name.empty() ? 0 : 1) +
                         (forwarder_.target.empty() ? 0 : 1) + (has_icon() ? 1 : 0) +
                         (has_background() ? 1 : 0) + (has_music() ? 1 : 0);
        const float progress = static_cast<float>(done) / static_cast<float>(parts);
        const Rect bar{x, 496.0f, 420.0f, 8.0f};
        list.rounded_rect(bar, 4.0f, kWhite.with_alpha(0.18f));
        if (done > 0)
            list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * progress * in), bar.h}, 4.0f,
                              tint);
        const bool ready =
            !forwarder_.display_name.empty() && !forwarder_.target.empty() && has_icon();
        (void)std::snprintf(text, sizeof(text), "%d of %d set  \xC2\xB7  %s", done, parts,
                            ready ? (editing_ ? "ready to save" : "ready to generate")
                                  : "name, target and icon are required");
        hui::ui::text(list, fonts.regular, text, bar.x + bar.w + 24.0f, 508.0f, 22.0f,
                      kWhite.with_alpha(0.7f));
        list.pop_opacity();
    }

    // The concept's frosted details sheet, with the form where its artwork
    // and blurb go and its three stat tiles reporting the presentation assets.
    void draw_sheet(Context &context, hui::ui::Canvas &overlay) const
    {
        hui::gfx::DrawList &list = overlay.list;
        const hui::ui::Fonts &fonts = context.fonts;
        const Color tint = accent();
        const float t = stagger(2, 0.1f, 0.5f);
        const Rect sheet{kSheet.x, kSheet.y + 60.0f * (1.0f - t), kSheet.w, kSheet.h};
        list.push_opacity(hui::tween::clamp01(t * 1.4f));
        list.shadow({sheet.x, sheet.y + 20.0f, sheet.w, sheet.h}, 44.0f, 60.0f,
                    Color::rgb(0x000000, 0.5f));
        if (overlay.glass != 0)
            list.glass(overlay.glass, sheet, 44.0f, kWhite);
        list.rounded_rect(
            sheet, 44.0f,
            hui::gfx::mix(hui::gfx::mix(tint, kInk, 0.75f), kInk, 0.5f).with_alpha(0.62f));
        list.bordered_rect(sheet, 44.0f, Color::rgb(0x000000, 0.0f), 1.5f,
                           kWhite.with_alpha(0.22f));

        // Stat tiles on the right: ICON / BACKGROUND / MUSIC.
        const float tiles_x = sheet.x + sheet.w - 56.0f - (kTilePitch * 2.0f + kTileW);
        const float tiles_y = sheet.y + 56.0f;
        const char *labels[3] = {"ICON", "BACKGROUND", "MUSIC"};
        for (int i = 0; i < 3; ++i)
        {
            const float appear = hui::tween::stagger(age_, 3 + i, 0.12f, 0.6f);
            const Rect tile{tiles_x + static_cast<float>(i) * kTilePitch,
                            tiles_y + 24.0f * (1.0f - appear), kTileW, kTileH};
            list.push_opacity(appear);
            list.rounded_rect(tile, 22.0f, kWhite.with_alpha(0.08f));
            hui::ui::text(list, fonts.semibold, labels[i], tile.x + 22.0f, tile.y + 36.0f, 15.0f,
                          kWhite.with_alpha(0.55f), hui::gfx::Align::left, 3.0f);
            const char *value = "Missing";
            Color value_color = kWhite.with_alpha(0.5f);
            if (i == 0 && has_icon())
            {
                value = "Set";
                value_color = kWhite;
                if (icon_tex_ != 0)
                    list.image(icon_tex_,
                               {tile.x + tile.w - 22.0f - 56.0f, tile.y + 48.0f, 56.0f, 56.0f},
                               hui::gfx::kFullUv, kWhite, 12.0f);
            }
            else if (i == 1)
            {
                value = has_background() ? "Set" : "None";
                value_color = has_background() ? kWhite : kWhite.with_alpha(0.5f);
            }
            else if (i == 2)
            {
                const bool playing = context.music != nullptr && context.music->playing();
                value = playing ? "Playing" : (has_music() ? "Set" : "None");
                value_color = has_music() ? (playing ? tint : kWhite) : kWhite.with_alpha(0.5f);
                if (playing)
                {
                    // A small level meter breathing with the music, as a sign of life.
                    const float level = 0.5f + 0.5f * hui::ui::breathe(clock_, 1.1f);
                    for (int b = 0; b < 4; ++b)
                    {
                        const float h =
                            10.0f +
                            22.0f * level * (0.55f + 0.45f * std::sin(clock_ * 5.0f + b * 1.7f));
                        list.rounded_rect({tile.x + tile.w - 22.0f - 44.0f + b * 11.0f,
                                           tile.y + 96.0f - h, 7.0f, h},
                                          3.0f, tint.with_alpha(0.9f));
                    }
                }
            }
            hui::ui::text(list, fonts.semibold, value, tile.x + 22.0f, tile.y + 92.0f, 34.0f,
                          value_color);
            list.pop_opacity();
        }

        // Under the tiles: where the row controls lead.
        hui::ui::paragraph(list, fonts.regular,
                           converting_ ? "Converting the audio to ATRAC9 online..."
                                       : "Pick the icon from a file or SteamGridDB; backgrounds "
                                         "and music are optional and previewed here.",
                           tiles_x, tiles_y + kTileH + 44.0f, 22.0f, kTilePitch * 2.0f + kTileW,
                           30.0f, kWhite.with_alpha(0.6f), 3);
        list.pop_opacity();
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kEdit[] = {
            {hui::ui::Button::cross, "Select"},
            {hui::ui::Button::triangle, "Save"},
            {hui::ui::Button::circle, "Back"},
        };
        static const hui::ui::Hint kNew[] = {
            {hui::ui::Button::cross, "Select"},
            {hui::ui::Button::triangle, "Generate"},
            {hui::ui::Button::circle, "Back"},
        };
        return editing_ ? std::span<const hui::ui::Hint>(kEdit)
                        : std::span<const hui::ui::Hint>(kNew);
    }

  private:
    Context *context_;
    Forwarder forwarder_;
    Assets assets_;
    bool editing_;
    int target_index_ = 0;
    bool dirty_ = false;
    Prompting prompting_ = Prompting::none;
    mutable hui::ui::Form form_;
    mutable hui::ui::Dialog dialog_;
    mutable hui::ui::InputPrompt prompt_;
    mutable hui::ui::ToastStack toasts_;
    float age_ = 0.0f;   // seconds since the screen opened (entrance stagger)
    float clock_ = 0.0f; // free-running, for the artwork's float and the meter

    // Live preview.
    std::uint32_t icon_tex_ = 0;
    std::uint32_t background_tex_ = 0;
    bool icon_dirty_ = false;       // a new icon was picked
    bool icon_from_poster_ = false; // the icon is the picked app's box art, not the player's choice
    // What the streaming target's client published (fwd/stream_catalog.hpp).
    std::vector<StreamPc> catalog_;
    int pc_index_ = 0; // into catalog_; one past it: pc_custom_value_
    bool pc_custom_ = false;
    std::string pc_custom_value_;
    bool type_app_ = false; // open the app name prompt on the next frame
    // The emulator's games, while its list is open (fwd/x360_library.hpp).
    std::string x360_install_;
    std::vector<LibraryGame> games_;
    bool choose_game_file_ = false; // open the file picker on the next frame
    bool background_dirty_ = false; // a new background was picked
    bool music_dirty_ = false;      // new music was picked or converted
    bool existing_loaded_ = false;  // an existing forwarder's files were read

    // Online AT9 conversion worker.
    bool converting_ = false;
    bool convert_running_ = false;
    std::atomic<bool> convert_done_{false};
    pthread_t convert_thread_{};
    std::mutex convert_mutex_;
    std::vector<unsigned char> convert_in_;
    std::vector<unsigned char> convert_out_;
    std::string convert_name_;
    std::string convert_error_;
};

} // namespace

std::unique_ptr<Screen> make_edit_screen(Context &context, Forwarder initial, bool editing)
{
    return std::make_unique<EditScreen>(context, std::move(initial), editing);
}

} // namespace fwd
