#include "viewer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "wxmodel.h"

namespace wx {
namespace {

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::string hhmmZ(std::time_t t) {
    std::tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    return fmt("%02d:%02dZ", g.tm_hour, g.tm_min);
}

} // namespace

static void addServices(std::vector<ViewLine>& v, const std::vector<ServiceStatus>& a, const std::vector<ServiceStatus>* extra, std::time_t now) {
    std::vector<ServiceStatus> all = a;
    if (extra) all.insert(all.end(), extra->begin(), extra->end());
    if (all.empty()) return;
    v.push_back({LineKind::Blank, ""});
    v.push_back({LineKind::Header, "Data services"});
    for (const auto& s : all) {
        // the other region's radar and turbulence: only the services that apply where the aircraft is are listed
        if (s.state == SvcState::NotHere && (s.use.find("(US)") != std::string::npos || s.use.find("(Europe)") != std::string::npos ||
                                             s.use.find("(outside the US)") != std::string::npos)) continue;
        LineKind k = LineKind::SvcIdle;
        if (s.state == SvcState::Ok) k = LineKind::SvcOk;
        else if (s.state == SvcState::Degraded) k = LineKind::SvcWarn;
        else if (s.state == SvcState::Down) k = LineKind::SvcDown;
        std::string line = std::string(svcLabel(s.state)) + "   " + s.name + " - " + s.use;
        if (!s.detail.empty()) line += ": " + s.detail;
        if ((s.state == SvcState::Down || s.state == SvcState::Degraded) && s.lastOk > 0)
            line += fmt(" (last good %s, %ld min ago)", hhmmZ(s.lastOk).c_str(), (long)((now - s.lastOk) / 60));
        v.push_back({k, line});
    }
}

std::vector<ViewLine> buildViewLines(const FetchResult& res, bool haveData, const Config& cfg, std::time_t now, const std::vector<ServiceStatus>* extra) {
    std::vector<ViewLine> v;
    auto add = [&](LineKind k, const std::string& s) { v.push_back({k, s}); };

    if (!haveData) {
        add(LineKind::Title, "WXPlane - local weather data");
        add(LineKind::Note, "No weather has been downloaded yet. Use \"Refresh Weather Data Now\" and reopen this window.");
        addServices(v, res.services, extra, now);
        return v;
    }
    add(LineKind::Title, fmt("WXPlane - %zu METAR station(s), %d ATIS, updated %s", res.stations.size(), res.atisCount,
                             hhmmZ(res.fetchedAt).c_str()));
    if (!res.error.empty()) add(LineKind::Note, "Last error: " + res.error);
    add(LineKind::Blank, "");

    for (const auto& st : res.stations) {
        std::string head = fmt("%s   %.0f nm   field %.0f m (%.0f ft)", st.icao.c_str(), st.distNm, st.elevM, st.elevM / 0.3048);
        if (st.hasAtis) head += "   ATIS " + (st.atisLetter.empty() ? std::string("?") : st.atisLetter);
        add(LineKind::Header, head);
        // Some reports (UK AUTO stations) already begin with "METAR"/"SPECI": no second label then
        const bool typed = st.rawMetar.compare(0, 6, "METAR ") == 0 || st.rawMetar.compare(0, 6, "SPECI ") == 0;
        add(LineKind::Metar, st.rawMetar.empty() ? std::string("(no METAR text)") : typed ? st.rawMetar : "METAR  " + st.rawMetar);
        if (st.hasAtis) add(LineKind::Atis, "ATIS   " + st.atisText);
        if (!st.filled.empty()) add(LineKind::Note, "Completed: " + st.filled);
        XpWx x = buildXp(st, cfg, now, &res.aloft, true);
        add(LineKind::Applied, "Applied  " + describeXp(x, st.elevM));
        add(LineKind::Blank, "");
    }
    if (res.stations.empty()) { add(LineKind::Note, "No METAR stations were found near the aircraft."); add(LineKind::Blank, ""); }

    // Radar: the header names the region and its network, so it changes as the aircraft moves between the US, Europe and elsewhere
    const std::string region = radarRegion(res.centerLat, res.centerLon);
    if (!cfg.radar) {
        add(LineKind::Header, "Radar: off");
    } else if (region.empty() && !res.radar.ok) {
        add(LineKind::Header, "Radar: no coverage here");
        add(LineKind::Note, "Radar storms cover the US (NOAA MRMS) and Europe (EUMETNET OPERA). Here, thunderstorms come from the METARs (TS, CB).");
    } else {
        const RadarResult& r = res.radar;
        const std::string reg = region.empty() ? std::string("") : " - " + region;
        if (r.ok) {
            std::string age;
            if (r.dataTime > 0) age = fmt(", valid %s (%ld min old)", hhmmZ(r.dataTime).c_str(), (long)((now - r.dataTime) / 60));
            add(LineKind::Header, fmt("Radar%s: %s - %zu storm cells applied of %d echo blocks%s", reg.c_str(), r.source.c_str(), r.cells.size(), r.echoBlocks, age.c_str()));
            add(LineKind::Note, r.topsFromRadar ? "Storm tops from radar echo tops." : "Storm tops estimated from reflectivity (no echo-top data).");
            if (!r.note.empty()) add(LineKind::Note, r.note);
            std::vector<const RadarCell*> strong;
            for (const auto& c : r.cells) strong.push_back(&c);
            std::sort(strong.begin(), strong.end(), [](const RadarCell* a, const RadarCell* b) { return a->dbz > b->dbz; });
            for (size_t i = 0; i < strong.size() && i < 12; ++i) {
                const RadarCell& c = *strong[i];
                add(LineKind::Applied, fmt("  %.0f dBZ  rain %.0f mm/h  top %.0f ft%s  %.0f nm away  (%.2f, %.2f)", c.dbz, c.rateMmH,
                                           c.topM / 0.3048, c.topKnown ? "" : " (est.)", c.distNm, c.lat, c.lon));
            }
        } else {
            add(LineKind::Header, "Radar" + reg + ": " + radarProvider(region) + " unavailable");
            std::string note = r.note.empty() ? std::string("not fetched yet") : r.note;
            const std::string pre = "European radar (OPERA): ";
            if (note.compare(0, pre.size(), pre) == 0) note = note.substr(pre.size());
            add(LineKind::Note, note);
        }
    }
    add(LineKind::Blank, "");

    addServices(v, res.services, extra, now);
    return v;
}

std::string radarRegion(double lat, double lon) {
    if (!mrmsRegionsFor(lat, lon).empty()) return "US";
    if (operaCovers(lat, lon)) return "Europe";
    return "";
}

std::string radarProvider(const std::string& region) {
    return region == "US" ? "NOAA MRMS" : region == "Europe" ? "EUMETNET OPERA" : "";
}

std::vector<std::string> wrapText(const std::string& s, size_t maxChars) {
    std::vector<std::string> out;
    if (maxChars < 8) maxChars = 8;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        size_t end = i + maxChars;
        if (end >= s.size()) { out.push_back(s.substr(i)); break; }
        size_t br = s.rfind(' ', end);
        if (br == std::string::npos || br <= i) br = end;
        out.push_back(s.substr(i, br - i));
        i = br;
    }
    if (out.empty()) out.push_back("");
    return out;
}

} // namespace wx
