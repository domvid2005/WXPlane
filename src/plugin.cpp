// WXPlane - X-Plane 12 plugin entry point (XPLM SDK 4.2.0 / X-Plane 12.3+).
//
// Flow: flight loop (1 Hz, before flight model) -> asks the worker thread for NOAA METAR/ATIS data around the
// aircraft every N minutes -> applies the result with XPLMBeginWeatherUpdate/XPLMSetWeatherAt*/XPLMEndWeatherUpdate.

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>
#include <set>
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
#include "XPLMScenery.h"
#include "XPLMUtilities.h"
#include "XPLMWeather.h"
#include "windshear.h"

#include "config.h"
#include "fetcher.h"
#include "geo.h"
#include "http.h"
#include "sources.h"
#include "radar.h"
#include "turbfetch.h"
#include "wxgl.h"
#include "viewer.h"
#ifndef WX_HARNESS
#include "xpimgui.h"
#endif
#include "wxui.h"
#include "version.h"
#include "wxmodel.h"
#include "loadclock.h"

namespace {

using Clock = std::chrono::steady_clock;

enum MenuId { kEnabled = 1, kRefresh, kAtis, kAloft, kShow, kRadar, kRadarGlobalRemoved /* keeps the ids below unchanged */, kSettings, kWindshear, kTurbulence };

wx::Config g_cfg;
wx::Fetcher g_fetcher;
std::string g_cfgPath;

XPLMMenuID g_menu = nullptr;
int g_idxEnabled = -1, g_idxRefresh = -1, g_idxShow = -1, g_idxAtis = -1, g_idxAloft = -1, g_idxWindshear = -1, g_idxTurbulence = -1, g_idxStatus1 = -1, g_idxStatus2 = -1;
XPLMFlightLoopID g_loop = nullptr;

bool g_radarDeferred = false;                 // first update went out without radar; storms are still to come
Clock::time_point g_firstApplyAt{};
Clock::time_point g_enabledAt{};   // start of the sim's first frame (start-up waits count from here)
bool g_loopStarted = false;
Clock::time_point g_lastApplyLogAt{};
XPLMDataRef g_drFramePeriod = nullptr;
XPLMDataRef g_drLat = nullptr, g_drLon = nullptr, g_drLevels = nullptr, g_drAgl = nullptr, g_drLoading = nullptr;
XPLMDataRef g_drGsFetch = nullptr, g_drTrkFetch = nullptr;   // ground speed (m/s) and true track: the GFS lattice is centred ahead of a fast aircraft
float g_levelsM[XPLM_NUM_TEMPERATURE_LAYERS];
float g_windAltM[XPLM_NUM_WIND_LAYERS];
bool g_haveWindAlts = false;
bool g_haveLevels = false;

bool g_pluginEnabled = false;
unsigned g_viewVersion = 1;     // bumped whenever new data is applied (METAR/ATIS window re-wraps on change)
wx::FetchResult g_lastRes;      // last applied data, kept so a visibility correction can re-apply without a download
bool g_haveLast = false;
float g_visK = 1.0f;            // factor between the METAR visibility and what we send (vis_scale; the optional self-check adjusts it)
bool g_checkPending = false;
bool g_loggedNoNearStation = false;
std::string g_checkedIcao;       // station the visibility check last looked at; a different one nearby (moved, reloaded) restarts it
Clock::time_point g_moveScanAt{};
Clock::time_point g_lastRadarRequest{};
XPLMDataRef g_drElev = nullptr;
bool g_watchOn = false;
int g_watchN = 0;
Clock::time_point g_watchAt{};
Clock::time_point g_checkAt{};
int g_calIter = 0;
int g_checkTries = 0;
float g_prevReadVis = 0;
float g_visBeforeCorr = 0;
bool g_reapplyRequested = false;   // settings that change the weather we send (not what we download): rebuild from the data we already have
bool g_manualRefresh = false;
bool g_radarKick = false;          // radar was switched on / changed: fetch only the radar (stations and winds are kept), no full refresh
bool g_eraseRequested = false;
bool g_reenable = false;           // the weather was switched back on: the next download is not held, and fades in
bool g_firstApplyDone = false;
// The weather window's countdown while the weather is built from scratch (start-up, switched back on, a jump to a new place)
wx::LoadClock g_load;
bool g_loadFetchIssued = false;     // the download for it has been asked for
bool g_fetchRunning = false;        // a full download is running
std::chrono::steady_clock::time_point g_fetchStartAt{}, g_lastSendAt{};
bool g_lastSendImmediate = false;
double g_fetchEstS = 25;            // how long full downloads take (learned)
double g_loadS = -1;                // seconds left on the countdown, -1: none
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

bool g_uiOpen = false;     // the weather window is open (the radar map's turbulence layer is only built then)
bool g_xpRealWx = false;   // X-Plane's own real-world weather is on (it should be off while WXPlane runs)
std::string g_status1, g_status2;
void setStatus(const std::string& l1, const std::string& l2) {
    g_status1 = l1; g_status2 = l2;
    if (!g_menu) return;
    XPLMSetMenuItemName(g_menu, g_idxStatus1, l1.c_str(), 0);
    XPLMSetMenuItemName(g_menu, g_idxStatus2, g_xpRealWx ? "X-Plane's real-world weather is ON - turn it off" : l2.c_str(), 0);
}

// X-Plane's own real-world weather (2026-11-10 customer log, ENGM): left on, X-Plane keeps downloading and re-reading its GRIB files
// every minute and blends them with WXPlane's records, and the regen_weather WXPlane sends at start made it fetch everything again
// (a 19.7 s freeze during the load, none on a machine with it off). sim/weather/region/weather_source is 1 ("Real Weather") and
// change_mode 7 ("Using Real Weather") then. Checked every 10 s; logged when it changes; the menu says so.
void realWeatherCheck() {
    static XPLMDataRef drSrc = XPLMFindDataRef("sim/weather/region/weather_source"), drMode = XPLMFindDataRef("sim/weather/region/change_mode");
    static Clock::time_point next{};
    static int lastSrc = -99, lastMode = -99;
    const auto nowC = Clock::now();
    if (nowC < next) return;
    next = nowC + std::chrono::seconds(10);
    const int src = drSrc ? XPLMGetDatai(drSrc) : -1, mode = drMode ? XPLMGetDatai(drMode) : -1;
    if (src == lastSrc && mode == lastMode) return;
    const bool on = src == 1 || mode == 7;
    logf("X-Plane weather source %d (0 preset, 1 real weather, 2 control pad, 3 plugin), change mode %d%s", src, mode,
         on ? ": X-Plane's own real-world weather is ON - it should be off while WXPlane runs (X-Plane's weather settings), or the two are blended"
            : "");
    lastSrc = src; lastMode = mode;
    if (on != g_xpRealWx) { g_xpRealWx = on; setStatus(g_status1, g_status2); }
}

void syncMenu() {
    if (!g_menu) return;
    XPLMCheckMenuItem(g_menu, g_idxEnabled, g_cfg.enabled ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g_menu, g_idxAtis, g_cfg.useAtis ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    XPLMCheckMenuItem(g_menu, g_idxAloft, g_cfg.useAloft ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    if (g_idxWindshear >= 0) XPLMCheckMenuItem(g_menu, g_idxWindshear, g_cfg.windshear ? xplm_Menu_Checked : xplm_Menu_Unchecked);
    if (g_idxTurbulence >= 0) XPLMCheckMenuItem(g_menu, g_idxTurbulence, g_cfg.realisticTurbulence ? xplm_Menu_Checked : xplm_Menu_Unchecked);
}

// Removes every record this plugin has set, handing control back to X-Plane's own weather.
struct Rec {
    XPLMWeatherInfo_t info;   // as sent, with age zeroed so unchanged data compares equal
    bool airport = false;
    std::string icao;
    double lat = 0, lon = 0, elev = 0;
    bool kept = false;        // an airport that left range, left in place with its surroundings' weather (see applyResult)
};
std::map<std::string, Rec> g_recs; // what we have currently set in the sim, by station

// Records are handed to X-Plane a few at a time (nearest stations first, then storms) instead of all at once: every
// weather update makes the sim rebuild what changed, and doing 100 of them in one frame is what shows up as a stutter.
bool g_pendingImmediate = false;
bool g_startupPhase = true;        // true until the start-up updates (stations, then storms) are out
bool g_pendingStartup = false;
XPLMCommandRef g_regenCmd = nullptr;
bool g_regenDone = false;
Clock::time_point g_regenAt{};
bool g_pendingReplace = false;      // the pending update replaces all of WXPlane's records (one non-incremental update, see applyResult)
bool g_pendingManual = false;       // the pending update was asked for with "Refresh Weather Data Now"
Clock::time_point g_nextRelease{};   // when the next queued record may be handed to X-Plane
double g_releaseStepS = 0.0;         // seconds between records, so a whole update spreads over transition_s
struct Todo { std::string key; Rec rec; };
std::vector<Todo> g_pendingTodo;
std::vector<std::string> g_pendingGone;
// Storm intensity-level outline records (keys "radar:L..." / "radar:M...") are held out of the big start-up update and handed over on their
// own a little later: the one time they went out with it, X-Plane threw the whole start-up update away (log 2026-11-03, "XPLMBeginWeatherUpdate()
// called without matching XPLMEndWeatherUpdate()", no clouds at all). This way the station, fill and storm weather is in first whatever happens.
std::vector<Todo> g_heldLevel;
Clock::time_point g_heldLevelAt{};
bool isLevelKey(const std::string& k) { return k.rfind("radar:L", 0) == 0 || k.rfind("radar:M", 0) == 0; }
// Is this spot well inside a storm, i.e. are there 4 or more measured (or hole-filled) storm blocks within 1.2 block widths? Only then do
// station and gap-fill records carry the storm's rain (on the rim, the storm records' own outline decides). Before, every record within ~16 nm of any storm cell got at least 5 % rain, so light rain spread out
// around storms in 12 nm gap-fill squares on X-Plane's radar map (2026-11-03 screenshots); now the storm records' own rounded outline
// decides where the rain ends.
bool insideStorm(const std::vector<wx::RadarCell>& cells, double lat, double lon, double cellNm) {
    int n = 0;
    for (const auto& c : cells) {
        if (c.synthetic && (c.key.empty() || c.key[0] != 'H')) continue;
        if (wx::distNm(lat, lon, c.lat, c.lon) < cellNm * 1.2 && ++n >= 4) return true;
    }
    return false;
}

// Is a measured (or hole-filled) storm block within reachNm? Rim, outline and intensity-level cells do not count.
bool underStorm(const std::vector<wx::RadarCell>& cells, double lat, double lon, double reachNm) {
    for (const auto& c : cells)
        if ((!c.synthetic || (!c.key.empty() && c.key[0] == 'H')) && wx::distNm(lat, lon, c.lat, c.lon) < reachNm) return true;
    return false;
}

// Altitude hunting (2026-11-04, Salt Lake City): every gap-fill, dry and storm record carried the QNH of its NEAREST station, so the
// pressure field was a patchwork of the station values, with steps of several hPa where two records met. Around mountains the stations'
// altimeter settings differ a lot (they are reduced to sea level from very different heights), and each hPa is about 27 ft, so crossing
// record edges moved the barometric altitude up and down by hundreds of feet within a few miles, and the autopilot chased it.
// Now every record takes a smooth pressure field: a Gaussian-weighted mean (sigma 50 nm) of the station QNHs, with stations more than
// 4 hPa off the local median left out of it. A station's own record keeps its QNH within 2 hPa of that field (so the altimeter setting at
// the field stays close to the ATIS), never more.
void smoothPressure(std::vector<Todo>& todo, const std::vector<wx::Station>& stations) {
    if (!g_cfg.smoothPressure) return;
    struct Q { double lat, lon, hpa; };
    std::vector<Q> qs;
    for (const auto& s : stations) if (s.report.hasAltim && s.report.altimHpa > 850 && s.report.altimHpa < 1090) qs.push_back({s.lat, s.lon, s.report.altimHpa});
    if (qs.size() < 2) return;
    std::vector<unsigned char> keep(qs.size(), 1);
    for (size_t i = 0; i < qs.size(); ++i) {   // outliers against the median of the stations within 100 nm
        std::vector<double> around;
        for (const auto& o : qs) if (wx::distNm(qs[i].lat, qs[i].lon, o.lat, o.lon) <= 100.0) around.push_back(o.hpa);
        std::sort(around.begin(), around.end());
        if (around.size() >= 3 && std::fabs(qs[i].hpa - around[around.size() / 2]) > 4.0) keep[i] = 0;
    }
    const double sig = 50.0;
    auto field = [&](double lat, double lon, double& out) {
        double sw = 0, sv = 0;
        for (size_t i = 0; i < qs.size(); ++i) {
            if (!keep[i]) continue;
            const double d = wx::distNm(lat, lon, qs[i].lat, qs[i].lon);
            const double w = std::exp(-d * d / (2.0 * sig * sig)) + 1e-9 / (1.0 + d);   // far from all stations: the nearest still counts a little
            sw += w; sv += w * qs[i].hpa;
        }
        if (sw <= 0) return false;
        out = sv / sw; return true;
    };
    for (auto& t : todo) {
        XPLMWeatherInfo_t& in = t.rec.info;
        if (!(in.pressure_alt > 0)) continue;
        double lat = t.rec.lat, lon = t.rec.lon;
        bool station = false;   // a station's own record (an airport record, or a location record for an airport X-Plane does not know)
        for (const auto& st : stations) if (st.icao == t.key) { lat = st.lat; lon = st.lon; station = true; break; }
        double f;
        if (!field(lat, lon, f)) continue;
        double hpa = f;
        if (station) hpa = f + std::max(-2.0, std::min(2.0, in.pressure_alt / 100.0 - f));
        in.pressure_alt = (float)(std::round(hpa * 10.0) * 10.0);   // 0.1 hPa steps, in Pa
    }
}

// Smooth value noise -1..1 on absolute nm coordinates (two octaves), the same from one update to the next.
double noise2(double X, double Y, double scale, int salt) {
    auto one = [&](double sc, int s2) {
        const double u = X / sc + s2 * 13.7, v = Y / sc - s2 * 7.3;
        const long iu = (long)std::floor(u), iv = (long)std::floor(v);
        const double fu = u - iu, fv = v - iv;
        auto val = [&](long a, long b) {
            uint32_t h = (uint32_t)a * 0x8da6b343u ^ (uint32_t)b * 0xd8163841u ^ (uint32_t)(s2 + 31) * 0xcb1ab31fu;
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
            return (double)(h % 20001u) / 10000.0 - 1.0;
        };
        const double su = fu * fu * (3 - 2 * fu), sv = fv * fv * (3 - 2 * fv);
        const double t = val(iu, iv) * (1 - su) + val(iu + 1, iv) * su, b = val(iu, iv + 1) * (1 - su) + val(iu + 1, iv + 1) * su;
        return t * (1 - sv) + b * sv;
    };
    return 0.67 * one(scale, salt) + 0.33 * one(scale * 0.45, salt + 5);
}

// Cloud layers line up across all records (see the call in applyResult). X-Plane drops empty slots and moves the rest down (the read-back
// showed our slot 1 deck as its layer 0), then blends layer 0 with layer 0 of the neighbouring record, layer 1 with layer 1 and so on.
// So a record with fog + deck next to one with only the deck blended the fog into the deck, and a record whose deck sat in another slot
// than its neighbour's blended two different layers: thin sloping sheets, seen from above as ribbons and snakes in clear air.
// Here all real layers of all records are sorted into up to three height bands (the same for every record), each record's layers go into
// its band's slot in height order, and a band a record has no cloud in gets a nearly empty layer (g_cfg.emptyLayerCoverage) at the height
// of the nearest record that has one there. Every record then sends the same three layers in the same order, and between a cloudy and
// a clear record a layer only thins out, at its own height.
void matchEmptyLayers(std::vector<Todo>& todo) {
    const int K = XPLM_NUM_CLOUD_LAYERS;
    const float ph = (float)g_cfg.emptyLayerCoverage;
    if (todo.empty() || (ph <= 0.0f && g_cfg.cloudSmoothNm <= 0.0)) return;
    auto real = [](const XPLMWeatherInfoClouds_t& c) { return c.coverage > 0.05f && c.alt_top > c.alt_base; };
    // 1) height bands: the layer bases, sorted, split at the largest jumps in height (at most 3 bands, a jump of 300 m or more), so layers
    // at nearly the same height always share a band (k-means once put a 761 m deck in a different band than the 695 m one next to it)
    std::vector<double> bases;
    for (const auto& t : todo) for (int k = 0; k < K; ++k) if (real(t.rec.info.cloud_layers[k])) bases.push_back(t.rec.info.cloud_layers[k].alt_base);
    if (bases.empty()) return;
    std::sort(bases.begin(), bases.end());
    std::vector<std::pair<double, size_t>> jumps;   // (size, index after the jump)
    for (size_t i = 1; i < bases.size(); ++i) if (bases[i] - bases[i - 1] >= 300.0) jumps.push_back({bases[i] - bases[i - 1], i});
    std::sort(jumps.begin(), jumps.end(), [](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) { return a.first > b.first; });
    if ((int)jumps.size() > K - 1) jumps.resize((size_t)(K - 1));
    std::vector<size_t> cuts;
    for (const auto& jp : jumps) cuts.push_back(jp.second);
    std::sort(cuts.begin(), cuts.end());
    std::vector<double> limits;   // a base at or above limits[b] belongs to band b+1
    for (size_t c : cuts) limits.push_back(0.5 * (bases[c - 1] + bases[c]));

    auto bandOf = [&](double base) { int b = 0; while (b < (int)limits.size() && base >= limits[b]) ++b; return b; };
    // 2) each record's layers into band slots, in height order (two layers in one band: the upper one moves up a slot, if there is one)
    const size_t n = todo.size();
    std::vector<std::array<bool, 3>> isReal(n);
    for (size_t i = 0; i < n; ++i) {
        XPLMWeatherInfoClouds_t* L = todo[i].rec.info.cloud_layers;
        std::vector<XPLMWeatherInfoClouds_t> mine;
        for (int k = 0; k < K; ++k) if (real(L[k])) mine.push_back(L[k]);
        std::sort(mine.begin(), mine.end(), [](const XPLMWeatherInfoClouds_t& a, const XPLMWeatherInfoClouds_t& b) { return a.alt_base < b.alt_base; });
        XPLMWeatherInfoClouds_t out[3] = {};
        std::array<bool, 3> used = {false, false, false};
        int next = 0;
        for (size_t m = 0; m < mine.size(); ++m) {
            const int bb = bandOf(mine[m].alt_base);
            int slot = std::max(next, bb);
            slot = std::min(slot, K - (int)(mine.size() - m));   // leave room for the layers still to come
            out[slot] = mine[m]; used[slot] = true; next = slot + 1;
        }
        for (int k = 0; k < K; ++k) L[k] = out[k];
        isReal[i] = used;
    }
    // 2b) Smooth cloud edges (2026-11-04: "no sign of a square grid anywhere"). Each record stood for its own square or tile, so where a
    // cloudy one met a clear one the edge of the deck ran along the record grid. Now every layer's coverage, base, top and type is a
    // Gaussian-weighted mean over the records around it (sigma cloud_smooth_nm, each record weighted by its area), with a gentle
    // random wobble in the coverage (fixed to the ground), so a deck thins out over several miles along a curve that has nothing to do
    // with the grid. Airport records keep exactly their own METAR layers (they are used as sources only), and ground fog stays local.
    if (g_cfg.cloudSmoothNm > 0.0) {
        // sigma grows with the record's own size, so the coarse 30 nm tiles further out are smoothed against their neighbours too
        auto sigOf = [&](size_t i) { return std::max(g_cfg.cloudSmoothNm, 0.7 * (double)todo[i].rec.info.radius_nm); };
        auto isFog = [&](size_t i, int k) { const auto& c = todo[i].rec.info.cloud_layers[k]; return isReal[i][k] && c.alt_base < todo[i].rec.elev + 250.0; };
        struct Acc { double w = 0, wc = 0, cb = 0, ct = 0, cy = 0; };
        std::vector<std::array<XPLMWeatherInfoClouds_t, 3>> out(n);
        std::vector<std::array<bool, 3>> outReal(n);
        for (size_t i = 0; i < n; ++i) {
            const XPLMWeatherInfoClouds_t* L = todo[i].rec.info.cloud_layers;
            for (int k = 0; k < K; ++k) { out[i][k] = L[k]; outReal[i][k] = isReal[i][k]; }
            if (todo[i].rec.airport) continue;
            Acc a[3];
            const double sig = sigOf(i), reach = 3.0 * sig;
            for (size_t j = 0; j < n; ++j) {
                const double d = wx::distNm(todo[i].rec.lat, todo[i].rec.lon, todo[j].rec.lat, todo[j].rec.lon);
                if (d > reach) continue;
                const double r = std::max(1.0, (double)todo[j].rec.info.radius_nm);
                const double w = std::exp(-d * d / (2.0 * sig * sig)) * r * r;
                for (int k = 0; k < K; ++k) {
                    if (isFog(j, k)) continue;
                    const XPLMWeatherInfoClouds_t& c = todo[j].rec.info.cloud_layers[k];
                    a[k].w += w;
                    if (!isReal[j][k]) continue;
                    a[k].wc += w * c.coverage; a[k].cb += w * c.coverage * c.alt_base; a[k].ct += w * c.coverage * c.alt_top; a[k].cy += w * c.coverage * c.cloud_type;
                }
            }
            for (int k = 0; k < K; ++k) {
                if (isFog(i, k) || a[k].w <= 0) continue;
                double cov = a[k].wc / a[k].w;
                if (cov > 0.02) {   // the wobble: up to +-0.12, smooth over ~15 nm, only where the deck is thinning (not inside a solid one)
                    const double X = todo[i].rec.lon * 60.0 * std::cos(todo[i].rec.lat * 3.14159265358979 / 180.0), Y = todo[i].rec.lat * 60.0;
                    const double edge = 4.0 * cov * (1.0 - cov);   // 1 at 50 %, 0 at 0 and 100 %
                    cov += 0.12 * edge * noise2(X, Y, 15.0, k);
                }
                cov = std::min(1.0, std::max(0.0, cov));
                XPLMWeatherInfoClouds_t& o = out[i][k];
                if (cov < 0.05 || a[k].wc <= 0) { o.coverage = 0; o.cloud_type = 0; o.alt_base = 0; o.alt_top = 0; outReal[i][k] = false; continue; }
                o.coverage = (float)cov;
                o.alt_base = (float)(a[k].cb / a[k].wc);
                o.alt_top = (float)(a[k].ct / a[k].wc);
                // the type is blended too, but a real thunderstorm core (type 3) keeps its type, and nothing is made more convective than
                // the records around it: X-Plane flies type 3 with strong up- and downdrafts
                o.cloud_type = (isReal[i][k] && L[k].cloud_type > 2.0f) ? L[k].cloud_type : (float)std::min(2.0, a[k].cy / a[k].wc);   // (TCU 2.5 and CB 3 keep theirs)
                if (o.alt_top < o.alt_base + 300.0f) o.alt_top = o.alt_base + 300.0f;   // never a thin sliver (X-Plane dropped an update full of them)
                o.alt_top = std::min(o.alt_top, o.alt_base + wx::xpMaxThicknessM(o.cloud_type));   // and never thicker than X-Plane allows for the type
                if (o.cloud_type < 0.5f) o.coverage = std::min(o.coverage, 0.75f);   // cirrus stays thin (and out of reach of solid-layer effects)
                outReal[i][k] = true;
            }
        }
        for (size_t i = 0; i < n; ++i) {
            for (int k = 0; k < K; ++k) todo[i].rec.info.cloud_layers[k] = out[i][k];
            isReal[i] = outReal[i];
        }
    }
    if (ph <= 0.0f) {
        for (size_t i = 0; i < n; ++i) for (int k = 0; k < K; ++k) { auto& c = todo[i].rec.info.cloud_layers[k]; if (c.coverage <= 0) { c.cloud_type = 0; c.alt_base = 0; c.alt_top = 0; } }
        return;
    }
    // 3) a nearly empty layer in every empty slot: at the height of the nearest record with a real layer in that slot (within 60 nm), and
    // only where it fits whole (at least 400 m deep, clear of the record's own layers). Nothing is ever squeezed into a thin sliver: the
    // build that filled every slot with thin 50-120 m layers stacked on top of each other made X-Plane throw the whole start-up update
    // away (log 2026-11-03 22:23, "XPLMBeginWeatherUpdate() called without matching XPLMEndWeatherUpdate()", no clouds at all).
    // Until 2026-11-12 only slots BELOW a record's own layers were filled. A clear record (or one whose layers were all lower) then sent
    // the slot at 0-0 m, and X-Plane blended the neighbouring deck's base down towards the ground: scattered cloud at 6,600 ft around
    // KBOS hung down in cones and funnels ("odd overcast formation", screenshots 2026-11-12). Now the slot keeps the deck's height there
    // too, so the deck only thins out towards the clear side.
    for (int k = 0; k < K; ++k) {
        std::vector<size_t> has;
        for (size_t i = 0; i < n; ++i) if (isReal[i][k]) has.push_back(i);
        if (has.empty()) continue;
        for (size_t i = 0; i < n; ++i) {
            if (isReal[i][k]) continue;
            XPLMWeatherInfoClouds_t* L = todo[i].rec.info.cloud_layers;
            // above a record's own layers, a filler only directly on top of a filled slot: a gap under it would make X-Plane move it
            // down a slot and blend it with the neighbours' lower deck (a sloping sheet)
            bool above = false;
            for (int m = k + 1; m < K; ++m) if (isReal[i][m]) above = true;
            if (!above && k > 0 && L[k - 1].coverage <= 0.0f) continue;
            double bd = 60.0; size_t bj = n;
            for (size_t j : has) {
                const double d = wx::distNm(todo[i].rec.lat, todo[i].rec.lon, todo[j].rec.lat, todo[j].rec.lon);
                if (d < bd) { bd = d; bj = j; }
            }
            if (bj == n) continue;
            const XPLMWeatherInfoClouds_t& src = todo[bj].rec.info.cloud_layers[k];
            if (src.alt_top - src.alt_base < 400.0f) continue;
            const float lo = k > 0 && L[k - 1].coverage > 0 ? L[k - 1].alt_top : -1e9f;
            float hi = 1e9f;
            for (int m = k + 1; m < K; ++m) if (isReal[i][m]) { hi = L[m].alt_base; break; }
            if (src.alt_base < lo || src.alt_top > hi) continue;   // does not fit whole: leave the slot empty
            L[k] = src;
            L[k].coverage = ph;
            // a filler is (almost) invisible; as stratus it brings none of the up- and downdrafts of a convective cloud (cirrus stays cirrus)
            if (L[k].cloud_type >= 0.5f) L[k].cloud_type = 1.0f;
            L[k].alt_top = std::min(L[k].alt_top, L[k].alt_base + wx::xpMaxThicknessM(L[k].cloud_type));
        }
    }
    // the empty slots go out as before (0 m, no coverage), so X-Plane drops them
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < K; ++k) {
            XPLMWeatherInfoClouds_t& c = todo[i].rec.info.cloud_layers[k];
            if (c.coverage <= 0.0f) { c.cloud_type = 0; c.coverage = 0; c.alt_base = 0; c.alt_top = 0; }
        }
}

void sendRec(Todo& t) {
    // The altimeter read below sea level at KSLC with the ATIS setting dialled in: X-Plane took our QNH in pressure_alt as the pressure at
    // the station's elevation (1016 hPa at 4227 ft instead of at sea level), about 4,000 ft of error in the mountains and a different error
    // in every record whose elevation differs. A QNH is the sea-level pressure of the standard atmosphere, so it now goes in pressure_sl,
    // which X-Plane uses whenever pressure_alt is 0; the record we keep (and compare against) is unchanged. qnh_as_sea_level = 0 undoes it.
    XPLMWeatherInfo_t sendInfo = t.rec.info;
    if (g_cfg.qnhAsSeaLevel && sendInfo.pressure_alt > 0) { sendInfo.pressure_sl = sendInfo.pressure_alt; sendInfo.pressure_alt = 0.0f; }
    if (t.rec.airport) XPLMSetWeatherAtAirport(t.rec.icao.c_str(), &sendInfo);
    // X-Plane calibrates a location record's QNH at the ground altitude passed here. Gap-fill and radar records used to pass the nearest
    // station's elevation, which jumps by thousands of feet between neighbouring tiles in the mountains; with one smooth QNH field that
    // still gave each record a slightly different pressure at the same height. They now all calibrate at sea level (QNH = sea-level pressure).
    else XPLMSetWeatherAtLocation(t.rec.lat, t.rec.lon, (t.rec.icao.empty() && g_cfg.fillQnhAtSeaLevel) ? 0.0 : t.rec.elev, &sendInfo);
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

// ---- Realistic turbulence (docs/turbulence.md) ---------------------------------------------------------------------------------------
// The worker (turbfetch.cpp) keeps NOAA GTGN (US) and ECMWF Open Data (elsewhere) current; here, once a second, the EDR of the air at the
// aircraft's position is taken at each altitude of X-Plane's region turbulence array (the 13 wind layers), smoothed over ~20 s, scaled
// by Max turbulence, mapped to X-Plane's 0-3 response through the measured calibration, and written every 5 s (at most 0.5 per write).
// Only sim/weather/region/turbulence is written. When the feature is turned off, X-Plane's own values from before are put back.
double awGet(XPLMDataRef r);
wx::turb::Fetcher g_turbFetch;
std::shared_ptr<const wx::hz::Snapshot> g_mapReports;   // latest pilot reports, for the map, the turbulence floor and windshear odds
struct TurbState {
    XPLMDataRef arr = nullptr, alts = nullptr, gN = nullptr, simT = nullptr, elev = nullptr;
    int n = 0;
    bool init = false, writing = false, everData = false, loggedNoArr = false;
    std::vector<float> saved, sent;
    std::vector<double> edrS;
    std::vector<bool> has;      // the layer has a value (its own data, or filled from the layers around it)
    std::vector<bool> real;     // the layer had data of its own at the last sample (smoothing only runs between real values)
    Clock::time_point nextSample, nextWrite, nextLog;
    double sg = 0, sg2 = 0; int cnt = 0;
    std::string lastSource;
} g_ts;

// What the weather window shows about turbulence and windshear (main thread only)
struct UiTurbState {
    double edr = -1;
    std::string source, data;
    std::vector<wx::UiTurbLayer> profile;
    std::vector<bool> filled;   // the layer had no data of its own (filled from the layers around it)
    std::vector<wx::UiTurbCell> grid;   // turbulence around the aircraft at the map's altitude, for the map (uiTurbGridTick)
    Clock::time_point gridAt{};
    double gridDLat = 0.2, gridDLon = 0.27, gridAltFt = -1e9;
    long gridSeq = 0;
    int wsSources = 0, wsHigh = 0, wsMed = 0, wsLow = 0;
    std::string wsStrongest;
} g_uiTs;

void turbLog(const char* fmt, ...) {
    char buf[900]; va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    XPLMDebugString((std::string("[WXPlane Turbulence] ") + buf + "\n").c_str());
}

std::string utcHM(std::time_t t) {
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[16]; std::strftime(b, sizeof b, "%H:%MZ", &g); return b;
}

void turbRestore(const char* why) {
    TurbState& t = g_ts;
    if (!t.writing) return;
    if (t.arr && (int)t.saved.size() == t.n) XPLMSetDatavf(t.arr, t.saved.data(), 0, t.n);
    t.writing = false;
    turbLog("%s: X-Plane's region turbulence put back", why);
}

void turbulenceTick() {
    for (const auto& s : g_turbFetch.takeLog()) turbLog("%s", s.c_str());
#ifndef WX_HARNESS
    g_uiOpen = wx::xpui::isOpen();
#endif
    TurbState& t = g_ts;
    if (!t.init) {
        t.init = true;
        t.arr = XPLMFindDataRef("sim/weather/region/turbulence");
        t.alts = XPLMFindDataRef("sim/weather/region/wind_altitude_msl_m");
        t.gN = XPLMFindDataRef("sim/flightmodel/forces/g_nrml");
        t.simT = XPLMFindDataRef("sim/time/total_running_time_sec");
        t.elev = XPLMFindDataRef("sim/flightmodel/position/elevation");
        if (t.arr) t.n = XPLMGetDatavf(t.arr, nullptr, 0, 0);
        if (t.n < 0 || t.n > 64) t.n = 0;
    }
    const double lat = g_drLat ? XPLMGetDatad(g_drLat) : 0, lon = g_drLon ? XPLMGetDatad(g_drLon) : 0;
    const double altFt = t.elev ? XPLMGetDatad(t.elev) / 0.3048 : 0;
    const bool on = g_cfg.enabled && g_cfg.realisticTurbulence && !(lat == 0 && lon == 0);
    // Pilot reports are kept current whenever the weather is on (for the map layer and the floor).
    g_turbFetch.update(lat, lon, altFt, on, g_cfg.enabled && !(lat == 0 && lon == 0));
    g_mapReports = g_turbFetch.reports();
    if (!on) { turbRestore(g_cfg.realisticTurbulence ? "weather off" : "realistic turbulence off"); return; }
    if (!t.arr || !XPLMCanWriteDataRef(t.arr) || t.n <= 0) {
        if (!t.loggedNoArr) { t.loggedNoArr = true; turbLog("sim/weather/region/turbulence %s: realistic turbulence not possible", t.arr ? "is not writable" : "not found"); }
        return;
    }
    if (t.gN) { const double g = XPLMGetDataf(t.gN); t.sg += g; t.sg2 += g * g; ++t.cnt; }
    const auto now = Clock::now();
    if (now < t.nextSample) return;
    t.nextSample = now + std::chrono::seconds(1);

    std::vector<float> altM((size_t)t.n, 0.0f);
    if (t.alts && XPLMGetDatavf(t.alts, altM.data(), 0, t.n) != t.n) for (int k = 0; k < t.n; ++k) altM[k] = (float)(k * 1000.0);
    const wx::turb::Inputs in = g_turbFetch.inputs();
    const std::time_t wall = std::time(nullptr);
    const double simT = t.simT ? XPLMGetDataf(t.simT) : 0.0;
    const std::shared_ptr<const wx::hz::Snapshot> rep = g_cfg.turbReports ? g_mapReports : nullptr;
    if (!in.gtgn && !in.ecA && !rep) return;   // nothing has arrived yet: X-Plane's values stay as they are
    // Pilot reports raise the gridded value where the gridded data does not already contain them:
    // GTGN assimilates pilot reports, so the floor is weighted by (1 - GTGN coverage).
    auto withReports = [&](const wx::turb::Sample& sm, double altQ, std::string* why) {
        double e = std::strcmp(sm.source, "none") != 0 ? sm.edr : 0.0;
        if (rep) {
            const wx::hz::Floor f = wx::hz::floorAt(*rep, lat, lon, altQ, wall);
            const double fl = f.edr * (1.0 - sm.coverage);
            if (fl > e) { e = fl; if (why) *why = f.why; }
        }
        return e;
    };
    if ((int)t.edrS.size() != t.n) { t.edrS.assign((size_t)t.n, 0.0); t.sent.assign((size_t)t.n, 0.0f); t.has.assign((size_t)t.n, false); t.real.assign((size_t)t.n, false); }
    const double scale = std::max(0.0, std::min(1.0, g_cfg.maxTurbulence)), a = 1.0 - std::exp(-1.0 / 20.0);
    for (int k = 0; k < t.n; ++k) {
        const wx::turb::Sample s = wx::turb::sampleAt(in, lat, lon, altM[k] / 0.3048, wall, simT);
        std::string why;
        const double raw = withReports(s, altM[k] / 0.3048, &why);
        const double e = raw * scale;
        const bool had = t.real[k];
        t.has[k] = std::strcmp(s.source, "none") != 0 || !why.empty();
        t.real[k] = t.has[k];
        if (!t.has[k]) t.edrS[k] = 0;
        else t.edrS[k] = (t.everData && had) ? t.edrS[k] + (e - t.edrS[k]) * a : e;
    }
    // Every layer is WXPlane's (2026-11-12, "all turbulence controlled by this plugin, not by X-Plane's own"): a layer with no data
    // (below FL100 outside the US, above the top of the data) takes its value from the layers that have data - between two of them
    // linearly, below the lowest one that value tapering to calm at the ground, above the highest one that value. No data at all: calm.
    {
        std::vector<int> dat;
        for (int k = 0; k < t.n; ++k) if (t.has[k]) dat.push_back(k);
        g_uiTs.filled.assign((size_t)t.n, false);
        for (int k = 0; k < t.n; ++k) {
            if (t.has[k]) continue;
            g_uiTs.filled[(size_t)k] = true;
            double v = 0;
            if (!dat.empty()) {
                int lo = -1, hi = -1;
                for (int j : dat) { if (j < k) lo = j; if (j > k && hi < 0) hi = j; }
                if (lo >= 0 && hi >= 0) { const double f = altM[hi] > altM[lo] ? (altM[k] - altM[lo]) / (altM[hi] - altM[lo]) : 0.5; v = t.edrS[lo] + (t.edrS[hi] - t.edrS[lo]) * f; }
                else if (hi >= 0) v = t.edrS[hi] * std::max(0.0, std::min(1.0, altM[hi] > 0 ? altM[k] / altM[hi] : 1.0));
                else v = t.edrS[lo];
            }
            t.edrS[k] = v;
            t.has[k] = true;
        }
    }
    if (!t.everData) {
        t.everData = true;
    }
    if (now >= t.nextWrite) {
        t.nextWrite = now + std::chrono::seconds(5);
        if (!t.writing) {
            t.saved.assign((size_t)t.n, 0.0f);
            XPLMGetDatavf(t.arr, t.saved.data(), 0, t.n);
            t.writing = true;
            t.nextLog = now;
            for (int k = 0; k < t.n; ++k) t.sent[k] = t.has[k] ? (float)wx::turb::edrToXpLevel(t.edrS[k]) : t.saved[k];   // start where the data is

            turbLog("realistic turbulence on: writing sim/weather/region/turbulence (%d layers) every 5 s; max turbulence %.2f", t.n, scale);
        }
        for (int k = 0; k < t.n; ++k) {
            // Every layer has a value (filled from the layers with data where it has none of its own), so X-Plane's own is never used.
            const double target = t.has[k] ? wx::turb::edrToXpLevel(t.edrS[k]) : t.saved[k];
            t.sent[k] += (float)std::max(-0.5, std::min(0.5, target - t.sent[k]));
        }
        XPLMSetDatavf(t.arr, t.sent.data(), 0, t.n);
    }
    // Log every minute, and when the source changes: what the data says at the aircraft and what X-Plane does with it.
    const wx::turb::Sample here = wx::turb::sampleAt(in, lat, lon, altFt, wall, simT);
    std::string hereWhy;
    const double hereEdr = withReports(here, altFt, &hereWhy);
    const std::string hereSource = hereWhy.empty() ? std::string(here.source) : "pilot report floor";
    {   // for the weather window
        g_uiTs.edr = (here.gtgnEdr >= 0 || here.ecTi >= 0 || !hereWhy.empty()) ? hereEdr : -1;
        g_uiTs.source = hereSource;
        g_uiTs.data.clear();
        if (in.gtgn) g_uiTs.data = "GTGN " + utcHM(in.gtgn->valid);
        if (in.ecA) g_uiTs.data += (g_uiTs.data.empty() ? "" : " · ") + std::string("ECMWF ") + in.ecA->run;
        g_uiTs.profile.clear();
        for (int k = 0; k < t.n; ++k) g_uiTs.profile.push_back({altM[k] / 0.3048, t.edrS.size() > (size_t)k ? t.edrS[k] : 0.0, !(g_uiTs.filled.size() > (size_t)k && g_uiTs.filled[(size_t)k])});
    }
    if (now >= t.nextLog || hereSource != t.lastSource) {
        t.nextLog = now + std::chrono::seconds(60);
        if (hereSource != t.lastSource && !t.lastSource.empty()) turbLog("source changed: %s -> %s", t.lastSource.c_str(), hereSource.c_str());
        t.lastSource = hereSource;
        // requested level at the aircraft's altitude (between the two layers around it) and what X-Plane holds there
        std::vector<float> back((size_t)t.n, 0.0f);
        XPLMGetDatavf(t.arr, back.data(), 0, t.n);
        double req = t.sent[0], held = back[0], edrHere = t.edrS[0];
        const double am = altFt * 0.3048;
        for (int k = 0; k + 1 < t.n; ++k)
            if (am >= altM[k] && am <= altM[k + 1] && altM[k + 1] > altM[k]) {
                const double f = (am - altM[k]) / (altM[k + 1] - altM[k]);
                req = t.sent[k] + (t.sent[k + 1] - t.sent[k]) * f; held = back[k] + (back[k + 1] - back[k]) * f; edrHere = t.edrS[k] + (t.edrS[k + 1] - t.edrS[k]) * f;
            }
        if (am > altM[t.n - 1]) { req = t.sent[t.n - 1]; held = back[t.n - 1]; edrHere = t.edrS[t.n - 1]; }
        std::string data;
        char b[300];
        if (in.gtgn) { std::snprintf(b, sizeof b, "GTGN %s (%ld min old)%s", utcHM(in.gtgn->valid).c_str(), (long)(wall - in.gtgn->valid) / 60, wall - in.gtgn->valid > wx::turb::kGtgnMaxAgeS ? " STALE, not used" : ""); data += b; }
        if (in.ecA) { std::snprintf(b, sizeof b, "%sECMWF %s%s", data.empty() ? "" : ", ", in.ecA->run.c_str(), in.ecB ? (" / " + in.ecB->run.substr(in.ecB->run.find('+'))).c_str() : ""); data += b; }
        std::string parts;
        if (here.gtgnEdr >= 0) { std::snprintf(b, sizeof b, " GTGN EDR %.3f", here.gtgnEdr); parts += b; }
        if (here.ecTi >= 0) { std::snprintf(b, sizeof b, " ECMWF Ellrod TI1 %.1f (EDR-equivalent %.2f, estimate)", here.ecTi, here.ecEdr); parts += b; }
        if (!hereWhy.empty()) { std::snprintf(b, sizeof b, " floor EDR %.3f from %s", hereEdr, hereWhy.c_str()); parts += b; }
        const double m = t.cnt ? t.sg / t.cnt : 1.0, sd = t.cnt > 1 ? std::sqrt(std::max(0.0, t.sg2 / t.cnt - m * m)) : 0.0;
        turbLog("%s | data: %s | aircraft %.0f ft MSL:%s%s; coverage %.2f, variation x%.2f -> EDR %.3f (x max %.2f, smoothed %.3f) -> X-Plane level %.2f requested, %.2f held | load factor sd %.3f g over %d samples",
                hereSource.c_str(), data.c_str(), altFt, parts.empty() ? " no value at this altitude" : parts.c_str(), "", here.coverage, here.noise, hereEdr, scale,
                edrHere, req, held, sd, t.cnt);
        if (g_cfg.diagVerbosity >= 2) {
            std::string L;
            for (int k = 0; k < t.n; ++k) {
                if (t.has[k]) std::snprintf(b, sizeof b, " %.0f:%.2f/%.2f", altM[k] / 0.3048, t.edrS[k], t.sent[k]);
                else std::snprintf(b, sizeof b, " %.0f:X-Plane/%.2f", altM[k] / 0.3048, t.sent[k]);
                L += b;
            }
            turbLog("  layers (ft: EDR/level):%s", L.c_str());
        }
        t.sg = t.sg2 = 0; t.cnt = 0;
    }
}


// The weather window's map: turbulence on a 25 x 25 grid over 300 x 300 nm at the map's altitude (its slider), rebuilt every 10 s and
// whenever the slider has moved (at most 4 times a second, so dragging it stays smooth), while the turbulence layer is shown.
void uiTurbGridTick(double mapAltFt) {
    if ((mapAltFt < 0 && mapAltFt != -2.0) || !(g_cfg.enabled && g_cfg.realisticTurbulence)) return;   // -2: composite
    const auto now = Clock::now();
    const double since = std::chrono::duration<double>(now - g_uiTs.gridAt).count();
    if (!(since >= 10.0 || (std::fabs(mapAltFt - g_uiTs.gridAltFt) > 150.0 && since >= 0.25))) return;
    const wx::turb::Inputs in = g_turbFetch.inputs();
    if (!in.gtgn && !in.ecA) return;
    const double lat = g_drLat ? XPLMGetDatad(g_drLat) : 0, lon = g_drLon ? XPLMGetDatad(g_drLon) : 0;
    if (lat == 0 && lon == 0) return;
    const std::time_t wall = std::time(nullptr);
    const double simT = g_ts.simT ? XPLMGetDataf(g_ts.simT) : 0.0;
    g_uiTs.gridAt = now;
    g_uiTs.gridAltFt = mapAltFt;
    g_uiTs.grid.clear();
    const double kx = std::max(0.2, std::cos(lat * 3.14159265358979 / 180.0));
    for (int gy = 0; gy < 25; ++gy)
        for (int gx = 0; gx < 25; ++gx) {
            const double la = lat + (gy - 12) * 12.0 / 60.0, lo = lon + (gx - 12) * 12.0 / (60.0 * kx);
            if (mapAltFt == -2.0) {   // composite: the most turbulent level in the column (every 3,000 ft, 1,000 to 45,000 ft)
                double e = -1;
                for (double ft = 1000; ft <= 45000; ft += 3000) {
                    const wx::turb::Sample g = wx::turb::sampleAt(in, la, lo, ft, wall, simT);
                    if (std::strcmp(g.source, "none") != 0) e = std::max(e, g.edr);
                }
                if (e >= 0) g_uiTs.grid.push_back({la, lo, e});
                continue;
            }
            const wx::turb::Sample g = wx::turb::sampleAt(in, la, lo, mapAltFt, wall, simT);
            if (std::strcmp(g.source, "none") == 0) continue;
            g_uiTs.grid.push_back({la, lo, g.edr});
        }
    g_uiTs.gridDLat = 12.0 / 60.0; g_uiTs.gridDLon = 12.0 / (60.0 * kx);
    ++g_uiTs.gridSeq;
}

// The weather window's map: the clouds WXPlane gave X-Plane, on a 61 x 61 grid (6 nm) around the aircraft, from the records as sent:
// at each grid point the record that covers it most closely (the smallest one it lies inside: storm cells over gap-fill over
// airports). Rebuilt every 5 s while the window is open, and when the aircraft has moved 6 nm.
std::shared_ptr<const wx::UiCloudGrid> g_uiClouds;
Clock::time_point g_uiCloudsAt{};
long g_uiCloudsSeq = 0;
void uiCloudsTick() {
    const double lat = g_drLat ? XPLMGetDatad(g_drLat) : 0, lon = g_drLon ? XPLMGetDatad(g_drLon) : 0;
    if (lat == 0 && lon == 0) return;
    const auto now = Clock::now();
    static double atLat = 0, atLon = 0;
    if (g_uiClouds && std::chrono::duration<double>(now - g_uiCloudsAt).count() < 5.0 && wx::distNm(lat, lon, atLat, atLon) < 6.0) return;
    g_uiCloudsAt = now; atLat = lat; atLon = lon;
    auto g = std::make_shared<wx::UiCloudGrid>();
    const int N = 61, H = N / 2;
    g->nx = g->ny = N;
    g->dLat = 0.1;
    const double kx = std::max(0.2, std::cos(std::round(lat) * 3.14159265358979 / 180.0));   // fixed per degree of latitude, so the grid does not shift
    g->dLon = 0.1 / kx;
    g->lat0 = std::round(lat / g->dLat) * g->dLat - H * g->dLat;
    g->lon0 = std::round(lon / g->dLon) * g->dLon - H * g->dLon;
    const size_t cells = (size_t)N * (size_t)N;
    g->n.assign(cells, 0);
    g->layers.assign(cells * 3, wx::UiCloudLayer{});
    std::vector<float> bestR(cells, 1e9f), bestF(cells, 1e9f);
    std::vector<const Rec*> best(cells, nullptr);
    const double latMax = g->lat0 + (N - 1) * g->dLat, lonMax = g->lon0 + (N - 1) * g->dLon;
    for (const auto& kv : g_recs) {
        const Rec& r = kv.second;
        const double rad = std::max(0.5, (double)r.info.radius_nm);
        const double rLat = rad / 60.0, rLon = rad / (60.0 * kx);
        if (r.lat + rLat < g->lat0 || r.lat - rLat > latMax || r.lon + rLon < g->lon0 || r.lon - rLon > lonMax) continue;
        const int y0 = std::max(0, (int)std::floor((r.lat - rLat - g->lat0) / g->dLat)), y1 = std::min(N - 1, (int)std::ceil((r.lat + rLat - g->lat0) / g->dLat));
        const int x0 = std::max(0, (int)std::floor((r.lon - rLon - g->lon0) / g->dLon)), x1 = std::min(N - 1, (int)std::ceil((r.lon + rLon - g->lon0) / g->dLon));
        for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix) {
                const double dy = (g->lat0 + iy * g->dLat - r.lat) * 60.0, dx = (g->lon0 + ix * g->dLon - r.lon) * 60.0 * kx;
                const double dd = std::sqrt(dx * dx + dy * dy);
                if (dd > rad) continue;
                const size_t c = (size_t)iy * N + ix;
                const float fr = (float)(dd / rad);
                if ((float)rad < bestR[c] - 0.01f || (std::fabs((float)rad - bestR[c]) <= 0.01f && fr < bestF[c])) { bestR[c] = (float)rad; bestF[c] = fr; best[c] = &r; }
            }
    }
    for (size_t c = 0; c < cells; ++c) {
        if (!best[c]) continue;
        int n = 0;
        for (int k = 0; k < XPLM_NUM_CLOUD_LAYERS && n < 3; ++k) {
            const XPLMWeatherInfoClouds_t& L = best[c]->info.cloud_layers[k];
            if (L.coverage <= 0.01f || L.alt_top <= L.alt_base) continue;
            g->layers[c * 3 + (size_t)n++] = wx::UiCloudLayer{L.alt_base / 0.3048f, L.alt_top / 0.3048f, L.coverage, L.cloud_type};
        }
        g->n[c] = (unsigned char)std::max(1, n);   // 1 with an empty layer: a record is there, with no cloud
        if (n == 0) g->layers[c * 3] = wx::UiCloudLayer{};
    }
    g->seq = ++g_uiCloudsSeq;
    g_uiClouds = g;
}

// ---- Map layer: pilot reports (with its own toggle in X-Plane's map layer list) ----------------------
XPLMMapLayerID g_mapPireps = nullptr;
bool inBounds(const float* bd, float x, float y) { return x >= bd[0] && x <= bd[2] && y <= bd[1] && y >= bd[3]; }

void drawPireps(XPLMMapLayerID, const float* bd, float, float uiUnit, XPLMMapStyle, XPLMMapProjectionID proj, void*) {
    if (!g_mapReports || !wxgl::load()) return;
    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
    const float r = 5 * uiUnit;
    for (const auto& p : g_mapReports->pireps) {
        if (p.intensity.empty() && !p.llws) continue;   // only reports about turbulence or low-level wind shear
        float x, y; XPLMMapProject(proj, p.lat, p.lon, &x, &y);
        if (!inBounds(bd, x, y)) continue;
        float c[3] = {1.0f, 0.55f, 0.0f};                  // routine report (UA): orange
        if (p.urgent) { c[0] = 1.0f; c[1] = 0.1f; c[2] = 0.1f; }   // urgent (UUA): red
        const float rr = p.urgent ? r * 1.5f : r;
        wxgl::color(c[0], c[1], c[2], 0.95f);
        wxgl::begin(wxgl::TRIANGLE_FAN);   // a triangle pointing up, the usual turbulence-report mark
        wxgl::vertex(x, y + rr); wxgl::vertex(x - rr, y - rr * 0.8f); wxgl::vertex(x + rr, y - rr * 0.8f);
        wxgl::end();
        wxgl::color(0, 0, 0, 0.8f); wxgl::lineWidth(1);
        wxgl::begin(wxgl::LINE_LOOP); wxgl::vertex(x, y + rr); wxgl::vertex(x - rr, y - rr * 0.8f); wxgl::vertex(x + rr, y - rr * 0.8f); wxgl::end();
    }
}

void labelPireps(XPLMMapLayerID layer, const float* bd, float, float uiUnit, XPLMMapStyle, XPLMMapProjectionID proj, void*) {
    if (!g_mapReports) return;
    std::vector<std::pair<std::pair<float, float>, const wx::hz::Pirep*>> vis;
    for (const auto& p : g_mapReports->pireps) {
        if (p.intensity.empty() && !p.llws) continue;
        float x, y; XPLMMapProject(proj, p.lat, p.lon, &x, &y);
        if (inBounds(bd, x, y)) vis.push_back({{x, y}, &p});
    }
    if (vis.size() > 80) return;   // zoomed out over a busy area: marks only, labels would overlap
    for (const auto& v : vis) {
        const wx::hz::Pirep& p = *v.second;
        std::tm g{}; std::time_t t = p.obsTime;
#ifdef _WIN32
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        char b[120];
        std::snprintf(b, sizeof b, "%s%s%s %s %s %02d%02dZ", p.urgent ? "UUA " : "", p.llws ? "LLWS " : "", p.intensity.c_str(),
                      p.altFt >= 0 ? ("FL" + std::to_string((int)std::lround(p.altFt / 100))).c_str() : "", p.acType.c_str(), g.tm_hour, g.tm_min);
        XPLMDrawMapLabel(layer, b, v.first.first, v.first.second - 9 * uiUnit, xplm_MapOrientation_UI, 0);
    }
}

void mapLayerDeleted(XPLMMapLayerID l, void*) { if (l == g_mapPireps) g_mapPireps = nullptr; }

void createMapLayers() {
    XPLMCreateMapLayer_t p;
    std::memset(&p, 0, sizeof p);
    p.structSize = sizeof p;
    p.mapToCreateLayerIn = XPLM_MAP_USER_INTERFACE;
    p.layerType = xplm_MapLayer_Markings;
    p.willBeDeletedCallback = mapLayerDeleted;
    p.showUiToggle = 1;
    if (!g_mapPireps) {
        p.drawCallback = drawPireps; p.labelCallback = labelPireps; p.layerName = "WXPlane: Pilot reports (turbulence)";
        g_mapPireps = XPLMCreateMapLayer(&p);
    }
}
void mapCreated(const char* id, void*) { if (g_pluginEnabled && std::strcmp(id, XPLM_MAP_USER_INTERFACE) == 0) createMapLayers(); }
void destroyMapLayers() {
    if (g_mapPireps) { XPLMDestroyMapLayer(g_mapPireps); g_mapPireps = nullptr; }
}

// ---- Dry runways (2026-11-06) ---------------------------------------------------------------------------------------------------------
// X-Plane can leave the pavement wet (its own weather before WXPlane took over, or rain that has long stopped). Near an airport whose
// reports show no precipitation for dry_runway_min minutes (current report, older reports of the last ~70 min, begin/end remarks, hourly
// amounts, recent-weather groups), the sim's runway state is set to dry - only when it is wet or puddly, never when it shows snow or ice,
// and never where WXPlane itself sends rain. The runway state is region-wide in X-Plane, so it follows the airport the aircraft is at
// or approaching (within 8 nm, below 3,000 ft AGL). Everything it sees and does is logged with the prefix "dry runway".
// Read / write a dataref by its own type (runway_friction: the weather screen's ground condition, 2026-11-06; read as an int it gave 0
// whatever the screen said, so a wet ground was never seen as wet).
double drGetNum(XPLMDataRef r) {
    const XPLMDataTypeID t = XPLMGetDataRefTypes(r);
    if (t & xplmType_Float) return XPLMGetDataf(r);
    if (t & xplmType_Double) return XPLMGetDatad(r);
    return XPLMGetDatai(r);
}
void drSetNum(XPLMDataRef r, double v) {
    const XPLMDataTypeID t = XPLMGetDataRefTypes(r);
    if (t & xplmType_Float) XPLMSetDataf(r, (float)v);
    else if (t & xplmType_Double) XPLMSetDatad(r, v);
    else XPLMSetDatai(r, (int)std::lround(v));
}

void dryRunwayTick() {
    // Once per load (2026-11-06), as early as possible: the decision is made the moment the first reports are in, before the first
    // weather has faded in, and it is held through the load. X-Plane sets its own start-up ground state while it loads (standing water
    // at Daytona Beach in dry weather), so for 90 s after the decision any wet or standing-water value X-Plane puts back is set to dry
    // again (checked every 2 s). After that X-Plane wets and dries the ground with the weather itself (it held standing water in rain at
    // KORF on its own). A jump of more than 50 nm between two samples (a new flight, a reposition) is a new load.
    enum Phase { kIdle, kHold, kDone };
    if (!g_cfg.enabled) return;   // WXPlane off: the ground is X-Plane's
    static Phase phase = kIdle;
    static Clock::time_point holdUntil{}, next{};
    static std::string icao, why;
    static bool dry = false;
    static double dist = 0;
    const auto nowC = Clock::now();
    if (nowC < next) return;
    next = nowC + std::chrono::seconds(2);
    const double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
    // The reports: WXPlane's own weather once it is in; before that, a quick fetch of just the airports around the aircraft, started on
    // the first frame, so the ground is decided before the first weather even arrives (that waits for the scenery to settle).
    static std::mutex earlyMx;
    static std::vector<wx::Station> early;
    static bool earlyStarted = false, earlyDone = false;
    static int earlyGen = 0;          // a fetch started before a new load is thrown away when it lands
    static std::time_t jumpWall = 0;  // when the last new load was seen (wall clock, to compare with the weather's fetch time)
    // A new load is a jump between two samples 2 s apart (a reposition or a new flight): 50 nm in 2 s cannot be flown. (Measuring from
    // where the decision was made re-ran the check every 50 nm of flight, seen in the 2026-11-07 cruise log.) A new load gets its own
    // quick fetch at the new position: WXPlane's last weather is still the old place's (2026-11-10 customer log, ENGM -> ENAL:
    // "loaded more than 8 nm from any reporting airport", the old airports were all that was known).
    static double prevLat = 0, prevLon = 0;
    static bool havePrev = false, afterJump = false;
    if (havePrev && wx::distNm(lat, lon, prevLat, prevLon) > 50.0) {
        if (phase != kIdle) phase = kIdle;
        afterJump = true;
        jumpWall = std::time(nullptr);
        std::lock_guard<std::mutex> l(earlyMx);
        ++earlyGen; earlyStarted = false; earlyDone = false; early.clear();
    }
    prevLat = lat; prevLon = lon; havePrev = true;
    const std::vector<wx::Station>* src = nullptr;
    if (!afterJump && g_haveLast && !g_lastRes.stations.empty()) src = &g_lastRes.stations;
    else {
        if (!earlyStarted && (lat != 0.0 || lon != 0.0)) {
            earlyStarted = true;
            const std::string url = wx::awcMetarUrl(lat, lon, 15.0);
            int gen;
            { std::lock_guard<std::mutex> l(earlyMx); gen = earlyGen; }
            std::thread([url, gen] {
                std::string body, err; long st = 0;
                std::vector<wx::Station> v;
                if (wx::httpGet(url, body, st, err, 12)) v = wx::parseAwcMetarJson(body);
                std::lock_guard<std::mutex> l(earlyMx);
                if (gen != earlyGen) return;   // a new load happened meanwhile: this answer is for the old place
                early = std::move(v);
                earlyDone = true;
            }).detach();
        }
        std::lock_guard<std::mutex> l(earlyMx);
        if (!earlyDone) return;
        if (!early.empty()) src = &early;
        else if (afterJump && g_haveLast && !g_lastRes.stations.empty() && g_lastRes.fetchedAt >= jumpWall) src = &g_lastRes.stations;   // WXPlane's weather for the new place
        else return;   // nothing came back: the decision waits for WXPlane's own weather
    }
    if (phase == kDone) return;
    static XPLMDataRef drFr = XPLMFindDataRef("sim/weather/region/runway_friction"), drAgl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
    if (!drFr || !XPLMCanWriteDataRef(drFr)) {
        if (phase == kIdle) logf("dry runway: sim/weather/region/runway_friction %s, nothing done", drFr ? "is read-only" : "not found");
        phase = kDone;
        return;
    }
    // X-Plane's ground condition (the weather screen's "runway condition", log 2026-11-06): 0 dry, 1-3 wet, 4-6 standing water,
    // 7-9 snow (snow_cover 0.25 / 0.5 / 1), 10-12 ice, 13-15 snow and ice. It covers all paved surfaces, taxiways and aprons as well.
    const int fr = (int)std::lround(drGetNum(drFr));
    if (phase == kIdle) {
        const double agl = drAgl ? XPLMGetDataf(drAgl) / 0.3048 : 0.0;
        const wx::Station* best = nullptr; double bd = 1e9;
        for (const auto& s : *src) {
            if (s.icao.compare(0, 3, "GFS") == 0) continue;   // the global model's open-water points are not airports
            const double d = wx::distNm(lat, lon, s.lat, s.lon); if (d < bd) { bd = d; best = &s; }
        }
        if (!best || bd > 8.0 || agl > 3000.0) {
            logf("dry runway: loaded %s - no check this time", agl > 3000.0 ? "in the air above 3,000 ft AGL" : "more than 8 nm from any reporting airport");
            phase = kDone;
            return;
        }
        icao = best->icao; dist = bd;
        dry = wx::runwayShouldBeDry(*best, std::time(nullptr), g_cfg.dryRunwayMin, &why);
        auto it = g_recs.find(best->icao);
        if (dry && it != g_recs.end() && it->second.info.precip_rate > 0.0f) { dry = false; why = "WXPlane sends rain there (radar)"; }
        logf("dry runway: %s (%.1f nm, %s): %s - runway state %d now; %s%s", icao.c_str(), dist, src == &g_lastRes.stations ? "WXPlane's weather" : "quick fetch before the first weather", why.c_str(), fr,
             dry ? "keeping it dry through the load (90 s)" : "left to X-Plane", best->rainWhy.empty() ? "" : (" [" + best->rainWhy + "]").c_str());
        if (!dry) { phase = kDone; return; }
        phase = kHold;
        holdUntil = nowC + std::chrono::seconds(90);
    }
    if (phase == kHold) {
        if (fr >= 1 && fr <= 6) {
            drSetNum(drFr, 0);
            const int after = (int)std::lround(drGetNum(drFr));
            logf("dry runway: %s - runway state %d -> %d%s", icao.c_str(), fr, after, after != 0 ? " (X-Plane did not take it)" : "");
        }
        if (nowC >= holdUntil) {
            logf("dry runway: %s - load done, runway state %d; X-Plane follows the weather from here", icao.c_str(), (int)std::lround(drGetNum(drFr)));
            phase = kDone;
        }
    }
}

// ---- Realistic windshear (2026-11-06) ------------------------------------------------------------------------------------------------
// The model (windshear.cpp) decides where fields form, from the stations and radar cells of the last update, stepped with simulation time.
// What reaches X-Plane are low-level wind records (gust fronts, microburst outflow) sent through incremental weather updates in this
// pre-flight-model loop: X-Plane fades a record in over ~2 min and delivers 60-80 % of it (probe, 2026-11-06), so each record is sent
// for the state windshear_lead_s ahead and windshear_gain stronger. Downdrafts are modelled and logged, never applied: the weather API
// has no vertical wind. The aircraft is never touched.
void fillInfo(XPLMWeatherInfo_t& info, const wx::XpWx& x);
bool g_disableErase = false;   // plugin was disabled: erase our records from the next callback, then stop
wx::shear::Engine g_ws;
int g_wsCmd = 0;   // 1 enable, -1 disable (set by the menu, done in the loop)
struct WsSent { double lat = 0, lon = 0; float kt = 0, dir = 0; XPLMWeatherInfo_t info; };
std::map<std::string, WsSent> g_wsSent;

void wsLog(const char* fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    XPLMDebugString((std::string("[WXPlane Windshear] ") + buf + "\n").c_str());
}

void windshearForget() { g_wsSent.clear(); g_ws.clear(); }

void windshearResend() {   // inside an open XPLMBeginWeatherUpdate that replaces all our records
    if (g_wsSent.empty()) return;
    for (auto& kv : g_wsSent) { XPLMWeatherInfo_t i = kv.second.info; XPLMSetWeatherAtLocation(kv.second.lat, kv.second.lon, 0.0, &i); }
    wsLog("%zu windshear record(s) re-sent with WXPlane's replacing update", g_wsSent.size());
}

double wsSimTime() {
    static XPLMDataRef dr = XPLMFindDataRef("sim/time/total_running_time_sec");   // simulation time: stops while paused, runs faster with time acceleration
    return dr ? (double)XPLMGetDataf(dr) : 0.0;
}

wx::shear::Env wsEnv(double lat, double lon) {
    wx::shear::Env e;
    e.stations = &g_lastRes.stations;
    e.cells = &g_lastRes.radar.cells;
    // steering wind: the profile at the aircraft at 10,000 ft (FD / global model)
    wx::Station b = wx::blendAt(g_lastRes.stations, lat, lon);
    wx::XpWx x = wx::buildXp(b, g_cfg, std::time(nullptr), &g_lastRes.aloft, false);
    if (x.winds.size() >= 2) {
        const wx::XpWind w = wx::windAt(x.winds, 3048.0f);
        if (std::isfinite(w.dir) && std::isfinite(w.ms)) { e.steerDirDeg = w.dir; e.steerKt = w.ms / 0.514444f; }
    }
    return e;
}

// The record X-Plane gets for one windshear record: the normal weather of that place, its winds up to topAglFt above the ground
// being the place's own wind plus the outflow vector.
void wsBuildInfo(const wx::shear::ShearRecord& r, XPLMWeatherInfo_t& info, double& groundM) {
    wx::Station b = wx::blendAt(g_lastRes.stations, r.lat, r.lon);
    wx::XpWx x = wx::buildXp(b, g_cfg, std::time(nullptr), &g_lastRes.aloft, false);
    groundM = b.elevM;
    const float topM = (float)(b.elevM + r.topAglFt * 0.3048);
    const float rr = r.towardDeg * 3.14159265f / 180.0f, addE = r.kt * 0.514444f * std::sin(rr), addN = r.kt * 0.514444f * std::cos(rr);
    bool any = false;
    for (auto& w : x.winds) {
        if (w.altMsl > topM) continue;
        const float wr = w.dir * 3.14159265f / 180.0f;
        const float e = -w.ms * std::sin(wr) + addE, n = -w.ms * std::cos(wr) + addN;   // FROM direction -> toward components
        w.ms = std::sqrt(e * e + n * n);
        w.dir = std::fmod(std::atan2(-e, -n) * 180.0f / 3.14159265f + 360.0f, 360.0f);
        w.gustMs = std::max(w.gustMs, w.ms);
        any = true;
    }
    if (!any) {
        wx::XpWind w; w.altMsl = (float)b.elevM + 10.0f; w.ms = r.kt * 0.514444f; w.dir = std::fmod(r.towardDeg + 180.0f, 360.0f); w.gustMs = w.ms;
        x.winds.insert(x.winds.begin(), w);
    }
    fillInfo(info, x);
    info.radius_nm = r.radiusNm;
    if (info.pressure_alt > 0) { info.pressure_sl = info.pressure_alt; info.pressure_alt = 0; }
}

void windshearTick() {
    static double nextEval = -1, nextSend = -1, nextExp = -1;
    static double lastLat = 0, lastLon = 0;
    static std::time_t lastRisk = 0;
    static std::string lastRiskLine;
    if (!g_haveLast || g_lastRes.stations.empty() || g_disableErase) return;
    if (!g_cfg.enabled) {   // WXPlane off: its windshear records went with the rest (eraseAll); nothing is computed or sent again
        if (!g_wsSent.empty() || !g_ws.fields().empty()) windshearForget();
        return;
    }
    const double simT = wsSimTime();
    const double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(simT)) return;
    bool sendNow = false;
    static bool seeded = false;
    if (!seeded && g_cfg.windshear && g_wsCmd == 0) g_wsCmd = 1;   // on from the start (the default): same set-up as turning it on
    if (g_wsCmd == 1) seeded = true;
    if (g_wsCmd != 0) {
        const int cmd = g_wsCmd; g_wsCmd = 0;
        if (cmd == 1) { g_ws.setSeed((uint32_t)g_cfg.windshearSeed); wsLog("realistic windshear enabled (seed %d)", g_cfg.windshearSeed); nextEval = simT; lastRiskLine.clear(); }
        else if (cmd == -1) {
            if (!g_wsSent.empty()) {
                XPLMBeginWeatherUpdate();
                for (const auto& kv : g_wsSent) XPLMEraseWeatherAtLocation(kv.second.lat, kv.second.lon);
                XPLMEndWeatherUpdate(1, 0);
            }
            wsLog("realistic windshear disabled: %zu field(s) and %zu record(s) removed", g_ws.fields().size(), g_wsSent.size());
            windshearForget();
            return;
        }
    }
    const bool any = !g_ws.fields().empty();
    if (!g_cfg.windshear && !any && g_wsSent.empty()) return;
    // a jump (teleport, new flight): fields far away are dropped, not carried to the new place
    if (wx::distNm(lat, lon, lastLat, lastLon) > 50.0) {
        const int n = g_ws.dropFar(lat, lon, 150.0, simT);
        if (n) wsLog("aircraft moved %.0f nm at once: %d far field(s) dropped", wx::distNm(lat, lon, lastLat, lastLon), n);
    }
    lastLat = lat; lastLon = lon;
    // 1) the atmosphere: every 30 s of simulation time
    if (g_cfg.windshear && (nextEval < 0 || simT >= nextEval || simT < nextEval - 3600)) {
        nextEval = simT + 30.0;
        const wx::shear::Env env = wsEnv(lat, lon);
        // only the atmosphere around the aircraft is simulated (within 100 nm); a field formed there keeps existing when the aircraft leaves
        std::vector<wx::shear::Source> src;
        for (const auto& s : wx::shear::assess(env)) if (wx::distNm(lat, lon, s.lat, s.lon) < 100.0) src.push_back(s);
        if (g_mapReports) {   // a low-level wind shear pilot report near a source: its hourly odds +0.025 (owner's rule, 2026-11-06)
            static XPLMDataRef drEl0 = XPLMFindDataRef("sim/flightmodel/position/elevation"), drAgl0 = XPLMFindDataRef("sim/flightmodel/position/y_agl");
            const double groundFt = (drEl0 && drAgl0) ? (XPLMGetDatad(drEl0) - XPLMGetDataf(drAgl0)) / 0.3048 : 0.0;
            const std::time_t wall = std::time(nullptr);
            static std::string lastBoost;
            int raised = 0; const wx::hz::Pirep* by = nullptr;
            for (auto& s : src) {
                const wx::hz::Pirep* p = wx::hz::llwsNear(*g_mapReports, s.lat, s.lon, groundFt, wall);
                if (!p) continue;
                s.pHourGustFront = std::min(1.0, s.pHourGustFront + 0.025);
                s.pHourMicroburst = std::min(1.0, s.pHourMicroburst + 0.025);
                s.why += "; LLWS pilot report nearby: odds +0.025/h";
                ++raised; by = p;
            }
            const std::string key = by ? by->raw : std::string();
            if (by && key != lastBoost) wsLog("low-level wind shear pilot report nearby (%s): hourly odds of a gust front and of a microburst +0.025 at %d source(s)", by->raw.c_str(), raised);
            lastBoost = key;
        }
        int hi = 0, md = 0, lo = 0; std::string top; float topDbz = -1;
        for (const auto& s : src) {
            if (s.confidence == wx::shear::kHigh) ++hi; else if (s.confidence == wx::shear::kMedium) ++md; else ++lo;
            if (s.dbz > topDbz) { topDbz = s.dbz; top = s.why; }
        }
        char rl[300];
        std::snprintf(rl, sizeof rl, "risk: %zu source(s) supporting windshear (confidence high %d, medium %d, low %d)%s%s; steering wind %03.0f/%.0f kt; "
                      "nonconvective shear (fronts, gradients, low-level jets) is in the station and winds-aloft data already, nothing added",
                      src.size(), hi, md, lo, src.empty() ? "" : "; strongest: ", top.c_str(), env.steerDirDeg, env.steerKt);
        const std::time_t now = std::time(nullptr);
        if (rl != lastRiskLine || now - lastRisk > 600) { wsLog("%s", rl); lastRiskLine = rl; lastRisk = now; }
        g_uiTs.wsSources = (int)src.size(); g_uiTs.wsHigh = hi; g_uiTs.wsMed = md; g_uiTs.wsLow = lo; g_uiTs.wsStrongest = top;
        for (const auto& l : g_ws.step(simT, src, env)) wsLog("%s", l.c_str());
        sendNow = true;
    }
    // 2) records to X-Plane (incremental, only between WXPlane's own batches)
    if ((sendNow || simT >= nextSend) && g_pendingTodo.empty() && g_pendingGone.empty()) {
        nextSend = simT + 30.0;
        std::vector<wx::shear::ShearRecord> want = g_ws.records(simT + g_cfg.windshearLeadS, g_cfg.windshearGain);
        std::vector<wx::shear::ShearRecord> inRange;   // not "near": a macro in the Windows headers
        std::sort(want.begin(), want.end(), [&](const wx::shear::ShearRecord& a, const wx::shear::ShearRecord& b) {
            return wx::distNm(lat, lon, a.lat, a.lon) < wx::distNm(lat, lon, b.lat, b.lon); });
        for (const auto& r : want) if (wx::distNm(lat, lon, r.lat, r.lon) < 40.0 && inRange.size() < 80) inRange.push_back(r);
        std::set<std::string> keep;
        int sets = 0, erases = 0;
        bool open = false;
        auto begin = [&]() { if (!open) { XPLMBeginWeatherUpdate(); open = true; } };
        for (const auto& r : inRange) {
            keep.insert(r.key);
            auto it = g_wsSent.find(r.key);
            const bool moved = it != g_wsSent.end() && wx::distNm(it->second.lat, it->second.lon, r.lat, r.lon) > 0.3;
            if (it != g_wsSent.end() && !moved && std::fabs(it->second.kt - r.kt) < 2.0f && std::fabs(it->second.dir - r.towardDeg) < 5.0f) continue;
            begin();
            if (moved) { XPLMEraseWeatherAtLocation(it->second.lat, it->second.lon); ++erases; }
            WsSent s; s.lat = r.lat; s.lon = r.lon; s.kt = r.kt; s.dir = r.towardDeg;
            double g = 0;
            wsBuildInfo(r, s.info, g);
            XPLMWeatherInfo_t i = s.info;
            XPLMSetWeatherAtLocation(r.lat, r.lon, 0.0, &i);
            g_wsSent[r.key] = s;
            ++sets;
        }
        for (auto it = g_wsSent.begin(); it != g_wsSent.end();) {
            if (keep.count(it->first)) { ++it; continue; }
            begin();
            XPLMEraseWeatherAtLocation(it->second.lat, it->second.lon);
            ++erases;
            it = g_wsSent.erase(it);
        }
        if (open) {
            XPLMEndWeatherUpdate(1 /*incremental*/, 0 /*fade in*/);
            wsLog("records: %d sent/updated, %d removed, %zu active (state %.0f s ahead, x%.1f for X-Plane's blending)", sets, erases, g_wsSent.size(),
                  g_cfg.windshearLeadS, g_cfg.windshearGain);
        }
    }
    // 3) the aircraft in a field: entry, exit and the winds every 10 s
    if (simT >= nextExp) {
        nextExp = simT + 1.0;
        static XPLMDataRef drAgl = XPLMFindDataRef("sim/flightmodel/position/y_agl"), drEl = XPLMFindDataRef("sim/flightmodel/position/elevation"),
                           drTrk = XPLMFindDataRef("sim/flightmodel/position/hpath"), drGs = XPLMFindDataRef("sim/flightmodel/position/groundspeed"),
                           drIas = XPLMFindDataRef("sim/flightmodel/position/indicated_airspeed");
        const double agl = drAgl ? XPLMGetDataf(drAgl) / 0.3048 : 1e9, trk = drTrk ? XPLMGetDataf(drTrk) : 0.0;
        const wx::shear::Vec3 p = g_ws.perturbation(lat, lon, agl, simT);
        const double tr = trk * 3.14159265 / 180.0;
        const double head = -(p.east * std::sin(tr) + p.north * std::cos(tr));   // + = headwind added by the field
        const double mag = std::sqrt(p.east * p.east + p.north * p.north);
        static bool inside = false;
        static double maxHead = -1e9, minHead = 1e9, maxDown = 0, enterT = 0;
        static double ambE = 0, ambN = 0, xpMaxHead = -1e9, xpMinHead = 1e9;   // X-Plane's wind just before the field, and its headwind change inside
        const bool now = mag > 3.0 || p.up < -300.0;
        if (now || inside) {
            XPLMWeatherInfo_t w;
            std::memset(&w, 0, sizeof w);
            w.structSize = sizeof w;
            XPLMGetWeatherAtLocation(lat, lon, drEl ? XPLMGetDatad(drEl) : 0.0, &w);
            const double gs = drGs ? XPLMGetDataf(drGs) / 0.514444 : 0, ias = drIas ? XPLMGetDataf(drIas) : 0;
            const double wr = w.wind_dir_alt * 3.14159265 / 180.0, wkt = w.wind_spd_alt / 0.514444;
            const double xe = -wkt * std::sin(wr), xn = -wkt * std::cos(wr);   // X-Plane's wind as a vector (toward)
            if (now && !inside) { ambE = xe; ambN = xn; xpMaxHead = -1e9; xpMinHead = 1e9; }
            const double xpHead = -((xe - ambE) * std::sin(tr) + (xn - ambN) * std::cos(tr));   // headwind X-Plane added since the entry
            if (now && !inside) {
                inside = true; enterT = simT; maxHead = minHead = head; maxDown = p.up;
                wsLog("aircraft ENTERS windshear at %.4f,%.4f, %.0f ft AGL: model adds %.0f kt (headwind %+.0f), vertical %.0f fpm (not applied) | X-Plane wind here %03.0f/%.0f kt, GS %.0f kt, IAS %.0f kt",
                      lat, lon, agl, mag, head, p.up, w.wind_dir_alt, w.wind_spd_alt / 0.514444f, gs, ias);
            }
            if (inside) {
                maxHead = std::max(maxHead, head); minHead = std::min(minHead, head); maxDown = std::min(maxDown, p.up);
                xpMaxHead = std::max(xpMaxHead, xpHead); xpMinHead = std::min(xpMinHead, xpHead);
                if (!now) {
                    inside = false;
                    wsLog("aircraft EXITS windshear after %.0f s: model headwind %+.0f .. %+.0f kt (change %.0f kt); X-Plane headwind %+.0f .. %+.0f kt (change %.0f kt); strongest modelled downdraft %.0f fpm (not applied)",
                          simT - enterT, minHead, maxHead, maxHead - minHead, xpMinHead, xpMaxHead, xpMaxHead - xpMinHead, maxDown);
                }
            }
        }
    }
}

void windshearResend();
void flushPending() {
    if (g_pendingTodo.empty() && g_pendingGone.empty()) return;
    const bool immediateNow = g_pendingImmediate && (g_pendingManual || (g_cfg.firstLoadImmediate && g_pendingStartup));
    if (g_pendingStartup && g_cfg.regenWeatherOnStart && g_xpRealWx && !g_regenDone) {
        g_regenDone = true;   // with X-Plane's real weather on, a regen only makes it download and rebuild all of it again
        logf("not sending regen_weather: X-Plane's real-world weather is on (it would download and rebuild everything again)");
    }
    if (g_pendingStartup && g_cfg.regenWeatherOnStart) {
        // Taking over: first have X-Plane throw away its previous weather (its clouds included), give that a few seconds to
        // take effect, and only then hand over ours, so none of the old clouds are blended into the new weather.
        if (!g_regenDone) {
            g_regenDone = true;
            if (!g_regenCmd) g_regenCmd = XPLMFindCommand("sim/operation/regen_weather");
            if (g_regenCmd) { XPLMCommandOnce(g_regenCmd); g_regenAt = Clock::now(); logf("cleared X-Plane's previous weather (regen_weather) before applying ours"); }
        }
        if (g_regenAt != Clock::time_point{} && Clock::now() < g_regenAt + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(g_cfg.regenWaitS))) return;
    }
    auto t0 = Clock::now();
    // The start-up load always goes to X-Plane as ONE update, whatever apply_per_tick / transition_s say: every extra update is
    // another rebuild, and spreading it out was what made the load-in a couple of minutes of changing clouds and light.
    size_t budget = (g_pendingStartup || g_pendingReplace) ? g_pendingTodo.size() + g_pendingGone.size() : (size_t)g_cfg.applyPerTick, sent = 0, erased = 0;
    if (g_releaseStepS > 0.0 && !g_pendingStartup && !g_pendingReplace) {
        if (t0 < g_nextRelease) return;
        double behind = std::chrono::duration<double>(t0 - g_nextRelease).count();
        budget = std::min(budget, (size_t)(behind / g_releaseStepS) + 1);
        g_nextRelease = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(g_releaseStepS * (double)budget));
    }
    XPLMBeginWeatherUpdate();
    while (budget > 0 && !g_pendingGone.empty()) { eraseRec(g_pendingGone.back()); g_pendingGone.pop_back(); --budget; ++erased; }
    size_t n = std::min(budget, g_pendingTodo.size());
    // Setting the records is what costs the frame (2026-11-06: 61 ms for 316 records, 0 ms in X-Plane's end of update), so an
    // in-flight update is handed over a few milliseconds per tick (apply_frame_ms) as consecutive incremental updates. The start-up
    // load and a replacing update still go as one (X-Plane is rebuilding anyway, and a replacing update cannot be split).
    const bool sliced = g_cfg.applyFrameMs > 0 && !g_pendingStartup && !g_pendingReplace;
    size_t done = 0;
    for (; done < n; ++done) {
        if (sliced && done > 0 && std::chrono::duration<double, std::milli>(Clock::now() - t0).count() >= g_cfg.applyFrameMs) break;
        sendRec(g_pendingTodo[done]);
    }
    n = done;
    const auto tSets = Clock::now();
    sent = n;
    g_pendingTodo.erase(g_pendingTodo.begin(), g_pendingTodo.begin() + (long)n);
    // A replacing update would wipe the windshear cells (it did the probe's record, log 2026-11-06 11:51): they go along with it.
    if (g_pendingReplace) windshearResend();
    XPLMEndWeatherUpdate(g_pendingReplace ? 0 : 1 /*0: replaces all our previous records*/, (g_pendingImmediate && (g_pendingManual || (g_cfg.firstLoadImmediate && g_pendingStartup))) ? 1 : 0);
    if (g_pendingTodo.empty()) g_pendingReplace = false;
    if (sent || erased) { g_lastSendAt = Clock::now(); g_lastSendImmediate = immediateNow; }
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const double msSets = std::chrono::duration<double, std::milli>(tSets - t0).count();
    if (ms > 20.0) logf("weather update took %.0f ms for %zu records, %zu erased (%zu still queued): %.0f ms setting records, %.0f ms in X-Plane's end of update",
                        ms, sent, erased, g_pendingTodo.size() + g_pendingGone.size(), msSets, ms - msSets);
    if (g_pendingTodo.empty() && g_pendingGone.empty()) logf("all weather records handed to X-Plane");
}

