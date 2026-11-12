#include "opera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "inflate.h"

namespace wx {
namespace {

const double kD2R = 3.14159265358979323846 / 180.0;

struct Reader {
    const std::string& s;
    bool le = true;
    bool ok(size_t o, size_t n) const { return o + n <= s.size(); }
    uint16_t u16(size_t o) const {
        const uint8_t* p = (const uint8_t*)s.data() + o;
        return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
    }
    uint32_t u32(size_t o) const {
        const uint8_t* p = (const uint8_t*)s.data() + o;
        return le ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
                  : ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    double f64(size_t o) const {
        uint64_t v = 0;
        const uint8_t* p = (const uint8_t*)s.data() + o;
        for (int i = 0; i < 8; ++i) v |= (uint64_t)p[le ? i : 7 - i] << (8 * i);
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
};

size_t typeSize(int t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: case 16: return 8;
        default: return 0;
    }
}

double qOf(double phi, double e2, double e) {
    const double s = std::sin(phi);
    return (1 - e2) * (s / (1 - e2 * s * s) - (1 / (2 * e)) * std::log((1 - e * s) / (1 + e * s)));
}

} // namespace

std::time_t operaNewestSlot(std::time_t now) { return ((now - 240) / 300) * 300; }

static const char* operaBase(int store) {
    return store == 1 ? "https://object-store.os-api.cci1.ecmwf.int/openradar-24h-backup" : "https://s3.waw3-1.cloudferro.com/openradar-24h";
}

std::string operaUrl(std::time_t t, int store) {
    t = (t / 300) * 300;
    std::tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[240];
    std::snprintf(b, sizeof b, "%s/%04d/%02d/%02d/OPERA/COMP/OPERA@%04d%02d%02dT%02d%02d@0@DBZH.tiff", operaBase(store),
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min);
    return b;
}

std::string operaListUrl(std::time_t t, int store) {
    t = (t / 3600) * 3600;
    std::tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[260];
    std::snprintf(b, sizeof b, "%s?list-type=2&max-keys=200&prefix=%04d/%02d/%02d/OPERA/COMP/OPERA@%04d%02d%02dT%02d", operaBase(store),
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour);
    return b;
}

static std::time_t timegmUtc(int y, int mo, int d, int h, int mi) {
    // days from civil (Howard Hinnant), no timegm needed
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (std::time_t)(days * 86400 + h * 3600 + mi * 60);
}

std::time_t operaNewestInListing(const std::string& xml) {
    std::time_t best = 0;
    for (size_t at = xml.find("OPERA@"); at != std::string::npos; at = xml.find("OPERA@", at + 6)) {
        int y, mo, d, h, mi;
        char tail[16] = {0};
        if (std::sscanf(xml.c_str() + at, "OPERA@%4d%2d%2dT%2d%2d@0@%15[A-Z.a-z]", &y, &mo, &d, &h, &mi, tail) != 6) continue;
        if (std::strncmp(tail, "DBZH.tiff", 9) != 0) continue;
        best = std::max(best, timegmUtc(y, mo, d, h, mi));
    }
    return best;
}

void operaForward(const OperaHeader& h, double lat, double lon, double& x, double& y) {
    const double f = 1.0 / h.invF, e2 = 2 * f - f * f, e = std::sqrt(e2);
    const double qp = qOf(90 * kD2R, e2, e), Rq = h.a * std::sqrt(qp / 2);
    const double p0 = h.lat0 * kD2R, b0 = std::asin(qOf(p0, e2, e) / qp);
    const double D = h.a * std::cos(p0) / (std::sqrt(1 - e2 * std::sin(p0) * std::sin(p0)) * Rq * std::cos(b0));
    const double b = std::asin(std::max(-1.0, std::min(1.0, qOf(lat * kD2R, e2, e) / qp)));
    double dl = (lon - h.lon0) * kD2R;
    const double den = 1 + std::sin(b0) * std::sin(b) + std::cos(b0) * std::cos(b) * std::cos(dl);
    const double B = Rq * std::sqrt(2 / std::max(1e-12, den));
    x = h.fe + B * D * std::cos(b) * std::sin(dl);
    y = h.fn + (B / D) * (std::cos(b0) * std::sin(b) - std::sin(b0) * std::cos(b) * std::cos(dl));
}

bool operaCovers(double lat, double lon) {
    if (lat < 25 || lat > 80 || lon < -50 || lon > 70) return false;
    OperaHeader h;   // the standard OPERA domain: 3800 x 4400 km around 55N 10E
    h.width = 3800; h.height = 4400; h.dx = h.dy = 1000; h.x0 = -500; h.y0 = 500; h.lat0 = 55; h.lon0 = 10; h.fe = 1950000; h.fn = -2100000;
    double x, y;
    operaForward(h, lat, lon, x, y);
    const double col = (x - h.x0) / h.dx, row = (h.y0 - y) / h.dy;
    return col >= 0 && row >= 0 && col < h.width && row < h.height;
}

bool operaParseHeader(const std::string& head, OperaHeader& h, std::string& err, size_t& needBytes) {
    needBytes = 0;
    h = OperaHeader();
    if (head.size() < 8) { err = "short header"; needBytes = 65536; return false; }
    Reader r{head};
    if (head[0] == 'I' && head[1] == 'I') r.le = true;
    else if (head[0] == 'M' && head[1] == 'M') r.le = false;
    else { err = "not a TIFF file"; return false; }
    if (r.u16(2) != 42) { err = "not a classic TIFF (BigTIFF is not supported)"; return false; }
    const size_t ifd = r.u32(4);
    if (!r.ok(ifd, 2)) { needBytes = ifd + 4096; err = "header continues"; return false; }
    const int n = r.u16(ifd);
    if (!r.ok(ifd + 2, (size_t)n * 12 + 4)) { needBytes = ifd + 2 + (size_t)n * 12 + 4096; err = "header continues"; return false; }
    int bps = 0, comp = 0, pred = 1, fmt = 0, planar = 1;
    std::vector<double> scale, tie, gdouble;
    std::vector<uint16_t> gkeys;
    for (int i = 0; i < n; ++i) {
        const size_t e = ifd + 2 + (size_t)i * 12;
        const int tag = r.u16(e), typ = r.u16(e + 2);
        const uint32_t cnt = r.u32(e + 4);
        const size_t ts = typeSize(typ);
        if (!ts) continue;
        const size_t sz = ts * cnt;
        const size_t at = sz <= 4 ? e + 8 : r.u32(e + 8);
        if (!r.ok(at, sz)) {
            if (tag == 324 || tag == 325 || tag == 33550 || tag == 33922 || tag == 34735 || tag == 34736) { needBytes = std::max(needBytes, at + sz + 1024); }
            continue;
        }
        auto val = [&](uint32_t k) -> double {
            const size_t o = at + k * ts;
            switch (typ) {
                case 3: return r.u16(o);
                case 4: return r.u32(o);
                case 12: return r.f64(o);
                case 1: return (uint8_t)head[o];
                default: return 0;
            }
        };
        switch (tag) {
            case 256: h.width = (int)val(0); break;
            case 257: h.height = (int)val(0); break;
            case 258: bps = (int)val(0); break;
            case 259: comp = (int)val(0); break;
            case 277: h.spp = (int)val(0); break;
            case 284: planar = (int)val(0); break;
            case 317: pred = (int)val(0); break;
            case 322: h.tileW = (int)val(0); break;
            case 323: h.tileH = (int)val(0); break;
            case 324: h.tileOff.resize(cnt); for (uint32_t k = 0; k < cnt; ++k) h.tileOff[k] = (uint64_t)val(k); break;
            case 325: h.tileLen.resize(cnt); for (uint32_t k = 0; k < cnt; ++k) h.tileLen[k] = (uint64_t)val(k); break;
            case 339: fmt = (int)val(0); break;
            case 33550: for (uint32_t k = 0; k < cnt; ++k) scale.push_back(val(k)); break;
            case 33922: for (uint32_t k = 0; k < cnt; ++k) tie.push_back(val(k)); break;
            case 34735: for (uint32_t k = 0; k < cnt; ++k) gkeys.push_back((uint16_t)val(k)); break;
            case 34736: for (uint32_t k = 0; k < cnt; ++k) gdouble.push_back(val(k)); break;
            default: break;
        }
    }
    if (needBytes > head.size()) { err = "header continues"; return false; }
    needBytes = 0;
    if (h.width <= 0 || h.height <= 0 || h.tileW <= 0 || h.tileH <= 0) { err = "not a tiled image"; return false; }
    if (bps != 32 || fmt != 3) { err = "unexpected sample type (want 32-bit float)"; return false; }
    if (comp != 8 && comp != 32946) { err = "unexpected compression " + std::to_string(comp); return false; }
    if (pred != 1) { err = "unexpected predictor " + std::to_string(pred); return false; }
    if (planar != 1 || h.spp < 1) { err = "unexpected sample layout"; return false; }
    const size_t nt = (size_t)h.tilesAcross() * (size_t)((h.height + h.tileH - 1) / h.tileH);
    if (h.tileOff.size() != nt || h.tileLen.size() != nt) { err = "tile table does not match the image"; return false; }
    if (scale.size() < 2 || tie.size() < 6 || scale[0] <= 0 || scale[1] <= 0) { err = "no georeferencing"; return false; }
    h.dx = scale[0]; h.dy = scale[1];
    h.x0 = tie[3] - tie[0] * h.dx;
    h.y0 = tie[4] + tie[1] * h.dy;
    // GeoKeys: the projection must be Lambert azimuthal equal-area (ProjCoordTransGeoKey 3075 = 10), in metres
    bool laea = false, haveLat = false, haveLon = false;
    if (gkeys.size() >= 4) {
        const int nk = gkeys[3];
        for (int k = 0; k < nk && 4 + k * 4 + 3 < (int)gkeys.size(); ++k) {
            const int id = gkeys[4 + k * 4], loc = gkeys[4 + k * 4 + 1], cnt = gkeys[4 + k * 4 + 2], vo = gkeys[4 + k * 4 + 3];
            double v;
            if (loc == 0) v = vo;
            else if (loc == 34736 && cnt >= 1 && vo < (int)gdouble.size()) v = gdouble[vo];
            else continue;
            switch (id) {
                case 3075: laea = (int)v == 10; break;
                case 3076: if ((int)v != 9001) { err = "projected units are not metres"; return false; } break;
                case 3088: h.lon0 = v; haveLon = true; break;   // ProjCenterLong
                case 3089: h.lat0 = v; haveLat = true; break;   // ProjCenterLat
                case 3080: if (!haveLon) { h.lon0 = v; haveLon = true; } break;   // ProjNatOriginLong
                case 3081: if (!haveLat) { h.lat0 = v; haveLat = true; } break;   // ProjNatOriginLat
                case 3082: h.fe = v; break;
                case 3083: h.fn = v; break;
                case 2057: h.a = v; break;
                case 2059: h.invF = v; break;
                default: break;
            }
        }
    }
    if (!laea || !haveLat || !haveLon) { err = "not a Lambert azimuthal equal-area grid"; return false; }
    if (!(h.a > 6.3e6 && h.a < 6.4e6) || !(h.invF > 290 && h.invF < 310)) { err = "unexpected ellipsoid"; return false; }
    return true;
}

bool operaDecodeTile(const std::string& data, const OperaHeader& h, std::vector<float>& dbz, std::string& err) {
    std::vector<uint8_t> raw;
    const size_t want = (size_t)h.tileW * h.tileH * h.spp * 4;
    if (!zlibDecompress((const uint8_t*)data.data(), data.size(), raw, want + 16) || raw.size() < want) { err = "bad tile data"; return false; }
    dbz.assign((size_t)h.tileW * h.tileH, 0.0f);
    for (size_t i = 0; i < dbz.size(); ++i) {
        const uint8_t* p = raw.data() + i * h.spp * 4;   // the first sample of each pixel is DBZH
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        float f;
        std::memcpy(&f, &u, 4);
        if (std::isnan(f)) f = -99.0f;            // "undetect": the radars see no echo
        else if (f < -1e5f) f = -999.0f;          // no radar covers this pixel
        dbz[i] = f;
    }
    return true;
}

bool operaWindow(const OperaHeader& h, const GribWindow& win, double stepDeg, const OperaRangeFn& get, std::map<int, std::vector<float>>& cache,
                 GribField& out, int& tilesFetched, std::string& err) {
    tilesFetched = 0;
    out = GribField();
    if (stepDeg <= 0) stepDeg = 0.01;
    const int ni = (int)std::floor((win.lonMax - win.lonMin) / stepDeg) + 1, nj = (int)std::floor((win.latMax - win.latMin) / stepDeg) + 1;
    if (ni <= 0 || nj <= 0 || (size_t)ni * nj > 4000000u) { err = "bad radar window"; return false; }
    // every output cell's source pixel, and the tiles they need
    std::vector<int> tileOf((size_t)ni * nj, -1), pixOf((size_t)ni * nj, -1);
    std::set<int> need;
    const int across = h.tilesAcross();
    for (int j = 0; j < nj; ++j) {
        const double lat = win.latMax - j * stepDeg;
        for (int i = 0; i < ni; ++i) {
            const double lon = win.lonMin + i * stepDeg;
            double x, y;
            operaForward(h, lat, lon, x, y);
            const double cf = (x - h.x0) / h.dx, rf = (h.y0 - y) / h.dy;
            if (!(cf >= 0 && rf >= 0 && cf < h.width && rf < h.height)) continue;
            const int col = (int)cf, row = (int)rf, t = (row / h.tileH) * across + col / h.tileW;
            tileOf[(size_t)j * ni + i] = t;
            pixOf[(size_t)j * ni + i] = (row % h.tileH) * h.tileW + (col % h.tileW);
            need.insert(t);
        }
    }
    if (need.empty()) { err = "outside the European composite"; return false; }
    for (int t : need) {
        if (cache.count(t)) continue;
        if (t < 0 || t >= (int)h.tileOff.size()) continue;
        std::vector<float> v;
        if (h.tileLen[t] == 0) { v.assign((size_t)h.tileW * h.tileH, -999.0f); cache[t] = std::move(v); continue; }   // a sparse (empty) tile
        std::string data, e;
        if (!get(h.tileOff[t], h.tileLen[t], data, e)) { err = "tile download failed: " + e; return false; }
        if (!operaDecodeTile(data, h, v, e)) { err = e; return false; }
        cache[t] = std::move(v);
        ++tilesFetched;
    }
    out.ni = ni; out.nj = nj; out.lat0 = win.latMax; out.lon0 = win.lonMin; out.dLat = stepDeg; out.dLon = stepDeg;
    out.v.assign((size_t)ni * nj, -999.0f);
    for (size_t k = 0; k < out.v.size(); ++k) {
        if (tileOf[k] < 0) continue;
        auto it = cache.find(tileOf[k]);
        if (it != cache.end() && pixOf[k] >= 0 && pixOf[k] < (int)it->second.size()) out.v[k] = it->second[(size_t)pixOf[k]];
    }
    return true;
}

} // namespace wx
