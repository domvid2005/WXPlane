#include "windshear.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "geo.h"

namespace wx {
namespace shear {

namespace {
const double kPi = 3.14159265358979323846;
double rad(double d) { return d * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }
double norm360(double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360.0 : d; }

void dest(double lat, double lon, double brgDeg, double nm, double& oLat, double& oLon) {
    const double d = nm / 3440.065, b = rad(brgDeg), la = rad(lat), lo = rad(lon);
    const double la2 = std::asin(std::sin(la) * std::cos(d) + std::cos(la) * std::sin(d) * std::cos(b));
    const double lo2 = lo + std::atan2(std::sin(b) * std::sin(d) * std::cos(la), std::cos(d) - std::sin(la) * std::sin(la2));
    oLat = deg(la2); oLon = deg(lo2);
}
double bearing(double lat1, double lon1, double lat2, double lon2) {
    const double y = std::sin(rad(lon2 - lon1)) * std::cos(rad(lat2));
    const double x = std::cos(rad(lat1)) * std::sin(rad(lat2)) - std::sin(rad(lat1)) * std::cos(rad(lat2)) * std::cos(rad(lon2 - lon1));
    return norm360(deg(std::atan2(y, x)));
}
double smooth01(double x) { x = std::max(0.0, std::min(1.0, x)); return x * x * (3 - 2 * x); }
bool hasTs(const Report& r) {
    if (r.thunder) return true;
    return r.wxText.find("TS") != std::string::npos || r.wxText.find("SQ") != std::string::npos;
}
// vertical profile of the horizontal outflow: full strength up to ~1,500 ft above the ground, gone by `depth`
double outflowZ(double aglFt, double depthFt) {
    if (aglFt < 0) return 0;
    const double full = depthFt * 0.5;
    if (aglFt <= full) return 1.0;
    return std::max(0.0, 1.0 - (aglFt - full) / (depthFt - full));
}
}  // namespace

double hash01(uint32_t seed, const std::string& s, long long a, long long b) {
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)seed * 0x9E3779B97F4A7C15ULL;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; h ^= h >> 29; };
    for (unsigned char c : s) mix(c);
    mix((uint64_t)a); mix((uint64_t)b);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (double)(h >> 11) / 9007199254740992.0;
}

double intervalProbability(double pHour, double dtS) {
    if (!(pHour > 0)) return 0;
    if (pHour >= 1) return 1;
    const double lambda = -std::log(1.0 - pHour) / 3600.0;
    return 1.0 - std::exp(-lambda * dtS);
}

std::string fieldName(int type) { return type == kMicroburst ? "microburst" : "gust front"; }

