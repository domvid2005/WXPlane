#include "sources.h"

#include <algorithm>
#include <cmath>
#include "geo.h"
#include <cstdio>

#include "json_mini.h"

namespace wx {

std::string awcMetarUrl(double lat, double lon, double searchNm) {
    double dLat = searchNm / 60.0;
    double cosLat = std::cos(lat * 3.14159265358979323846 / 180.0);
    double dLon = searchNm / (60.0 * (cosLat < 0.1 ? 0.1 : cosLat));
    double minLat = std::max(-90.0, lat - dLat), maxLat = std::min(90.0, lat + dLat);
    double minLon = std::max(-180.0, lon - dLon), maxLon = std::min(180.0, lon + dLon);
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "https://aviationweather.gov/api/data/metar?bbox=%.3f,%.3f,%.3f,%.3f&format=json",
                  minLat, minLon, maxLat, maxLon);
    return buf;
}

std::string datisUrl(const std::string& icao) { return "https://datis.clowd.io/api/" + icao; }

// NOAA publishes each METAR both as raw text and as decoded fields. The plugin parses the text; this compares the result with
// NOAA's decoded fields. Elements the text parse missed are filled in, and where the two disagree NOAA's decoded value wins.
// Returns a short note ("" when they agree), e.g. "NOAA decoded data: visibility corrected, clouds filled".
static std::string crossCheckAwc(const Json& o, Report& r) {
    std::vector<std::string> notes;
    auto add = [&](const char* w) { notes.push_back(w); };
    if (const Json* v = o.get("visib")) {
        if (v->type == Json::Num || v->type == Json::Str) {
            std::string t = v->asString();
            double sm = std::atof(t.c_str());
            if (sm > 0.0 || t == "0") {
                double m = sm * 1609.344;
                if (!r.hasVis && !r.cavok) { r.hasVis = true; r.visM = m; add("visibility filled"); }
                else if (!r.cavok && r.hasVis && std::fabs(r.visM - m) > 0.1 * m + 200.0 && !(r.visM >= 9999.0 && r.visM <= 10000.5 && sm >= 6.0)) {
                    r.visM = m; add("visibility corrected");
                }
            }
        }
    }
    const Json* ws = o.get("wspd");
    if (ws && ws->type == Json::Num) {
        const Json* wd = o.get("wdir");
        const Json* wg = o.get("wgst");
        double kt = ws->num;
        bool vrb = wd && wd->type == Json::Str && wd->str == "VRB";
        int dir = (wd && wd->type == Json::Num) ? (int)std::lround(wd->num) : 0;
        double gust = (wg && wg->type == Json::Num) ? wg->num : 0.0;
        if (!r.hasWind) {
            r.hasWind = true; r.windKt = kt; r.windDir = dir; r.windVariable = vrb; r.gustKt = gust; add("wind filled");
        } else {
            double dd = std::fabs(std::fmod(std::fabs((double)(r.windDir - dir)) + 180.0, 360.0) - 180.0);
            if (std::fabs(r.windKt - kt) >= 1.0 || std::fabs(r.gustKt - gust) >= 1.0 || (!vrb && !r.windVariable && kt >= 3 && dd >= 10.0)) {
                r.windKt = kt; r.gustKt = gust; if (!vrb) { r.windDir = dir; r.windVariable = false; } else r.windVariable = true;
                add("wind corrected");
            }
        }
    }
    if (const Json* v = o.get("temp")) if (v->type == Json::Num) {
        if (!r.hasTemp) { r.hasTemp = true; r.tempC = v->num; add("temperature filled"); }
        else if (std::fabs(r.tempC - v->num) > 0.9) { r.tempC = v->num; add("temperature corrected"); }
    }
    if (const Json* v = o.get("dewp")) if (v->type == Json::Num) {
        if (!r.hasDewp) { r.hasDewp = true; r.dewpC = v->num; add("dew point filled"); }
        else if (std::fabs(r.dewpC - v->num) > 0.9) { r.dewpC = v->num; add("dew point corrected"); }
    }
    if (const Json* v = o.get("altim")) if (v->type == Json::Num && v->num > 850 && v->num < 1100) {
        if (!r.hasAltim) { r.hasAltim = true; r.altimHpa = v->num; add("pressure filled"); }
        else if (std::fabs(r.altimHpa - v->num) > 1.0) { r.altimHpa = v->num; add("pressure corrected"); }
    }
    if (r.clouds.empty() && !r.clearSky && !r.cavok) {
        if (const Json* cl = o.get("clouds")) if (cl->type == Json::Arr) {
            bool any = false;
            for (const Json& c : cl->arr) {
                const Json* cv = c.get("cover");
                const Json* bs = c.get("base");
                if (!cv) continue;
                std::string cover = cv->asString();
                CloudGroup g;
                if (cover == "FEW") g.cover = CloudGroup::FEW;
                else if (cover == "SCT") g.cover = CloudGroup::SCT;
                else if (cover == "BKN") g.cover = CloudGroup::BKN;
                else if (cover == "OVC") g.cover = CloudGroup::OVC;
                else if (cover == "OVX") {
                    const Json* vv = o.get("vertVis");
                    g.cover = CloudGroup::VV; g.baseFtAgl = vv ? (int)vv->asNum(0) : 0; r.clouds.push_back(g); any = true; continue;
                }
                else if (cover == "CLR" || cover == "CAVOK") { r.clearSky = true; any = true; continue; }
                else continue;
                g.baseFtAgl = bs ? (int)bs->asNum(0) : 0;
                r.clouds.push_back(g); any = true;
            }
            if (any) add("clouds filled");
        }
    }
    std::string out;
    for (const auto& n : notes) out += (out.empty() ? "NOAA decoded data: " : ", ") + n;
    return out;
}

