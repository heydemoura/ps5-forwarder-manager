// ps5fwdgen - Plays a forwarder's selection music on a mixer music deck, for
// the edit screen's live preview.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Owned by the App (so the rings outlive any screen; the audio thread keeps
// reading a ring for a few grains after a swap). Decoded PCM is kept as
// 48 kHz stereo S16 and fed to the mixer about a second ahead, looping, the
// way the console loops snd0.at9 on the home screen.
#pragma once

#include "audio/at9.hpp"
#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fwd
{

class PreviewMusic
{
  public:
    // The mixer's audio thread may already be running; the deck is attached
    // with a thread-safe swap.
    void init(hui::audio::Mixer &mixer);

    // Starts looping the clip (any channel count / rate; converted here).
    // Fades the previous clip out. False when the clip is unusable.
    bool play(const at9::Decoded &clip);
    // Fades out and stops feeding the deck.
    void stop();
    // Once per frame, from the main thread.
    void pump();

    bool playing() const
    {
        return playing_;
    }

  private:
    static constexpr std::size_t kSlot = 1; // deck 0 is left for app music
    static constexpr std::size_t kRingFrames = 1u << 16;

    hui::audio::Mixer *mixer_ = nullptr;
    // Two rings so a new clip starts on an empty one while the audio thread
    // finishes with the other.
    std::array<hui::audio::StreamRing, 2> rings_{hui::audio::StreamRing{kRingFrames},
                                                 hui::audio::StreamRing{kRingFrames}};
    std::size_t active_ring_ = 0;
    std::vector<std::int16_t> pcm_; // stereo 48 kHz
    std::size_t frames_ = 0;
    std::size_t position_ = 0;
    bool playing_ = false;
    std::vector<float> scratch_;
};

} // namespace fwd
