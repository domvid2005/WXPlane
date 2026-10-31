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
#include <map>
#include <ctime>
#include <string>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMDefs.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMap.h"
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
#include "radar.h"
#include "viewer.h"
#include "wxmodel.h"

namespace {

using Clock = std::chrono::steady_clock;

enum MenuId { kEnabled = 1, kRefresh, kAtis, kAloft, kShow, kRadar, kRadarGlobal, kImmediate };

wx::Config g_cfg;
wx::Fetcher g_fetcher;
std::string g_cfgPath;

XPLMMenuID g_menu = nullptr;
int g_idxEnabled = -1, g_idxRefresh = -1, g_idxShow = -1, g_idxAtis = -1, g_idxAloft = -1, g_idxRadar = -1, g_idxRadarGlobal = -1, g_idxImmediate = -1, g_idxStatus1 = -1, g_idxStatus2 = -1;
XPLMFlightLoopID g_loop = nullptr;

bool g_radarDeferred = false;                 // first update went out without radar; storms are still to come
Clock::time_point g_firstApplyAt{};
Clock::time_point g_lastApplyLogAt{};
XPLMDataRef g_drFramePeriod = nullptr;
XPLMDataRef g_drLat = nullptr, g_drLon = nullptr, g_drLevels = nullptr, g_drAgl = nullptr, g_drLoading = nullptr;
float g_levelsM[XPLM_NUM_TEMPERATURE_LAYERS];
bool g_haveLevels = false;

bool g_pluginEnabled = false;
unsigned g_viewVersion = 1;     // bumped whenever new data is applied (METAR/ATIS window re-wraps on change)
wx::FetchResult g_lastRes;      // last applied data, kept so a visibility correction can re-apply without a download
bool g_haveLast = false;
float g_visK = 1.0f;            // factor between the METAR visibility and what we send (vis_scale; the optional self-check adjusts it)
bool g_checkPending = false;
Clock::time_point g_checkAt{};
int g_calIter = 0;
int g_checkTries = 0;
float g_prevReadVis = 0;
float g_visBeforeCorr = 0;
bool g_manualRefresh = false;
bool g_eraseRequested = false;
bool g_firstApplyDone = false;
std::time_t g_fogUntil = 0;
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
    XPLMCheckMenuItem(g_menu, g_idxRadar, g_cfg.radar ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g_menu, g_idxRadarGlobal, g_cfg.radarGlobal ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g_menu, g_idxImmediate, g_cfg.applyImmediately ? xplm_Menu_Checked : xplm_Menu_Unchecked);
}

// Removes every record this plugin has set, handing control back to X-Plane's own weather.
struct Rec {
    XPLMWeatherInfo_t info;   // as sent, with age zeroed so unchanged data compares equal
    bool airport = false;
    std::string icao;
    double lat = 0, lon = 0, elev = 0;
};
std::map<std::string, Rec> g_recs; // what we have currently set in the sim, by station

// Records are handed to X-Plane a few at a time (nearest stations first, then storms) instead of all at once: every
// weather update makes the sim rebuild what changed, and doing 100 of them in one frame is what shows up as a stutter.
bool g_pendingImmediate = false;
bool g_startupPhase = true;        // true until the start-up updates (stations, then storms) are out
bool g_pendingStartup = false;
bool g_pendingManual = false;       // the pending update was asked for with "Refresh Weather Data Now"
Clock::time_point g_nextRelease{};   // when the next queued record may be handed to X-Plane
double g_releaseStepS = 0.0;         // seconds between records, so a whole update spreads over transition_s
struct Todo { std::string key; Rec rec; };
std::vector<Todo> g_pendingTodo;
std::vector<std::string> g_pendingGone;

void sendRec(Todo& t) {
    if (t.rec.airport) XPLMSetWeatherAtAirport(t.rec.icao.c_str(), &t.rec.info);
    else XPLMSetWeatherAtLocation(t.rec.lat, t.rec.lon, t.rec.elev, &t.rec.info);
    t.rec.info.age = 0.0f;
    g_recs[t.key] = t.rec;
}
void eraseRec(const std::string& k) {
    auto it = g_recs.find(k);
    if (it == g_recs.end()) return;
    if (it->second.airport) XPLMEraseWeatherAtAirport(it->second.icao.c_str());
    else XPLMEraseWeatherAtLocation(it->second.lat, it->second.lon);
    g_recs.erase(it);
}
void flushPending() {
    if (g_pendingTodo.empty() && g_pendingGone.empty()) return;
    auto t0 = Clock::now();
    size_t budget = (size_t)g_cfg.applyPerTick, sent = 0, erased = 0;
    if (g_releaseStepS > 0.0) {
        if (t0 < g_nextRelease) return;
        double behind = std::chrono::duration<double>(t0 - g_nextRelease).count();
        budget = std::min(budget, (size_t)(behind / g_releaseStepS) + 1);
        g_nextRelease = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(g_releaseStepS * (double)budget));
    }
    XPLMBeginWeatherUpdate();
    while (budget > 0 && !g_pendingGone.empty()) { eraseRec(g_pendingGone.back()); g_pendingGone.pop_back(); --budget; ++erased; }
    size_t n = std::min(budget, g_pendingTodo.size());
    for (size_t i = 0; i < n; ++i) sendRec(g_pendingTodo[i]);
    sent = n;
    g_pendingTodo.erase(g_pendingTodo.begin(), g_pendingTodo.begin() + (long)n);
    XPLMEndWeatherUpdate(1 /*incremental*/, (g_pendingImmediate && (g_cfg.applyImmediately || g_pendingManual || (g_cfg.firstLoadImmediate && g_pendingStartup))) ? 1 : 0);
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (ms > 20.0) logf("weather update took %.0f ms for %zu records, %zu erased (%zu still queued)", ms, sent, erased, g_pendingTodo.size() + g_pendingGone.size());
    if (g_pendingTodo.empty() && g_pendingGone.empty()) logf("all weather records handed to X-Plane");
}

