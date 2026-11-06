// NOAA winds & temperatures aloft (FD winds, aviationweather.gov /api/data/windtemp). No X-Plane dependencies.
#pragma once
#include <string>
#include <vector>

namespace wx {

struct AloftLevel {
    double altFt = 0; // MSL
    bool hasWind = false;
    double dirDeg = 0, speedKt = 0; // true direction; speed 0 for "light and variable"
    bool hasTemp = false;
    double tempC = 0;
    bool hasDewp = false;   // dewpoint, from the global model's relative humidity (FD products have none)
    double dewpC = 0;
};

struct AloftStation {
    std::string id; // 3-letter FD id, e.g. "ABI"
    double lat = 0, lon = 0;
    bool located = false;
    double highCloud = -1; // global model only: high cloud cover (0..1) at this point, -1 = not known
    bool model = false; // a point of the global model (Open-Meteo / GFS), not an FD station: used above the FD levels and where FD has no station
    std::vector<AloftLevel> levels; // ascending altitude
};

// Parses one windtemp text product (low or high level). Stations come back without coordinates.
std::vector<AloftStation> parseWindTemp(const std::string& text);

// Merges the levels of `more` into `into` (matching by station id).
void mergeAloft(std::vector<AloftStation>& into, const std::vector<AloftStation>& more);

std::string windTempUrl(const std::string& region, const std::string& level, int fcstHours);
// Picks the NOAA region for a position, and the ICAO prefix letter used to look up station coordinates.
void aloftRegionFor(double lat, double lon, std::string& region, char& icaoPrefix);

// Global model (Open-Meteo GFS endpoint): wind, temperature and geopotential height on pressure levels at a few points.
// Levels come back as feet MSL (geopotential height when given, else ISA).
double isaAltFtFromHpa(double hpa);
std::string openMeteoUrl(const std::vector<std::pair<double, double>>& points, const std::vector<int>& levelsHpa, bool highCloud = true);
// One model station per point (id "GFS<n>", model = true, located). Picks the hour closest to `now` (unix time). Empty on a malformed answer; `err` says why.
std::vector<AloftStation> parseOpenMeteo(const std::string& body, const std::vector<std::pair<double, double>>& points,
                                         const std::vector<int>& levelsHpa, long long now, std::string* err = nullptr);

// Winds/temps at a point, blended from the nearest stations (inverse-distance-squared on wind vectors).
struct AloftSample {
    double altFt = 0;
    bool hasWind = false;
    double dirDeg = 0, speedKt = 0;
    bool hasTemp = false;
    double tempC = 0;
    bool hasDewp = false;
    double dewpC = 0;
};
std::vector<AloftSample> sampleAloft(const std::vector<AloftStation>& stations, double lat, double lon,
                                     double maxRangeNm, int maxStations = 4);

} // namespace wx