std::vector<Source> assess(const Env& env) {
    std::vector<Source> out;
    if (!env.stations) return out;
    // 10 nm squares (key) with their strongest measured echo
    struct Sq { double lat = 0, lon = 0; float dbz = 0; };
    std::map<std::string, Sq> sq;
    auto keyOf = [](double lat, double lon) {
        char b[48];
        std::snprintf(b, sizeof b, "%ld:%ld", (long)std::floor(lat * 6.0), (long)std::floor(lon * 6.0 * std::max(0.2, std::cos(lat * kPi / 180.0))));
        return std::string(b);
    };
    if (env.cells)
        for (const auto& c : *env.cells) {
            if (c.synthetic || !(c.dbz >= 30.0f) || !std::isfinite(c.lat) || !std::isfinite(c.lon)) continue;
            Sq& s = sq[keyOf(c.lat, c.lon)];
            if (c.dbz > s.dbz) { s.dbz = c.dbz; s.lat = c.lat; s.lon = c.lon; }
        }
    // thunder reported at a station with no radar echo of its own (radar off or outside coverage): a source of its own
    // (the global model's open-water points are forecasts, not observations: they never count as evidence)
    auto observed = [](const Station& st) { return st.icao.compare(0, 3, "GFS") != 0; };
    for (const auto& st : *env.stations)
        if (observed(st) && hasTs(st.report)) {
            Sq& s = sq[keyOf(st.lat, st.lon)];
            if (s.dbz <= 0) { s.lat = st.lat; s.lon = st.lon; }
        }
    for (const auto& kv : sq) {
        Source s;
        s.key = kv.first; s.lat = kv.second.lat; s.lon = kv.second.lon; s.dbz = kv.second.dbz;
        const Station* nearest = nullptr; double nd = 1e9;
        for (const auto& st : *env.stations) {
            const double d = distNm(s.lat, s.lon, st.lat, st.lon);
            if (d < 15.0 && observed(st)) { if (hasTs(st.report)) s.tsObserved = true; s.gustKt = std::max(s.gustKt, (float)st.report.gustKt); }
            if (d < nd) { nd = d; nearest = &st; }
        }
        // dry microburst evidence: a convective echo that is not heavy, a deep dry sub-cloud layer (spread >= 15 C) and a high cloud base
        if (nearest && nd < 25.0 && s.dbz >= 30.0f && s.dbz < 45.0f) {
            const Report& r = nearest->report;
            int lowBase = 1 << 30;
            for (const auto& c : r.clouds) lowBase = std::min(lowBase, c.baseFtAgl);
            const bool spread = r.hasTemp && r.hasDewp && r.tempC - r.dewpC >= 15.0;
            const bool highBase = !r.clouds.empty() && lowBase >= 8000;
            s.dry = spread && highBase;
        }
        const bool strong = s.dbz >= 50.0f || (s.tsObserved && s.gustKt >= 30.0f);
        const bool conv = s.dbz >= 40.0f || s.tsObserved;
        if (strong) { s.pHourGustFront = 0.15; s.pHourMicroburst = 0.02; }
        else if (conv) { s.pHourGustFront = 0.10; s.pHourMicroburst = 0.007; }
        else if (s.dry) { s.pHourGustFront = 0.02; s.pHourMicroburst = 0.005; }
        else continue;   // nothing that supports windshear: no source (missing data lowers confidence, it never invents a field)
        if ((s.dbz >= 45.0f && s.tsObserved) || (s.dbz >= 40.0f && s.tsObserved && s.gustKt >= 25.0f)) s.confidence = kHigh;
        else if (conv) s.confidence = kMedium;
        else s.confidence = kLow;
        if (s.confidence == kLow) { s.pHourGustFront *= 0.5; s.pHourMicroburst *= 0.5; }
        char b[160];
        std::snprintf(b, sizeof b, "%s%.0f dBZ%s%s%s", strong ? "strong convection, " : (conv ? "convection, " : "dry-microburst environment, "), s.dbz,
                      s.tsObserved ? ", TS reported" : "", s.gustKt > 0 ? (", gusts " + std::to_string((int)s.gustKt) + " kt").c_str() : "", s.dry ? ", dry sub-cloud layer" : "");
        s.why = b;
        out.push_back(s);
    }
    return out;
}

double Engine::intensity(const Field& f, double t) {
    const double a = t - f.t0;
    if (a < 0) return 0;
    if (f.type == kMicroburst) {   // AIM 7-1-24: intensifies ~5 min after reaching the ground, 2-4 min at peak, gone within ~15 min
        if (a < 300) return smooth01(a / 300.0);
        if (a < 480) return 1.0;
        if (a < 900) return 0.5 * (1.0 + std::cos(kPi * (a - 480.0) / 420.0));
        return 0;
    }
    const double rise = 600, fall = 900;   // gust front: builds over 10 min, weakens over the last 15
    if (a < rise) return smooth01(a / rise);
    if (a < f.lifeS - fall) return 1.0;
    if (a < f.lifeS) return 0.5 * (1.0 + std::cos(kPi * (a - (f.lifeS - fall)) / fall));
    return 0;
}

void Engine::position(const Field& f, double t, double& lat, double& lon) {
    const double nm = f.moveKt * std::max(0.0, t - f.t0) / 3600.0;
    dest(f.lat0, f.lon0, f.moveToDeg, nm, lat, lon);
}

