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
    int version = 0;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        double d = std::atof(v.c_str());
        if (k == "config_version") version = (int)d;
        else if (k == "enabled") c.enabled = toBool(v);
        else if (k == "use_atis") c.useAtis = toBool(v);
        else if (k == "use_aloft") c.useAloft = toBool(v);
        else if (k == "update_minutes") c.updateMinutes = (int)(d < 1 ? 1 : d);
        else if (k == "search_nm") c.searchNm = d < 20 ? 20 : (d > 400 ? 400 : d);
        else if (k == "max_stations") c.maxStations = (int)(d < 1 ? 1 : (d > 120 ? 120 : d));
        else if (k == "radius_nm") c.radiusNm = d < 5 ? 5 : d;
        else if (k == "atis_range_nm") c.atisRangeNm = d;
        else if (k == "atis_max_airports") c.atisMaxAirports = (int)(d < 0 ? 0 : (d > 5 ? 5 : d));
        else if (k == "max_age_hours") c.maxAgeHours = d < 0.5 ? 0.5 : d;
        else if (k == "refetch_move_nm") c.refetchMoveNm = d < 5 ? 5 : d;
        else if (k == "fog_layer") c.fogLayer = toBool(v);
        else if (k == "fog_haze") c.fogHaze = toBool(v);
        else if (k == "fog_top_agl_m") c.fogTopAglM = d < 10 ? 10 : d;
        else if (k == "normal_ceiling_ft") c.normalCeilingFt = (int)d;
        else if (k == "aloft_ceiling_ft") c.cruiseCeilingFt = (int)d;
        else if (k == "aloft_range_nm") c.aloftRangeNm = d < 50 ? 50 : d;
        else if (k == "aloft_fcst_hours") c.aloftFcstHours = (int)d;
        else if (k == "aloft_stations") c.aloftStations = (int)(d < 1 ? 1 : (d > 8 ? 8 : d));
        else if (k == "clear_vis_m") c.clearVisM = d < 10000 ? 10000 : d;
        else if (k == "vis_scale") c.visScale = d < 0.2 ? 0.2 : (d > 1.5 ? 1.5 : d);
        else if (k == "vis_autocal") c.visAutocal = toBool(v);
        else if (k == "vis_check_s") c.visCheckS = (int)(d < 1 ? 1 : d);
        else if (k == "radar") c.radar = toBool(v);
        else if (k == "radar_global") c.radarGlobal = toBool(v);
        else if (k == "radar_range_nm") c.radarRangeNm = d < 30 ? 30 : (d > 300 ? 300 : d);
        else if (k == "radar_cell_nm") c.radarCellNm = d < 3 ? 3 : (d > 30 ? 30 : d);
        else if (k == "radar_max_cells") c.radarMaxCells = (int)(d < 10 ? 10 : (d > 400 ? 400 : d));
        else if (k == "radar_min_dbz") c.radarMinDbz = d < 5 ? 5 : (d > 45 ? 45 : d);
        else if (k == "radar_rain_full_mmh") c.radarRainFullMmH = d < 5 ? 5 : d;
        else if (k == "radar_turbulence") c.radarTurbulence = d < 0 ? 0 : (d > 2 ? 2 : d);
        else if (k == "radar_echo_tops") c.radarEchoTops = toBool(v);
        else if (k == "storm_coverage_max") c.stormCoverageMax = d < 0.5 ? 0.5 : (d > 1 ? 1 : d);
        else if (k == "radar_map_tops") c.radarMapTops = toBool(v);
        else if (k == "first_load_immediate") c.firstLoadImmediate = toBool(v);
        else if (k == "apply_immediately") c.applyImmediately = toBool(v);
        else if (k == "apply_per_tick") c.applyPerTick = (int)(d < 1 ? 1 : (d > 1000 ? 1000 : d));
        else if (k == "fill_gaps") c.fillGaps = toBool(v);
        else if (k == "fill_range_nm") c.fillRangeNm = (int)(d < 10 ? 10 : (d > 150 ? 150 : d));
        else if (k == "fill_cell_nm") c.fillCellNm = (int)(d < 6 ? 6 : (d > 30 ? 30 : d));
        else if (k == "fill_far_range_nm") c.fillFarRangeNm = (int)(d < 20 ? 20 : (d > 250 ? 250 : d));
        else if (k == "fill_far_cell_nm") c.fillFarCellNm = (int)(d < 12 ? 12 : (d > 60 ? 60 : d));
        else if (k == "radar_minutes") c.radarMinutes = d < 0.1 ? 0.1 : (d > 120 ? 120 : d);
        else if (k == "storm_smooth_tops") c.stormSmoothTops = toBool(v);
        else if (k == "station_radius_factor") c.stationRadiusFactor = d < 0.3 ? 0.3 : (d > 1.2 ? 1.2 : d);
        else if (k == "fill_radius_factor") c.fillRadiusFactor = d < 0.5 ? 0.5 : (d > 1.5 ? 1.5 : d);
        else if (k == "station_radius_auto") c.stationRadiusAuto = toBool(v);
        else if (k == "startup_wait_fps") c.startupWaitFps = toBool(v);
        else if (k == "startup_radar_delay_s") c.startupRadarDelayS = (int)(d < 0 ? 0 : (d > 600 ? 600 : d));
        else if (k == "startup_staged") c.startupStaged = toBool(v);
        else if (k == "startup_max_wait_s") c.startupMaxWaitS = (int)(d < 5 ? 5 : (d > 600 ? 600 : d));
        else if (k == "startup_wait_scenery") c.startupWaitScenery = toBool(v);
        else if (k == "transition_s") c.transitionS = (int)(d < 0 ? 0 : (d > 600 ? 600 : d));
        else if (k == "update_transition_s") c.updateTransitionS = (int)(d < 0 ? 0 : (d > 600 ? 600 : d));
        else if (k == "startup_delay_s") c.startupDelayS = (int)(d < 0 ? 0 : (d > 300 ? 300 : d));
    }
    if (version < kConfigVersion) {   // the start-up defaults changed: take the new ones once, keep everything else as the user set it
        Config d;
        c.updateMinutes = d.updateMinutes;
        c.startupDelayS = d.startupDelayS;
        c.startupMaxWaitS = d.startupMaxWaitS;
        c.startupRadarDelayS = d.startupRadarDelayS;
        c.startupStaged = d.startupStaged;
        c.startupWaitFps = d.startupWaitFps;
        c.firstLoadImmediate = d.firstLoadImmediate;
        c.transitionS = d.transitionS;   // drip-feeding records over time (older test builds) made the load-in last minutes
        c.updateTransitionS = d.updateTransitionS;
        c.applyPerTick = d.applyPerTick;
        c.visAutocal = d.visAutocal;     // the self-check turned out to chase X-Plane's slow fade and cut visibility; off by default now
    }
    return true;
}

