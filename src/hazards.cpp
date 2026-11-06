#include "hazards.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "geo.h"
#include "json_mini.h"

namespace wx {
namespace hz {
namespace {

double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

std::string str(const Json& o, const char* k) { const Json* v = o.get(k); return v ? v->asString() : std::string(); }
double num(const Json& o, const char* k, double d = -1) {
    const Json* v = o.get(k);
    if (!v || v->type == Json::Null) return d;
    if (v->type == Json::Str && v->str.empty()) return d;
    return v->asNum(d);
}

std::time_t fromCivil(int y, int mo, int d, int h, int mi, int s) {
    y -= mo <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (std::time_t)((era * 146097L + (long)doe - 719468L) * 86400L + h * 3600L + mi * 60L + s);
}
std::time_t isoTime(const std::string& t) {   // 2026-11-07T00:00:00.000Z
    int y, mo, d, h, mi, s;
    if (std::sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    return fromCivil(y, mo, d, h, mi, s);
}
// A time that may be an epoch number or an ISO string.
std::time_t timeOf(const Json& o, const char* k) {
    const Json* v = o.get(k);
    if (!v) return 0;
    if (v->type == Json::Num) return (std::time_t)v->num;
    if (v->type == Json::Str) { std::time_t t = isoTime(v->str); if (t) return t; return (std::time_t)v->asNum(0); }
    return 0;
}
std::vector<std::pair<double, double>> coordsOf(const Json& o) {
    std::vector<std::pair<double, double>> pts;
    const Json* c = o.get("coords");
    if (!c || c->type != Json::Arr) return pts;
    for (const auto& p : c->arr) {
        double la = num(p, "lat", 999), lo = num(p, "lon", 999);
        if (la >= -90 && la <= 90 && lo >= -360 && lo <= 360) pts.emplace_back(la, lo);
    }
    return pts;
}
bool parseArray(const std::string& json, Json& root, std::string* err, const char* what) {
    if (json.empty()) { root = Json(); root.type = Json::Arr; return true; }   // HTTP 204: nothing
    if (!parseJson(json, root) || root.type != Json::Arr) { if (err) *err = std::string("unreadable ") + what + " answer"; return false; }
    return true;
}
// Altitude in ft from G-AIRMET strings ("SFC", "130" = FL130/13,000 ft, "FZL" = freezing level: unknown).
double hundreds(const std::string& s) {
    if (s.empty() || s == "SFC") return -1;
    char* e = nullptr;
    double v = std::strtod(s.c_str(), &e);
    return e == s.c_str() ? -1 : v * 100.0;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

} // namespace

ReporterClass reporterClass(const std::string& t) {
    static const char* medium[] = {"A318", "A319", "A320", "A321", "A19N", "A20N", "A21N", "A22", "BCS1", "BCS3", "B73", "B37M", "B38M", "B39M",
                                   "B3XM", "B72", "B75", "B712", "MD8", "MD9", "C919", "T204", "SU95", "A148", "E290", "E295"};
    static const char* heavy[] = {"A30", "A310", "A33", "A34", "A35", "A38", "B74", "B76", "B77", "B78", "MD11", "DC10", "C5M", "K35",
                                  "KC10", "IL76", "A124", "A400", "B52", "A3ST", "BLCF", "IL96", "A225"};
    static const char* regional[] = {"CRJ", "E13", "E14", "E17", "E19", "E29", "E75", "E45", "DH8", "AT4", "AT7", "SF34", "JS3", "JS4", "B190",
                                     "C25", "C50", "C51", "C52", "C55", "C56", "C68", "C70", "C75", "CL30", "CL35", "CL60", "GLF", "GLEX",
                                     "GL5T", "GL7T", "FA", "LJ", "H25", "E55P", "E50P", "PRM1", "HDJT", "BE40", "SW4", "F2TH", "F900", "D328",
                                     "SB20", "AN26", "C130", "C30J", "PC24", "F70", "F100", "RJ", "B46"};
    static const char* light[] = {"C1", "C20", "C21", "C3", "C40", "C41", "C42", "C44", "PA", "P28", "P32", "P46", "SR2", "S22", "DA2", "DA4",
                                  "DA6", "BE3", "BE5", "BE6", "BE7", "BE9", "BE10", "BE20", "B350", "M20", "PC6", "PC7", "PC12", "PC21",
                                  "TBM", "C208", "KODI", "DHC2", "DHC3", "DHC6", "RV", "AA5", "GLAS", "M7", "CH7", "TB2", "TRIN", "COL",
                                  "LNC", "EV97", "AT3", "P210", "G58", "G36", "C77R", "EPIC"};
    // The report should give an ICAO type designator. When it does not (blank, UNKN, an AIREP callsign such as UAL1463, or a type
    // not in these lists), the size cannot be determined and the lightest class is assumed (owner's rule, 2026-11-06): the
    // report then stands for the least air, so it can never overstate the turbulence.
    std::string u;
    for (char c : t) if (c != ' ') u += (char)std::toupper((unsigned char)c);
    if (u.empty() || u.size() > 4) return {"unknown type, taken as light", 0.6};
    for (const char* p : medium) if (startsWith(u, p)) return {"medium", 1.0};
    if (u == "C17" || u == "C5") return {"heavy", 1.25};   // exact: C172 etc. are light
    for (const char* p : heavy) if (startsWith(u, p)) return {"heavy", 1.25};
    for (const char* p : regional) if (startsWith(u, p)) return {"regional", 0.85};
    for (const char* p : light) if (startsWith(u, p)) return {"light", 0.6};
    return {"unknown type, taken as light", 0.6};
}

double edrForIntensity(const std::string& i, const std::string& acType) {
    double e = 0;
    if (i == "SMTH-LGT" || i == "SMTH") e = i == "SMTH" ? 0 : 0.08;
    else if (i == "LGT") e = 0.15;
    else if (i == "LGT-MOD") e = 0.22;
    else if (i == "MOD") e = 0.30;
    else if (i == "MOD-SEV") e = 0.40;
    else if (i == "SEV") e = 0.50;
    else if (i == "SEV-EXTM" || i == "SEV-EXTRM") e = 0.55;
    else if (i == "EXTRM" || i == "EXTM") e = 0.60;
    return e * reporterClass(acType).factor;
}

std::vector<Pirep> parsePireps(const std::string& json, std::string* err) {
    std::vector<Pirep> out;
    Json root;
    if (!parseArray(json, root, err, "PIREP")) return out;
    for (const auto& o : root.arr) {
        Pirep p;
        p.lat = num(o, "lat", 999); p.lon = num(o, "lon", 999);
        if (std::fabs(p.lat) > 90 || std::fabs(p.lon) > 360) continue;
        double fl = num(o, "fltLvl", -1);
        p.altFt = fl >= 0 ? fl * 100.0 : -1;
        // The stronger of the two turbulence groups, with its layer.
        std::string i1 = str(o, "tbInt1"), i2 = str(o, "tbInt2");
        const bool second = edrForIntensity(i2) > edrForIntensity(i1);
        p.intensity = second ? i2 : i1;
        const char* b = second ? "tbBas2" : "tbBas1"; const char* tp = second ? "tbTop2" : "tbTop1";
        double bs = num(o, b, -1), tt = num(o, tp, -1);
        p.baseFt = bs >= 0 ? bs * 100.0 : -1; p.topFt = tt >= 0 ? tt * 100.0 : -1;
        p.tbType = str(o, second ? "tbType2" : "tbType1");
        p.acType = str(o, "acType");
        p.raw = str(o, "rawOb");
        p.kind = str(o, "pirepType");
        p.obsTime = timeOf(o, "obsTime");
        p.urgent = p.raw.find(" UUA ") != std::string::npos || p.raw.compare(0, 4, "UUA ") == 0;
        out.push_back(p);
    }
    return out;
}

std::vector<Area> parseAirSigmets(const std::string& json, std::string* err) {
    std::vector<Area> out;
    Json root;
    if (!parseArray(json, root, err, "AIR/SIGMET")) return out;
    for (const auto& o : root.arr) {
        Area a;
        std::string type = str(o, "airSigmetType");
        a.hazard = str(o, "hazard");
        a.product = (type == "SIGMET" && a.hazard == "CONVECTIVE") ? "CONVECTIVE SIGMET" : type.empty() ? "SIGMET" : type;
        double sev = num(o, "severity", -1);
        if (a.hazard == "TURB" || a.hazard == "ICE") a.severity = type == "SIGMET" ? "SEV" : "MOD";
        else if (sev >= 0) a.severity = "";
        a.baseFt = num(o, "altitudeLow1", -1);
        a.topFt = num(o, "altitudeHi1", -1);
        a.validFrom = timeOf(o, "validTimeFrom"); a.validTo = timeOf(o, "validTimeTo");
        a.id = str(o, "seriesId");
        a.raw = str(o, "rawAirSigmet");
        a.pts = coordsOf(o);
        if (a.pts.size() >= 2) out.push_back(a);
    }
    return out;
}

std::vector<Area> parseIntlSigmets(const std::string& json, std::string* err) {
    std::vector<Area> out;
    Json root;
    if (!parseArray(json, root, err, "international SIGMET")) return out;
    for (const auto& o : root.arr) {
        Area a;
        a.product = "INTL SIGMET";
        a.hazard = str(o, "hazard");
        a.severity = str(o, "qualifier");
        a.baseFt = num(o, "base", -1);
        a.topFt = num(o, "top", -1);
        a.validFrom = timeOf(o, "validTimeFrom"); a.validTo = timeOf(o, "validTimeTo");
        a.id = str(o, "firId") + " " + str(o, "seriesId");
        a.raw = str(o, "rawSigmet");
        a.line = str(o, "geom") == "LINE";
        a.pts = coordsOf(o);
        if (a.pts.size() >= 2 || (a.pts.size() == 1 && !a.line)) out.push_back(a);
    }
    return out;
}

std::vector<Area> parseGairmets(const std::string& json, std::time_t now, std::string* err) {
    std::vector<Area> out;
    Json root;
    if (!parseArray(json, root, err, "G-AIRMET")) return out;
    std::time_t best = 0;
    for (const auto& o : root.arr) {
        std::time_t vt = timeOf(o, "validTime");
        if (vt && (!best || std::labs((long)(vt - now)) < std::labs((long)(best - now)))) best = vt;
    }
    for (const auto& o : root.arr) {
        if (timeOf(o, "validTime") != best) continue;
        Area a;
        a.product = "G-AIRMET " + str(o, "product");
        a.hazard = str(o, "hazard");
        a.severity = str(o, "severity");
        a.baseFt = hundreds(str(o, "base"));
        a.topFt = hundreds(str(o, "top"));
        a.validFrom = best - 90 * 60;            // a snapshot stands for +-90 minutes around its time
        a.validTo = std::max<std::time_t>(best + 90 * 60, 0);
        std::time_t exp = timeOf(o, "expireTime");
        if (exp && exp < a.validTo) a.validTo = exp;
        a.id = str(o, "tag");
        std::string dueTo = str(o, "due_to");
        a.raw = a.product + " " + a.hazard + (a.severity.empty() ? "" : " " + a.severity) + (dueTo.empty() ? "" : " - " + dueTo);
        a.line = str(o, "geometryType") == "LINE" || a.hazard == "FZLVL" || a.hazard == "M_FZLVL";
        a.pts = coordsOf(o);
        if (a.pts.size() >= 2) out.push_back(a);
    }
    return out;
}

bool Area::contains(double lat, double lon) const {
    if (line || pts.size() < 3) return false;
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        auto rel = [&](double lo) { double d = lo - lon; while (d > 180) d -= 360; while (d < -180) d += 360; return d; };
        double yi = pts[i].first, xi = rel(pts[i].second), yj = pts[j].first, xj = rel(pts[j].second);
        if ((yi > lat) != (yj > lat)) {
            double x = xj + (lat - yj) * (xi - xj) / (yi - yj);
            if (x > 0) in = !in;
        }
    }
    return in;
}

bool Area::isTurbulence() const {
    return hazard == "TURB" || hazard == "MTW" || hazard == "TURB-LO" || hazard == "TURB-HI";
}

Floor floorAt(const Snapshot& s, double lat, double lon, double altFt, std::time_t now) {
    Floor f;
    char b[220];
    for (const auto& p : s.pireps) {
        double e = edrForIntensity(p.intensity, p.acType);
        if (e <= 0) continue;
        double age = (double)(now - p.obsTime);
        if (age < 0) age = 0;
        if (age > 5400) continue;
        double lo, hi;
        if (p.baseFt >= 0 || p.topFt >= 0) { lo = p.baseFt >= 0 ? p.baseFt : p.topFt; hi = p.topFt >= 0 ? p.topFt : p.baseFt; if (hi < lo) std::swap(lo, hi); }
        else if (p.altFt > 0) { lo = p.altFt - 3000; hi = p.altFt + 3000; }
        else continue;
        double dv = altFt < lo ? lo - altFt : altFt > hi ? altFt - hi : 0;
        double vw = clamp01(1.0 - dv / 2000.0);
        if (vw <= 0) continue;
        double d = distNm(lat, lon, p.lat, p.lon);
        double hw = clamp01(1.0 - (d - 10.0) / 40.0);
        double tw = age <= 1800 ? 1.0 : clamp01(1.0 - (age - 1800) / 3600.0);
        double v = e * hw * tw * vw;
        if (v > f.edr) {
            f.edr = v;
            ReporterClass rc = reporterClass(p.acType);
            std::snprintf(b, sizeof b, "%s %s %s (%s, x%.2f) FL%03.0f, %.0f nm, %.0f min ago", p.kind.empty() ? "PIREP" : p.kind.c_str(), p.intensity.c_str(),
                          p.acType.empty() ? "?" : p.acType.c_str(), rc.name, rc.factor, p.altFt / 100.0, d, age / 60.0);
            f.why = b;
        }
    }
    for (const auto& a : s.areas) {
        if (!a.isTurbulence()) continue;
        if ((a.validFrom && now < a.validFrom) || (a.validTo && now > a.validTo)) continue;
        double lo = a.baseFt >= 0 ? a.baseFt : 0, hi = a.topFt >= 0 ? a.topFt : 60000;
        if (altFt < lo - 500 || altFt > hi + 500) continue;
        if (!a.contains(lat, lon)) continue;
        double v = a.product.find("SIGMET") != std::string::npos ? 0.30 : (a.severity == "SEV" ? 0.30 : 0.18);
        if (v > f.edr) {
            f.edr = v;
            std::snprintf(b, sizeof b, "%s %s %s %s, %s-%s", a.product.c_str(), a.id.c_str(), a.severity.c_str(), a.hazard.c_str(),
                          a.baseFt >= 0 ? std::to_string((int)(a.baseFt / 100)).c_str() : "SFC", a.topFt >= 0 ? std::to_string((int)(a.topFt / 100)).c_str() : "?");
            f.why = b;
        }
    }
    return f;
}

} // namespace hz
} // namespace wx
