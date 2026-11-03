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

void smoothStormTops(std::vector<RadarCell>& cells, double cellNm, double blend, double stepM) {
    std::vector<float> orig(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) orig[i] = cells[i].topM;
    const double reach = cellNm * 1.7;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (!(cells[i].topM > 0) || cells[i].synthetic) continue;
        double sum = 0;
        int n = 0;
        for (size_t j = 0; j < cells.size(); ++j) {
            if (j == i || !(orig[j] > 0) || cells[j].synthetic) continue;
            if (distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon) <= reach) { sum += orig[j]; ++n; }
        }
        float cap = n == 0 ? 8000.0f : (float)(sum / n) + 2000.0f;
        if (cells[i].topM > cap) cells[i].topM = std::max(1500.0f, std::round(cap / 500.0f) * 500.0f);
    }
    // optional real smoothing: pull each top toward the distance-weighted mean of its neighbours (rounds the steps between cells)
    if (blend > 0.0 && stepM > 0.0) {
        std::vector<float> clipped(cells.size());
        for (size_t i = 0; i < cells.size(); ++i) clipped[i] = cells[i].topM;
        const double sigma = cellNm * 0.9;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (!(clipped[i] > 0) || cells[i].synthetic) continue;
            double sum = 0, ws = 0;
            for (size_t j = 0; j < cells.size(); ++j) {
                if (j == i || !(clipped[j] > 0) || cells[j].synthetic) continue;
                const double d = distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon);
                if (d > reach) continue;
                const double w = std::exp(-(d * d) / (2.0 * sigma * sigma));
                sum += clipped[j] * w; ws += w;
            }
            if (ws <= 0) continue;
            const float mean = (float)(sum / ws);
            // smoothing rounds the steps between cells but never takes more than 1.5 km off a measured top (or adds more), so tall cores stay tall
            const float t = std::min(clipped[i] + 1500.0f, std::max(clipped[i] - 1500.0f, (float)((1.0 - blend) * clipped[i] + blend * mean)));
            cells[i].topM = std::max(1500.0f, (float)(std::round(t / stepM) * stepM));
        }
    }
}

void smoothStormRain(std::vector<RadarCell>& cells, double cellNm, double blend) {
    if (blend <= 0.0) return;
    std::vector<float> orig(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) orig[i] = cells[i].dbzFine;
    const double sigma = cellNm * 0.9, reach = cellNm * 1.7;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cells[i].synthetic) continue;
        double sum = 0, ws = 0;
        for (size_t j = 0; j < cells.size(); ++j) {
            if (j == i || cells[j].synthetic) continue;
            const double d = distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon);
            if (d > reach) continue;
            const double w = std::exp(-(d * d) / (2.0 * sigma * sigma));
            sum += orig[j] * w; ws += w;
        }
        if (ws <= 0) continue;
        const float f = (float)((1.0 - blend) * orig[i] + blend * (sum / ws));
        cells[i].dbzFine = std::max(f, 0.0f);   // may rise or fall: this is smoothing, not a limiter
        cells[i].rateMmH = rateFromDbz(cells[i].dbzFine);
    }
}

float stormPrecipAt(const std::vector<RadarCell>& cells, double lat, double lon, double reachNm, float rainFullMmH) {
    double sw = 0, sp = 0;
    for (const auto& c : cells) {
        const double d = distNm(lat, lon, c.lat, c.lon);
        if (d > reachNm) continue;
        const double w = 1.0 / (0.25 * reachNm + d);
        sw += w; sp += w * clampf(c.rateMmH / std::max(1.0f, rainFullMmH), 0.05f, 1.0f);
    }
    return sw > 0 ? (float)(sp / sw) : 0.0f;
}

