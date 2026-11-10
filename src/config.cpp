#include "config.h"

#include <cmath>
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
    bool sawRealistic = false;
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
        else if (k == "upper_air") c.upperAir = toBool(v);
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
        else if (k == "rough_min_coverage") c.roughMinCoverage = d < 0.3 ? 0.3 : (d > 0.95 ? 0.95 : d);
        else if (k == "manual_refresh_gap_s") c.manualRefreshGapS = d < 0 ? 0 : (d > 600 ? 600 : d);
        else if (k == "manual_refresh_immediate") c.manualRefreshImmediate = toBool(v);
        else if (k == "top_bumps") c.topBumps = toBool(v);
        else if (k == "bump_count") c.bumpCount = (int)(d < 0 ? 0 : (d > 40 ? 40 : d));
        else if (k == "bump_fill_count") c.bumpFillCount = (int)(d < 0 ? 0 : (d > 10 ? 10 : d));
        else if (k == "bump_max") c.bumpMax = (int)(d < 0 ? 0 : (d > 800 ? 800 : d));
        else if (k == "bump_height_m") c.bumpHeightM = d < 50 ? 50 : (d > 1500 ? 1500 : d);
        else if (k == "bump_radius_factor") c.bumpRadiusFactor = d < 0.05 ? 0.05 : (d > 0.8 ? 0.8 : d);
        else if (k == "vis_check_s") c.visCheckS = (int)(d < 1 ? 1 : d);
        else if (k == "radar") c.radar = toBool(v);
        else if (k == "radar_global") c.radarGlobal = toBool(v);
        else if (k == "radar_range_nm") c.radarRangeNm = d < 30 ? 30 : (d > 300 ? 300 : d);
        else if (k == "radar_cell_nm") c.radarCellNm = d < 3 ? 3 : (d > 30 ? 30 : d);
        else if (k == "radar_max_cells") c.radarMaxCells = (int)(d < 10 ? 10 : (d > 400 ? 400 : d));
        else if (k == "radar_min_dbz") c.radarMinDbz = d < 5 ? 5 : (d > 45 ? 45 : d);
        else if (k == "radar_rain_full_mmh") c.radarRainFullMmH = d < 5 ? 5 : d;
        else if (k == "max_turbulence") c.maxTurbulence = d < 0 ? 0 : (d > 1 ? 1 : d);
        else if (k == "turb_reports") c.turbReports = toBool(v);
        else if (k == "realistic_turbulence") { c.realisticTurbulence = toBool(v); sawRealistic = true; }
        else if (k == "radar_echo_tops") c.radarEchoTops = toBool(v);
        else if (k == "storm_coverage_max") c.stormCoverageMax = d < 0.5 ? 0.5 : (d > 1 ? 1 : d);
        else if (k == "first_load_immediate") c.firstLoadImmediate = toBool(v);
        else if (k == "kept_airports_max") c.keptAirportsMax = (int)(d < 0 ? 0 : (d > 5000 ? 5000 : d));
        else if (k == "apply_frame_ms") c.applyFrameMs = d < 0 ? 0 : (d > 100 ? 100 : d);
        else if (k == "apply_per_tick") c.applyPerTick = (int)(d < 1 ? 1 : (d > 1000 ? 1000 : d));
        else if (k == "fill_gaps") c.fillGaps = toBool(v);
        else if (k == "fill_range_nm") c.fillRangeNm = (int)(d < 10 ? 10 : (d > 150 ? 150 : d));
        else if (k == "fill_cell_nm") c.fillCellNm = (int)(d < 6 ? 6 : (d > 30 ? 30 : d));
        else if (k == "fill_far_range_nm") c.fillFarRangeNm = (int)(d < 20 ? 20 : (d > 250 ? 250 : d));
        else if (k == "fill_far_cell_nm") c.fillFarCellNm = (int)(d < 12 ? 12 : (d > 60 ? 60 : d));
        else if (k == "stratus_cloud_type") c.stratusCloudType = d < 1.0 ? 1.0 : (d > 2.0 ? 2.0 : d);
        else if (k == "model_cirrus") c.modelCirrus = toBool(v);
        else if (k == "model_clouds") c.modelClouds = toBool(v);
        else if (k == "ocean_model_weather") c.oceanModelWeather = toBool(v);
        else if (k == "low_cloud_type") c.lowCloudType = d < 1.0 ? 1.0 : (d > 3.0 ? 3.0 : d);
        else if (k == "storm_top_variation_m") c.stormTopVariationM = d < 0 ? 0 : (d > 4000 ? 4000 : d);
        else if (k == "startup_calm_s") c.startupCalmS = d < 0 ? 0 : (d > 30 ? 30 : d);
        else if (k == "regen_wait_s") c.regenWaitS = d < 0.5 ? 0.5 : (d > 30 ? 30 : d);
        else if (k == "regen_weather_on_start") c.regenWeatherOnStart = toBool(v);
        else if (k == "radar_minutes") c.radarMinutes = d < 0.1 ? 0.1 : (d > 120 ? 120 : d);
        else if (k == "storm_smooth_tops") c.stormSmoothTops = toBool(v);
        else if (k == "station_radius_factor") c.stationRadiusFactor = d < 0.3 ? 0.3 : (d > 1.2 ? 1.2 : d);
        else if (k == "storm_top_step_m") c.stormTopStepM = d < 50 ? 50 : (d > 1000 ? 1000 : d);
        else if (k == "storm_top_blend") c.stormTopBlend = d < 0 ? 0 : (d > 0.8 ? 0.8 : d);
        else if (k == "storm_top_variation_known_m") c.stormTopVariationKnownM = d < 0 ? 0 : (d > 1000 ? 1000 : d);
        else if (k == "storm_rim_rings") c.stormRimRings = (int)(d < 1 ? 1 : (d > 3 ? 3 : d));
        else if (k == "storm_replaces_fill") c.stormReplacesFill = toBool(v);
        else if (k == "storm_fill_pits") c.stormFillPits = toBool(v);
        else if (k == "storm_rim") c.stormRim = toBool(v);
        else if (k == "storm_contour") c.stormContour = toBool(v);
        else if (k == "storm_level_contours") c.stormLevelContours = toBool(v);
        else if (k == "radar_natural") c.radarNatural = toBool(v);
        else if (k == "smooth_pressure") c.smoothPressure = toBool(v);
        else if (k == "region_winds") c.regionWinds = toBool(v);
        else if (k == "dry_runway_min") c.dryRunwayMin = d < 15 ? 15 : (d > 180 ? 180 : d);
        else if (k == "windshear") c.windshear = toBool(v);
        else if (k == "windshear_seed") c.windshearSeed = (int)d;
        else if (k == "windshear_gain") c.windshearGain = d < 0.5 ? 0.5 : (d > 3 ? 3 : d);
        else if (k == "windshear_lead_s") c.windshearLeadS = d < 0 ? 0 : (d > 600 ? 600 : d);
        else if (k == "diag_verbosity") c.diagVerbosity = (int)(d < 0 ? 0 : (d > 2 ? 2 : d));
        else if (k == "storm_clouds_at_stations") c.stormCloudsAtStations = toBool(v);
        else if (k == "qnh_as_sea_level") c.qnhAsSeaLevel = toBool(v);
        else if (k == "fill_qnh_at_sea_level") c.fillQnhAtSeaLevel = toBool(v);
        else if (k == "cloud_smooth_nm") c.cloudSmoothNm = d < 0 ? 0 : (d > 30 ? 30 : d);
        else if (k == "empty_layer_coverage") c.emptyLayerCoverage = d < 0 ? 0 : (d > 0.1 ? 0.1 : d);
        else if (k == "radar_natural_max") c.radarNaturalMax = (int)(d < 30 ? 30 : (d > 400 ? 400 : d));
        else if (k == "radar_natural_fine_nm") c.radarNaturalFineNm = d < 1.5 ? 1.5 : (d > 12 ? 12 : d);
        else if (k == "storm_level_delay_s") c.stormLevelDelayS = (int)(d < 0 ? 0 : (d > 600 ? 600 : d));
        else if (k == "fill_stagger") c.fillStagger = toBool(v);
        else if (k == "storm_top_max_drop_m") c.stormTopMaxDropM = d < 0 ? 0 : (d > 20000 ? 20000 : d);
        else if (k == "storm_core_coverage") c.stormCoreCoverage = d < 0.5 ? 0.5 : (d > 1 ? 1 : d);
        else if (k == "storm_base_smooth") c.stormBaseSmooth = toBool(v);
        else if (k == "storm_centroid") c.stormCentroid = toBool(v);
        else if (k == "storm_rain_blend") c.stormRainBlend = d < 0 ? 0 : (d > 0.8 ? 0.8 : d);
        else if (k == "storm_fill_holes") c.stormFillHoles = toBool(v);
        else if (k == "storm_radius_factor") c.stormRadiusFactor = d < 0.6 ? 0.6 : (d > 2.0 ? 2.0 : d);
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
    if (version < 26) { Config d; c.regionWinds = d.regionWinds; }
    if (version < 30) { c.maxTurbulence = 1.0; if (!sawRealistic) c.realisticTurbulence = true; }   // v30: the storm slider (0-200 %) became Max turbulence (0-1)
    if (version < 29) c.windshear = true;   // v29: windshear on by default (turned on once; turning it off afterwards sticks)
    if (version < 28 && std::fabs(c.windshearGain - 1.4) < 1e-6) { Config d; c.windshearGain = d.windshearGain; }   // v28: 1.4 left ~40 % of the outflow
    if (version < 27 && c.radarNaturalMax == 150) { Config d; c.radarNaturalMax = d.radarNaturalMax; }   // v27: 150 left widespread rain in 24 nm squares   // v26: one build saved region_winds = 0, which left the winds aloft out of the sim
    if (version < 25) { Config d; c.stormRadiusFactor = d.stormRadiusFactor; c.stormCoreCoverage = d.stormCoreCoverage; c.stormTopMaxDropM = d.stormTopMaxDropM; }   // v25: back to the look of the earlier build (ragged walls)
    if (version < 24) { Config d; c.stormRadiusFactor = d.stormRadiusFactor; }   // v24: wider overlap
    if (version < 23) { Config d; c.stormCoverageMax = d.stormCoverageMax; }   // v23: back to 95 % (the 100 % trial removed the natural breakup)
    if (version < 22) {   // the start-up defaults changed: take the new ones once, keep everything else as the user set it
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
        // The plugin writes every setting to the file on start-up, so values from earlier test builds stick even after the default
        // changes (an old low_cloud_type = 2 kept the layers at type 2, an old radius factor of 0.65 kept the record overlap).
        c.startupCalmS = d.startupCalmS;
        c.topBumps = d.topBumps; c.bumpCount = d.bumpCount; c.bumpFillCount = d.bumpFillCount; c.bumpMax = d.bumpMax; c.bumpHeightM = d.bumpHeightM; c.bumpRadiusFactor = d.bumpRadiusFactor;
        c.regenWaitS = d.regenWaitS;
        c.lowCloudType = d.lowCloudType;
        c.stationRadiusFactor = d.stationRadiusFactor;
        c.fillRadiusFactor = d.fillRadiusFactor;
        c.stormRadiusFactor = d.stormRadiusFactor;
        c.radarCellNm = d.radarCellNm; c.radarMaxCells = d.radarMaxCells;   // storm block size back to 12 nm (v22)
        c.stormTopStepM = d.stormTopStepM; c.stormTopBlend = d.stormTopBlend; c.stormRainBlend = d.stormRainBlend; c.stormCentroid = d.stormCentroid; c.stormRim = d.stormRim; c.stormRimRings = d.stormRimRings;
        c.stormSmoothTops = d.stormSmoothTops;
        c.stormTopVariationM = d.stormTopVariationM;
        c.radarMinutes = d.radarMinutes;
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
      << "upper_air = " << (c.upperAir ? 1 : 0) << "\n"
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
      << "manual_refresh_immediate = " << (c.manualRefreshImmediate ? 1 : 0) << "\n"
      << "top_bumps = " << (c.topBumps ? 1 : 0) << "\n"
      << "bump_count = " << c.bumpCount << "\n"
      << "bump_max = " << c.bumpMax << "\n"
      << "bump_fill_count = " << c.bumpFillCount << "\n"
      << "bump_height_m = " << c.bumpHeightM << "\n"
      << "bump_radius_factor = " << c.bumpRadiusFactor << "\n"
      << "vis_check_s = " << c.visCheckS << "\n"
      << "radar = " << (c.radar ? 1 : 0) << "\n"
      << "radar_global = " << (c.radarGlobal ? 1 : 0) << "\n"
      << "radar_range_nm = " << c.radarRangeNm << "\n"
      << "radar_cell_nm = " << c.radarCellNm << "\n"
      << "radar_max_cells = " << c.radarMaxCells << "\n"
      << "radar_min_dbz = " << c.radarMinDbz << "\n"
      << "radar_rain_full_mmh = " << c.radarRainFullMmH << "\n"
      << "max_turbulence = " << c.maxTurbulence << "\n"
      << "realistic_turbulence = " << (c.realisticTurbulence ? 1 : 0) << "\n"
      << "turb_reports = " << (c.turbReports ? 1 : 0) << "\n"
      << "radar_echo_tops = " << (c.radarEchoTops ? 1 : 0) << "\n"
      << "storm_coverage_max = " << c.stormCoverageMax << "\n"
      << "storm_level_contours = " << (c.stormLevelContours ? 1 : 0) << "\n"
      << "radar_natural = " << (c.radarNatural ? 1 : 0) << "\n"
      << "smooth_pressure = " << (c.smoothPressure ? 1 : 0) << "\n"
      << "region_winds = " << (c.regionWinds ? 1 : 0) << "\n"
      << "dry_runway_min = " << c.dryRunwayMin << "\n"
      << "windshear = " << (c.windshear ? 1 : 0) << "\n"
      << "windshear_seed = " << c.windshearSeed << "\n"
      << "windshear_gain = " << c.windshearGain << "\n"
      << "windshear_lead_s = " << c.windshearLeadS << "\n"
      << "diag_verbosity = " << c.diagVerbosity << "\n"
      << "storm_clouds_at_stations = " << (c.stormCloudsAtStations ? 1 : 0) << "\n"
      << "qnh_as_sea_level = " << (c.qnhAsSeaLevel ? 1 : 0) << "\n"
      << "fill_qnh_at_sea_level = " << (c.fillQnhAtSeaLevel ? 1 : 0) << "\n"
      << "stratus_cloud_type = " << c.stratusCloudType << "\n"
      << "model_cirrus = " << (c.modelCirrus ? 1 : 0) << "\n"
      << "model_clouds = " << (c.modelClouds ? 1 : 0) << "\n"
      << "ocean_model_weather = " << (c.oceanModelWeather ? 1 : 0) << "\n"
      << "cloud_smooth_nm = " << c.cloudSmoothNm << "\n"
      << "empty_layer_coverage = " << c.emptyLayerCoverage << "\n"
      << "radar_natural_max = " << c.radarNaturalMax << "\n"
      << "radar_natural_fine_nm = " << c.radarNaturalFineNm << "\n"
      << "fill_gaps = " << (c.fillGaps ? 1 : 0) << "\n"
      << "fill_stagger = " << (c.fillStagger ? 1 : 0) << "\n"
      << "fill_range_nm = " << c.fillRangeNm << "\n"
      << "fill_cell_nm = " << c.fillCellNm << "\n"
      << "fill_far_range_nm = " << c.fillFarRangeNm << "\n"
      << "fill_far_cell_nm = " << c.fillFarCellNm << "\n"
      << "regen_weather_on_start = " << (c.regenWeatherOnStart ? 1 : 0) << "\n"
      << "radar_minutes = " << c.radarMinutes << "\n"
      << "startup_delay_s = " << c.startupDelayS << "\n";
    return true;
}

} // namespace wx
