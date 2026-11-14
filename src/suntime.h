// Sunrise and sunset (NOAA's solar position equations, accurate to about a minute) and time zone names for the weather window.
#pragma once
#include <cmath>
#include <ctime>
#include <string>

namespace wx {

// Sunrise and sunset (UTC) on the UTC day containing `day`, for the point lat/lon (degrees, east positive): the sun's centre
// 0.833 degrees below the horizon (refraction and its radius). Returns 1 normal, 0 the sun stays up all day, -1 it stays down.
inline int sunTimes(double lat, double lon, std::time_t day, std::time_t& rise, std::time_t& set) {
    const double kD = 3.14159265358979323846 / 180.0;
    const std::time_t midnight = day - (day % 86400 + 86400) % 86400;
    const double jd = midnight / 86400.0 + 2440587.5 + 0.5;   // Julian day at noon UTC of that day
    const double t = (jd - 2451545.0) / 36525.0;
    const double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    const double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double c = std::sin(m * kD) * (1.914602 - t * (0.004817 + 0.000014 * t)) + std::sin(2 * m * kD) * (0.019993 - 0.000101 * t) + std::sin(3 * m * kD) * 0.000289;
    const double om = 125.04 - 1934.136 * t;
    const double lambda = l0 + c - 0.00569 - 0.00478 * std::sin(om * kD);
    const double eps = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0 + 0.00256 * std::cos(om * kD);
    const double decl = std::asin(std::sin(eps * kD) * std::sin(lambda * kD));
    const double y = std::tan(eps * kD / 2) * std::tan(eps * kD / 2);
    const double eqt = 4.0 / kD * (y * std::sin(2 * l0 * kD) - 2 * e * std::sin(m * kD) + 4 * e * y * std::sin(m * kD) * std::cos(2 * l0 * kD) -
                                   0.5 * y * y * std::sin(4 * l0 * kD) - 1.25 * e * e * std::sin(2 * m * kD));   // minutes
    const double cosH = (std::cos(90.833 * kD) - std::sin(lat * kD) * std::sin(decl)) / (std::cos(lat * kD) * std::cos(decl));
    if (cosH < -1) return 0;
    if (cosH > 1) return -1;
    const double h = std::acos(cosH) / kD;            // degrees
    const double noon = 720.0 - 4.0 * lon - eqt;      // minutes after midnight UTC
    rise = midnight + (std::time_t)std::lround((noon - 4.0 * h) * 60.0);
    set = midnight + (std::time_t)std::lround((noon + 4.0 * h) * 60.0);
    return 1;
}

// Is daylight saving time on at this moment (UTC): the US and Canada (2nd Sunday of March 2:00 local to 1st Sunday of November),
// Europe (last Sunday of March to last Sunday of October, 1:00 UTC)
inline int dayOfWeek(int y, int mo, int d) {   // 0 = Sunday
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (mo < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[mo - 1] + d) % 7;
}
inline std::time_t utcOf(int y, int mo, int d, int h) {
    // days from 1970-01-01 (civil from days, Howard Hinnant)
    const int yy = y - (mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153 * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return ((std::time_t)era * 146097 + (std::time_t)doe - 719468) * 86400 + h * 3600;
}
inline int yearOf(std::time_t t) { std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return tm.tm_year + 1900; }
inline bool usDst(std::time_t utc, double stdOffsetH) {
    const int y = yearOf(utc);
    int d1 = 8; while (dayOfWeek(y, 3, d1) != 0) ++d1;     // 2nd Sunday of March
    int d2 = 1; while (dayOfWeek(y, 11, d2) != 0) ++d2;    // 1st Sunday of November
    const std::time_t on = utcOf(y, 3, d1, 2) - (std::time_t)(stdOffsetH * 3600), off = utcOf(y, 11, d2, 2) - (std::time_t)((stdOffsetH + 1) * 3600);
    return utc >= on && utc < off;
}
inline bool euDst(std::time_t utc) {
    const int y = yearOf(utc);
    int d1 = 31; while (dayOfWeek(y, 3, d1) != 0) --d1;
    int d2 = 31; while (dayOfWeek(y, 10, d2) != 0) --d2;
    return utc >= utcOf(y, 3, d1, 1) && utc < utcOf(y, 10, d2, 1);
}

// Southern hemisphere: south-east Australia (1st Sunday of October 2:00 standard to 1st Sunday of April 3:00 daylight time) and
// New Zealand (last Sunday of September to 1st Sunday of April)
inline bool auDst(std::time_t utc, double stdOffsetH, bool nz) {
    const int y = yearOf(utc);
    int d1 = 1; while (dayOfWeek(y, 10, d1) != 0) ++d1;
    if (nz) { d1 = 30; while (dayOfWeek(y, 9, d1) != 0) --d1; }
    int d2 = 1; while (dayOfWeek(y, 4, d2) != 0) ++d2;
    const std::time_t on = utcOf(y, nz ? 9 : 10, d1, 2) - (std::time_t)(stdOffsetH * 3600), off = utcOf(y, 4, d2, 3) - (std::time_t)((stdOffsetH + 1) * 3600);
    return utc >= on || utc < off;
}

// Places that keep standard time all year inside a zone whose neighbours change (2026-11-14: Arizona and Saskatchewan were
// rectangles; the strip of New Mexico up to 109.0 W lost its daylight time and the Navajo Nation, which keeps it, did not get it).
// Outlines (lon, lat), simplified from the state, provincial and reservation boundaries; used for the local time and drawn on the
// map's Time Zone layer while daylight time is on around them.
struct NoDstPt { double lon, lat; };
inline bool inNoDstPoly(const NoDstPt* p, size_t n, double lat, double lon) {
    bool in = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        if ((p[i].lat > lat) != (p[j].lat > lat) && lon < (p[j].lon - p[i].lon) * (lat - p[i].lat) / (p[j].lat - p[i].lat) + p[i].lon) in = !in;
    return in;
}
inline const NoDstPt* arizonaOutline(size_t& n) {   // the state line; the west side follows the Colorado River
    static const NoDstPt p[] = {
        {-114.0506, 37.0004}, {-109.0452, 37.0004}, {-109.0475, 31.3323}, {-111.0747, 31.3322}, {-114.8126, 32.4943},
        {-114.8094, 32.6176}, {-114.7221, 32.7208}, {-114.5269, 32.8091}, {-114.4686, 32.9452}, {-114.5114, 33.0273},
        {-114.7064, 33.1053}, {-114.6795, 33.3592}, {-114.5329, 33.5262}, {-114.5354, 33.9284}, {-114.4105, 34.1023},
        {-114.1405, 34.3031}, {-114.3405, 34.4466}, {-114.5650, 34.7746}, {-114.6342, 35.0018}, {-114.5772, 35.1335},
        {-114.6800, 35.5000}, {-114.6534, 35.6100}, {-114.7380, 36.0101}, {-114.6200, 36.1400}, {-114.4000, 36.1500},
        {-114.2600, 36.0300}, {-114.0466, 36.1940}};
    n = sizeof p / sizeof p[0];
    return p;
}
inline const NoDstPt* navajoOutline(size_t& n) {   // the Navajo Nation's part of Arizona: it keeps daylight time
    static const NoDstPt p[] = {
        {-111.60, 37.0004}, {-109.0452, 37.0004}, {-109.0465, 35.20}, {-109.60, 35.17}, {-110.40, 35.28}, {-111.00, 35.50},
        {-111.40, 35.76}, {-111.72, 36.10}, {-111.62, 36.60}};
    n = sizeof p / sizeof p[0];
    return p;
}
inline const NoDstPt* hopiOutline(size_t& n) {   // the Hopi Reservation inside it: standard time all year, like the rest of Arizona
    static const NoDstPt p[] = {
        {-111.00, 35.75}, {-111.00, 36.00}, {-110.75, 36.15}, {-110.25, 36.20}, {-110.00, 36.00}, {-110.00, 35.55}, {-110.75, 35.55}};
    n = sizeof p / sizeof p[0];
    return p;
}
inline const NoDstPt* saskatchewanOutline(size_t& n) {   // the province (Lloydminster on the Alberta line keeps Alberta's time)
    static const NoDstPt p[] = {
        {-110.005, 49.0}, {-101.367, 49.0}, {-101.50, 52.0}, {-101.60, 53.9}, {-101.89, 55.8}, {-102.00, 60.0}, {-110.005, 60.0}};
    n = sizeof p / sizeof p[0];
    return p;
}
inline const NoDstPt* pageOutline(size_t& n) {   // Page and Lake Powell at the Navajo Nation's north-west corner: not in it (MST all year)
    static const NoDstPt p[] = {{-111.62, 36.84}, {-111.33, 36.84}, {-111.33, 37.0004}, {-111.60, 37.0004}};
    n = sizeof p / sizeof p[0];
    return p;
}
inline bool arizonaStandardAllYear(double lat, double lon) {
    size_t n = 0;
    const NoDstPt* az = arizonaOutline(n);
    if (!inNoDstPoly(az, n, lat, lon)) return false;
    const NoDstPt* nv = navajoOutline(n);
    if (!inNoDstPoly(nv, n, lat, lon)) return true;
    const NoDstPt* pg = pageOutline(n);
    if (inNoDstPoly(pg, n, lat, lon)) return true;   // Page (KPGA): MST all year (2026-11-14 review)
    const NoDstPt* hp = hopiOutline(n);
    return inNoDstPoly(hp, n, lat, lon);
}
inline bool saskatchewanStandardAllYear(double lat, double lon) {
    size_t n = 0;
    const NoDstPt* sk = saskatchewanOutline(n);
    return inNoDstPoly(sk, n, lat, lon);
}

// Daylight saving time added (seconds) to a place's standard UTC offset (in quarter hours) at a moment: the US and Canada,
// Europe, south-east Australia and New Zealand (the places that do not change are left out: Hawaii, Arizona, Saskatchewan,
// Mexico, Iceland, Russia, Turkey, Queensland, the Northern Territory, Western Australia ...); 0 elsewhere.
inline double dstShiftS(double lat, double lon, int stdQ, std::time_t utc) {
    if (lat > 14 && lon < -50 && lon > -180) {   // North America
        if (stdQ < -36 || stdQ > -14) return 0;                                           // Hawaii
        if (!(lat > 31.3 || (lon > -97.2 && lat > 24.3))) return 0;                        // Mexico, Central America, the Caribbean
        if (stdQ == -28 && arizonaStandardAllYear(lat, lon)) return 0;                     // Arizona (not the Navajo Nation)
        if (stdQ == -24 && saskatchewanStandardAllYear(lat, lon)) return 0;                // Saskatchewan
        return usDst(utc, stdQ / 4.0) ? 3600 : 0;
    }
    if (lat > 35.9 && lat < 72 && lon > -32 && lon < 45) {   // Europe
        if (stdQ != 0 && stdQ != 4 && stdQ != 8) return 0;                                 // Turkey, Belarus, Russia
        if (lon < -13 && lat > 62) return 0;                                                // Iceland
        if (stdQ == 8 && lat > 54.3 && lat < 55.4 && lon > 19.6 && lon < 22.9) return 0;   // Kaliningrad
        return euDst(utc) ? 3600 : 0;
    }
    if (lat < -28.2 && lat > -44 && lon > 129 && lon < 154 && (stdQ == 40 || stdQ == 38)) return auDst(utc, stdQ / 4.0, false) ? 3600 : 0;
    if (lat < -33 && lat > -48 && lon > 165 && lon < 179.9 && stdQ == 48) return auDst(utc, 12, true) ? 3600 : 0;
    return 0;
}

// A time zone's usual name for a UTC offset at a place and moment (EST, CDT, GMT, BST, CET ...); "" where none is known (the
// caller shows UTC+x). North America and Europe are named; Hawaii, Alaska, Atlantic, Newfoundland too.
inline std::string zoneName(double lat, double lon, double offS, std::time_t utc) {
    const int q = (int)std::lround(offS / 900.0);   // quarter hours
    const bool na = lat > 14 && lon < -50 && lon > -180;
    const bool eu = lat > 34 && lat < 72 && lon > -25 && lon < 45;
    if (na) {
        if (lat < 23 && lon < -150) return q == -40 ? "HST" : "";   // Hawaii: no DST
        if (q == -28 && arizonaStandardAllYear(lat, lon)) return "MST";   // Arizona: MST all year
        struct Z { int q; const char* s; const char* d; double stdH; };
        static const Z zs[] = {{-36, "AKST", "AKDT", -9}, {-32, "PST", "PDT", -8}, {-28, "MST", "MDT", -7}, {-24, "CST", "CDT", -6},
                               {-20, "EST", "EDT", -5}, {-16, "AST", "ADT", -4}, {-14, "NST", "NDT", -3.5}};
        for (const Z& z : zs) {
            const bool dst = usDst(utc, z.stdH);
            if (!dst && q == z.q) return z.s;
            if (dst && q == z.q + 4) return z.d;
        }
        for (const Z& z : zs) if (q == z.q) return z.s;   // places without DST (Arizona: MST all year, Saskatchewan: CST)
        return "";
    }
    if (eu) {
        const bool dst = euDst(utc);
        const bool iberia = lat < 44 && lon < -6;                                  // Portugal
        const bool ireland = lon < -6 && lat > 51.3 && lat < 55.4 && !(lat > 54 && lon > -8.2);
        if (q == 0) return iberia ? "WET" : "GMT";
        if (q == 4) return dst ? (iberia ? "WEST" : (ireland ? "IST" : (lon < 2 && lat > 49.8 ? "BST" : "CET"))) : "CET";
        if (q == 8) return dst ? "CEST" : "EET";
        if (q == 12) return dst && lon < 40 ? "EEST" : "MSK";
    }
    if (lat < -10 && lat > -45 && lon > 112 && lon < 155) {   // Australia
        if (q == 32) return "AWST";
        if (q == 38) return "ACST";
        if (q == 42) return "ACDT";
        if (q == 40) return "AEST";
        if (q == 44) return "AEDT";
    }
    if (lat < -33 && lat > -48 && lon > 165 && lon < 179.9) return q == 48 ? "NZST" : (q == 52 ? "NZDT" : "");
    return "";
}

} // namespace wx