int fillStormHoles(std::vector<RadarCell>& kept, const std::vector<RadarCell>& dropped, double cellNm, double aircraftLat, double aircraftLon,
                   double stepM, double minDbz, int minNeighbours, int maxAdd) {
    if (kept.size() < (size_t)minNeighbours || maxAdd <= 0) return 0;
    int added = 0;
    const double cosLat = std::max(0.2, std::cos(kept[0].lat * kPi / 180.0));
    for (int pass = 0; pass < 2 && added < maxAdd; ++pass) {
        struct Cand { double lat, lon; const RadarCell* src; };
        std::vector<Cand> cands;
        for (const auto& d : dropped) cands.push_back({d.lat, d.lon, &d});
        for (const auto& k : kept)
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    if (!dj && !di) continue;
                    cands.push_back({k.lat + dj * cellNm / 60.0, k.lon + di * cellNm / 60.0 / std::max(0.2, std::cos(k.lat * kPi / 180.0)), nullptr});
                }
        std::vector<RadarCell> fresh;
        for (const auto& c : cands) {
            bool taken = false;
            for (const auto& k : kept) if (distNm(c.lat, c.lon, k.lat, k.lon) < cellNm * 0.55) { taken = true; break; }
            if (!taken) for (const auto& f : fresh) if (distNm(c.lat, c.lon, f.lat, f.lon) < cellNm * 0.55) { taken = true; break; }
            if (taken) continue;
            int n = 0; bool north = false, south = false, east = false, west = false;
            double sDbz = 0, sFrac = 0, sTop = 0; int nTop = 0;
            for (const auto& k : kept) {
                const double d = distNm(c.lat, c.lon, k.lat, k.lon);
                if (d > cellNm * 1.45) continue;
                ++n; sDbz += k.dbz; sFrac += k.frac;
                if (k.topM > 0) { sTop += k.topM; ++nTop; }
                const double dy = (k.lat - c.lat) * 60.0, dx = (k.lon - c.lon) * 60.0 * cosLat;
                if (dy > cellNm * 0.3) north = true;
                if (dy < -cellNm * 0.3) south = true;
                if (dx > cellNm * 0.3) east = true;
                if (dx < -cellNm * 0.3) west = true;
            }
            if (n < minNeighbours || !(north && south && east && west)) continue;
            RadarCell h;
            h.lat = c.lat; h.lon = c.lon;
            h.dbz = std::max((float)minDbz, std::round((float)(sDbz / n) / 5.0f) * 5.0f);
            h.frac = std::max(0.25f, std::round((float)(sFrac / n) * 4.0f) / 4.0f);
            h.topKnown = nTop > 0;
            h.topM = nTop > 0 ? (float)(std::round(sTop / nTop / stepM) * stepM) : std::round(estimateTopM(h.dbz, h.lat) / (float)stepM) * (float)stepM;
            h.dbzFine = (float)(sDbz / n);
            h.rateMmH = rateFromDbz(h.dbzFine);
            h.distNm = distNm(aircraftLat, aircraftLon, h.lat, h.lon);
            if (c.src) h.key = c.src->key;
            else { char kb[48]; std::snprintf(kb, sizeof kb, "H%ld:%ld", (long)std::lround(h.lat * 100.0), (long)std::lround(h.lon * 100.0)); h.key = kb; }
            fresh.push_back(h);
            if (added + (int)fresh.size() >= maxAdd) break;
        }
        if (fresh.empty()) break;
        added += (int)fresh.size();
        for (auto& f : fresh) kept.push_back(f);
    }
    return added;
}

namespace {
unsigned hashStr(const std::string& s, int k) {
    unsigned h = 2166136261u ^ (unsigned)(k * 40503);
    for (unsigned char ch : s) { h ^= ch; h *= 16777619u; }
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}
double unit(unsigned h, int salt) { return (double)(hashStr(std::to_string(h), salt) % 10001u) / 10000.0; }   // 0..1
}

