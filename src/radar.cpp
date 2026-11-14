#include "radar.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <queue>
#include <map>
#include <sstream>

#include "geo.h"

namespace wx {
namespace {

const double kPi = 3.14159265358979323846;
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }


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

void smoothStormTops(std::vector<RadarCell>& cells, double cellNm, double blend, double stepM, bool fillPits) {
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
        // the opposite of a spike: a pit. A block much lower than the ring of blocks around it (typically one with a weak echo and no echo top,
        // whose height was only estimated from reflectivity) renders as a sheer rectangular crater in the middle of a storm. Lift it toward its neighbours.
        else if (fillPits && n >= 4 && cells[i].topM < 0.6f * (float)(sum / n))
            cells[i].topM = std::round((float)(sum / n) / 500.0f) * 500.0f;
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

void limitStormTopSlope(std::vector<RadarCell>& cells, double cellNm, double maxDropM, double stepM) {
    if (maxDropM <= 0 || cellNm <= 0) return;
    const size_t n = cells.size();
    std::vector<float> start(n);
    for (size_t i = 0; i < n; ++i) start[i] = cells[i].topM;
    const double reach = cellNm * 1.5;
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<float> delta(n, 0.0f);
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (!(cells[i].topM > 0) || cells[i].synthetic) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (!(cells[j].topM > 0) || cells[j].synthetic) continue;
                const double d = distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon);
                if (d > reach) continue;
                const double allowed = maxDropM * std::max(0.5, d / cellNm);
                const double diff = (double)cells[i].topM - cells[j].topM;
                const double excess = std::fabs(diff) - allowed;
                if (excess <= 0) continue;
                any = true;
                const float h = (float)(excess * 0.5);
                if (diff > 0) { delta[i] -= h; delta[j] += h; } else { delta[i] += h; delta[j] -= h; }
            }
        }
        if (!any) break;
        for (size_t i = 0; i < n; ++i) {
            if (delta[i] == 0.0f) continue;
            // a cell moves at most 2.5 km from where it started (its own measurement stays recognisable)
            float t = std::min(start[i] + 2500.0f, std::max(start[i] - 2500.0f, cells[i].topM + delta[i] * 0.7f));
            cells[i].topM = std::max(1500.0f, t);
        }
    }
    if (stepM > 0)
        for (auto& c : cells) if (c.topM > 0 && !c.synthetic) c.topM = std::max(1500.0f, (float)(std::round(c.topM / stepM) * stepM));
}

