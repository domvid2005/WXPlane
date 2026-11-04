#pragma once
#include <string>

namespace wx {

constexpr int kConfigVersion = 25;   // bumped when defaults change; older config files get the new start-up defaults once

struct Config {
    bool enabled = true;          // master switch (also in the plugin menu)
    bool useAtis = true;          // merge D-ATIS data for airports near the aircraft
    bool upperAir = true;        // global model (Open-Meteo GFS) winds/temps for the levels above FD (FL390) and where no FD station is near
    bool useAloft = true;         // NOAA winds/temps aloft instead of X-Plane's own above the surface
    double lowCloudType = 3.0;    // X-Plane cloud type for low/overcast layers: 1 = smooth stratus sheet (curtain-like edges), 2 = cumulus, 3 = billowy cumulonimbus
    double stormTopVariationM = 1500; // estimated storm tops (no radar echo-top data) vary by up to +/- this, stable per block, so tops are not one flat plateau
    double startupCalmS = 1.5;        // scenery loading must have been finished this long before the first download starts
    double regenWaitS = 1.5;          // pause between X-Plane's weather reset and our first update
    bool regenWeatherOnStart = true;  // before the first load is applied, run X-Plane's own 'regenerate weather' command so none of its previous clouds are left
    double radarMinutes = 5;         // storm radar (MRMS publishes every ~2 min) is refetched this often, on its own
    bool stormSmoothTops = true;  // tame lone tall echo tops that render as thin spikes
    int updateMinutes = 5;        // periodic refresh
    double searchNm = 120;        // METARs are fetched within this distance of the aircraft
    int maxStations = 40;         // nearest N stations are applied
    double radiusNm = 30;         // influence radius of each report
    double atisRangeNm = 35;      // ATIS lookups for airports this close to the aircraft
    int atisMaxAirports = 2;
    double maxAgeHours = 3;       // ignore older METARs
    double refetchMoveNm = 40;    // refresh early when the aircraft moves this far from the last fetch point
    bool fogLayer = true;         // render FG/BCFG/MIFG/VV as a ground-hugging stratus layer
    double fogTopAglM = 200;      // depth of a full fog layer (thin layers shimmer when seen from above)
    bool fogHaze = true;          // fog / mist / haze also cap visibility (and saturate the air for fog)
    int normalCeilingFt = 10000;  // reach of a report that has no aloft data (X-Plane default)
    int cruiseCeilingFt = 60000;  // reach of a report carrying winds/temps aloft (FL600)
    double aloftRangeNm = 250;    // use FD stations up to this far from each weather report
    int aloftFcstHours = 6;       // NOAA winds-aloft forecast cycle: 6, 12 or 24
    int aloftStations = 4;        // FD stations blended per report
    double clearVisM = 40000;     // visibility used for "10 SM / 9999 or more" and when a report gives none
    double roughMinCoverage = 0.65; // layers at least this solid get rough-top records (0.65 = broken and overcast; 0.85 = overcast only)
    bool topBumps = false;        // EXPERIMENTAL: scatter records over thick overcasts that differ from the parent only by a slightly higher top
    int bumpCount = 10;           // rough-top records per overcast station
    int bumpMax = 200;            // most of them in one update
    int bumpFillCount = 1;        // rough-top records per gap-fill tile that carries an overcast
    double bumpHeightM = 550;     // how far a bump can stand above the deck top (m)
    double bumpRadiusFactor = 0.26; // record radius = this x the station record radius
    double visScale = 1.0;        // factor on reported visibility below 10 SM before it goes to X-Plane (1 = exactly the METAR/ATIS value)
    bool visAutocal = true;       // compare what X-Plane reports with the METAR/ATIS and correct our input
    int visCheckS = 100;          // seconds after an update to run that comparison
    // Radar storms (NOAA MRMS mosaic, optional global fallback)
    bool radar = true;            // turn radar echoes into storm cells
    bool radarGlobal = false;     // outside MRMS coverage use RainViewer tiles (experimental, colour-decoded)
    double radarRangeNm = 150;    // storms within this distance of the aircraft
    double stormRadiusFactor = 1.3;   // storm record radius = this x radar cell size (min 6 nm); cells sit at the centre of their echo (storm_centroid), up to half a block off the grid, so 1.3 keeps them overlapping each other and the gap-fill tiles
    bool stormCentroid = true;       // a storm cell sits at the reflectivity-weighted centre of its echo instead of the block centre (breaks up the square grid)
    double stormRainBlend = 0.4;     // 0..0.8: pull each cell's reflectivity (rain rate) toward its neighbours', so rain steps between blocks round off
    double stormTopStepM = 250;      // storm tops are rounded to this step (smaller = fewer terraces between neighbouring cells, more records change between updates)
    double stormTopBlend = 0.4;      // 0 = only tall spikes are clipped; 0.3-0.5 also pulls every top toward its neighbours (rounds plateau steps)
    double stormTopVariationKnownM = 0; // fixed per-block height variation (m) added to MEASURED echo tops too (estimated tops always use storm_top_variation_m)
    int stormRimRings = 3;           // 1..3: how many fading rings of rim cells go around a storm (more = softer, rounder edge, more records)
    bool stormReplacesFill = true;   // no background (gap-fill) tile where a confirmed storm cell covers: its low deck would cut a canyon through the storm
    bool stormFillPits = true;       // a block much lower than the ring of blocks around it is lifted toward them (no sheer rectangular craters inside a storm)
    double stormTopMaxDropM = 0;     // largest top difference allowed between neighbouring storm cells, per cell width (0 = off)
    bool stormBaseSmooth = true;     // storm cells next to each other share a smoothed cloud base, so the underside has no steps where nearest stations differ
    int stormLevelDelayS = 30;       // the intensity-outline records follow the rest of an update this many seconds later, in their own update
    bool stormLevelContours = true;  // the same smooth outline for every intensity level inside a storm (30/40/50 dBZ), so cores are curved, not grid squares
    bool stormContour = true;        // draw one smooth, randomised outline around the storm blocks (instead of random rim rings) and keep the records inside it
    bool stormRim = true;            // ragged, tapered rim cells around the confirmed storm blocks (and in their inner corners), so outlines are not square walls
    bool stormFillHoles = true;      // a block enclosed by storm cells but missing from the kept set (cap, weak echo, no echo) gets a cell interpolated from its neighbours
    double radarCellNm = 12;      // block size one storm record represents
    int radarMaxCells = 60;       // cap on storm records handed to X-Plane
    // Natural radar picture (2026-11): instead of 12 nm blocks, the radar mosaic is smoothed, given a gentle random warp (stable between
    // updates) and covered with records that are small where the picture changes (edges, intensity steps) and large where it does not.
    bool radarNatural = true;
    double cloudSmoothNm = 8.0;         // cloud edges: every layer is averaged over the records within ~3x this (Gaussian sigma, nm); 0 = off
    double emptyLayerCoverage = 0.01;   // coverage given to an empty cloud slot that takes a neighbour's height (no ribbons; 1% shows nothing, but keeps the height)
    int radarNaturalMax = 150;          // records the natural picture may use (rain and the dry records around it)
    double radarNaturalFineNm = 3.0;    // smallest record near the aircraft (grows with distance, and when the budget runs short)
    double radarMinDbz = 20;      // ignore weaker echoes
    double radarRainFullMmH = 40; // rain rate that maps to X-Plane's full precipitation (1.0)
    double radarTurbulence = 1.0; // 0 disables storm turbulence, 1 = scaled to reflectivity
    bool radarEchoTops = true;    // download MRMS echo tops for storm heights
    double stormCoreCoverage = 0.95; // coverage of storm cells that are surrounded by other storm cells (35+ dBZ); rim cells keep storm_coverage_max, so only the outline breaks up
    double stormCoverageMax = 0.95; // coverage of the densest storm cloud
    double manualRefreshGapS = 60;   // menu refreshes closer together than this are coalesced into one
    bool manualRefreshImmediate = false; // Refresh Weather from the menu: true = the new weather appears at once but the sim freezes for a few seconds; false = it fades in
    bool firstLoadImmediate = true;  // the start-up updates (stations, then radar storms) skip X-Plane's slow fade-in, so the clouds do not dissolve in patches
    int applyPerTick = 500;       // most records handed to X-Plane in one update; every update makes the sim rebuild, so fewer is better
    int startupDelayS = 0;        // wait this long after the plugin loads before the first download
    bool fillGaps = true;            // fill the gaps between station circles so every point takes the nearest station's weather
    int fillRangeNm = 60;            // ...within this distance of the aircraft
    int fillFarRangeNm = 120;        // coarse fill out to here (never beyond the METAR search radius)
    int fillFarCellNm = 30;          // ...on a grid this size
    int fillCellNm = 12;             // ...using location records on a grid this size
    bool fillStagger = false;         // offset every other row of the fill grid by half a tile and wobble each tile a little, so cloud edges are not grid steps. Off by default: in X-Plane it opened clear gaps under storms with thin ribbons of cloud along them (2026-11-03)
    double stationRadiusFactor = 0.49; // station radius = this x distance to nearest neighbour (0.49 = circles just touch; above that they overlap and X-Plane blends the edge)
    double fillRadiusFactor = 1.0;    // gap-fill record radius = this x grid cell size (0.71 would just cover the corners; 1.0 overlaps the neighbouring tiles and the storm cells so no gap is left)
    bool stationRadiusAuto = true;   // shrink each station's radius so close neighbours do not blend into it
    bool startupWaitFps = false;     // true: hold the first update until the frame rate has been steady for a few seconds (up to startup_max_wait_s)
    bool startupStaged = false;      // true: first update = METAR/ATIS/winds only, radar storms follow startup_radar_delay_s later
    int startupRadarDelayS = 8;      // staged start-up: seconds after the first update before radar storms are added
    int startupMaxWaitS = 90;        // never hold the first update back longer than this after the sim starts running
    bool startupWaitScenery = false; // also wait (up to 2.5 min) until X-Plane reports scenery loading finished
    int transitionS = 0;          // first load: release the weather records evenly over this many seconds (0 = all in one update, recommended: each extra update is another hitch)
    int updateTransitionS = 0;    // later updates: same, for the records that changed       
};

bool loadConfig(const std::string& path, Config& cfg);
bool saveConfig(const std::string& path, const Config& cfg);

} // namespace wx