// Two records X-Plane would show the same way: differences below what anyone could see or feel. The 2026-11-06 refresh re-sent all
// 316 records (tiny drifts from blending and smoothing), and setting them was the whole main-thread cost. The comparison is against
// what was last sent, so a slow drift still goes out once it adds up.
bool nearlySame(const XPLMWeatherInfo_t& a, const XPLMWeatherInfo_t& b) {
    auto within = [](float x, float y, float tol) { return std::fabs(x - y) <= tol; };
    auto rel = [](float x, float y, float r, float floor) { return std::fabs(x - y) <= std::max(floor, r * std::max(std::fabs(x), std::fabs(y))); };
    if (!within(a.temperature_alt, b.temperature_alt, 0.3f) || !within(a.dewpoint_alt, b.dewpoint_alt, 0.3f)) return false;
    if (!within(a.pressure_alt, b.pressure_alt, 30.0f) || !within(a.pressure_sl, b.pressure_sl, 30.0f)) return false;   // Pa
    if (!rel(a.visibility, b.visibility, 0.03f, 50.0f) || !within(a.precip_rate, b.precip_rate, 0.02f)) return false;
    if (!within(a.radius_nm, b.radius_nm, 0.05f) || !within(a.max_altitude_msl_ft, b.max_altitude_msl_ft, 1.0f)) return false;
    if (!within(a.wave_height, b.wave_height, 0.1f) || a.wave_dir != b.wave_dir || !within(a.thermal_climb, b.thermal_climb, 0.05f)) return false;
    if (!within(a.troposphere_alt, b.troposphere_alt, 150.0f) || !within(a.troposphere_temp, b.troposphere_temp, 1.0f)) return false;
    for (int i = 0; i < XPLM_NUM_TEMPERATURE_LAYERS; ++i)
        if (!within(a.temp_layers[i], b.temp_layers[i], 0.3f) || !within(a.dewp_layers[i], b.dewp_layers[i], 0.3f)) return false;
    for (int i = 0; i < XPLM_NUM_WIND_LAYERS; ++i) {
        const auto &x = a.wind_layers[i], &y = b.wind_layers[i];
        if (!within(x.alt_msl, y.alt_msl, 1.0f) || !within(x.speed, y.speed, 0.3f) || !within(x.gust_speed, y.gust_speed, 0.5f)) return false;
        float dd = std::fabs(std::fmod(x.direction - y.direction + 540.0f, 360.0f) - 180.0f);
        if (std::max(x.speed, y.speed) > 1.0f && dd > 3.0f) return false;
        if (!within(x.shear, y.shear, 2.0f) || !within(x.turbulence, y.turbulence, 0.02f)) return false;
    }
    for (int i = 0; i < XPLM_NUM_CLOUD_LAYERS; ++i) {
        const auto &x = a.cloud_layers[i], &y = b.cloud_layers[i];
        if (!within(x.cloud_type, y.cloud_type, 0.05f) || !within(x.coverage, y.coverage, 0.02f)) return false;
        if (!within(x.alt_base, y.alt_base, 20.0f) || !within(x.alt_top, y.alt_top, 20.0f)) return false;
    }
    return true;
}

