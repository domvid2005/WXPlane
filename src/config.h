#pragma once
#include <string>

namespace wx {

struct Config {
    bool enabled = true;          // master switch (also in the plugin menu)
    bool useAtis = true;          // merge D-ATIS data for airports near the aircraft
    bool extendToCruise = false;  // let each report's temperature/pressure reach cruise altitude
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
    int normalCeilingFt = 10000;  // max_altitude_msl_ft for reports (X-Plane default)
    int cruiseCeilingFt = 45000;  // used when extendToCruise is on
    double clearVisM = 40000;     // visibility used when the report says 10 km / 10 SM or more
};

bool loadConfig(const std::string& path, Config& cfg);
bool saveConfig(const std::string& path, const Config& cfg);

} // namespace wx
