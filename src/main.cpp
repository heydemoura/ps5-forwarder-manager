// ps5fwdgen - Application entry point.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asks Lapy for filesystem access first (single-threaded, before any other
// subsystem), then opens the display, controller and audio and runs the
// forwarder generator every frame: read input, update, play the sounds it
// asked for, draw, present.

#include "app/app.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "gfx/renderer.hpp"
#include "platform/app_paths.hpp"
#include "platform/elevation.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace
{

// Settings live in the title's own save area, which exists in both the
// sandboxed and the elevated state.
constexpr const char *kDataRoot = "/download0/ps5fwdgen";

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[FWD] heap frames=%llu live=%zu peak=%zu blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live, peak, blocks, failures);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &assets, const char *name,
               hui::gfx::Font *font, hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = assets + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[FWD] font %s failed: %s", path.c_str(), font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

const char *status_name(elevation::Status status)
{
    switch (status)
    {
    case elevation::Status::ok:
        return "ok";
    case elevation::Status::invalid_request:
        return "invalid_request";
    case elevation::Status::unavailable:
        return "unavailable";
    case elevation::Status::timeout:
        return "timeout";
    case elevation::Status::denied:
        return "denied";
    }
    return "unknown";
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[FWD] entry title=%s root=%s", paths::title_id().c_str(), paths::app_root().c_str());
    sys::log("[FWD] storage dir=%d", save::ensure_directory(kDataRoot) ? 1 : 0);

    // Elevation first: the Lapy helper is handed to the local ELF loader
    // while the process is still single-threaded. Only ok permits /data use.
    const std::string helper = paths::app_root() + "/lapy.elf";
    const std::int64_t elevate_start = sys::monotonic_us();
    const elevation::Status elevated =
        elevation::request(elevation::Capability::filesystem, helper.c_str());
    sys::log("[FWD] elevation status=%s path=%s in %lld ms", status_name(elevated),
             elevation::path(), static_cast<long long>((sys::monotonic_us() - elevate_start) / 1000));
    // The process root may have changed: find the app's files again.
    paths::refresh();
    sys::log("[FWD] app root after elevation: %s", paths::app_root().c_str());
    const std::string assets = paths::assets();

    ps5::Display display;
    if (!display.open(1920, 1080))
    {
        sys::log("[FWD] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, assets, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, assets, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, assets, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, assets, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[FWD] fatal: renderer init failed");
        sys::park();
    }

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(assets + "/audio/sfx");
    sys::log("[FWD] sounds files=%d rejected=%d", bank.files, bank.rejected);

    fwd::App app(fonts, renderer, kDataRoot, elevated == elevation::Status::ok,
                 status_name(elevated));
    const std::string version = read_content_version(paths::app_root() + "/sce_sys/param.json");
    app.set_version(version);
    sys::log("[FWD] version %s", version.empty() ? "unknown" : version.c_str());

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    PadSample samples[64];
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                                static_cast<std::uint64_t>(now));

        app.update(input, dt);
        if (app.take_settings_changed())
        {
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = app.swap_confirm();
            tracker.set_settings(input_settings);
        }
        const ui::Feedback &feedback = app.feedback();
        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, event.set == audio::SoundSet::count ? app.sound_set() : event.set,
                        event);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        const gfx::Color accent = app.accent();
        pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                          static_cast<std::uint8_t>(accent.g * 255.0f),
                          static_cast<std::uint8_t>(accent.b * 255.0f));

        app.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[FWD] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        if (frames == 1)
        {
            sys::log("[FWD] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[FWD] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
        }
        previous = presented;
        if (app.quit_requested())
        {
            sys::log("[FWD] closing: quit requested");
            pad.close();
            sys::quit();
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[FWD] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}
