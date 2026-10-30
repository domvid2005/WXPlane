// Turns radar reflectivity (and echo tops) into storm cells that are applied through X-Plane's weather API.
#pragma once
#include <ctime>
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
};

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

// Which MRMS product regions could cover this position, best first. Directory is "" for CONUS.
std::vector<std::string> mrmsRegionsFor(double lat, double lon);
std::string mrmsUrl(const std::string& region, const std::string& product);

// Groups reflectivity pixels into blocks of about cfg.radarCellNm and keeps the most relevant cfg.radarMaxCells.
// `tops` (km MSL, may be null) supplies echo tops. Values below zero (MRMS "missing" codes) mean no echo.
RadarResult buildRadarCells(const GribField& refl, const GribField* tops, double lat, double lon, const Config& cfg);

// RainViewer tiles are only coloured images. `dbzFromColor` is the (approximate) inverse of their scheme.
float dbzFromRainViewerPixel(int r, int g, int b, int a);
// Builds a GribField-compatible reflectivity grid from decoded RGBA tiles (z/x/y list); see radar.cpp.
struct TileImage { int z = 0, x = 0, y = 0; int w = 0, h = 0; std::vector<uint8_t> rgba; };
bool tilesToField(const std::vector<TileImage>& tiles, const GribWindow& win, GribField& out);
// Optional user palette (lines "r,g,b,dbz") that overrides the built-in approximation of the RainViewer colours.
int loadRainViewerPalette(const std::string& path);
void tileRange(double lat, double lon, double rangeNm, int z, int& x0, int& x1, int& y0, int& y1);

// Applies a storm cell on top of the weather built for the nearest station.
void applyStorm(XpWx& x, const RadarCell& c, const Config& cfg, double fieldElevM, double lat);

} // namespace wx
