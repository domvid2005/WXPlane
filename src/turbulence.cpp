#include "turbulence.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace wx {
namespace turb {
namespace {
const double kPi = 3.14159265358979323846, kDeg = kPi / 180.0, kEarthR = 6371229.0;
const float kNaN = std::numeric_limits<float>::quiet_NaN();
double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// Piecewise-linear interpolation through (x, y) points sorted by x; clamped at both ends.
double pwl(const double (*t)[2], int n, double x) {
    if (x <= t[0][0]) return t[0][1];
    for (int k = 1; k < n; ++k)
        if (x <= t[k][0]) return t[k - 1][1] + (t[k][1] - t[k - 1][1]) * (x - t[k - 1][0]) / (t[k][0] - t[k - 1][0]);
    return t[n - 1][1];
}
} // namespace

// ---- Calibration --------------------------------------------------------------------------------------------------
double targetSigmaG(double edr) {
    // Assumption (docs/turbulence.md): sigma of the vertical load factor = 0.65 x (EDR - 0.07) for a typical transport,
    // so that air below EDR 0.07 (smooth to very light) adds nothing, the ~1-minute peak (about 3 sigma) is ~0.35 g at
    // EDR 0.25 (light to moderate) and ~0.85 g at EDR 0.5 (severe). The 0.07 offset was added after the first flight
    // (2026-11-06): EDR 0.05 from ECMWF gave a clearly bumpy ride.
    return 0.65 * std::max(0.0, edr - 0.07);
}

double xpLevelForSigma(double s) {
    // Measured 2026-11-06: in the response test (250 kt, 3,000 ft) level 2 gave 0.21 g and level 3 0.39 g, and levels above 3
    // no more (3 is the top of the usable scale). In the first flight with this system (climbing through 11,000 ft) a held
    // level of 0.6 already gave 0.14 g, far more than the test's level 1 (0.045 g, which X-Plane applied late). Below 1 the
    // flight point is used: 0.23 g per level.
    static const double t[][2] = {{0.0, 0.0}, {0.23, 1.0}, {0.32, 2.0}, {0.39, 3.0}};
    return pwl(t, 4, s);
}

double ellrodToEdrEquivalent(double ti) {
    // Ellrod & Knapp (1992) thresholds as commonly used (TI1, 1e-7 s^-2): ~4 light to moderate, ~8 moderate,
    // ~12 moderate to severe. The EDR values are the centres of the matching categories; above 16 the estimate is held
    // at 0.40 because a 0.25-degree model index cannot support a "severe" call on its own.
    static const double t[][2] = {{0, 0.0}, {2, 0.03}, {4, 0.12}, {8, 0.22}, {12, 0.32}, {16, 0.40}};
    return pwl(t, 6, std::max(0.0, ti));
}

double isaAltFt(double hPa) {
    double m = hPa >= 226.32 ? 44330.77 * (1.0 - std::pow(hPa / 1013.25, 0.190263)) : 11000.0 + 6341.62 * std::log(226.32 / hPa);
    return m / 0.3048;
}
double isaPressureHpa(double altFt) {
    double m = altFt * 0.3048;
    return m <= 11000.0 ? 1013.25 * std::pow(1.0 - m / 44330.77, 5.25588) : 226.32 * std::exp(-(m - 11000.0) / 6341.62);
}

// ---- NOAA GTGN ----------------------------------------------------------------------------------------------------
double gtgnLevelFt(const gribx::Message& m) {
    switch (m.levelType) {
        case 102: return m.level / 0.3048;        // metres above mean sea level
        case 100: return isaAltFt(m.level / 100.0);
        case 1: return 0.0;
        default: return std::numeric_limits<double>::quiet_NaN();
    }
}
bool isEdr(const gribx::Message& m) { return m.discipline == 0 && m.category == 19 && m.number == 30; }

void GtgnCube::addLevel(double altFt, const std::vector<float>& full, double clat, double clon, int half) {
    if (levels.empty() || ni == 0) {
        double fi, fj;
        if (!grid.locate(clat, clon, fi, fj)) { fi = grid.nx / 2.0; fj = grid.ny / 2.0; }
        int ci = (int)std::lround(fi), cj = (int)std::lround(fj);
        i0 = std::max(0, ci - half); j0 = std::max(0, cj - half);
        int i1 = std::min(grid.nx - 1, ci + half), j1 = std::min(grid.ny - 1, cj + half);
        ni = std::max(0, i1 - i0 + 1); nj = std::max(0, j1 - j0 + 1);
        centerLat = clat; centerLon = clon;
    }
    GtgnLevel L;
    L.altFt = altFt;
    L.v.assign((size_t)ni * (size_t)nj, kNaN);
    if (full.size() == (size_t)grid.nx * (size_t)grid.ny)
        for (int j = 0; j < nj; ++j)
            for (int i = 0; i < ni; ++i) L.v[(size_t)j * ni + i] = full[grid.index(i0 + i, j0 + j)];
    auto it = std::find_if(levels.begin(), levels.end(), [&](const GtgnLevel& x) { return std::fabs(x.altFt - altFt) < 1; });
    if (it != levels.end()) *it = std::move(L);
    else levels.push_back(std::move(L));
    std::sort(levels.begin(), levels.end(), [](const GtgnLevel& a, const GtgnLevel& b) { return a.altFt < b.altFt; });
}

bool GtgnCube::hasLevel(double altFt) const {
    for (auto& l : levels) if (std::fabs(l.altFt - altFt) < 1) return true;
    return false;
}

bool GtgnCube::cropCovers(double lat, double lon, int margin) const {
    double fi, fj;
    if (ni == 0 || !grid.locate(lat, lon, fi, fj)) return false;
    const bool atEdgeI0 = i0 == 0, atEdgeJ0 = j0 == 0, atEdgeI1 = i0 + ni >= grid.nx, atEdgeJ1 = j0 + nj >= grid.ny;
    return (fi - i0 >= margin || atEdgeI0) && (fj - j0 >= margin || atEdgeJ0) && (i0 + ni - 1 - fi >= margin || atEdgeI1) &&
           (j0 + nj - 1 - fj >= margin || atEdgeJ1);
}

bool GtgnCube::sample(double lat, double lon, double altFt, double& edr, double& coverage) const {
    edr = 0; coverage = 0;
    double fi, fj;
    if (levels.empty() || ni < 2 || nj < 2 || !grid.locate(lat, lon, fi, fj)) return false;
    double li = fi - i0, lj = fj - j0;
    if (li < 0 || lj < 0 || li > ni - 1 || lj > nj - 1) return false;
    int a = std::min((int)li, ni - 2), b = std::min((int)lj, nj - 2);
    double ti = li - a, tj = lj - b;
    auto hz = [&](const GtgnLevel& L) -> double {
        float p = L.v[(size_t)b * ni + a], q = L.v[(size_t)b * ni + a + 1], r = L.v[(size_t)(b + 1) * ni + a], s = L.v[(size_t)(b + 1) * ni + a + 1];
        if (std::isnan(p) || std::isnan(q) || std::isnan(r) || std::isnan(s)) return -1;
        return (p * (1 - ti) + q * ti) * (1 - tj) + (r * (1 - ti) + s * ti) * tj;
    };
    // Valid levels at this spot (the ones below ground are missing).
    double loA = -1, loV = 0, hiA = -1, hiV = 0;
    bool missingBelow = false;   // a level held below this altitude has no value here (it is below the ground)
    for (auto& L : levels) {
        double v = hz(L);
        if (v < 0) { if (L.altFt <= altFt) missingBelow = true; continue; }
        if (L.altFt <= altFt) { loA = L.altFt; loV = v; }
        else if (hiA < 0) { hiA = L.altFt; hiV = v; }
    }
    if (loA >= 0 && hiA >= 0 && hiA - loA <= 6000) edr = loV + (hiV - loV) * (altFt - loA) / (hiA - loA);
    else if (loA >= 0 && altFt - loA <= 2000) edr = loV;
    else if (hiA >= 0 && (hiA - altFt <= 2000 || (missingBelow && loA < 0))) edr = hiV;   // just below the lowest level above ground
    else return false;   // not among the levels held (only those near the aircraft are downloaded)
    edr = std::max(0.0, edr);
    double dEdge = std::min(std::min(fi, fj), std::min(grid.nx - 1 - fi, grid.ny - 1 - fj));
    coverage = clamp01((dEdge - 3.0) / 17.0);
    return true;
}

// ---- ECMWF Open Data ----------------------------------------------------------------------------------------------
namespace {
bool jsonField(const std::string& line, const char* key, std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = line.find(k);
    if (p == std::string::npos) return false;
    p = line.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < line.size() && line[p] == ' ') ++p;
    if (p < line.size() && line[p] == '"') {
        size_t e = line.find('"', p + 1);
        if (e == std::string::npos) return false;
        out = line.substr(p + 1, e - p - 1);
    } else {
        size_t e = p;
        while (e < line.size() && (std::isdigit((unsigned char)line[e]) || line[e] == '-' || line[e] == '.')) ++e;
        out = line.substr(p, e - p);
    }
    return true;
}
} // namespace

