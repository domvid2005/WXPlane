#include "sources.h"

#include <algorithm>
#include <cmath>
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
