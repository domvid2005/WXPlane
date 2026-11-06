// Pilot reports, SIGMETs and AIRMETs from the NOAA Aviation Weather Center data API (no X-Plane or network calls).
//  - PIREPs/AIREPs:        /api/data/pirep?format=json&age=H&bbox=lat0,lon0,lat1,lon1
//  - US SIGMETs:           /api/data/airsigmet?format=json          (convective and non-convective)
//  - International SIGMETs /api/data/isigmet?format=json             (worldwide, incl. TURB and MTW)
//  - US G-AIRMETs:         /api/data/gairmet?format=json             (Sierra, Tango, Zulu; several forecast hours)
// They are drawn on X-Plane's map (two layers with their own toggles) and turn into a turbulence floor where the
// gridded turbulence data does not already include them (outside NOAA GTGN coverage).
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace wx {
namespace hz {

struct Pirep {
    double lat = 0, lon = 0;
    double altFt = -1;                 // flight level x 100, -1 unknown
    double baseFt = -1, topFt = -1;    // turbulence layer from the report, -1 when not given
    std::string intensity;             // NEG, SMTH-LGT, LGT, LGT-MOD, MOD, MOD-SEV, SEV, EXTRM ("" = no turbulence report)
    std::string tbType, acType, raw, kind;   // CHOP/CAT/..., aircraft, raw text, PIREP/AIREP
    std::time_t obsTime = 0;
    bool urgent = false;
};

struct Area {
    std::string product;               // "SIGMET", "CONVECTIVE SIGMET", "INTL SIGMET", "G-AIRMET"
    std::string hazard;                // TURB, MTW, TS, CONVECTIVE, ICE, IFR, MT_OBSC, LLWS, SFC_WND, FZLVL, VA, TC, ...
    std::string severity;              // MOD, SEV, ... ("" when not given)
    std::string id, raw;
    double baseFt = -1, topFt = -1;    // -1 = surface / unknown top
    std::time_t validFrom = 0, validTo = 0;
    bool line = false;                 // a line (e.g. freezing level) rather than an area
    std::vector<std::pair<double, double>> pts;   // (lat, lon)
    bool contains(double lat, double lon) const;
    bool isTurbulence() const;
};

struct Snapshot {
    std::vector<Pirep> pireps;
    std::vector<Area> areas;
    std::time_t fetched = 0;
};

std::vector<Pirep> parsePireps(const std::string& json, std::string* err = nullptr);
// `product` labels US airsigmet entries (SIGMET / CONVECTIVE SIGMET / AIRMET from airSigmetType and hazard).
std::vector<Area> parseAirSigmets(const std::string& json, std::string* err = nullptr);
std::vector<Area> parseIntlSigmets(const std::string& json, std::string* err = nullptr);
// G-AIRMETs come as snapshots for several forecast hours: only the snapshot valid nearest `now` is kept.
std::vector<Area> parseGairmets(const std::string& json, std::time_t now, std::string* err = nullptr);

// A reported intensity is relative to the aircraft that reported it: "moderate" in a C172 is far less air than "moderate"
// in an A320. Reports are converted to the EDR of the air using the reporter's size class:
//  - light (piston singles and twins, small turboprops):   x0.6
//  - regional (regional jets, airliner turboprops, bizjets): x0.85
//  - medium (A320 / B737 class):                             x1.0
//  - heavy (widebodies):                                     x1.25
//  - type not determinable (blank, UNKN, callsign, unlisted): x0.6, the lightest class
// The EDR of the air then reaches X-Plane, whose flight model makes the user's own aircraft respond to it.
struct ReporterClass { const char* name; double factor; };
ReporterClass reporterClass(const std::string& acType);
// EDR of the air that a reported intensity stands for (category centre for a medium aircraft x the class factor; NEG, "" = 0).
double edrForIntensity(const std::string& intensity, const std::string& acType = "");

struct Floor {
    double edr = 0;
    std::string why;                   // e.g. "PIREP MOD FL290 A321 12 nm, 20 min ago"
};
// Turbulence floor at a point from reports and turbulence SIGMETs/AIRMETs (strongest one wins).
//  - PIREPs (converted for the reporter's size, see reporterClass): full within 10 nm and 30 min, fading to nothing at 50 nm and 90 min; in the reported layer (or +-3,000 ft of the
//    flight level) with a 2,000 ft taper.
//  - Turbulence SIGMETs (TURB, MTW; usually SEV): 0.30, inside the area and its altitudes, while valid.
//  - G-AIRMET Tango (TURB-LO/HI, MOD): 0.18.
Floor floorAt(const Snapshot& s, double lat, double lon, double altFt, std::time_t now);

} // namespace hz
} // namespace wx
