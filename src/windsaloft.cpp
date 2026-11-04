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

bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> t;
    std::istringstream is(line);
    std::string w;
    while (is >> w) t.push_back(w);
    return t;
}

// "2625+11", "3047-30", "305945" (above 24000 ft the minus sign is implied), "9900", "751145"
bool decodeEntry(const std::string& e, double altFt, AloftLevel& L) {
    if (e.size() < 4 || !isDigits(e.substr(0, 4))) return false;
    int dd = std::atoi(e.substr(0, 2).c_str());
    int ss = std::atoi(e.substr(2, 2).c_str());
    L = AloftLevel();
    L.altFt = altFt;
    L.hasWind = true;
    if (dd == 99) { // light and variable
        L.dirDeg = 0; L.speedKt = 0;
    } else {
        if (dd >= 51 && dd <= 86) { dd -= 50; ss += 100; } // speeds of 100 kt or more
        L.dirDeg = dd * 10.0;
        L.speedKt = ss;
    }
    std::string rest = e.substr(4);
    if (!rest.empty()) {
        char sign = 0;
        if (rest[0] == '+' || rest[0] == '-') { sign = rest[0]; rest.erase(0, 1); }
        if (rest.size() == 2 && isDigits(rest)) {
            double t = std::atof(rest.c_str());
            if (sign == '-' || (sign == 0 && altFt > 24000)) t = -t;
            L.hasTemp = true;
            L.tempC = t;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<AloftStation> parseWindTemp(const std::string& text) {
    std::vector<AloftStation> out;
    std::vector<double> levels;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> t = split(line);
        if (t.empty()) continue;
        if (t[0] == "FT" && t.size() > 2 && isDigits(t[1])) {
            levels.clear();
            for (size_t i = 1; i < t.size(); ++i)
                if (isDigits(t[i])) levels.push_back(std::atof(t[i].c_str()));
            continue;
        }
        if (levels.empty()) continue;
        const std::string& id = t[0];
        if (id.size() != 3 || !std::all_of(id.begin(), id.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) continue;
        size_t n = t.size() - 1;
        if (n == 0 || n > levels.size()) continue;
        AloftStation st;
        st.id = id;
        // Levels below the station's terrain are left blank, so entries align with the highest levels.
        size_t first = levels.size() - n;
        for (size_t j = 0; j < n; ++j) {
            AloftLevel L;
            if (decodeEntry(t[1 + j], levels[first + j], L)) st.levels.push_back(L);
        }
        if (!st.levels.empty()) out.push_back(std::move(st));
    }
    return out;
}

void mergeAloft(std::vector<AloftStation>& into, const std::vector<AloftStation>& more) {
    for (const auto& m : more) {
        auto it = std::find_if(into.begin(), into.end(), [&](const AloftStation& s) { return s.id == m.id; });
        if (it == into.end()) { into.push_back(m); continue; }
        for (const auto& L : m.levels) {
            bool have = std::any_of(it->levels.begin(), it->levels.end(), [&](const AloftLevel& x) { return x.altFt == L.altFt; });
            if (!have) it->levels.push_back(L);
        }
        std::sort(it->levels.begin(), it->levels.end(), [](const AloftLevel& a, const AloftLevel& b) { return a.altFt < b.altFt; });
    }
}

std::string windTempUrl(const std::string& region, const std::string& level, int fcstHours) {
    int f = fcstHours <= 6 ? 6 : (fcstHours <= 12 ? 12 : 24);
    char buf[200];
    std::snprintf(buf, sizeof buf, "https://aviationweather.gov/api/data/windtemp?region=%s&level=%s&fcst=%02d",
                  region.c_str(), level.c_str(), f);
    return buf;
}

void aloftRegionFor(double lat, double lon, std::string& region, char& icaoPrefix) {
    if (lat > 51 && lon < -129) { region = "alaska"; icaoPrefix = 'P'; }
    else if (lat < 25 && lon < -150) { region = "hawaii"; icaoPrefix = 'P'; }
    else { region = "all"; icaoPrefix = 'K'; }
}

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

} // namespace wx
