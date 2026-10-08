// ps5fwdgen - Create or edit one forwarder.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/screens/edit_screen.hpp"

#include "app/context.hpp"
#include "app/screens/art_screen.hpp"
#include "app/screens/file_picker_screen.hpp"
#include "fwd/store.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/form.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/toast.hpp"
#include "net/http.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"
#include "app/tile.hpp"
#include "gfx/renderer.hpp"
#include "gfx/gl_batch.hpp"
#include <GL/glcorearb.h>
#include <cmath>

#include <atomic>
#include <mutex>
#include <pthread.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

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
};

constexpr hui::gfx::Rect kFormBounds{96.0f, 300.0f, 1040.0f, 640.0f};

class EditScreen final : public Screen
{
  public:
    EditScreen(Context &context, Forwarder initial, bool editing)
        : forwarder_(std::move(initial)), editing_(editing)
    {
        if (!editing_ && forwarder_.title_id.empty())
            forwarder_.title_id = new_title_id(context.settings.forwarders_root);
        if (forwarder_.target.empty())
            forwarder_.target = "PPSA99008";
        target_index_ = index_for_target(forwarder_.target);
        restyle(context);
        form_.set_bounds(kFormBounds);
        form_.set_active(true);
        build(context);
        {
            auto kb = hui::ui::KeyboardBindings::standard();
            kb.done = hui::Action::page_next; // R1 confirms (a console keyboard nicety)
            prompt_.keyboard.style.bindings = kb;
        }
        prompt_.style.buttons = false; // single Done: the keyboard's own key (closes on press)
    }

    ~EditScreen() override
    {
        join_convert();
        if (preview_tex_ != 0)
            glDeleteTextures(1, &preview_tex_);
    }

