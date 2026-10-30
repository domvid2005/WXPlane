// Converts parsed reports into X-Plane-ready values (SI units, MSL altitudes). No X-Plane dependencies.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "config.h"
#include "metar.h"

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
};

XpWx buildXp(const Station& st, const Config& cfg, std::time_t now);

// ATIS values override the METAR where the ATIS actually provides them.
void mergeAtis(Station& st, const Report& atis, const std::string& atisText);

} // namespace wx