bool saveConfig(const std::string& path, const Config& c) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# WXPlane settings. Edit while X-Plane is closed (menu toggles are saved automatically).\n"
      << "config_version = " << kConfigVersion << "\n"
      << "enabled = " << (c.enabled ? 1 : 0) << "\n"
      << "use_atis = " << (c.useAtis ? 1 : 0) << "\n"
      << "use_aloft = " << (c.useAloft ? 1 : 0) << "\n"
      << "update_minutes = " << c.updateMinutes << "\n"
      << "search_nm = " << c.searchNm << "\n"
      << "max_stations = " << c.maxStations << "\n"
      << "radius_nm = " << c.radiusNm << "\n"
      << "atis_range_nm = " << c.atisRangeNm << "\n"
      << "atis_max_airports = " << c.atisMaxAirports << "\n"
      << "max_age_hours = " << c.maxAgeHours << "\n"
      << "refetch_move_nm = " << c.refetchMoveNm << "\n"
      << "fog_layer = " << (c.fogLayer ? 1 : 0) << "\n"
      << "fog_haze = " << (c.fogHaze ? 1 : 0) << "\n"
      << "fog_top_agl_m = " << c.fogTopAglM << "\n"
      << "normal_ceiling_ft = " << c.normalCeilingFt << "\n"
      << "aloft_ceiling_ft = " << c.cruiseCeilingFt << "\n"
      << "aloft_range_nm = " << c.aloftRangeNm << "\n"
      << "aloft_fcst_hours = " << c.aloftFcstHours << "\n"
      << "aloft_stations = " << c.aloftStations << "\n"
      << "clear_vis_m = " << c.clearVisM << "\n"
      << "vis_scale = " << c.visScale << "\n"
      << "vis_autocal = " << (c.visAutocal ? 1 : 0) << "\n"
      << "vis_check_s = " << c.visCheckS << "\n"
      << "radar = " << (c.radar ? 1 : 0) << "\n"
      << "radar_global = " << (c.radarGlobal ? 1 : 0) << "\n"
      << "radar_range_nm = " << c.radarRangeNm << "\n"
      << "radar_cell_nm = " << c.radarCellNm << "\n"
      << "radar_max_cells = " << c.radarMaxCells << "\n"
      << "radar_min_dbz = " << c.radarMinDbz << "\n"
      << "radar_rain_full_mmh = " << c.radarRainFullMmH << "\n"
      << "radar_turbulence = " << c.radarTurbulence << "\n"
      << "radar_echo_tops = " << (c.radarEchoTops ? 1 : 0) << "\n"
      << "storm_coverage_max = " << c.stormCoverageMax << "\n"
      << "radar_map_tops = " << (c.radarMapTops ? 1 : 0) << "\n"
      << "first_load_immediate = " << (c.firstLoadImmediate ? 1 : 0) << "\n"
      << "apply_immediately = " << (c.applyImmediately ? 1 : 0) << "\n"
      << "apply_per_tick = " << c.applyPerTick << "\n"
      << "fill_gaps = " << (c.fillGaps ? 1 : 0) << "\n"
      << "fill_range_nm = " << c.fillRangeNm << "\n"
      << "fill_cell_nm = " << c.fillCellNm << "\n"
      << "fill_far_range_nm = " << c.fillFarRangeNm << "\n"
      << "fill_far_cell_nm = " << c.fillFarCellNm << "\n"
      << "radar_minutes = " << c.radarMinutes << "\n"
      << "storm_smooth_tops = " << (c.stormSmoothTops ? 1 : 0) << "\n"
      << "station_radius_factor = " << c.stationRadiusFactor << "\n"
      << "fill_radius_factor = " << c.fillRadiusFactor << "\n"
      << "station_radius_auto = " << (c.stationRadiusAuto ? 1 : 0) << "\n"
      << "startup_wait_fps = " << (c.startupWaitFps ? 1 : 0) << "\n"
      << "startup_radar_delay_s = " << c.startupRadarDelayS << "\n"
      << "startup_staged = " << (c.startupStaged ? 1 : 0) << "\n"
      << "startup_max_wait_s = " << c.startupMaxWaitS << "\n"
      << "startup_wait_scenery = " << (c.startupWaitScenery ? 1 : 0) << "\n"
      << "transition_s = " << c.transitionS << "\n"
      << "update_transition_s = " << c.updateTransitionS << "\n"
      << "startup_delay_s = " << c.startupDelayS << "\n";
    return true;
}

} // namespace wx