void eraseAll() {
    g_pendingTodo.clear();
    g_pendingGone.clear();
    XPLMBeginWeatherUpdate();
    XPLMEndWeatherUpdate(0 /*not incremental: wipes our previous records*/, 1 /*immediately*/);
    g_recs.clear();
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
        if (x.clouds[i].coverage > 0) {
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
    // Only stations whose data changed are touched, and stations that left range are erased. Re-setting identical
    // records makes X-Plane rebuild their cloud layers, which shows up as hitches, so unchanged ones are left alone.
    const bool full = g_recs.empty() || !g_firstApplyDone;
    g_pendingTodo.clear();   // anything still queued is superseded by this update
    g_pendingGone.clear();
    std::vector<Todo> todo;
    std::map<std::string, bool> seen;
    // The fog slot layout stays on for 30 minutes after the last fog report: flipping it moves every station's clouds
    // to a different slot, and X-Plane rebuilds all of them (a visible reload).
    bool fogNow = false;
    for (const auto& s : res.stations) fogNow = fogNow || wx::stationHasFog(s, g_cfg);
    if (fogNow) g_fogUntil = now + 1800;
    const bool anyFog = now < g_fogUntil;
    std::map<std::string, wx::XpWx> baseWx;   // weather built per station, reused for storm cells
    std::map<std::string, float> stationRadius;
    for (const auto& s : res.stations) {
        wx::XpWx x = wx::buildXp(s, g_cfg, now, &res.aloft, anyFog);
        baseWx[s.icao] = x;
        if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK); // g_visK stays 1 unless the read-back check learned otherwise
        XPLMWeatherInfo_t info;
        fillInfo(info, x);
        if (g_cfg.stationRadiusAuto) {
            // Station records never overlap: each one reaches at most 49% of the distance to its nearest other station, so for any
            // two stations the two radii add up to less than the distance between them. X-Plane then never blends one station's
            // weather into another's, and at a station you get exactly that station's METAR/ATIS.
            double nn = 1e9;
            for (const auto& o : res.stations)
                if (&o != &s) nn = std::min(nn, wx::distNm(s.lat, s.lon, o.lat, o.lon));
            info.radius_nm = (float)std::min(g_cfg.radiusNm, std::max(0.5, nn * 0.49));
        }
        stationRadius[s.icao] = info.radius_nm;
        if (x.fogLayerApplied) ++fogCount;
        if (x.aloft) ++aloftCount;
        Rec r;
        r.airport = airportKnown(s);
        r.icao = s.icao; r.lat = s.lat; r.lon = s.lon; r.elev = s.elevM;
        r.info = info;
        r.info.age = 0.0f;
        seen[s.icao] = true;
        auto it = g_recs.find(s.icao);
        if (!full && it != g_recs.end() && it->second.airport == r.airport && std::memcmp(&it->second.info, &r.info, sizeof r.info) == 0)
            continue; // identical to what the sim already has
        r.info.age = info.age; // keep the real age for the call itself
        todo.push_back({s.icao, r});
    }
    // Gap fill: the station circles do not overlap, so between them X-Plane would fall back to its own weather. Location records
    // on fixed grids cover everything around the aircraft: a fine grid close by and a coarse one further out (up to the METAR
    // search radius). Each record holds a smooth inverse-distance blend of the nearest stations (the closest one dominates), so
    // the weather changes gradually from airport to airport and no two stations compete over the same spot.
    int fillCount = 0;
    if (g_cfg.fillGaps && g_cfg.stationRadiusAuto && !res.stations.empty()) {
        auto addGrid = [&](int cellNm, double rMin, double rMax, const char* tag) {
            const double stepLat = cellNm / 60.0;
            const double rangeDeg = rMax / 60.0;
            const long j0 = (long)std::floor((res.centerLat - rangeDeg) / stepLat), j1 = (long)std::floor((res.centerLat + rangeDeg) / stepLat);
            for (long j = j0; j <= j1; ++j) {
                const double latc = (j + 0.5) * stepLat;
                if (latc < -85 || latc > 85) continue;
                const double cosL = std::max(0.2, std::cos(latc * 3.14159265358979 / 180.0));
                const double stepLon = stepLat / cosL;
                const double rangeLon = rangeDeg / cosL;
                const long i0 = (long)std::floor((res.centerLon - rangeLon) / stepLon), i1 = (long)std::floor((res.centerLon + rangeLon) / stepLon);
                for (long i = i0; i <= i1; ++i) {
                    const double lonc = (i + 0.5) * stepLon;
                    const double dc = wx::distNm(res.centerLat, res.centerLon, latc, lonc);
                    if (dc > rMax || dc <= rMin) continue;
                    const wx::Station* nearest = &res.stations[0];
                    double nd = 1e9;
                    for (const auto& s : res.stations) {
                        double d = wx::distNm(latc, lonc, s.lat, s.lon);
                        if (d < nd) { nd = d; nearest = &s; }
                    }
                    auto rit = stationRadius.find(nearest->icao);
                    if (rit != stationRadius.end() && nd <= rit->second * 0.9) continue;   // inside the station's own circle
                    wx::Station syn = wx::blendAt(res.stations, latc, lonc);
                    wx::XpWx x = wx::buildXp(syn, g_cfg, now, &res.aloft, anyFog);
                    if (x.fogLayerApplied) { x.clouds[0] = wx::XpCloud(); x.fogLayerApplied = false; }
                    if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK);
                    XPLMWeatherInfo_t info;
                    fillInfo(info, x);
                    info.radius_nm = (float)(cellNm * 0.8);   // a little more than half the cell diagonal: tiles without gaps
                    Rec r;
                    r.airport = false;
                    r.lat = latc; r.lon = lonc; r.elev = nearest->elevM;
                    r.info = info;
                    r.info.age = 0.0f;
                    const std::string key = std::string("fill") + tag + ":" + std::to_string(j) + ":" + std::to_string(i);
                    seen[key] = true;
                    ++fillCount;
                    auto it = g_recs.find(key);
                    if (!full && it != g_recs.end() && std::memcmp(&it->second.info, &r.info, sizeof r.info) == 0) continue;
                    r.info.age = info.age;
                    todo.push_back({key, r});
                }
            }
        };
        const double nearR = g_cfg.fillRangeNm;
        const double farR = std::min((double)g_cfg.fillFarRangeNm, g_cfg.searchNm * 1.0);
        addGrid(g_cfg.fillCellNm, -1.0, nearR, "n");
        if (farR > nearR) addGrid(g_cfg.fillFarCellNm, nearR, farR, "f");
    }
    // Radar storm cells: the weather of the nearest station with the storm applied on top, as location records.
    int stormCount = 0;
    if (g_cfg.radar && res.radar.ok && !res.stations.empty()) {
        for (const auto& c : res.radar.cells) {
            const wx::Station* nearest = &res.stations[0];
            double nd = 1e9;
            for (const auto& s : res.stations) {
                double d = wx::distNm(c.lat, c.lon, s.lat, s.lon);
                if (d < nd) { nd = d; nearest = &s; }
            }
            wx::XpWx x = baseWx[nearest->icao];
            // A station's ground fog and low visibility are local; do not project them onto storms far from it.
            if (x.fogLayerApplied) { x.clouds[0] = wx::XpCloud(); x.fogLayerApplied = false; }
            // Haze/rain visibility (5 km and up) belongs to the whole air mass, so storm records within 60 nm carry the station's
            // value. Giving them 40 km instead made X-Plane blend them into the station area and the METAR visibility never showed.
            if (nd > 60.0 || x.visM < 5000.0f) x.visM = (float)g_cfg.clearVisM;
            if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK);
            wx::applyStorm(x, c, g_cfg, nearest->elevM, c.lat);
            XPLMWeatherInfo_t info;
            fillInfo(info, x);
            info.radius_nm = (float)std::max(6.0, g_cfg.radarCellNm * 0.9);
            Rec r;
            r.airport = false;
            r.lat = c.lat; r.lon = c.lon; r.elev = nearest->elevM;
            r.info = info;
            r.info.age = 0.0f;
            const std::string key = "radar:" + c.key;
            seen[key] = true;
            ++stormCount;
            auto it = g_recs.find(key);
            if (!full && it != g_recs.end() && std::memcmp(&it->second.info, &r.info, sizeof r.info) == 0) continue;
            r.info.age = info.age;
            todo.push_back({key, r});
        }
    }
    std::vector<std::string> gone;
    if (!full)
        for (const auto& kv : g_recs)
            if (!seen.count(kv.first)) gone.push_back(kv.first);

    // Everything is queued; flushPending() (every 0.25 s) hands it to X-Plane a few records at a time.
    if (full) g_recs.clear();
    g_pendingTodo = todo;
    g_pendingGone = gone;
    g_pendingImmediate = res.immediate || !g_firstApplyDone || g_startupPhase;
    g_pendingStartup = g_startupPhase;
    g_pendingManual = res.immediate && !g_startupPhase;
    {   // spread the records evenly over the configured transition time (nearest stations first)
        size_t n = todo.size() + gone.size();
        int T = g_firstApplyDone ? g_cfg.updateTransitionS : g_cfg.transitionS;
        if (g_cfg.applyImmediately && g_pendingImmediate) T = 0;
        g_releaseStepS = (n > 0 && T > 0) ? std::max(0.0625, (double)T / (double)n) : 0.0;
        g_nextRelease = Clock::now();
    }
    if (res.noaaFixes > 0 || res.filledElements > 0)
        logf("NOAA crosscheck: %d of %zu reports corrected or completed from NOAA's decoded fields, %d missing elements filled from neighbouring stations",
             res.noaaFixes, res.stations.size(), res.filledElements);
    logf("update: %zu stations, %d gap-fill records, %d storm cells, %zu changed, %zu removed%s%s", res.stations.size(), fillCount, stormCount, todo.size(), gone.size(), full ? " (full)" : "",
         (g_pendingTodo.empty() && g_pendingGone.empty()) ? "" : " [queued, sent a few at a time]");
    if (g_cfg.radar) {
        if (res.radar.ok) {
            float maxDbz = 0, maxTop = 0; int cb = 0;
            for (const auto& c : res.radar.cells) { maxDbz = std::max(maxDbz, c.dbz); maxTop = std::max(maxTop, c.topM); if (c.dbz >= 40) ++cb; }
            logf("radar: %s, %d echo blocks, %d cells applied (%d at 40+ dBZ = cumulonimbus), strongest %.0f dBZ, highest echo top %.0f m%s%s",
                 res.radar.source.c_str(), res.radar.echoBlocks, stormCount, cb, maxDbz, maxTop,
                 res.radar.topsFromRadar ? ", echo tops from radar" : ", tops estimated", res.radar.note.empty() ? "" : (" (" + res.radar.note + ")").c_str());
        }
        else if (g_radarDeferred)
            logf("radar: deferred, storms follow in about %d s", g_cfg.startupRadarDelayS);
        else
            logf("radar: none (%s)", res.radar.note.c_str());
    }
    g_lastApplyLogAt = Clock::now();
    if (!g_firstApplyDone) g_firstApplyAt = Clock::now();
    if (!g_radarDeferred) g_startupPhase = false;     // start-up is over once the storm update (or the only update) is applied
    g_firstApplyDone = true;
    g_lastRes = res;
    g_haveLast = true;
    ++g_viewVersion;
    g_prevReadVis = 0;
    if (fromFetch) { g_calIter = 0; g_checkTries = 0; }
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
    std::string radarNote = !g_cfg.radar ? "Radar: off" : (res.radar.ok ? "Radar: " + std::to_string(stormCount) + " storm cells" : "Radar: " + (res.radar.note.empty() ? std::string("none") : res.radar.note));
    std::string aloftNote = g_cfg.useAloft ? (aloftCount ? "Winds aloft: " + std::to_string(res.aloft.size()) + " NOAA stations"
                                                          : "Winds aloft: " + (res.aloftNote.empty() ? std::string("none") : res.aloftNote))
                                           : "Winds aloft: off";
    if (!res.stations.empty())
        std::snprintf(l2, sizeof l2, "%s | %s | nearest %s %.0f nm", aloftNote.c_str(), radarNote.c_str(), res.stations[0].icao.c_str(), res.stations[0].distNm);
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
    // Re-applying records makes X-Plane rebuild their cloud layers, which shows as flicker. The comparison is only
    // meaningful near the ground anyway, so skip it (and any correction) while airborne.
    if (g_drAgl && XPLMGetDataf(g_drAgl) > 1000.0f) return;
    double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
    const wx::Station* best = nullptr;
    double bestD = 1e9;
    for (const auto& s : g_lastRes.stations) {
        double d = wx::distNm(lat, lon, s.lat, s.lon);
        if (d < bestD) { bestD = d; best = &s; }
    }
    if (!best || bestD > 15.0) return; // only meaningful close to a station
    const bool anyFog = std::time(nullptr) < g_fogUntil;
    wx::XpWx x = wx::buildXp(*best, g_cfg, std::time(nullptr), &g_lastRes.aloft, anyFog);
    XPLMWeatherInfo_t info;
    std::memset(&info, 0, sizeof info);
    info.structSize = sizeof info;
    int detailed = XPLMGetWeatherAtLocation(best->lat, best->lon, best->elevM + 5.0, &info);
    logf("visibility check %s (%.0f nm away): report says %.0f m, we sent %.0f m, X-Plane is using %.0f m (correction %.2f, detailed=%d)",
         best->icao.c_str(), bestD, x.visM, std::max(50.0f, x.visM < 20000.0f ? x.visM * g_visK : x.visM), info.visibility, g_visK, detailed);
    logf("  read-back cloud layers at %s:%s", best->icao.c_str(), "");
    for (int i = 0; i < XPLM_NUM_CLOUD_LAYERS; ++i)
        logf("    layer %d: type %.1f coverage %.2f base %.0f m top %.0f m (we set: %s)", i, info.cloud_layers[i].cloud_type,
             info.cloud_layers[i].coverage, info.cloud_layers[i].alt_base, info.cloud_layers[i].alt_top,
             x.clouds[i].coverage > 0 ? "coverage>0" : "none");
    if (!g_cfg.visAutocal || x.visM >= 20000.0f || info.visibility < 100.0f || g_calIter >= 2) return;
    // X-Plane fades new weather in over minutes (longer while it is busy loading), so right after an update the read-back
    // still shows the OLD weather. Calibrating against it drives our visibility far too low. Wait until the value has
    // stopped moving (under 4% change between two checks a minute apart) before trusting it.
    {
        const float v = info.visibility;
        const bool stable = g_prevReadVis > 0 && std::fabs(v - g_prevReadVis) <= 0.04f * v;
        g_prevReadVis = v;
        if (!stable) {
            if (g_checkTries < 6) {
                ++g_checkTries;
                g_checkPending = true;
                g_checkAt = Clock::now() + std::chrono::seconds(std::min(60, std::max(1, g_cfg.visCheckS)));
                logf("visibility check: X-Plane's value is still changing (%.0f m), checking again in 60 s", v);
            } else {
                logf("visibility check: X-Plane's value never settled, leaving visibility as sent");
            }
            return;
        }
    }
    // If the last correction lowered our value but X-Plane's reading did not come down with it, the reading is being driven by
    // something else (other records, its own weather). Lowering further only makes the sky murky for nothing: undo and stop.
    if (g_calIter == 1 && g_visK < 1.0f && g_visBeforeCorr > 0 && info.visibility >= g_visBeforeCorr * 0.9f) {
        logf("visibility check: X-Plane did not follow the lower value (%.0f m before, %.0f m now); restoring the METAR value and stopping", g_visBeforeCorr, info.visibility);
        g_visK = (float)g_cfg.visScale;
        g_calIter = 2;
        wx::FetchResult copy = g_lastRes;
        copy.immediate = true;
        applyResult(copy, false);
        return;
    }
    double ratio = x.visM / info.visibility;
    if (std::fabs(ratio - 1.0) <= 0.25) return;
    g_visBeforeCorr = info.visibility;
    g_visK = (float)std::min(1.5, std::max(0.2, g_visK * std::pow(ratio, 0.8)));
    ++g_calIter;
    logf("visibility off by %.0f%%: correction now %.2f, re-applying (pass %d/2)", (ratio - 1.0) * 100.0, g_visK, g_calIter);
    wx::FetchResult copy = g_lastRes;
    copy.immediate = true;
    applyResult(copy, false);
}