    void restyle(Context &context) override
    {
        form_.style.theme = context.theme;
        form_.style.panel = true;
        dialog_.style.theme = context.theme;
        toasts_.style.theme = context.theme;
        prompt_.style.theme = context.theme;
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
        form_.add_action(RowName, "Display name").text =
            forwarder_.display_name.empty() ? "(required)" : forwarder_.display_name;
        form_.add_choice(RowTarget, "Target app", target_names, target_index_);
        if (target_is_generic())
            form_.add_action(RowTargetId, "Target title ID").text =
                forwarder_.target.empty() ? "(enter one)" : forwarder_.target;
        form_.add_action(RowTitleId, "Forwarder title ID").text = forwarder_.title_id;

        form_.add_header("Launch");
        form_.add_action(RowRom, "ROM file").text =
            forwarder_.rom.empty() ? "(none)" : forwarder_.rom;
        if (current_target().exit_after_game)
            form_.add_toggle(RowExit, "Exit after quitting game", forwarder_.exit_after_game);

        form_.add_header("Presentation");
        form_.add_action(RowIcon, "Tile icon").text =
            (forwarder_.has_icon || !assets_.icon_png.empty()) ? "set" : "(required)";
        form_.add_action(RowBackground, "Backgrounds").text =
            (forwarder_.has_backgrounds || !assets_.pic0_dds.empty()) ? "set" : "(optional)";
        form_.add_action(RowMusic, "Selection music (.at9)").text =
            (forwarder_.has_music || !assets_.music_at9.empty()) ? "set" : "(optional)";

        form_.add_header("");
        form_.add_action(RowGenerate, editing_ ? "Save forwarder" : "Generate forwarder")
            .chevron = false;
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
        forwarder_.exit_after_game =
            current_target().exit_after_game && forwarder_.exit_after_game;
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
        if (!forwarder_.has_icon && assets_.icon_png.empty())
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

        if (input.is_pressed(hui::Action::back))
        {
            context.pop();
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
        case RowRom:
        {
            EditScreen *self = this;
            context.push(make_file_picker_screen(
                context, "/data", {},
                [self](const std::string &path)
                {
                    const std::size_t slash = path.find_last_of('/');
                    self->forwarder_.rom = slash == std::string::npos ? path : path.substr(slash + 1);
                    self->dirty_ = true;
                }));
            break;
        }
        case RowIcon:
        {
            EditScreen *self = this;
            context.push(make_art_screen(
                context, ArtKind::icon, forwarder_.display_name,
                [self](std::vector<unsigned char> bytes)
                {
                    self->assets_.icon_png = std::move(bytes);
                    self->dirty_ = true;
                    self->icon_dirty_ = true;
                }));
            break;
        }
        case RowBackground:
        {
            EditScreen *self = this;
            context.push(make_art_screen(
                context, ArtKind::background, forwarder_.display_name,
                [self](std::vector<unsigned char> bytes)
                {
                    self->assets_.pic0_dds = bytes;
                    self->assets_.pic1_dds = std::move(bytes);
                    self->dirty_ = true;
                }));
            break;
        }
        case RowMusic:
        {
            EditScreen *self = this;
            context.push(make_file_picker_screen(
                context, "/data", {".at9", ".mp3", ".wav", ".ogg", ".flac", ".m4a"},
                [self](const std::string &path)
                {
                    std::vector<unsigned char> bytes;
                    if (!read_whole_file(path, bytes))
                        return;
                    const bool is_at9 = path.size() > 4 &&
                                        path.compare(path.size() - 4, 4, ".at9") == 0;
                    if (is_at9)
                    {
                        self->assets_.music_at9 = std::move(bytes);
                        self->dirty_ = true;
                    }
                    else
                    {
                        // Non-AT9 audio is converted online, as the site does.
                        const std::size_t slash = path.find_last_of('/');
                        self->start_convert(std::move(bytes),
                                            slash == std::string::npos ? path
                                                                       : path.substr(slash + 1));
                    }
                }));
            break;
        }
        case RowGenerate:
            save(context, feedback);
            break;
        case RowDelete:
            dialog_.open({hui::ui::StatusKind::danger, "Delete this forwarder?",
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
    static constexpr const char *kConvertUrl = "https://ps5-forwarder.mph.am/api/convert-at9";

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
        const net::Response response = net::post_file(kConvertUrl, "file", name, audio);
        std::lock_guard<std::mutex> lock(self->convert_mutex_);
        const bool looks_at9 = response.body.size() > 12 && response.body[0] == 'R' &&
                               response.body[1] == 'I' && response.body[2] == 'F' &&
                               response.body[3] == 'F';
        if (response.status == 200 && looks_at9)
            self->convert_out_ = response.body;
        else if (response.status == 0)
            self->convert_error_ = response.error.empty() ? "conversion failed" : response.error;
        else
            self->convert_error_ = "conversion HTTP " + std::to_string(response.status);
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
            assets_.music_at9 = std::move(convert_out_);
            dirty_ = true;
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
        prompt_.style.max_length = kind == Prompting::name ? 60 : 9;
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

    void enter(Context &context) override
    {
        if (dirty_)
        {
            build(context);
            dirty_ = false;
        }
        if (preview_tex_ == 0 && assets_.icon_png.empty() && editing_ && forwarder_.has_icon)
        {
            const std::string dir =
                context.settings.forwarders_root + "/" + forwarder_.title_id;
            preview_tex_ = load_icon_texture(context.renderer, dir);
        }
        refresh_preview(context);
    }

    void refresh_preview(Context &context)
    {
        if (!icon_dirty_)
            return;
        icon_dirty_ = false;
        if (preview_tex_ != 0)
        {
            glDeleteTextures(1, &preview_tex_);
            preview_tex_ = 0;
        }
        if (!assets_.icon_png.empty())
            preview_tex_ = upload_image_texture(context.renderer, assets_.icon_png.data(),
                                               assets_.icon_png.size());
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        hui::ui::text(scene.list, context.fonts.display,
                      editing_ ? "Edit forwarder" : "New forwarder", 96.0f, 150.0f, 46.0f, text);
        form_.draw(scene);
        draw_preview(context, scene.list);
        if (converting_)
            hui::ui::text(scene.list, context.fonts.regular, "Converting audio to ATRAC9 online...",
                          96.0f, 960.0f, 26.0f, theme.text_muted);
        const bool modal = dialog_.visible() || prompt_.visible();
        dialog_.draw(overlay);
        prompt_.draw(overlay);
        toasts_.draw(overlay);
        return modal;
    }

    void draw_preview(Context &context, hui::gfx::DrawList &list) const
    {
        using hui::gfx::Color;
        using hui::gfx::Rect;
        const hui::ui::Fonts &fonts = context.fonts;
        const Color white = Color::rgb(0xffffff);
        const std::string seed =
            forwarder_.title_id.empty() ? std::string("__new__") : forwarder_.title_id;
        const Color accent = accent_for(seed);

        // Panel.
        const Rect panel{1180.0f, 300.0f, 644.0f, 620.0f};
        list.rounded_rect(panel, 28.0f, Color::rgb(0x0b0d16, 0.5f));
        list.bordered_rect(panel, 28.0f, Color::rgb(0x000000, 0.0f), 1.5f, white.with_alpha(0.14f));
        hui::ui::text(list, fonts.semibold, "LIVE PREVIEW", panel.x + 36.0f, panel.y + 52.0f, 18.0f,
                      accent, hui::gfx::Align::left, 4.0f);

        // The tile as the home screen would show it.
        const float size = 300.0f;
        const Rect art{panel.cx() - size * 0.5f, panel.y + 84.0f, size, size};
        list.glow(art.inset(24.0f), 50.0f, 70.0f, accent.with_alpha(0.3f));
        list.shadow({art.x, art.y + 20.0f, art.w, art.h}, 30.0f, 40.0f, Color::rgb(0x000000, 0.5f));
        if (preview_tex_ != 0)
        {
            list.image(preview_tex_, art, hui::gfx::kFullUv, white, 28.0f);
        }
        else
        {
            list.gradient_rect(art, 28.0f, hui::gfx::mix(accent, Color::rgb(0x0b0d16), 0.4f),
                               Color::rgb(0x0b0d16));
            hui::ui::text(list, fonts.regular, "no icon yet", art.cx(), art.cy() + 8.0f, 24.0f,
                          white.with_alpha(0.6f), hui::gfx::Align::center);
        }
        list.bordered_rect(art, 28.0f, Color::rgb(0x000000, 0.0f), 2.0f, white.with_alpha(0.16f));

        // Title under the tile, like the home screen.
        const std::string name =
            forwarder_.display_name.empty() ? std::string("Untitled forwarder")
                                            : forwarder_.display_name;
        hui::ui::text(list, fonts.semibold, name, panel.cx(), art.y + art.h + 54.0f, 30.0f,
                      forwarder_.display_name.empty() ? white.with_alpha(0.5f) : white,
                      hui::gfx::Align::center);
        hui::ui::text(list, fonts.regular,
                      std::string("Runs on ") + target_display_name(forwarder_.target),
                      panel.cx(), art.y + art.h + 94.0f, 24.0f, accent, hui::gfx::Align::center);
        hui::ui::text(list, fonts.regular, forwarder_.title_id, panel.cx(), art.y + art.h + 128.0f,
                      20.0f, white.with_alpha(0.55f), hui::gfx::Align::center);
        if (!forwarder_.rom.empty())
            hui::ui::text(list, fonts.regular, std::string("ROM: ") + forwarder_.rom, panel.cx(),
                          art.y + art.h + 162.0f, 20.0f, white.with_alpha(0.7f),
                          hui::gfx::Align::center);
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        static const hui::ui::Hint kHints[] = {
            {hui::ui::Button::cross, "Select"},
            {hui::ui::Button::circle, "Back"},
        };
        return kHints;
    }

  private:
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

    // Live preview.
    std::uint32_t preview_tex_ = 0;
    bool icon_dirty_ = false;  // set when art changes; enter() loads an existing icon

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
