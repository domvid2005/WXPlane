// WXPlane - X-Plane 12 plugin entry point (XPLM SDK 4.2.0 / X-Plane 12.3+).
//
// Flow: flight loop (1 Hz, before flight model) -> asks the worker thread for NOAA METAR/ATIS data around the
// aircraft every N minutes -> applies the result with XPLMBeginWeatherUpdate/XPLMSetWeatherAt*/XPLMEndWeatherUpdate.

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "XPLMDataAccess.h"
#include "XPLMDefs.h"
#include "XPLMMenus.h"
#include "XPLMNavigation.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "XPLMWeather.h"

#include "config.h"
#include "fetcher.h"
#include "geo.h"
#include "http.h"
#include "wxmodel.h"

namespace {

using Clock = std::chrono::steady_clock;

enum MenuId { kEnabled = 1, kRefresh, kAtis, kAloft };

wx::Config g_cfg;
wx::Fetcher g_fetcher;
std::string g_cfgPath;

XPLMMenuID g_menu = nullptr;
int g_idxEnabled = -1, g_idxRefresh = -1, g_idxAtis = -1, g_idxAloft = -1, g_idxStatus1 = -1, g_idxStatus2 = -1;
XPLMFlightLoopID g_loop = nullptr;

XPLMDataRef g_drLat = nullptr, g_drLon = nullptr, g_drLevels = nullptr;
float g_levelsM[XPLM_NUM_TEMPERATURE_LAYERS];
bool g_haveLevels = false;

bool g_pluginEnabled = false;
wx::FetchResult g_lastRes;      // last applied data, kept so a visibility correction can re-apply without a download
bool g_haveLast = false;
float g_visK = 1.0f;            // learned correction between what we send and what X-Plane ends up using
bool g_checkPending = false;
Clock::time_point g_checkAt{};
int g_calIter = 0;
bool g_manualRefresh = false;
bool g_eraseRequested = false;
bool g_firstApplyDone = false;
bool g_haveCenter = false;
double g_centerLat = 0, g_centerLon = 0;
Clock::time_point g_lastRequest{};
bool g_everRequested = false;
std::string g_lastGoodStatus;

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string line = std::string("[WXPlane] ") + buf + "\n";
    XPLMDebugString(line.c_str());
}

std::string configPath() {
    char prefs[1024] = {0};
    XPLMGetPrefsPath(prefs);       // .../Output/preferences/X-Plane.prf
    XPLMExtractFileAndPath(prefs); // strips the file name, leaves the directory
    return std::string(prefs) + XPLMGetDirectorySeparator() + "WXPlane.cfg";
}

void setStatus(const std::string& l1, const std::string& l2) {
    if (!g_menu) return;
    XPLMSetMenuItemName(g_menu, g_idxStatus1, l1.c_str(), 0);
    XPLMSetMenuItemName(g_menu, g_idxStatus2, l2.c_str(), 0);
}

void syncMenu() {
    if (!g_menu) return;
    XPLMCheckMenuItem(g_menu, g_idxEnabled, g_cfg.enabled ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g_menu, g_idxAtis, g_cfg.useAtis ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g_menu, g_idxAloft, g_cfg.useAloft ? xplm_Menu_Checked : xplm_Menu_Unchecked);
}

// Removes every record this plugin has set, handing control back to X-Plane's own weather.
void eraseAll() {
    XPLMBeginWeatherUpdate();
    XPLMEndWeatherUpdate(0 /*not incremental: wipes our previous records*/, 1 /*immediately*/);
    logf("plugin weather records removed");
}

bool airportKnown(const wx::Station& s) {
    XPLMNavRef ref = XPLMFindNavAid(nullptr, s.icao.c_str(), nullptr, nullptr, nullptr, xplm_Nav_Airport);
    if (ref == XPLM_NAV_NOT_FOUND) return false;
    char id[32] = {0};
    float la = 0, lo = 0;
    XPLMGetNavAidInfo(ref, nullptr, &la, &lo, nullptr, nullptr, nullptr, id, nullptr, nullptr);
    return s.icao == id && wx::distNm(la, lo, s.lat, s.lon) < 5.0;
}