// ---------------- storm tops on X-Plane's map ----------------
XPLMMapLayerID g_mapLayer = nullptr;
bool g_mapHookRegistered = false;

void mapLabels(XPLMMapLayerID layer, const float* bounds, float zoom, float, XPLMMapStyle, XPLMMapProjectionID proj, void*) {
    if (!g_cfg.radar || !g_cfg.radarMapTops || !g_haveLast || !g_lastRes.radar.ok) return;
    // bounds: left, top, right, bottom in map units. Stronger storms first; fewer labels when zoomed out.
    const float minDbz = zoom >= 2.0f ? 30.0f : 40.0f;
    std::vector<const wx::RadarCell*> cells;
    for (const auto& c : g_lastRes.radar.cells) if (c.dbz >= minDbz) cells.push_back(&c);
    std::sort(cells.begin(), cells.end(), [](const wx::RadarCell* a, const wx::RadarCell* b) { return a->dbz > b->dbz; });
    int drawn = 0;
    for (const auto* c : cells) {
        float x = 0, y = 0;
        XPLMMapProject(proj, c->lat, c->lon, &x, &y);
        if (x < bounds[0] || x > bounds[2] || y > bounds[1] || y < bounds[3]) continue;
        std::string t = wx::formatFl(wx::stormTopM(*c, g_cfg, 900.0f), c->topKnown);   // same value the cloud is built with
        XPLMDrawMapLabel(layer, t.c_str(), x, y, xplm_MapOrientation_Map, 0.0f);
        if (++drawn >= 150) break;
    }
}
void mapPrep(XPLMMapLayerID, const float*, XPLMMapProjectionID, void*) {}
void mapGone(XPLMMapLayerID, void*) { g_mapLayer = nullptr; }

