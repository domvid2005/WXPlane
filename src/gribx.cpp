#include "gribx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "png.h"

extern "C" {
#include <openjpeg.h>
#include <libaec.h>
}

namespace wx {
namespace gribx {
namespace {

const double kPi = 3.14159265358979323846, kDeg = kPi / 180.0;
const float kNaN = std::numeric_limits<float>::quiet_NaN();

uint32_t u32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint16_t u16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
int32_t s32(const uint8_t* p) { uint32_t v = u32(p); return (v & 0x80000000u) ? -(int32_t)(v & 0x7fffffffu) : (int32_t)v; }
int s16(const uint8_t* p) { int v = u16(p); return (v & 0x8000) ? -(v & 0x7fff) : v; }
int s8(uint8_t v) { return (v & 0x80) ? -(int)(v & 0x7f) : (int)v; }
float f32(const uint8_t* p) { uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }

std::time_t utcToTime(int y, int mo, int d, int h, int mi, int sec) {
    y -= mo <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return (std::time_t)(days * 86400L + h * 3600L + mi * 60L + sec);
}

double wrap180(double d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }

// Lambert conformal on a sphere (Snyder, Map Projections - A Working Manual, eq. 15-1 .. 15-5).
struct Lcc {
    double n = 0, F = 0, R = 0, lam0 = 0;
    explicit Lcc(const Grid& g) {
        R = g.radius;
        double p1 = g.latin1 * kDeg, p2 = g.latin2 * kDeg;
        if (std::fabs(g.latin1 - g.latin2) < 1e-6) n = std::sin(p1);
        else n = std::log(std::cos(p1) / std::cos(p2)) / std::log(std::tan(kPi / 4 + p2 / 2) / std::tan(kPi / 4 + p1 / 2));
        F = std::cos(p1) * std::pow(std::tan(kPi / 4 + p1 / 2), n) / n;
        lam0 = g.lov;
    }
    void fwd(double lat, double lon, double& x, double& y) const {
        double rho = R * F / std::pow(std::tan(kPi / 4 + lat * kDeg / 2), n);
        double th = n * wrap180(lon - lam0) * kDeg;
        x = rho * std::sin(th);
        y = -rho * std::cos(th);
    }
    void inv(double x, double y, double& lat, double& lon) const {
        double rho = (n > 0 ? 1 : -1) * std::sqrt(x * x + y * y);
        double th = n > 0 ? std::atan2(x, -y) : std::atan2(-x, y);
        lat = (2 * std::atan(std::pow(R * F / rho, 1 / n)) - kPi / 2) / kDeg;
        lon = wrap180(lam0 + th / n / kDeg);
    }
};

} // namespace

bool Grid::periodicI() const { return tmpl == 0 && std::fabs(nx * di - 360.0) < di * 0.5; }

bool Grid::locate(double lat, double lon, double& fi, double& fj) const {
    const bool iNeg = (scan & 0x80) != 0, jPos = (scan & 0x40) != 0;
    if (tmpl == 0) {
        double dl = iNeg ? lo1 - lon : lon - lo1;
        dl = std::fmod(dl, 360.0); if (dl < 0) dl += 360.0;
        fi = dl / di;
        if (periodicI()) { if (fi >= nx) fi -= nx; }
        else if (fi > nx - 1 + 1e-6) {
            if (fi * di > 360.0 - di * 0.5) fi -= 360.0 / di;   // just west of the first column
            if (fi < -1e-6 || fi > nx - 1 + 1e-6) return false;
        }
        fj = (jPos ? lat - la1 : la1 - lat) / dj;
        return fj >= -1e-6 && fj <= ny - 1 + 1e-6;
    }
    if (tmpl == 30) {
        Lcc p(*this);
        double x, y, x1, y1;
        p.fwd(lat, lon, x, y);
        p.fwd(la1, lo1, x1, y1);
        fi = (iNeg ? x1 - x : x - x1) / dx;
        fj = (jPos ? y - y1 : y1 - y) / dy;
        return fi >= -1e-6 && fj >= -1e-6 && fi <= nx - 1 + 1e-6 && fj <= ny - 1 + 1e-6;
    }
    return false;
}

bool Grid::latLonOf(double fi, double fj, double& lat, double& lon) const {
    const bool iNeg = (scan & 0x80) != 0, jPos = (scan & 0x40) != 0;
    if (tmpl == 0) {
        lon = wrap180(lo1 + (iNeg ? -1 : 1) * fi * di);
        lat = la1 + (jPos ? 1 : -1) * fj * dj;
        return true;
    }
    if (tmpl == 30) {
        Lcc p(*this);
        double x1, y1;
        p.fwd(la1, lo1, x1, y1);
        p.inv(x1 + (iNeg ? -1 : 1) * fi * dx, y1 + (jPos ? 1 : -1) * fj * dy, lat, lon);
        return true;
    }
    return false;
}

std::string Grid::describe() const {
    char b[200];
    if (tmpl == 0) std::snprintf(b, sizeof b, "lat/lon %dx%d from %.3f,%.3f step %.3fx%.3f deg, scan %02x", nx, ny, la1, wrap180(lo1), di, dj, scan);
    else if (tmpl == 30) std::snprintf(b, sizeof b, "Lambert %dx%d from %.3f,%.3f, %.0f m, LoV %.2f, Latin %.2f/%.2f, R %.0f m, scan %02x", nx, ny, la1, wrap180(lo1), dx, wrap180(lov), latin1, latin2, radius, scan);
    else std::snprintf(b, sizeof b, "grid template 3.%d (not supported)", tmpl);
    return b;
}

std::time_t Message::validTime() const {
    if (!refTime) return 0;
    long s = 0;
    switch (ftUnit) {
        case 0: s = 60; break; case 1: s = 3600; break; case 2: s = 86400; break; case 10: s = 3 * 3600; break;
        case 11: s = 6 * 3600; break; case 12: s = 12 * 3600; break; case 13: s = 1; break; default: return refTime;
    }
    return refTime + (std::time_t)(ftValue * s);
}

uint64_t messageLength(const uint8_t* d, size_t n) {
    if (n < 16 || std::memcmp(d, "GRIB", 4) != 0 || d[7] != 2) return 0;
    return ((uint64_t)u32(d + 8) << 32) | u32(d + 12);
}

namespace {

// Walks the sections. With `full`, sections 6 and 7 must be present too.
struct Sections {
    const uint8_t* bitmap = nullptr; int bmInd = 255;
    const uint8_t* payload = nullptr; size_t payloadLen = 0;
    const uint8_t* s5 = nullptr; size_t s5len = 0;
};

bool walk(const uint8_t* d, size_t n, Message& m, Sections& sec, bool full) {
    m.error.clear();
    uint64_t total = messageLength(d, n);
    if (!total) { m.error = "not a GRIB2 message"; return false; }
    m.discipline = d[6];
    m.length = (size_t)total;
    size_t end = (size_t)std::min<uint64_t>(total, n);
    size_t pos = 16;
    bool have3 = false, have4 = false, have5 = false;
    while (pos + 5 <= end) {
        if (std::memcmp(d + pos, "7777", 4) == 0) break;
        uint32_t len = u32(d + pos);
        int num = d[pos + 4];
        if (len < 5) { m.error = "bad section length"; return false; }
        if (pos + len > end) {
            if (!full && have3 && have4 && have5) break;   // a header read: the data need not be here
            m.error = pos + len > total ? "truncated section" : "message is longer than the bytes read";
            return false;
        }
        const uint8_t* s = d + pos;
        if (num == 1 && len >= 19) {
            m.refTime = utcToTime(u16(s + 12), s[14], s[15], s[16], s[17], s[18]);
        } else if (num == 3 && len >= 14) {
            Grid& g = m.grid;
            g.tmpl = u16(s + 12);
            int shape = s[14];
            if (shape == 0) g.radius = 6367470.0;
            else if (shape == 6) g.radius = 6371229.0;
            else if (shape == 1 && len >= 20) g.radius = u32(s + 16) / std::pow(10.0, s8(s[15]));
            else if (shape == 8) g.radius = 6371200.0;
            else g.radius = 6371229.0;                 // ellipsoids: a sphere of mean radius (error < 0.5 km on these grids)
            if (g.tmpl == 0 && len >= 72) {
                g.nx = (int)u32(s + 30); g.ny = (int)u32(s + 34);
                uint32_t basic = u32(s + 38), sub = u32(s + 42);
                double unit = (basic == 0 || sub == 0 || sub == 0xFFFFFFFFu) ? 1e-6 : (double)basic / (double)sub;
                g.la1 = s32(s + 46) * unit; g.lo1 = s32(s + 50) * unit;
                g.di = u32(s + 63) * unit; g.dj = u32(s + 67) * unit;
                g.scan = s[71];
            } else if (g.tmpl == 30 && len >= 81) {
                g.nx = (int)u32(s + 30); g.ny = (int)u32(s + 34);
                g.la1 = s32(s + 38) * 1e-6; g.lo1 = s32(s + 42) * 1e-6;
                g.lad = s32(s + 47) * 1e-6; g.lov = s32(s + 51) * 1e-6;
                g.dx = u32(s + 55) * 1e-3; g.dy = u32(s + 59) * 1e-3;
                g.southPole = (s[63] & 0x80) != 0;
                g.scan = s[64];
                g.latin1 = s32(s + 65) * 1e-6; g.latin2 = s32(s + 69) * 1e-6;
                if (g.southPole) { m.error = "south-pole Lambert grid not supported"; return false; }
            } else { m.error = "unsupported grid template 3." + std::to_string(g.tmpl); return false; }
            have3 = true;
        } else if (num == 4 && len >= 34) {
            m.pdt = u16(s + 7);
            m.category = s[9]; m.number = s[10];
            if (m.pdt == 0 || m.pdt == 1 || m.pdt == 8 || m.pdt == 11 || m.pdt == 15) {
                m.ftUnit = s[17];
                m.ftValue = s32(s + 18);
                m.levelType = s[22];
                uint32_t raw = u32(s + 24);
                if (s[23] == 0xFF || raw == 0xFFFFFFFFu) m.level = 0;
                else m.level = (double)s32(s + 24) * std::pow(10.0, -s8(s[23]));
            }
            have4 = true;
        } else if (num == 5 && len >= 21) {
            m.nvalues = u32(s + 5);
            m.packing = u16(s + 9);
            m.nbits = s[19];
            sec.s5 = s; sec.s5len = len;
            have5 = true;
        } else if (num == 6 && len >= 6) {
            sec.bmInd = s[5];
            if (sec.bmInd == 0) sec.bitmap = s + 6;
        } else if (num == 7) {
            sec.payload = s + 5; sec.payloadLen = len - 5;
            break;                                     // the first field of the message is enough
        }
        pos += len;
    }
    if (!have3 || !have4 || !have5) { m.error = "missing sections"; return false; }
    if (full && !sec.payload) { m.error = "no data section"; return false; }
    return true;
}

// ---- OpenJPEG: decode a JPEG2000 codestream from memory -------------------------------------------------------
struct MemSrc { const uint8_t* p; size_t n, pos; };
OPJ_SIZE_T memRead(void* buf, OPJ_SIZE_T nb, void* ud) {
    auto* m = static_cast<MemSrc*>(ud);
    if (m->pos >= m->n) return (OPJ_SIZE_T)-1;
    OPJ_SIZE_T k = std::min<OPJ_SIZE_T>(nb, m->n - m->pos);
    std::memcpy(buf, m->p + m->pos, k);
    m->pos += k;
    return k;
}
OPJ_OFF_T memSkip(OPJ_OFF_T nb, void* ud) {
    auto* m = static_cast<MemSrc*>(ud);
    if (nb < 0) { if ((size_t)(-nb) > m->pos) nb = -(OPJ_OFF_T)m->pos; }
    else if (m->pos + (size_t)nb > m->n) nb = (OPJ_OFF_T)(m->n - m->pos);
    m->pos = (size_t)((OPJ_OFF_T)m->pos + nb);
    return nb;
}
OPJ_BOOL memSeek(OPJ_OFF_T nb, void* ud) {
    auto* m = static_cast<MemSrc*>(ud);
    if (nb < 0 || (size_t)nb > m->n) return OPJ_FALSE;
    m->pos = (size_t)nb;
    return OPJ_TRUE;
}
void opjQuiet(const char*, void*) {}

bool decodeJpeg2000(const uint8_t* p, size_t n, size_t want, std::vector<uint32_t>& out, std::string& err) {
    const bool jp2 = n >= 12 && std::memcmp(p + 4, "jP  ", 4) == 0;
    opj_codec_t* codec = opj_create_decompress(jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K);
    if (!codec) { err = "OpenJPEG init failed"; return false; }
    opj_set_info_handler(codec, opjQuiet, nullptr);
    opj_set_warning_handler(codec, opjQuiet, nullptr);
    opj_set_error_handler(codec, opjQuiet, nullptr);
    opj_dparameters_t prm;
    opj_set_default_decoder_parameters(&prm);
    MemSrc src{p, n, 0};
    opj_stream_t* st = opj_stream_create(1u << 16, OPJ_TRUE);
    opj_stream_set_user_data(st, &src, nullptr);
    opj_stream_set_user_data_length(st, n);
    opj_stream_set_read_function(st, memRead);
    opj_stream_set_skip_function(st, memSkip);
    opj_stream_set_seek_function(st, memSeek);
    opj_image_t* img = nullptr;
    bool ok = opj_setup_decoder(codec, &prm) && opj_read_header(st, codec, &img) && opj_decode(codec, st, img) && opj_end_decompress(codec, st);
    if (ok && (!img || img->numcomps < 1)) ok = false;
    if (ok) {
        const opj_image_comp_t& c = img->comps[0];
        size_t cnt = (size_t)c.w * (size_t)c.h;
        if (cnt < want) { err = "JPEG2000 image has " + std::to_string(cnt) + " samples, " + std::to_string(want) + " expected"; ok = false; }
        else {
            out.resize(want);
            for (size_t k = 0; k < want; ++k) out[k] = (uint32_t)c.data[k];
        }
    } else if (err.empty()) err = "JPEG2000 data would not decode";
    if (img) opj_image_destroy(img);
    opj_stream_destroy(st);
    opj_destroy_codec(codec);
    return ok;
}

// ---- libaec: decode CCSDS (template 5.42) -------------------------------------------------------------------------
bool decodeCcsds(const uint8_t* p, size_t n, int nbits, int flags, int block, int rsi, size_t want, std::vector<uint32_t>& out, std::string& err) {
    int nbytes = nbits <= 8 ? 1 : nbits <= 16 ? 2 : (nbits <= 24 && (flags & AEC_DATA_3BYTE)) ? 3 : 4;
    std::vector<unsigned char> buf(want * (size_t)nbytes);
    aec_stream s;
    std::memset(&s, 0, sizeof s);
    s.next_in = p; s.avail_in = n;
    s.next_out = buf.data(); s.avail_out = buf.size();
    s.bits_per_sample = (unsigned)nbits; s.block_size = (unsigned)block; s.rsi = (unsigned)rsi; s.flags = (unsigned)flags;
    int rc = aec_buffer_decode(&s);
    if (rc != AEC_OK) { err = "CCSDS data would not decode (libaec " + std::to_string(rc) + ")"; return false; }
    if (s.total_out < buf.size()) { err = "CCSDS data shorter than expected"; return false; }
    out.resize(want);
    const bool msb = (flags & AEC_DATA_MSB) != 0;
    for (size_t k = 0; k < want; ++k) {
        const unsigned char* q = &buf[k * (size_t)nbytes];
        uint32_t v = 0;
        if (msb) for (int b = 0; b < nbytes; ++b) v = (v << 8) | q[b];
        else for (int b = nbytes - 1; b >= 0; --b) v = (v << 8) | q[b];
        out[k] = v;
    }
    return true;
}

} // namespace

bool readHeader(const uint8_t* d, size_t n, Message& m) {
    Sections sec;
    return walk(d, n, m, sec, false);
}

bool decode(const uint8_t* d, size_t n, Message& m, std::vector<float>& values) {
    Sections sec;
    values.clear();
    if (!walk(d, n, m, sec, true)) return false;
    const Grid& g = m.grid;
    const size_t total = (size_t)g.nx * (size_t)g.ny;
    if (g.nx <= 0 || g.ny <= 0 || total > 50000000u) { m.error = "bad grid size"; return false; }
    if (sec.bmInd != 0 && sec.bmInd != 255) { m.error = "unsupported bitmap indicator " + std::to_string(sec.bmInd); return false; }
    const size_t npts = m.nvalues;
    if (npts > total) { m.error = "more values than grid points"; return false; }
    const uint8_t* s5 = sec.s5;
    const double R = f32(s5 + 11), E = s16(s5 + 15), D = s16(s5 + 17);
    const int nbits = s5[19];
    const double sE = std::pow(2.0, E), sD = std::pow(10.0, D);

    std::vector<uint32_t> X;   // packed integers
    if (nbits == 0) X.assign(npts, 0);
    else if (m.packing == 0) {
        if (nbits > 32) { m.error = "bit width over 32"; return false; }
        if ((npts * (size_t)nbits + 7) / 8 > sec.payloadLen) { m.error = "data section too short"; return false; }
        X.resize(npts);
        for (size_t k = 0; k < npts; ++k) {
            size_t bit = k * (size_t)nbits, byte = bit / 8;
            int off = (int)(bit % 8), need = (off + nbits + 7) / 8;
            uint64_t acc = 0;
            for (int b = 0; b < need; ++b) acc = (acc << 8) | sec.payload[byte + (size_t)b];
            acc >>= (need * 8 - off - nbits);
            X[k] = (uint32_t)(acc & ((nbits == 32) ? 0xFFFFFFFFull : ((1ull << nbits) - 1)));
        }
    } else if (m.packing == 40) {
        if (!decodeJpeg2000(sec.payload, sec.payloadLen, npts, X, m.error)) return false;
    } else if (m.packing == 41) {
        PngImage png;
        if (!decodePng(sec.payload, sec.payloadLen, png, 100000000) || (size_t)png.w * (size_t)png.h < npts) { m.error = "PNG data would not decode"; return false; }
        size_t bpp = png.bytesPerPixel();
        X.resize(npts);
        for (size_t k = 0; k < npts; ++k) { uint32_t v = 0; for (size_t b = 0; b < bpp; ++b) v = (v << 8) | png.px[k * bpp + b]; X[k] = v; }
    } else if (m.packing == 42) {
        if (sec.s5len < 25) { m.error = "short CCSDS template"; return false; }
        if (!decodeCcsds(sec.payload, sec.payloadLen, nbits, s5[21], s5[22], u16(s5 + 23), npts, X, m.error)) return false;
    } else { m.error = "unsupported data packing 5." + std::to_string(m.packing); return false; }

    values.assign(total, kNaN);
    size_t k = 0;
    for (size_t q = 0; q < total; ++q) {
        if (sec.bitmap && !((sec.bitmap[q / 8] >> (7 - (q % 8))) & 1)) continue;
        if (k >= npts) break;
        values[q] = (float)((R + X[k++] * sE) / sD);
    }
    return true;
}

float bilinear(const Grid& g, const std::vector<float>& v, double fi, double fj) {
    if (v.size() != (size_t)g.nx * (size_t)g.ny) return kNaN;
    int i0 = (int)std::floor(fi), j0 = (int)std::floor(fj);
    double ti = fi - i0, tj = fj - j0;
    int i1 = i0 + 1, j1 = j0 + 1;
    if (g.periodicI()) { i0 = ((i0 % g.nx) + g.nx) % g.nx; i1 = (i0 + 1) % g.nx; }
    else { if (i0 < 0) { i0 = 0; ti = 0; } if (i1 > g.nx - 1) { i1 = g.nx - 1; if (i0 > i1) i0 = i1; } }
    if (j0 < 0) { j0 = 0; tj = 0; }
    if (j1 > g.ny - 1) { j1 = g.ny - 1; if (j0 > j1) j0 = j1; }
    float a = v[g.index(i0, j0)], b = v[g.index(i1, j0)], c = v[g.index(i0, j1)], d = v[g.index(i1, j1)];
    if (std::isnan(a) || std::isnan(b) || std::isnan(c) || std::isnan(d)) return kNaN;
    return (float)((a * (1 - ti) + b * ti) * (1 - tj) + (c * (1 - ti) + d * ti) * tj);
}

} // namespace gribx
} // namespace wx
