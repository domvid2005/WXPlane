// Winds & temperatures aloft from the global model (Open-Meteo GFS), and the clouds and surface weather from the same model. No X-Plane
// dependencies. (NOAA's FD wind/temperature tables were discontinued with the NAM model, SCN 26-47; their parser was removed 2026-11-10.)
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
};

struct AloftStation {
    std::string id; // 3-letter FD id, e.g. "ABI"
    double lat = 0, lon = 0;
    bool located = false;
    double highCloud = -1; // global model only: high cloud cover (0..1) at this point, -1 = not known
    bool model = false; // a point of the global model (Open-Meteo / GFS), not an FD station: used above the FD levels and where FD has no station
    std::vector<AloftLevel> levels; // ascending altitude
    // Global-model clouds (separate, slower download): cloud cover on pressure levels, (altitude ft MSL, 0..1) ascending.
    std::vector<std::pair<double, double>> cloudLevels;
};

// Global model (Open-Meteo GFS endpoint): wind, temperature and geopotential height on pressure levels at a few points.
// Levels come back as feet MSL (geopotential height when given, else ISA).
double isaAltFtFromHpa(double hpa);
std::string openMeteoUrl(const std::vector<std::pair<double, double>>& points, const std::vector<int>& levelsHpa, bool highCloud = true);
// One model station per point (id "GFS<n>", model = true, located). Picks the hour closest to `now` (unix time). Empty on a malformed answer; `err` says why.
std::vector<AloftStation> parseOpenMeteo(const std::string& body, const std::vector<std::pair<double, double>>& points,
                                         const std::vector<int>& levelsHpa, long long now, std::string* err = nullptr);

// Global-model clouds and surface weather at a lattice point (Open-Meteo GFS): cloud cover on pressure levels, for cloud layers above
// what an airport report can see, and the surface weather, for open water where no airport reports.
struct ModelPoint {
    double lat = 0, lon = 0;       // as requested (lattice node)
    double elevM = 0;              // the model's ground height there
    std::vector<std::pair<double, double>> cloudLevels;   // (altitude ft MSL, cover 0..1), ascending
    bool sfc = false;
    double tempC = 0, dewC = 0, mslHpa = 0, windDir = 0, windKt = 0, gustKt = 0, visM = -1, precipMm = 0, snowCm = 0;
    int weatherCode = -1;
};
const std::vector<int>& modelCloudLevels();   // 1000 ... 150 hPa
std::string openMeteoCloudUrl(const std::vector<std::pair<double, double>>& points);
std::vector<ModelPoint> parseOpenMeteoClouds(const std::string& body, const std::vector<std::pair<double, double>>& points, long long now,
                                             std::string* err = nullptr);

// Winds/temps at a point, blended from the nearest stations (inverse-distance-squared on wind vectors).
struct AloftSample {
    double altFt = 0;
    bool hasWind = false;
    double dirDeg = 0, speedKt = 0;
    bool hasTemp = false;
    double tempC = 0;
};
std::vector<AloftSample> sampleAloft(const std::vector<AloftStation>& stations, double lat, double lon,
                                     double maxRangeNm, int maxStations = 4);

} // namespace wx
