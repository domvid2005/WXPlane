#include <algorithm>
#include "png.h"

#include <cstdlib>
#include <cstring>

#include "inflate.h"

namespace wx {
namespace {
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}
} // namespace

bool decodePng(const uint8_t* d, size_t n, PngImage& out, size_t maxPixels) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 33 || std::memcmp(d, sig, 8) != 0) return false;
    int w = 0, h = 0, bd = 0, ct = 0, il = 0;
    std::vector<uint8_t> idat, plte, trns;
    size_t pos = 8;
    bool gotHdr = false, gotEnd = false;
    while (pos + 12 <= n && !gotEnd) {
        uint32_t len = be32(d + pos);
        const uint8_t* type = d + pos + 4;
        if ((size_t)len > n - pos - 12) return false;
        const uint8_t* body = d + pos + 8;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)be32(body); h = (int)be32(body + 4); bd = body[8]; ct = body[9]; il = body[12];
            gotHdr = true;
        } else if (!std::memcmp(type, "PLTE", 4)) plte.assign(body, body + len);
        else if (!std::memcmp(type, "tRNS", 4)) trns.assign(body, body + len);
        else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), body, body + len);
        else if (!std::memcmp(type, "IEND", 4)) gotEnd = true;
        pos += 12 + len;
    }
    if (!gotHdr || w <= 0 || h <= 0 || il != 0 || (size_t)w * (size_t)h > maxPixels) return false;
    int ch;
    switch (ct) { case 0: ch = 1; break; case 2: ch = 3; break; case 3: ch = 1; break; case 4: ch = 2; break; case 6: ch = 4; break; default: return false; }
    if (bd != 1 && bd != 2 && bd != 4 && bd != 8 && bd != 16) return false;
    if (bd < 8 && ct != 0 && ct != 3) return false;
    if (bd == 16 && ct == 3) return false;

    size_t bitsPerPixel = (size_t)ch * (size_t)bd;
    size_t stride = ((size_t)w * bitsPerPixel + 7) / 8;
    size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);
    std::vector<uint8_t> raw;
    if (!zlibDecompress(idat.data(), idat.size(), raw, (stride + 1) * (size_t)h + 16)) return false;
    if (raw.size() < (stride + 1) * (size_t)h) return false;

    std::vector<uint8_t> img(stride * (size_t)h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = raw.data() + (size_t)y * (stride + 1);
        int f = src[0];
        ++src;
        uint8_t* dst = img.data() + (size_t)y * stride;
        const uint8_t* up = y ? dst - stride : nullptr;
        for (size_t i = 0; i < stride; ++i) {
            int a = i >= bpp ? dst[i - bpp] : 0, b = up ? up[i] : 0, c = (up && i >= bpp) ? up[i - bpp] : 0, x = src[i], v;
            switch (f) {
                case 0: v = x; break;
                case 1: v = x + a; break;
                case 2: v = x + b; break;
                case 3: v = x + ((a + b) >> 1); break;
                case 4: v = x + paeth(a, b, c); break;
                default: return false;
            }
            dst[i] = (uint8_t)v;
        }
    }

    out = PngImage();
    out.w = w; out.h = h;
    if (bd < 8) {   // widen packed samples
        std::vector<uint8_t> wide((size_t)w * (size_t)h);
        int mask = (1 << bd) - 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t bit = (size_t)x * (size_t)bd;
                int v = (img[(size_t)y * stride + bit / 8] >> (8 - bd - (int)(bit % 8))) & mask;
                wide[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)v;
            }
        img.swap(wide);
        bd = 8;
    }
    if (ct == 3) {  // palette -> RGBA
        out.channels = 4; out.depth = 8;
        out.px.resize((size_t)w * (size_t)h * 4);
        for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
            size_t idx = img[i];
            uint8_t* o = &out.px[i * 4];
            if (idx * 3 + 2 < plte.size()) { o[0] = plte[idx * 3]; o[1] = plte[idx * 3 + 1]; o[2] = plte[idx * 3 + 2]; }
            else { o[0] = o[1] = o[2] = 0; }
            o[3] = idx < trns.size() ? trns[idx] : 255;
        }
        return true;
    }
    out.channels = ch; out.depth = bd;
    out.px.swap(img);
    return true;
}

} // namespace wx