int addStormRim(std::vector<RadarCell>& cells, double cellNm, double aircraftLat, double aircraftLon, double stepM, double minDbz, int maxAdd, int rings) {
    if (cells.empty() || maxAdd <= 0) return 0;
    rings = std::max(1, std::min(3, rings));
    const size_t nReal = cells.size();
    std::vector<RadarCell> fresh;
    std::vector<size_t> sources;                 // indices into `fresh` of the previous ring (ring 1 uses the real cells)
    for (int ring = 1; ring <= rings && (int)fresh.size() < maxAdd; ++ring) {
        std::vector<size_t> next;
        const size_t nSrc = ring == 1 ? nReal : sources.size();
        for (size_t si = 0; si < nSrc && (int)fresh.size() < maxAdd; ++si) {
            const RadarCell c = ring == 1 ? cells[si] : fresh[sources[si]];
            const double cosL = std::max(0.2, std::cos(c.lat * kPi / 180.0));
            for (int k = 0; k < 8 && (int)fresh.size() < maxAdd; ++k) {
                const unsigned h = hashStr(c.key, k + 8 * ring);
                const double ang = (k * 45.0 + (unit(h, 1) - 0.5) * 40.0) * kPi / 180.0;
                const double dist = cellNm * (ring == 1 ? 0.75 + 0.35 * unit(h, 2) : 0.55 + 0.3 * unit(h, 2));
                const double plat = c.lat + std::cos(ang) * dist / 60.0, plon = c.lon + std::sin(ang) * dist / 60.0 / cosL;
                bool taken = false;
                const double gap = ring == 1 ? 0.7 : 0.5;
                for (size_t j = 0; j < nReal && !taken; ++j) taken = distNm(plat, plon, cells[j].lat, cells[j].lon) < cellNm * gap;
                for (size_t j = 0; j < fresh.size() && !taken; ++j) taken = distNm(plat, plon, fresh[j].lat, fresh[j].lon) < cellNm * (gap - 0.1);
                if (taken) continue;
                // inner corner: two or more confirmed blocks right next to this spot (a straight side only has its own block that close)
                int n = 0; double sDbz = 0, sFrac = 0, sTop = 0; int nTop = 0;
                for (size_t j = 0; ring == 1 && j < nReal; ++j) {
                    if (distNm(plat, plon, cells[j].lat, cells[j].lon) > cellNm * 1.2) continue;
                    ++n; sDbz += cells[j].dbzFine; sFrac += cells[j].frac;
                    if (cells[j].topM > 0) { sTop += cells[j].topM; ++nTop; }
                }
                const bool corner = ring == 1 && n >= 2;
                if (!corner && unit(h, 3) > (ring == 1 ? 0.7 : 0.5)) continue;   // only some directions get a cell: the outline stays uneven
                RadarCell r;
                r.lat = plat; r.lon = plon; r.synthetic = true;
                if (corner) {
                    r.dbzFine = std::max((float)minDbz, (float)(sDbz / n) - 3.0f);
                    r.frac = std::max(0.25f, std::round((float)(sFrac / n) * 4.0f) / 4.0f);
                    r.topM = nTop > 0 ? (float)(sTop / nTop * 0.9) : 0.0f;
                    r.radiusScale = (float)(0.9 + 0.15 * unit(h, 4));
                } else if (ring == 1) {
                    r.dbzFine = std::max((float)minDbz, c.dbzFine - (float)(6.0 + 10.0 * unit(h, 5)));   // lighter and lower toward the outside
                    r.frac = std::max(0.25f, c.frac * 0.6f);
                    r.topM = c.topM > 0 ? std::max(3000.0f, c.topM * (float)(0.5 + 0.3 * unit(h, 6))) : 0.0f;
                    r.radiusScale = (float)(0.5 + 0.35 * unit(h, 7));
                } else {                                      // outer rings: fainter, lower, smaller again, so the edge fades out instead of ending at a wall
                    r.dbzFine = std::max((float)minDbz, c.dbzFine - (float)(4.0 + 6.0 * unit(h, 5)));
                    r.frac = std::max(0.25f, c.frac * 0.75f);
                    r.topM = c.topM > 0 ? std::max(3000.0f, c.topM * (float)(0.55 + 0.25 * unit(h, 6))) : 0.0f;
                    r.radiusScale = std::max(0.4f, c.radiusScale * (float)(0.65 + 0.2 * unit(h, 7)));
                }
                r.topKnown = r.topM > 0;
                if (r.topM > 0 && stepM > 0) r.topM = std::round(r.topM / (float)stepM) * (float)stepM;
                r.dbz = std::max(5.0f, std::round(r.dbzFine / 5.0f) * 5.0f);
                r.rateMmH = rateFromDbz(r.dbzFine);
                r.distNm = distNm(aircraftLat, aircraftLon, r.lat, r.lon);
                r.key = "Q" + std::to_string(ring) + c.key + ":" + std::to_string(k);
                fresh.push_back(r);
                if (!corner) next.push_back(fresh.size() - 1);
            }
        }
        sources = next;
    }
    for (auto& f : fresh) cells.push_back(f);
    return (int)fresh.size();
}