double Engine::outflowRadiusNm(const Field& f, double t) {
    const double a = std::max(0.0, t - f.t0);
    // microburst: outflow ~2.5 statute miles across (AIM) = ~1.1 nm radius, spreading from 0.7 to 1.3 nm
    if (f.type == kMicroburst) return 0.7 + 0.6 * std::min(1.0, a / 420.0);
    return 2.0 + 12.0 * a / 3600.0;   // gust front: 2 nm out at formation, spreading ~12 kt from its parent
}

std::vector<std::string> Engine::step(double simT, const std::vector<Source>& sources, const Env& env) {
    std::vector<std::string> log;
    if (!std::isfinite(simT)) { log.push_back("rejected: simulation time not a number"); return log; }
    // retire
    for (size_t i = 0; i < fields_.size();) {
        if (simT > fields_[i].t0 + fields_[i].lifeS) {
            char b[160];
            std::snprintf(b, sizeof b, "field #%u (%s) dissipated after %.0f min", fields_[i].id, fieldName(fields_[i].type).c_str(), (simT - fields_[i].t0) / 60.0);
            log.push_back(b);
            fields_.erase(fields_.begin() + (long)i);
        } else ++i;
    }
    const long long bucket = (long long)std::floor(simT / 30.0);
    long long first = lastT_ < 0 ? bucket : (long long)std::floor(lastT_ / 30.0) + 1;
    if (bucket - first > 20) first = bucket;   // a jump (replay, teleport, long pause with acceleration): no burst of back-dated fields
    lastT_ = simT;
    for (long long bk = first; bk <= bucket; ++bk) {
        for (const auto& s : sources) {
            for (int type : {kGustFront, kMicroburst}) {
                const double pH = type == kGustFront ? s.pHourGustFront : s.pHourMicroburst;
                if (!(pH > 0)) continue;
                bool active = false;
                for (const auto& f : fields_) if (f.source == s.key && f.type == type) { active = true; break; }
                if (active) continue;   // one field of each kind per source at a time; the next can form after it dies
                if (hash01(seed_, s.key + (type == kGustFront ? "g" : "m"), bk, 0) >= intervalProbability(pH, 30.0)) continue;
                if (fields_.size() >= 12) { log.push_back("field budget (12) full: formation skipped"); continue; }
                Field f;
                f.id = nextId_++;
                f.type = type;
                f.source = s.key;
                f.cause = s.why;
                f.confidence = s.confidence;
                f.t0 = (double)bk * 30.0;
                // storms drift with ~70 % of the environmental wind (an assumption: one radar picture gives no measured storm motion)
                f.moveToDeg = norm360(env.steerDirDeg + 180.0);
                f.moveKt = std::max(0.0, std::min(60.0, env.steerKt * 0.7));
                const double h1 = hash01(seed_, s.key, bk, 1), h2 = hash01(seed_, s.key, bk, 2), h3 = hash01(seed_, s.key, bk, 3);
                dest(s.lat, s.lon, h1 * 360.0, (type == kMicroburst ? 2.5 : 1.0) * h2, f.lat0, f.lon0);
                if (type == kMicroburst) {
                    double v = s.dry ? 12.0 + 12.0 * h3 : std::max(8.0, std::min(30.0, 10.0 + (s.dbz - 35.0) * 0.6)) * (0.7 + 0.6 * h3);
                    if (hash01(seed_, s.key, bk, 4) < 0.05 && s.dbz >= 50.0f) v *= 1.5;   // the rare intense one, only in strong storms
                    f.peakKt = (float)std::min(45.0, v);
                    f.downFpm = (float)std::min(6000.0, 600.0 + f.peakKt * 90.0);
                    f.lifeS = 900;
                } else {
                    const double v = std::max(10.0, std::min(35.0, 10.0 + (s.dbz - 40.0) * 0.8 + s.gustKt * 0.3)) * (0.8 + 0.4 * h3);
                    f.peakKt = (float)std::min(40.0, v);
                    f.lifeS = 2700 + 1800 * hash01(seed_, s.key, bk, 5);
                    f.outDirDeg = norm360(f.moveToDeg + (h1 - 0.5) * 80.0);   // the outflow leads the storm, give or take 40 deg
                }
                char b[300];
                std::snprintf(b, sizeof b, "field #%u formed: %s at %.4f,%.4f, peak %.0f kt%s, life %.0f min, moving %03.0f/%.0f kt, confidence %s, cause: %s",
                              f.id, fieldName(type).c_str(), f.lat0, f.lon0, f.peakKt,
                              type == kMicroburst ? (", downdraft " + std::to_string((int)f.downFpm) + " fpm (modelled, not applied)").c_str() : "",
                              f.lifeS / 60.0, f.moveToDeg, f.moveKt, f.confidence == kHigh ? "high" : (f.confidence == kMedium ? "medium" : "low"), s.why.c_str());
                log.push_back(b);
                fields_.push_back(f);
            }
        }
    }
    return log;
}