void fillInfo(XPLMWeatherInfo_t& info, const wx::XpWx& x) {
    std::memset(&info, 0, sizeof info);
    info.structSize = sizeof(XPLMWeatherInfo_t);
    const float kUnset = -300.0f; // anything below -273.5 degC means "not provided"

    info.temperature_alt = x.hasTemp ? x.tempC : kUnset;
    info.dewpoint_alt = x.hasDewp ? x.dewpC : kUnset;
    info.pressure_alt = x.qnhPa; // QNH; 0 = fall back to pressure_sl
    info.pressure_sl = 0.0f;
    info.visibility = x.visM;
    info.precip_rate = x.precip;
    info.wave_height = x.waveHeightM;
    info.wave_dir = x.waveDir;
    info.thermal_climb = 0.0f;

    for (int i = 0; i < XPLM_NUM_WIND_LAYERS; ++i) {
        info.wind_layers[i].alt_msl = 0.0f;
        info.wind_layers[i].speed = XPLM_WIND_UNDEFINED_LAYER; // undefined layers fall back to X-Plane's own data
    }
    for (size_t i = 0; i < x.winds.size() && i < (size_t)XPLM_NUM_WIND_LAYERS; ++i) {
        XPLMWeatherInfoWinds_t& w = info.wind_layers[i];
        w.alt_msl = x.winds[i].altMsl;
        w.speed = x.winds[i].ms;
        w.direction = x.winds[i].dir;
        w.gust_speed = x.winds[i].gustMs;
        w.shear = x.winds[i].shearDeg;
        w.turbulence = x.winds[i].turbulence;
    }
    for (int i = 0; i < XPLM_NUM_CLOUD_LAYERS; ++i) {
        // A layer with coverage 0 is removed, which is what we want when the METAR reports clear skies.
        XPLMWeatherInfoClouds_t& c = info.cloud_layers[i];
        if (i < x.cloudCount) {
            c.cloud_type = x.clouds[i].type;
            c.coverage = x.clouds[i].coverage;
            c.alt_base = x.clouds[i].baseM;
            c.alt_top = x.clouds[i].topM;
        } else {
            c.cloud_type = 0; c.coverage = 0; c.alt_base = 0; c.alt_top = 0;
        }
    }
    for (int i = 0; i < XPLM_NUM_TEMPERATURE_LAYERS; ++i) {
        info.temp_layers[i] = kUnset;
        info.dewp_layers[i] = kUnset;
    }
    if (g_haveLevels && x.tempProfile.size() >= 2) {
        // Temperatures aloft: sampled at X-Plane's own fixed temperature levels. The surface value is part of the
        // profile, so the single surface offset is not used as well.
        for (int i = 0; i < XPLM_NUM_TEMPERATURE_LAYERS; ++i)
            info.temp_layers[i] = wx::profileTemp(x.tempProfile, g_levelsM[i]);
        info.temperature_alt = kUnset;
    }
    info.troposphere_alt = 0.0f;   // derive from existing data
    info.troposphere_temp = kUnset;
    info.age = x.ageS;
    info.radius_nm = (float)g_cfg.radiusNm;
    info.max_altitude_msl_ft = x.maxAltFt;
}

void applyResult(const wx::FetchResult& res, bool fromFetch) {
    std::time_t now = std::time(nullptr);
    int fogCount = 0, aloftCount = 0;
    g_haveLevels = false;
    if (g_drLevels && XPLMGetDatavf(g_drLevels, nullptr, 0, 0) == XPLM_NUM_TEMPERATURE_LAYERS)
        g_haveLevels = XPLMGetDatavf(g_drLevels, g_levelsM, 0, XPLM_NUM_TEMPERATURE_LAYERS) == XPLM_NUM_TEMPERATURE_LAYERS;
    XPLMBeginWeatherUpdate();
    for (const auto& s : res.stations) {
        wx::XpWx x = wx::buildXp(s, g_cfg, now, &res.aloft);
        if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK); // g_visK stays 1 unless the read-back check learned otherwise
        XPLMWeatherInfo_t info;
        fillInfo(info, x);
        if (x.fogLayerApplied) ++fogCount;
        if (x.aloft) ++aloftCount;
        if (airportKnown(s)) XPLMSetWeatherAtAirport(s.icao.c_str(), &info);
        else XPLMSetWeatherAtLocation(s.lat, s.lon, s.elevM, &info);
    }
    // Non-incremental: our previous set is replaced wholesale, so stale stations never linger.
    XPLMEndWeatherUpdate(0, (res.immediate || !g_firstApplyDone) ? 1 : 0);
    g_firstApplyDone = true;
    g_lastRes = res;
    g_haveLast = true;
    if (fromFetch) g_calIter = 0;
    g_checkPending = g_cfg.visAutocal && !res.stations.empty();
    g_checkAt = Clock::now() + std::chrono::seconds(res.immediate ? std::min(15, g_cfg.visCheckS) : g_cfg.visCheckS);

    char t[16];
    std::tm g;
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    std::strftime(t, sizeof t, "%H:%MZ", &g);

    char l1[160], l2[160];
    std::string fogNote = fogCount ? ", fog at " + std::to_string(fogCount) : "";
    std::snprintf(l1, sizeof l1, "Updated %s: %d METAR, %d ATIS%s", t, res.metarCount, res.atisCount, fogNote.c_str());
    std::string aloftNote = g_cfg.useAloft ? (aloftCount ? "Winds aloft: " + std::to_string(res.aloft.size()) + " NOAA stations"
                                                          : "Winds aloft: " + (res.aloftNote.empty() ? std::string("none") : res.aloftNote))
                                           : "Winds aloft: off";
    if (!res.stations.empty())
        std::snprintf(l2, sizeof l2, "%s | nearest %s %.0f nm", aloftNote.c_str(), res.stations[0].icao.c_str(), res.stations[0].distNm);
    else
        std::snprintf(l2, sizeof l2, "No stations in range");
    g_lastGoodStatus = l1;
    setStatus(l1, l2);
    logf("applied %d stations (%d ATIS, %d fog, %d with winds/temps aloft from %zu FD stations) around %.2f,%.2f%s",
         res.metarCount, res.atisCount, fogCount, aloftCount, res.aloft.size(), res.centerLat, res.centerLon,
         res.immediate ? " [immediate]" : "");
    if (g_cfg.useAloft && !res.aloftNote.empty()) logf("%s", res.aloftNote.c_str());
    for (const auto& s : res.stations) {
        if (s.hasAtis) logf("  %s ATIS %s: %s", s.icao.c_str(), s.atisLetter.c_str(), s.atisText.substr(0, 110).c_str());
    }
}

