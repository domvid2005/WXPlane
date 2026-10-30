#pragma once
#include <string>

namespace wx {

struct Config {
    bool enabled = true;          // master switch (also in the plugin menu)
    bool useAtis = true;          // merge D-ATIS data for airports near the aircraft
    bool useAloft = true;         // NOAA winds/temps aloft instead of X-Plane's own above the surface
    int updateMinutes = 5;        // periodic refresh
    double searchNm = 120;        // METARs are fetched within this distance of the aircraft
    int maxStations = 40;         // nearest N stations are applied
    double radiusNm = 30;         // influence radius of each report
    double atisRangeNm = 35;      // ATIS lookups for airports this close to the aircraft
    int atisMaxAirports = 2;
    double maxAgeHours = 3;       // ignore older METARs
    double refetchMoveNm = 40;    // refresh early when the aircraft moves this far from the last fetch point
    bool fogLayer = true;         // render FG/BCFG/MIFG/VV as a ground-hugging stratus layer
    double fogTopAglM = 120;      // depth of a full fog layer
    int normalCeilingFt = 10000;  // reach of a report that has no aloft data (X-Plane default)
    int cruiseCeilingFt = 60000;  // reach of a report carrying winds/temps aloft (FL600)
    double aloftRangeNm = 250;    // use FD stations up to this far from each weather report
    int aloftFcstHours = 6;       // NOAA winds-aloft forecast cycle: 6, 12 or 24
    int aloftStations = 4;        // FD stations blended per report
    double clearVisM = 40000;     // visibility used for "10 SM / 9999 or more" and when a report gives none
    bool visAutocal = true;       // compare what X-Plane reports with the METAR/ATIS and correct our input
    int visCheckS = 100;          // seconds after an update to run that comparison
};

bool loadConfig(const std::string& path, Config& cfg);
bool saveConfig(const std::string& path, const Config& cfg);

} // namespace wx
