#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace wx {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool toBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
} // namespace

bool loadConfig(const std::string& path, Config& c) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        double d = std::atof(v.c_str());
        if (k == "enabled") c.enabled = toBool(v);
        else if (k == "use_atis") c.useAtis = toBool(v);
        else if (k == "extend_to_cruise") c.extendToCruise = toBool(v);
        else if (k == "update_minutes") c.updateMinutes = (int)(d < 1 ? 1 : d);
        else if (k == "search_nm") c.searchNm = d < 20 ? 20 : (d > 400 ? 400 : d);
        else if (k == "max_stations") c.maxStations = (int)(d < 1 ? 1 : (d > 120 ? 120 : d));
        else if (k == "radius_nm") c.radiusNm = d < 5 ? 5 : d;
        else if (k == "atis_range_nm") c.atisRangeNm = d;
        else if (k == "atis_max_airports") c.atisMaxAirports = (int)(d < 0 ? 0 : (d > 5 ? 5 : d));
        else if (k == "max_age_hours") c.maxAgeHours = d < 0.5 ? 0.5 : d;
        else if (k == "refetch_move_nm") c.refetchMoveNm = d < 5 ? 5 : d;
        else if (k == "fog_layer") c.fogLayer = toBool(v);
        else if (k == "fog_top_agl_m") c.fogTopAglM = d < 10 ? 10 : d;
        else if (k == "normal_ceiling_ft") c.normalCeilingFt = (int)d;
        else if (k == "cruise_ceiling_ft") c.cruiseCeilingFt = (int)d;
        else if (k == "clear_vis_m") c.clearVisM = d < 10000 ? 10000 : d;
    }
    return true;
}

bool saveConfig(const std::string& path, const Config& c) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Weather Fix settings. Edit while X-Plane is closed (menu toggles are saved automatically).\n"
      << "enabled = " << (c.enabled ? 1 : 0) << "\n"
      << "use_atis = " << (c.useAtis ? 1 : 0) << "\n"
      << "extend_to_cruise = " << (c.extendToCruise ? 1 : 0) << "\n"
      << "update_minutes = " << c.updateMinutes << "\n"
      << "search_nm = " << c.searchNm << "\n"
      << "max_stations = " << c.maxStations << "\n"
      << "radius_nm = " << c.radiusNm << "\n"
      << "atis_range_nm = " << c.atisRangeNm << "\n"
      << "atis_max_airports = " << c.atisMaxAirports << "\n"
      << "max_age_hours = " << c.maxAgeHours << "\n"
      << "refetch_move_nm = " << c.refetchMoveNm << "\n"
      << "fog_layer = " << (c.fogLayer ? 1 : 0) << "\n"
      << "fog_top_agl_m = " << c.fogTopAglM << "\n"
      << "normal_ceiling_ft = " << c.normalCeilingFt << "\n"
      << "cruise_ceiling_ft = " << c.cruiseCeilingFt << "\n"
      << "clear_vis_m = " << c.clearVisM << "\n";
    return true;
}

} // namespace wx