void smoothStormRain(std::vector<RadarCell>& cells, double cellNm, double blend, bool fillPits) {
    if (blend <= 0.0 && !fillPits) return;
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
    if (!fillPits) return;
    // pits: judged on the unsmoothed values of the 8 surrounding blocks
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cells[i].synthetic) continue;
        double sum = 0; int n = 0;
        for (size_t j = 0; j < cells.size(); ++j) {
            if (j == i || cells[j].synthetic) continue;
            if (distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon) <= cellNm * 1.5) { sum += orig[j]; ++n; }
        }
        if (n < 4) continue;
        const float mean = (float)(sum / n);
        if (cells[i].dbzFine < mean - 6.0f) {
            cells[i].dbzFine = mean - 2.0f;
            cells[i].rateMmH = rateFromDbz(cells[i].dbzFine);
        }
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
    for (int pass = 0; pass < 3 && added < maxAdd; ++pass) {
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
            std::vector<std::pair<double, double>> vec;
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
                const double len = std::sqrt(dx * dx + dy * dy);
                if (len > 1e-9) vec.push_back({dx / len, dy / len});
            }
            // enclosed on all four sides (a hole), or between confirmed blocks on two opposite sides (a channel: open at its ends, so a plain
            // hole test never filled it, and X-Plane renders such a one-block-wide gap as a sheer canyon through the storm)
            const bool hole = north && south && east && west && n >= minNeighbours;
            bool opposite = false;                                 // two blocks on (nearly) opposite sides: more than ~150 degrees apart
            for (size_t a = 0; a < vec.size() && !opposite; ++a)
                for (size_t b = a + 1; b < vec.size() && !opposite; ++b) opposite = vec[a].first * vec[b].first + vec[a].second * vec[b].second < -0.87;
            const bool channel = n >= 3 && opposite;
            if (!hole && !channel) continue;
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
                    r.coverScale = 0.85f;
                } else if (ring == 1) {
                    r.dbzFine = std::max((float)minDbz, c.dbzFine - (float)(6.0 + 10.0 * unit(h, 5)));   // lighter and lower toward the outside
                    r.frac = std::max(0.25f, c.frac * 0.6f);
                    r.topM = c.topM > 0 ? std::max(3000.0f, c.topM * (float)(0.5 + 0.3 * unit(h, 6))) : 0.0f;
                    r.radiusScale = (float)(0.5 + 0.35 * unit(h, 7));
                    r.coverScale = 0.6f;
                } else {                                      // outer rings: fainter, lower, smaller again, so the edge fades out instead of ending at a wall
                    r.dbzFine = std::max((float)minDbz, c.dbzFine - (float)(4.0 + 6.0 * unit(h, 5)));
                    r.frac = std::max(0.25f, c.frac * 0.75f);
                    r.topM = c.topM > 0 ? std::max(3000.0f, c.topM * (float)(0.55 + 0.25 * unit(h, 6))) : 0.0f;
                    r.radiusScale = std::max(0.4f, c.radiusScale * (float)(0.65 + 0.2 * unit(h, 7)));
                    r.coverScale = std::max(0.3f, c.coverScale * 0.75f);
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

namespace {
// Value noise in -1..1 on an absolute lattice (so the wobble of an outline is the same from one update to the next).
double latticeNoise(double lat, double lon, double cellNm) {
    const double u = lon * 60.0 / (cellNm * 1.3), v = lat * 60.0 / cellNm;
    const long iu = (long)std::floor(u), iv = (long)std::floor(v);
    const double fu = u - iu, fv = v - iv;
    auto val = [&](long a, long b) { return unit(hashStr(std::to_string(a) + "," + std::to_string(b), 77), 3) * 2.0 - 1.0; };
    const double su = fu * fu * (3 - 2 * fu), sv = fv * fv * (3 - 2 * fv);
    const double top = val(iu, iv) * (1 - su) + val(iu + 1, iv) * su, bot = val(iu, iv + 1) * (1 - su) + val(iu + 1, iv + 1) * su;
    return top * (1 - sv) + bot * sv;
}
}   // namespace

int addStormContour(std::vector<RadarCell>& cells, double cellNm, double baseRadiusNm, double aircraftLat, double aircraftLon,
                    double stepM, double minDbz, int maxAdd) {
    if (cells.empty() || cellNm <= 0) return 0;
    double lat0 = 0, lon0 = 0;
    for (const auto& c : cells) { lat0 += c.lat; lon0 += c.lon; }
    lat0 = std::round(lat0 / cells.size()); lon0 = std::round(lon0 / cells.size());
    const double cosL = std::max(0.2, std::cos(lat0 * kPi / 180.0));
    auto toX = [&](double lon) { return (lon - lon0) * 60.0 * cosL; };
    auto toY = [&](double lat) { return (lat - lat0) * 60.0; };
    const double h = cellNm * 0.5, sigma = cellNm * 0.4, reachSq = std::pow(sigma * 2.6, 2.0);
    struct P { double x, y; };
    std::vector<P> pc;
    pc.reserve(cells.size());
    for (const auto& c : cells) if (!c.synthetic) pc.push_back({toX(c.lon), toY(c.lat)});
    if (pc.empty()) return 0;
    auto field = [&](double x, double y) {
        double keep = 1.0;
        for (const auto& p : pc) {
            const double dx = std::max(std::fabs(x - p.x) - h, 0.0), dy = std::max(std::fabs(y - p.y) - h, 0.0);
            const double dd = dx * dx + dy * dy;
            if (dd > reachSq) continue;
            keep *= 1.0 - std::exp(-dd / (sigma * sigma));
        }
        return 1.0 - keep;
    };
    auto inside = [&](double x, double y) {
        const double lat = lat0 + y / 60.0, lon = lon0 + x / (60.0 * cosL);
        return field(x, y) >= 0.5 + 0.12 * latticeNoise(lat, lon, cellNm);   // the wobble: the threshold moves by up to +-0.12
    };
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& p : pc) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const double g = cellNm / 6.0, pad = cellNm * 1.2;
    const int x0 = (int)std::floor((minX - pad) / g), x1 = (int)std::ceil((maxX + pad) / g);
    const int y0 = (int)std::floor((minY - pad) / g), y1 = (int)std::ceil((maxY + pad) / g);
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    if (W <= 2 || H <= 2 || (long)W * H > 400000) return 0;
    std::vector<unsigned char> in((size_t)W * H, 0);
    for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) in[(size_t)j * W + i] = inside((x0 + i) * g, (y0 + j) * g) ? 1 : 0;
    std::vector<P> boundary;
    for (int j = 1; j < H - 1; ++j)
        for (int i = 1; i < W - 1; ++i) {
            if (!in[(size_t)j * W + i]) continue;
            if (!in[(size_t)j * W + i - 1] || !in[(size_t)j * W + i + 1] || !in[(size_t)(j - 1) * W + i] || !in[(size_t)(j + 1) * W + i])
                boundary.push_back({(x0 + i) * g, (y0 + j) * g});
        }
    if (boundary.empty()) return 0;
    // 1) keep every measured block's circle inside the outline
    for (auto& c : cells) {
        if (c.synthetic) continue;
        const double cx = toX(c.lon), cy = toY(c.lat);
        double best = 1e9;
        for (const auto& b : boundary) best = std::min(best, std::hypot(b.x - cx, b.y - cy));
        c.contourScale = (float)clampf((float)((best + 0.05 * cellNm) / std::max(1.0, baseRadiusNm)), 0.4f, 1.0f);
    }
    // 2) skin cells just inside the outline, at random but evenly spread spots
    std::vector<size_t> order(boundary.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::vector<unsigned> hv(boundary.size());
    for (size_t i = 0; i < boundary.size(); ++i) hv[i] = hashStr(std::to_string((long)std::lround(boundary[i].x / g)) + "," + std::to_string((long)std::lround(boundary[i].y / g)) + "@" + std::to_string((long)lat0) + "," + std::to_string((long)lon0), 5);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return hv[a] < hv[b]; });
    std::vector<size_t> chosen;
    const double minSep = cellNm * 0.55;
    for (size_t oi : order) {
        bool ok = true;
        for (size_t ci : chosen) if (std::hypot(boundary[oi].x - boundary[ci].x, boundary[oi].y - boundary[ci].y) < minSep) { ok = false; break; }
        if (ok) chosen.push_back(oi);
    }
    std::vector<RadarCell> fresh;
    for (size_t oi : chosen) {
        const P b = boundary[oi];
        double gx = field(b.x + g, b.y) - field(b.x - g, b.y), gy = field(b.x, b.y + g) - field(b.x, b.y - g);
        double len = std::hypot(gx, gy);
        // the field falls off outwards: outward = -gradient
        double nx = len > 1e-9 ? -gx / len : 0.0, ny = len > 1e-9 ? -gy / len : 0.0;
        size_t src = 0; double bd = 1e18;
        size_t k = 0;
        for (size_t ci = 0; ci < cells.size(); ++ci) {
            if (cells[ci].synthetic) continue;
            const double d = std::hypot(pc[k].x - b.x, pc[k].y - b.y);
            ++k;
            if (d < bd) { bd = d; src = ci; }
        }
        const RadarCell& s = cells[src];
        const unsigned hh = hv[oi];
        const double depth = cellNm * (0.26 + 0.1 * unit(hh, 1));
        const double cx = b.x - nx * depth, cy = b.y - ny * depth;
        RadarCell r;
        r.lat = lat0 + cy / 60.0; r.lon = lon0 + cx / (60.0 * cosL);
        r.synthetic = true;
        r.dbzFine = std::max((float)minDbz, s.dbzFine - (float)(4.0 + 4.0 * unit(hh, 2)));
        r.frac = std::max(0.25f, std::round(s.frac * 0.7f * 4.0f) / 4.0f);
        r.topM = s.topM > 0 ? std::max(3000.0f, s.topM * (float)(0.6 + 0.2 * unit(hh, 3))) : 0.0f;
        r.topKnown = r.topM > 0;
        if (r.topM > 0 && stepM > 0) r.topM = std::round(r.topM / (float)stepM) * (float)stepM;
        r.dbz = std::max(5.0f, std::round(r.dbzFine / 5.0f) * 5.0f);
        r.rateMmH = rateFromDbz(r.dbzFine);
        r.coverScale = 0.7f;
        r.radiusScale = (float)clampf((float)((depth + 0.03 * cellNm) / std::max(1.0, baseRadiusNm)), 0.25f, 1.0f);
        r.distNm = distNm(aircraftLat, aircraftLon, r.lat, r.lon);
        r.key = "C" + std::to_string((long)std::lround(r.lat * 60.0 / (cellNm * 0.5))) + ":" + std::to_string((long)std::lround(r.lon * 60.0 * cosL / (cellNm * 0.5)));
        fresh.push_back(r);
    }
    std::sort(fresh.begin(), fresh.end(), [](const RadarCell& a, const RadarCell& b) { return a.distNm < b.distNm; });
    if ((int)fresh.size() > maxAdd) fresh.resize((size_t)std::max(0, maxAdd));
    for (auto& f : fresh) cells.push_back(f);
    return (int)fresh.size();
}

