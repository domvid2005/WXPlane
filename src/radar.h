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
    float coverScale = 1.0f;   // cloud coverage multiplier (rim rings get thinner the further out they are)
    float radiusScale = 1.0f;  // record radius multiplier (rim cells are smaller and vary, so the outline is lumpy, not a circle chain)
    float contourScale = 1.0f; // radius multiplier that keeps the record inside the storm outline (see addStormContour)
    float edge = 0;            // 0 = inside a cluster of storm blocks .. 1 = isolated / on the rim of one (see markStormEdges)
    bool natural = false;      // a record of the natural picture (buildNaturalCells): its size is sizeNm, not the block size
    float sizeNm = 0;          // side of the square the record stands for (natural records)
    bool stratiform = false;   // steady (stratiform) rain or snow, not convection (MRMS PrecipFlag): no cumulonimbus, no storm turbulence or windshear
};

// Echo tops: a block much taller than its neighbours (or with no neighbours) renders as a thin spike. Limit each measured top to
// the neighbour mean + 2 km, and lone blocks to 8 km. Only ever lowers a top.
// Rain a storm field gives at a point (0..1, same scale as XpWx::precip): inverse-distance mean of the cells within reachNm, 0 if none.
// Station and gap-fill records use it so a dry-reporting station or fill tile inside a storm does not leave a round dry spot.
// Rain pits: a measured cell whose reflectivity is far below the ring of cells around it (at least 4 neighbours, 6 dBZ lower than their mean)
// is lifted to just under that mean, so no pocket of light rain shows in the middle of a storm. Run by smoothStormRain when fillPits is set.
void smoothStormRain(std::vector<RadarCell>& cells, double cellNm, double blend, bool fillPits = false);
// Ragged skirt: around every confirmed block, in each of 8 directions that has no block, a smaller, lighter, lower cell at a randomised
// (but stable between updates) distance and angle; in inner corners (2+ blocks right next to the spot) a full cell with the neighbours' mean; further rings (up to 3) repeat this from the previous ring, fainter and smaller each time. At most maxAdd.
int addStormRim(std::vector<RadarCell>& cells, double cellNm, double aircraftLat, double aircraftLon, double stepM, double minDbz, int maxAdd, int rings = 1);
float stormPrecipAt(const std::vector<RadarCell>& cells, double lat, double lon, double reachNm, float rainFullMmH);

// Interpolates cells into holes and channels: positions enclosed on all four sides by kept cells (at least minNeighbours within ~1.4 cells),
// or lying between kept cells on two opposite sides (at least 3 within ~1.4 cells; a one-block-wide gap open at its ends)
// that have no cell of their own. `dropped` are real blocks that lost out to the cell cap; their position and key are reused.
// Returns how many cells were added (at most maxAdd); each takes the mean reflectivity, top and coverage of its neighbours.
int fillStormHoles(std::vector<RadarCell>& kept, const std::vector<RadarCell>& dropped, double cellNm, double aircraftLat, double aircraftLon,
                   double stepM, double minDbz, int minNeighbours, int maxAdd);
// Levels tops between neighbouring cells: no pair closer than 1.5 cells may differ by more than maxDropM per cell width; the excess is split
// between the taller and the lower cell. Synthetic (rim) cells keep their deliberate taper. maxDropM <= 0 does nothing.
void limitStormTopSlope(std::vector<RadarCell>& cells, double cellNm, double maxDropM, double stepM);
void smoothStormTops(std::vector<RadarCell>& cells, double cellNm, double blend = 0.0, double stepM = 500.0, bool fillPits = false);

// Rim of a storm cluster: a block with few neighbouring blocks among the applied cells (the rim of a system, or the cut made by the cell
// limit) gets edge up to 1. A storm record stops sharply at its circle, so a full-depth block at the rim shows up as a cliff against the
// weather beside it; applyStorm gives rim blocks a lower top and less coverage in proportion, so the cluster slopes off. Interior blocks
// (5 or more neighbours within 1.7 block widths) are not touched.
void markStormEdges(std::vector<RadarCell>& cells, double cellNm);

