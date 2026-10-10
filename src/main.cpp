// ps5fwdgen - Application entry point.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asks Lapy for filesystem access first (single-threaded, before any other
// subsystem), then opens the display, controller and audio and runs the
// Forwarder Manager every frame: read input, update, play the sounds it
// asked for, draw, present.

#include "app/app.hpp"
#include "app/dev_input.hpp"
#include "app/dev_selftest.hpp"
#include "app/settings.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "fwd/image.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "net/http.hpp"
#include "platform/app_paths.hpp"
#include "platform/elevation.hpp"
#include "platform/launcher_inject.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstring>
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
    case elevation::Status::unsupported_version:
        return "unsupported_version";
    case elevation::Status::unsupported_capability:
        return "unsupported_capability";
    case elevation::Status::target_mismatch:
        return "target_mismatch";
    case elevation::Status::unavailable:
        return "unavailable";
    case elevation::Status::prepare_failed:
        return "prepare_failed";
    case elevation::Status::apply_failed:
        return "apply_failed";
    case elevation::Status::rollback_failed:
        return "rollback_failed";
    case elevation::Status::transport_error:
        return "transport_error";
    case elevation::Status::protocol_error:
        return "protocol_error";
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

    // A pull-request build carries a label ("PR 12, 1a2b3c4") so it can be
    // told apart on the console. Read it through /app0 now, before elevation
    // moves the process root.
    std::string build_label;
    if (save::read_file("/app0/build-label.txt", &build_label, 256u))
    {
        while (!build_label.empty() && (build_label.back() == '\n' || build_label.back() == '\r' ||
                                        build_label.back() == ' '))
            build_label.pop_back();
        sys::log("[FWD] build label: %s", build_label.c_str());
    }

    // Elevation first: the Lapy helper is handed to the local ELF loader
    // while the process is still single-threaded. Only ok permits /data use.
    const std::string helper = paths::app_root() + "/lapy.elf";
    const std::int64_t elevate_start = sys::monotonic_us();
    const elevation::Status elevated =
        elevation::request(elevation::Capability::filesystem, helper.c_str());
    sys::log("[FWD] elevation status=%s path=%s in %lld ms", status_name(elevated),
             elevation::path(),
             static_cast<long long>((sys::monotonic_us() - elevate_start) / 1000));
    // The process root may have changed: find the app's files again.
    paths::refresh();
    sys::log("[FWD] app root after elevation: %s", paths::app_root().c_str());
    // Forwarder tiles need a launcher payload; start the built-in one if
    // nothing serves them yet (see platform/launcher_inject.hpp).
    launcher::ensure_running(paths::assets() + "/launcher/fwd-launcher.elf");
    const std::string assets = paths::assets();

    // Dev-only: exercise the real write path on hardware when triggered.
    {
        const fwd::Settings dev_settings =
            fwd::Settings::load(std::string(kDataRoot) + "/settings.txt");
        fwd::run_dev_selftest(dev_settings.forwarders_root, assets + "/forwarder-template");
    }

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

    fwd::App app(fonts, renderer, kDataRoot, mixer, elevated == elevation::Status::ok,
                 status_name(elevated));
    std::string version = read_content_version(paths::app_root() + "/sce_sys/param.json");
    if (!build_label.empty())
        version += " (" + build_label + ")";
    app.set_version(version);
    sys::log("[FWD] version %s", version.empty() ? "unknown" : version.c_str());

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    PadSample samples[64];
    std::int64_t last_frame_start = sys::monotonic_us();
    gfx::Canvas shot_canvas;
    bool shot_pending = false;
    int shot_index = 0;
    constexpr int kShotW = 1920;
    constexpr int kShotH = 1080;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        // Development-only scripted input: drives the real screens when a
        // script has been dropped on the console (see app/dev_input).
        if (frames % 30 == 0)
            dev_input::poll();
        const dev_input::Command dev = dev_input::next();
        InputFrame input;
        if (dev.kind == dev_input::Kind::button)
        {
            input = dev.frame;
        }
        else if (dev.kind == dev_input::Kind::text)
        {
            (void)app.dev_type(dev.text);
            input.connected = true;
        }
        else if (dev.kind == dev_input::Kind::quit)
        {
            app.dev_quit();
            input.connected = true;
        }
        else if (dev.kind == dev_input::Kind::shot)
        {
            shot_pending = true;
            input.connected = true;
        }
        else
        {
            const std::size_t count = pad.read(samples);
            input = tracker.update(std::span<const PadSample>(samples, count),
                                   static_cast<std::uint64_t>(now));
        }

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
        if (shot_pending)
        {
            shot_pending = false;
            if (shot_canvas.texture() == 0)
                shot_canvas.create(kShotW, kShotH, 1);
            shot_canvas.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(shot_canvas.framebuffer(), kShotW, kShotH);
            glBindFramebuffer(GL_FRAMEBUFFER, shot_canvas.framebuffer());
            std::vector<unsigned char> px(static_cast<std::size_t>(kShotW) * kShotH * 4);
            glReadPixels(0, 0, kShotW, kShotH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            // glReadPixels is bottom-row-first; flip to top-row-first for PNG.
            std::vector<unsigned char> flip(px.size());
            for (int y = 0; y < kShotH; ++y)
                std::memcpy(&flip[static_cast<std::size_t>(y) * kShotW * 4],
                            &px[static_cast<std::size_t>(kShotH - 1 - y) * kShotW * 4],
                            static_cast<std::size_t>(kShotW) * 4);
            // Write into the app's own download0 (the sandbox download dir):
            // the build host reads it over FTP at
            // /mnt/sandbox/<TITLE>_NNN/download0/, which round-trips cleanly
            // even when the post-suspend /data union mount cannot. After
            // elevation the process root is the console's, so bare /download0
            // no longer resolves; derive it from the resolved app root
            // (".../app0" -> ".../download0").
            std::string shot_dir = "/download0";
            {
                const std::string &root = paths::app_root();
                const std::size_t slash = root.find_last_of('/');
                if (slash != std::string::npos && slash > 0)
                    shot_dir = root.substr(0, slash) + "/download0";
            }
            char shot_path[160];
            (void)std::snprintf(shot_path, sizeof(shot_path), "%s/shot-%d.png", shot_dir.c_str(),
                                shot_index++);
            const bool ok = fwd::write_png_file(shot_path, flip.data(), kShotW, kShotH);
            sys::log("[FWD] screenshot %s ok=%d", shot_path, ok ? 1 : 0);
            // Dev-only: when /data/ps5fwdgen-dev/upload.txt holds "host:port",
            // ship the raw RGBA to the build host over HTTP, which the broken
            // post-suspend union mount cannot round-trip through the FTP server.
            {
                std::string host;
                if (hui::save::read_file("/data/ps5fwdgen-dev/upload.txt", &host, 256u) &&
                    !host.empty())
                {
                    while (!host.empty() &&
                           (host.back() == '\n' || host.back() == '\r' || host.back() == ' '))
                        host.pop_back();
                    char url[160];
                    (void)std::snprintf(url, sizeof(url), "http://%s/shot?w=%d&h=%d&n=%d",
                                        host.c_str(), kShotW, kShotH, shot_index - 1);
                    const net::Response r =
                        net::post_bytes(url, flip, "application/octet-stream", 15000);
                    sys::log("[FWD] screenshot upload http=%ld curl=%d", r.status, r.curl_code);
                }
            }
            last_frame_start = sys::monotonic_us();
        }
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