int addStormLevelContours(std::vector<RadarCell>& cells, double cellNm, double baseRadiusNm, double aircraftLat, double aircraftLon,
                          double stepM, const std::vector<double>& levels, int maxAdd) {
    if (cells.empty() || cellNm <= 0 || maxAdd <= 0 || levels.empty()) return 0;
    struct P { double x, y; };
    double lat0 = 0, lon0 = 0; int nr = 0;
    for (const auto& c : cells) if (!c.synthetic) { lat0 += c.lat; lon0 += c.lon; ++nr; }
    if (nr < 2) return 0;
    lat0 = std::round(lat0 / nr); lon0 = std::round(lon0 / nr);
    const double cosL = std::max(0.2, std::cos(lat0 * kPi / 180.0));
    auto toX = [&](double lon) { return (lon - lon0) * 60.0 * cosL; };
    auto toY = [&](double lat) { return (lat - lat0) * 60.0; };
    std::vector<size_t> realIdx;
    std::vector<P> all;
    for (size_t i = 0; i < cells.size(); ++i) if (!cells[i].synthetic) { realIdx.push_back(i); all.push_back({toX(cells[i].lon), toY(cells[i].lat)}); }
    // where the storm is (the soft union of rounded squares addStormContour draws its outline from)
    const double h = cellNm * 0.5, sigma = cellNm * 0.4, reachSq = std::pow(sigma * 2.6, 2.0);
    auto storm = [&](double x, double y) {
        double keep = 1.0;
        for (const auto& p : all) {
            const double dx = std::max(std::fabs(x - p.x) - h, 0.0), dy = std::max(std::fabs(y - p.y) - h, 0.0);
            const double dd = dx * dx + dy * dy;
            if (dd > reachSq) continue;
            keep *= 1.0 - std::exp(-dd / (sigma * sigma));
        }
        return 1.0 - keep;
    };
    // The reflectivity surface: an interpolation of the blocks' values that passes through each block's own value (inverse distance
    // squared, fading out by 1.8 blocks). A level's outline is where this surface crosses it, so a 57 dBZ block next to a 43 dBZ one puts the
    // 50 dBZ line halfway between them, and a core made of a rectangle of strong blocks comes out as the oval the radar saw, not as the
    // rectangle (outlining WHICH blocks are above a level still gave the rectangle). It passes through the blocks, so a single 42 dBZ block
    // still gets its own round 40 dBZ outline (a smoothing average pulled it under 40, and its square stayed).
    const double eps2 = std::pow(cellNm * 0.1, 2.0), reach2 = std::pow(cellNm * 1.8, 2.0);
    auto refl = [&](double x, double y) {
        double sw = 0, sv = 0;
        for (size_t k = 0; k < all.size(); ++k) {
            const double dd = (x - all[k].x) * (x - all[k].x) + (y - all[k].y) * (y - all[k].y);
            if (dd > reach2) continue;
            const double w = (1.0 - dd / reach2) / (dd + eps2);
            sw += w; sv += w * cells[realIdx[k]].dbzFine;
        }
        return sw > 1e-12 ? sv / sw : 0.0;
    };
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& p : all) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const double g = cellNm / 6.0, pad = cellNm * 0.6;
    const int x0 = (int)std::floor((minX - pad) / g), x1 = (int)std::ceil((maxX + pad) / g);
    const int y0 = (int)std::floor((minY - pad) / g), y1 = (int)std::ceil((maxY + pad) / g);
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    if (W <= 2 || H <= 2 || (long)W * H > 400000) return 0;
    std::vector<double> R((size_t)W * H, 0.0);
    std::vector<unsigned char> inStorm((size_t)W * H, 0);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const double x = (x0 + i) * g, y = (y0 + j) * g;
            inStorm[(size_t)j * W + i] = storm(x, y) >= 0.55 ? 1 : 0;   // inside the storm (a line only counts between two parts of it: the outer edge is addStormContour's)
            R[(size_t)j * W + i] = inStorm[(size_t)j * W + i] ? refl(x, y) : 0.0;
        }
    std::vector<double> sorted(levels);
    std::sort(sorted.begin(), sorted.end());
    std::vector<RadarCell> fresh;
    std::vector<P> allLines;   // every level's line, for step 3
    std::vector<unsigned char> unlined(realIdx.size(), 0);   // a block at or above a level that has no area of that level around it
    for (size_t li = 0; li < sorted.size(); ++li) {
        const double L = sorted[li];
        const double nextL = li + 1 < sorted.size() ? sorted[li + 1] : 999.0;
        const double prevL = li > 0 ? sorted[li - 1] : L - 10.0;
        // the line, with a gentle wobble of its own per level (+-1.5 dBZ), so it is not a perfectly smooth curve either
        auto above = [&](int i, int j) {
            const double x = (x0 + i) * g, y = (y0 + j) * g;
            const double lat = lat0 + y / 60.0, lon = lon0 + x / (60.0 * cosL);
            return R[(size_t)j * W + i] >= L + 1.5 * latticeNoise(lat + L * 0.37, lon - L * 0.21, cellNm);
        };
        std::vector<unsigned char> in((size_t)W * H, 0);
        for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) in[(size_t)j * W + i] = inStorm[(size_t)j * W + i] && above(i, j);
        // a block only just at this level (e.g. a lone 40 dBZ block) may get no area of it on the surface, so no line and no pairs:
        // it would still paint its whole square in that colour, so it gets the smaller circle (step 3) and shows as a round blob
        for (size_t k = 0; k < realIdx.size(); ++k) {
            if (cells[realIdx[k]].dbzFine < L) continue;
            const int i = (int)std::lround(all[k].x / g) - x0, j = (int)std::lround(all[k].y / g) - y0;
            if (i >= 0 && i < W && j >= 0 && j < H && !in[(size_t)j * W + i]) unlined[k] = 1;
        }
        std::vector<P> boundary;
        for (int j = 1; j < H - 1; ++j)
            for (int i = 1; i < W - 1; ++i) {
                if (!in[(size_t)j * W + i]) continue;
                bool edge = false;
                const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& d : nb) {
                    const size_t q = (size_t)(j + d[1]) * W + (i + d[0]);
                    if (!in[q] && inStorm[q]) edge = true;   // next to a weaker part of the storm (not to the storm's own edge)
                }
                if (edge) boundary.push_back({(x0 + i) * g, (y0 + j) * g});
            }
        if (boundary.empty()) continue;
        allLines.insert(allLines.end(), boundary.begin(), boundary.end());
        // Pairs of skin cells straddle the line at evenly spread, stable spots: one just inside at this level, one just outside at the level
        // below. X-Plane splits the area between two neighbouring records along a straight line, so between the big 12 nm block records a
        // level change follows the block grid. The pairs are smaller but close to the line, so near it they win, and the split between the
        // inner and the outer one of each pair lies ON the curve.
        std::vector<unsigned> hv(boundary.size());
        std::vector<size_t> order(boundary.size());
        for (size_t i = 0; i < boundary.size(); ++i) {
            order[i] = i;
            hv[i] = hashStr(std::to_string((long)std::lround(boundary[i].x / g)) + "," + std::to_string((long)std::lround(boundary[i].y / g)) + "@" +
                            std::to_string((long)lat0) + "," + std::to_string((long)lon0) + "L" + std::to_string((int)L), 9);
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return hv[a] < hv[b]; });
        std::vector<size_t> chosen;
        const double minSep = cellNm * 0.45;
        for (size_t oi : order) {
            bool ok = true;
            for (size_t ci : chosen) if (std::hypot(boundary[oi].x - boundary[ci].x, boundary[oi].y - boundary[ci].y) < minSep) { ok = false; break; }
            if (ok) chosen.push_back(oi);
        }
        for (size_t oi : chosen) {
            const P b = boundary[oi];
            const double gx = refl(b.x + g, b.y) - refl(b.x - g, b.y), gy = refl(b.x, b.y + g) - refl(b.x, b.y - g);
            const double len = std::hypot(gx, gy);
            const double nx = len > 1e-9 ? -gx / len : 0.0, ny = len > 1e-9 ? -gy / len : 0.0;   // outward = towards weaker echo
            const unsigned hh = hv[oi];
            const double depth = cellNm * (0.2 + 0.06 * unit(hh, 1));
            for (int side = 0; side < 2; ++side) {               // 0 = inside (this level), 1 = outside (the level below)
                const double sx = side == 0 ? -1.0 : 1.0;
                const double cx = b.x + sx * nx * depth, cy = b.y + sx * ny * depth;
                if (storm(cx, cy) < 0.55) continue;                // would sit outside the storm
                // the nearest block on this side of the line supplies coverage and top
                size_t src = realIdx[0]; double bd = 1e18;
                for (size_t k = 0; k < realIdx.size(); ++k) {
                    const RadarCell& c = cells[realIdx[k]];
                    if ((side == 0) != (c.dbzFine >= L)) continue;
                    const double d = std::hypot(all[k].x - cx, all[k].y - cy);
                    if (d < bd) { bd = d; src = realIdx[k]; }
                }
                const RadarCell& s = cells[src];
                RadarCell r;
                r.lat = lat0 + cy / 60.0; r.lon = lon0 + cx / (60.0 * cosL);
                r.synthetic = true;
                // inside: just above this level (below the next); outside: just below it (above the one before)
                r.dbzFine = side == 0 ? (float)std::min(nextL - 1.0, L + 2.0) : (float)std::max(prevL + 1.0, L - 2.0);
                r.frac = s.frac;
                r.topM = s.topM > 0 ? s.topM : 0.0f;
                r.topKnown = r.topM > 0;
                if (r.topM > 0 && stepM > 0) r.topM = std::round(r.topM / (float)stepM) * (float)stepM;
                r.dbz = std::max(5.0f, std::round(r.dbzFine / 5.0f) * 5.0f);
                r.rateMmH = rateFromDbz(r.dbzFine);
                r.coverScale = 1.0f;   // inside the storm: no thinning
                r.radiusScale = (float)clampf((float)(cellNm * 0.55 / std::max(1.0, baseRadiusNm)), 0.25f, 1.0f);
                r.distNm = distNm(aircraftLat, aircraftLon, r.lat, r.lon);
                r.key = std::string(side == 0 ? "L" : "M") + std::to_string((int)L) + ":" + std::to_string((long)std::lround(b.y / g)) + ":" +
                        std::to_string((long)std::lround(b.x / g)) + "@" + std::to_string((long)lat0) + "," + std::to_string((long)lon0);
                fresh.push_back(r);
            }
        }
    }
    // 3) A block whose centre lies close to a line would still win the area around it with its big circle, and the line would follow the
    // block grid there after all. Such blocks get a smaller circle (never below 55%, which still closes the gap to the neighbouring blocks),
    // so near the line the pairs decide. With pairs on both sides this hands no area to the wrong level.
    if (!allLines.empty() || std::count(unlined.begin(), unlined.end(), 1) > 0)
        for (size_t k = 0; k < realIdx.size(); ++k) {
            double best = unlined[k] ? 0.0 : 1e9;
            for (const auto& b : allLines) best = std::min(best, std::hypot(b.x - all[k].x, b.y - all[k].y));
            RadarCell& c = cells[realIdx[k]];
            c.contourScale = std::min(c.contourScale, (float)clampf((float)((best + 0.5 * cellNm) / std::max(1.0, baseRadiusNm)), 0.55f, 1.0f));
        }
    std::sort(fresh.begin(), fresh.end(), [](const RadarCell& a, const RadarCell& b) { return a.distNm < b.distNm; });
    if ((int)fresh.size() > maxAdd) fresh.resize((size_t)maxAdd);
    for (auto& f : fresh) cells.push_back(f);
    return (int)fresh.size();
}