// `immediately`: at once (X-Plane rebuilds its weather, the sim pauses for a moment: plugin disable/stop), or faded out over
// X-Plane's usual transition (the window's "WXPlane weather on" switch: no freeze).
void eraseAll(bool immediately = true) {
    g_pendingTodo.clear();
    g_pendingGone.clear();
    g_heldLevel.clear();
    XPLMBeginWeatherUpdate();
    XPLMEndWeatherUpdate(0 /*not incremental: wipes our previous records*/, immediately ? 1 : 0);
    g_recs.clear();
    logf("plugin weather records removed");
}

bool airportCached(const wx::Station& s);
// XPLMFindNavAid searches X-Plane's whole navigation database, so each lookup is slow; with ~40 stations per update this ran
// on the main thread every 5 minutes. The answer never changes for a station, so it is looked up once and kept.
std::map<std::string, bool> g_airportKnownCache;
double g_navLookupMs = 0;   // time spent in lookups during the current update (logged)
bool airportKnown(const wx::Station& s) {
    char key[64];
    std::snprintf(key, sizeof key, "%s %.2f %.2f", s.icao.c_str(), s.lat, s.lon);
    auto c = g_airportKnownCache.find(key);
    if (c != g_airportKnownCache.end()) return c->second;
    const auto t0 = Clock::now();
    bool known = false;
    XPLMNavRef ref = XPLMFindNavAid(nullptr, s.icao.c_str(), nullptr, nullptr, nullptr, xplm_Nav_Airport);
    if (ref != XPLM_NAV_NOT_FOUND) {
        char id[32] = {0};
        float la = 0, lo = 0;
        XPLMGetNavAidInfo(ref, nullptr, &la, &lo, nullptr, nullptr, nullptr, id, nullptr, nullptr);
        known = s.icao == id && wx::distNm(la, lo, s.lat, s.lon) < 5.0;
    }
    g_navLookupMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    g_airportKnownCache[key] = known;
    return known;
}
bool airportCached(const wx::Station& s) {
    char key[64];
    std::snprintf(key, sizeof key, "%s %.2f %.2f", s.icao.c_str(), s.lat, s.lon);
    return g_airportKnownCache.count(key) != 0;
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
    // The sim keeps its wind layers at fixed altitudes of its own (the log showed it ignoring layers placed anywhere else),
    // so when it tells us those altitudes we resample our profile onto them instead of using our own grid.
    std::vector<wx::XpWind> winds = x.winds;
    if (g_haveWindAlts && x.winds.size() >= 2) {
        winds.clear();
        const auto& src = x.winds;
        for (int k = 0; k < XPLM_NUM_WIND_LAYERS; ++k) {
            const float a = g_windAltM[k];
            wx::XpWind w = src.front();
            if (a >= src.back().altMsl) w = src.back();
            else if (a > src.front().altMsl) {
                size_t j = 1;
                while (j + 1 < src.size() && src[j].altMsl < a) ++j;
                const wx::XpWind &lo = src[j - 1], &hi = src[j];
                const float span = hi.altMsl - lo.altMsl, t = span > 0 ? (a - lo.altMsl) / span : 0.0f;
                const float rad = 3.14159265f / 180.0f;
                const float ux = (1 - t) * lo.ms * std::sin(lo.dir * rad) + t * hi.ms * std::sin(hi.dir * rad);
                const float uy = (1 - t) * lo.ms * std::cos(lo.dir * rad) + t * hi.ms * std::cos(hi.dir * rad);
                w = lo;
                w.ms = std::sqrt(ux * ux + uy * uy);
                w.dir = w.ms > 0.01f ? std::fmod(std::atan2(ux, uy) / rad + 360.0f, 360.0f) : lo.dir;
                w.gustMs = (1 - t) * lo.gustMs + t * hi.gustMs;
                w.shearDeg = (1 - t) * lo.shearDeg + t * hi.shearDeg;
                w.turbulence = (1 - t) * lo.turbulence + t * hi.turbulence;
            }
            w.altMsl = a;
            winds.push_back(w);
        }
    }
    for (size_t i = 0; i < winds.size() && i < (size_t)XPLM_NUM_WIND_LAYERS; ++i) {
        XPLMWeatherInfoWinds_t& w = info.wind_layers[i];
        w.alt_msl = winds[i].altMsl;
        w.speed = winds[i].ms;
        w.direction = winds[i].dir;
        w.gust_speed = winds[i].gustMs;
        w.shear = winds[i].shearDeg;
        w.turbulence = winds[i].turbulence;
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
    // The tropopause from the same profile (GFS): left to itself X-Plane derived the standard one and pinned -56.5 C at 11,000 m
    // (FL360 read -57 C where the profile had -52 C, log 2026-11-10). Without a profile that shows one, X-Plane still derives it.
    float tropoM = 0, tropoC = 0;
    if (x.tempProfile.size() >= 3 && wx::findTropopause(x.tempProfile, tropoM, tropoC)) { info.troposphere_alt = tropoM; info.troposphere_temp = tropoC; }
    else { info.troposphere_alt = 0.0f; info.troposphere_temp = kUnset; }   // derive from existing data
    info.age = 0.0f;   // X-Plane weights a record by its age; an old METAR would count for less than its own weather
    info.radius_nm = (float)g_cfg.radiusNm;
    info.max_altitude_msl_ft = x.maxAltFt;
}

// ATIS winds (D-ATIS text, as broadcast) are magnetic; METAR and every other source WXPlane uses (winds aloft, the global model, the
// X-Plane weather API) are true. Each ATIS wind is turned into true here, once, with X-Plane's own magnetic variation at the airport.
// The SDK does not document the sign of XPLMGetMagneticVariation, so it is calibrated once against XPLMDegMagneticToDegTrue at the
// aircraft (whose result is the east variation); with both under 1 deg the declination convention (east positive) is assumed.
// Magnetic variation (degrees, east positive) at a point. X-Plane's sign convention is checked once against its own magnetic-to-true
// conversion, and flipped if it turns out to be west positive.
double magVarEastAt(double lat, double lon) {
    static int sign = 0;
    if (sign == 0) {
        const double alat = XPLMGetDatad(g_drLat), alon = XPLMGetDatad(g_drLon);
        const float v = XPLMGetMagneticVariation(alat, alon);
        float t = XPLMDegMagneticToDegTrue(0.0f);
        if (t > 180.0f) t -= 360.0f;
        if (std::fabs(v) > 1.0f && std::fabs(t) > 1.0f) {
            sign = (v * t > 0) ? 1 : -1;
            logf("magnetic variation: X-Plane gives %.1f at the aircraft, true heading of magnetic north %.1f: %s", v, t, sign > 0 ? "east positive" : "west positive, sign flipped");
        }
    }
    return (sign ? sign : 1) * (double)XPLMGetMagneticVariation(lat, lon);
}

void annotateMagneticVariation(wx::FetchResult& res) {
    for (auto& s : res.stations) {
        wx::Report& r = s.report;
        if (!r.hasWind || !r.windMagnetic) continue;
        const double var = magVarEastAt(s.lat, s.lon);
        const bool calmOrVrb = r.windVariable || r.windKt < 0.5 || r.windDir == 0;
        const int mag = r.windDir, tru = wx::magneticToTrue(mag, var, calmOrVrb);
        r.windDir = tru;
        r.windMagnetic = false;
        if (calmOrVrb) logf("wind reference: %s ATIS %s, no conversion", s.icao.c_str(), r.windKt < 0.5 ? "calm" : "variable");
        else logf("wind reference: %s ATIS %03d MAGNETIC, variation %.1f %s, applied %03d TRUE", s.icao.c_str(), mag, std::fabs(var), var >= 0 ? "EAST" : "WEST", tru);
    }
}

void applyResult(const wx::FetchResult& res, bool fromFetch) {
    // Everything below assumes at least one station (the nearest one, blends between them). With none - the open ocean, 2026-11-09
    // crash report - the weather already set is kept as it is.
    if (res.stations.empty()) { logf("no stations to build weather from: the weather already set is kept"); return; }
    const auto applyT0 = Clock::now();   // this runs on the sim's main thread: logged when it takes long enough to cost a frame
    g_navLookupMs = 0;
    std::time_t now = std::time(nullptr);
    int fogCount = 0, aloftCount = 0; int cirrusCount = 0;
    g_haveLevels = false;
    if (g_drLevels && XPLMGetDatavf(g_drLevels, nullptr, 0, 0) == XPLM_NUM_TEMPERATURE_LAYERS)
        g_haveLevels = XPLMGetDatavf(g_drLevels, g_levelsM, 0, XPLM_NUM_TEMPERATURE_LAYERS) == XPLM_NUM_TEMPERATURE_LAYERS;
    g_haveWindAlts = false;
    {   // the sim's own wind layer altitudes (ascending, 13 of them); if it won't say, our own grid is used
        XPLMDataRef dr = XPLMFindDataRef("sim/weather/region/wind_altitude_msl_m");
        if (dr && XPLMGetDatavf(dr, g_windAltM, 0, XPLM_NUM_WIND_LAYERS) == XPLM_NUM_WIND_LAYERS) {
            bool ok = g_windAltM[0] > 0;
            for (int k = 1; k < XPLM_NUM_WIND_LAYERS; ++k) ok = ok && g_windAltM[k] > g_windAltM[k - 1];
            g_haveWindAlts = ok;
        }
    }
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
    int stationStorms = 0;                    // stations inside a storm that took its cloud
    std::map<std::string, wx::XpWx> baseWx;   // weather built per station, reused for storm cells
    std::map<std::string, float> stationRadius;
    int bumpCount = 0;
    int valFixes = 0;           // adjustments made by validateXp across all records this update
    std::string valNote;        // first of them, for the log
    int stormClash = 0;         // storm records whose own layers had to replace a station layer
    // Rough tops (experimental, off by default): small records over a thick overcast whose layers equal the parent's except for a slightly
    // higher top, so the top is a little uneven. Used for the station decks and for the overcast that continues over the gap-fill tiles between stations.
    auto addBumps = [&](const wx::XpWx& x, const XPLMWeatherInfo_t& info, double cLat, double cLon, double radNm, double elev,
                        const std::string& id, int count, double fracMin, double fracMax) {
        if (!g_cfg.topBumps || bumpCount >= g_cfg.bumpMax) return;
        auto ovc = [&](int i) { return x.clouds[i].coverage >= (float)g_cfg.roughMinCoverage && x.clouds[i].type > 0.5f && !(i == 0 && x.fogLayerApplied); };
        bool any = false;
        for (int i = 0; i < 3; ++i) any = any || ovc(i);
        if (!any) return;
        const unsigned seed = (unsigned)std::fabs(cLat * 1000.0) * 2654435761u + (unsigned)std::fabs(cLon * 1000.0) * 40503u;
        auto hash = [](unsigned v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; };
        for (int k = 0; k < count && bumpCount < g_cfg.bumpMax; ++k) {
            const unsigned h1 = hash(seed + (unsigned)k * 7919u), h2 = hash(h1 + 17u), h3 = hash(h2 + 31u);
            const double ang = 6.283185307179586 * (h1 % 3600) / 3600.0;
            const double frac = fracMin + (fracMax - fracMin) * ((h2 % 1000) / 1000.0);
            // Footprint: the bump record's circle must lie inside the parent record (centre + radius <= 0.9 R), so a bump never
            // sticks out past the deck it belongs to as a detached tower over a neighbouring station's weather.
            const double bumpR = std::min(radNm * 0.35, std::max(0.5, radNm * g_cfg.bumpRadiusFactor * (0.6 + 0.8 * ((h3 % 1000) / 1000.0))));
            const double rNm = std::max(0.0, std::min(radNm * frac, radNm * 0.9 - bumpR));
            Rec e;
            e.airport = false;
            e.lat = cLat + rNm / 60.0 * std::cos(ang);
            e.lon = cLon + rNm / 60.0 * std::sin(ang) / std::max(0.2, std::cos(cLat * 3.14159265358979 / 180.0));
            e.elev = elev;
            e.info = info;
            e.info.radius_nm = (float)bumpR;
            bool changed = false;
            for (int i = 0; i < 3; ++i) {
                auto& L = e.info.cloud_layers[i];
                if (!ovc(i)) continue;
                float ceilM = 1e9f;   // base of the next layer above this one
                for (int j = 0; j < 3; ++j)
                    if (j != i && info.cloud_layers[j].coverage > 0 && info.cloud_layers[j].alt_base > L.alt_top) ceilM = std::min(ceilM, info.cloud_layers[j].alt_base);
                const float thick = L.alt_top - L.alt_base;
                // Experimental, conservative: a bump record is a weather SAMPLE that X-Plane blends with its neighbours slot by slot, so it keeps
                // the parent layer's slot, type, coverage and base exactly and differs only in the top: at most 20% of the deck thickness
                // higher, never into the layer above. (Earlier builds also changed type, coverage and base, which blended into walls.)
                const float rise = std::min({(float)g_cfg.bumpHeightM * (0.25f + 0.75f * ((h2 / 1000) % 1000) / 1000.0f), 0.2f * thick, std::max(0.0f, ceilM - L.alt_top - 120.0f), std::max(0.0f, wx::xpMaxThicknessM(L.cloud_type) - thick), std::max(0.0f, wx::kXpTopM - L.alt_top)});
                if (thick < 120.0f || rise < 20.0f) continue;   // too thin to carry a bump: leave this layer as the parent has it
                L.alt_top = L.alt_top + rise;
                e.info.max_altitude_msl_ft = std::max(e.info.max_altitude_msl_ft, L.alt_top / 0.3048f);   // the record's ceiling must cover its own top
                changed = true;
            }
            if (!changed) continue;
            e.info.age = 0.0f;
            const std::string key = "bump:" + id + ":" + std::to_string(k);
            seen[key] = true;
            ++bumpCount;
            auto bit = g_recs.find(key);
            (void)bit;
            e.info.age = info.age;
            todo.push_back({key, e});
        }
    };
    for (const auto& s : res.stations) {
        wx::XpWx x = wx::buildXp(s, g_cfg, now, &res.aloft, anyFog);
        valFixes += wx::validateXp(x, false, &valNote);
        baseWx[s.icao] = x;
        // A station inside the storm area takes the radar's rain (if that is more than it reports). The station circle (about 11 nm) is
        // drawn on top of the storm records, so a dry or "-RA" station inside a storm showed as a round beige or pale-blue hole in the radar
        // picture (2026-11-03 screenshot at KOMA). Same reach as the gap-fill tiles the storm replaces, so where those go, stations follow.
        const bool stormHere = g_cfg.radar && res.radar.ok &&
            (res.radar.natural ? wx::radarCovers(res.radar, s.lat, s.lon)
                               : underStorm(res.radar.cells, s.lat, s.lon, std::max(6.0, g_cfg.radarCellNm * g_cfg.stormRadiusFactor) * 0.8));
        if (stormHere && res.radar.natural) {
            // natural picture: the rain of the record the station stands in (none if it stands in a dry one: then its own report)
            if (const wx::RadarCell* nc = wx::naturalCellAt(res.radar, s.lat, s.lon))
                x.precip = std::max(x.precip, std::min(1.0f, std::max(0.05f, nc->rateMmH / std::max(1.0f, (float)g_cfg.radarRainFullMmH))));
        } else if (stormHere)
            x.precip = std::max(x.precip, wx::stormPrecipAt(res.radar.cells, s.lat, s.lon, g_cfg.radarCellNm * g_cfg.stormRadiusFactor, (float)g_cfg.radarRainFullMmH));
        if (stormHere && g_cfg.stormCloudsAtStations) {
            // ...and its cloud too. The station kept only its METAR layers, so inside a storm deck its circle showed as a round hole in the
            // tops (2026-11-04 screenshot, Ohio) and a hole in the map. The storm of the record it stands in is added to its own layers,
            // the same way a storm record grows out of a station's deck; its visibility and surface wind stay the report's.
            const wx::RadarCell* sc = res.radar.natural ? wx::naturalCellAt(res.radar, s.lat, s.lon) : nullptr;
            if (!res.radar.natural) {
                double best = std::max(6.0, g_cfg.radarCellNm * g_cfg.stormRadiusFactor) * 0.8;
                for (const auto& c : res.radar.cells) { const double d = wx::distNm(s.lat, s.lon, c.lat, c.lon); if (d < best) { best = d; sc = &c; } }
            }
            if (sc && sc->dbz >= g_cfg.radarMinDbz) {
                const float vis = x.visM, wd = x.windDir, wm = x.windMs;
                wx::applyStorm(x, *sc, g_cfg, s.elevM, s.lat);
                x.visM = vis; x.windDir = wd; x.windMs = wm;
                ++stationStorms;
            }
        }
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
            info.radius_nm = (float)std::min(g_cfg.radiusNm, std::max(0.5, nn * g_cfg.stationRadiusFactor));
            // Under a storm the station keeps a small circle (the airport still gets its own METAR), and the storm records around it
            // decide the radar picture: a full-size station circle showed as one flat round blob of a single rain level in the storm.
            if (stormHere) info.radius_nm = std::min(info.radius_nm, 5.0f);
        }
        stationRadius[s.icao] = info.radius_nm;
        if (x.fogLayerApplied) ++fogCount;
        if (x.modelCirrus) ++cirrusCount;
        if (x.aloft) ++aloftCount;
        Rec r;
        r.airport = airportKnown(s);
        r.icao = s.icao; r.lat = s.lat; r.lon = s.lon; r.elev = s.elevM;
        r.info = info;
        r.info.age = 0.0f;
        seen[s.icao] = true;
        addBumps(x, info, s.lat, s.lon, info.radius_nm, s.elevM, s.icao, g_cfg.bumpCount, 0.15, 0.85);
        auto it = g_recs.find(s.icao);
        (void)it;   // compared with what the sim already has after the cloud-slot pass (see matchEmptyLayers)
        r.info.age = info.age; // keep the real age for the call itself
        todo.push_back({s.icao, r});
    }
    const auto tPh1 = Clock::now();   // phase timings (stations | gap fill | storms | natural picture | the rest), logged with the total
    // Gap fill: the station circles do not overlap, so between them X-Plane would fall back to its own weather. Location records
    // on fixed grids cover everything around the aircraft: a fine grid close by and a coarse one further out (up to the METAR
    // search radius). Each record holds a smooth inverse-distance blend of the nearest stations (the closest one dominates), so
    // the weather changes gradually from airport to airport and no two stations compete over the same spot.
    int fillCount = 0, fillUnderStorm = 0;
    if (g_cfg.fillGaps && g_cfg.stationRadiusAuto && !res.stations.empty()) {
        auto addGrid = [&](int cellNm, double rMin, double rMax, const char* tag) {
            const double stepLat = cellNm / 60.0;
            const double rangeDeg = rMax / 60.0;
            const long j0 = (long)std::floor((res.centerLat - rangeDeg) / stepLat), j1 = (long)std::floor((res.centerLat + rangeDeg) / stepLat);
            for (long j = j0; j <= j1; ++j) {
                const double latRow = (j + 0.5) * stepLat;
                if (latRow < -85 || latRow > 85) continue;
                const double cosL = std::max(0.2, std::cos(latRow * 3.14159265358979 / 180.0));
                const double stepLon = stepLat / cosL;
                const double rangeLon = rangeDeg / cosL;
                const long i0 = (long)std::floor((res.centerLon - rangeLon) / stepLon), i1 = (long)std::floor((res.centerLon + rangeLon) / stepLon);
                for (long i = i0; i <= i1 + 1; ++i) {
                    // Staggered, wobbled grid: on a square grid the boundaries between tiles of different weather (and the holes left
                    // under storms) are straight N-S/E-W steps on X-Plane's cloud map. Every other row moves half a tile east and each
                    // tile moves a fixed (hash-based, so stable between updates) bit off its spot, so edges wander instead.
                    double lonc = (i + 0.5) * stepLon;
                    double latc = latRow;
                    if (g_cfg.fillStagger) {
                        if (j & 1) lonc += 0.5 * stepLon;
                        unsigned hsh = (unsigned)(j * 73856093L) ^ (unsigned)(i * 19349663L) ^ (unsigned)(cellNm * 83492791);
                        hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                        latc += ((double)(hsh % 1000u) / 1000.0 - 0.5) * 0.36 * stepLat;
                        lonc += ((double)((hsh >> 10) % 1000u) / 1000.0 - 0.5) * 0.36 * stepLon;
                    } else if (i > i1) continue;
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
                    if (g_cfg.radar && res.radar.ok && res.radar.natural) {
                        // the natural picture has its own rain and dry records here
                        if (wx::radarCovers(res.radar, latc, lonc)) { ++fillUnderStorm; continue; }
                    } else if (g_cfg.radar && g_cfg.stormReplacesFill && res.radar.ok) {
                        // A background tile inside a confirmed storm would blend its low deck (tops of 2-3 km) into the storm's 13 km tops, and X-Plane
                        // renders that as a sheer canyon through the storm. The storm cells cover this spot, so the tile is not needed. Rim cells (the faint
                        // outskirts) do not count: below them the deck stays.
                        bool under = false;
                        // Every storm, light or tall (2026-11-03): a tile left inside an area of light rain either carries the rain or not, and both
                        // ways the rain's edge on X-Plane's radar map then runs along the 12 nm tile grid (the stair-steps in the screenshots). The
                        // storm records carry the station's deck as well, so the cloud stays; only intensity-outline and rim cells do not count.
                        const double reach = std::max(6.0, g_cfg.radarCellNm * g_cfg.stormRadiusFactor) * 0.8;
                        for (const auto& rc : res.radar.cells)
                            if (!rc.synthetic || (!rc.key.empty() && rc.key[0] == 'H')) { if (wx::distNm(latc, lonc, rc.lat, rc.lon) < reach) { under = true; break; } }
                        if (under) { ++fillUnderStorm; continue; }
                    }
                    wx::Station syn = wx::blendAt(res.stations, latc, lonc);
                    wx::XpWx x = wx::buildXp(syn, g_cfg, now, &res.aloft, anyFog);
                    if (x.fogLayerApplied) { x.clouds[0] = wx::XpCloud(); x.fogLayerApplied = false; }
                    valFixes += wx::validateXp(x, false, &valNote);
                    if (g_cfg.radar && res.radar.ok && res.radar.natural)
                        x.precip = 0.0f;   // natural picture: a tile only stands outside the radar's echoes, so no rain (a "-RA" blended in from a
                                           // station drew pale-blue areas with 12 nm tile edges on the radar map, 2026-11-03)
                    else if (g_cfg.radar && res.radar.ok && insideStorm(res.radar.cells, latc, lonc, g_cfg.radarCellNm))
                        x.precip = std::max(x.precip, wx::stormPrecipAt(res.radar.cells, latc, lonc, g_cfg.radarCellNm * g_cfg.stormRadiusFactor, (float)g_cfg.radarRainFullMmH));
                    if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK);
                    XPLMWeatherInfo_t info;
                    fillInfo(info, x);
                    info.radius_nm = (float)(cellNm * g_cfg.fillRadiusFactor);   // above ~0.8 neighbouring tiles overlap, so X-Plane blends their edges
                    Rec r;
                    r.airport = false;
                    r.lat = latc; r.lon = lonc; r.elev = nearest->elevM;
                    r.info = info;
                    r.info.age = 0.0f;
                    addBumps(x, info, latc, lonc, cellNm * 0.5, nearest->elevM, std::string("f") + tag + std::to_string(j) + "_" + std::to_string(i), g_cfg.bumpFillCount, 0.0, 0.9);
                    const std::string key = std::string("fill") + tag + ":" + std::to_string(j) + ":" + std::to_string(i);
                    seen[key] = true;
                    ++fillCount;
                    auto it = g_recs.find(key);
                    (void)it;
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
    const auto tPh2 = Clock::now();
    // Radar storm cells: the weather of the nearest station with the storm applied on top, as location records.
    int stormCount = 0;
    if (g_cfg.radar && res.radar.ok && !res.stations.empty()) {
        // Pass 1: each cell's station weather and the base its own station deck would give it.
        struct StormPre { wx::XpWx x; const wx::Station* nearest; double nd; float base; };
        std::vector<StormPre> pre;
        pre.reserve(res.radar.cells.size());
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
            // A storm whose circle overlaps its station's record keeps the station's visibility instead (rain/mist of 1 km or more; ground fog stays local): with 40 km here X-Plane blended
            // it into the station record and the METAR value read back at the airport several times too high (KFYV: 2.8 km reported, 9.5 km read).
            auto srit = stationRadius.find(nearest->icao);
            const bool overlapsStation = srit != stationRadius.end() && nd <= srit->second + std::max(6.0, g_cfg.radarCellNm * 0.9);
            if (nd > 60.0 || (x.visM < 5000.0f && !(overlapsStation && x.visM >= 1000.0f))) x.visM = (float)g_cfg.clearVisM;
            if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK);
            pre.push_back({x, nearest, nd, wx::stormBaseFor(x, c, nearest->elevM)});
        }
        // Neighbouring cells take their base from different stations' decks (1051 m next to 1987 m in a log), which X-Plane blends into
        // a stepped underside and walls. Cells within 1.7 cell widths therefore share a distance-weighted mean base.
        std::vector<float> baseSm(pre.size(), -1.0f);
        if (g_cfg.stormBaseSmooth) {
            const double reach = g_cfg.radarCellNm * 1.7;
            for (size_t i = 0; i < pre.size(); ++i) {
                double sw = 0, sb = 0;
                for (size_t j = 0; j < pre.size(); ++j) {
                    const double d = wx::distNm(res.radar.cells[i].lat, res.radar.cells[i].lon, res.radar.cells[j].lat, res.radar.cells[j].lon);
                    if (d > reach) continue;
                    const double w = 1.0 - 0.6 * d / reach;
                    sw += w; sb += w * pre[j].base;
                }
                if (sw > 0) baseSm[i] = (float)(sb / sw);
            }
        }
        // Pass 2: apply the storm with the shared base and build the records.
        for (size_t ci = 0; ci < res.radar.cells.size(); ++ci) {
            const auto& c = res.radar.cells[ci];
            const wx::Station* nearest = pre[ci].nearest;
            wx::XpWx x = pre[ci].x;
            const int layersBefore = x.cloudCount;
            wx::applyStorm(x, c, g_cfg, nearest->elevM, c.lat, baseSm[ci]);
            if (layersBefore >= 3) ++stormClash;
            valFixes += wx::validateXp(x, true, &valNote);   // storms may overlap the station's layers on purpose; only sanity-checked
            XPLMWeatherInfo_t info;
            fillInfo(info, x);
            info.radius_nm = c.natural ? (float)std::max(1.2, c.sizeNm * 0.9)   // in proportion to its square, so neighbours split along the square edges
                                       : (float)std::max(4.0, std::max(6.0, g_cfg.radarCellNm * g_cfg.stormRadiusFactor) * c.radiusScale);
            Rec r;
            r.airport = false;
            r.lat = c.lat; r.lon = c.lon; r.elev = nearest->elevM;
            r.info = info;
            r.info.age = 0.0f;
            const std::string key = "radar:" + c.key;
            seen[key] = true;
            ++stormCount;
            auto it = g_recs.find(key);
            (void)it;
            r.info.age = info.age;
            todo.push_back({key, r});
        }
    }
    const auto tPh3 = Clock::now();
    // Natural picture: the dry records around and between the rain (the stations' blended weather, like a gap-fill tile, in the size of
    // their square). They take the place of the gap-fill tiles there, so the rain ends where the radar's does, not on the 12 nm tile grid.
    int dryCount = 0;
    if (g_cfg.radar && res.radar.ok && res.radar.natural && !res.stations.empty()) {
        for (const auto& c : res.radar.dryCells) {
            const wx::Station* nearest = &res.stations[0];
            double nd = 1e9;
            for (const auto& st : res.stations) { const double d = wx::distNm(c.lat, c.lon, st.lat, st.lon); if (d < nd) { nd = d; nearest = &st; } }
            auto rit = stationRadius.find(nearest->icao);
            if (rit != stationRadius.end() && nd <= rit->second * 0.5 && c.sizeNm <= 6.0f) continue;   // a small square right at a station: the station record covers it
            wx::Station syn = wx::blendAt(res.stations, c.lat, c.lon);
            wx::XpWx x = wx::buildXp(syn, g_cfg, now, &res.aloft, anyFog);
            if (x.fogLayerApplied) { x.clouds[0] = wx::XpCloud(); x.fogLayerApplied = false; }
            x.precip = 0.0f;   // the radar shows no rain here
            valFixes += wx::validateXp(x, false, &valNote);
            if (x.visM < 20000.0f) x.visM = std::max(50.0f, x.visM * g_visK);
            XPLMWeatherInfo_t info;
            fillInfo(info, x);
            info.radius_nm = (float)std::max(1.2, c.sizeNm * 0.9);
            Rec r;
            r.airport = false;
            r.lat = c.lat; r.lon = c.lon; r.elev = nearest->elevM;
            r.info = info;
            r.info.age = 0.0f;
            const std::string key = "radar:" + c.key;
            seen[key] = true;
            ++dryCount;
            auto it = g_recs.find(key);
            (void)it;
            r.info.age = info.age;
            todo.push_back({key, r});
        }
    }
    const auto tPh4 = Clock::now();
    // Cloud ribbons (2026-11-03 screenshots): X-Plane blends each of the 3 cloud slots between neighbouring records, base and top included.
    // An empty slot was sent at 0-0 m, so between a record with a deck at 700-2100 m and one without it the blend made a thin sheet that
    // sloped down to the ground; seen from above, where its coverage still showed, that was a long thin ribbon or a ring in clear air.
    // An empty slot now carries the base, top and type of the same slot in the nearest record that has a cloud there, with (almost) no
    // coverage, so towards the clear side the deck only thins out, at its own height.
    matchEmptyLayers(todo);
    smoothPressure(todo, res.stations);
    // An airport record that is no longer wanted (the aircraft moved and the station list changed): X-Plane crashed twice right as
    // WXPlane began removing records after a move (logs 2026-11-03 22:41 and 23:03, both with airport records to remove, both gone with
    // no crash report before the first batch was through; an update that only removed location records went through). Such an update
    // now replaces all of WXPlane's weather in one go instead (a non-incremental update, the way the plugin's records are cleared when it
    // is switched off), so nothing is removed one record at a time.
    // That replacing update came at almost every refresh in flight (airports keep leaving range) and each one made X-Plane rebuild
    // all of its clouds: ~5 GB of VRAM held twice for a moment and 80-120 ms on the main thread (log and VRAM graph 2026-11-07).
    // So an airport that leaves range is now left in place instead: its record is set once more with the blended weather of its
    // surroundings (an ordinary one-record update, nothing removed) and then left alone; it is far behind the aircraft, and when it
    // comes back into range it is updated like any other station. A replacing update is only used for a big jump (many airports gone
    // at once, X-Plane rebuilds there anyway) or as a clean-up once more than kept_airports_max (200) have piled up.
    bool replaceAll = false;
    const char* replaceWhy = "";
    if (!full) {
        std::vector<std::string> newlyGone;
        size_t keptGone = 0;
        for (auto& kv : g_recs) {
            if (!kv.second.airport) continue;
            if (seen.count(kv.first)) { kv.second.kept = false; continue; }   // in range again: an ordinary station
            if (kv.second.kept) ++keptGone; else newlyGone.push_back(kv.first);
        }
        if (newlyGone.size() >= 15) {
            replaceAll = true; replaceWhy = "a big move (many airports left range at once)";
            if (!g_load.active()) { g_load.start("New location"); g_loadFetchIssued = true; }
        }
        else if (keptGone + newlyGone.size() > (size_t)g_cfg.keptAirportsMax) { replaceAll = true; replaceWhy = "clean-up of the airports kept behind the aircraft"; }
        else {
            for (const auto& k : newlyGone) {
                const Rec& old = g_recs[k];
                wx::Station syn = wx::blendAt(res.stations, old.lat, old.lon);
                wx::XpWx x = wx::buildXp(syn, g_cfg, now, &res.aloft, anyFog);
                if (x.fogLayerApplied) { x.clouds[0] = wx::XpCloud(); x.fogLayerApplied = false; }
                wx::validateXp(x, false, nullptr);
                XPLMWeatherInfo_t info;
                fillInfo(info, x);
                info.radius_nm = old.info.radius_nm;
                Rec r = old;
                r.info = info;
                r.info.age = 0.0f;
                r.kept = true;
                todo.push_back({k, r});
            }
            for (const auto& kv : g_recs) if (kv.second.airport && !seen.count(kv.first)) seen[kv.first] = true;   // kept: never erased
            if (!newlyGone.empty())
                logf("%zu airport(s) left range: kept in place with their surroundings' weather (%zu kept behind the aircraft; clean-up above %d)",
                     newlyGone.size(), keptGone + newlyGone.size(), g_cfg.keptAirportsMax);
        }
    }
    if (!full && !replaceAll) {   // only what changed goes to X-Plane
        std::vector<Todo> changed;
        for (auto& t : todo) {
            auto it = g_recs.find(t.key);
            if (it != g_recs.end() && it->second.airport == t.rec.airport) {
                XPLMWeatherInfo_t a = t.rec.info;
                a.age = 0.0f;
                if (std::memcmp(&it->second.info, &a, sizeof a) == 0 || nearlySame(it->second.info, a)) continue;
            }
            changed.push_back(t);
        }
        todo.swap(changed);
    }
    std::vector<std::string> gone;
    if (!full && !replaceAll)
        for (const auto& kv : g_recs)
            if (!seen.count(kv.first)) gone.push_back(kv.first);

    // Everything is queued; flushPending() (every 0.25 s) hands it to X-Plane a few records at a time.
    if (full || replaceAll) g_recs.clear();
    g_pendingReplace = replaceAll;
    if (replaceAll) logf("%s: all %zu records go to X-Plane as one replacing update, nothing removed one by one", replaceWhy, todo.size());
    g_heldLevel.clear();
    {   // every update: the outline records follow in their own update once the rest is in (30 s after this one, at the earliest)
        std::vector<Todo> main;
        for (auto& t : todo) (isLevelKey(t.key) ? g_heldLevel : main).push_back(t);
        todo.swap(main);
        g_heldLevelAt = Clock::now() + std::chrono::seconds(g_cfg.stormLevelDelayS);
    }
    g_pendingTodo = todo;
    g_pendingGone = gone;
    g_pendingImmediate = res.immediate || !g_firstApplyDone || g_startupPhase;
    g_pendingStartup = g_startupPhase;
    g_pendingManual = res.immediate && !g_startupPhase && g_cfg.manualRefreshImmediate;
    const bool reallyImmediate = g_pendingImmediate && (g_pendingManual || (g_cfg.firstLoadImmediate && g_pendingStartup));   // immediate = the sim freezes for several seconds while X-Plane rebuilds
    {   // spread the records evenly over the configured transition time (nearest stations first)
        size_t n = todo.size() + gone.size();
        int T = g_startupPhase ? 0 : (g_firstApplyDone ? g_cfg.updateTransitionS : g_cfg.transitionS);
        g_releaseStepS = (n > 0 && T > 0) ? std::max(0.0625, (double)T / (double)n) : 0.0;
        g_nextRelease = Clock::now();
    }
    if (!res.elevNote.empty()) logf("station elevations corrected: %s", res.elevNote.c_str());
    if (res.noaaFixes > 0 || res.filledElements > 0)
        logf("NOAA crosscheck: %d of %zu reports corrected or completed from NOAA's decoded fields, %d missing elements filled from neighbouring stations",
             res.noaaFixes, res.stations.size(), res.filledElements);
    logf("update (+%d s after the sim started running): %zu stations, %d gap-fill records (%d left out under storms), %d storm cells, %zu changed, %zu removed%s%s", g_loopStarted ? (int)std::chrono::duration<double>(Clock::now() - g_enabledAt).count() : -1, res.stations.size(), fillCount, fillUnderStorm, stormCount, todo.size(), gone.size(), full ? " (full)" : (res.radarOnly ? " (radar only)" : ""),
         (g_pendingTodo.empty() && g_pendingGone.empty()) ? "" : " [queued, sent a few at a time]");
    if (g_cfg.radar) {
        if (res.radar.ok) {
            float maxDbz = 0, maxTop = 0; int cb = 0, steady = 0;
            for (const auto& c : res.radar.cells) { maxDbz = std::max(maxDbz, c.dbz); maxTop = std::max(maxTop, c.topM); if (c.dbz >= 40 && !c.stratiform) ++cb; if (c.stratiform) ++steady; }
            if (res.radar.natural)
                logf("radar: natural picture: %d rain records, %d dry records around them (radar_natural = 0 goes back to 12 nm blocks)%s", stormCount, dryCount,
                     res.radar.note.empty() ? "" : (" - " + res.radar.note).c_str());
            logf("radar: %s, %d echo blocks, %d cells applied (%d convective at 40+ dBZ = cumulonimbus, %d steady rain/snow), strongest %.0f dBZ, highest echo top %.0f m%s%s; %d hole(s) interpolated, %d rim cell(s)",
                 res.radar.source.c_str(), res.radar.echoBlocks, stormCount, cb, steady, maxDbz, maxTop,
                 res.radar.topsFromRadar ? ", echo tops from radar" : ", tops estimated", res.radar.note.empty() ? "" : (" (" + res.radar.note + ")").c_str(), res.radar.holesFilled, res.radar.rimCells);
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
    g_watchOn = !res.stations.empty();
    g_watchN = 0;
    g_watchAt = Clock::now() + std::chrono::seconds(30);
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
    const std::string rReg = wx::radarRegion(res.centerLat, res.centerLon), rLab = rReg.empty() ? std::string("Radar") : "Radar (" + rReg + ")";
    std::string radarNote = !g_cfg.radar ? "Radar: off"
                          : res.radar.ok ? rLab + ": " + std::to_string(stormCount) + " storm cells"
                          : rReg.empty() ? std::string("Radar: none here")
                          : rLab + ": " + (res.radar.note.find("feed down") != std::string::npos ? std::string("feed down") : std::string("unavailable"));
    std::string aloftNote = g_cfg.useAloft ? (aloftCount ? "Winds aloft: " + std::to_string(res.aloft.size()) + " model points"
                                                          : "Winds aloft: " + (res.aloftNote.empty() ? std::string("none") : res.aloftNote))
                                           : "Winds aloft: off";
    if (!res.stations.empty())
        std::snprintf(l2, sizeof l2, "%s | %s | nearest %s %.0f nm", aloftNote.c_str(), radarNote.c_str(), res.stations[0].icao.c_str(), res.stations[0].distNm);
    else
        std::snprintf(l2, sizeof l2, "No stations in range");
    g_lastGoodStatus = l1;
    setStatus(l1, l2);
    {
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - applyT0).count();
        auto d = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        if (ms > 15.0) logf("building this update took %.0f ms on the sim's main thread: stations %.0f (airport lookups %.0f), gap fill %.0f, storms %.0f, natural picture %.0f, rest %.0f",
                            ms, d(applyT0, tPh1), g_navLookupMs, d(tPh1, tPh2), d(tPh2, tPh3), d(tPh3, tPh4), d(tPh4, Clock::now()));
    }
    if (g_cfg.topBumps) logf("overcast tops: %d rough-top records scattered over thick overcast decks", bumpCount);
    logf("submitted weather: %d stations, %d storm records, %d rough-top records; validation adjusted %d item(s)%s%s; %d storm(s) replaced a third station layer; %d station(s) inside a storm took its cloud",
         (int)res.stations.size(), stormCount, bumpCount, valFixes, valNote.empty() ? "" : " - first: ", valNote.c_str(), stormClash, stationStorms);
    logf("applied %d stations (%d ATIS, %d fog, %d with cirrus from the global model, %d with winds/temps aloft from %zu global-model points) around %.2f,%.2f%s",
         res.metarCount, res.atisCount, fogCount, cirrusCount, aloftCount, res.aloft.size(), res.centerLat, res.centerLon,
         reallyImmediate ? " [immediate: sim pauses while X-Plane rebuilds]" : " [fade-in]");
    if (g_cfg.useAloft && !res.aloftNote.empty()) logf("%s", res.aloftNote.c_str());
    for (const auto& s : res.stations) {
        if (s.hasAtis) logf("  %s ATIS %s: %s", s.icao.c_str(), s.atisLetter.c_str(), s.atisText.substr(0, 110).c_str());
    }
}

// Log-only: what X-Plane is using at the aircraft compared with what the stations around it call for. Shows whether the sim
// has gone back to its own weather after our update (checked every minute for 10 minutes, then every 5 minutes).
// Lists (up to 8) of OUR submitted records whose circle contains this point, nearest first: what we sent, as opposed to what
// X-Plane reports there. When the read-back differs from the station's own values this shows which overlapping records (station,
// gap fill, radar storm, rough-top bump) X-Plane is blending at that spot.
void logCoveringRecords(double lat, double lon, const char* label) {
    struct Hit { double d; const std::string* key; const Rec* r; };
    std::vector<Hit> hits;
    for (const auto& kv : g_recs) {
        const double d = wx::distNm(lat, lon, kv.second.lat, kv.second.lon);
        if (d <= kv.second.info.radius_nm) hits.push_back({d, &kv.first, &kv.second});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.d < b.d; });
    logf("  records we submitted that cover %s: %zu", label, hits.size());
    for (size_t i = 0; i < hits.size() && i < 8; ++i) {
        const auto& L = hits[i].r->info.cloud_layers;
        char buf[256]; int n = 0;
        buf[0] = 0;
        for (int j = 0; j < XPLM_NUM_CLOUD_LAYERS && n < 200; ++j)
            if (L[j].coverage > 0) n += std::snprintf(buf + n, sizeof buf - n, " [%d: t%.0f %.0f%% %.0f-%.0f m]", j, L[j].cloud_type, L[j].coverage * 100, L[j].alt_base, L[j].alt_top);
        logf("    %s  %.1f nm of r=%.1f nm, QNH %.1f hPa, rain %.2f:%s", hits[i].key->c_str(), hits[i].d, hits[i].r->info.radius_nm, hits[i].r->info.pressure_alt / 100.0f,
             hits[i].r->info.precip_rate, buf);
    }
}

// The wind profile we sent next to the one X-Plane reports back, so "the winds aloft look low" can be checked from Log.txt.
void logWindProfile(const wx::XpWx& x, const XPLMWeatherInfo_t& info, const char* where, double lat, double lon) {
    std::string sent, back;
    char b[64];
    for (const auto& w : x.winds) { std::snprintf(b, sizeof b, " %.0fft %03.0f/%.0fkt", w.altMsl / 0.3048f, w.dir, w.ms / 0.514444f); sent += b; }
    for (int i = 0; i < XPLM_NUM_WIND_LAYERS; ++i) {
        const auto& w = info.wind_layers[i];
        if (w.speed < 0 || w.alt_msl < 0 || (w.alt_msl == 0 && w.speed == 0 && w.direction == 0 && i > 0)) continue;
        std::snprintf(b, sizeof b, " %.0fft %03.0f/%.0fkt", w.alt_msl / 0.3048f, w.direction, w.speed / 0.514444f);
        back += b;
    }
    logf("  winds aloft we sent for %s:%s", where, sent.empty() ? " none" : sent.c_str());
    logf("  winds aloft X-Plane reports there:%s", back.empty() ? " none" : back.c_str());
    // What the sim says the wind is at a few flight levels over the same spot (the wind_*_alt fields are the values at the altitude asked for).
    std::string lv;
    for (double ft : {3000.0, 10000.0, 18000.0, 24000.0, 30000.0, 36000.0, 45000.0}) {
        XPLMWeatherInfo_t q;
        std::memset(&q, 0, sizeof q);
        q.structSize = sizeof q;
        XPLMGetWeatherAtLocation(lat, lon, ft * 0.3048, &q);
        std::snprintf(b, sizeof b, " FL%03.0f %03.0f/%.0fkt", ft / 100.0, q.wind_dir_alt, q.wind_spd_alt / 0.514444f);
        lv += b;
    }
    logf("  X-Plane's wind at flight levels there:%s", lv.c_str());
    // Temperatures aloft the same way: ours (the profile that went into the record, ISA deviation in brackets) next to X-Plane's at that spot.
    if (x.tempProfile.size() >= 2) {
        std::string tl;
        for (double ft : {5000.0, 10000.0, 18000.0, 24000.0, 30000.0, 36000.0, 45000.0}) {
            const float m = (float)(ft * 0.3048);
            XPLMWeatherInfo_t q;
            std::memset(&q, 0, sizeof q);
            q.structSize = sizeof q;
            XPLMGetWeatherAtLocation(lat, lon, m, &q);
            const float ours = wx::profileTemp(x.tempProfile, m);
            const float isa = m < 11000.0f ? 15.0f - 0.0065f * m : -56.5f;
            std::snprintf(b, sizeof b, " FL%03.0f %+.0f/%+.0fC(ISA%+.0f)", ft / 100.0, ours, q.temperature_alt, ours - isa);
            tl += b;
        }
        // and the tropopause: ours (from the profile) and the one X-Plane reports at this spot
        float ta = 0, tt = 0;
        XPLMWeatherInfo_t q;
        std::memset(&q, 0, sizeof q);
        q.structSize = sizeof q;
        XPLMGetWeatherAtLocation(lat, lon, 10000.0, &q);
        if (wx::findTropopause(x.tempProfile, ta, tt)) std::snprintf(b, sizeof b, " | tropopause ours FL%03.0f %+.0fC", ta / 0.3048f / 100.0f, tt);
        else std::snprintf(b, sizeof b, " | tropopause ours: none in the profile");
        tl += b;
        std::snprintf(b, sizeof b, ", X-Plane's FL%03.0f %+.0fC", q.troposphere_alt / 0.3048f / 100.0f, q.troposphere_temp);
        tl += b;
        logf("  temperatures aloft, ours/X-Plane's:%s", tl.c_str());
    }
}

// The sim kept ignoring the upper wind layers of our records (FL100 and up read 180 deg / 1 kt), so as well as the records
// the profile for the aircraft's position is written into the sim's own region wind arrays. Throttled, and only if writable.
// Still needed (log 2026-11-04 13:26): with this off, X-Plane read 180 deg / 1 kt at every flight level from FL100 up, whatever the
// records said; the winds aloft only reach the sim through its region arrays. region_winds = 0 turns it off.
// What X-Plane's region arrays held before WXPlane first wrote them, so they can be given back when WXPlane is switched off
// (2026-11-10 customer log, ENAT: after "weather off" the 20 s region writes went on, so X-Plane kept WXPlane's winds, temperatures
// and tropopause with WXPlane off). The ramp starts again from the sim's values after a restore.
struct RegionSaved {
    bool w = false, t = false, tro = false;
    float alt[XPLM_NUM_WIND_LAYERS], spd[XPLM_NUM_WIND_LAYERS], dir[XPLM_NUM_WIND_LAYERS];
    float temp[XPLM_NUM_TEMPERATURE_LAYERS];
    float troAlt = 0, troTemp = 0;
};
RegionSaved g_regionSaved;
bool g_rwHaveW = false, g_rwHaveT = false, g_rwHaveTro = false;
std::time_t g_rwLast = 0;

void regionRestore(const char* why) {
    const bool any = g_regionSaved.w || g_regionSaved.t || g_regionSaved.tro;
    if (g_regionSaved.w) {
        XPLMDataRef drA = XPLMFindDataRef("sim/weather/region/wind_altitude_msl_m"), drS = XPLMFindDataRef("sim/weather/region/wind_speed_msc"),
                    drD = XPLMFindDataRef("sim/weather/region/wind_direction_degt");
        if (drA && drS && drD) {
            XPLMSetDatavf(drA, g_regionSaved.alt, 0, XPLM_NUM_WIND_LAYERS);
            XPLMSetDatavf(drS, g_regionSaved.spd, 0, XPLM_NUM_WIND_LAYERS);
            XPLMSetDatavf(drD, g_regionSaved.dir, 0, XPLM_NUM_WIND_LAYERS);
        }
    }
    if (g_regionSaved.t) {
        XPLMDataRef drT = XPLMFindDataRef("sim/weather/region/temperatures_aloft_deg_c");
        if (drT) XPLMSetDatavf(drT, g_regionSaved.temp, 0, XPLM_NUM_TEMPERATURE_LAYERS);
    }
    if (g_regionSaved.tro) {
        XPLMDataRef drTa = XPLMFindDataRef("sim/weather/region/tropo_alt_m"), drTt = XPLMFindDataRef("sim/weather/region/tropo_temp_c");
        if (drTa && drTt) { XPLMSetDataf(drTa, g_regionSaved.troAlt); XPLMSetDataf(drTt, g_regionSaved.troTemp); }
    }
    g_regionSaved = RegionSaved();
    g_rwHaveW = g_rwHaveT = g_rwHaveTro = false;
    g_rwLast = 0;
    if (any) logf("%s: X-Plane's region winds, temperatures and tropopause put back", why);
}

void pushRegionWinds(const wx::XpWx& xin) {
    if (!g_cfg.regionWinds || !g_cfg.enabled || !g_pluginEnabled) return;   // WXPlane off: the region arrays are X-Plane's again
    std::time_t& last = g_rwLast;
    static bool reported = false;
    const std::time_t now = std::time(nullptr);
    if (now - last < 20) return;
    // Validated first (2026-11-06): a profile with impossible values is not written at all, the sim keeps the last good one.
    wx::XpWx x = xin;
    std::string why;
    const int bad = wx::validateProfile(x, true, &why);
    static std::time_t lastBadLog = 0;
    if (bad && now - lastBadLog > 120) { lastBadLog = now; logf("region profile: %d value(s) rejected before writing (%s)", bad, why.c_str()); }
    if (x.winds.size() < 2) return;
    last = now;
    XPLMDataRef drA = XPLMFindDataRef("sim/weather/region/wind_altitude_msl_m"), drS = XPLMFindDataRef("sim/weather/region/wind_speed_msc"),
                drD = XPLMFindDataRef("sim/weather/region/wind_direction_degt");
    if (!drA || !drS || !drD || !XPLMCanWriteDataRef(drA) || !XPLMCanWriteDataRef(drS) || !XPLMCanWriteDataRef(drD)) {
        if (!reported) { reported = true; logf("region wind datarefs are missing or read-only: not writing winds aloft there"); }
        return;
    }
    // Fixed altitudes, the same at every write (before, the layout followed the nearest station's terrain and could squeeze 7 layers into
    // 20 ft), and each write moves at most 3 m/s per level towards the new profile, so crossing from one record's winds to another's is
    // a change over a minute or two, not a step the autopilot and the ground speed feel at once.
    // low levels closer together (2026-11-06, windshear): vertical shear in the lowest 4,500 ft (low-level jets, inversions) is kept
    static const float kRegionFt[XPLM_NUM_WIND_LAYERS] = {1000, 2000, 3000, 4500, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000, 45000};
    bool& haveW = g_rwHaveW;
    static float cu[XPLM_NUM_WIND_LAYERS], cv[XPLM_NUM_WIND_LAYERS];
    float al[XPLM_NUM_WIND_LAYERS], sp[XPLM_NUM_WIND_LAYERS], di[XPLM_NUM_WIND_LAYERS];
    const float rad = 3.14159265f / 180.0f, kStepMs = 3.0f;
    for (int k = 0; k < XPLM_NUM_WIND_LAYERS; ++k) {
        al[k] = kRegionFt[k] * 0.3048f;
        const wx::XpWind w = wx::windAt(x.winds, al[k]);
        const float tu = w.ms * std::sin(w.dir * rad), tv = w.ms * std::cos(w.dir * rad);
        if (!haveW) { cu[k] = tu; cv[k] = tv; }
        else {
            const float du = tu - cu[k], dv = tv - cv[k], d = std::sqrt(du * du + dv * dv);
            const float f = d > kStepMs ? kStepMs / d : 1.0f;
            cu[k] += du * f; cv[k] += dv * f;
        }
        sp[k] = std::sqrt(cu[k] * cu[k] + cv[k] * cv[k]);
        di[k] = sp[k] > 0.05f ? std::fmod(std::atan2(cu[k], cv[k]) / rad + 360.0f, 360.0f) : w.dir;
    }
    haveW = true;
    if (!g_regionSaved.w) {
        g_regionSaved.w = XPLMGetDatavf(drA, g_regionSaved.alt, 0, XPLM_NUM_WIND_LAYERS) == XPLM_NUM_WIND_LAYERS &&
                          XPLMGetDatavf(drS, g_regionSaved.spd, 0, XPLM_NUM_WIND_LAYERS) == XPLM_NUM_WIND_LAYERS &&
                          XPLMGetDatavf(drD, g_regionSaved.dir, 0, XPLM_NUM_WIND_LAYERS) == XPLM_NUM_WIND_LAYERS;
    }
    XPLMSetDatavf(drA, al, 0, XPLM_NUM_WIND_LAYERS);
    XPLMSetDatavf(drS, sp, 0, XPLM_NUM_WIND_LAYERS);
    XPLMSetDatavf(drD, di, 0, XPLM_NUM_WIND_LAYERS);
    if (!reported) { reported = true; logf("writing winds aloft into the sim's region wind datarefs (every 20 s, at most 3 m/s change per level per write)"); }
    // Temperatures aloft the same way (log 2026-11-04 13:29): X-Plane used our temperatures right at the airport, but a few miles away
    // it drifted back to its own, and patches of different temperature are patches of different pressure at the same height.
    // Ramped too (2026-11-06): the pressure at FL350 depends on the temperature of the whole column below, so a column changed all at
    // once moves the altimeter all at once (by 4,000 ft near Creede). At most 1.5 C per level per write, starting from what the sim has.
    static XPLMDataRef drT = XPLMFindDataRef("sim/weather/region/temperatures_aloft_deg_c");
    static bool tReported = false;
    bool& haveT = g_rwHaveT;
    static float ct[XPLM_NUM_TEMPERATURE_LAYERS];
    if (drT && XPLMCanWriteDataRef(drT) && x.tempProfile.size() >= 2 && g_drLevels) {
        float lv[XPLM_NUM_TEMPERATURE_LAYERS], tc[XPLM_NUM_TEMPERATURE_LAYERS];
        if (XPLMGetDatavf(g_drLevels, lv, 0, XPLM_NUM_TEMPERATURE_LAYERS) == XPLM_NUM_TEMPERATURE_LAYERS) {
            if (!haveT) {
                float cur[XPLM_NUM_TEMPERATURE_LAYERS];
                const bool got = XPLMGetDatavf(drT, cur, 0, XPLM_NUM_TEMPERATURE_LAYERS) == XPLM_NUM_TEMPERATURE_LAYERS;
                for (int k = 0; k < XPLM_NUM_TEMPERATURE_LAYERS; ++k)
                    ct[k] = (got && std::isfinite(cur[k]) && cur[k] > -95.0f && cur[k] < 60.0f) ? cur[k] : wx::profileTemp(x.tempProfile, lv[k]);
                haveT = true;
            }
            float maxGap = 0; int gapK = 0;
            for (int k = 0; k < XPLM_NUM_TEMPERATURE_LAYERS; ++k) {
                const float target = wx::profileTemp(x.tempProfile, lv[k]);
                const float d = target - ct[k];
                if (std::fabs(d) > maxGap) { maxGap = std::fabs(d); gapK = k; }
                ct[k] += std::max(-1.5f, std::min(1.5f, d));
                tc[k] = ct[k];
            }
            if (!g_regionSaved.t) g_regionSaved.t = XPLMGetDatavf(drT, g_regionSaved.temp, 0, XPLM_NUM_TEMPERATURE_LAYERS) == XPLM_NUM_TEMPERATURE_LAYERS;
            XPLMSetDatavf(drT, tc, 0, XPLM_NUM_TEMPERATURE_LAYERS);
            static std::time_t lastRampLog = 0;
            if (maxGap > 5.0f && now - lastRampLog > 60) {
                lastRampLog = now;
                logf("region temperatures ramping: %.0f ft is %.1f C from the new profile (moving 1.5 C per 20 s)", lv[gapK] / 0.3048f, maxGap);
            }
            if (!tReported) { tReported = true; logf("writing temperatures aloft into the sim's region temperature datarefs (every 20 s, at most 1.5 C per level per write)"); }
        }
    } else if (!tReported && x.tempProfile.size() >= 2) {
        tReported = true;
        logf("region temperature datarefs are missing or read-only: not writing temperatures aloft there");
    }
    // The tropopause, if X-Plane has region datarefs for it (not documented; looked up, written only when they exist and are writable).
    // Ramped like the temperatures: at most 1.5 C and 300 m per write.
    static XPLMDataRef drTa = XPLMFindDataRef("sim/weather/region/tropo_alt_m"), drTt = XPLMFindDataRef("sim/weather/region/tropo_temp_c");
    static bool troReported = false;
    bool& haveTro = g_rwHaveTro;
    static float cTa = 0, cTt = 0;
    float ta = 0, tt = 0;
    const bool canTro = drTa && drTt && XPLMCanWriteDataRef(drTa) && XPLMCanWriteDataRef(drTt);
    if (!troReported) {
        troReported = true;
        logf(canTro ? "tropopause: writing the GFS profile's tropopause into the sim's region tropopause datarefs too"
                    : "tropopause: no writable region tropopause datarefs (set in the weather records only)");
    }
    if (canTro && x.tempProfile.size() >= 3 && wx::findTropopause(x.tempProfile, ta, tt)) {
        if (!haveTro) {
            const float a0 = XPLMGetDataf(drTa), t0 = XPLMGetDataf(drTt);
            cTa = (std::isfinite(a0) && a0 > 5000.0f && a0 < 20000.0f) ? a0 : ta;
            cTt = (std::isfinite(t0) && t0 > -95.0f && t0 < -20.0f) ? t0 : tt;
            haveTro = true;
        }
        cTa += std::max(-300.0f, std::min(300.0f, ta - cTa));
        cTt += std::max(-1.5f, std::min(1.5f, tt - cTt));
        if (!g_regionSaved.tro) { g_regionSaved.troAlt = XPLMGetDataf(drTa); g_regionSaved.troTemp = XPLMGetDataf(drTt); g_regionSaved.tro = true; }
        XPLMSetDataf(drTa, cTa);
        XPLMSetDataf(drTt, cTt);
    }
}

// Every 20 s, independent of the (much slower) weather watch: the profile for where the aircraft is now, so what the sim's region arrays
// hold follows the aircraft in small steps instead of jumping every few minutes.
void regionTick() {
    if (!g_cfg.enabled || !g_cfg.regionWinds || !g_haveLast || g_lastRes.stations.empty()) return;
    const double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
    wx::Station b = wx::blendAt(g_lastRes.stations, lat, lon);
    wx::XpWx x = wx::buildXp(b, g_cfg, std::time(nullptr), &g_lastRes.aloft, std::time(nullptr) < g_fogUntil);
    pushRegionWinds(x);
}


void weatherWatch() {
    if (!g_haveLast || g_lastRes.stations.empty() || !g_drElev) return;
    const double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon), el = XPLMGetDatad(g_drElev);
    wx::Station b = wx::blendAt(g_lastRes.stations, lat, lon);
    wx::XpWx x = wx::buildXp(b, g_cfg, std::time(nullptr), &g_lastRes.aloft, std::time(nullptr) < g_fogUntil);
    pushRegionWinds(x);
    XPLMWeatherInfo_t info;
    std::memset(&info, 0, sizeof info);
    info.structSize = sizeof info;
    int det = XPLMGetWeatherAtLocation(lat, lon, el + 2.0, &info);
    float cov = 0, base = 0, xcov = 0, xbase = 0;
    for (int i = 0; i < XPLM_NUM_CLOUD_LAYERS; ++i) {
        if (info.cloud_layers[i].coverage > cov) { cov = info.cloud_layers[i].coverage; base = info.cloud_layers[i].alt_base; }
        if (x.clouds[i].coverage > xcov) { xcov = x.clouds[i].coverage; xbase = x.clouds[i].baseM; }
    }
    const wx::Station* nearSt = nullptr;
    double nearD = 1e9;
    for (const auto& st : g_lastRes.stations) {
        double d = wx::distNm(lat, lon, st.lat, st.lon);
        if (d < nearD) { nearD = d; nearSt = &st; }
    }
    logf("weather watch (nearest %s, %.0f nm): X-Plane wind %.0f/%.1f m/s vis %.0f m cloud %.2f@%.0f m | we want wind %.0f/%.1f m/s vis %.0f m cloud %.2f@%.0f m%s",
         nearSt ? nearSt->icao.c_str() : "?", nearSt ? nearD : 0.0, info.wind_layers[0].direction, info.wind_layers[0].speed, info.visibility,
         cov, base, x.windDir, x.windMs, x.visM, xcov, xbase, det ? "" : " (no detailed data)");
    // pressure_alt in a weather read-back is the QNH-style value X-Plane holds for the spot (it read 1008-1045 hPa at FL350), not the air
    // pressure at the aircraft; the real static pressure is in the altitude watch ("outside pressure").
    logf("  pressure: report QNH %.1f hPa | X-Plane's read-back for this spot: QNH-style value %.1f hPa, sea level %.1f hPa (aircraft at %.0f ft; outside pressure is in the altitude watch)",
         x.qnhPa / 100.0f, info.pressure_alt / 100.0f, info.pressure_sl / 100.0f, el / 0.3048);
    for (int i = 0; i < XPLM_NUM_CLOUD_LAYERS; ++i)
        logf("  layer %d: X-Plane type %.1f cov %.2f %.0f-%.0f m | we want type %.1f cov %.2f %.0f-%.0f m (aircraft at %.0f m)", i,
             info.cloud_layers[i].cloud_type, info.cloud_layers[i].coverage, info.cloud_layers[i].alt_base, info.cloud_layers[i].alt_top,
             x.clouds[i].type, x.clouds[i].coverage, x.clouds[i].baseM, x.clouds[i].topM, el);
    logWindProfile(x, info, "the aircraft", lat, lon);
    logCoveringRecords(lat, lon, "the aircraft");
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
    // Only meaningful at a station we have a report for. A station 10 nm away says nothing about the airport you are at (a
    // small field without a METAR gets the blend of its neighbours), and "correcting" against it only made the sky murky.
    if (!best || bestD > 4.0) {
        if (!g_loggedNoNearStation) { g_loggedNoNearStation = true; logf("visibility check: no METAR station within 4 nm (nearest %s, %.0f nm), nothing to compare against", best ? best->icao.c_str() : "none", best ? bestD : 0.0); }
        return;
    }
    g_loggedNoNearStation = false;
    g_checkedIcao = best->icao;
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
    logWindProfile(x, info, best->icao.c_str(), best->lat, best->lon);
    logCoveringRecords(best->lat, best->lon, best->icao.c_str());
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
        copy.immediate = false;   // an immediate update freezes the sim for 10+ s while X-Plane rebuilds; it fades in on its own
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
    copy.immediate = false;   // see above: no freeze
    applyResult(copy, false);
}

