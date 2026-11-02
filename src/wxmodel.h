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
    std::string filled;      // what was corrected from NOAA's decoded fields or filled in from neighbouring stations
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

// Sanity checks on a built weather record, fixing what X-Plane would otherwise render as artefacts: coverage outside 0..1, a top at or
// below its base, a layer thinner than 30 m, absurd thickness (over 16 km, more than any real cloud), non-empty layers out of vertical
// order or overlapping (unless allowOverlap: radar storm records deliberately run through the station's own layers), and a record whose
// maximum altitude lies below its highest cloud top. Deep convection and layered frontal decks are untouched. Returns the number of
// adjustments; the first one is described in *note (if given).
// Highest cloud top: 45000 ft. Seen as ch=45000.00 in two separate D/RWX weather-state dumps of the sim's own state.
constexpr float kXpTopM = 13716.0f;
constexpr float kXpMaxTopM = 13716.0f;   // X-Plane's manual weather limits only tops (45,000 ft, every cloud type) and thickness; bases may be anywhere down to 0 ft MSL
// Largest thickness X-Plane's manual weather screen allows per cloud type (read off its Weather Settings with the top at 45000 ft:
// cirrus 43,200-45,000 ft, stratus 40,080-45,000 ft, cumulus 28,320-45,000 ft; cumulonimbus has no thickness limit, base 0 to top 45,000 ft).
// A stratus layer in X-Plane's own state dump is exactly 4920 ft thick too. Cloud type is rounded to the nearest of 0..3.
constexpr float kXpMaxThickM[4] = {1800.0f * 0.3048f, 4920.0f * 0.3048f, 16680.0f * 0.3048f, 1e9f};
inline float xpMaxThicknessM(float cloudType) { int t = (int)(cloudType + 0.5f); return kXpMaxThickM[t < 0 ? 0 : (t > 3 ? 3 : t)]; }
int validateXp(XpWx& x, bool allowOverlap, std::string* note = nullptr);

// True if this station's report calls for a fog layer under the given config.
bool stationHasFog(const Station& st, const Config& cfg);

// One-line human-readable summary of what is applied for a station (used by the METAR/ATIS window).
std::string describeXp(const XpWx& x, double elevM);

// ATIS values override the METAR where the ATIS actually provides them.
void mergeAtis(Station& st, const Report& atis, const std::string& atisText);

// Gaps in a station's report (no wind, temperature, dew point, pressure or visibility) are filled from the nearest stations that
// have the element (inverse-distance weighted, within rangeNm). Returns how many elements were filled; notes go to Station::filled.
int fillMissing(std::vector<Station>& stations, double rangeNm = 80.0);

// Smooth weather at any point: scalar elements (wind, visibility, temperature, dew point, pressure) are an inverse-distance
// (power 3) blend of the nearest stations, so the closest station dominates and borders blend gradually instead of stepping.
// Clouds and weather phenomena come from the nearest station; ground fog stays at its airport.
Station blendAt(const std::vector<Station>& stations, double lat, double lon);

} // namespace wx
