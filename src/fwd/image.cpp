// image.cpp - see image.hpp.
//
// The BC7 (mode 6) encoder and the DDS (DX10) header writer below are a direct
// port of the encoder used by https://ps5-forwarder.mph.am (its bundled module
// 1212). The integer math is transcribed verbatim from the JavaScript so the
// output is bit-identical for the same RGBA input: the same endpoint fit
// (`s`), the same `(x+1)>>1` rounding and clamp to 0..127, the same
// `2*i+parity` reconstruction, the same interpolation table, the same
// endpoint-swap-when-first-index-high-bit rule, and the same LSB-first bit
// packing.

#include "fwd/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// The build compiles third_party with -w, but this translation unit is built
// with -Wall -Wextra, so silence stb's warnings around the implementation
// includes. The include paths resolve from the repo root (the build passes
// -Isrc and uses the repo root as an include dir), hence the "third_party/..."
// spelling.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "third_party/stb/stb_image.h"
#include "third_party/stb/stb_image_write.h"
#include "third_party/stb/stb_image_resize2.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fwd {
namespace {

// ---------------------------------------------------------------------------
// BC7 mode-6 encoder (port of module 1212's `i`, `s` and the inner packer).
// ---------------------------------------------------------------------------

// 4-bit interpolation weights (JS: `a=[0,4,9,...,64]`).
constexpr int kWeights[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                              34, 38, 43, 47, 51, 56, 60, 64};

// Port of `s(A,e,t)`: given 4 input channel values `in`, pick the parity (p-bit)
// that minimizes reconstruction error, write the 7-bit endpoint values into
// `ep7` and the reconstructed 8-bit values into `recon8`. Returns the parity.
int fit_endpoint(const int in[4], int ep7[4], int recon8[4]) {
    int parity = 0;
    long long best = -1;  // stands in for JS `1/0` (+Infinity)
    for (int e = 0; e <= 1; ++e) {
        long long err = 0;
        for (int c = 0; c < 4; ++c) {
            int s = (in[c] - e + 1) >> 1;
            if (s < 0) s = 0;
            if (s > 127) s = 127;
            err += std::abs(2 * s + e - in[c]);
        }
        if (best < 0 || err < best) {
            best = err;
            parity = e;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int v = (in[c] - parity + 1) >> 1;
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        ep7[c] = v;
        recon8[c] = 2 * v + parity;
    }
    return parity;
}

// Port of the inner anonymous packer. `texels` holds 16 RGBA texels (64 ints),
// `out` receives the 16-byte BC7 block.
void encode_block(const int texels[64], unsigned char out[16]) {
    int mn[4] = {255, 255, 255, 255};
    int mx[4] = {0, 0, 0, 0};
    for (int t = 0; t < 16; ++t) {
        for (int c = 0; c < 4; ++c) {
            int a = texels[4 * t + c];
            if (a < mn[c]) mn[c] = a;
            if (a > mx[c]) mx[c] = a;
        }
    }

    // r/oo: 7-bit endpoints; l/cc: reconstructed 8-bit endpoints.
    int r[4], l[4], oo[4], cc[4];
    int d = fit_endpoint(mn, r, l);
    int g = fit_endpoint(mx, oo, cc);

    // Interpolated palette: 16 entries (one per weight).
    int h[64];
    for (int A = 0; A < 16; ++A) {
        int w = kWeights[A];
        for (int c = 0; c < 4; ++c) {
            h[4 * A + c] = (l[c] * (64 - w) + cc[c] * w + 32) >> 6;
        }
    }

    // Choose the nearest palette entry for each texel.
    int B[16];
    for (int t = 0; t < 16; ++t) {
        int best = 0;
        long long bestd = -1;
        for (int s = 0; s < 16; ++s) {
            long long dist = 0;
            for (int c = 0; c < 4; ++c) {
                int diff = texels[4 * t + c] - h[4 * s + c];
                dist += static_cast<long long>(diff) * diff;
            }
            if (bestd < 0 || dist < bestd) {
                bestd = dist;
                best = s;
            }
        }
        B[t] = best;
    }

    // BC7 mode 6 requires the first index's high bit to be 0; if not, invert
    // all indices and swap the endpoints (and their parities).
    if (B[0] >= 8) {
        for (int i = 0; i < 16; ++i) B[i] = 15 - B[i];
        for (int i = 0; i < 4; ++i) {
            int tmp = r[i];
            r[i] = oo[i];
            oo[i] = tmp;
        }
        int tmp = d;
        d = g;
        g = tmp;
    }

    for (int i = 0; i < 16; ++i) out[i] = 0;

    int bitpos = 0;
    auto u = [&](int value, int bits) {
        for (int s = 0; s < bits; ++s) {
            if ((value >> s) & 1) {
                out[(bitpos + s) >> 3] |=
                    static_cast<unsigned char>(1 << ((bitpos + s) & 7));
            }
        }
        bitpos += bits;
    };

    u(64, 7);  // mode-6 header (7 bits = 0b1000000)
    for (int A = 0; A < 4; ++A) {
        u(r[A], 7);
        u(oo[A], 7);
    }
    u(d, 1);
    u(g, 1);
    u(B[0], 3);
    for (int A = 1; A < 16; ++A) u(B[A], 4);
}

// Port of `i(A,w,h)`: gather 4x4 blocks row-major and pack each to BC7.
std::vector<unsigned char> bc7_encode(const unsigned char *rgba, int w, int h) {
    int nbx = w >> 2;
    int nby = h >> 2;
    std::vector<unsigned char> blocks(static_cast<std::size_t>(nbx) * nby * 16);
    int texels[64];
    std::size_t c = 0;
    for (int by = 0; by < nby; ++by) {
        for (int bx = 0; bx < nbx; ++bx) {
            for (int a = 0; a < 4; ++a) {
                std::size_t base =
                    (static_cast<std::size_t>(4 * by + a) * w + 4 * bx) * 4;
                for (int e = 0; e < 4; ++e) {
                    std::size_t src = base + static_cast<std::size_t>(4 * e);
                    int idx = (4 * a + e) * 4;
                    texels[idx] = rgba[src];
                    texels[idx + 1] = rgba[src + 1];
                    texels[idx + 2] = rgba[src + 2];
                    texels[idx + 3] = rgba[src + 3];
                }
            }
            encode_block(texels, blocks.data() + c);
            c += 16;
        }
    }
    return blocks;
}

// Port of `l(...)`: wrap BC7 blocks in a 148-byte DDS (DX10) header.
void put_u32(std::vector<unsigned char> &v, std::size_t off, std::uint32_t val) {
    v[off + 0] = static_cast<unsigned char>(val & 0xff);
    v[off + 1] = static_cast<unsigned char>((val >> 8) & 0xff);
    v[off + 2] = static_cast<unsigned char>((val >> 16) & 0xff);
    v[off + 3] = static_cast<unsigned char>((val >> 24) & 0xff);
}

std::vector<unsigned char> wrap_dds(const std::vector<unsigned char> &blocks) {
    std::vector<unsigned char> out(148 + blocks.size(), 0);
    out[0] = 68;  // 'D'
    out[1] = 68;  // 'D'
    out[2] = 83;  // 'S'
    out[3] = 32;  // ' '
    put_u32(out, 4, 124);     // header size
    put_u32(out, 8, 528391);  // DDSD flags (verbatim from JS)
    put_u32(out, 12, static_cast<std::uint32_t>(kBackgroundHeight));
    put_u32(out, 16, static_cast<std::uint32_t>(kBackgroundWidth));
    put_u32(out, 20, static_cast<std::uint32_t>(blocks.size()));  // linearSize
    put_u32(out, 28, 1);   // mipCount
    put_u32(out, 76, 32);  // pixelformat size
    put_u32(out, 80, 4);   // DDPF_FOURCC
    out[84] = 68;          // 'D'
    out[85] = 88;          // 'X'
    out[86] = 49;          // '1'
    out[87] = 48;          // '0'
    put_u32(out, 108, 4096);  // DDSCAPS_TEXTURE
    // DX10 header at offset 128.
    put_u32(out, 128, 98);  // DXGI_FORMAT_BC7_UNORM
    put_u32(out, 132, 3);   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    put_u32(out, 136, 0);   // miscFlag
    put_u32(out, 140, 1);   // arraySize
    put_u32(out, 144, 0);   // miscFlags2
    std::memcpy(out.data() + 148, blocks.data(), blocks.size());
    return out;
}

// stb_image_write sink into a std::vector.
void png_write_cb(void *ctx, void *data, int size) {
    auto *v = static_cast<std::vector<unsigned char> *>(ctx);
    const unsigned char *p = static_cast<const unsigned char *>(data);
    v->insert(v->end(), p, p + size);
}

}  // namespace

bool decode_image(const unsigned char *data, std::size_t size, int &w, int &h,
                  std::vector<unsigned char> &out_rgba) {
    if (data == nullptr || size == 0) return false;
    int comp = 0;
    unsigned char *px = stbi_load_from_memory(
        data, static_cast<int>(size), &w, &h, &comp, 4);
    if (px == nullptr) return false;
    out_rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

std::vector<unsigned char> make_icon_png_rgba(const unsigned char *rgba, int w,
                                              int h) {
    if (rgba == nullptr || w <= 0 || h <= 0) return {};

    // Center-crop to a square of side = min(w, h).
    int side = std::min(w, h);
    int ox = (w - side) / 2;
    int oy = (h - side) / 2;
    std::vector<unsigned char> square(static_cast<std::size_t>(side) * side * 4);
    for (int y = 0; y < side; ++y) {
        const unsigned char *srow =
            rgba + (static_cast<std::size_t>(y + oy) * w + ox) * 4;
        std::memcpy(square.data() + static_cast<std::size_t>(y) * side * 4,
                    srow, static_cast<std::size_t>(side) * 4);
    }

    // Resize to kIconSize x kIconSize (sRGB-aware, high quality).
    std::vector<unsigned char> icon(
        static_cast<std::size_t>(kIconSize) * kIconSize * 4);
    if (stbir_resize_uint8_srgb(square.data(), side, side, 0, icon.data(),
                                kIconSize, kIconSize, 0, STBIR_RGBA) == nullptr) {
        return {};
    }

    std::vector<unsigned char> out;
    if (stbi_write_png_to_func(png_write_cb, &out, kIconSize, kIconSize, 4,
                               icon.data(), kIconSize * 4) == 0) {
        return {};
    }
    return out;
}

std::vector<unsigned char> make_icon_png(const unsigned char *data,
                                         std::size_t size) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!decode_image(data, size, w, h, rgba)) return {};
    return make_icon_png_rgba(rgba.data(), w, h);
}