void createMapLayer(const char* mapId, void*) {
    if (g_mapLayer || std::strcmp(mapId, XPLM_MAP_USER_INTERFACE) != 0) return;
    XPLMCreateMapLayer_t p;
    std::memset(&p, 0, sizeof p);
    p.structSize = sizeof p;
    p.mapToCreateLayerIn = XPLM_MAP_USER_INTERFACE;
    p.layerType = xplm_MapLayer_Markings;
    p.willBeDeletedCallback = mapGone;
    p.prepCacheCallback = mapPrep;
    p.drawCallback = nullptr;
    p.iconCallback = nullptr;
    p.labelCallback = mapLabels;
    p.showUiToggle = 1;
    p.layerName = "WXPlane storm tops";
    p.refcon = nullptr;
    g_mapLayer = XPLMCreateMapLayer(&p);
}


void mapCreatedHook(const char* mapId, void* rc) { if (g_pluginEnabled) createMapLayer(mapId, rc); }

// ---------------- start-up gating ----------------
// Scenery loading is the busiest moment of a flight; downloads and weather rebuilds then add to the load. Hold the first
// update back until the plugin has been up for startup_delay_s and X-Plane reports no scenery loading for a few seconds.
Clock::time_point g_enabledAt{};
Clock::time_point g_calmSince{};
bool g_calm = false;