// Asks X-Plane what visibility it is actually using next to the airport we are at, and compares it with the
// METAR/ATIS. If the sim does not honour our value (blending with its own data), correct our input and re-apply.
void visibilityCheck() {
    if (!g_haveLast || g_lastRes.stations.empty()) return;
    double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
    const wx::Station* best = nullptr;
    double bestD = 1e9;
    for (const auto& s : g_lastRes.stations) {
        double d = wx::distNm(lat, lon, s.lat, s.lon);
        if (d < bestD) { bestD = d; best = &s; }
    }
    if (!best || bestD > 15.0) return; // only meaningful close to a station
    wx::XpWx x = wx::buildXp(*best, g_cfg, std::time(nullptr), &g_lastRes.aloft);
    XPLMWeatherInfo_t info;
    std::memset(&info, 0, sizeof info);
    info.structSize = sizeof info;
    int detailed = XPLMGetWeatherAtLocation(best->lat, best->lon, best->elevM + 5.0, &info);
    logf("visibility check %s (%.0f nm away): report says %.0f m, we sent %.0f m, X-Plane is using %.0f m (correction %.2f, detailed=%d)",
         best->icao.c_str(), bestD, x.visM, std::max(50.0f, x.visM < 20000.0f ? x.visM * g_visK : x.visM), info.visibility, g_visK, detailed);
    if (!g_cfg.visAutocal || x.visM >= 20000.0f || info.visibility < 100.0f || g_calIter >= 4) return;
    double ratio = x.visM / info.visibility;
    if (std::fabs(ratio - 1.0) <= 0.15) return;
    g_visK = (float)std::min(1.5, std::max(0.15, g_visK * std::pow(ratio, 0.8)));
    ++g_calIter;
    logf("visibility off by %.0f%%: correction now %.2f, re-applying (pass %d/4)", (ratio - 1.0) * 100.0, g_visK, g_calIter);
    wx::FetchResult copy = g_lastRes;
    copy.immediate = true;
    applyResult(copy, false);
}

float flightLoop(float, float, int, void*) {
    if (g_checkPending && Clock::now() >= g_checkAt) {
        g_checkPending = false;
        visibilityCheck();
    }

    // 1) results from the worker thread
    wx::FetchResult res;
    if (g_fetcher.take(res)) {
        if (!res.ok) {
            logf("update failed: %s", res.error.c_str());
            setStatus("Update failed (will retry)", res.error.substr(0, 60));
        } else if (g_cfg.enabled) {
            if (!res.error.empty()) logf("%s", res.error.c_str());
            applyResult(res, true);
        }
    }

    // 2) switched off from the menu -> hand weather back to X-Plane
    if (g_eraseRequested) {
        g_eraseRequested = false;
        g_checkPending = false;
        eraseAll();
        g_haveCenter = false;
        setStatus("Disabled - X-Plane weather in control", "");
    }

    // 3) schedule the next download
    if (g_cfg.enabled && !g_fetcher.busy()) {
        double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
        bool valid = std::fabs(lat) > 0.001 || std::fabs(lon) > 0.001;
        if (valid) {
            auto now = Clock::now();
            double since = g_everRequested ? std::chrono::duration<double>(now - g_lastRequest).count() : 1e9;
            bool due = since >= g_cfg.updateMinutes * 60.0;
            bool moved = g_haveCenter && since >= 60.0 && wx::distNm(lat, lon, g_centerLat, g_centerLon) > g_cfg.refetchMoveNm;
            if (due || moved || g_manualRefresh) {
                wx::FetchRequest rq;
                rq.lat = lat; rq.lon = lon; rq.cfg = g_cfg;
                rq.immediate = g_manualRefresh || !g_firstApplyDone;
                g_fetcher.request(rq);
                g_lastRequest = now;
                g_everRequested = true;
                g_centerLat = lat; g_centerLon = lon; g_haveCenter = true;
                g_manualRefresh = false;
                if (since > 1e8 || rq.immediate) setStatus("Downloading NOAA weather...", "");
            }
        }
    }
    return 1.0f;
}

