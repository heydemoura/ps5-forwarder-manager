// ps5fwdgen - Home: the forwarders as a console home screen (Aurora Shelf).
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Modelled on ps5-homebrew-ui's Aurora Shelf design: a hero preview of the
// focused forwarder above a shelf of cover tiles, a backdrop eased toward the
// selection's colour, a gliding focus ring, and a frosted details sheet with
// Edit / Delete. The first tile creates a new forwarder.

#include "app/screens/home_screen.hpp"

#include "app/context.hpp"
#include "app/screens/edit_screen.hpp"
#include "app/screens/settings_screen.hpp"
#include "app/tile.hpp"
#include "core/tween.hpp"
#include "fwd/store.hpp"
#include "gfx/gl_batch.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <GL/glcorearb.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fwd
{

namespace
{

using hui::gfx::Color;
using hui::gfx::Rect;

const Color kWhite = Color::rgb(0xffffff);

constexpr float kMargin = 96.0f;
constexpr float kCard = 200.0f;
constexpr float kCardGap = 28.0f;
constexpr float kCardGrow = 1.18f;
constexpr float kShelfY = 724.0f;
constexpr float kHeroTextWidth = 1180.0f;

extern "C" void glDeleteTextures(int n, const unsigned int *textures);

class HomeScreen final : public Screen
{
  public:
    explicit HomeScreen(Context &context)
    {
        restyle(context);
    }

    ~HomeScreen() override
    {
        release_textures();
    }

    void restyle(Context &context) override
    {
    }

    void release_textures()
    {
        for (std::uint32_t tex : textures_)
            if (tex != 0)
                glDeleteTextures(1, &tex);
        textures_.clear();
    }

    void enter(Context &context) override
    {
        rescan(context);
    }

    // Tile 0 is "Create"; tiles 1..N are the forwarders visible under the
    // active tab (visible_ holds indices into the full forwarders_ list).
    int tile_count() const
    {
        return 1 + static_cast<int>(visible_.size());
    }
    bool is_create(int tile) const
    {
        return tile == 0;
    }
    int source_index(int tile) const
    {
        return visible_[static_cast<std::size_t>(tile - 1)];
    }
    const Forwarder &forwarder_at(int tile) const
    {
        return forwarders_[static_cast<std::size_t>(source_index(tile))];
    }
    std::uint32_t tex_of(int tile) const
    {
        const int i = source_index(tile);
        return i >= 0 && i < static_cast<int>(textures_.size())
                   ? textures_[static_cast<std::size_t>(i)]
                   : 0;
    }

    void rescan(Context &context)
    {
        release_textures();
        forwarders_ = scan_forwarders(context.settings.forwarders_root);
        textures_.assign(forwarders_.size(), 0);
        for (std::size_t i = 0; i < forwarders_.size(); ++i)
        {
            const std::string dir =
                context.settings.forwarders_root + "/" + forwarders_[i].title_id;
            textures_[i] = load_icon_texture(context.renderer, dir);
        }
        // Group by system (the target app): one tab per distinct target, plus
        // "All" first.
        // Tabs cover every system PS5 Forwarder Builder supports, not only the
        // ones with an installed forwarder, then any extra target seen on disk.
        systems_.clear();
        for (const Target &target : known_targets())
            if (target.title_id[0] != '\0')
                systems_.push_back(target.name);
        for (const Forwarder &f : forwarders_)
        {
            const std::string name = target_display_name(f.target);
            if (std::find(systems_.begin(), systems_.end(), name) == systems_.end())
                systems_.push_back(name);
        }
        if (active_tab_ >= 1 + static_cast<int>(systems_.size()))
            active_tab_ = 0;
        rebuild_visible();
        if (focus_ >= tile_count())
            focus_ = std::max(0, tile_count() - 1);
        shown_ = focus_;
        apply_palette(context, true);
        ring_.snap(card_rect(focus_, true));
        scroll_.reveal(0.0f, 0.0f, hui::gfx::kVirtualWidth - kMargin, kMargin * 2.2f);
    }

    void rebuild_visible()
    {
        visible_.clear();
        for (int i = 0; i < static_cast<int>(forwarders_.size()); ++i)
        {
            if (active_tab_ == 0 ||
                target_display_name(forwarders_[static_cast<std::size_t>(i)].target) ==
                    systems_[static_cast<std::size_t>(active_tab_ - 1)])
                visible_.push_back(i);
        }
    }

    void set_tab(Context &context, int tab, hui::ui::Feedback &feedback)
    {
        const int count = 1 + static_cast<int>(systems_.size());
        if (count <= 1)
            return;
        active_tab_ = ((tab % count) + count) % count;
        feedback.play(hui::audio::Cue::tab);
        rebuild_visible();
        focus_ = 0;
        shown_ = 0;
        apply_palette(context, true);
        ring_.snap(card_rect(0, true));
        scroll_.reveal(0.0f, 0.0f, hui::gfx::kVirtualWidth - kMargin, kMargin * 2.2f);
    }

    std::string seed_of(int tile) const
    {
        if (is_create(tile))
            return "__create__";
        return forwarder_at(tile).title_id;
    }

    void apply_palette(Context &context, bool snap)
    {
        (void)context;
        const std::array<Color, 4> target = palette_for(seed_of(focus_));
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(target[static_cast<std::size_t>(i)]);
            else
                palette_[i].target(target[static_cast<std::size_t>(i)]);
        }
    }

    Rect card_rect(int tile, bool focused) const
    {
        const float x = kMargin + static_cast<float>(tile) * (kCard + kCardGap) - scroll_.offset();
        const float y = kShelfY;
        if (!focused)
            return {x, y, kCard, kCard};
        const float grown = kCard * kCardGrow;
        return {x, y - (grown - kCard), grown, grown};
    }

    void update(Context &context, const hui::InputFrame &input, float dt,
                hui::ui::Feedback &feedback) override
    {
        clock_ += dt;
        age_ += dt;

        if (!context.elevated)
        {
            if (input.is_pressed(hui::Action::menu))
                context.push(make_settings_screen(context));
            if (input.is_pressed(hui::Action::north))
                context.quit = true;
            return;
        }

        update_shelf(context, input, feedback);

        if (focus_ != shown_)
        {
            previous_ = shown_;
            shown_ = focus_;
            hero_.start(context.settings.reduced_motion ? 0.12f : 0.4f);
            apply_palette(context, false);
        }
        hero_.update(dt);
        for (hui::ui::SpringColor &c : palette_)
            c.update(dt, 4.0f);
        const float start = static_cast<float>(focus_) * (kCard + kCardGap);
        scroll_.reveal(start, start + kCard * kCardGrow, hui::gfx::kVirtualWidth - kMargin,
                       kMargin * 2.2f);
        scroll_.update(dt, 12.0f);
        ring_.target(card_rect(focus_, true));
        ring_.update(dt, 20.0f);
        nudge_.update(dt, 9.0f);
    }

    void update_shelf(Context &context, const hui::InputFrame &input, hui::ui::Feedback &feedback)
    {
        bool refused = false;
        if (input.nav == hui::Direction::left || input.nav == hui::Direction::right)
        {
            const int next = focus_ + (input.nav == hui::Direction::right ? 1 : -1);
            if (next >= 0 && next < tile_count())
            {
                focus_ = next;
                feedback.play(hui::audio::Cue::focus, 1.0f,
                              hui::ui::pan_for_x(card_rect(next, false).cx()));
            }
            else
            {
                refused = !input.nav_repeat;
                nudge_direction_ = input.nav == hui::Direction::right ? 1.0f : -1.0f;
            }
        }
        if (refused)
        {
            feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.6f);
            nudge_.trigger();
        }
        if (input.is_pressed(hui::Action::page_prev))
            set_tab(context, active_tab_ - 1, feedback);
        if (input.is_pressed(hui::Action::page_next))
            set_tab(context, active_tab_ + 1, feedback);
        if (input.is_pressed(hui::Action::confirm))
        {
            if (is_create(focus_))
            {
                feedback.play(hui::audio::Cue::open);
                context.push(make_edit_screen(context, Forwarder{}, false));
            }
            else
            {
                // Straight to the edit screen, which also offers Delete.
                feedback.play(hui::audio::Cue::select);
                context.push(make_edit_screen(context, forwarder_at(focus_), true));
            }
        }
        if (input.is_pressed(hui::Action::north)) // Triangle: new
        {
            feedback.play(hui::audio::Cue::open);
            context.push(make_edit_screen(context, Forwarder{}, false));
        }
        if (input.is_pressed(hui::Action::menu)) // Options: settings
            context.push(make_settings_screen(context));
    }

    // ---- drawing -----------------------------------------------------------

    void draw_tile_art(hui::gfx::DrawList &list, const Rect &rect, int tile, float alpha,
                       float radius) const
    {
        if (is_create(tile))
        {
            const Color accent = accent_for("__create__");
            list.rounded_rect(rect, radius, Color::rgb(0x10131c, alpha * 0.9f));
            list.bordered_rect(rect, radius, Color::rgb(0x000000, 0.0f), 2.5f,
                               accent.with_alpha(alpha * 0.8f));
            const float cx = rect.cx();
            const float cy = rect.cy();
            const float s = rect.w * 0.18f;
            list.rounded_rect({cx - s, cy - 5, 2 * s, 10}, 5, kWhite.with_alpha(alpha));
            list.rounded_rect({cx - 5, cy - s, 10, 2 * s}, 5, kWhite.with_alpha(alpha));
            return;
        }
        const std::uint32_t tex = tex_of(tile);
        if (tex != 0)
        {
            list.image(tex, rect, hui::gfx::kFullUv, kWhite.with_alpha(alpha), radius);
        }
        else
        {
            const Color accent = accent_for(seed_of(tile));
            list.gradient_rect(rect, radius, hui::gfx::mix(accent, Color::rgb(0x0b0d16), 0.4f),
                               Color::rgb(0x0b0d16, alpha));
        }
    }

    bool draw(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const override
    {
        if (!context.elevated)
            return draw_not_elevated(context, scene, overlay);

        (void)overlay;
        hui::gfx::DrawList &list = scene.list;
        draw_header(context, list);
        draw_hero(context, list);
        draw_shelf(context, list);
        return false;
    }

    void backdrop(Context &context, hui::gfx::BackdropSpec &spec) const override
    {
        if (!context.elevated)
            return;
        spec.mode = hui::gfx::BackdropMode::aurora;
        spec.colors[0] = palette_[0].value();
        spec.colors[1] = palette_[1].value();
        spec.colors[2] = palette_[2].value();
        spec.colors[3] = palette_[3].value();
    }

    void draw_header(Context &context, hui::gfx::DrawList &list) const
    {
        // The Aurora Shelf top bar (ps5-homebrew-ui src/concepts/aurora.cpp,
        // draw_top_bar): the tabs are plain text labels running from the left
        // margin; the active one is semibold at full white with a 4px accent
        // underline, the rest are regular at 55% white. The whole bar fades and
        // slides down into place on entry. Our systems take the concept's
        // {Home, Library, ...} slots, and the total count sits where the
        // concept puts its clock.
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = tween_stagger(0, 0.05f, 0.5f);
        list.push_opacity(in);
        const Color accent = palette_[3].value();
        const float baseline = 92.0f - 10.0f * (1.0f - in);
        const int tab_count = 1 + static_cast<int>(systems_.size());
        float x = kMargin;
        for (int i = 0; i < tab_count; ++i)
        {
            const std::string_view label =
                i == 0 ? std::string_view("All") : std::string_view(systems_[i - 1]);
            const bool active = i == active_tab_;
            const float w = hui::ui::text(list, active ? fonts.semibold : fonts.regular, label, x,
                                          baseline, 26.0f,
                                          kWhite.with_alpha(active ? 1.0f : 0.55f));
            if (active)
                list.rounded_rect({x, 104.0f, w, 4.0f}, 2.0f, accent);
            x += w + 44.0f;
        }
        char count[64];
        (void)std::snprintf(count, sizeof(count), "%zu forwarder%s", forwarders_.size(),
                            forwarders_.size() == 1 ? "" : "s");
        hui::ui::text(list, fonts.regular, count, hui::gfx::kVirtualWidth - kMargin, baseline, 26.0f,
                      kWhite.with_alpha(0.8f), hui::gfx::Align::right);
        list.pop_opacity();
    }

    void draw_hero(Context &context, hui::gfx::DrawList &list) const
    {
        const float in = tween_stagger(1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (hero_.running)
        {
            const float t = hero_.progress();
            draw_hero_item(context, list, previous_,
                           1.0f - hui::tween::smoothstep(t * 2.2f),
                           -36.0f * hui::tween::cubic_in(hui::tween::clamp01(t * 2.2f)));
            const float arrive = hui::tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero_item(context, list, shown_, hui::tween::smoothstep(arrive),
                           44.0f * (1.0f - hui::tween::quint_out(arrive)));
        }
        else
        {
            draw_hero_item(context, list, shown_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    void draw_hero_item(Context &context, hui::gfx::DrawList &list, int tile, float alpha,
                        float slide) const
    {
        if (alpha <= 0.01f)
            return;
        const hui::ui::Fonts &fonts = context.fonts;
        list.push_opacity(alpha);
        const Color accent = accent_for(seed_of(tile));

        // Big preview artwork, floating gently, with its own glow.
        const float bob = context.settings.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 6.0f;
        const Rect art{1348 + slide * 1.6f, 150 + bob, 420, 420};
        list.glow(art.inset(28), 60, 90, accent.with_alpha(0.3f));
        list.shadow({art.x, art.y + 26, art.w, art.h}, 36, 46, Color::rgb(0x000000, 0.55f));
        draw_tile_art(list, art, tile, 1.0f, 40.0f);
        list.bordered_rect(art, 40, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.16f));

        const float x = kMargin + slide;
        if (is_create(tile))
        {
            hui::ui::text(list, fonts.semibold, "NEW", x, 236, 20, accent, hui::gfx::Align::left, 4.0f);
            hui::ui::text(list, fonts.display, "Create a forwarder", x - 4, 320, 76, kWhite);
            hui::ui::paragraph(list, fonts.regular,
                               "Make a home-screen tile that launches an app with a ROM. "
                               "Pick a target, a ROM file and tile art.",
                               x, 392, 28, 760, 40, kWhite.with_alpha(0.82f), 3);
            draw_cta(list, fonts, x, "Create", accent);
            list.pop_opacity();
            return;
        }

        const Forwarder &f = forwarder_at(tile);
        hui::ui::text(list, fonts.semibold, hui::ui::upper(target_display_name(f.target)), x, 236,
                      20, accent, hui::gfx::Align::left, 4.0f);
        const std::string title = fonts.display.font->fit(
            f.display_name.empty() ? f.title_id : f.display_name, 72.0f, kHeroTextWidth);
        hui::ui::text(list, fonts.display, title, x - 4, 320, 72, kWhite);
        char meta[200];
        (void)std::snprintf(meta, sizeof(meta), "Runs on %s  \xC2\xB7  %s",
                            target_display_name(f.target).c_str(), f.title_id.c_str());
        hui::ui::text(list, fonts.regular, fonts.regular.font->fit(meta, 26.0f, kHeroTextWidth), x,
                      372, 26, kWhite.with_alpha(0.78f));
        if (!f.rom.empty())
            hui::ui::text(list, fonts.regular,
                          fonts.regular.font->fit("ROM: " + f.rom, 24.0f, kHeroTextWidth), x, 414,
                          24, kWhite.with_alpha(0.7f));
        if (f.exit_after_game)
            hui::ui::text(list, fonts.regular, "Exits when you quit the game", x, 452, 22,
                          kWhite.with_alpha(0.55f));
        draw_cta(list, fonts, x, "Edit", accent);
        list.pop_opacity();
    }

    void draw_cta(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, float x,
                  const char *label, const Color &accent) const
    {
        const Rect button{x, 528, 240, 64};
        list.glow(button, 32, 18, accent.with_alpha(0.35f));
        list.rounded_rect(button, 32, kWhite);
        hui::ui::draw_button(list, fonts, hui::ui::GlyphStyle::light(), hui::ui::Button::cross,
                             button.x + 24, button.cy(), 34);
        hui::ui::text(list, fonts.semibold, label, button.x + 74, button.cy() + 10, 26,
                      Color::rgb(0x0b0d16));
    }

    void draw_shelf(Context &context, hui::gfx::DrawList &list) const
    {
        const hui::ui::Fonts &fonts = context.fonts;
        const float in = tween_stagger(3, 0.09f, 0.6f);
        const float nudge =
            hui::ui::shake(nudge_.value, clock_, 16.0f, 8.0f) * nudge_direction_;
        list.push_opacity(in);
        if (visible_.empty())
        {
            const std::string sys = active_tab_ == 0 ? std::string("any system")
                                                     : systems_[static_cast<std::size_t>(
                                                           active_tab_ - 1)];
            hui::ui::text(list, fonts.regular, "No forwarders for " + sys + " yet — press Create",
                          kMargin + kCard + kCardGap + 24.0f, kShelfY + kCard * 0.5f, 26.0f,
                          kWhite.with_alpha(0.65f));
        }
        for (int tile = 0; tile < tile_count(); ++tile)
        {
            if (tile == focus_)
                continue; // focused drawn last
            Rect rect = card_rect(tile, false);
            if (tile > focus_)
                rect.x += kCard * (kCardGrow - 1.0f);
            if (rect.x > hui::gfx::kVirtualWidth || rect.x + rect.w < -80.0f)
                continue;
            // Drop shadow under every icon, for depth.
            list.shadow({rect.x, rect.y + 18.0f, rect.w, rect.h}, 28.0f, 34.0f,
                        Color::rgb(0x000000, 0.65f));
            draw_tile_art(list, rect, tile, 0.82f, 22.0f);
        }
        list.pop_opacity();

        // Focused card: grown, lifted, ringed.
        Rect ring = ring_.value();
        ring.x += nudge;
        const Color accent = accent_for(seed_of(focus_));
        list.push_opacity(in);
        list.shadow({ring.x, ring.y + 18, ring.w, ring.h}, 26, 34, Color::rgb(0x000000, 0.6f));
        list.glow(ring, 26, 26, accent.with_alpha(0.4f + 0.15f * hui::ui::breathe(clock_)));
        draw_tile_art(list, ring, focus_, 1.0f, 26.0f);
        list.bordered_rect(ring.inset(-5), 30, Color::rgb(0x000000, 0.0f), 4, kWhite);
        list.pop_opacity();

        // Hints along the bottom.
        const hui::ui::Hint hints[] = {{hui::ui::Button::cross, is_create(focus_) ? "Create" : "Edit"},
                                       {hui::ui::Button::triangle, "New"},
                                       {hui::ui::Button::options, "Settings"}};
        list.push_opacity(in);
        hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints, 3, 1824, true);
        list.pop_opacity();
    }

    bool draw_not_elevated(Context &context, hui::ui::Canvas &scene, hui::ui::Canvas &overlay) const
    {
        (void)overlay;
        const hui::ui::Theme &theme = context.theme;
        hui::ui::text(scene.list, context.fonts.display, "PS5 Forwarder Generator", 96, 150, 48,
                      kWhite);
        hui::ui::text(scene.list, context.fonts.semibold, "Lapy JB Daemon is not running", 96, 360,
                      34, theme.danger);
        hui::ui::paragraph(scene.list, context.fonts.regular,
                           "This app needs ArkSama's PS5-Lapy-JB-Daemon loaded to reach "
                           "/data/homebrew. Load it, then relaunch. Elevation: " +
                               context.elevation_status + ".",
                           96, 420, 26, 1500, 1.5f, kWhite.with_alpha(0.7f), 6);
        return false;
    }

    std::span<const hui::ui::Hint> hints() const override
    {
        return {};
    }

  private:
    float tween_stagger(int index, float step, float duration) const
    {
        return hui::tween::stagger(age_, index, step, duration);
    }

    std::vector<Forwarder> forwarders_;         // the full scan
    std::vector<std::uint32_t> textures_;       // aligned to forwarders_
    std::vector<std::string> systems_;          // tab labels after "All"
    std::vector<int> visible_;                  // forwarders_ indices in the active tab
    int active_tab_ = 0;                        // 0 = All
    int focus_ = 0;
    int shown_ = 0;
    int previous_ = 0;
    float clock_ = 0.0f;
    float age_ = 0.0f;
    hui::tween::Timer hero_;
    hui::ui::SpringColor palette_[4];
    hui::ui::Scroller scroll_;
    hui::ui::SpringRect ring_;
    hui::ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
};

} // namespace

std::unique_ptr<Screen> make_home_screen(Context &context)
{
    return std::make_unique<HomeScreen>(context);
}

} // namespace fwd