Clock::time_point g_fpsCalmSince{};
bool g_fpsCalm = false;
// True once the frame time has stayed under 50 ms (20 fps+) for 4 s in a row. Without the dataref there is nothing to wait for.
bool framesSteady() {
    if (!g_drFramePeriod) return true;
    auto now = Clock::now();
    if (XPLMGetDataf(g_drFramePeriod) > 0.05f) { g_fpsCalm = false; return false; }
    if (!g_fpsCalm) { g_fpsCalm = true; g_fpsCalmSince = now; }
    return std::chrono::duration<double>(now - g_fpsCalmSince).count() >= 4.0;
}


bool sceneryReady() {
    auto now = Clock::now();
    double up = std::chrono::duration<double>(now - g_enabledAt).count();
    if (up < g_cfg.startupDelayS) return false;
    if (g_cfg.startupWaitFps && up < g_cfg.startupMaxWaitS && !framesSteady()) return false;
    if (!g_cfg.startupWaitScenery || up > std::max(150.0, (double)g_cfg.startupMaxWaitS) || !g_drLoading) return true;           // never wait forever; without the dataref the delay alone applies
    if (XPLMGetDatai(g_drLoading) != 0) { g_calm = false; return false; }
    if (!g_calm) { g_calm = true; g_calmSince = now; }
    return std::chrono::duration<double>(now - g_calmSince).count() >= 3.0;
}

