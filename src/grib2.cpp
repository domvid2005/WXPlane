#include "grib2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "png.h"

namespace wx {
namespace {

std::time_t utcToTime(int y, int mo, int d, int h, int mi, int sec) {
    // days from civil (Howard Hinnant's algorithm), avoids timegm portability problems
    y -= mo <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return (std::time_t)(days * 86400L + h * 3600L + mi * 60L + sec);
}

uint32_t u32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint16_t u16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
// GRIB stores signed values as sign + magnitude.
int32_t s32(const uint8_t* p) { uint32_t v = u32(p); return (v & 0x80000000u) ? -(int32_t)(v & 0x7fffffffu) : (int32_t)v; }
int s16(const uint8_t* p) { int v = u16(p); return (v & 0x8000) ? -(v & 0x7fff) : v; }
float f32(const uint8_t* p) { uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }
const float kNaN = std::numeric_limits<float>::quiet_NaN();

double normLon(double lon) { while (lon > 180) lon -= 360; while (lon < -180) lon += 360; return lon; }

} // namespace

float GribField::at(double lat, double lon) const {
    if (!valid()) return kNaN;
    long j = std::lround((lat0 - lat) / dLat), i = std::lround((normLon(lon) - lon0) / dLon);
    if (i < 0 || j < 0 || i >= ni || j >= nj) return kNaN;
    return v[(size_t)j * (size_t)ni + (size_t)i];
}

bool decodeGrib2(const uint8_t* d, size_t n, const GribWindow& win, GribField& out) {
    out = GribField();
    if (n < 16 || std::memcmp(d, "GRIB", 4) != 0 || d[7] != 2) { out.error = "not a GRIB2 message"; return false; }
    out.discipline = d[6];
    uint64_t total = ((uint64_t)u32(d + 8) << 32) | u32(d + 12);
    size_t end = (total > 0 && total <= n) ? (size_t)total : n;

    int gi = 0, gj = 0, scan = 0, tmpl3 = -1, tmpl5 = -1, nbits = 0, bmInd = 255;
    double la1 = 0, lo1 = 0, di = 0, dj = 0;
    double R = 0, E = 0, D = 0;
    size_t npts = 0;
    const uint8_t* bitmap = nullptr;
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;

    size_t pos = 16;
    while (pos + 5 <= end) {
        if (std::memcmp(d + pos, "7777", 4) == 0) break;
        uint32_t len = u32(d + pos);
        int num = d[pos + 4];
        if (len < 5 || pos + len > end) { out.error = "truncated section"; return false; }
        const uint8_t* s = d + pos;
        if (num == 1 && len >= 19) {
            out.refTime = utcToTime(u16(s + 12), s[14], s[15], s[16], s[17], s[18]);
        } else if (num == 3 && len >= 72) {
            tmpl3 = u16(s + 12);
            if (tmpl3 != 0) { out.error = "unsupported grid template " + std::to_string(tmpl3); return false; }
            gi = (int)u32(s + 30); gj = (int)u32(s + 34);
            uint32_t basic = u32(s + 38), sub = u32(s + 42);
            double unit = (basic == 0 || sub == 0 || sub == 0xFFFFFFFFu) ? 1e-6 : (double)basic / (double)sub;
            la1 = s32(s + 46) * unit; lo1 = s32(s + 50) * unit;
            di = u32(s + 63) * unit; dj = u32(s + 67) * unit;
            scan = s[71];
        } else if (num == 5 && len >= 21) {
            npts = u32(s + 5);
            tmpl5 = u16(s + 9);
            R = f32(s + 11); E = s16(s + 15); D = s16(s + 17); nbits = s[19];
            if (tmpl5 != 0 && tmpl5 != 41) { out.error = "unsupported data packing template 5." + std::to_string(tmpl5) + " (need 0 or 41)"; return false; }
        } else if (num == 6 && len >= 6) {
            bmInd = s[5];
            if (bmInd == 0) bitmap = s + 6;
            else if (bmInd != 255) { out.error = "unsupported bitmap"; return false; }
        } else if (num == 7) {
            payload = s + 5; payloadLen = len - 5;
        }
        pos += len;
    }
    if (tmpl3 < 0 || tmpl5 < 0 || !payload || gi <= 0 || gj <= 0 || di <= 0 || dj <= 0) { out.error = "missing sections"; return false; }
    size_t total_pts = (size_t)gi * (size_t)gj;
    if (npts > total_pts) { out.error = "inconsistent point count"; return false; }

    // Values are stored for the points the bitmap marks present (all of them without a bitmap).
    PngImage png;
    size_t bpp = 0;
    if (tmpl5 == 41) {
        if (!decodePng(payload, payloadLen, png, 100000000)) { out.error = "PNG-packed data would not decode"; return false; }
        if ((size_t)png.w * (size_t)png.h < npts) { out.error = "PNG smaller than point count"; return false; }
        bpp = png.bytesPerPixel();
    } else if (nbits > 32) { out.error = "unsupported bit width"; return false; }

    const double scaleE = std::pow(2.0, E), scaleD = std::pow(10.0, D);
    auto packed = [&](size_t k) -> double {   // k-th stored value, unpacked
        double X = 0;
        if (nbits == 0) X = 0;
        else if (tmpl5 == 41) {
            const uint8_t* p = &png.px[k * bpp];
            for (size_t b = 0; b < bpp; ++b) X = X * 256.0 + p[b];
        } else {
            size_t bit = k * (size_t)nbits, byte = bit / 8;
            int off = (int)(bit % 8);
            uint64_t acc = 0;
            int need = (off + nbits + 7) / 8;
            for (int b = 0; b < need; ++b) acc = (acc << 8) | (byte + (size_t)b < payloadLen ? payload[byte + (size_t)b] : 0);
            acc >>= (need * 8 - off - nbits);
            X = (double)(acc & ((nbits == 32) ? 0xFFFFFFFFull : ((1ull << nbits) - 1)));
        }
        return (R + X * scaleE) / scaleD;
    };
    if (tmpl5 == 0 && nbits > 0 && (npts * (size_t)nbits + 7) / 8 > payloadLen) { out.error = "data section too short"; return false; }

    // Geometry: row j has latitude la1 -/+ j*dj, column i has longitude lo1 +/- i*di (scan-mode dependent).
    const bool iNeg = (scan & 0x80) != 0, jPos = (scan & 0x40) != 0, jFast = (scan & 0x20) != 0;
    auto latOf = [&](int j) { return la1 + (jPos ? 1 : -1) * j * dj; };
    auto lonOf = [&](int i) { return normLon(lo1 + (iNeg ? -1 : 1) * i * di); };
    int iMin = gi, iMax = -1, jMin = gj, jMax = -1;
    for (int j = 0; j < gj; ++j) { double la = latOf(j); if (la >= win.latMin - dj && la <= win.latMax + dj) { jMin = std::min(jMin, j); jMax = std::max(jMax, j); } }
    for (int i = 0; i < gi; ++i) { double lo = lonOf(i); if (lo >= win.lonMin - di && lo <= win.lonMax + di) { iMin = std::min(iMin, i); iMax = std::max(iMax, i); } }
    if (iMax < iMin || jMax < jMin) { out.error = "window outside this product's coverage"; return false; }
    // The window must map to a contiguous run of columns (i.e. not straddle the grid's wrap-around).
    for (int i = iMin; i < iMax; ++i) {
        double a = lonOf(i), b = lonOf(i + 1);
        if (iNeg ? b > a : b < a) { out.error = "window straddles the grid seam"; return false; }
    }

    // Number of stored values before each point is needed only when a bitmap thins the data.
    std::vector<uint32_t> before;
    if (bitmap) {
        before.assign(total_pts + 1, 0);
        for (size_t k = 0; k < total_pts; ++k)
            before[k + 1] = before[k] + ((bitmap[k / 8] >> (7 - (k % 8))) & 1);
    }
    auto valueAt = [&](int i, int j) -> float {
        size_t k = jFast ? (size_t)i * (size_t)gj + (size_t)j : (size_t)j * (size_t)gi + (size_t)i;   // storage order
        if (bitmap) {
            if (!((bitmap[k / 8] >> (7 - (k % 8))) & 1)) return kNaN;
            k = before[k];
        }
        return k < npts ? (float)packed(k) : kNaN;
    };

    // Output is normalised to rows north -> south, columns west -> east.
    int wi = iMax - iMin + 1, wj = jMax - jMin + 1;
    out.ni = wi; out.nj = wj; out.dLat = dj; out.dLon = di;
    out.v.assign((size_t)wi * (size_t)wj, kNaN);
    bool rowsN2S = !jPos, colsW2E = !iNeg;
    out.lat0 = rowsN2S ? latOf(jMin) : latOf(jMax);
    out.lon0 = colsW2E ? lonOf(iMin) : lonOf(iMax);
    for (int r = 0; r < wj; ++r) {
        int j = rowsN2S ? jMin + r : jMax - r;
        for (int c = 0; c < wi; ++c) {
            int i = colsW2E ? iMin + c : iMax - c;
            out.v[(size_t)r * (size_t)wi + (size_t)c] = valueAt(i, j);
        }
    }
    return true;
}

} // namespace wx
