#include "radar.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "geo.h"

namespace wx {
namespace {

const double kPi = 3.14159265358979323846;
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct PalEntry { int r, g, b; float dbz; };
std::vector<PalEntry> g_pal;

} // namespace

float rateFromDbz(float dbz) {
    if (dbz < 5) return 0;
    double z = std::pow(10.0, dbz / 10.0);
    double r = dbz < 40 ? std::pow(z / 200.0, 1.0 / 1.6) : std::pow(z / 300.0, 1.0 / 1.4);
    return (float)std::min(r, 200.0);
}

float tropopauseKm(double lat) { return (float)std::max(9.0, 17.0 - 0.17 * std::fabs(lat)); }

float estimateTopM(float dbz, double lat) {
    float km = clampf(2.5f + (dbz - 15.0f) * 0.22f, 3.0f, tropopauseKm(lat));
    return km * 1000.0f;
}

std::string formatFl(float topM, bool known) {
    long fl = std::lround(topM / 0.3048 / 1000.0) * 10;
    if (fl < 0) fl = 0;
    char b[24];
    std::snprintf(b, sizeof b, "%sFL%03ld", known ? "" : "~", fl);
    return b;
}

std::vector<std::string> mrmsRegionsFor(double lat, double lon) {
    std::vector<std::string> r;
    if (lat >= 20.0 && lat <= 55.0 && lon >= -130.0 && lon <= -60.0) r.push_back("");          // CONUS
    if (lat >= 50.0 && lat <= 73.0 && lon >= -180.0 && lon <= -125.0) r.push_back("ALASKA");
    if (lat >= 15.0 && lat <= 26.0 && lon >= -165.0 && lon <= -150.0) r.push_back("HAWAII");
    if (lat >= 9.0 && lat <= 24.0 && lon >= -90.0 && lon <= -58.0) r.push_back("CARIB");
    if (lat >= 8.0 && lat <= 20.0 && lon >= 140.0 && lon <= 152.0) r.push_back("GUAM");
    return r;
}

std::string mrmsUrl(const std::string& region, const std::string& product) {
    std::string u = "https://mrms.ncep.noaa.gov/2D/";
    if (!region.empty()) u += region + "/";
    return u + product + "/MRMS_" + product + ".latest.grib2.gz";
}

RadarResult buildRadarCells(const GribField& refl, const GribField* tops, double lat, double lon, const Config& cfg,
                            const std::set<std::string>* prev) {
    RadarResult res;
    res.attempted = true;
    if (!refl.valid()) { res.note = "no reflectivity grid"; return res; }
    const double blockLat = cfg.radarCellNm / 60.0;
    struct Acc { float maxDbz = -999; int n = 0; float top = 0; double lat = 0, lon = 0, step = 0; };
    std::map<std::pair<long, long>, Acc> blocks;
    const float minDbz = (float)cfg.radarMinDbz;
    std::set<std::pair<long, long>> prevBlocks;
    if (prev)
        for (const auto& k : *prev) {
            long a, b;
            if (std::sscanf(k.c_str(), "R%ld:%ld", &a, &b) == 2) prevBlocks.insert({a, b});
        }

    for (int j = 0; j < refl.nj; ++j) {
        double la = refl.lat0 - j * refl.dLat;
        double step = std::max(blockLat, std::round(blockLat / std::max(0.2, std::cos(la * kPi / 180.0)) * 100.0) / 100.0);
        long bj = (long)std::floor((90.0 - la) / blockLat);
        for (int i = 0; i < refl.ni; ++i) {
            float v = refl.v[(size_t)j * (size_t)refl.ni + (size_t)i];
            if (!(v >= minDbz - 5.0f)) continue;          // also rejects NaN and MRMS "missing" codes (-99, -999)
            double lo = refl.lon0 + i * refl.dLon;
            long bi = (long)std::floor((lo + 180.0) / step);
            const bool wasThere = !prevBlocks.empty() && prevBlocks.count({bj, bi}) > 0;
            if (v < minDbz && !wasThere) continue;        // blocks already shown stay until they drop 5 dBZ lower
            if (distNm(lat, lon, la, lo) > cfg.radarRangeNm * (wasThere ? 1.15 : 1.0)) continue;
            Acc& a = blocks[{bj, bi}];
            if (a.n == 0) { a.step = step; a.lat = 90.0 - (bj + 0.5) * blockLat; a.lon = (bi + 0.5) * step - 180.0; }
            ++a.n;
            a.maxDbz = std::max(a.maxDbz, std::min(v, 70.0f));    // above ~70 dBZ is hail/ground clutter, not heavier rain
            if (tops) {
                float t = tops->at(la, lo);
                if (t > 0.5f && t < 30.0f) a.top = std::max(a.top, t);
            }
        }
    }

    std::vector<RadarCell> all;
    for (auto& kv : blocks) {
        const Acc& a = kv.second;
        RadarCell c;
        c.lat = a.lat; c.lon = a.lon;
        if (a.maxDbz < minDbz - 5.0f) continue;
        c.dbz = std::max(5.0f, std::round(a.maxDbz / 5.0f) * 5.0f);
        double pxPerBlock = (blockLat / refl.dLat) * (a.step / refl.dLon);
        c.frac = std::max(0.25f, std::round(clampf((float)(a.n / std::max(1.0, pxPerBlock)), 0.0f, 1.0f) * 4.0f) / 4.0f);
        c.topKnown = a.top > 0;
        c.topM = c.topKnown ? std::round(a.top * 2.0f) * 500.0f : std::round(estimateTopM(c.dbz, c.lat) / 500.0f) * 500.0f;
        c.rateMmH = rateFromDbz(c.dbz);
        c.distNm = distNm(lat, lon, c.lat, c.lon);
        char key[48];
        std::snprintf(key, sizeof key, "R%ld:%ld", kv.first.first, kv.first.second);
        c.key = key;
        all.push_back(c);
    }
    res.echoBlocks = (int)all.size();
    // Keep the most relevant: close and intense first.
    std::set<std::string> prevKeys = prev ? *prev : std::set<std::string>();
    auto score = [&](const RadarCell& c) { return c.distNm - 2.0 * (c.dbz - 20.0) - (prevKeys.count(c.key) ? 25.0 : 0.0); };
    std::sort(all.begin(), all.end(), [&](const RadarCell& a, const RadarCell& b) { return score(a) < score(b); });
    if ((int)all.size() > cfg.radarMaxCells) all.resize((size_t)cfg.radarMaxCells);
    res.cells = std::move(all);
    res.ok = true;
    res.topsFromRadar = tops != nullptr;
    return res;
}

// ---------------------------------------------------------------------------------------------------------
// RainViewer (experimental): the free tiles are coloured images, not data, so intensity is inferred from colour.

int loadRainViewerPalette(const std::string& path) {
    std::ifstream f(path);
    if (!f) return 0;
    std::string line;
    std::vector<PalEntry> p;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        PalEntry e;
        float r, g, b, d;
        if (std::sscanf(line.c_str(), "%f , %f , %f , %f", &r, &g, &b, &d) == 4) { e.r = (int)r; e.g = (int)g; e.b = (int)b; e.dbz = d; p.push_back(e); }
    }
    g_pal = p;
    return (int)p.size();
}

