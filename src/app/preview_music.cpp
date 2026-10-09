// ps5fwdgen - Plays a forwarder's selection music on a mixer music deck.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/preview_music.hpp"

#include <algorithm>

namespace fwd
{

void PreviewMusic::init(hui::audio::Mixer &mixer)
{
    mixer_ = &mixer;
    mixer_->swap_stream(kSlot, &rings_[active_ring_]);
    mixer_->set_stream_gain(kSlot, 0.0f, 0.0f);
}

bool PreviewMusic::play(const at9::Decoded &clip)
{
    if (mixer_ == nullptr || !clip.ok() || clip.frames == 0 || clip.channels <= 0 ||
        clip.sample_rate <= 0)
        return false;

    // To 48 kHz stereo S16: duplicate mono, drop extra channels, and resample
    // linearly when the rate differs (the website converts at 48 kHz, so this
    // is a fallback).
    const std::size_t in_frames = clip.frames;
    const int in_channels = clip.channels;
    const std::size_t out_frames =
        clip.sample_rate == hui::audio::kSampleRate
            ? in_frames
            : static_cast<std::size_t>(static_cast<double>(in_frames) * hui::audio::kSampleRate /
                                       clip.sample_rate);
    if (out_frames == 0)
        return false;
    std::vector<std::int16_t> pcm(out_frames * 2);
    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (std::size_t i = 0; i < out_frames; ++i)
    {
        const double src = static_cast<double>(i) * step;
        const std::size_t a = std::min(static_cast<std::size_t>(src), in_frames - 1);
        const std::size_t b = std::min(a + 1, in_frames - 1);
        const float t = static_cast<float>(src - static_cast<double>(a));
        for (int ch = 0; ch < 2; ++ch)
        {
            const int sc = in_channels == 1 ? 0 : std::min(ch, in_channels - 1);
            const float sa = clip.pcm[a * static_cast<std::size_t>(in_channels) + sc];
            const float sb = clip.pcm[b * static_cast<std::size_t>(in_channels) + sc];
            pcm[i * 2 + ch] = static_cast<std::int16_t>(sa + (sb - sa) * t);
        }
    }

    // Start on the other, empty ring so no tail of the previous clip plays.
    active_ring_ = (active_ring_ + 1) % rings_.size();
    pcm_ = std::move(pcm);
    frames_ = out_frames;
    position_ = 0;
    playing_ = true;
    pump(); // prime before the deck becomes audible
    mixer_->swap_stream(kSlot, &rings_[active_ring_]);
    mixer_->set_stream_gain(kSlot, 1.0f, 0.5f);
    return true;
}

void PreviewMusic::stop()
{
    if (mixer_ == nullptr || !playing_)
        return;
    mixer_->set_stream_gain(kSlot, 0.0f, 0.3f);
    playing_ = false;
    position_ = 0;
}

void PreviewMusic::pump()
{
    if (!playing_ || frames_ == 0)
        return;
    hui::audio::StreamRing &ring = rings_[active_ring_];
    std::size_t space = ring.space();
    while (space > 0)
    {
        const std::size_t chunk = std::min<std::size_t>({space, 2048u, frames_ - position_});
        scratch_.resize(chunk * 2);
        for (std::size_t i = 0; i < chunk * 2; ++i)
            scratch_[i] = static_cast<float>(pcm_[(position_ * 2) + i]) / 32768.0f;
        const std::size_t written = ring.write(scratch_.data(), chunk);
        if (written == 0)
            break;
        position_ += written;
        if (position_ >= frames_)
            position_ = 0; // loop, like the console's selection music
        space -= written;
    }
}

} // namespace fwd