// ---------------- start-up gating ----------------
// Scenery loading is the busiest moment of a flight; downloads and weather rebuilds then add to the load. Hold the first
// update back until the plugin has been up for startup_delay_s and X-Plane reports no scenery loading for a few seconds.
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
    return std::chrono::duration<double>(now - g_calmSince).count() >= g_cfg.startupCalmS;
}

Clock::time_point g_lastLoopAt{};
int g_stallLogs = 0;
Clock::time_point g_lastStallLog{};

// ---- Altitude watch: one log line every 10 s while airborne --------------------------------------------------------------------------
// For "the autopilot can't hold altitude": separates the two possible causes. If the indicated altitude moves while the true (geometric)
// altitude stays put, the pressure field under the aircraft is changing (our records' QNH/temperatures); if the true altitude moves with
// vertical air (updrafts/downdrafts from clouds, terrain or turbulence), the aircraft is really being pushed up and down.
bool g_eventDump = false;   // set by the altitude watch on an event: the weather watch prints its full picture at the next loop
struct AltWatch {
    bool init = false;
    XPLMDataRef geo = nullptr, ind = nullptr, palt = nullptr, baro = nullptr, setting = nullptr, vs = nullptr, vy = nullptr, agl = nullptr, apAlt = nullptr, turb = nullptr;
    XPLMDataRef ias = nullptr, tas = nullptr, gs = nullptr, mach = nullptr, wdir = nullptr, wspd = nullptr, zulu = nullptr;
    int n = 0;
    double geoMin = 0, geoMax = 0, indMin = 0, indMax = 0, dMin = 0, dMax = 0, pMin = 0, pMax = 0, vsMin = 0, vsMax = 0, vyMin = 0, vyMax = 0, tMax = 0;
    double iasMin = 0, iasMax = 0, tasMin = 0, tasMax = 0, gsMin = 0, gsMax = 0, mMin = 0, mMax = 0;
    Clock::time_point start;
    Clock::time_point lastSummary, lastEvent;
} g_aw;