bool g_disableErase = false;   // plugin was disabled: erase our records from the next callback, then stop
Clock::time_point g_lastLoopAt{};
bool g_loopStarted = false;
int g_stallLogs = 0;
Clock::time_point g_lastStallLog{};

float flightLoop(float, float, int, void*) {
    if (!g_pluginEnabled) {
        if (g_disableErase) { g_disableErase = false; g_checkPending = false; eraseAll(); }
        return 0.0f;   // stop calling us until the plugin is enabled again
    }
    const auto nowT = Clock::now();
    if (!g_loopStarted) {
        // X-Plane does not run flight loops while it is still loading (this can be well over a minute after the plugin was
        // enabled), so every start-up wait is counted from here, from the moment the sim starts running.
        g_loopStarted = true;
        g_enabledAt = nowT;
    } else if (g_firstApplyDone && g_stallLogs < 20) {
        // Frame stall detector, measured with our own clock (X-Plane's elapsed-time argument is not the gap between frames):
        // this callback runs every 0.25 s, so a much longer gap means the whole sim stood still. Logged only after our first
        // update, at most one line per 10 s, so the log shows whether a freeze came right after one of our updates.
        double gap = std::chrono::duration<double>(nowT - g_lastLoopAt).count();
        if (gap > 1.5 && std::chrono::duration<double>(nowT - g_lastStallLog).count() > 10.0) {
            g_lastStallLog = nowT;
            ++g_stallLogs;
            logf("sim stalled for %.1f s (our last weather update was %d s ago)", gap,
                 (int)std::chrono::duration<double>(nowT - g_lastApplyLogAt).count());
        }
    }
    g_lastLoopAt = nowT;
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

    flushPending();

    // 2) switched off from the menu -> hand weather back to X-Plane
    if (g_eraseRequested) {
        g_eraseRequested = false;
        g_checkPending = false;
        eraseAll();
        g_haveCenter = false;
        setStatus("Disabled - X-Plane weather in control", "");
    }

    // 3) schedule the next download
    if (g_cfg.enabled && !g_fetcher.busy() && (g_everRequested || g_manualRefresh || sceneryReady())) {
        double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
        bool valid = std::fabs(lat) > 0.001 || std::fabs(lon) > 0.001;
        if (valid) {
            auto now = Clock::now();
            double since = g_everRequested ? std::chrono::duration<double>(now - g_lastRequest).count() : 1e9;
            bool due = since >= g_cfg.updateMinutes * 60.0;
            // staged start-up: radar storms follow the first (small) update once the sim has settled
            const bool catchUp = g_radarDeferred && g_firstApplyDone &&
                                 std::chrono::duration<double>(now - g_firstApplyAt).count() >= g_cfg.startupRadarDelayS && (!g_cfg.startupWaitFps || framesSteady());
            if (catchUp) due = true;
            bool moved = g_haveCenter && since >= 60.0 && wx::distNm(lat, lon, g_centerLat, g_centerLon) > g_cfg.refetchMoveNm;
            if (due || moved || g_manualRefresh) {
                wx::FetchRequest rq;
                rq.lat = lat; rq.lon = lon; rq.cfg = g_cfg;
                if (catchUp) g_radarDeferred = false;
                else if (!g_firstApplyDone && g_cfg.startupStaged && g_cfg.radar) { rq.cfg.radar = false; g_radarDeferred = true; }
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
    return 0.25f;
}

// ---------------- "Show Local METAR/ATIS Data" window ----------------
XPLMWindowID g_win = nullptr;
int g_scroll = 0;                 // first visible wrapped line
struct DrawLine { wx::LineKind kind; std::string text; };
std::vector<DrawLine> g_wrapped;
unsigned g_wrappedVersion = 0;
int g_wrappedWidth = -1;

void colorFor(wx::LineKind k, float* c) {
    switch (k) {
        case wx::LineKind::Title:   c[0] = 1.0f; c[1] = 1.0f; c[2] = 1.0f; break;
        case wx::LineKind::Header:  c[0] = 0.55f; c[1] = 0.85f; c[2] = 1.0f; break;
        case wx::LineKind::Metar:   c[0] = 0.85f; c[1] = 0.95f; c[2] = 0.7f; break;
        case wx::LineKind::Atis:    c[0] = 1.0f; c[1] = 0.85f; c[2] = 0.5f; break;
        case wx::LineKind::Applied: c[0] = 0.75f; c[1] = 0.75f; c[2] = 0.75f; break;
        default:                    c[0] = 0.9f; c[1] = 0.9f; c[2] = 0.9f; break;
    }
}

// Word wrap using the real font width so nothing runs off the window edge.
void wrapPx(const std::string& text, wx::LineKind kind, int widthPx, std::vector<DrawLine>& out) {
    if (text.empty()) { out.push_back({kind, ""}); return; }
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        std::string word = text.substr(i, j - i);
        std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && XPLMMeasureString(xplmFont_Proportional, trial.c_str(), (int)trial.size()) > widthPx) {
            out.push_back({kind, line});
            line = "    " + word;   // continuation lines are indented
        } else {
            line = trial;
        }
        // a single token longer than the window: hard split
        while (line.size() > 8 && XPLMMeasureString(xplmFont_Proportional, line.c_str(), (int)line.size()) > widthPx) {
            size_t cut = line.size();
            while (cut > 4 && XPLMMeasureString(xplmFont_Proportional, line.c_str(), (int)cut) > widthPx) --cut;
            out.push_back({kind, line.substr(0, cut)});
            line = "    " + line.substr(cut);
        }
        i = j + 1;
    }
    out.push_back({kind, line});
}

void rewrap(int widthPx) {
    if (g_wrappedVersion == g_viewVersion && g_wrappedWidth == widthPx) return;
    g_wrapped.clear();
    for (const auto& l : wx::buildViewLines(g_lastRes, g_haveLast, g_cfg, std::time(nullptr))) wrapPx(l.text, l.kind, widthPx, g_wrapped);
    g_wrappedVersion = g_viewVersion;
    g_wrappedWidth = widthPx;
}

void drawWin(XPLMWindowID w, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(w, &l, &t, &r, &b);
    int cw = 0, ch = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &cw, &ch, nullptr);
    int lineH = ch + 3;
    int x = l + 10, widthPx = (r - l) - 28;
    rewrap(widthPx);
    int rows = ((t - b) - 20) / lineH;
    if (rows < 1) rows = 1;
    int maxScroll = std::max(0, (int)g_wrapped.size() - rows);
    g_scroll = std::max(0, std::min(g_scroll, maxScroll));
    int y = t - 8 - ch;
    for (int i = g_scroll; i < (int)g_wrapped.size() && i < g_scroll + rows; ++i, y -= lineH) {
        float c[3];
        colorFor(g_wrapped[(size_t)i].kind, c);
        std::string s = g_wrapped[(size_t)i].text;
        XPLMDrawString(c, x, y, &s[0], nullptr, xplmFont_Proportional);
    }
    if (maxScroll > 0) {   // slim scroll indicator on the right edge
        int track = (t - b) - 20;
        int barH = std::max(12, track * rows / (int)g_wrapped.size());
        int barTop = t - 10 - (track - barH) * g_scroll / maxScroll;
        XPLMDrawTranslucentDarkBox(r - 12, barTop, r - 8, barTop - barH);
    }
}