void markStormEdges(std::vector<RadarCell>& cells, double cellNm) {
    const double reach = cellNm * 1.7;
    for (size_t i = 0; i < cells.size(); ++i) {
        int n = 0;
        for (size_t j = 0; j < cells.size(); ++j)
            if (j != i && !(cells[j].synthetic && !cells[j].key.empty() && (cells[j].key[0] == 'L' || cells[j].key[0] == 'M')) &&
                distNm(cells[i].lat, cells[i].lon, cells[j].lat, cells[j].lon) <= reach) ++n;
        cells[i].edge = clampf((5.0f - (float)n) / 5.0f, 0.0f, 1.0f);
    }
    // Corner cut: a confirmed block that has open ground on some of its 8 sides (a convex corner of the storm, or a side) is pulled in:
    // more edge taper, and a smaller circle, so its square corner is carved away and the outline ramps down in a curve instead of a corner.
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cells[i].synthetic) continue;
        const double cosL = std::max(0.2, std::cos(cells[i].lat * kPi / 180.0));
        int open = 0;
        for (int k = 0; k < 8; ++k) {
            const double a = k * 45.0 * kPi / 180.0;
            const double plat = cells[i].lat + std::cos(a) * cellNm / 60.0, plon = cells[i].lon + std::sin(a) * cellNm / 60.0 / cosL;
            bool covered = false;
            for (size_t j = 0; j < cells.size() && !covered; ++j)
                covered = j != i && !cells[j].synthetic && distNm(plat, plon, cells[j].lat, cells[j].lon) < cellNm * 0.6;
            if (!covered) ++open;
        }
        const float e = (float)open / 8.0f;
        cells[i].edge = std::max(cells[i].edge, e);
        cells[i].radiusScale = std::min(cells[i].radiusScale, 1.0f - 0.25f * e);   // a fully open block shrinks to 75%
    }
}

// ---------------------------------------------------------------------------------------------------------
// Natural radar picture.
//
// X-Plane gives every spot to (roughly) the nearest weather record, measured against each record's radius, so a picture made of equal
// 12 nm blocks shows 12 nm stair-steps wherever the rain changes. Here the mosaic itself is used instead of the blocks:
//  1. it is put on a 1 nm raster (the strongest of a few samples per cell), and lightly smoothed (Gaussian, about 1.5 nm), which keeps
//     the real shapes of the echoes but takes out single-pixel speckle;
//  2. it is warped a little with smooth random noise fixed to the ground (each spot reads the radar from up to ~2.5 nm away, and the
//     value moves by up to ~2 dBZ), so edges wander like real echoes instead of running along record rows, and stay put between updates;
//  3. squares on a fixed grid (24 nm) are split into four, again and again, wherever the picture inside changes intensity class (dry,
//     20, 30, 40, 50 dBZ): small records along every edge and every step inside a storm, big ones where it is all the same. Near the
//     aircraft down to radar_natural_fine_nm, further out coarser, and coarser still if the budget (radar_natural_max) runs short;
//  4. every square becomes one record whose radius is in proportion to its size, so between neighbours the split runs along the square
//     edges (which follow the warped picture at that resolution). Squares without rain become dry records: around the rain they take the
//     place of the gap-fill tiles, so the rain's edge is drawn by the fine records, not by the 12 nm tile grid.
namespace {
double fbm(double X, double Y, double scaleNm, int salt) {   // smooth noise -1..1 on absolute nm coordinates, two octaves
    auto lat = [&](double x, double y, double sc, int s) {
        const double u = x / sc + s * 17.31, v = y / sc - s * 9.73;
        const long iu = (long)std::floor(u), iv = (long)std::floor(v);
        const double fu = u - iu, fv = v - iv;
        auto val = [&](long a, long b) {   // integer hash (this runs a few hundred thousand times per update)
            uint32_t h = (uint32_t)a * 0x8da6b343u ^ (uint32_t)b * 0xd8163841u ^ (uint32_t)(s + 101) * 0xcb1ab31fu;
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
            return (double)(h % 20001u) / 10000.0 - 1.0;
        };
        const double su = fu * fu * (3 - 2 * fu), sv = fv * fv * (3 - 2 * fv);
        const double t = val(iu, iv) * (1 - su) + val(iu + 1, iv) * su, b = val(iu, iv + 1) * (1 - su) + val(iu + 1, iv + 1) * su;
        return t * (1 - sv) + b * sv;
    };
    return (lat(X, Y, scaleNm, salt) * 0.67 + lat(X, Y, scaleNm * 0.45, salt + 5) * 0.33);
}
}   // namespace