std::vector<IndexEntry> parseEcmwfIndex(const std::string& text) {
    std::vector<IndexEntry> out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        IndexEntry x;
        std::string v;
        if (!jsonField(line, "param", x.param) || !jsonField(line, "_offset", v)) continue;
        x.offset = std::strtoull(v.c_str(), nullptr, 10);
        if (!jsonField(line, "_length", v)) continue;
        x.length = std::strtoull(v.c_str(), nullptr, 10);
        jsonField(line, "levtype", x.levtype);
        if (jsonField(line, "levelist", v)) x.level = std::atoi(v.c_str());
        if (jsonField(line, "step", v)) x.step = std::atoi(v.c_str());
        out.push_back(x);
    }
    return out;
}

const std::vector<int>& ecmwfLevels() {
    static const std::vector<int> L = {700, 600, 500, 400, 300, 250, 200, 150, 100};
    return L;
}

bool buildEcCube(const gribx::Grid& g, const std::vector<EcLevelField>& lv, double clat, double clon, double half, EcCube& out, std::string& err) {
    out = EcCube();
    if (g.tmpl != 0 || g.di <= 0 || std::fabs(g.di - g.dj) > 1e-6) { err = "ECMWF grid is not a regular lat/lon grid"; return false; }
    if (lv.size() < 2) { err = "fewer than 2 pressure levels"; return false; }
    for (auto& L : lv) if (!L.u || !L.v || L.u->size() != (size_t)g.nx * g.ny || L.v->size() != L.u->size()) { err = "field size mismatch"; return false; }
    const double d = g.di;
    double north = std::min(89.0, clat + half), south = std::max(-89.0, clat - half);
    double halfLon = std::min(179.0, half / std::max(0.2, std::cos(clat * kDeg)));
    double fi0, fj0, fi1, fj1;
    if (!g.locate(north, clon - halfLon, fi0, fj0) || !g.locate(south, clon - halfLon, fi1, fj1)) { err = "crop outside the grid"; return false; }
    int i0 = (int)std::floor(fi0), j0 = (int)std::floor(std::min(fj0, fj1));
    int nj = (int)std::ceil(std::fabs(fj1 - fj0)) + 2, ni = (int)std::ceil(2 * halfLon / d) + 2;
    if (!g.periodicI()) ni = std::min(ni, g.nx - i0);
    nj = std::min(nj, g.ny - j0);
    if (ni < 3 || nj < 3) { err = "crop too small"; return false; }
    double lat0, lon0;
    g.latLonOf(i0, j0, lat0, lon0);
    const bool northFirst = (g.scan & 0x40) == 0;
    if (!northFirst) { err = "south-to-north ECMWF grid not expected"; return false; }
    out.lat0 = lat0; out.lon0 = lon0; out.d = d; out.ni = ni; out.nj = nj; out.centerLat = clat; out.centerLon = clon;
    auto at = [&](const std::vector<float>& f, int c, int r) -> double {
        int i = i0 + c; if (g.periodicI()) i %= g.nx;
        return f[g.index(i, j0 + r)];
    };
    // Layers between adjacent levels, ordered from the lowest.
    std::vector<EcLevelField> s = lv;
    std::sort(s.begin(), s.end(), [](const EcLevelField& a, const EcLevelField& b) { return a.hPa > b.hPa; });
    for (size_t k = 0; k + 1 < s.size(); ++k) {
        const auto& A = s[k]; const auto& B = s[k + 1];
        EcLayer L;
        L.loHpa = A.hPa; L.hiHpa = B.hPa;
        double za = isaAltFt(A.hPa) * 0.3048, zb = isaAltFt(B.hPa) * 0.3048;
        L.altFt = (za + zb) / 2 / 0.3048;
        double dz = zb - za;
        L.ti.assign((size_t)ni * nj, kNaN);
        for (int r = 1; r < nj - 1; ++r) {
            double lat = lat0 - r * d;
            if (std::fabs(lat) > 85) continue;
            double dx = kEarthR * std::cos(lat * kDeg) * d * kDeg, dy = kEarthR * d * kDeg;
            for (int c = 1; c < ni - 1; ++c) {
                double du = at(*B.u, c, r) - at(*A.u, c, r), dv = at(*B.v, c, r) - at(*A.v, c, r);
                double vws = std::sqrt(du * du + dv * dv) / dz;
                auto um = [&](int cc, int rr) { return 0.5 * (at(*A.u, cc, rr) + at(*B.u, cc, rr)); };
                auto vm = [&](int cc, int rr) { return 0.5 * (at(*A.v, cc, rr) + at(*B.v, cc, rr)); };
                double dudx = (um(c + 1, r) - um(c - 1, r)) / (2 * dx), dvdx = (vm(c + 1, r) - vm(c - 1, r)) / (2 * dx);
                double dudy = (um(c, r - 1) - um(c, r + 1)) / (2 * dy), dvdy = (vm(c, r - 1) - vm(c, r + 1)) / (2 * dy);   // row r-1 is north
                double dst = dudx - dvdy, dsh = dvdx + dudy;
                double ti = vws * std::sqrt(dst * dst + dsh * dsh) * 1e7;
                if (std::isfinite(ti)) L.ti[(size_t)r * ni + c] = (float)ti;
            }
        }
        out.layers.push_back(std::move(L));
    }
    return true;
}

