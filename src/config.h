#pragma once
#include <string>

namespace wx {

constexpr int kConfigVersion = 14;   // bumped when defaults change; older config files get the new start-up defaults once

struct Config {
    bool enabled = true;          // master switch (also in the plugin menu)
    bool useAtis = true;          // merge D-ATIS data for airports near the aircraft
    bool useAloft = true;         // NOAA winds/temps aloft instead of X-Plane's own above the surface
    double lowCloudType = 3.0;    // X-Plane cloud type for low/overcast layers: 1 = smooth stratus sheet (curtain-like edges), 2 = cumulus, 3 = billowy cumulonimbus
    double stormTopVariationM = 1500; // estimated storm tops (no radar echo-top data) vary by up to +/- this, stable per block, so tops are not one flat plateau
    double startupCalmS = 1.5;        // scenery loading must have been finished this long before the first download starts
    double regenWaitS = 1.5;          // pause between X-Plane's weather reset and our first update
    bool regenWeatherOnStart = true;  // before the first load is applied, run X-Plane's own 'regenerate weather' command so none of its previous clouds are left
    double radarMinutes = 5;         // storm radar (MRMS publishes every ~2 min) is refetched this often, on its own
    bool stormSmoothTops = true;  // tame lone tall echo tops that render as thin spikes
    int updateMinutes = 30;       // periodic refresh
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
    bool edgeClouds = false;       // ring of type-3 clouds around the edge of every overcast so its edge is billowy, not a smooth wall
    double edgeCoverage = 0.8;    // coverage of the overcast layer in those edge records
    double edgeRadiusFactor = 0.6; // edge record radius = this x the station record radius
    int edgePoints = 8;           // edge records per overcast station
    int edgeMax = 120;            // most edge records in one update
    double roughMinCoverage = 0.65; // layers at least this solid get rough tops, edges and the cap layer (0.65 = broken and overcast; 0.85 = overcast only)
    bool stormRough = true;       // ring and cap strong storm cells with turret/tower records so they are not straight-walled tabletops
    int stormRoughCount = 5;      // records per storm cell (every third one is a tower above the top, the rest short turrets round the rim)
    int stormRoughMax = 220;      // most of them in one update
    double stormRoughMinDbz = 35; // only cells at least this strong
    double stormRoughHeightM = 1800; // how far a tower stands above the cell top (m)
    bool capLayer = false;        // very thin type-3 layer just above every thick overcast (breaks up the flat top)
    double capCoverage = 0.5;     // its coverage
    double capThickM = 70;        // its thickness (m)
    double capGapM = 40;          // clear gap between the overcast top and the cap layer (m)
    bool topBumps = true;         // scatter billowy records over thick overcasts so the top is uneven, not one flat sheet
    int bumpCount = 14;           // rough-top records per overcast station
    int bumpMax = 320;            // most of them in one update
    int bumpFillCount = 2;        // rough-top records per gap-fill tile that carries an overcast
    double bumpHeightM = 550;     // how far a bump can stand above the deck top (m)
    double bumpCoverage = 0.7;    // coverage of the overcast layer in those records
    double bumpRadiusFactor = 0.26; // record radius = this x the station record radius
    int handoffAltFt = 0;         // above this altitude (ft MSL) X-Plane's own weather takes over from ours; 0 = our weather all the way up
    bool handoffRealWeather = true; // also switch X-Plane to its own real weather while above the hand-off band (put back below it)
    int handoffBandFt = 3000;     // our clouds fade out over this many feet below the hand-off altitude
    double visScale = 1.0;        // factor on reported visibility below 10 SM before it goes to X-Plane (1 = exactly the METAR/ATIS value)
    bool visAutocal = true;       // compare what X-Plane reports with the METAR/ATIS and correct our input
    int visCheckS = 100;          // seconds after an update to run that comparison
    // Radar storms (NOAA MRMS mosaic, optional global fallback)
    bool radar = true;            // turn radar echoes into storm cells
    bool radarGlobal = false;     // outside MRMS coverage use RainViewer tiles (experimental, colour-decoded)
    double radarRangeNm = 150;    // storms within this distance of the aircraft
    double radarCellNm = 12;      // block size one storm record represents
    int radarMaxCells = 60;       // cap on storm records handed to X-Plane
    double radarMinDbz = 20;      // ignore weaker echoes
    double radarRainFullMmH = 40; // rain rate that maps to X-Plane's full precipitation (1.0)
    double radarTurbulence = 1.0; // 0 disables storm turbulence, 1 = scaled to reflectivity
    bool radarEchoTops = true;    // download MRMS echo tops for storm heights
    double stormCoverageMax = 0.95; // coverage of the densest storm cloud
    bool manualRefreshImmediate = false; // Refresh Weather from the menu: true = the new weather appears at once but the sim freezes for a few seconds; false = it fades in
    bool firstLoadImmediate = true;  // the start-up updates (stations, then radar storms) skip X-Plane's slow fade-in, so the clouds do not dissolve in patches
    int applyPerTick = 500;       // most records handed to X-Plane in one update; every update makes the sim rebuild, so fewer is better
    int startupDelayS = 0;        // wait this long after the plugin loads before the first download
    bool fillGaps = true;            // fill the gaps between station circles so every point takes the nearest station's weather
    int fillRangeNm = 60;            // ...within this distance of the aircraft
    int fillFarRangeNm = 120;        // coarse fill out to here (never beyond the METAR search radius)
    int fillFarCellNm = 30;          // ...on a grid this size
    int fillCellNm = 12;             // ...using location records on a grid this size
    double stationRadiusFactor = 0.49; // station radius = this x distance to nearest neighbour (0.49 = circles just touch; above that they overlap and X-Plane blends the edge)
    double fillRadiusFactor = 0.8;    // gap-fill record radius = this x grid cell size (0.8 = barely touching)
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