static void buildNaturalCells(const GribField& refl, const GribField* tops, double acLat, double acLon, const Config& cfg, RadarResult& res,
                              const std::vector<Station>* stations) {
    res.natural = true;
    const double R = std::max(30.0, cfg.radarRangeNm);
    const double cosRef = std::max(0.2, std::cos(std::round(acLat) * kPi / 180.0));   // fixed per whole degree, so the grid stays put
    res.cosRef = cosRef;
    const double root = 24.0;
    res.rootNm = root;
    const float minDbz = (float)cfg.radarMinDbz;
    auto toX = [&](double lon) { return lon * 60.0 * cosRef; };
    auto toY = [&](double lat) { return lat * 60.0; };
    auto toLat = [&](double Y) { return Y / 60.0; };
    auto toLon = [&](double X) { return X / (60.0 * cosRef); };
    // 1) raster, 1 nm, covering every root square that touches the radar range (plus one root of margin for the warp and the dry ring)
    const double g = 1.0;
    const double ax = toX(acLon), ay = toY(acLat);
    const long rx0 = (long)std::floor((ax - R) / root) - 1, rx1 = (long)std::floor((ax + R) / root) + 1;
    const long ry0 = (long)std::floor((ay - R) / root) - 1, ry1 = (long)std::floor((ay + R) / root) + 1;
    const double X0 = rx0 * root, Y0 = ry0 * root;
    const int W = (int)std::lround((rx1 - rx0 + 1) * root / g), H = (int)std::lround((ry1 - ry0 + 1) * root / g);
    std::vector<float> Z((size_t)W * H, 0.0f), T((size_t)W * H, 0.0f);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            const double X = X0 + (i + 0.5) * g, Y = Y0 + (j + 0.5) * g;
            const double lat = toLat(Y), lon = toLon(X);
            if (distNm(acLat, acLon, lat, lon) > R) continue;
            float best = 0, tb = 0;
            for (int q = 0; q < 4; ++q) {
                const double la = toLat(Y + ((q & 1) ? 0.25 : -0.25) * g), lo = toLon(X + ((q & 2) ? 0.25 : -0.25) * g);
                const float v = refl.at(la, lo);
                if (v >= minDbz - 10.0f && v < 90.0f) best = std::max(best, std::min(v, 70.0f));
                if (tops) { const float t = tops->at(la, lo); if (t > 0.5f && t < 30.0f) tb = std::max(tb, t); }
            }
            Z[(size_t)j * W + i] = best;
            T[(size_t)j * W + i] = tb;
        }
    // Gaussian smoothing (separable, sigma 1.5 nm). Tops: the largest top within 2 nm (a top is a maximum, smoothing would only lower it).
    // Cores keep their strength: the result is never more than 3 dBZ under the strongest raw value next to it (a 60 dBZ core of 2 nm
    // would otherwise come out at 50), while speckle and ragged pixel edges at the weak end are smoothed away.
    std::vector<float> Zraw = Z;
    {
        const int rad = 4;
        double k[2 * rad + 1], ks = 0;
        for (int d = -rad; d <= rad; ++d) { k[d + rad] = std::exp(-(d * d) / (2.0 * 1.5 * 1.5)); ks += k[d + rad]; }
        std::vector<float> tmp((size_t)W * H, 0.0f);
        for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) {
            double s = 0;
            for (int d = -rad; d <= rad; ++d) { const int ii = std::min(W - 1, std::max(0, i + d)); s += k[d + rad] * Z[(size_t)j * W + ii]; }
            tmp[(size_t)j * W + i] = (float)(s / ks);
        }
        for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) {
            double s = 0;
            for (int d = -rad; d <= rad; ++d) { const int jj = std::min(H - 1, std::max(0, j + d)); s += k[d + rad] * tmp[(size_t)jj * W + i]; }
            Z[(size_t)j * W + i] = (float)(s / ks);
        }
        for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) {
            float m = 0;
            for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
                const int ii = i + di, jj = j + dj;
                if (ii >= 0 && jj >= 0 && ii < W && jj < H) m = std::max(m, Zraw[(size_t)jj * W + ii]);
            }
            float& z = Z[(size_t)j * W + i];
            if (m - 2.0f >= 35.0f) z = std::max(z, m - 2.0f);
        }
        std::vector<float> t2((size_t)W * H, 0.0f);
        for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) {
            float m = 0;
            for (int dj = -2; dj <= 2; ++dj) for (int di = -2; di <= 2; ++di) {
                const int ii = i + di, jj = j + dj;
                if (ii >= 0 && jj >= 0 && ii < W && jj < H) m = std::max(m, T[(size_t)jj * W + ii]);
            }
            t2[(size_t)j * W + i] = m;
        }
        T.swap(t2);
    }
    // the picture for the map (the smoothed mosaic, not the records)
    res.viewX0 = X0; res.viewY0 = Y0; res.viewG = g; res.viewW = W; res.viewH = H;
    res.view.resize((size_t)W * H);
    for (size_t q = 0; q < res.view.size(); ++q) res.view[q] = (unsigned char)std::max(0.0f, std::min(255.0f, std::round(Z[q] * 3.0f)));
    auto bil = [&](const std::vector<float>& A, double X, double Y) {
        const double u = (X - X0) / g - 0.5, v = (Y - Y0) / g - 0.5;
        const int i = (int)std::floor(u), j = (int)std::floor(v);
        if (i < 0 || j < 0 || i + 1 >= W || j + 1 >= H) return 0.0;
        const double fu = u - i, fv = v - j;
        return (A[(size_t)j * W + i] * (1 - fu) + A[(size_t)j * W + i + 1] * fu) * (1 - fv) +
               (A[(size_t)(j + 1) * W + i] * (1 - fu) + A[(size_t)(j + 1) * W + i + 1] * fu) * fv;
    };
    // 2) the warped picture
    auto Zn = [&](double X, double Y) {
        const double wx = 2.5 * fbm(X, Y, 9.0, 1), wy = 2.5 * fbm(X, Y, 9.0, 2);
        const double z = bil(Z, X + wx, Y + wy);
        if (z < 5.0) return 0.0;
        return z + 2.0 * fbm(X, Y, 6.0, 3);
    };
    const double levels[4] = {minDbz, 30.0, 40.0, 50.0};
    auto cls = [&](double z) { int c = 0; for (double L : levels) if (z >= L) ++c; return c; };
    // which root squares are used: those with rain, and the ring around them (dry records there replace the gap-fill tiles)
    std::set<std::pair<long, long>> wet;
    for (long ry = ry0; ry <= ry1; ++ry)
        for (long rx = rx0; rx <= rx1; ++rx) {
            bool any = false;
            for (double y = 0.5; y < root && !any; y += 1.0)
                for (double x = 0.5; x < root && !any; x += 1.0)
                    if (Zn(rx * root + x, ry * root + y) >= minDbz) any = true;
            if (any) wet.insert({rx, ry});
        }
    std::set<std::pair<long, long>> used;
    for (const auto& w : wet)
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const long rx = w.first + dx, ry = w.second + dy;
            const double cx = (rx + 0.5) * root, cy = (ry + 0.5) * root;
            if (distNm(acLat, acLon, toLat(cy), toLon(cx)) > R + root) continue;
            used.insert({rx, ry});
        }
    {   // Widespread rain (log 2026-11-04, Ohio: 156 root squares for a budget of 150) left nothing to split, so the whole picture stayed in
        // 24 nm squares that averaged the cores away (40 dBZ where the mosaic had more) and showed as big squares. The nearest squares are
        // kept up to just over half the budget, so the rest of it refines the picture; the farthest ones are left out.
        const size_t keepN = (size_t)std::max(8, (int)(cfg.radarNaturalMax * 0.55));
        if (used.size() > keepN) {
            std::vector<std::pair<double, std::pair<long, long>>> byD;
            for (const auto& r : used) byD.push_back({distNm(acLat, acLon, toLat((r.second + 0.5) * root), toLon((r.first + 0.5) * root)), r});
            std::sort(byD.begin(), byD.end());
            char b[128];
            std::snprintf(b, sizeof b, "%zu far radar square(s) beyond %.0f nm left out to keep detail near the aircraft", byD.size() - keepN, byD[keepN].first);
            res.note += res.note.empty() ? b : std::string("; ") + b;
            used.clear();
            for (size_t k = 0; k < keepN; ++k) used.insert(byD[k].second);
        }
    }
    // 3) split the squares, the most useful split first: a square whose picture is mixed (more than one intensity class) is split into
    // four; priority is its size, weighted down with distance (an edge close to the aircraft matters most), until the budget is used.
    // An airport inside a square: one record can only carry one airport's cloud decks, and a 24 nm square of uniform rain reached over
    // several of them (2026-11-13 KAPC: the overcast at 1377 m read back at 605 m, the deck of another airport under the same square).
    // A square with an airport in it (or within 2 nm of its edge) is split down to 6 nm, so the record over each airport is its own.
    std::vector<std::pair<double, double>> stnXY;
    if (stations) for (const auto& st : *stations) stnXY.push_back({toX(st.lon), toY(st.lat)});
    auto hasStation = [&](double X, double Y, double s) {
        for (const auto& q : stnXY) if (q.first >= X - 2.0 && q.first <= X + s + 2.0 && q.second >= Y - 2.0 && q.second <= Y + s + 2.0) return true;
        return false;
    };
    const double kStationSquareNm = 6.0;
    struct Leaf { double X, Y, s; double z; int c; float frac; bool mixed; bool stn; };
    auto measure = [&](double X, double Y, double s) {
        const double cx = X + s / 2, cy = Y + s / 2;
        const int n = std::max(4, std::min(16, (int)std::lround(s / 1.0)));
        const double st = s / n;
        int c0 = -1; bool mixed = false; double sum = 0, mx = 0; int wetN = 0, cnt = 0;
        int cc[5] = {0, 0, 0, 0, 0}; double cs[5] = {0, 0, 0, 0, 0};
        for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
            const double z = Zn(X + (i + 0.5) * st, Y + (j + 0.5) * st);
            const int c = cls(z);
            if (c0 < 0) c0 = c; else if (c != c0) mixed = true;
            sum += z; mx = std::max(mx, z); ++cnt; if (c > 0) ++wetN;
            ++cc[c]; cs[c] += z;
        }
        // a uniform square lies between its mean and its strongest spot (it stays in its class). A mixed one takes the value at its
        // centre, unless its strongest part fills a third of it: then that part's mean, so a small core is not lost in a bigger square
        double zc = 0.5 * (sum / cnt + mx);
        if (mixed) {
            zc = Zn(cx, cy);
            for (int c = 4; c > 0; --c) if (cc[c] > 0) { if (cc[c] * 3 >= cnt) zc = std::max(zc, cs[c] / cc[c]); break; }
        }
        return Leaf{cx, cy, s, zc, cls(zc), (float)wetN / (float)cnt, mixed, hasStation(X, Y, s)};
    };
    const double fine = std::max(1.5, cfg.radarNaturalFineNm);
    auto prio = [&](const Leaf& L) {
        const bool stnSplit = L.stn && L.s / 2 >= kStationSquareNm - 1e-6;
        if ((!L.mixed || L.s / 2 < fine - 1e-6) && !stnSplit) return -1.0;
        const double d = distNm(acLat, acLon, toLat(L.Y), toLon(L.X));
        return L.s / (1.0 + std::pow(d / 40.0, 1.5));   // (40 nm: the rim further out is split finer too, so it shows no big squares)
    };
    std::vector<Leaf> leaves;
    for (const auto& r : used) leaves.push_back(measure(r.first * root, r.second * root, root));
    using QE = std::pair<double, size_t>;
    std::priority_queue<QE> pq;
    for (size_t k = 0; k < leaves.size(); ++k) { const double p = prio(leaves[k]); if (p > 0) pq.push({p, k}); }
    std::vector<unsigned char> gone(leaves.size(), 0);
    int count = (int)leaves.size();
    while (!pq.empty() && count + 3 <= cfg.radarNaturalMax) {
        const size_t k = pq.top().second; pq.pop();
        const Leaf L = leaves[k];
        gone[k] = 1;
        const double h = L.s / 2, X = L.X - h, Y = L.Y - h;
        for (int q = 0; q < 4; ++q) {
            leaves.push_back(measure(X + (q & 1) * h, Y + (q >> 1) * h, h));
            gone.push_back(0);
            const double p = prio(leaves.back());
            if (p > 0) pq.push({p, leaves.size() - 1});
        }
        count += 3;
    }
    { std::vector<Leaf> keep; for (size_t k = 0; k < leaves.size(); ++k) if (!gone[k]) keep.push_back(leaves[k]); leaves.swap(keep); }
    if ((int)leaves.size() > cfg.radarNaturalMax) res.note += res.note.empty() ? "natural picture over budget" : "; natural picture over budget";
    // 4) records
    const float step = (float)std::max(50.0, cfg.stormTopStepM);
    for (const auto& L : leaves) {
        RadarCell c;
        c.natural = true;
        c.sizeNm = (float)L.s;
        {   // a fixed small offset per record (up to 18% of its size), so the splits between records do not run along straight rows
            const unsigned hh = hashStr(std::to_string((long)std::floor(L.X / L.s)) + "," + std::to_string((long)std::floor(L.Y / L.s)) + "s" + std::to_string((int)std::lround(L.s * 4)), 29);
            (void)hh;   // no offset after all: irregular record spacing made ribbons (as the staggered fill grid did, 2026-11-03)
            c.lat = toLat(L.Y); c.lon = toLon(L.X);
        }
        c.distNm = distNm(acLat, acLon, c.lat, c.lon);
        char key[64];
        // one key per square, rain or dry: when a square dries up (or starts raining) between updates its record is changed in place, not
        // erased and sent again at the same spot (two operations on one location in one update)
        std::snprintf(key, sizeof key, "N%ld:%ld:%d", (long)std::floor(L.X / L.s), (long)std::floor(L.Y / L.s), (int)std::lround(L.s * 4));
        c.key = key;
        if (L.c == 0) { res.dryCells.push_back(c); continue; }
        c.dbzFine = (float)std::round(std::max((double)minDbz, std::min(70.0, L.z)));
        c.dbz = std::max(5.0f, std::round(c.dbzFine / 5.0f) * 5.0f);
        c.rateMmH = rateFromDbz(c.dbzFine);
        c.frac = std::max(0.25f, std::round(clampf(L.frac, 0.0f, 1.0f) * 4.0f) / 4.0f);
        // top: the highest measured top in the square, else the estimate for the reflectivity (varied a little per record)
        float t = 0;
        for (double dy = -0.4; dy <= 0.41; dy += 0.2) for (double dx = -0.4; dx <= 0.41; dx += 0.2) t = std::max(t, (float)bil(T, L.X + dx * L.s, L.Y + dy * L.s));
        c.topKnown = t > 0.5f;
        if (c.topKnown) c.topM = std::round(t * 1000.0f / step) * step;
        else {
            const double v = unit(hashStr(c.key, 13), 1) * 2.0 - 1.0;
            c.topM = std::max(3000.0f, std::round((estimateTopM(c.dbz, c.lat) + (float)(v * cfg.stormTopVariationM)) / step) * step);
        }
        // rim: how much dry ground there is around it (at a square's width, at least 3 nm)
        int dry = 0;
        const double rr = std::max(3.0, L.s);
        for (int k = 0; k < 8; ++k) {
            const double a = k * kPi / 4.0;
            if (cls(Zn(L.X + rr * std::cos(a), L.Y + rr * std::sin(a))) == 0) ++dry;
        }
        c.edge = clampf(dry / 5.0f, 0.0f, 1.0f);
        // no square outlines (2026-11-12, "storm fronts too square"): each record stands for a square, and squares of the same intensity
        // side by side drew walls and flat plateaus along the record grid. Along the rim the coverage now breaks up with smooth noise
        // fixed to the ground (the outer squares thin out unevenly, into separate towers, instead of ending in a straight wall), and
        // every square's top varies a little with the same kind of noise (the slope limit below keeps neighbours together)
        if (c.edge > 0.01f) {
            const double nz = fbm(L.X, L.Y, 9.0, 41);
            c.coverScale = clampf((float)(1.0 - c.edge * (0.22 + 0.18 * nz)), 0.45f, 1.0f);
        }
        c.topM = std::max(3000.0f, c.topM + (float)(std::round(fbm(L.X, L.Y, 7.0, 43) * 450.0 / step) * step));
        res.cells.push_back(c);
    }
    res.roots = used;
    if (cfg.stormSmoothTops) limitStormTopSlope(res.cells, cfg.radarCellNm, cfg.stormTopMaxDropM, step);
}