bool EcCube::cropCovers(double lat, double lon, double margin) const {
    if (ni == 0) return false;
    double dl = lon - lon0; while (dl < 0) dl += 360; while (dl >= 360) dl -= 360;
    double c = dl / d, r = (lat0 - lat) / d, m = margin / d;
    return r >= m && c >= m / std::max(0.2, std::cos(lat * kDeg)) && r <= nj - 1 - m && c <= ni - 1 - m / std::max(0.2, std::cos(lat * kDeg));
}

bool EcCube::sampleTi(double lat, double lon, double altFt, double& ti) const {
    ti = 0;
    if (layers.empty() || altFt < 10000 || altFt > 50000) return false;
    double dl = lon - lon0; while (dl < 0) dl += 360; while (dl >= 360) dl -= 360;
    double c = dl / d, r = (lat0 - lat) / d;
    if (c < 1 || r < 1 || c > ni - 2 || r > nj - 2) return false;
    int a = std::min((int)c, ni - 3), b = std::min((int)r, nj - 3);
    double tc = c - a, tr = r - b;
    auto hz = [&](const EcLayer& L) -> double {
        float p = L.ti[(size_t)b * ni + a], q = L.ti[(size_t)b * ni + a + 1], s = L.ti[(size_t)(b + 1) * ni + a], t = L.ti[(size_t)(b + 1) * ni + a + 1];
        if (std::isnan(p) || std::isnan(q) || std::isnan(s) || std::isnan(t)) return -1;
        return (p * (1 - tc) + q * tc) * (1 - tr) + (s * (1 - tc) + t * tc) * tr;
    };
    const EcLayer* lo = nullptr; const EcLayer* hi = nullptr;
    for (auto& L : layers) { if (L.altFt <= altFt) lo = &L; else if (!hi) hi = &L; }
    double v;
    if (lo && hi) {
        double x = hz(*lo), y = hz(*hi);
        if (x < 0 || y < 0) return false;
        v = x + (y - x) * (altFt - lo->altFt) / (hi->altFt - lo->altFt);
    } else if (lo) { v = hz(*lo); }
    else { v = hz(*hi); }
    if (v < 0) return false;
    ti = v;
    return true;
}

