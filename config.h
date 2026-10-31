#pragma once
#include <string>

namespace wx {

constexpr int kConfigVersion = 6;   // bumped when defaults change; older config files get the new start-up defaults once

struct Config {
    bool enabled = true;          // master switch (also in the plugin menu)
    bool useAtis = true;          // merge D-ATIS data for airports near the aircraft
    bool useAloft = true;         // NOAA winds/temps aloft instead of X-Plane's own above the surface
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
    double visScale = 1.0;        // factor on reported visibility below 10 SM before it goes to X-Plane (1 = exactly the METAR/ATIS value)
    bool visAutocal = false;      // compare what X-Plane reports with the METAR/ATIS and correct our input
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
    bool radarMapTops = true;     // label storm tops (FLxxx) on X-Plane's map
    bool firstLoadImmediate = true;  // the start-up updates (stations, then radar storms) skip X-Plane's slow fade-in, so the clouds do not dissolve in patches
    bool applyImmediately = false; // true forces X-Plane to rebuild clouds at once (a visible stall); false lets it fade in over 1-2 min
    int applyPerTick = 500;       // most records handed to X-Plane in one update; every update makes the sim rebuild, so fewer is better
    int startupDelayS = 0;        // wait this long after the plugin loads before the first download
    bool fillGaps = true;            // fill the gaps between station circles so every point takes the nearest station's weather
    int fillRangeNm = 60;            // ...within this distance of the aircraft
    int fillFarRangeNm = 120;        // coarse fill out to here (never beyond the METAR search radius)
    int fillFarCellNm = 30;          // ...on a grid this size
    int fillCellNm = 12;             // ...using location records on a grid this size
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