double awGet(XPLMDataRef r) {
    if (!r) return NAN;
    const XPLMDataTypeID t = XPLMGetDataRefTypes(r);
    if (t & xplmType_Double) return XPLMGetDatad(r);
    if (t & xplmType_Float) return XPLMGetDataf(r);
    if (t & xplmType_Int) return XPLMGetDatai(r);
    return NAN;
}

void altitudeWatch() {
    AltWatch& a = g_aw;
    if (!a.init) {
        a.init = true;
        a.geo = XPLMFindDataRef("sim/flightmodel/position/elevation");
        a.ind = XPLMFindDataRef("sim/cockpit2/gauges/indicators/altitude_ft_pilot");
        a.palt = XPLMFindDataRef("sim/flightmodel2/position/pressure_altitude");
        a.baro = XPLMFindDataRef("sim/weather/aircraft/barometer_current_pas");
        a.setting = XPLMFindDataRef("sim/cockpit2/gauges/actuators/barometer_setting_in_hg_pilot");
        a.vs = XPLMFindDataRef("sim/flightmodel/position/vh_ind_fpm");
        a.vy = XPLMFindDataRef("sim/weather/aircraft/wind_now_y_msc");
        a.agl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
        a.apAlt = XPLMFindDataRef("sim/cockpit/autopilot/altitude");
        a.turb = XPLMFindDataRef("sim/weather/aircraft/turbulence");
        a.ias = XPLMFindDataRef("sim/flightmodel/position/indicated_airspeed");   // kt
        a.tas = XPLMFindDataRef("sim/flightmodel/position/true_airspeed");        // m/s
        a.gs = XPLMFindDataRef("sim/flightmodel/position/groundspeed");           // m/s
        a.mach = XPLMFindDataRef("sim/flightmodel/misc/machno");
        a.wdir = XPLMFindDataRef("sim/cockpit2/gauges/indicators/wind_heading_deg_mag");
        a.wspd = XPLMFindDataRef("sim/cockpit2/gauges/indicators/wind_speed_kts");
        a.zulu = XPLMFindDataRef("sim/time/zulu_time_sec");
        a.lastSummary = a.lastEvent = Clock::now() - std::chrono::hours(1);
    }
    if (g_cfg.diagVerbosity <= 0) return;
    const double agl = awGet(a.agl);
    if (!(agl > 150.0)) { a.n = 0; return; }   // on the ground or about to land: nothing to watch
    const double geoFt = awGet(a.geo) / 0.3048, ind = awGet(a.ind), d = ind - geoFt, p = awGet(a.baro) / 100.0, vs = awGet(a.vs), vy = awGet(a.vy), tb = awGet(a.turb);
    auto mm = [&](double v, double& lo, double& hi) { if (std::isnan(v)) return; if (a.n == 0 || std::isnan(lo)) { lo = hi = v; } else { lo = std::min(lo, v); hi = std::max(hi, v); } };
    if (a.n == 0) { a.start = Clock::now(); a.geoMin = a.indMin = a.dMin = a.pMin = a.vsMin = a.vyMin = a.iasMin = a.tasMin = a.gsMin = a.mMin = NAN; a.tMax = 0; }
    mm(geoFt, a.geoMin, a.geoMax); mm(ind, a.indMin, a.indMax); mm(d, a.dMin, a.dMax); mm(p, a.pMin, a.pMax); mm(vs, a.vsMin, a.vsMax); mm(vy, a.vyMin, a.vyMax);
    mm(awGet(a.ias), a.iasMin, a.iasMax); mm(awGet(a.tas) / 0.514444, a.tasMin, a.tasMax); mm(awGet(a.gs) / 0.514444, a.gsMin, a.gsMax); mm(awGet(a.mach), a.mMin, a.mMax);
    if (!std::isnan(tb)) a.tMax = std::max(a.tMax, tb);
    ++a.n;
    if (Clock::now() - a.start < std::chrono::seconds(10)) return;
    // Verbosity (diag_verbosity): 2 = this line every 10 s; 1 (default) = a summary every 5 min, plus every window that looks like an
    // event - the indicated-minus-true gap moving by 200+ ft, 2,500+ fpm while the autopilot target is within 1,000 ft, or the ground
    // speed moving by 25+ kt - and then the full weather picture at the aircraft once a minute; 0 = off.
    const double apT = awGet(a.apAlt);
    const bool event = (a.dMax - a.dMin) > 200.0 || (std::max(std::fabs(a.vsMin), std::fabs(a.vsMax)) > 2500.0 && std::fabs(apT - a.indMax) < 1000.0) ||
                       (a.gsMax - a.gsMin) > 25.0;
    const auto nowC = Clock::now();
    const bool summary = nowC - a.lastSummary > std::chrono::minutes(5);
    if (g_cfg.diagVerbosity < 2 && !event && !summary) { a.n = 0; return; }
    if (summary) a.lastSummary = nowC;
    const double z = awGet(a.zulu);
    logf("%s (UTC %02d:%02d:%02d, %.4f,%.4f): IAS %.0f..%.0f kt, TAS %.0f..%.0f kt, Mach %.3f..%.3f, GS %.0f..%.0f kt, wind at the aircraft %03.0f MAG/%.0f kt",
         event ? "ALTITUDE/SPEED EVENT" : "flight state", std::isnan(z) ? 0 : (int)(z / 3600) % 24, std::isnan(z) ? 0 : (int)(z / 60) % 60, std::isnan(z) ? 0 : (int)z % 60,
         XPLMGetDatad(g_drLat), XPLMGetDatad(g_drLon), a.iasMin, a.iasMax, a.tasMin, a.tasMax, a.mMin, a.mMax, a.gsMin, a.gsMax, awGet(a.wdir), awGet(a.wspd));
    if (event && nowC - a.lastEvent > std::chrono::minutes(1)) { a.lastEvent = nowC; g_eventDump = true; }
    logf("altitude watch (%d samples over 10 s, %.0f ft AGL): true %.0f..%.0f ft, indicated %.0f..%.0f ft (indicated minus true %+.0f..%+.0f), outside pressure %.1f..%.1f hPa, "
         "baro set %.2f inHg, VS %+.0f..%+.0f fpm, vertical air %+.1f..%+.1f m/s, turbulence %.2f, AP target %.0f ft",
         a.n, agl / 0.3048, a.geoMin, a.geoMax, a.indMin, a.indMax, a.dMin, a.dMax, a.pMin, a.pMax, awGet(a.setting), a.vsMin, a.vsMax, a.vyMin, a.vyMax, a.tMax, awGet(a.apAlt));
    a.n = 0;
}

