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

std::vector<ViewLine> buildViewLines(const FetchResult& res, bool haveData, const Config& cfg, std::time_t now) {
    std::vector<ViewLine> v;
    auto add = [&](LineKind k, const std::string& s) { v.push_back({k, s}); };

    if (!haveData) {
        add(LineKind::Title, "WXPlane - local weather data");
        add(LineKind::Note, "No weather has been downloaded yet. Use \"Refresh Weather Data Now\" and reopen this window.");
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
        add(LineKind::Metar, st.rawMetar.empty() ? std::string("(no METAR text)") : "METAR  " + st.rawMetar);
        if (st.hasAtis) add(LineKind::Atis, "ATIS   " + st.atisText);
        XpWx x = buildXp(st, cfg, now, &res.aloft, true);
        add(LineKind::Applied, "Applied  " + describeXp(x, st.elevM));
        add(LineKind::Blank, "");
    }
    if (res.stations.empty()) { add(LineKind::Note, "No METAR stations were found near the aircraft."); add(LineKind::Blank, ""); }

    // Radar
    if (cfg.radar) {
        const RadarResult& r = res.radar;
        if (r.ok) {
            std::string age;
            if (r.dataTime > 0) age = fmt(", valid %s (%ld min old)", hhmmZ(r.dataTime).c_str(), (long)((now - r.dataTime) / 60));
            add(LineKind::Header, fmt("Radar: %s - %zu storm cells applied of %d echo blocks%s", r.source.c_str(), r.cells.size(), r.echoBlocks, age.c_str()));
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
            add(LineKind::Header, "Radar: none");
            add(LineKind::Note, r.note.empty() ? std::string("not fetched yet") : r.note);
        }
    } else {
        add(LineKind::Header, "Radar: off");
    }
    add(LineKind::Blank, "");

    add(LineKind::Header, fmt("NOAA winds/temps aloft stations nearby: %zu", res.aloft.size()));
    if (!res.aloftNote.empty()) add(LineKind::Note, res.aloftNote);
    std::string row;
    for (const auto& a : res.aloft) {
        std::string one = a.id + (a.located ? "" : "?") + "  ";
        if (row.size() + one.size() > 70) { add(LineKind::Note, row); row.clear(); }
        row += one;
    }
    if (!row.empty()) add(LineKind::Note, row);
    return v;
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

std::string flightCategory(const Station& st) {
    const Report& r = st.report;
    if (st.rawMetar.empty() && !r.hasVis && r.clouds.empty() && !r.cavok) return "";
    double visSm = r.cavok || !r.hasVis ? 99.0 : r.visM / 1609.344;
    double ceilFt = 1e9;
    for (const auto& c : r.clouds)
        if (c.cover >= CloudGroup::BKN) ceilFt = std::min(ceilFt, (double)c.baseFtAgl);
    if (visSm < 1.0 || ceilFt < 500) return "LIFR";
    if (visSm < 3.0 || ceilFt < 1000) return "IFR";
    if (visSm <= 5.0 || ceilFt <= 3000) return "MVFR";
    return "VFR";
}

std::vector<std::string> mapStationLines(const Station& st, bool detailed, size_t wrapChars, size_t maxAtisLines) {
    std::vector<std::string> out;
    std::string cat = flightCategory(st);
    if (!detailed) {
        std::string t = st.icao;
        if (!cat.empty()) t += " " + cat;
        const Report& r = st.report;
        if (r.hasWind) {
            char b[48];
            char dir[8];
            if (r.windVariable) std::snprintf(dir, sizeof dir, "VRB"); else std::snprintf(dir, sizeof dir, "%03d", r.windDir);
            if (r.windKt < 1) std::snprintf(b, sizeof b, " calm");
            else if (r.gustKt > 0) std::snprintf(b, sizeof b, " %s%.0fG%.0fKT", dir, r.windKt, r.gustKt);
            else std::snprintf(b, sizeof b, " %s%.0fKT", dir, r.windKt);
            t += b;
        }
        if (st.hasAtis) t += " ATIS " + (st.atisLetter.empty() ? std::string("?") : st.atisLetter);
        out.push_back(t);
        return out;
    }
    out.push_back(st.icao + (cat.empty() ? "" : "  " + cat) + (st.hasAtis ? "  ATIS " + (st.atisLetter.empty() ? std::string("?") : st.atisLetter) : ""));
    for (auto& l : wrapText(st.rawMetar.empty() ? std::string("(no METAR text)") : st.rawMetar, wrapChars)) out.push_back(l);
    if (st.hasAtis) {
        auto a = wrapText("ATIS " + st.atisText, wrapChars);
        for (size_t i = 0; i < a.size() && i < maxAtisLines; ++i) out.push_back(i + 1 == maxAtisLines && a.size() > maxAtisLines ? a[i] + " ..." : a[i]);
    }
    return out;
}

} // namespace wx
