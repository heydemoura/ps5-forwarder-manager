// ps5fwdgen - Decode a forwarder's selection music (RIFF/ATRAC9) to PCM.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console only ships hardware ATRAC9 decoding behind an undocumented
// ABI, so the edit screen's live preview decodes snd0.at9 in software with
// LibAtrac9 (src/third_party/libatrac9). A whole file is decoded up front;
// selection music is at most 87 s, typically 15 s.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fwd::at9
{

struct Decoded
{
    std::vector<std::int16_t> pcm; // interleaved, `channels` per frame
    int channels = 0;
    int sample_rate = 0;
    std::size_t frames = 0; // per channel
    std::string error;      // "" on success
    bool ok() const
    {
        return error.empty();
    }
};

// data is a RIFF WAVE file whose fmt chunk is WAVE_FORMAT_EXTENSIBLE with the
// ATRAC9 sub-format (what the website's converter and the console write).
Decoded decode(const unsigned char *data, std::size_t size);

} // namespace fwd::at9
