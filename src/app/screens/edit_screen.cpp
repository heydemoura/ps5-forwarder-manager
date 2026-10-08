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
#include "ui/fonts.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fwd
{

namespace
{

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

constexpr hui::gfx::Rect kFormBounds{96.0f, 300.0f, 1728.0f, 640.0f};

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
        prompt_.keyboard.style.bindings = hui::ui::KeyboardBindings::standard();
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
                context, context.settings.forwarders_root, {},
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
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        const hui::ui::Theme &theme = context.theme;
        const hui::gfx::Color text = theme.page_text.a > 0.0f ? theme.page_text : theme.text;
        hui::ui::text(scene.list, context.fonts.display,
                      editing_ ? "Edit forwarder" : "New forwarder", 96.0f, 150.0f, 46.0f, text);
        form_.draw(scene);
        const bool modal = dialog_.visible() || prompt_.visible();
        dialog_.draw(overlay);
        prompt_.draw(overlay);
        toasts_.draw(overlay);
        return modal;
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
};

} // namespace

std::unique_ptr<Screen> make_edit_screen(Context &context, Forwarder initial, bool editing)
{
    return std::make_unique<EditScreen>(context, std::move(initial), editing);
}

} // namespace fwd