const Field& Engine::force(int type, double lat, double lon, double simT, double trackDeg, const Env& env) {
    Field f;
    f.id = nextId_++;
    f.type = type;
    f.source = "test" + std::to_string(f.id);
    f.cause = "test event (forced from the menu)";
    f.confidence = kHigh;
    f.forced = true;
    f.moveToDeg = norm360(env.steerDirDeg + 180.0);
    f.moveKt = 0;   // a test event stays where it was put
    if (type == kMicroburst) {
        f.peakKt = 30; f.downFpm = 3300; f.lifeS = 900;
        f.t0 = simT - 300.0;   // already at peak: 3 min at full strength, then the normal decay
        f.lat0 = lat; f.lon0 = lon;
    } else {
        f.peakKt = 25; f.lifeS = 1800;
        f.t0 = simT - 600.0;   // already built
        f.outDirDeg = norm360(trackDeg + 180.0);   // blowing against the aircraft: a headwind gust front
        // the front line is at `lat,lon`; its parent lies behind it, on the far side
        const double d = outflowRadiusNm(f, simT);
        dest(lat, lon, trackDeg, d, f.lat0, f.lon0);
    }
    fields_.push_back(f);
    return fields_.back();
}

int Engine::dropFar(double lat, double lon, double maxNm, double simT) {
    int n = 0;
    for (size_t i = 0; i < fields_.size();) {
        double la, lo;
        position(fields_[i], simT, la, lo);
        if (distNm(lat, lon, la, lo) > maxNm) { fields_.erase(fields_.begin() + (long)i); ++n; } else ++i;
    }
    return n;
}