// The countdown: what is still to come for a weather built from scratch, and a jump to a new place (which starts one, and a download
// at once instead of after the usual minute)
void loadTick(bool staged) {
    const auto now = Clock::now();
    static auto last = now;
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (g_drLat && g_drLon && g_cfg.enabled) {
        static double pLat = 0, pLon = 0;
        static auto pAt = now;
        const double lat = XPLMGetDatad(g_drLat), lon = XPLMGetDatad(g_drLon);
        const bool valid = std::fabs(lat) > 0.001 || std::fabs(lon) > 0.001;
        // a jump: 20 nm or more, faster than any aircraft flies (the loop may stand still for seconds while the new scenery loads)
        const double gapS = std::max(0.25, std::chrono::duration<double>(now - pAt).count()), jumpNm = valid ? wx::distNm(lat, lon, pLat, pLon) : 0.0;
        if (valid && (pLat != 0 || pLon != 0) && g_firstApplyDone && jumpNm > 20.0 && jumpNm / (gapS / 3600.0) > 2500.0) {
            logf("the aircraft jumped %.0f nm: new weather for the new place", jumpNm);
            g_load.start("New location");
            g_loadFetchIssued = false;
            g_manualRefresh = true; g_reenable = true;   // download now (not held), and fade in
        }
        if (valid) { pLat = lat; pLon = lon; pAt = now; }
    }
    if (!g_cfg.enabled) g_load.stop();
    if (!g_load.active()) { g_loadS = -1; return; }
    wx::LoadPhase ph;
    ph.waitingToStart = !g_loadFetchIssued;
    ph.startInS = !g_everRequested ? 5.0 : 1.0;
    ph.downloading = g_fetchRunning;
    ph.downloadElapsedS = std::chrono::duration<double>(now - g_fetchStartAt).count();
    ph.downloadEstS = g_fetchEstS;
    ph.recordsQueued = staged || !g_pendingTodo.empty() || !g_pendingGone.empty();
    ph.radarDeferred = g_radarDeferred;
    ph.radarInS = g_firstApplyDone ? g_cfg.startupRadarDelayS - std::chrono::duration<double>(now - g_firstApplyAt).count() : (double)g_cfg.startupRadarDelayS;
    ph.heldOutlines = !g_heldLevel.empty();
    ph.heldInS = std::chrono::duration<double>(g_heldLevelAt - now).count();
    ph.sinceLastSendS = g_lastSendAt == Clock::time_point{} ? 1e9 : std::chrono::duration<double>(now - g_lastSendAt).count();
    ph.fadeS = g_lastSendImmediate && !ph.radarDeferred ? 3.0 : (double)g_cfg.loadFadeS;
    g_loadS = g_load.update(ph, dt);
    if (g_loadS < 0) logf("countdown: the weather is in");
}

