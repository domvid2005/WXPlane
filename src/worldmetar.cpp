#include "worldmetar.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "metar.h"

namespace wx {

namespace {

// one CSV row: commas outside double quotes split, "" inside quotes is a quote
void splitCsv(const std::string& line, std::vector<std::string>& f) {
    f.clear();
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { f.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    f.push_back(cur);
}

// "2026-11-17T17:53:00Z" (or with a space): unix time, 0 when unreadable (days from civil, Howard Hinnant; no timegm)
std::time_t isoUtc(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5) return 0;
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + doe - 719468;
    return (std::time_t)(days * 86400L + h * 3600L + mi * 60L + se);
}

bool num(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* e = nullptr;
    v = std::strtod(s.c_str(), &e);
    return e && e != s.c_str();
}

} // namespace

bool parseWorldMetarCsv(const std::string& csv, std::time_t now, std::vector<Station>& out, double maxAgeS) {
    out.clear();
    std::istringstream in(csv);
    std::string line;
    std::vector<std::string> f;
    int cRaw = -1, cId = -1, cLat = -1, cLon = -1, cTime = -1, cElev = -1;
    bool header = false;
    while (std::getline(in, line)) {
        if (!header) {
            if (line.compare(0, 9, "raw_text,") != 0) continue;   // the preamble ("No errors", "5123 results" ...)
            splitCsv(line, f);
            for (int i = 0; i < (int)f.size(); ++i) {
                const std::string& n = f[(size_t)i];
                if (n == "raw_text") cRaw = i;
                else if (n == "station_id") cId = i;
                else if (n == "latitude") cLat = i;
                else if (n == "longitude") cLon = i;
                else if (n == "observation_time") cTime = i;
                else if (n == "elevation_m") cElev = i;
            }
            if (cRaw < 0 || cId < 0 || cLat < 0 || cLon < 0) return false;
            header = true;
            continue;
        }
        if (line.empty()) continue;
        splitCsv(line, f);
        const int need = std::max(std::max(cRaw, cId), std::max(cLat, cLon));
        if ((int)f.size() <= need) continue;
        Station st;
        st.icao = f[(size_t)cId];
        st.rawMetar = f[(size_t)cRaw];
        if (st.icao.empty() || st.rawMetar.empty()) continue;
        if (!num(f[(size_t)cLat], st.lat) || !num(f[(size_t)cLon], st.lon)) continue;
        if (st.lat < -90 || st.lat > 90 || st.lon < -180 || st.lon > 180) continue;
        if (cElev >= 0 && cElev < (int)f.size()) num(f[(size_t)cElev], st.elevM);
        if (cTime >= 0 && cTime < (int)f.size()) st.obsTime = (long long)isoUtc(f[(size_t)cTime]);
        if (st.obsTime > 0 && maxAgeS > 0 && (double)(now - (std::time_t)st.obsTime) > maxAgeS) continue;
        if (!parseReport(st.rawMetar, st.report)) continue;
        out.push_back(std::move(st));
    }
    return header;
}

namespace {
bool utcParts(std::time_t t, std::tm& out) {
#ifdef _WIN32
    return gmtime_s(&out, &t) == 0;
#else
    return gmtime_r(&t, &out) != nullptr;
#endif
}
} // namespace

std::string xpMetarFileName(std::time_t now) {
    std::tm u{};
    if (!utcParts(now, u)) return "";
    char b[64];
    std::snprintf(b, sizeof b, "METAR-%d-%d-%d-%02d.00-ZULU.txt", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour);
    return b;
}

std::string formatXpMetarFile(const std::vector<Station>& stations, std::time_t now) {
    std::vector<const Station*> v;
    v.reserve(stations.size());
    for (const auto& s : stations) {
        if (s.icao.size() < 3 || s.rawMetar.empty()) continue;
        bool clean = true;
        for (unsigned char c : s.rawMetar) if (c < 0x20 || c > 0x7e) { clean = false; break; }
        if (clean) v.push_back(&s);
    }
    std::sort(v.begin(), v.end(), [](const Station* a, const Station* b) { return a->icao < b->icao; });
    std::string out;
    out.reserve(v.size() * 110);
    char d[64];
    for (const Station* s : v) {
        std::tm u{};
        if (!utcParts(s->obsTime > 0 ? (std::time_t)s->obsTime : now, u)) continue;
        std::snprintf(d, sizeof d, "%04d/%02d/%02d %02d:%02d\n", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min);
        out += d;
        out += s->rawMetar;
        out += "\n\n";
    }
    return out;
}

std::string writeXpMetarFile(const std::string& dir, const std::vector<Station>& stations, std::time_t now,
                             const std::string& previous, std::string* err) {
    namespace fs = std::filesystem;
    const std::string name = xpMetarFileName(now);
    const std::string text = formatXpMetarFile(stations, now);
    if (name.empty() || text.empty()) { if (err) *err = "no reports to write"; return ""; }
    std::error_code ec;
    const fs::path d = fs::u8path(dir);
    fs::create_directories(d, ec);
    if (ec) { if (err) *err = "could not create the folder (" + ec.message() + ")"; return ""; }
    const fs::path tmp = d / "WXPlane-METAR.tmp", dst = d / fs::u8path(name);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "could not open a file there"; return ""; }
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) { if (err) *err = "could not write the file"; f.close(); fs::remove(tmp, ec); return ""; }
    }
    fs::rename(tmp, dst, ec);   // replaces a file of that name (an earlier write this hour, or X-Plane's own for this hour)
    if (ec) { if (err) *err = "could not put the file in place (" + ec.message() + ")"; fs::remove(tmp, ec); return ""; }
    if (!previous.empty() && previous != name) fs::remove(d / fs::u8path(previous), ec);
    return name;
}

} // namespace wx
