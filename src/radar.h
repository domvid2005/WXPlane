// Turns radar reflectivity (and echo tops) into storm cells that are applied through X-Plane's weather API.
#pragma once
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "grib2.h"
#include "wxmodel.h"

namespace wx {

struct RadarCell {
    double lat = 0, lon = 0;   // centre of the block
    float dbz = 0;             // strongest reflectivity in the block
    float frac = 1;            // fraction of the block that is echo (0..1)
    float topM = 0;            // echo top, metres MSL; 0 = unknown (estimated later)
    bool topKnown = false;
    float dbzFine = 0;         // reflectivity at 1 dBZ resolution (rain rate comes from this, not from the 5 dBZ class)
    float rateMmH = 0;         // rain rate for dbzFine
    double distNm = 0;         // from the aircraft when built
    std::string key;           // stable per block, used to diff updates
    bool synthetic = false;    // made up around/between confirmed blocks (rim or hole fill), not measured
    float radiusScale = 1.0f;  // record radius multiplier (rim cells are smaller and vary, so the outline is lumpy, not a circle chain)
    float edge = 0;            // 0 = inside a cluster of storm blocks .. 1 = isolated / on the rim of one (see markStormEdges)
};

// Echo tops: a block much taller than its neighbours (or with no neighbours) renders as a thin spike. Limit each measured top to
// the neighbour mean + 2 km, and lone blocks to 8 km. Only ever lowers a top.
// Rain a storm field gives at a point (0..1, same scale as XpWx::precip): inverse-distance mean of the cells within reachNm, 0 if none.
// Station and gap-fill records use it so a dry-reporting station or fill tile inside a storm does not leave a round dry spot.
void smoothStormRain(std::vector<RadarCell>& cells, double cellNm, double blend);
// Ragged skirt: around every confirmed block, in each of 8 directions that has no block, a smaller, lighter, lower cell at a randomised
// (but stable between updates) distance and angle; in inner corners (3+ blocks near) a full cell with the neighbours' mean. At most maxAdd.
int addStormRim(std::vector<RadarCell>& cells, double cellNm, double aircraftLat, double aircraftLon, double stepM, double minDbz, int maxAdd);
float stormPrecipAt(const std::vector<RadarCell>& cells, double lat, double lon, double reachNm, float rainFullMmH);

// Interpolates cells into holes: positions enclosed on all four sides by kept cells (at least minNeighbours within ~1.4 cells)
// that have no cell of their own. `dropped` are real blocks that lost out to the cell cap; their position and key are reused.
// Returns how many cells were added (at most maxAdd); each takes the mean reflectivity, top and coverage of its neighbours.
int fillStormHoles(std::vector<RadarCell>& kept, const std::vector<RadarCell>& dropped, double cellNm, double aircraftLat, double aircraftLon,
                   double stepM, double minDbz, int minNeighbours, int maxAdd);
void smoothStormTops(std::vector<RadarCell>& cells, double cellNm, double blend = 0.0, double stepM = 500.0);

// Rim of a storm cluster: a block with few neighbouring blocks among the applied cells (the rim of a system, or the cut made by the cell
// limit) gets edge up to 1. A storm record stops sharply at its circle, so a full-depth block at the rim shows up as a cliff against the
// weather beside it; applyStorm gives rim blocks a lower top and less coverage in proportion, so the cluster slopes off. Interior blocks
// (5 or more neighbours within 1.7 block widths) are not touched.
void markStormEdges(std::vector<RadarCell>& cells, double cellNm);

struct RadarResult {
    bool attempted = false;
    bool ok = false;
    std::string source;        // e.g. "NOAA MRMS (CONUS)" or "RainViewer (global, experimental)"
    std::string note;          // why there is nothing, or caveats
    std::time_t dataTime = 0;  // when the radar mosaic was valid (0 = unknown)
    int echoBlocks = 0;        // blocks with echo before the cap
    bool topsFromRadar = false;
    int rimCells = 0;          // ragged rim cells added around the confirmed blocks
    int holesFilled = 0;       // cells interpolated into enclosed holes
    std::vector<RadarCell> cells;
};

// Rain rate in mm/h for a reflectivity (Marshall-Palmer stratiform, convective relation above 40 dBZ).
float rateFromDbz(float dbz);
// Typical tropopause height in km for a latitude; storm tops are limited to this when estimated.
float tropopauseKm(double lat);
// Echo top estimate (metres MSL) when no echo-top product is available.
float estimateTopM(float dbz, double lat);

// Storm top as a flight level label, rounded to the nearest 1000 ft (FL390); "~" prefix when the top is only estimated.
std::string formatFl(float topM, bool known);

// Which MRMS product regions could cover this position, best first. Directory is "" for CONUS.
std::vector<std::string> mrmsRegionsFor(double lat, double lon);
std::string mrmsUrl(const std::string& region, const std::string& product);

// Groups reflectivity pixels into blocks of about cfg.radarCellNm and keeps the most relevant cfg.radarMaxCells.
// `tops` (km MSL, may be null) supplies echo tops. Values below zero (MRMS "missing" codes) mean no echo.
// Values are quantized (5 dBZ, 500 m, quarter coverage) so that small radar wobbles do not change the records, and
// `prev` (keys applied last time) gets hysteresis so storms do not flicker in and out at the thresholds.
RadarResult buildRadarCells(const GribField& refl, const GribField* tops, double lat, double lon, const Config& cfg,
                            const std::set<std::string>* prev = nullptr);

// RainViewer tiles are only coloured images. `dbzFromColor` is the (approximate) inverse of their scheme.
float dbzFromRainViewerPixel(int r, int g, int b, int a);
// Builds a GribField-compatible reflectivity grid from decoded RGBA tiles (z/x/y list); see radar.cpp.
struct TileImage { int z = 0, x = 0, y = 0; int w = 0, h = 0; std::vector<uint8_t> rgba; };
bool tilesToField(const std::vector<TileImage>& tiles, const GribWindow& win, GribField& out);
// Optional user palette (lines "r,g,b,dbz") that overrides the built-in approximation of the RainViewer colours.
int loadRainViewerPalette(const std::string& path);
void tileRange(double lat, double lon, double rangeNm, int z, int& x0, int& x1, int& y0, int& y1);

// Applies a storm cell on top of the weather built for the nearest station.
// Top (MSL, metres) of the storm cloud applied for a cell, and the value the map label shows: the measured radar echo top when
// there is one (capped at tropopause + 2.5 km), otherwise an estimate from reflectivity. baseM is the cloud base (MSL) it will sit on.
float stormTopM(const RadarCell& c, const Config& cfg, float baseM);

void applyStorm(XpWx& x, const RadarCell& c, const Config& cfg, double fieldElevM, double lat);

} // namespace wx