// ---- Small-scale variation ------------------------------------------------------------------------------------------
namespace {
double hash4(long x, long y, long z, long t) {
    uint64_t h = 1469598103934665603ull;
    for (long v : {x, y, z, t}) { h ^= (uint64_t)v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 1099511628211ull; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (double)(h >> 11) / (double)(1ull << 53) * 2.0 - 1.0;   // -1 .. 1
}
double smooth(double t) { return t * t * (3 - 2 * t); }
} // namespace

double pocketNoise(double lat, double lon, double altFt, double simTime, double amp) {
    // Value noise on ~8 km cells, 3,000 ft layers and 4-minute time steps, so pockets have a size and a lifetime.
    double x = lon * 111.32 * std::cos(lat * kDeg) / 8.0, y = lat * 111.32 / 8.0, z = altFt / 3000.0, t = simTime / 240.0;
    long xi = (long)std::floor(x), yi = (long)std::floor(y), zi = (long)std::floor(z), ti = (long)std::floor(t);
    double fx = smooth(x - xi), fy = smooth(y - yi), fz = smooth(z - zi), ft = smooth(t - ti);
    double n = 0;
    for (int c = 0; c < 16; ++c) {
        int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1, dt = (c >> 3) & 1;
        double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz) * (dt ? ft : 1 - ft);
        n += w * hash4(xi + dx, yi + dy, zi + dz, ti + dt);
    }
    return std::exp(amp * 1.5 * n);
}