std::vector<unsigned char> make_background_dds_rgba(const unsigned char *rgba,
                                                    int w, int h) {
    if (rgba == nullptr || w <= 0 || h <= 0) return {};

    // backgroundRgba(): COVER scale factor so the image fills the 3840x2160
    // canvas, then center-crop the overflow.
    double t = std::max(static_cast<double>(kBackgroundWidth) / w,
                        static_cast<double>(kBackgroundHeight) / h);
    long sw = std::lround(w * t);
    long sh = std::lround(h * t);
    if (sw < kBackgroundWidth) sw = kBackgroundWidth;
    if (sh < kBackgroundHeight) sh = kBackgroundHeight;

    std::vector<unsigned char> scaled(
        static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 4);
    if (stbir_resize_uint8_srgb(rgba, w, h, 0, scaled.data(),
                                static_cast<int>(sw), static_cast<int>(sh), 0,
                                STBIR_RGBA) == nullptr) {
        return {};
    }

    // Center-crop to exactly 3840x2160.
    std::vector<unsigned char> canvas(
        static_cast<std::size_t>(kBackgroundWidth) * kBackgroundHeight * 4);
    int ox = static_cast<int>((sw - kBackgroundWidth) / 2);
    int oy = static_cast<int>((sh - kBackgroundHeight) / 2);
    for (int y = 0; y < kBackgroundHeight; ++y) {
        const unsigned char *srow =
            scaled.data() +
            (static_cast<std::size_t>(y + oy) * static_cast<std::size_t>(sw) +
             ox) * 4;
        unsigned char *drow =
            canvas.data() +
            static_cast<std::size_t>(y) * kBackgroundWidth * 4;
        std::memcpy(drow, srow, static_cast<std::size_t>(kBackgroundWidth) * 4);
    }

    std::vector<unsigned char> blocks =
        bc7_encode(canvas.data(), kBackgroundWidth, kBackgroundHeight);
    return wrap_dds(blocks);
}

std::vector<unsigned char> make_background_dds(const unsigned char *data,
                                               std::size_t size) {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!decode_image(data, size, w, h, rgba)) return {};
    return make_background_dds_rgba(rgba.data(), w, h);
}

}  // namespace fwd
