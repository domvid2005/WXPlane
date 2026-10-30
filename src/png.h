// Minimal PNG decoder (non-interlaced), enough for GRIB2 PNG-packed data and radar map tiles.
#pragma once
#include <cstdint>
#include <vector>

namespace wx {

struct PngImage {
    int w = 0, h = 0;
    int channels = 0;        // samples per pixel after decoding (palette images become RGBA = 4)
    int depth = 8;           // bits per sample: 8 or 16 (sub-byte samples are widened to 8 without scaling)
    std::vector<uint8_t> px; // rows tightly packed, big-endian 16-bit samples
    size_t bytesPerPixel() const { return (size_t)channels * (size_t)(depth / 8); }
};

bool decodePng(const uint8_t* data, size_t n, PngImage& out, size_t maxPixels = 40000000);

} // namespace wx
