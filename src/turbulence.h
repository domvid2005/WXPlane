// Real-data turbulence (no X-Plane calls; see docs/turbulence.md).
//  - NOAA GTGN (US): observed-and-modelled EDR nowcast on the 3 km HRRR Lambert grid, every 15 min, 1,000 ft levels.
//  - ECMWF Open Data (elsewhere): no EDR is published, so an Ellrod TI1 index (vertical wind shear x deformation) is
//    computed from the IFS winds and converted to an EDR-equivalent category. It is an estimate, labelled as such.
// The EDR (or EDR-equivalent) of the air is then mapped to X-Plane's sim/weather/region/turbulence scale through the
// response measured in X-Plane (2026-11-06 test), not linearly.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "gribx.h"

namespace wx {
namespace turb {

// ---- Calibration --------------------------------------------------------------------------------------------------
// Standard deviation of the vertical load factor (g) that a given EDR should give a typical transport aircraft.
double targetSigmaG(double edr);
// X-Plane region turbulence level (0..3) that gave that response in the 2026-11-06 test (piecewise linear, inverted).
double xpLevelForSigma(double sigmaG);
inline double edrToXpLevel(double edr) { return xpLevelForSigma(targetSigmaG(edr)); }
// Ellrod TI1 (in 1e-7 s^-2) to an EDR-equivalent value (category conversion; uncertain by about one category).
double ellrodToEdrEquivalent(double ti1e7);
// ISA altitude (ft) of a pressure (hPa), and the reverse.
double isaAltFt(double hPa);
double isaPressureHpa(double altFt);

// ---- NOAA GTGN ----------------------------------------------------------------------------------------------------
struct GtgnLevel {
    double altFt = 0;
    std::vector<float> v;          // crop values (ni x nj, row j major), NaN where missing (below ground, outside)
};
struct GtgnCube {
    gribx::Grid grid;              // the full grid
    int i0 = 0, j0 = 0, ni = 0, nj = 0;   // the crop in grid points
    std::vector<GtgnLevel> levels; // sorted by altitude
    std::time_t valid = 0;
    std::string file;
    double centerLat = 0, centerLon = 0;
    // EDR at a point; `coverage` 0..1 falls to 0 within ~60 km of the grid edge. False outside the crop.
    bool sample(double lat, double lon, double altFt, double& edr, double& coverage) const;
    // Crops a decoded full-grid field around a centre (half-size in grid points) and adds it as a level.
    void addLevel(double altFt, const std::vector<float>& full, double clat, double clon, int halfPts);
    bool hasLevel(double altFt) const;
    bool cropCovers(double lat, double lon, int marginPts) const;
};
// Altitude in ft of a GTGN message's level (fixed surface types 102 m MSL, 103 m AGL, 100 Pa, 1 surface); NaN if unknown.
double gtgnLevelFt(const gribx::Message& m);
bool isEdr(const gribx::Message& m);   // discipline 0, category 19, parameter 30 (eddy dissipation parameter)

// ---- ECMWF Open Data ----------------------------------------------------------------------------------------------
struct IndexEntry { std::string param, levtype; int level = 0; int step = -1; uint64_t offset = 0, length = 0; };
// Parses an ECMWF Open Data .index file (one JSON object per line).
std::vector<IndexEntry> parseEcmwfIndex(const std::string& text);
// Pressure levels used for the index (FL100 to FL530): 700 ... 100 hPa.
const std::vector<int>& ecmwfLevels();

struct EcLayer {
    double altFt = 0;              // ISA altitude of the layer middle
    double loHpa = 0, hiHpa = 0;
    std::vector<float> ti;         // Ellrod TI1 (1e-7 s^-2) on the crop, NaN at the crop edge
};
struct EcCube {
    double lat0 = 0, lon0 = 0;     // crop north-west corner
    double d = 0.25;               // spacing (deg)
    int ni = 0, nj = 0;
    std::vector<EcLayer> layers;   // sorted by altitude
    std::time_t valid = 0;
    std::string run;               // e.g. "2026-11-06 00Z +18 h"
    double centerLat = 0, centerLon = 0;
    // Ellrod TI1 at a point. False outside the crop or outside FL100-FL450 (where the index is not used).
    bool sampleTi(double lat, double lon, double altFt, double& ti) const;
    bool cropCovers(double lat, double lon, double marginDeg) const;
};
// Builds the cube from u and v fields (full global 0.25-degree grids, one per level in ecmwfLevels() order present).
struct EcLevelField { int hPa = 0; const std::vector<float>* u = nullptr; const std::vector<float>* v = nullptr; };
bool buildEcCube(const gribx::Grid& g, const std::vector<EcLevelField>& levels, double clat, double clon, double halfDeg, EcCube& out, std::string& err);

// ---- The combined field ---------------------------------------------------------------------------------------------
struct Inputs {
    std::shared_ptr<const GtgnCube> gtgn, gtgnPrev;   // newest analysis, and the one before (cross-faded over 3 min)
    std::time_t gtgnArrived = 0;                       // when the newest one was loaded
    std::shared_ptr<const EcCube> ecA, ecB;            // two forecast steps bracketing now
};
struct Sample {
    double edr = 0;                // value used (EDR, or EDR-equivalent from ECMWF), before the realism scale
    double gtgnEdr = -1, ecTi = -1, ecEdr = -1;        // the parts (-1 = not available)
    double coverage = 0;           // GTGN weight 0..1
    double noise = 1;              // small-scale multiplier applied
    const char* source = "none";   // "GTGN", "ECMWF estimate", "GTGN+ECMWF", "none"
};
// Fresh-data limits: GTGN analysis older than this is not used; ECMWF steps further than this from now are not used.
constexpr long kGtgnMaxAgeS = 45 * 60;
constexpr long kEcMaxGapS = 4 * 3600;
// The EDR at a point and altitude at time `now`. `simTime` drives the small-scale variation.
Sample sampleAt(const Inputs& in, double lat, double lon, double altFt, std::time_t now, double simTime);
// Smooth, position-and-time correlated multiplier (about 0.8 .. 1.25) for pockets of smoother and rougher air.
double pocketNoise(double lat, double lon, double altFt, double simTime, double amplitude);

} // namespace turb
} // namespace wx