void markStormEdges(std::vector<RadarCell>& cells, double cellNm) {
    const double reach = cellNm * 1.7;
    for (size_t i = 0; i < cells.size(); ++i) {
        int n = 0;
        for (size_t j = 0; j < cells.size(); ++j)
            if (j != i && distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon) <= reach) ++n;
        cells[i].edge = clampf((5.0f - (float)n) / 5.0f, 0.0f, 1.0f);
    }
}

RadarResult buildRadarCells(const GribField& refl, const GribField* tops, double lat, double lon, const Config& cfg,
                            const std::set<std::string>* prev) {
    RadarResult res;
    res.attempted = true;
    if (!refl.valid()) { res.note = "no reflectivity grid"; return res; }
    const double blockLat = cfg.radarCellNm / 60.0;
    struct Acc { float maxDbz = -999; int n = 0; float top = 0; double lat = 0, lon = 0, step = 0; double sw = 0, sLat = 0, sLon = 0; };
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
            { const double w = std::max(1.0f, v - 15.0f); a.sw += w; a.sLat += w * la; a.sLon += w * lo; }
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
        if (cfg.stormCentroid && a.sw > 0) { c.lat = a.sLat / a.sw; c.lon = a.sLon / a.sw; }   // centre of the echo, not of the grid block
        if (a.maxDbz < minDbz - 5.0f) continue;
        c.dbz = std::max(5.0f, std::round(a.maxDbz / 5.0f) * 5.0f);
        double pxPerBlock = (blockLat / refl.dLat) * (a.step / refl.dLon);
        c.frac = std::max(0.25f, std::round(clampf((float)(a.n / std::max(1.0, pxPerBlock)), 0.0f, 1.0f) * 4.0f) / 4.0f);
        c.topKnown = a.top > 0;
        const float step = (float)std::max(50.0, cfg.stormTopStepM);
        c.topM = c.topKnown ? std::round(a.top * 1000.0f / step) * step : std::round(estimateTopM(c.dbz, c.lat) / step) * step;
        if ((!c.topKnown && cfg.stormTopVariationM > 0) || (c.topKnown && cfg.stormTopVariationKnownM > 0)) {
            const double amp = c.topKnown ? cfg.stormTopVariationKnownM : cfg.stormTopVariationM;
            // one estimate per intensity makes every such storm the same height (a flat plateau with straight walls): vary each
            // block by a fixed amount derived from its key, so the height does not change from one update to the next
            char hk[48];
            std::snprintf(hk, sizeof hk, "R%ld:%ld", kv.first.first, kv.first.second);
            unsigned h = 2166136261u;
            for (const char* q = hk; *q; ++q) { h ^= (unsigned char)*q; h *= 16777619u; }
            const double v = (double)(h % 2001u) / 1000.0 - 1.0;   // -1 .. +1
            c.topM = std::max(3000.0f, std::round((c.topM + (float)(v * amp)) / step) * step);
        }
        c.dbzFine = std::max(5.0f, std::round(std::min(a.maxDbz, 70.0f)));
        c.rateMmH = rateFromDbz(c.dbzFine);
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
    std::vector<RadarCell> droppedCells;
    if ((int)all.size() > cfg.radarMaxCells) { droppedCells.assign(all.begin() + cfg.radarMaxCells, all.end()); all.resize((size_t)cfg.radarMaxCells); }
    if (cfg.stormFillHoles) res.holesFilled = fillStormHoles(all, droppedCells, cfg.radarCellNm, lat, lon, std::max(50.0, cfg.stormTopStepM), cfg.radarMinDbz, 5, std::max(4, cfg.radarMaxCells / 4));
    if (cfg.stormRim) res.rimCells = addStormRim(all, cfg.radarCellNm, lat, lon, std::max(50.0, cfg.stormTopStepM), cfg.radarMinDbz, std::max(4, cfg.radarMaxCells * 3 / 4), cfg.stormRimRings);
    if (cfg.stormSmoothTops) smoothStormTops(all, cfg.radarCellNm, cfg.stormTopBlend, cfg.stormTopStepM);
    smoothStormRain(all, cfg.radarCellNm, cfg.stormRainBlend);
    markStormEdges(all, cfg.radarCellNm);
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

float stormTopM(const RadarCell& c, const Config&, float baseM) {
    // The radar echo top (or, without echo-top data, an estimate from reflectivity), capped at the tropopause + 2.5 km.
    // X-Plane's own weather state (the D/RWX dumps in Log.txt) shows cumulonimbus tops of exactly 45000.00 ft, so that is the sim's ceiling.
    const float limit = std::min(tropopauseKm(c.lat) * 1000.0f + 2500.0f, kXpTopM);
    float topM = std::min(c.topM > 0 ? c.topM : estimateTopM(c.dbz, c.lat), limit);
    if (topM < baseM + 1500.0f) topM = baseM + 1500.0f;
    return topM;
}

void applyStorm(XpWx& x, const RadarCell& c, const Config& cfg, double fieldElevM, double /*lat*/) {
    const float dbz = c.dbz;
    // Precipitation: X-Plane takes 0..1.
    float p = clampf(c.rateMmH / (float)cfg.radarRainFullMmH, 0.05f, 1.0f);
    x.precip = std::max(x.precip, p);

    // Heavy rain cuts visibility.
    float cap = 1e9f;
    if (dbz >= 50) cap = 800; else if (dbz >= 45) cap = 1500; else if (dbz >= 40) cap = 2500;
    else if (dbz >= 35) cap = 4000; else if (dbz >= 30) cap = 6000; else if (dbz >= 25) cap = 8000;
    x.visM = std::min(x.visM, cap);

    // Slot choice: the station's own layers must survive. Use the top slot if it is free, else the middle one; only when all
    // are taken does the storm replace the highest of them (slot 0 may hold ground fog and is never used).
    int slot = 2;
    if (x.clouds[2].coverage > 0 && x.clouds[1].coverage <= 0) slot = 1;   // else every slot is taken: slot 2 (the highest, layers are packed bottom-up) is replaced
    float baseM = (float)fieldElevM + (dbz >= 40 ? 900.0f : 600.0f);
    if (x.clouds[slot].coverage > 0 && x.clouds[slot].baseM <= fieldElevM + 2500.0f) {
        // X-Plane blends records slot by slot, bases and tops included. A storm base 700 m below the deck it replaces turned the
        // blend at the station into a thin 560-6000 m column (seen in a log at KFYV), so the storm grows from the replaced deck's base.
        baseM = x.clouds[slot].baseM;
    } else {
        for (int i = 0; i < 3; ++i)    // otherwise do not float above the station's own lowest layer
            if (x.clouds[i].coverage > 0 && x.clouds[i].baseM > fieldElevM + 150 && !(x.fogLayerApplied && i == 0)) baseM = std::min(baseM, x.clouds[i].baseM);
    }
    float topM = stormTopM(c, cfg, baseM);
    // Rim of a cluster: up to 45% less height above the base (never below the 1500 m minimum) so the system slopes off instead of ending in a wall.
    if (c.edge > 0) topM = std::max(baseM + 1500.0f, baseM + (topM - baseM) * (1.0f - 0.45f * c.edge));
    XpCloud& cl = x.clouds[slot];
    const float replacedCov = cl.coverage;   // the deck this storm grows out of (0 if the slot was free)
    // Cumulonimbus from 30 dBZ up (and for any measured echo top above 6 km); a weak echo is cumulus (2.0), not a deep anvil. Ramping
    // further down to 1.0 made smooth stratus sheets with curtain edges, so the floor stays at cumulus.
    cl.type = (dbz >= 30.0f || (c.topKnown && c.topM > 6000.0f)) ? 3.0f : 2.0f;
    float t = clampf((dbz - 20.0f) / 25.0f, 0.0f, 1.0f);
    cl.coverage = (0.5f + ((float)cfg.stormCoverageMax - 0.5f) * t) * (0.8f + 0.2f * c.frac);
    cl.coverage *= 1.0f - 0.25f * c.edge;   // and up to 25% less coverage on the rim (the station deck it replaces still sets the floor below)
    // A storm inside a solid deck must not thin it: X-Plane blends coverage slot by slot, so a 58% storm over a 94% overcast punched a
    // sinuous hole-edged contour through the sheet (KFYV log). The station's own coverage is the floor.
    cl.coverage = std::max(cl.coverage, replacedCov);
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
