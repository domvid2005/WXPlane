// Pilot reports from the NOAA Aviation Weather Center data API (no X-Plane or network calls):
//   /api/data/pirep?format=json&age=H&bbox=lat0,lon0,lat1,lon1
// They are drawn on X-Plane's map (a layer with its own toggle), become a turbulence floor where the gridded turbulence data does
// not already include them (outside NOAA GTGN coverage), and LLWS reports raise the windshear odds.
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
    bool llws = false;                 // the report mentions low-level wind shear (LLWS, or a /WS group)
};

struct Snapshot {
    std::vector<Pirep> pireps;
    std::time_t fetched = 0;
};

std::vector<Pirep> parsePireps(const std::string& json, std::string* err = nullptr);

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
// Turbulence floor at a point from pilot reports (strongest one wins).
//  - PIREPs (converted for the reporter's size, see reporterClass): full within 10 nm and 30 min, fading to nothing at 50 nm and 90 min; in the reported layer (or +-3,000 ft of the
//    flight level) with a 2,000 ft taper.
Floor floorAt(const Snapshot& s, double lat, double lon, double altFt, std::time_t now);

// A low-level wind shear pilot report near a point: within 15 nm and 90 minutes, reported no higher than 4,000 ft above the
// local ground (LLWS is a below-2,000-ft-AGL phenomenon; a report without a usable altitude counts as low). Windshear sources
// there get +0.025 on their hourly odds of a gust front and of a microburst (owner's rule, 2026-11-06); strength is unchanged.
const Pirep* llwsNear(const Snapshot& s, double lat, double lon, double groundFt, std::time_t now);

} // namespace hz
} // namespace wx