float dbzFromRainViewerPixel(int r, int g, int b, int a) {
    if (a < 24) return -99.0f;
    if (!g_pal.empty()) {
        int best = -1, bestD = 40 * 40 * 3;
        for (size_t i = 0; i < g_pal.size(); ++i) {
            int dr = r - g_pal[i].r, dg = g - g_pal[i].g, db = b - g_pal[i].b, d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = (int)i; }
        }
        return best >= 0 ? g_pal[(size_t)best].dbz : -99.0f;
    }
    // Approximation: stronger echoes are drawn darker / more saturated. Light = drizzle, dark = heavy rain.
    float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    return 10.0f + 45.0f * clampf(1.0f - lum, 0.0f, 1.0f);
}

void tileRange(double lat, double lon, double rangeNm, int z, int& x0, int& x1, int& y0, int& y1) {
    double dLat = rangeNm / 60.0, dLon = rangeNm / (60.0 * std::max(0.2, std::cos(lat * kPi / 180.0)));
    auto tx = [&](double lo) { return (int)std::floor((lo + 180.0) / 360.0 * (1 << z)); };
    auto ty = [&](double la) {
        la = clampf((float)la, -85.0f, 85.0f);
        double r = la * kPi / 180.0;
        return (int)std::floor((1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * (1 << z));
    };
    int n = 1 << z;
    x0 = std::max(0, tx(lon - dLon)); x1 = std::min(n - 1, tx(lon + dLon));
    y0 = std::max(0, ty(lat + dLat)); y1 = std::min(n - 1, ty(lat - dLat));
}

bool tilesToField(const std::vector<TileImage>& tiles, const GribWindow& win, GribField& out) {
    out = GribField();
    if (tiles.empty()) { out.error = "no tiles"; return false; }
    const double step = 0.02;   // output spacing in degrees (about 1.2 nm)
    int z = tiles[0].z;
    out.dLat = step; out.dLon = step;
    out.lat0 = win.latMax; out.lon0 = win.lonMin;
    out.ni = (int)std::floor((win.lonMax - win.lonMin) / step) + 1;
    out.nj = (int)std::floor((win.latMax - win.latMin) / step) + 1;
    if (out.ni <= 0 || out.nj <= 0 || (long)out.ni * out.nj > 4000000) { out.error = "bad window"; out.ni = out.nj = 0; return false; }
    const float kNone = -99.0f;
    out.v.assign((size_t)out.ni * (size_t)out.nj, kNone);
    std::map<std::pair<int, int>, const TileImage*> idx;
    for (const auto& t : tiles) idx[{t.x, t.y}] = &t;
    double n = (double)(1 << z);
    for (int j = 0; j < out.nj; ++j) {
        double la = out.lat0 - j * step;
        double r = clampf((float)la, -85.0f, 85.0f) * kPi / 180.0;
        double fy = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * n;
        for (int i = 0; i < out.ni; ++i) {
            double fx = (out.lon0 + i * step + 180.0) / 360.0 * n;
            auto it = idx.find({(int)std::floor(fx), (int)std::floor(fy)});
            if (it == idx.end()) continue;
            const TileImage& t = *it->second;
            int px = std::min(t.w - 1, (int)((fx - std::floor(fx)) * t.w)), py = std::min(t.h - 1, (int)((fy - std::floor(fy)) * t.h));
            const uint8_t* p = &t.rgba[((size_t)py * (size_t)t.w + (size_t)px) * 4];
            out.v[(size_t)j * (size_t)out.ni + (size_t)i] = dbzFromRainViewerPixel(p[0], p[1], p[2], p[3]);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------

void applyStorm(XpWx& x, const RadarCell& c, const Config& cfg, double fieldElevM, double lat) {
    const float dbz = c.dbz;
    // Precipitation: X-Plane takes 0..1.
    float p = clampf(c.rateMmH / (float)cfg.radarRainFullMmH, 0.05f, 1.0f);
    x.precip = std::max(x.precip, p);

    // Heavy rain cuts visibility.
    float cap = 1e9f;
    if (dbz >= 50) cap = 800; else if (dbz >= 45) cap = 1500; else if (dbz >= 40) cap = 2500;
    else if (dbz >= 35) cap = 4000; else if (dbz >= 30) cap = 6000; else if (dbz >= 25) cap = 8000;
    x.visM = std::min(x.visM, cap);

    // Storm cloud in slot 2 (slot 0 may hold ground fog; slot 1 the station's own cloud).
    // Real storms are not slabs: tops differ from tower to tower, weaker and edge cells are lower, and the cloud is
    // not solid. Uniform tops and full coverage in neighbouring blocks merge into a flat-topped mesa.
    float limit = tropopauseKm(lat) * 1000.0f + 2500.0f;
    float baseM = (float)fieldElevM + (dbz >= 40 ? 900.0f : 600.0f);
    for (int i = 0; i < 3; ++i)    // do not float above the station's own lowest layer
        if (x.clouds[i].coverage > 0 && x.clouds[i].baseM > fieldElevM + 150 && !(x.fogLayerApplied && i == 0)) baseM = std::min(baseM, x.clouds[i].baseM);
    float topM = c.topM > 0 ? c.topM : estimateTopM(dbz, lat);
    float intensity = clampf((dbz - 20.0f) / 20.0f, 0.0f, 1.0f);              // 20 dBZ -> 0, 40 dBZ and up -> 1
    float depth = std::max(0.0f, topM - baseM);
    depth *= (0.45f + 0.55f * intensity) * (0.7f + 0.3f * c.frac);            // weaker and edge cells are shallower
    topM = baseM + depth;
    uint32_t h = 2166136261u;                                                  // FNV-1a of the block key: stable between updates
    for (char ch : c.key) { h ^= (uint8_t)ch; h *= 16777619u; }
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;   // avalanche: neighbouring keys differ a lot
    float j = ((float)(h & 0xFFFFu) / 32767.5f) - 1.0f;                        // -1 .. 1
    topM += j * (float)cfg.stormTopJitterM * (0.4f + 0.6f * intensity);
    topM = std::min(topM, limit);
    if (topM < baseM + 1500.0f) topM = baseM + 1500.0f;
    XpCloud& cl = x.clouds[2];
    cl.type = clampf(1.0f + (dbz - 20.0f) / 12.5f, 1.0f, 3.0f);               // cumulonimbus from about 45 dBZ
    float t = clampf((dbz - 20.0f) / 35.0f, 0.0f, 1.0f);
    cl.coverage = (0.35f + ((float)cfg.stormCoverageMax - 0.35f) * t) * (0.7f + 0.3f * c.frac);
    cl.baseM = baseM;
    cl.topM = topM;
    x.cloudCount = 0;
    for (const auto& k : x.clouds) if (k.coverage > 0) ++x.cloudCount;

    // Turbulence and gusts below the storm top.
    float turb = clampf((dbz - 30.0f) / 30.0f, 0.0f, 1.0f) * (float)cfg.radarTurbulence;
    if (turb > 0) {
        if (x.winds.empty() && x.hasWind) {
            XpWind w;
            w.altMsl = x.windAltMsl; w.dir = x.windDir; w.ms = x.windMs; w.gustMs = x.gustMs;
            x.winds.push_back(w);
        }
        for (auto& w : x.winds) {
            if (w.altMsl > topM) continue;
            w.turbulence = std::max(w.turbulence, std::min(1.0f, turb));
            w.gustMs = std::max(w.gustMs, w.ms + 10.0f * turb);
        }
        x.turbulence = std::max(x.turbulence, std::min(1.0f, turb));
        x.gustMs = std::max(x.gustMs, x.windMs + 10.0f * turb);
    }
    float topFt = topM / 0.3048f;
    if (x.maxAltFt < topFt) x.maxAltFt = topFt;
}

} // namespace wx
