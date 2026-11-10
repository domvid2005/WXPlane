#include "windsaloft.h"
#include "json_mini.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

#include "geo.h"

namespace wx {
namespace {

const double kPi = 3.14159265358979323846;

} // namespace

namespace {
// One station's value at an altitude, filled in where the station has no report of its own: linear between the levels above and below
// (a missing level inside its profile), or held from the nearest level when that is within 6000 ft. No value beyond that, so a station
// that only has the low-level product never contributes to FL390.
struct StationProfile {
    std::vector<AloftLevel> w, t;   // levels that carry a wind / a temperature, ascending
};

StationProfile profileOf(const AloftStation& s) {
    StationProfile p;
    for (const auto& L : s.levels) { if (L.hasWind) p.w.push_back(L); if (L.hasTemp) p.t.push_back(L); }
    return p;
}

template <class V, class Get>
bool valueAt(const std::vector<AloftLevel>& lv, double alt, Get get, V& out) {
    if (lv.empty()) return false;
    for (const auto& L : lv) if (L.altFt == alt) { out = get(L); return true; }
    if (alt < lv.front().altFt) { if (lv.front().altFt - alt > 6000.0) return false; out = get(lv.front()); return true; }
    if (alt > lv.back().altFt) { if (alt - lv.back().altFt > 6000.0) return false; out = get(lv.back()); return true; }
    for (size_t i = 1; i < lv.size(); ++i) {
        if (alt < lv[i].altFt) {
            const double f = (alt - lv[i - 1].altFt) / (lv[i].altFt - lv[i - 1].altFt);
            const V a = get(lv[i - 1]), b = get(lv[i]);
            out = a + (b - a) * f;
            return true;
        }
    }
    return false;
}
}   // namespace

static std::vector<AloftSample> sampleStations(const std::vector<AloftStation>& stations, double lat, double lon,
                                              double maxRangeNm, int maxStations, double softLimitNm) {
    struct Near { const AloftStation* s; double d; };
    std::vector<Near> all;
    for (const auto& s : stations) {
        if (!s.located) continue;
        all.push_back({&s, distNm(lat, lon, s.lat, s.lon)});
    }
    std::sort(all.begin(), all.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    std::vector<Near> near;
    for (const auto& n : all) if (n.d <= maxRangeNm && (int)near.size() < maxStations) near.push_back(n);
    // No FD station within range (the edge of the coverage, or far from every report): use the nearest ones anyway, up to 1000 nm,
    // rather than leave this place to the sim's own winds.
    if (near.empty())
        for (const auto& n : all) if (n.d <= softLimitNm && (int)near.size() < maxStations) near.push_back(n);

    std::vector<double> alts;
    for (const auto& n : near)
        for (const auto& L : n.s->levels)
            if (std::find(alts.begin(), alts.end(), L.altFt) == alts.end()) alts.push_back(L.altFt);
    std::sort(alts.begin(), alts.end());
    std::vector<StationProfile> prof;
    for (const auto& n : near) prof.push_back(profileOf(*n.s));

    std::vector<AloftSample> out;
    for (double alt : alts) {
        double wU = 0, wV = 0, wSum = 0, tSum = 0, tW = 0;
        for (size_t k = 0; k < near.size(); ++k) {
            const double w = 1.0 / std::max(25.0, near[k].d * near[k].d); // floor keeps one station from taking all weight
            double u = 0, v = 0, tc = 0;
            struct UV { double u, v; UV operator+(const UV& o) const { return {u + o.u, v + o.v}; } UV operator-(const UV& o) const { return {u - o.u, v - o.v}; } UV operator*(double f) const { return {u * f, v * f}; } };
            UV uv{0, 0};
            auto getUV = [](const AloftLevel& L) { const double rad = L.dirDeg * kPi / 180.0; return UV{-L.speedKt * std::sin(rad), -L.speedKt * std::cos(rad)}; };
            // direction the wind blows FROM -> components of the vector it blows TOWARD; levels are interpolated as vectors, so a turn of the wind with height stays smooth
            if (valueAt<UV>(prof[k].w, alt, getUV, uv)) { u = uv.u; v = uv.v; wU += w * u; wV += w * v; wSum += w; }
            if (valueAt<double>(prof[k].t, alt, [](const AloftLevel& L) { return L.tempC; }, tc)) { tSum += w * tc; tW += w; }
        }
        AloftSample s;
        s.altFt = alt;
        if (wSum > 0) {
            double u = wU / wSum, v = wV / wSum;
            s.hasWind = true;
            s.speedKt = std::sqrt(u * u + v * v);
            double dir = std::atan2(-u, -v) * 180.0 / kPi;
            if (dir < 0) dir += 360.0;
            s.dirDeg = s.speedKt < 0.5 ? 0 : dir;
        }
        if (tW > 0) { s.hasTemp = true; s.tempC = tSum / tW; }
        if (s.hasWind || s.hasTemp) out.push_back(s);
    }
    return out;
}

std::vector<AloftSample> sampleAloft(const std::vector<AloftStation>& stations, double lat, double lon,
                                     double maxRangeNm, int maxStations) {
    std::vector<AloftStation> fd, mdl;
    for (const auto& s : stations) (s.model ? mdl : fd).push_back(s);
    std::vector<AloftSample> out = sampleStations(fd, lat, lon, maxRangeNm, maxStations, 1000.0);
    if (mdl.empty()) return out;
    // The model fills what FD cannot give: everything where no FD station is within reach, and the levels above the top FD level.
    std::vector<AloftSample> m = sampleStations(mdl, lat, lon, 2500.0, 4, 2500.0);
    double topFd = -1;
    for (const auto& q : out) if (q.hasWind) topFd = std::max(topFd, q.altFt);
    for (const auto& q : m)
        if (q.altFt > topFd + 1000.0 && q.altFt <= 70000.0) out.push_back(q);
    std::sort(out.begin(), out.end(), [](const AloftSample& a, const AloftSample& b) { return a.altFt < b.altFt; });
    return out;
}

double isaAltFtFromHpa(double hpa) {
    if (hpa <= 0) return 0;
    double m;
    if (hpa >= 226.32) m = 44330.77 * (1.0 - std::pow(hpa / 1013.25, 0.190263));
    else m = 11000.0 + 6341.62 * std::log(226.32 / hpa);
    return m / 0.3048;
}

std::string openMeteoUrl(const std::vector<std::pair<double, double>>& pts, const std::vector<int>& levels, bool highCloud) {
    std::string la, lo, vars;
    char b[64];
    for (size_t i = 0; i < pts.size(); ++i) {
        std::snprintf(b, sizeof b, "%.2f", pts[i].first); la += (i ? "," : "") + std::string(b);
        std::snprintf(b, sizeof b, "%.2f", pts[i].second); lo += (i ? "," : "") + std::string(b);
    }
    for (int l : levels) {
        std::snprintf(b, sizeof b, "%d", l);
        const std::string n = b;
        vars += (vars.empty() ? "" : ",") + std::string("wind_speed_") + n + "hPa,wind_direction_" + n + "hPa,temperature_" + n + "hPa,geopotential_height_" + n + "hPa";
    }
    if (highCloud) vars += ",cloud_cover_high";   // high cloud (cirrus): METARs from automatic stations cannot see above 12,000 ft
    return "https://api.open-meteo.com/v1/gfs?latitude=" + la + "&longitude=" + lo + "&hourly=" + vars +
           "&wind_speed_unit=kn&forecast_hours=2&timeformat=unixtime";
}

std::vector<AloftStation> parseOpenMeteo(const std::string& body, const std::vector<std::pair<double, double>>& pts,
                                         const std::vector<int>& levels, long long now, std::string* err) {
    std::vector<AloftStation> out;
    Json root;
    if (!parseJson(body, root)) { if (err) *err = "unreadable answer from open-meteo.com"; return out; }
    std::vector<const Json*> locs;
    if (root.type == Json::Arr) for (const auto& o : root.arr) locs.push_back(&o);
    else if (root.type == Json::Obj) {
        if (root.get("error")) { const Json* r = root.get("reason"); if (err) *err = "open-meteo.com: " + (r ? r->asString() : std::string("error")); return out; }
        locs.push_back(&root);
    }
    for (size_t i = 0; i < locs.size(); ++i) {
        const Json* h = locs[i]->get("hourly");
        if (!h || h->type != Json::Obj) continue;
        size_t idx = 0;
        const Json* tm = h->get("time");
        if (tm && tm->type == Json::Arr && !tm->arr.empty()) {
            double best = 1e18;
            for (size_t k = 0; k < tm->arr.size(); ++k) { const double d = std::fabs(tm->arr[k].asNum(0) - (double)now); if (d < best) { best = d; idx = k; } }
        }
        auto val = [&](const std::string& name, double& v) {
            const Json* a = h->get(name);
            if (!a || a->type != Json::Arr || idx >= a->arr.size() || a->arr[idx].type != Json::Num) return false;
            v = a->arr[idx].num; return true;
        };
        AloftStation st;
        char b[24];
        std::snprintf(b, sizeof b, "GFS%zu", i);
        st.id = b; st.model = true; st.located = true;
        const Json* la = locs[i]->get("latitude"); const Json* lo = locs[i]->get("longitude");
        st.lat = la ? la->asNum() : (i < pts.size() ? pts[i].first : 0);
        st.lon = lo ? lo->asNum() : (i < pts.size() ? pts[i].second : 0);
        for (int l : levels) {
            const std::string n = std::to_string(l);
            double sp = 0, dr = 0, tc = 0, gh = 0;
            AloftLevel L;
            const bool gotGh = val("geopotential_height_" + n + "hPa", gh);
            L.altFt = gotGh ? gh / 0.3048 : isaAltFtFromHpa(l);
            if (val("wind_speed_" + n + "hPa", sp) && val("wind_direction_" + n + "hPa", dr) && sp >= 0) { L.hasWind = true; L.speedKt = sp; L.dirDeg = dr; }
            if (val("temperature_" + n + "hPa", tc)) { L.hasTemp = true; L.tempC = tc; }
            if (L.hasWind || L.hasTemp) st.levels.push_back(L);
        }
        { double hc = 0; if (val("cloud_cover_high", hc) && hc >= 0) st.highCloud = std::min(1.0, hc / 100.0); }
        std::sort(st.levels.begin(), st.levels.end(), [](const AloftLevel& a, const AloftLevel& c) { return a.altFt < c.altFt; });
        if (!st.levels.empty()) out.push_back(std::move(st));
    }
    if (out.empty() && err && err->empty()) *err = "open-meteo.com answered without wind levels";
    return out;
}

const std::vector<int>& modelCloudLevels() {
    static const std::vector<int> L = {1000, 925, 850, 700, 600, 500, 400, 300, 250, 200, 150};
    return L;
}

std::string openMeteoCloudUrl(const std::vector<std::pair<double, double>>& pts) {
    std::string la, lo, vars;
    char b[64];
    for (size_t i = 0; i < pts.size(); ++i) {
        std::snprintf(b, sizeof b, "%.2f", pts[i].first); la += (i ? "," : "") + std::string(b);
        std::snprintf(b, sizeof b, "%.2f", pts[i].second); lo += (i ? "," : "") + std::string(b);
    }
    for (int l : modelCloudLevels()) vars += (vars.empty() ? "" : ",") + std::string("cloud_cover_") + std::to_string(l) + "hPa,geopotential_height_" + std::to_string(l) + "hPa";
    vars += ",temperature_2m,dew_point_2m,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m,visibility,precipitation,snowfall,weather_code";
    return "https://api.open-meteo.com/v1/gfs?latitude=" + la + "&longitude=" + lo + "&hourly=" + vars +
           "&wind_speed_unit=kn&forecast_hours=2&timeformat=unixtime";
}

std::vector<ModelPoint> parseOpenMeteoClouds(const std::string& body, const std::vector<std::pair<double, double>>& pts, long long now, std::string* err) {
    std::vector<ModelPoint> out;
    Json root;
    if (!parseJson(body, root)) { if (err) *err = "unreadable answer from open-meteo.com"; return out; }
    std::vector<const Json*> locs;
    if (root.type == Json::Arr) for (const auto& o : root.arr) locs.push_back(&o);
    else if (root.type == Json::Obj) {
        if (root.get("error")) { const Json* r = root.get("reason"); if (err) *err = "open-meteo.com: " + (r ? r->asString() : std::string("error")); return out; }
        locs.push_back(&root);
    }
    for (size_t i = 0; i < locs.size() && i < pts.size(); ++i) {
        const Json* h = locs[i]->get("hourly");
        if (!h || h->type != Json::Obj) continue;
        size_t idx = 0;
        const Json* tm = h->get("time");
        if (tm && tm->type == Json::Arr && !tm->arr.empty()) {
            double best = 1e18;
            for (size_t k = 0; k < tm->arr.size(); ++k) { const double d = std::fabs(tm->arr[k].asNum(0) - (double)now); if (d < best) { best = d; idx = k; } }
        }
        auto val = [&](const std::string& name, double& v) {
            const Json* a = h->get(name);
            if (!a || a->type != Json::Arr || idx >= a->arr.size() || a->arr[idx].type != Json::Num) return false;
            v = a->arr[idx].num; return true;
        };
        ModelPoint p;
        p.lat = pts[i].first; p.lon = pts[i].second;
        { const Json* e = locs[i]->get("elevation"); if (e && e->type == Json::Num) p.elevM = e->num; }
        for (int l : modelCloudLevels()) {
            double cc = 0, gh = 0;
            if (!val("cloud_cover_" + std::to_string(l) + "hPa", cc) || cc < 0) continue;
            const double alt = val("geopotential_height_" + std::to_string(l) + "hPa", gh) ? gh / 0.3048 : isaAltFtFromHpa(l);
            p.cloudLevels.push_back({alt, std::min(1.0, cc / 100.0)});
        }
        std::sort(p.cloudLevels.begin(), p.cloudLevels.end());
        double v = 0;
        p.sfc = val("temperature_2m", p.tempC) && val("pressure_msl", p.mslHpa) && val("wind_speed_10m", p.windKt) && val("wind_direction_10m", p.windDir);
        if (val("dew_point_2m", v)) p.dewC = v; else p.dewC = p.tempC - 5;
        if (val("wind_gusts_10m", v)) p.gustKt = v;
        if (val("visibility", v)) p.visM = v;
        if (val("precipitation", v)) p.precipMm = std::max(0.0, v);
        if (val("snowfall", v)) p.snowCm = std::max(0.0, v);
        if (val("weather_code", v)) p.weatherCode = (int)v;
        if (!p.cloudLevels.empty() || p.sfc) out.push_back(std::move(p));
    }
    if (out.empty() && err && err->empty()) *err = "open-meteo.com answered without cloud levels";
    return out;
}

} // namespace wx