std::vector<ShearRecord> Engine::records(double t, double gain) const {
    // Fixed cells, not records that move with the field (log 2026-11-06 13:48): a moved record is a new record, and X-Plane fades every
    // new record in over ~2 minutes, so a spreading gust front or a growing microburst ring never reached full strength. The cells sit on
    // a fixed 1.2 nm lattice; only their wind changes as the fields move and evolve, which X-Plane blends in place. Each cell carries the
    // model's horizontal perturbation at 500 ft above the ground (the strongest part of an outflow), where it is 3 kt or more.
    std::vector<ShearRecord> out;
    const double step = 1.2;                           // nm
    const double dLat = step / 60.0;
    std::set<std::pair<long, long>> done;
    for (const auto& f : fields_) {
        if (intensity(f, t) < 0.05) continue;
        double cLat, cLon, reach;
        position(f, t, cLat, cLon);
        if (f.type == kMicroburst) reach = outflowRadiusNm(f, t) + 3.0;
        else {
            double fLat, fLon;
            dest(cLat, cLon, f.outDirDeg, outflowRadiusNm(f, t), fLat, fLon);
            dest(fLat, fLon, f.outDirDeg + 180.0, 3.0, cLat, cLon);   // the outflow lies behind the front
            reach = 10.0;
        }
        const double cosRef = std::max(0.2, std::cos(rad(std::round(cLat))));   // whole-degree reference: the lattice never shifts
        const double dLon = step / 60.0 / cosRef;
        const long j0 = (long)std::floor((cLat - reach / 60.0) / dLat), j1 = (long)std::ceil((cLat + reach / 60.0) / dLat);
        const long i0 = (long)std::floor((cLon - reach / 60.0 / cosRef) / dLon), i1 = (long)std::ceil((cLon + reach / 60.0 / cosRef) / dLon);
        for (long j = j0; j <= j1; ++j)
            for (long i = i0; i <= i1; ++i) {
                if (!done.insert({i, j}).second) continue;
                const double la = j * dLat, lo = i * dLon;
                if (distNm(cLat, cLon, la, lo) > reach) { done.erase({i, j}); continue; }
                const Vec3 p = perturbation(la, lo, 500.0, t);   // all fields together, so overlapping ones add up once
                const double v = std::sqrt(p.east * p.east + p.north * p.north);
                if (v < 3.0) continue;
                ShearRecord r;
                r.lat = la; r.lon = lo; r.radiusNm = 1.0f;
                r.towardDeg = (float)norm360(deg(std::atan2(p.east, p.north)));
                r.kt = (float)std::min(60.0, v * gain);
                r.topAglFt = 3000; r.fieldId = f.id;
                r.key = "WSc" + std::to_string(i) + ":" + std::to_string(j);
                out.push_back(r);
            }
    }
    return out;
}

Vec3 Engine::perturbation(double lat, double lon, double aglFt, double t) const {
    Vec3 v;
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(aglFt)) return v;
    for (const auto& f : fields_) {
        const double I = intensity(f, t);
        if (I <= 0) continue;
        double cLat, cLon;
        position(f, t, cLat, cLon);
        if (f.type == kMicroburst) {
            const double r = distNm(cLat, cLon, lat, lon), R = outflowRadiusNm(f, t);
            if (r > R + 4.0) continue;
            const double prof = r < R ? r / R : std::exp(-(r - R) / 0.8);   // outflow peaks at the ring, fades beyond it
            const double V = f.peakKt * I * prof * outflowZ(aglFt, 3000.0);
            const double b = rad(bearing(cLat, cLon, lat, lon));
            if (r > 1e-4) { v.east += V * std::sin(b); v.north += V * std::cos(b); }
            // downdraft core (< 1 statute mile across), weakening towards the ground where it turns into the outflow
            const double zf = aglFt < 300 ? std::max(0.0, aglFt / 300.0) : (aglFt < 6000 ? 1.0 : std::max(0.0, 1.0 - (aglFt - 6000.0) / 3000.0));
            v.up -= f.downFpm * I * std::exp(-std::pow(r / 0.45, 2)) * zf;
        } else {
            const double D = outflowRadiusNm(f, t);
            double fLat, fLon;
            dest(cLat, cLon, f.outDirDeg, D, fLat, fLon);
            // position relative to the front: s across it (positive ahead, away from the parent), a along it
            const double d = distNm(fLat, fLon, lat, lon), bb = rad(bearing(fLat, fLon, lat, lon) - f.outDirDeg);
            const double s = d * std::cos(bb), a = d * std::sin(bb);
            const double across = 0.5 * (1.0 - std::tanh(s / 0.7));      // 1 behind the front, 0 ahead, ~1.5 nm wide transition
            const double along = std::exp(-std::pow(std::max(0.0, std::fabs(a) - 6.0) / 2.0, 2));
            const double V = f.peakKt * I * across * along * outflowZ(aglFt, 3500.0) * (s < -8.0 ? std::exp((s + 8.0) / 4.0) : 1.0);
            v.east += V * std::sin(rad(f.outDirDeg)); v.north += V * std::cos(rad(f.outDirDeg));
        }
    }
    return v;
}

}  // namespace shear
}  // namespace wx
