// ps5fwdgen - Decode a forwarder's selection music (RIFF/ATRAC9) to PCM.
// Copyright (C) 2026 heydemoura
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/at9.hpp"

#include "third_party/libatrac9/libatrac9.h"

#include <cstring>

namespace fwd::at9
{

namespace
{

std::uint32_t u32(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t u16(const unsigned char *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// The ATRAC9 sub-format GUID, {47E142D2-36BA-4D8D-88FC-61654F8C836C}, as the
// bytes it is stored as.
constexpr unsigned char kAt9Guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                        0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

struct Riff
{
    const unsigned char *fmt = nullptr;
    std::size_t fmt_size = 0;
    const unsigned char *fact = nullptr;
    std::size_t fact_size = 0;
    const unsigned char *data = nullptr;
    std::size_t data_size = 0;
};

bool parse_riff(const unsigned char *d, std::size_t n, Riff &out)
{
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0)
        return false;
    std::size_t p = 12;
    while (p + 8 <= n)
    {
        const std::size_t size = u32(d + p + 4);
        const unsigned char *body = d + p + 8;
        if (size > n - p - 8)
            return false;
        if (std::memcmp(d + p, "fmt ", 4) == 0)
        {
            out.fmt = body;
            out.fmt_size = size;
        }
        else if (std::memcmp(d + p, "fact", 4) == 0)
        {
            out.fact = body;
            out.fact_size = size;
        }
        else if (std::memcmp(d + p, "data", 4) == 0)
        {
            out.data = body;
            out.data_size = size;
            break;
        }
        p += 8 + size + (size & 1);
    }
    return out.fmt != nullptr && out.data != nullptr;
}

} // namespace

Decoded decode(const unsigned char *data, std::size_t size)
{
    Decoded result;
    Riff riff;
    if (!parse_riff(data, size, riff))
    {
        result.error = "not a RIFF WAVE file";
        return result;
    }
    // WAVE_FORMAT_EXTENSIBLE (0xFFFE) with the ATRAC9 sub-format; the 4-byte
    // codec config follows the GUID and a version word.
    if (riff.fmt_size < 52 || u16(riff.fmt) != 0xfffe ||
        std::memcmp(riff.fmt + 24, kAt9Guid, 16) != 0)
    {
        result.error = "not an ATRAC9 WAVE";
        return result;
    }
    const int channels = u16(riff.fmt + 2);
    const int sample_rate = static_cast<int>(u32(riff.fmt + 4));
    unsigned char config[ATRAC9_CONFIG_DATA_SIZE];
    std::memcpy(config, riff.fmt + 44, sizeof(config));

    void *handle = Atrac9GetHandle();
    if (handle == nullptr)
    {
        result.error = "decoder allocation failed";
        return result;
    }
    if (Atrac9InitDecoder(handle, config) != 0)
    {
        Atrac9ReleaseHandle(handle);
        result.error = "bad ATRAC9 config";
        return result;
    }
    Atrac9CodecInfo info{};
    if (Atrac9GetCodecInfo(handle, &info) != 0 || info.channels <= 0 || info.superframeSize <= 0 ||
        info.framesInSuperframe <= 0 || info.frameSamples <= 0)
    {
        Atrac9ReleaseHandle(handle);
        result.error = "ATRAC9 codec info unavailable";
        return result;
    }

    std::size_t total_frames = 0;
    std::size_t encoder_delay = 0;
    if (riff.fact != nullptr && riff.fact_size >= 12)
    {
        total_frames = u32(riff.fact);
        encoder_delay = u32(riff.fact + 8);
    }

    const std::size_t superframes = riff.data_size / static_cast<std::size_t>(info.superframeSize);
    const std::size_t frame_pcm =
        static_cast<std::size_t>(info.frameSamples) * static_cast<std::size_t>(info.channels);
    std::vector<std::int16_t> pcm;
    pcm.reserve(superframes * static_cast<std::size_t>(info.framesInSuperframe) * frame_pcm);
    std::vector<std::int16_t> frame(frame_pcm);
    bool broken = false;
    for (std::size_t s = 0; s < superframes && !broken; ++s)
    {
        const unsigned char *p = riff.data + s * static_cast<std::size_t>(info.superframeSize);
        const unsigned char *end = p + info.superframeSize;
        for (int f = 0; f < info.framesInSuperframe; ++f)
        {
            int used = 0;
            if (p >= end || Atrac9Decode(handle, p, frame.data(), &used) != 0 || used <= 0)
            {
                broken = true;
                break;
            }
            pcm.insert(pcm.end(), frame.begin(), frame.end());
            p += used;
        }
    }
    Atrac9ReleaseHandle(handle);

    if (pcm.empty())
    {
        result.error = "no decodable ATRAC9 frames";
        return result;
    }
    // Drop the encoder's pre-roll and anything past the declared length.
    const std::size_t stride = static_cast<std::size_t>(info.channels);
    std::size_t frames = pcm.size() / stride;
    const std::size_t skip = encoder_delay < frames ? encoder_delay : 0;
    if (skip > 0)
    {
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(skip * stride));
        frames -= skip;
    }
    if (total_frames > 0 && total_frames < frames)
    {
        frames = total_frames;
        pcm.resize(frames * stride);
    }
    result.pcm = std::move(pcm);
    result.channels = info.channels;
    result.sample_rate = sample_rate > 0 ? sample_rate : info.samplingRate;
    result.frames = frames;
    (void)channels;
    return result;
}

} // namespace fwd::at9