void menuHandler(void*, void* item) {
    switch ((int)(intptr_t)item) {
        case kEnabled:
            g_cfg.enabled = !g_cfg.enabled;
            if (g_cfg.enabled) { g_manualRefresh = true; setStatus("Enabled - downloading...", ""); }
            else g_eraseRequested = true;
            break;
        case kRefresh:
            if (!g_cfg.enabled) { g_cfg.enabled = true; }
            g_manualRefresh = true;
            setStatus("Refresh requested...", "");
            break;
        case kAtis:
            g_cfg.useAtis = !g_cfg.useAtis;
            if (g_cfg.enabled) g_manualRefresh = true;
            break;
        case kAloft:
            g_cfg.useAloft = !g_cfg.useAloft;
            if (g_cfg.enabled) g_manualRefresh = true;
            break;
    }
    syncMenu();
    wx::saveConfig(g_cfgPath, g_cfg);
}

} // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strcpy(outName, "WXPlane");
    std::strcpy(outSig, "com.wxplane.xp12.noaa");
    std::strcpy(outDesc, "Replaces flawed X-Plane 12 weather with NOAA METAR and ATIS data, refreshed every few minutes.");

    g_cfgPath = configPath();
    wx::loadConfig(g_cfgPath, g_cfg);
    wx::saveConfig(g_cfgPath, g_cfg); // writes defaults on first run so the file can be edited

    g_drLat = XPLMFindDataRef("sim/flightmodel/position/latitude");
    g_drLon = XPLMFindDataRef("sim/flightmodel/position/longitude");
    g_drLevels = XPLMFindDataRef("sim/weather/region/atmosphere_alt_levels_m");

    int sub = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "WXPlane", nullptr, 0);
    g_menu = XPLMCreateMenu("WXPlane", XPLMFindPluginsMenu(), sub, menuHandler, nullptr);
    g_idxEnabled = XPLMAppendMenuItem(g_menu, "Enabled", (void*)(intptr_t)kEnabled, 0);
    g_idxRefresh = XPLMAppendMenuItem(g_menu, "Refresh Weather Data Now", (void*)(intptr_t)kRefresh, 0);
    XPLMAppendMenuSeparator(g_menu);
    g_idxAtis = XPLMAppendMenuItem(g_menu, "Use ATIS", (void*)(intptr_t)kAtis, 0);
    g_idxAloft = XPLMAppendMenuItem(g_menu, "Use NOAA Winds/Temps Aloft", (void*)(intptr_t)kAloft, 0);
    XPLMAppendMenuSeparator(g_menu);
    g_idxStatus1 = XPLMAppendMenuItem(g_menu, "Starting...", nullptr, 0);
    g_idxStatus2 = XPLMAppendMenuItem(g_menu, "", nullptr, 0);
    XPLMEnableMenuItem(g_menu, g_idxStatus1, 0);
    XPLMEnableMenuItem(g_menu, g_idxStatus2, 0);
    syncMenu();
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    if (g_menu) { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API int XPluginEnable(void) {
    wx::httpGlobalInit();
    g_fetcher.start();

    XPLMCreateFlightLoop_t p;
    std::memset(&p, 0, sizeof p);
    p.structSize = sizeof p;
    p.phase = xplm_FlightLoop_Phase_BeforeFlightModel; // the weather API must be used before the flight model
    p.callbackFunc = flightLoop;
    p.refcon = nullptr;
    g_loop = XPLMCreateFlightLoop(&p);
    XPLMScheduleFlightLoop(g_loop, 2.0f, 1);

    g_pluginEnabled = true;
    g_everRequested = false;
    g_firstApplyDone = false;
    setStatus(g_cfg.enabled ? "Waiting for sim..." : "Disabled", "");
    logf("enabled (update every %d min, search %.0f nm)", g_cfg.updateMinutes, g_cfg.searchNm);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    g_pluginEnabled = false;
    if (g_loop) { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    g_fetcher.stop();
    wx::httpGlobalCleanup();
    // Give weather back to X-Plane so our fog/cloud records don't outlive the plugin.
    eraseAll();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