int clickWin(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; }
void keyWin(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}
XPLMCursorStatus cursorWin(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
int wheelWin(XPLMWindowID, int, int, int wheel, int clicks, void*) {
    if (wheel == 0) g_scroll -= clicks * 3;
    return 1;
}

void showWindow() {
    ++g_viewVersion;   // always show fresh data when opened
    if (!g_win) {
        int sl, st, sr, sb;
        XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
        XPLMCreateWindow_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.left = sl + 60; p.right = std::min(sr - 60, sl + 60 + 860);
        p.top = st - 80;  p.bottom = std::max(sb + 80, st - 80 - 620);
        p.visible = 1;
        p.drawWindowFunc = drawWin;
        p.handleMouseClickFunc = clickWin;
        p.handleRightClickFunc = clickWin;
        p.handleKeyFunc = keyWin;
        p.handleCursorFunc = cursorWin;
        p.handleMouseWheelFunc = wheelWin;
        p.refcon = nullptr;
        p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
        p.layer = xplm_WindowLayerFloatingWindows;
        g_win = XPLMCreateWindowEx(&p);
        if (g_win) {
            XPLMSetWindowTitle(g_win, "WXPlane - local METAR / ATIS data");
            XPLMSetWindowResizingLimits(g_win, 360, 200, 4000, 4000);
        }
    }
    if (g_win) {
        g_scroll = 0;
        XPLMSetWindowIsVisible(g_win, 1);
        XPLMBringWindowToFront(g_win);
    }
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
        case kShow:
            showWindow();
            break;
        case kRadar:
            g_cfg.radar = !g_cfg.radar;
            if (g_cfg.enabled) g_manualRefresh = true;
            break;
        case kImmediate:   // applies to the next update; may cause a short stall while X-Plane rebuilds clouds
            g_cfg.applyImmediately = !g_cfg.applyImmediately;
            break;
        case kRadarGlobal:
            g_cfg.radarGlobal = !g_cfg.radarGlobal;
            if (g_cfg.enabled && g_cfg.radar) g_manualRefresh = true;
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
    {   // optional colour table for the experimental global radar: lines "r,g,b,dBZ" next to the config file
        std::string pal = g_cfgPath.substr(0, g_cfgPath.find_last_of("/\\") + 1) + "WXPlane_radar_palette.csv";
        int n = wx::loadRainViewerPalette(pal);
        if (n > 0) logf("loaded %d radar palette entries from %s", n, pal.c_str());
    }

    g_drLat = XPLMFindDataRef("sim/flightmodel/position/latitude");
    g_drLon = XPLMFindDataRef("sim/flightmodel/position/longitude");
    g_drLevels = XPLMFindDataRef("sim/weather/region/atmosphere_alt_levels_m");
    g_drAgl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
    g_drFramePeriod = XPLMFindDataRef("sim/operation/misc/frame_rate_period");
    g_drLoading = XPLMFindDataRef("sim/graphics/scenery/async_scenery_load_in_progress"); // null if this build lacks it

    int sub = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "WXPlane", nullptr, 0);
    g_menu = XPLMCreateMenu("WXPlane", XPLMFindPluginsMenu(), sub, menuHandler, nullptr);
    g_idxEnabled = XPLMAppendMenuItem(g_menu, "Enabled", (void*)(intptr_t)kEnabled, 0);
    g_idxRefresh = XPLMAppendMenuItem(g_menu, "Refresh Weather Data Now", (void*)(intptr_t)kRefresh, 0);
    g_idxShow = XPLMAppendMenuItem(g_menu, "Show Local METAR/ATIS Data...", (void*)(intptr_t)kShow, 0);
    XPLMAppendMenuSeparator(g_menu);
    g_idxAtis = XPLMAppendMenuItem(g_menu, "Use ATIS", (void*)(intptr_t)kAtis, 0);
    g_idxAloft = XPLMAppendMenuItem(g_menu, "Use NOAA Winds/Temps Aloft", (void*)(intptr_t)kAloft, 0);
    g_idxRadar = XPLMAppendMenuItem(g_menu, "Radar Storms (NOAA MRMS)", (void*)(intptr_t)kRadar, 0);
    g_idxRadarGlobal = XPLMAppendMenuItem(g_menu, "Radar Outside US (experimental)", (void*)(intptr_t)kRadarGlobal, 0);
    g_idxImmediate = XPLMAppendMenuItem(g_menu, "Apply Weather Immediately (may stutter)", (void*)(intptr_t)kImmediate, 0);
    XPLMAppendMenuSeparator(g_menu);
    g_idxStatus1 = XPLMAppendMenuItem(g_menu, "Starting...", nullptr, 0);
    g_idxStatus2 = XPLMAppendMenuItem(g_menu, "", nullptr, 0);
    XPLMEnableMenuItem(g_menu, g_idxStatus1, 0);
    XPLMEnableMenuItem(g_menu, g_idxStatus2, 0);
    syncMenu();
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    if (g_loop) {
        if (g_disableErase) { eraseAll(); g_disableErase = false; }   // no frame came after disabling: last chance
        XPLMDestroyFlightLoop(g_loop);
        g_loop = nullptr;
    }
    if (g_win) { XPLMDestroyWindow(g_win); g_win = nullptr; }
    if (g_menu) { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API int XPluginEnable(void) {
    wx::httpGlobalInit();
    g_fetcher.start();
    g_enabledAt = Clock::now();
    g_loopStarted = false;
    g_visK = (float)g_cfg.visScale;
    g_calm = false;
    if (!g_mapHookRegistered) { XPLMRegisterMapCreationHook(mapCreatedHook, nullptr); g_mapHookRegistered = true; }
    if (XPLMMapExists(XPLM_MAP_USER_INTERFACE)) createMapLayer(XPLM_MAP_USER_INTERFACE, nullptr);

    g_disableErase = false;
    if (!g_loop) {
        XPLMCreateFlightLoop_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.phase = xplm_FlightLoop_Phase_BeforeFlightModel; // the weather API must be used before the flight model
        p.callbackFunc = flightLoop;
        p.refcon = nullptr;
        g_loop = XPLMCreateFlightLoop(&p);
    }
    XPLMScheduleFlightLoop(g_loop, 2.0f, 1);

    g_pluginEnabled = true;
    g_everRequested = false;
    g_firstApplyDone = false;
    g_radarDeferred = false;
    g_startupPhase = true;
    setStatus(g_cfg.enabled ? "Waiting for sim..." : "Disabled", "");
    logf("enabled (update every %d min, search %.0f nm)", g_cfg.updateMinutes, g_cfg.searchNm);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    g_pluginEnabled = false;
    if (g_mapLayer) { XPLMDestroyMapLayer(g_mapLayer); g_mapLayer = nullptr; }
    g_fetcher.stop();
    wx::httpGlobalCleanup();
    // Give weather back to X-Plane so our fog/cloud records don't outlive the plugin. The weather API may only be used from the
    // pre-flight-model callback, so the erase is done there on the next frame (the loop stays alive until XPluginStop); only if
    // the sim is going away before another frame does XPluginStop erase directly.
    g_disableErase = true;
    if (g_loop) XPLMScheduleFlightLoop(g_loop, -1.0f, 1);   // negative = every frame; the callback stops itself after the erase
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
