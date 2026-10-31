#include "windsaloft.h"

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

std::vector<AloftSample> sampleAloft(const std::vector<AloftStation>& stations, double lat, double lon,
                                     double maxRangeNm, int maxStations) {
    struct Near { const AloftStation* s; double d; };
    std::vector<Near> near;
    for (const auto& s : stations) {
        if (!s.located) continue;
        double d = distNm(lat, lon, s.lat, s.lon);
        if (d <= maxRangeNm) near.push_back({&s, d});
    }
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
    if ((int)near.size() > maxStations) near.resize((size_t)maxStations);

    std::vector<double> alts;
    for (const auto& n : near)
        for (const auto& L : n.s->levels)
            if (std::find(alts.begin(), alts.end(), L.altFt) == alts.end()) alts.push_back(L.altFt);
    std::sort(alts.begin(), alts.end());

    std::vector<AloftSample> out;
    for (double alt : alts) {
        double wU = 0, wV = 0, wSum = 0, tSum = 0, tW = 0;
        for (const auto& n : near) {
            for (const auto& L : n.s->levels) {
                if (L.altFt != alt) continue;
                double w = 1.0 / std::max(25.0, n.d * n.d); // floor keeps one station from taking all weight
                if (L.hasWind) {
                    double rad = L.dirDeg * kPi / 180.0;
                    // direction the wind blows FROM -> components of the vector it blows TOWARD
                    wU += w * (-L.speedKt * std::sin(rad));
                    wV += w * (-L.speedKt * std::cos(rad));
                    wSum += w;
                }
                if (L.hasTemp) { tSum += w * L.tempC; tW += w; }
            }
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

} // namespace wx
