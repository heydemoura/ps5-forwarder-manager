// image.hpp - image helpers for Forwarder Manager.
//
// Ports two image encoders from the ps5-forwarder.mph.am website JavaScript
// to C++20:
//   - makeIcon()      -> make_icon_png() / make_icon_png_rgba()
//   - backgroundRgba()+backgroundDds() -> make_background_dds() /
//     make_background_dds_rgba()
//
// The DDS path contains a faithful transcription of the website's hand-rolled
// BC7 (mode 6) block encoder so that the produced .dds files are bit-identical
// to the ones the website generates for the same RGBA input; the PS5 shell
// reads these files.
//
// No exceptions, no RTTI. Allocation failure aborts (std::vector under
// -fno-exceptions), which is acceptable for this tool.

#ifndef FWD_IMAGE_HPP
#define FWD_IMAGE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fwd {

// Decode any common image (PNG/JPG/BMP/TGA/GIF/PSD) from a memory buffer to
// RGBA8. Returns false on failure. On success out_rgba has w*h*4 bytes.
bool decode_image(const unsigned char *data, std::size_t size,
                  int &w, int &h, std::vector<unsigned char> &out_rgba);

// Center-crop to a square (min dimension), resize to 512x512, encode PNG.
// Mirrors the website's makeIcon(). Input is an encoded image file in memory.
// Returns the PNG bytes, empty on failure.
std::vector<unsigned char> make_icon_png(const unsigned char *data,
                                         std::size_t size);

// Also accept already-decoded RGBA:
std::vector<unsigned char> make_icon_png_rgba(const unsigned char *rgba, int w,
                                              int h);

// Scale an image to COVER 1920x1080 (center-crop overflow), encode as a
// BC7 (mode 6) DDS, as the website's backgroundRgba()+backgroundDds() does.
// Capped at 1080p (down from the site's 4K) to stay within the 128 MB app
// heap; 4K peaked too high and crashed on device. Empty on failure.
std::vector<unsigned char> make_background_dds(const unsigned char *data,
                                               std::size_t size);
std::vector<unsigned char> make_background_dds_rgba(const unsigned char *rgba,
                                                    int w, int h);

// Same, but to an explicit output size. The runtime forwarder path uses the
// 1080p default above (heap-bound); the app's own build-time sce_sys
// backgrounds are encoded at full 4K through these.
std::vector<unsigned char> make_background_dds_sized(const unsigned char *data,
                                                     std::size_t size, int out_w, int out_h);
std::vector<unsigned char> make_background_dds_rgba_sized(const unsigned char *rgba, int w, int h,
                                                         int out_w, int out_h);

// Decode a BC7 DDS written by make_background_dds (DX10 header, mode 6
// blocks only, which is all the website's and this app's encoder emit) back
// to RGBA, for the edit screen's live preview. When the image is wider than
// max_width it is subsampled 2x on both axes while decoding, so a 4K file
// never needs a 33 MB buffer on the app heap. Returns false for anything but
// a mode-6 BC7 DX10 DDS.
bool decode_bc7_dds(const unsigned char *dds, std::size_t size, int &w, int &h,
                    std::vector<unsigned char> &out_rgba, int max_width = 1920);

// The target dimensions, for callers.
bool write_png_file(const char *path, const unsigned char *rgba, int w, int h);

constexpr int kIconSize = 512;
constexpr int kBackgroundWidth = 1920;
constexpr int kBackgroundHeight = 1080;

}  // namespace fwd

#endif  // FWD_IMAGE_HPP
