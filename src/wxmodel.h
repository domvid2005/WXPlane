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
    // Recent precipitation (2026-11-06, dry runways), from this station's reports of the last ~70 minutes and its remarks:
    long long obsTime = 0;   // unix time of the report (AWC obsTime)
    double rainMinAgo = -1;  // minutes before this report that precipitation was last seen (0 = now); -1 = none in the history
    double historyMin = 0;   // how many minutes back the history reaches (other reports, begin/end remarks)
    bool frozenRecent = false;   // snow, ice pellets, hail or freezing precipitation in the history (runway left alone)
    std::string rainWhy;     // what the history was read from
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
    // the global model's cloud cover on pressure levels here (altitude m MSL, cover 0..1, ascending): cloud tops (modelCloudTopM)
    std::vector<std::pair<float, float>> modelProfM;
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
    bool modelCirrus = false;   // a cirrus layer was added from the global model's high cloud
    std::vector<std::pair<float, float>> tempProfile; // (altitude MSL m, degC), ascending
    int aloftRejected = 0;                            // forecast values dropped as impossible (NaN, out of range)
    float maxAltFt = 10000;
};

// Temperature from a profile at an altitude (linear between points, constant outside).
float profileTemp(const std::vector<std::pair<float, float>>& p, float altM);
// The tropopause (WMO lapse-rate rule) of a profile; false when the profile is too short or shows none.
bool findTropopause(const std::vector<std::pair<float, float>>& p, float& altM, float& tempC);
float isaTempC(float altM);
// Wind at an altitude from an ascending profile, interpolated as vectors (350 and 010 deg give 000); ends are held.
XpWind windAt(const std::vector<XpWind>& p, float altM);
// Winds/temperatures aloft of a record from the sampled forecast levels; x.winds holds the surface layer (if any) on entry.
void buildAloftProfile(XpWx& x, const std::vector<AloftSample>& smp, double elevM, bool hasSfcT, float sfcT);
// Magnetic to true: TRUE = MAGNETIC + east variation (west variation negative), normalised to 0..359. Calm and variable winds keep their
// direction as reported (0 for calm/VRB).
int magneticToTrue(int dirMag, double varEastDeg, bool calmOrVariable);
// Checks a finished profile before it goes to X-Plane: ascending, at least 100 m apart, finite and physically possible. Returns the
// number of problems; with fix = true, bad entries are dropped.
int validateProfile(XpWx& x, bool fix, std::string* why = nullptr);

// fogSlot: reserve cloud slot 0 for ground fog at EVERY station (empty where there is none). X-Plane blends cloud
// layers between neighbouring reports by slot number, so fog must always sit in the same slot or it gets mixed
// with unrelated clouds as you fly past (this shows up as flicker).
// A report made from the global model's surface weather at a lattice node (open water: no airport within range). id "GFSN28W083".
Station modelStation(const ModelPoint& p, std::time_t now);
// neighbours: the other stations of the update (each cloud layer's coverage leans to the heavy or the light end of its range by the
// sky around it); without them every layer sits in its range by its own station's pseudo-random value only
XpWx buildXp(const Station& st, const Config& cfg, std::time_t now, const std::vector<AloftStation>* aloft = nullptr,
             bool fogSlot = false, const std::vector<Station>* neighbours = nullptr);

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
// Between two types the limit is taken in between as well (2026-11-13 KSMF: an overcast of type 1.5 sent 5,084 m deep, the cumulus
// limit, did not show at all; X-Plane's limit for 1.5 lies between stratus and cumulus). From cumulus up it is unlimited.
inline float xpMaxThicknessM(float cloudType) {
    const float t = cloudType < 0.0f ? 0.0f : (cloudType > 3.0f ? 3.0f : cloudType);
    const int lo = (int)t;
    if (lo >= 2) return t > 2.0f ? kXpMaxThickM[3] : kXpMaxThickM[2];
    const float f = t - (float)lo;
    return kXpMaxThickM[lo] + (kXpMaxThickM[lo + 1] - kXpMaxThickM[lo]) * f;
}
int validateXp(XpWx& x, bool allowOverlap, std::string* note = nullptr);
// The top of the cloud a deck based at baseM belongs to, from the model's cloud cover (modelProfM): the levels from the base up with at
// least 40 % cover, the top halfway to the first level below that. -1 if the model has no cloud at that base (the rules decide then).
float modelCloudTopM(const std::vector<std::pair<float, float>>& prof, float baseM);

// True if this station's report calls for a fog layer under the given config.
bool stationHasFog(const Station& st, const Config& cfg);

// Dry runway (2026-11-06): true when the report is current (<= 90 min), nothing frozen is in its history, and no precipitation was seen
// for at least dryMin minutes (counting the report's age), with a history long enough to know. `why` says which way and why.
bool runwayShouldBeDry(const Station& s, std::time_t now, double dryMin, std::string* why);

// One-line human-readable summary of what is applied for a station (used by the METAR/ATIS window).
std::string describeXp(const XpWx& x, double elevM);

// ATIS values override the METAR where the ATIS actually provides them.
void mergeAtis(Station& st, const Report& atis, const std::string& atisText);

// Gaps in a station's report (no wind, temperature, dew point, pressure or visibility) are filled from the nearest stations that
// have the element (inverse-distance weighted, within rangeNm). Returns how many elements were filled; notes go to Station::filled.
int fillMissing(std::vector<Station>& stations, double rangeNm = 80.0);
// High cloud (base 12,000 ft AGL or more) that one station reports and its automatic neighbours cannot see (their sensors stop at
// 12,000 ft; CLR/NCD/CAVOK say nothing about higher cloud): the nearest such layer within `rangeNm` is copied to those neighbours at
// the same height above sea level, one coverage step thinner beyond 25 nm and two beyond 50 nm. Without this an OVC200 at KBOS was a
// round 6 nm patch of overcast cirrus in a clear sky (2026-11-12). Not over SKC (an observer saw no cloud at any height).
// Returns the number of stations that got a layer.
int spreadHighCloud(std::vector<Station>& stations, double rangeNm = 80.0);

// Smooth weather at any point: scalar elements (wind, visibility, temperature, dew point, pressure) are an inverse-distance
// (power 3) blend of the nearest stations, so the closest station dominates and borders blend gradually instead of stepping.
// Clouds and weather phenomena come from the nearest station; ground fog stays at its airport.
Station blendAt(const std::vector<Station>& stations, double lat, double lon);

} // namespace wx