bool radarCovers(const RadarResult& r, double lat, double lon) {
    if (!r.natural || r.roots.empty()) return false;
    const long rx = (long)std::floor(lon * 60.0 * r.cosRef / r.rootNm), ry = (long)std::floor(lat * 60.0 / r.rootNm);
    return r.roots.count({rx, ry}) > 0;
}

const RadarCell* naturalCellAt(const RadarResult& r, double lat, double lon) {
    if (!r.natural) return nullptr;
    const double X = lon * 60.0 * r.cosRef, Y = lat * 60.0;
    for (const auto& c : r.cells) {
        if (!c.natural) continue;
        const double cx = c.lon * 60.0 * r.cosRef, cy = c.lat * 60.0, h = c.sizeNm * 0.5;
        if (std::fabs(X - cx) <= h && std::fabs(Y - cy) <= h) return &c;
    }
    return nullptr;
}

// Convection seen at an airport: thunder, showers, hail or squalls in the weather, or CB / TCU / lightning anywhere in the report
// (body or remarks: "BKN030CB", "CB DSNT N", "LTG DSNT"). The global model's points are forecasts and never count.
bool convectionReported(const Station& st) {
    if (st.icao.compare(0, 3, "GFS") == 0) return false;
    const Report& r = st.report;
    if (r.thunder) return true;
    for (const char* k : {"TS", "SH", "GR", "GS", "SQ"}) if (r.wxText.find(k) != std::string::npos) return true;
    std::string tok;
    std::istringstream in(st.rawMetar);
    int n = 0;
    while (in >> tok) {
        ++n;
        if (tok == "METAR" || tok == "SPECI" || (n <= 2 && tok.size() == 4)) continue;   // the header and the station's own identifier
        if (tok == "TSNO") continue;                                                      // "thunderstorm sensor not operating"
        auto ends = [&](const char* e) { const size_t k = std::strlen(e); return tok.size() >= k && tok.compare(tok.size() - k, k, e) == 0; };
        if (tok == "CB" || tok == "TCU" || ends("CB") || ends("TCU") || tok.compare(0, 3, "LTG") == 0) return true;
        const size_t ts = tok.find("TS");   // TSRA, +TSRA, VCTS, TSB05, TSE10 ...
        if (ts != std::string::npos && (ts == 0 || tok[ts - 1] == '+' || tok[ts - 1] == '-' || (ts >= 2 && tok.compare(ts - 2, 2, "VC") == 0))) return true;
    }
    return false;
}