float flightLoop(float, float, int, void*) {
    if (!g_pluginEnabled) {
        if (g_disableErase) { g_disableErase = false; g_checkPending = false; g_watchOn = false; eraseAll(); }
        return 0.0f;   // stop calling us until the plugin is enabled again
    }
    const auto nowT = Clock::now();
    realWeatherCheck();
    if (g_firstApplyDone) altitudeWatch();
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
    // Moved to another airport (flight reload, reposition, long taxi): the visibility check and its correction belong to the
    // station it looked at, so start them again for the new one, against the station's own value rather than the old correction.
    if (g_haveLast && g_cfg.visAutocal && !g_lastRes.stations.empty() && !g_checkPending && Clock::now() >= g_moveScanAt) {
        g_moveScanAt = Clock::now() + std::chrono::seconds(5);
        if (!(g_drAgl && XPLMGetDataf(g_drAgl) > 1000.0f)) {
            const double la = XPLMGetDatad(g_drLat), lo = XPLMGetDatad(g_drLon);
            const wx::Station* nb = nullptr;
            double nd = 1e9;
            for (const auto& st : g_lastRes.stations) {
                double d = wx::distNm(la, lo, st.lat, st.lon);
                if (d < nd) { nd = d; nb = &st; }
            }
            if (nb && nd <= 4.0 && nb->icao != g_checkedIcao && !g_checkedIcao.empty()) {
                logf("visibility check: now at %s (was %s), checking its visibility again", nb->icao.c_str(), g_checkedIcao.c_str());
                g_checkedIcao = nb->icao;
                g_visK = (float)g_cfg.visScale;
                g_calIter = 0; g_checkTries = 0; g_prevReadVis = 0;
                g_checkPending = true;
                g_checkAt = Clock::now() + std::chrono::seconds(std::max(20, g_cfg.visCheckS));
                g_watchOn = true; g_watchN = 0; g_watchAt = g_checkAt;
            }
        }
    }
    {
        static Clock::time_point nextRegion{};
        if (Clock::now() >= nextRegion) { nextRegion = Clock::now() + std::chrono::seconds(20); regionTick(); }
    }
    windshearTick();
    dryRunwayTick();
    turbulenceTick();
    if (g_eventDump && g_haveLast) { g_eventDump = false; logf("full weather picture at the aircraft for the event above:"); weatherWatch(); }
    if (g_watchOn && Clock::now() >= g_watchAt) {
        weatherWatch();
        ++g_watchN;
        g_watchAt = Clock::now() + std::chrono::seconds(g_watchN < 10 ? 60 : 300);
    }

    if (g_reapplyRequested) {
        g_reapplyRequested = false;
        if (g_haveLast && g_cfg.enabled) {
            wx::FetchResult copy = g_lastRes;
            copy.immediate = false;
            applyResult(copy, false);
        }
    }

    // 1) results from the worker thread
    // In flight, a result whose airports are not all looked up yet waits a few ticks while they are looked up a few milliseconds per
    // tick (each lookup searches X-Plane's navigation database; 8 new airports in one refresh took 30 ms in one frame, 2026-11-07).
    static wx::FetchResult staged;
    static bool haveStaged = false;
    wx::FetchResult res;
    bool got = false;
    if (haveStaged) {
        const auto t0 = Clock::now();
        size_t left = 0;
        for (const auto& st : staged.stations) {
            if (airportCached(st)) continue;
            if (std::chrono::duration<double, std::milli>(Clock::now() - t0).count() >= 4.0) { ++left; continue; }
            airportKnown(st);
        }
        if (left == 0) { res = staged; haveStaged = false; got = true; }
    } else if (g_fetcher.take(res)) {
        got = true;
        if (!res.radarOnly && g_fetchRunning) {
            g_fetchRunning = false;
            const double dur = std::chrono::duration<double>(Clock::now() - g_fetchStartAt).count();
            if (res.ok) g_fetchEstS = std::max(5.0, std::min(120.0, 0.6 * g_fetchEstS + 0.4 * dur));
        }
        if (!g_startupPhase && res.ok && !res.radarOnly && g_cfg.enabled) {
            size_t unknown = 0;
            for (const auto& st : res.stations) if (!airportCached(st)) ++unknown;
            if (unknown > 0) { staged = res; haveStaged = true; got = false; }
        }
    }
    if (got) {
        if (res.radarOnly) {
            // storm radar on its own: keep the last stations/winds, swap in the new storm cells, apply only what changed (non-immediate: X-Plane fades it in)
            if (!res.ok) logf("radar refresh failed: %s", res.error.c_str());
            else if (g_haveLast && g_cfg.enabled) {
                wx::FetchResult merged = g_lastRes;
                merged.radar = res.radar;
                merged.immediate = false;
                merged.radarOnly = true;
                applyResult(merged, true);
            }
        } else if (!res.ok) {
            logf("update failed: %s", res.error.c_str());
            setStatus("Update failed (will retry)", res.error.substr(0, 60));
            // Without this a failed download (a 12 s timeout while the sim was still loading) left the sky as it was until the next regular
            // refresh. Retry in 30 s instead; the regular timer resumes from that attempt.
            if (g_cfg.updateMinutes * 60.0 > 30.0) g_lastRequest = Clock::now() - std::chrono::seconds((long)(g_cfg.updateMinutes * 60.0 - 30.0));
        } else if (g_cfg.enabled) {
            if (!res.error.empty()) logf("%s", res.error.c_str());
            if (res.stations.empty()) setStatus("No airports within 600 nm", "keeping the last weather");
            else { annotateMagneticVariation(res); applyResult(res, true); }
        }
    }

    // the held storm-level outline records go out on their own once the main update is in and 30 s have passed
    if (!g_heldLevel.empty() && g_pendingTodo.empty() && g_pendingGone.empty() && Clock::now() >= g_heldLevelAt) {
        logf("storm intensity outlines: handing %zu records to X-Plane in a separate update (storm_level_contours = 0 turns them off)", g_heldLevel.size());
        g_pendingTodo.swap(g_heldLevel);
        g_heldLevel.clear();
        g_pendingStartup = false;
        g_pendingImmediate = false;
        g_pendingManual = false;
        g_releaseStepS = 0.0;
    }
    flushPending();
    loadTick(haveStaged);

    // 2) switched off from the menu -> hand weather back to X-Plane
    if (g_eraseRequested) {
        g_eraseRequested = false;
        g_checkPending = false;
        g_watchOn = false;
        eraseAll(false);   // switched off in the window: X-Plane fades back to its own weather (an immediate wipe froze the sim)
        regionRestore("weather off");
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
            // Menu refreshes are coalesced: at most one per 60 s, so clicking Refresh right after a radar toggle cannot stack rebuilds.
            const bool manualHold = g_manualRefresh && !g_reenable && g_everRequested && g_firstApplyDone && since < g_cfg.manualRefreshGapS;
            const bool manual = g_manualRefresh && !manualHold;
            if (manualHold) {   // say so (2026-11-10 customer: "refresh weather, nothing changes" right after switching on)
                static Clock::time_point lastHoldMsg{};
                if (now - lastHoldMsg > std::chrono::seconds(5)) {
                    lastHoldMsg = now;
                    char b[96];
                    std::snprintf(b, sizeof b, "Refresh queued: next download in %.0f s", std::max(1.0, g_cfg.manualRefreshGapS - since));
                    setStatus(b, g_status2);
                }
            }
            const bool kick = g_radarKick && g_cfg.radar && g_firstApplyDone && !g_radarDeferred && !g_startupPhase;
            if (kick) g_radarKick = false;
            const bool radarDue = g_cfg.radar && g_firstApplyDone && !g_radarDeferred && !due && !moved && !manual && !g_startupPhase &&
                                  (kick || std::chrono::duration<double>(now - g_lastRadarRequest).count() >= g_cfg.radarMinutes * 60.0);
            if (radarDue) {
                wx::FetchRequest rq;
                rq.lat = lat; rq.lon = lon; rq.cfg = g_cfg; rq.radarOnly = true;
                g_fetcher.request(rq);
                g_lastRadarRequest = now;
            }
            if (due || moved || manual) {
                g_lastRadarRequest = now;
                wx::FetchRequest rq;
                rq.lat = lat; rq.lon = lon; rq.cfg = g_cfg;
                rq.gsKt = g_drGsFetch ? XPLMGetDataf(g_drGsFetch) * 1.943844 : 0; rq.trackDeg = g_drTrkFetch ? XPLMGetDataf(g_drTrkFetch) : 0;
                if (catchUp) g_radarDeferred = false;
                else if (!g_firstApplyDone && g_cfg.startupStaged && g_cfg.radar) { rq.cfg.radar = false; g_radarDeferred = true; }
                rq.immediate = (manual && !g_reenable) || !g_firstApplyDone;
                g_reenable = false;
                g_fetcher.request(rq);
                g_fetchRunning = true; g_fetchStartAt = now;
                if (g_load.active()) g_loadFetchIssued = true;
                g_lastRequest = now;
                g_everRequested = true;
                g_centerLat = lat; g_centerLon = lon; g_haveCenter = true;
                g_manualRefresh = false;
                g_radarKick = false;
                if (since > 1e8 || rq.immediate) setStatus("Downloading NOAA weather...", "");
            }
        }
    }
    return 0.25f;
}