// Storm outline: a smooth, slightly randomised contour is drawn around all blocks (a soft union of rounded squares, so concave corners and notches
// fill in and convex corners round off, with a low-frequency random wobble). Small "skin" cells are then placed just inside the contour so the
// edge follows it, and every measured block's record radius is limited so its circle does not reach beyond the contour (contourScale).
// Returns how many skin cells were added (nearest to the aircraft first, at most maxAdd). baseRadiusNm = unscaled storm record radius.
int addStormContour(std::vector<RadarCell>& cells, double cellNm, double baseRadiusNm, double aircraftLat, double aircraftLon,
                    double stepM, double minDbz, int maxAdd);

// Intensity levels inside a storm (2026-11): the same treatment as addStormContour, once per reflectivity level (e.g. 30, 40, 50 dBZ).
// For each level a smooth, slightly randomised outline is drawn around the blocks at or above it; those blocks' circles are kept
// inside it (contourScale), and small skin cells of that level are placed just inside it, so the boundary between e.g. orange and red
// follows a curve instead of the 12 nm block grid. Only boundaries inside the storm get skin cells (the outer edge is addStormContour's).
// Returns how many cells were added (nearest to the aircraft first, at most maxAdd).
int addStormLevelContours(std::vector<RadarCell>& cells, double cellNm, double baseRadiusNm, double aircraftLat, double aircraftLon,
                          double stepM, const std::vector<double>& levels, int maxAdd);

struct RadarResult {
    bool attempted = false;
    bool ok = false;
    std::string source;        // e.g. "NOAA MRMS (CONUS)"
    std::string note;          // why there is nothing, or caveats
    std::time_t dataTime = 0;  // when the radar mosaic was valid (0 = unknown)
    int echoBlocks = 0;        // blocks with echo before the cap
    bool topsFromRadar = false;
    int rimCells = 0;          // ragged rim cells added around the confirmed blocks
    int holesFilled = 0;       // cells interpolated into enclosed holes
    int levelCells = 0;        // skin cells added along the inner intensity-level outlines
    std::vector<RadarCell> cells;
    // natural picture: dry records (no rain, the station weather) between and around the rain, and the squares it covers. Gap-fill
    // tiles are left out inside those squares, so the dry and rainy records alone decide where the rain ends.
    bool natural = false;
    std::vector<RadarCell> dryCells;
    double rootNm = 24, cosRef = 1;
    std::set<std::pair<long, long>> roots;
};

// Is this spot inside the area the natural picture covers (rain and dry records)? Always false without it.
bool radarCovers(const RadarResult& r, double lat, double lon);
// The rain record (natural picture) whose square holds this spot, or null.
const RadarCell* naturalCellAt(const RadarResult& r, double lat, double lon);

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
// `conv` (optional, same grid as refl): 1 where the precipitation is convective, 0 where it is stratiform rain or snow; cells with no
// convective pixel in their footprint are marked stratiform.
RadarResult buildRadarCells(const GribField& refl, const GribField* tops, double lat, double lon, const Config& cfg,
                            const std::set<std::string>* prev = nullptr, const GribField* conv = nullptr);
// Convection reported at an airport (TS, SH, GR, GS, SQ in the weather; CB, TCU or LTG anywhere in the report). GFS points never count.
bool convectionReported(const Station& st);
// Caps steady rain/snow pixels of `refl` at capDbz and returns a mask on its grid (1 = convective). With `flag` (MRMS PrecipFlag),
// 6, 7 and 96 are convective; without it, pixels above capDbz within 60 nm of an airport reporting convection. `capped` = pixels capped.
GribField convectiveMask(GribField& refl, const GribField* flag, const std::vector<Station>* stations, float capDbz, int& capped);
// Marks every cell whose footprint holds no convective pixel of `conv` as stratiform.
void markStratiform(std::vector<RadarCell>& cells, const GribField& conv, double cellNm);

// Applies a storm cell on top of the weather built for the nearest station.
// Top (MSL, metres) of the storm cloud applied for a cell, and the value the map label shows: the measured radar echo top when
// there is one (capped at tropopause + 2.5 km), otherwise an estimate from reflectivity. baseM is the cloud base (MSL) it will sit on.
float stormTopM(const RadarCell& c, const Config& cfg, float baseM);

// The base applyStorm would give this cell (from the slot it takes and the station deck it grows out of).
float stormBaseFor(const XpWx& x, const RadarCell& c, double fieldElevM);
// baseOverrideM > 0 replaces that base (used to share one smoothed base between neighbouring cells).
void applyStorm(XpWx& x, const RadarCell& c, const Config& cfg, double fieldElevM, double lat, float baseOverrideM = -1.0f);

} // namespace wx
