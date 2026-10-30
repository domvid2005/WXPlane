// Converts parsed reports into X-Plane-ready values (SI units, MSL altitudes). No X-Plane dependencies.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "config.h"
#include "metar.h"
#include "windsaloft.h"

namespace wx {

struct Station {
    std::string icao;
    double lat = 0, lon = 0, elevM = 0;
    double distNm = 0;       // from the aircraft at fetch time
    Report report;           // METAR, possibly overridden by ATIS values
    std::string rawMetar;
    bool hasAtis = false;
    std::string atisLetter;
    std::string atisText;
};

struct XpCloud {
    float type = 0;      // 0 cirrus, 1 stratus, 2 cumulus, 3 cumulonimbus (float gradient)
    float coverage = 0;  // 0..1
    float baseM = 0;     // MSL
    float topM = 0;      // MSL
};

struct XpWind {
    float altMsl = 0, dir = 0, ms = 0, gustMs = 0, shearDeg = 0, turbulence = 0;
};

struct XpWx {
    bool hasTemp = false, hasDewp = false;
    float tempC = 0, dewpC = 0;
    float qnhPa = 0; // 0 = unknown
    bool hasWind = false;
    float windAltMsl = 0, windDir = 0, windMs = 0, gustMs = 0, shearDeg = 0, turbulence = 0;
    float visM = 40000;
    float precip = 0;
    float waveHeightM = 0;
    int waveDir = 0;
    XpCloud clouds[3];
    int cloudCount = 0;
    bool fogLayerApplied = false;
    float ageS = 0;
    // Winds / temperatures aloft (NOAA FD winds blended at this station)
    bool aloft = false;
    std::vector<XpWind> winds;                         // surface layer first, then aloft levels (ascending)
    std::vector<std::pair<float, float>> tempProfile; // (altitude MSL m, degC), ascending
    float maxAltFt = 10000;
};

// Temperature from a profile at an altitude (linear between points, constant outside).
float profileTemp(const std::vector<std::pair<float, float>>& p, float altM);

// fogSlot: reserve cloud slot 0 for ground fog at EVERY station (empty where there is none). X-Plane blends cloud
// layers between neighbouring reports by slot number, so fog must always sit in the same slot or it gets mixed
// with unrelated clouds as you fly past (this shows up as flicker).
XpWx buildXp(const Station& st, const Config& cfg, std::time_t now, const std::vector<AloftStation>* aloft = nullptr,
             bool fogSlot = false);

// True if this station's report calls for a fog layer under the given config.
bool stationHasFog(const Station& st, const Config& cfg);

// One-line human-readable summary of what is applied for a station (used by the METAR/ATIS window).
std::string describeXp(const XpWx& x, double elevM);

// ATIS values override the METAR where the ATIS actually provides them.
void mergeAtis(Station& st, const Report& atis, const std::string& atisText);

} // namespace wx