// ---- Combined -----------------------------------------------------------------------------------------------------
Sample sampleAt(const Inputs& in, double lat, double lon, double altFt, std::time_t now, double simTime) {
    Sample s;
    bool haveG = false, haveE = false;
    double g = 0, cov = 0;
    if (in.gtgn && now - in.gtgn->valid <= kGtgnMaxAgeS && in.gtgn->sample(lat, lon, altFt, g, cov)) {
        haveG = true;
        double w = in.gtgnArrived ? clamp01((double)(now - in.gtgnArrived) / 180.0) : 1.0;
        double gp, cp;
        if (w < 1 && in.gtgnPrev && in.gtgnPrev->sample(lat, lon, altFt, gp, cp)) g = w * g + (1 - w) * gp;
        s.gtgnEdr = g;
    }
    double e = 0;
    if (in.ecA) {
        double ta, tb;
        bool okA = std::labs((long)(now - in.ecA->valid)) <= kEcMaxGapS && in.ecA->sampleTi(lat, lon, altFt, ta);
        bool okB = in.ecB && std::labs((long)(now - in.ecB->valid)) <= kEcMaxGapS && in.ecB->sampleTi(lat, lon, altFt, tb);
        if (okA && okB && in.ecB->valid != in.ecA->valid) {
            double t = clamp01((double)(now - in.ecA->valid) / (double)(in.ecB->valid - in.ecA->valid));
            s.ecTi = ta + (tb - ta) * t;
        } else if (okA) s.ecTi = ta;
        else if (okB) s.ecTi = tb;
        if (s.ecTi >= 0) { haveE = true; e = ellrodToEdrEquivalent(s.ecTi); s.ecEdr = e; }
    }
    if (haveG && haveE) { s.edr = cov * g + (1 - cov) * e; s.coverage = cov; s.source = cov >= 0.999 ? "GTGN" : cov <= 0.001 ? "ECMWF estimate" : "GTGN+ECMWF"; }
    else if (haveG) { s.edr = g; s.coverage = cov; s.source = "GTGN"; }
    else if (haveE) { s.edr = e; s.coverage = 0; s.source = "ECMWF estimate"; }
    else return s;
    const double amp = 0.12 * s.coverage + 0.25 * (1 - s.coverage);
    s.noise = pocketNoise(lat, lon, altFt, simTime, amp);
    s.edr *= s.noise;
    return s;
}

} // namespace turb
} // namespace wx