// ---------------- The text window (3.0; the test harness still uses it - the plugin opens the weather window, xpimgui.cpp) ----------------
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
        case wx::LineKind::SvcOk:   c[0] = 0.45f; c[1] = 0.9f; c[2] = 0.45f; break;
        case wx::LineKind::SvcWarn: c[0] = 1.0f; c[1] = 0.8f; c[2] = 0.3f; break;
        case wx::LineKind::SvcDown: c[0] = 1.0f; c[1] = 0.4f; c[2] = 0.4f; break;
        case wx::LineKind::SvcIdle: c[0] = 0.6f; c[1] = 0.6f; c[2] = 0.6f; break;
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
    static std::time_t wrappedAt = 0;
    const std::time_t now = std::time(nullptr);
    // the turbulence worker's services change between weather updates: the text is rebuilt every 10 s as well
    if (g_wrappedVersion == g_viewVersion && g_wrappedWidth == widthPx && now - wrappedAt < 10) return;
    wrappedAt = now;
    g_wrapped.clear();
    const std::vector<wx::ServiceStatus> extra = g_turbFetch.services(now);
    for (const auto& l : wx::buildViewLines(g_lastRes, g_haveLast, g_cfg, now, &extra)) wrapPx(l.text, l.kind, widthPx, g_wrapped);
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

// ---- Max turbulence window: one slider, 0 (no turbulence from WXPlane) to 1.0 (full realism) -------------------------------------------------------------
void keyWin(XPLMWindowID, char, XPLMKeyFlags, char, void*, int);
XPLMCursorStatus cursorWin(XPLMWindowID, int, int, void*);
int wheelWin(XPLMWindowID, int, int, int, int, void*);
XPLMWindowID g_setWin = nullptr;
bool g_setDrag = false;
int g_sliderL = 0, g_sliderR = 0;   // track edges in screen coordinates, set while drawing

void drawSettings(XPLMWindowID w, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(w, &l, &t, &r, &b);
    int cw = 0, ch = 0;
    XPLMGetFontDimensions(xplmFont_Proportional, &cw, &ch, nullptr);
    float white[3] = {0.9f, 0.9f, 0.9f}, grey[3] = {0.7f, 0.7f, 0.7f};
    char buf[160];
    std::snprintf(buf, sizeof buf, "Max turbulence: %.2f", g_cfg.maxTurbulence);
    XPLMDrawString(white, l + 14, t - 14 - ch, buf, nullptr, xplmFont_Proportional);
    g_sliderL = l + 18; g_sliderR = r - 18;
    const int ty = t - 52;
    XPLMDrawTranslucentDarkBox(g_sliderL, ty + 3, g_sliderR, ty - 3);
    const double f = std::max(0.0, std::min(1.0, g_cfg.maxTurbulence));
    const int tx = g_sliderL + (int)std::lround(f * (g_sliderR - g_sliderL));
    XPLMDrawTranslucentDarkBox(tx - 5, ty + 10, tx + 5, ty - 10);
    XPLMDrawTranslucentDarkBox(tx - 3, ty + 8, tx + 3, ty - 8);
    XPLMDrawString(grey, g_sliderL, ty - 18 - ch, (char*)"0 (none)", nullptr, xplmFont_Proportional);
    XPLMDrawString(grey, g_sliderR - 110, ty - 18 - ch, (char*)"1.0 (full realism)", nullptr, xplmFont_Proportional);
    XPLMDrawString(grey, l + 14, b + 12, (char*)"Scales all of WXPlane's turbulence: real data, storms, gusts. Saved on release.", nullptr, xplmFont_Proportional);
}

int clickSettings(XPLMWindowID w, int x, int y, XPLMMouseStatus st, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(w, &l, &t, &r, &b);
    const int ty = t - 52;
    if (st == xplm_MouseDown && std::abs(y - ty) <= 16 && x >= g_sliderL - 8 && x <= g_sliderR + 8) g_setDrag = true;
    if (g_setDrag && (st == xplm_MouseDown || st == xplm_MouseDrag) && g_sliderR > g_sliderL) {
        double f = (double)(x - g_sliderL) / (double)(g_sliderR - g_sliderL);
        f = std::max(0.0, std::min(1.0, f));
        g_cfg.maxTurbulence = std::round(f * 20.0) / 20.0;   // steps of 0.05
    }
    if (st == xplm_MouseUp && g_setDrag) {
        g_setDrag = false;
        wx::saveConfig(g_cfgPath, g_cfg);
        if (g_cfg.enabled && g_haveLast) g_reapplyRequested = true;   // storm and gust turbulence are rebuilt; the real-data part follows within seconds
    }
    return 1;
}

void showSettings() {
    if (!g_setWin) {
        int sl, st, sr, sb;
        XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
        XPLMCreateWindow_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.left = sl + 120; p.right = sl + 120 + 420;
        p.top = st - 140;  p.bottom = st - 140 - 130;
        p.visible = 1;
        p.drawWindowFunc = drawSettings;
        p.handleMouseClickFunc = clickSettings;
        p.handleRightClickFunc = clickWin;
        p.handleKeyFunc = keyWin;
        p.handleCursorFunc = cursorWin;
        p.handleMouseWheelFunc = wheelWin;
        p.refcon = nullptr;
        p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
        p.layer = xplm_WindowLayerFloatingWindows;
        g_setWin = XPLMCreateWindowEx(&p);
        if (g_setWin) XPLMSetWindowTitle(g_setWin, "WXPlane - max turbulence");
    }
    if (g_setWin) { XPLMSetWindowIsVisible(g_setWin, 1); XPLMBringWindowToFront(g_setWin); }
}
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

void menuHandler(void*, void* item);
#ifndef WX_HARNESS
XPLMCommandRef g_cmdWindow = nullptr, g_cmdRefresh = nullptr;
int g_pluginsItem = -1;
void uiBuild(const wx::UiFonts& fonts, float w, float h, bool poppedOut);
int cmdHandler(XPLMCommandRef, XPLMCommandPhase phase, void* ref) {
    if (phase != xplm_CommandBegin) return 1;
    if ((intptr_t)ref == 1) { if (wx::xpui::isOpen()) wx::xpui::hide(); else wx::xpui::open(uiBuild); }
    else menuHandler(nullptr, (void*)(intptr_t)kRefresh);
    return 1;
}

// The airport search (any ICAO code in the world): one lookup at a time on its own thread; the window shows its result.
struct UiSearch {
    std::mutex m;
    std::thread th;
    bool busy = false;
    std::string error;
    std::shared_ptr<const wx::Station> result;
} g_search;
void startSearch(const std::string& icao) {
    std::lock_guard<std::mutex> l(g_search.m);
    if (g_search.busy) return;
    if (g_search.th.joinable()) g_search.th.join();
    g_search.busy = true;
    g_search.error.clear();
    g_search.th = std::thread([icao]() {
        wx::Station st;
        std::string err;
        const bool ok = wx::lookupStation(icao, st, err);
        std::lock_guard<std::mutex> l(g_search.m);
        g_search.busy = false;
        if (ok) { g_search.result = std::make_shared<wx::Station>(st); g_search.error.clear(); }
        else { g_search.result.reset(); g_search.error = err; }
    });
}
void stopSearch() { if (g_search.th.joinable()) g_search.th.join(); }

// The weather window (4.0): fills the window's data from what the plugin already has, and acts on its buttons.
void uiBuild(const wx::UiFonts& fonts, float w, float h, bool poppedOut) {
    wx::UiData d;
    d.res = &g_lastRes; d.haveData = g_haveLast; d.cfg = &g_cfg; d.now = std::time(nullptr);
    d.extraServices = g_turbFetch.services(d.now);
    if (g_drLat && g_drLon) { d.lat = XPLMGetDatad(g_drLat); d.lon = XPLMGetDatad(g_drLon); d.havePos = !(d.lat == 0 && d.lon == 0); }
    if (g_drElev) d.altFt = XPLMGetDatad(g_drElev) / 0.3048;
    if (g_drTrkFetch) d.trackDeg = XPLMGetDataf(g_drTrkFetch);
    static XPLMDataRef drPsi = XPLMFindDataRef("sim/flightmodel/position/psi");   // true heading
    d.headingDeg = drPsi ? XPLMGetDataf(drPsi) : d.trackDeg;
    if (g_drGsFetch) d.gsKt = XPLMGetDataf(g_drGsFetch) * 1.943844;
    d.busy = g_fetcher.busy();
    if (g_everRequested && g_cfg.enabled) d.nextUpdateS = std::max(0.0, g_cfg.updateMinutes * 60.0 - std::chrono::duration<double>(Clock::now() - g_lastRequest).count());
    d.status1 = g_status1; d.status2 = g_status2;
    d.xpRealWeather = g_xpRealWx;
    d.turbOn = g_cfg.enabled && g_cfg.realisticTurbulence;
    d.turbEdr = g_uiTs.edr; d.turbSource = g_uiTs.source; d.turbData = g_uiTs.data; d.turbProfile = g_uiTs.profile; d.turbGrid = g_uiTs.grid; d.turbGridDLat = g_uiTs.gridDLat; d.turbGridDLon = g_uiTs.gridDLon;
    d.turbGridAltFt = g_uiTs.gridAltFt; d.turbGridSeq = g_uiTs.gridSeq;
    d.magVarAt = magVarEastAt;
    {   // X-Plane's time zone where the aircraft is: its local time minus its zulu time (whole quarter hours)
        static XPLMDataRef drLoc = XPLMFindDataRef("sim/time/local_time_sec"), drZulu = XPLMFindDataRef("sim/time/zulu_time_sec");
        if (drLoc && drZulu && d.havePos) {
            double off = (double)XPLMGetDataf(drLoc) - (double)XPLMGetDataf(drZulu);
            while (off > 14 * 3600.0) off -= 86400.0;
            while (off <= -12 * 3600.0) off += 86400.0;
            d.localOffsetS = std::round(off / 900.0) * 900.0;
            d.haveLocalOffset = true;
        }
    }
    if (d.havePos) d.magVarEast = magVarEastAt(d.lat, d.lon);
    uiCloudsTick();
    d.clouds = g_uiClouds;
    d.loadingS = g_loadS; d.loadingWhat = g_load.what();
    d.reports = g_mapReports;
    d.wsOn = g_cfg.enabled && g_cfg.windshear;
    d.wsSources = g_uiTs.wsSources; d.wsHigh = g_uiTs.wsHigh; d.wsMed = g_uiTs.wsMed; d.wsLow = g_uiTs.wsLow; d.wsStrongest = g_uiTs.wsStrongest;
    {
        std::lock_guard<std::mutex> l(g_search.m);
        d.searchBusy = g_search.busy; d.searchError = g_search.error; d.searchResult = g_search.result;
    }
    wx::UiActions act;
    wx::uiDraw(d, fonts, w, h, poppedOut, act);
    if (!act.search.empty()) startSearch(act.search);
    uiTurbGridTick(act.mapAltFt);
    if (act.refresh) menuHandler(nullptr, (void*)(intptr_t)kRefresh);
    if (act.maxTurbChanged || act.maxTurbReleased) g_cfg.maxTurbulence = std::max(0.0, std::min(1.0, act.maxTurb));
    if (act.maxTurbReleased) {
        wx::saveConfig(g_cfgPath, g_cfg);
        if (g_cfg.enabled && g_haveLast) g_reapplyRequested = true;
    }
    if (act.popOut) wx::xpui::togglePopOut();
    // the settings (the 3.0 menu's toggles), with the same effects
    if (act.toggleEnabled) menuHandler(nullptr, (void*)(intptr_t)kEnabled);
    if (act.toggleAtis) menuHandler(nullptr, (void*)(intptr_t)kAtis);
    if (act.toggleAloft) menuHandler(nullptr, (void*)(intptr_t)kAloft);
    if (act.toggleTurbulence) menuHandler(nullptr, (void*)(intptr_t)kTurbulence);
    if (act.toggleWindshear) menuHandler(nullptr, (void*)(intptr_t)kWindshear);
}
#endif

void menuHandler(void*, void* item) {
    switch ((int)(intptr_t)item) {
        case kEnabled:
            g_cfg.enabled = !g_cfg.enabled;
            // switched back on: download at once (not held by the manual-refresh gap) and fade in, instead of the immediate
            // rebuild that paused the sim for seconds
            if (g_cfg.enabled) { g_manualRefresh = true; g_reenable = true; setStatus("Enabled - downloading...", ""); g_load.start("Loading weather"); g_loadFetchIssued = false; }
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
#ifdef WX_HARNESS
            showWindow();   // the test harness has no OpenGL: it checks the text version of the same data
#else
            wx::xpui::open(uiBuild);
#endif
            break;
        case kSettings:
            showSettings();
            break;
        case kTurbulence:
            g_cfg.realisticTurbulence = !g_cfg.realisticTurbulence;   // applied (or X-Plane's values put back) by the loop
            break;
        case kWindshear:
            g_cfg.windshear = !g_cfg.windshear;
            g_wsCmd = g_cfg.windshear ? 1 : -1;   // applied in the pre-flight-model loop (weather API)
            break;
#ifdef WX_HARNESS
        case kRadar:   // radar is always on in the plugin (NOAA MRMS in the US, EUMETNET OPERA in Europe); the test harness can still switch it
            g_cfg.radar = !g_cfg.radar;
            if (g_cfg.enabled) { if (g_cfg.radar && g_haveLast) g_radarKick = true; else g_manualRefresh = true; }
            break;
#endif
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
    std::strcpy(outDesc, "WXPlane " WX_VERSION ": live METAR/ATIS weather, GFS winds and clouds aloft, NOAA MRMS and EUMETNET OPERA radar, turbulence and windshear.");

    g_cfgPath = configPath();
    wx::loadConfig(g_cfgPath, g_cfg);
    wx::saveConfig(g_cfgPath, g_cfg); // writes defaults on first run so the file can be edited

    g_drLat = XPLMFindDataRef("sim/flightmodel/position/latitude");
    g_drGsFetch = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    g_drTrkFetch = XPLMFindDataRef("sim/flightmodel/position/hpath");
    g_drLon = XPLMFindDataRef("sim/flightmodel/position/longitude");
    g_drLevels = XPLMFindDataRef("sim/weather/region/atmosphere_alt_levels_m");
    g_drAgl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
    g_drElev = XPLMFindDataRef("sim/flightmodel/position/elevation");
    g_drFramePeriod = XPLMFindDataRef("sim/operation/misc/frame_rate_period");
    g_drLoading = XPLMFindDataRef("sim/graphics/scenery/async_scenery_load_in_progress"); // null if this build lacks it

#ifndef WX_HARNESS
    // 4.0: one item, Plugins > WXPlane, opens the weather window, where every setting now is. It is a command, so it can be bound to a
    // key or a joystick button too (wxplane/weather_window); wxplane/refresh refreshes the weather.
    g_cmdWindow = XPLMCreateCommand("wxplane/weather_window", "WXPlane: open/close the weather window (GUI)");
    g_cmdRefresh = XPLMCreateCommand("wxplane/refresh", "WXPlane: refresh the weather now");
    if (g_cmdWindow) XPLMRegisterCommandHandler(g_cmdWindow, cmdHandler, 1, (void*)(intptr_t)1);
    if (g_cmdRefresh) XPLMRegisterCommandHandler(g_cmdRefresh, cmdHandler, 1, (void*)(intptr_t)2);
    g_pluginsItem = g_cmdWindow ? XPLMAppendMenuItemWithCommand(XPLMFindPluginsMenu(), "WXPlane", g_cmdWindow) : -1;
    return 1;
#endif
    int sub = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "WXPlane", nullptr, 0);
    g_menu = XPLMCreateMenu("WXPlane", XPLMFindPluginsMenu(), sub, menuHandler, nullptr);
    g_idxEnabled = XPLMAppendMenuItem(g_menu, "Enabled", (void*)(intptr_t)kEnabled, 0);
    g_idxRefresh = XPLMAppendMenuItem(g_menu, "Refresh Weather Data Now", (void*)(intptr_t)kRefresh, 0);
    g_idxShow = XPLMAppendMenuItem(g_menu, "Weather Window...", (void*)(intptr_t)kShow, 0);
    XPLMAppendMenuSeparator(g_menu);
    g_idxAtis = XPLMAppendMenuItem(g_menu, "Use ATIS", (void*)(intptr_t)kAtis, 0);
    g_idxAloft = XPLMAppendMenuItem(g_menu, "Global Winds/Temps & Clouds Aloft (GFS)", (void*)(intptr_t)kAloft, 0);
    g_idxWindshear = XPLMAppendMenuItem(g_menu, "Enable Realistic Windshear", (void*)(intptr_t)kWindshear, 0);
    g_idxTurbulence = XPLMAppendMenuItem(g_menu, "Enable Realistic Turbulence", (void*)(intptr_t)kTurbulence, 0);
    XPLMAppendMenuItem(g_menu, "Max Turbulence...", (void*)(intptr_t)kSettings, 0);   // below every toggle, above the status lines
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
#ifndef WX_HARNESS
    stopSearch();
    wx::xpui::destroy();
    if (g_cmdWindow) XPLMUnregisterCommandHandler(g_cmdWindow, cmdHandler, 1, (void*)(intptr_t)1);
    if (g_cmdRefresh) XPLMUnregisterCommandHandler(g_cmdRefresh, cmdHandler, 1, (void*)(intptr_t)2);
    if (g_pluginsItem >= 0) { XPLMRemoveMenuItem(XPLMFindPluginsMenu(), g_pluginsItem); g_pluginsItem = -1; }
#endif
    if (g_setWin) { XPLMDestroyWindow(g_setWin); g_setWin = nullptr; }
    if (g_menu) { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API int XPluginEnable(void) {
    wx::httpGlobalInit();
    logf("downloads: %s", wx::httpTlsInfo().c_str());
    g_fetcher.start();
    g_turbFetch.start();
    if (XPLMMapExists(XPLM_MAP_USER_INTERFACE)) createMapLayers();
    {   static bool hooked = false; if (!hooked) { hooked = true; XPLMRegisterMapCreationHook(mapCreated, nullptr); } }
    g_enabledAt = Clock::now();
    g_loopStarted = false;
    g_visK = (float)g_cfg.visScale;
    g_calm = false;

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
    g_regenDone = false;
    g_regenAt = Clock::time_point{};
    setStatus(g_cfg.enabled ? "Waiting for sim..." : "Disabled", "");
    if (g_cfg.enabled) { g_load.start("Loading weather"); g_loadFetchIssued = false; }
    g_fetchRunning = false;
    logf("WXPlane %s enabled (update every %d min, search %.0f nm)", WX_VERSION, g_cfg.updateMinutes, g_cfg.searchNm);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    g_pluginEnabled = false;
    g_fetcher.stop();
    g_turbFetch.stop();
    turbRestore("plugin disabled");
    regionRestore("plugin disabled");
    destroyMapLayers();
    wx::httpGlobalCleanup();
    // Give weather back to X-Plane so our fog/cloud records don't outlive the plugin. The weather API may only be used from the
    // pre-flight-model callback, so the erase is done there on the next frame (the loop stays alive until XPluginStop); only if
    // the sim is going away before another frame does XPluginStop erase directly.
    g_disableErase = true;
    windshearForget();     // the windshear cells go with the rest of our records
    if (g_loop) XPLMScheduleFlightLoop(g_loop, -1.0f, 1);   // negative = every frame; the callback stops itself after the erase
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