// Steady rain or snow under a melting layer reads 40-45 dBZ in the composite (the bright band) while 15-30 dBZ reach the ground
// (2026-11-10, eastern Washington: 10 "cumulonimbus" cells, no airport reporting TS, CB or more than 3 mm of rain an hour). MRMS
// PrecipFlag classes every pixel; only convection (6), hail (7) and tropical convection (96) keep their reflectivity and can become
// cumulonimbus. Everything else is capped at radar_stratiform_max_dbz and marked stratiform. Without PrecipFlag, an echo counts as
// convective only within 60 nm of an airport reporting convection. Returns the mask (1 = convective) on refl's grid.
GribField convectiveMask(GribField& refl, const GribField* flag, const std::vector<Station>* stations, float capDbz, int& capped) {
    GribField m = refl;
    capped = 0;
    std::vector<const Station*> ev;
    if (!flag && stations) for (const auto& st : *stations) if (convectionReported(st)) ev.push_back(&st);
    const bool sameGrid = flag && flag->ni == refl.ni && flag->nj == refl.nj && std::fabs(flag->lat0 - refl.lat0) < 1e-6 && std::fabs(flag->lon0 - refl.lon0) < 1e-6;
    for (int j = 0; j < refl.nj; ++j) {
        const double la = refl.lat0 - j * refl.dLat;
        for (int i = 0; i < refl.ni; ++i) {
            const size_t k = (size_t)j * (size_t)refl.ni + (size_t)i;
            const double lo = refl.lon0 + i * refl.dLon;
            bool conv;
            if (flag) {
                const float f = sameGrid ? flag->v[k] : flag->at(la, lo);
                conv = (f > 5.5f && f < 7.5f) || (f > 95.5f && f < 96.5f);
            } else {
                conv = false;
                if (refl.v[k] > capDbz)
                    for (const Station* s : ev) if (std::fabs(s->lat - la) < 1.0 && distNm(la, lo, s->lat, s->lon) <= 60.0) { conv = true; break; }
            }
            m.v[k] = conv ? 1.0f : 0.0f;
            if (!conv && refl.v[k] > capDbz) { refl.v[k] = capDbz; ++capped; }
        }
    }
    return m;
}

void markStratiform(std::vector<RadarCell>& cells, const GribField& conv, double cellNm) {
    if (!conv.valid()) return;
    for (auto& c : cells) {
        const double halfNm = std::max((double)c.sizeNm, cellNm) * 0.5;
        const double dLat = halfNm / 60.0, dLon = halfNm / (60.0 * std::max(0.2, std::cos(c.lat * kPi / 180.0)));
        const int j0 = std::max(0, (int)std::floor((conv.lat0 - (c.lat + dLat)) / conv.dLat)), j1 = std::min(conv.nj - 1, (int)std::ceil((conv.lat0 - (c.lat - dLat)) / conv.dLat));
        const int i0 = std::max(0, (int)std::floor((c.lon - dLon - conv.lon0) / conv.dLon)), i1 = std::min(conv.ni - 1, (int)std::ceil((c.lon + dLon - conv.lon0) / conv.dLon));
        bool any = false;
        for (int j = j0; j <= j1 && !any; ++j)
            for (int i = i0; i <= i1; ++i)
                if (conv.v[(size_t)j * (size_t)conv.ni + (size_t)i] > 0.5f) { any = true; break; }
        c.stratiform = !any;
    }
}