std::vector<Station> parseAwcMetarJson(const std::string& body, std::string* error) {
    std::vector<Station> out;
    Json root;
    if (body.empty()) return out; // HTTP 204: no reports in the area
    if (!parseJson(body, root) || root.type != Json::Arr) {
        if (error) *error = "unexpected response from aviationweather.gov";
        return out;
    }
    for (const Json& o : root.arr) {
        if (o.type != Json::Obj) continue;
        const Json* raw = o.get("rawOb");
        const Json* lat = o.get("lat");
        const Json* lon = o.get("lon");
        if (!raw || !lat || !lon) continue;
        Station s;
        s.rawMetar = raw->asString();
        if (!parseReport(s.rawMetar, s.report, false)) continue;
        s.filled = crossCheckAwc(o, s.report);
        const Json* id = o.get("icaoId");
        if (id) s.icao = id->asString();
        if (s.icao.empty()) s.icao = s.report.icao;
        if (s.icao.empty()) continue;
        s.lat = lat->asNum();
        s.lon = lon->asNum();
        const Json* el = o.get("elev");
        s.elevM = el ? el->asNum(0) : 0; // AWC reports station elevation in metres
        out.push_back(std::move(s));
    }
    return out;
}

std::string checkStationElevations(std::vector<Station>& st) {
    std::string out;
    std::vector<double> orig;
    for (const auto& s : st) orig.push_back(s.elevM);
    auto inRange = [](double m) { return std::isfinite(m) && m >= -450.0 && m <= 5600.0; };
    for (size_t i = 0; i < st.size(); ++i) {
        std::vector<double> nb;
        for (size_t j = 0; j < st.size(); ++j)
            if (j != i && inRange(orig[j]) && distNm(st[i].lat, st[i].lon, st[j].lat, st[j].lon) < 50.0) nb.push_back(orig[j]);
        std::sort(nb.begin(), nb.end());
        const bool haveNb = nb.size() >= 2;
        const double med = haveNb ? nb[nb.size() / 2] : 0.0;
        const double e = orig[i], asFt = e * 0.3048;
        double fixed = e;
        const char* why = nullptr;
        if (!inRange(e)) {
            if (inRange(asFt) && (!haveNb || std::fabs(asFt - med) < 1200.0)) { fixed = asFt; why = "outside -450..5600 m, read as feet"; }
            else { fixed = med; why = haveNb ? "outside -450..5600 m, neighbours' median used" : "outside -450..5600 m, sea level used"; }
        } else if (haveNb && std::fabs(e - med) > 1500.0 && std::fabs(asFt - med) < 600.0) {
            fixed = asFt; why = "far from every neighbour, fits them in feet";
        }
        if (why) {
            char b[200];
            std::snprintf(b, sizeof b, "%s%s elevation %.0f m -> %.0f m (%s)", out.empty() ? "" : "; ", st[i].icao.c_str(), e, fixed, why);
            out += b;
            st[i].elevM = fixed;
        }
    }
    return out;
}

bool parseDatisJson(const std::string& body, std::string& text) {
    text.clear();
    Json root;
    if (!parseJson(body, root) || root.type != Json::Arr) return false; // errors come back as an object
    // Prefer "combined" or "arr" (arrival) text; it carries the weather block.
    const Json* best = nullptr;
    for (const Json& o : root.arr) {
        const Json* d = o.get("datis");
        if (!d) continue;
        const Json* ty = o.get("type");
        std::string type = ty ? ty->asString() : "";
        if (!best || type == "combined" || (type == "arr" && best->get("type") && best->get("type")->asString() == "dep")) best = &o;
    }
    if (!best) return false;
    text = best->get("datis")->asString();
    return !text.empty();
}

} // namespace wx
