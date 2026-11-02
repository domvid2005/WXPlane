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
    float rateMmH = 0;         // rain rate for dbz
    double distNm = 0;         // from the aircraft when built
    std::string key;           // stable per block, used to diff updates
    float edge = 0;            // 0 = inside a cluster of storm blocks .. 1 = isolated / on the rim of one (see markStormEdges)
};

// Echo tops: a block much taller than its neighbours (or with no neighbours) renders as a thin spike. Limit each measured top to
// the neighbour mean + 2 km, and lone blocks to 8 km. Only ever lowers a top.
void smoothStormTops(std::vector<RadarCell>& cells, double cellNm);

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