RadarResult buildRadarCells(const GribField& refl, const GribField* tops, double lat, double lon, const Config& cfg,
                            const std::set<std::string>* prev, const GribField* conv, const std::vector<Station>* stations) {
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
    if (cfg.radarNatural) {
        buildNaturalCells(refl, tops, lat, lon, cfg, res, stations);
        if (conv) markStratiform(res.cells, *conv, cfg.radarCellNm);
        res.ok = true;
        res.topsFromRadar = tops != nullptr;
        return res;
    }
    // Keep the most relevant: close and intense first.
    std::set<std::string> prevKeys = prev ? *prev : std::set<std::string>();
    auto score = [&](const RadarCell& c) { return c.distNm - 2.0 * (c.dbz - 20.0) - (prevKeys.count(c.key) ? 25.0 : 0.0); };
    std::sort(all.begin(), all.end(), [&](const RadarCell& a, const RadarCell& b) { return score(a) < score(b); });
    std::vector<RadarCell> droppedCells;
    if ((int)all.size() > cfg.radarMaxCells) { droppedCells.assign(all.begin() + cfg.radarMaxCells, all.end()); all.resize((size_t)cfg.radarMaxCells); }
    if (cfg.stormFillHoles) res.holesFilled = fillStormHoles(all, droppedCells, cfg.radarCellNm, lat, lon, std::max(50.0, cfg.stormTopStepM), cfg.radarMinDbz, 5, std::max(4, cfg.radarMaxCells / 4));
    if (cfg.stormContour) res.rimCells = addStormContour(all, cfg.radarCellNm, std::max(6.0, cfg.radarCellNm * cfg.stormRadiusFactor), lat, lon, std::max(50.0, cfg.stormTopStepM), cfg.radarMinDbz, std::max(4, cfg.radarMaxCells));
    else if (cfg.stormRim) res.rimCells = addStormRim(all, cfg.radarCellNm, lat, lon, std::max(50.0, cfg.stormTopStepM), cfg.radarMinDbz, std::max(4, cfg.radarMaxCells), cfg.stormRimRings);
    if (cfg.stormSmoothTops) smoothStormTops(all, cfg.radarCellNm, cfg.stormTopBlend, cfg.stormTopStepM, cfg.stormFillPits);
    if (cfg.stormSmoothTops) limitStormTopSlope(all, cfg.radarCellNm, cfg.stormTopMaxDropM, std::max(50.0, cfg.stormTopStepM));
    smoothStormRain(all, cfg.radarCellNm, cfg.stormRainBlend, cfg.stormFillPits);
    // after the rain smoothing, so the outlines follow the reflectivity the records will actually carry
    if (cfg.stormContour && cfg.stormLevelContours)
        res.levelCells = addStormLevelContours(all, cfg.radarCellNm, std::max(6.0, cfg.radarCellNm * cfg.stormRadiusFactor), lat, lon,
                                               std::max(50.0, cfg.stormTopStepM), {30.0, 40.0, 50.0}, std::max(4, cfg.radarMaxCells));
    markStormEdges(all, cfg.radarCellNm);
    if (cfg.stormContour) for (auto& c : all) if (!c.synthetic) c.radiusScale = std::min(1.0f, c.contourScale);   // the outline decides how far a measured block reaches
    if (conv) markStratiform(all, *conv, cfg.radarCellNm);
    res.cells = std::move(all);
    res.ok = true;
    res.topsFromRadar = tops != nullptr;
    return res;
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

static int stormSlot(const XpWx& x) {
    int slot = 2;
    if (x.clouds[2].coverage > 0 && x.clouds[1].coverage <= 0) slot = 1;   // else every slot is taken: slot 2 (the highest, layers are packed bottom-up) is replaced
    return slot;
}

float stormBaseFor(const XpWx& x, const RadarCell& c, double fieldElevM) {
    const int slot = stormSlot(x);
    float baseM = (float)fieldElevM + (c.dbz >= 40 ? 900.0f : 600.0f);
    if (x.clouds[slot].coverage > 0 && x.clouds[slot].baseM <= fieldElevM + 2500.0f) {
        // X-Plane blends records slot by slot, bases and tops included. A storm base 700 m below the deck it replaces turned the
        // blend at the station into a thin 560-6000 m column (seen in a log at KFYV), so the storm grows from the replaced deck's base.
        baseM = x.clouds[slot].baseM;
    } else {
        for (int i = 0; i < 3; ++i)    // otherwise do not float above the station's own lowest layer
            if (x.clouds[i].coverage > 0 && x.clouds[i].baseM > fieldElevM + 150 && !(x.fogLayerApplied && i == 0)) baseM = std::min(baseM, x.clouds[i].baseM);
    }
    return baseM;
}

void applyStorm(XpWx& x, const RadarCell& c, const Config& cfg, double fieldElevM, double /*lat*/, float baseOverrideM) {
    const float dbz = c.dbz;
    // Precipitation: X-Plane takes 0..1.
    float p = clampf(c.rateMmH / (float)cfg.radarRainFullMmH, 0.05f, 1.0f);
    x.precip = std::max(x.precip, p);

    // Heavy rain cuts visibility.
    float cap = 1e9f;
    if (dbz >= 50) cap = 800; else if (dbz >= 45) cap = 1500; else if (dbz >= 40) cap = 2500;
    else if (dbz >= 35) cap = 4000; else if (dbz >= 30) cap = 6000; else if (dbz >= 25) cap = 8000;
    x.visM = std::min(x.visM, cap);

    // Steady rain (MRMS PrecipFlag, under 40 dBZ) falls out of the station's broken or overcast deck (nimbostratus); it is not a cloud of
    // its own (2026-11-13 near KCDW: a rain layer from the lowest base, 652 m, took slot 0 in the rain records and pushed the station's
    // decks a slot up; X-Plane blends slot by slot, so the overcast vanished there and the cloud top dropped from 12,300 to 6,000 ft).
    // The rain falls from that deck, which is left as the station reports it (see below). Without such a deck the rain still builds its
    // own cloud, below.
    // Showers under 40 dBZ too (2026-11-13 KAUN: a showery cell built its own layer from the lowest base, 605 m, on top of the broken
    // deck there and pushed out the 46 % layer at 5,186 m); only thunderstorm cores (40 dBZ and up) grow a cloud of their own.
    bool hosted = false;
    float topM = 0.0f;
    if (dbz < 40.0f) {
        int host = -1;
        for (int i = 0; i < 3; ++i) {
            const XpCloud& k = x.clouds[i];
            if (k.coverage < 0.6f || k.type < 0.5f || k.type >= 2.25f || (i == 0 && x.fogLayerApplied)) continue;
            if (host < 0 || k.coverage > x.clouds[host].coverage + 0.05f ||
                (k.coverage > x.clouds[host].coverage - 0.05f && k.topM - k.baseM > x.clouds[host].topM - x.clouds[host].baseM)) host = i;
        }
        if (host >= 0) {
            // The station's decks stay exactly as reported (2026-11-13 KSEA/KSMF): records whose deck the rain had made thicker and
            // taller lost their overcast in X-Plane, while the same decks in records with rain from the report alone, or no rain, showed.
            // X-Plane makes its own rain clouds from the precipitation; the radar here only sets the rain, the visibility and a shower's
            // turbulence (up to the deck's top).
            topM = x.clouds[host].topM;
            hosted = true;
        }
    }
    if (!hosted) {
    // Slot choice: the station's own layers must survive. Use the top slot if it is free, else the middle one; only when all
    // are taken does the storm replace the highest of them (slot 0 may hold ground fog and is never used).
    const int slot = stormSlot(x);
    float baseM = baseOverrideM > 0 ? baseOverrideM : stormBaseFor(x, c, fieldElevM);
    topM = stormTopM(c, cfg, baseM);
    // Rim of a cluster: up to 45% less height above the base (never below the 1500 m minimum) so the system slopes off instead of ending in a wall.
    if (c.edge > 0) topM = std::max(baseM + 1500.0f, baseM + (topM - baseM) * (1.0f - 0.45f * c.edge));
    // Steady rain (MRMS PrecipFlag): the radar sees the rain, not the small ice above it, so its echo top reads low for the cloud. Where
    // the global model has the cloud reaching higher from this base, the top goes up to the model's (within the depth X-Plane allows
    // for cumulus). It only ever raises a top: storms (40 dBZ and up, or convective) keep the radar's.
    if (c.stratiform && dbz < 40.0f && !x.modelProfM.empty()) {
        const float mt = modelCloudTopM(x.modelProfM, baseM);
        if (mt > topM) topM = std::min({mt, baseM + xpMaxThicknessM(2.0f), kXpTopM});
    }
    XpCloud& cl = x.clouds[slot];
    const float replacedCov = cl.coverage;   // the deck this storm grows out of (0 if the slot was free)
    // Cumulonimbus from 30 dBZ up (and for any measured echo top above 6 km); a weak echo is cumulus (2.0), not a deep anvil. Ramping
    // further down to 1.0 made smooth stratus sheets with curtain edges, so the floor stays at cumulus.
    // 3 (cumulonimbus) only for real thunderstorm cores: X-Plane gives that type strong up- and downdrafts. Ordinary rain clouds are
    // cumulus (2), even when they are tall (before: 30 dBZ or a top over 6 km already made a cumulonimbus, 2026-11-04 altitude-loss log).
    cl.type = dbz >= 40.0f && !c.stratiform ? 3.0f : 2.0f;   // steady rain (MRMS PrecipFlag) is never a cumulonimbus
    float t = clampf((dbz - 20.0f) / 25.0f, 0.0f, 1.0f);
    cl.coverage = (0.5f + ((float)cfg.stormCoverageMax - 0.5f) * t) * (0.8f + 0.2f * c.frac);
    if (!c.synthetic && c.edge <= 0.01f && dbz >= 35.0f)   // deep inside a storm: solid, so X-Plane draws no clear-air pockets (and no rain-free spots on the map) there
        cl.coverage = std::max(cl.coverage, (float)cfg.stormCoreCoverage);
    cl.coverage *= c.coverScale;            // rim rings: thinner and thinner toward the outside
    cl.coverage *= 1.0f - 0.25f * c.edge;   // and up to 25% less coverage on the rim (the station deck it replaces still sets the floor below)
    // A storm inside a solid deck must not thin it: X-Plane blends coverage slot by slot, so a 58% storm over a 94% overcast punched a
    // sinuous hole-edged contour through the sheet (KFYV log). The station's own coverage is the floor.
    cl.coverage = std::max(cl.coverage, replacedCov);
    cl.baseM = baseM;
    cl.topM = topM;
    x.cloudCount = 0;
    for (const auto& k : x.clouds) if (k.coverage > 0) ++x.cloudCount;
    }

    // Turbulence and gusts below the storm top.
    float turb = c.stratiform ? 0.0f : clampf((dbz - 30.0f) / 30.0f, 0.0f, 1.0f) * (float)cfg.maxTurbulence;   // steady rain: no storm turbulence
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
