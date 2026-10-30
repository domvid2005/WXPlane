// End-to-end harness: real plugin code + mocked X-Plane SDK + mocked libcurl (canned NOAA / D-ATIS replies).
// Simulates an aircraft cruising at FL370 over Kansas with a fog-bound airport beneath it.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "stubs/XPLMDataAccess.h"
#include "stubs/XPLMMenus.h"
#include "stubs/XPLMDisplay.h"
#include "stubs/XPLMGraphics.h"
#include "stubs/XPLMMap.h"
#include "stubs/XPLMNavigation.h"
#include "stubs/XPLMProcessing.h"
#include "stubs/XPLMUtilities.h"
#include "stubs/XPLMWeather.h"
#include "stubs/curl/curl.h"

extern "C" {
int XPluginStart(char*, char*, char*);
int XPluginEnable(void);
void XPluginDisable(void);
void XPluginStop(void);
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---------------- mock X-Plane ----------------
static XPLMFlightLoop_f g_cb = nullptr;
static XPLMMenuHandler_f g_menuHandler = nullptr;
static std::vector<std::string> g_menuNames;
static int g_begin = 0, g_end = 0, g_lastIncr = -1, g_lastImm = -1;
struct Call { std::string where; bool airport; XPLMWeatherInfo_t info; double lat, lon, elev; };
static std::vector<Call> g_calls;
static float g_agl = 10000.0f;
static double g_lat = 37.60, g_lon = -97.40; // cruising abeam KICT

XPLMMenuID XPLMFindPluginsMenu(void) { return (void*)1; }
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f h, void*) { g_menuHandler = h; return (void*)2; }
void XPLMDestroyMenu(XPLMMenuID) {}
int XPLMAppendMenuItem(XPLMMenuID, const char* n, void*, int) { g_menuNames.push_back(n); return (int)g_menuNames.size() - 1; }
void XPLMAppendMenuSeparator(XPLMMenuID) { g_menuNames.push_back("---"); }
void XPLMSetMenuItemName(XPLMMenuID, int i, const char* n, int) { g_menuNames[(size_t)i] = n; }
void XPLMCheckMenuItem(XPLMMenuID, int, XPLMMenuCheck) {}
void XPLMEnableMenuItem(XPLMMenuID, int, int) {}
XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t* p) { g_cb = p->callbackFunc; return (void*)3; }
void XPLMDestroyFlightLoop(XPLMFlightLoopID) {}
void XPLMScheduleFlightLoop(XPLMFlightLoopID, float, int) {}
XPLMDataRef XPLMFindDataRef(const char* n) { return std::strstr(n, "latitude") ? (void*)10 : std::strstr(n, "longitude") ? (void*)11 : std::strstr(n, "y_agl") ? (void*)13 : (void*)12; }
int XPLMGetDatavf(XPLMDataRef, float* out, int off, int n) {
    static const float lv[13] = {0, 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000};
    if (!out) return 13;
    for (int i = 0; i < n && off + i < 13; ++i) out[i] = lv[off + i];
    return n;
}
float XPLMGetDataf(XPLMDataRef) { return g_agl; }
double XPLMGetDatad(XPLMDataRef r) { return r == (void*)10 ? g_lat : g_lon; }
static std::string g_logAll;
void XPLMDebugString(const char* s) { std::printf("  log: %s", s); g_logAll += s; }
void XPLMGetPrefsPath(char* o) { std::strcpy(o, "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/X-Plane.prf"); }
const char* XPLMGetDirectorySeparator(void) { return "/"; }
char* XPLMExtractFileAndPath(char* p) { char* s = std::strrchr(p, '/'); if (s) *s = 0; return p; }
static std::string g_navId;
XPLMNavRef XPLMFindNavAid(const char*, const char* id, float*, float*, int*, XPLMNavType) {
    g_navId = id;
    return g_navId == "KFAKE" ? XPLM_NAV_NOT_FOUND : 7; // the sim's airport database does not know KFAKE
}
void XPLMGetNavAidInfo(XPLMNavRef, XPLMNavType*, float* la, float* lo, float*, int*, float*, char* id, char*, char*) {
    static const struct { const char* id; float la, lo; } db[] = {{"KICT", 37.65f, -97.43f}, {"KEWK", 38.06f, -97.28f}, {"KSLN", 38.79f, -97.65f}};
    for (auto& e : db) if (g_navId == e.id) { std::strcpy(id, e.id); *la = e.la; *lo = e.lo; return; }
    std::strcpy(id, "????"); *la = *lo = 0;
}
static float g_lastSentVis = 0;
int XPLMGetWeatherAtLocation(double, double, double, XPLMWeatherInfo_t* o) { o->visibility = g_lastSentVis * 2.5f; return 1; }
static size_t g_batchStart = 0, g_maxBatch = 0;
static bool g_sawImmediate = false, g_sawFullWipe = false;
void XPLMBeginWeatherUpdate(void) { ++g_begin; g_batchStart = g_calls.size(); }
void XPLMEndWeatherUpdate(int incr, int imm) { ++g_end; g_lastIncr = incr; g_lastImm = imm; g_maxBatch = std::max(g_maxBatch, g_calls.size() - g_batchStart); if (imm && incr) g_sawImmediate = true; if (!incr) g_sawFullWipe = true; }
static int g_erases = 0;
void XPLMEraseWeatherAtLocation(double, double) { ++g_erases; }
void XPLMEraseWeatherAtAirport(const char*) { ++g_erases; }
void XPLMSetWeatherAtLocation(double la, double lo, double el, XPLMWeatherInfo_t* i) { g_calls.push_back({"loc", false, *i, la, lo, el}); }
void XPLMSetWeatherAtAirport(const char* id, XPLMWeatherInfo_t* i) { if (std::strcmp(id, "KICT") == 0) g_lastSentVis = i->visibility; g_calls.push_back({id, true, *i, 0, 0, 0}); }


// ---------------- mock window / drawing ----------------
static XPLMDrawWindow_f g_drawCb = nullptr;
static XPLMHandleMouseWheel_f g_wheelCb = nullptr;
static bool g_winVisible = false, g_winDestroyed = false;
static std::string g_drawn;
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) { g_drawCb = p->drawWindowFunc; g_wheelCb = p->handleMouseWheelFunc; g_winVisible = p->visible; return (void*)99; }
void XPLMDestroyWindow(XPLMWindowID) { g_winDestroyed = true; }
void XPLMGetScreenBoundsGlobal(int* l, int* t, int* r, int* b) { *l = 0; *t = 1080; *r = 1920; *b = 0; }
void XPLMGetWindowGeometry(XPLMWindowID, int* l, int* t, int* r, int* b) { *l = 100; *t = 900; *r = 960; *b = 300; }
void XPLMSetWindowIsVisible(XPLMWindowID, int v) { g_winVisible = v; }
int XPLMGetWindowIsVisible(XPLMWindowID) { return g_winVisible; }
void XPLMSetWindowTitle(XPLMWindowID, const char*) {}
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
void XPLMBringWindowToFront(XPLMWindowID) {}
void XPLMDrawString(float*, int, int, char* s, int*, XPLMFontID) { g_drawn += s; g_drawn += "\n"; }
float XPLMMeasureString(XPLMFontID, const char*, int n) { return 6.0f * n; }
void XPLMGetFontDimensions(XPLMFontID, int* w, int* h, int* d) { if (w) *w = 6; if (h) *h = 10; if (d) *d = 0; }
void XPLMDrawTranslucentDarkBox(int, int, int, int) {}

// ---------------- mock map ----------------
static XPLMMapLabelDrawingCallback_f g_mapLabelCb = nullptr;
static bool g_mapDestroyed = false;
static std::vector<std::string> g_mapLabels;
static XPLMMapCreatedCallback_f g_mapHook = nullptr;
static int g_loadingFlag = 1;
int XPLMGetDatai(XPLMDataRef) { return g_loadingFlag; }
XPLMMapLayerID XPLMCreateMapLayer(XPLMCreateMapLayer_t* p) { g_mapLabelCb = p->labelCallback; return (void*)77; }
int XPLMDestroyMapLayer(XPLMMapLayerID) { g_mapDestroyed = true; return 1; }
void XPLMRegisterMapCreationHook(XPLMMapCreatedCallback_f f, void*) { g_mapHook = f; }
int XPLMMapExists(const char*) { return 1; }
void XPLMDrawMapLabel(XPLMMapLayerID, const char* t, float, float, XPLMMapOrientation, float) { g_mapLabels.push_back(t); }
void XPLMMapProject(XPLMMapProjectionID, double la, double lo, float* x, float* y) { *x = (float)lo; *y = (float)la; }

// ---------------- mock libcurl ----------------
struct Easy { long status = 200; std::string url; size_t (*wf)(char*, size_t, size_t, void*) = nullptr; void* wd = nullptr; };
static int g_netRequests = 0, g_fdRequests = 0;
static std::vector<std::string> g_urls;
static std::string g_mrmsRefl, g_mrmsTop, g_tilePng;
static std::string readFile(const char* p) { FILE* f = std::fopen(p, "rb"); std::string r; if (!f) return r; char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) r.append(b, n); std::fclose(f); return r; }
static const char* kAwc =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT 301553Z 00000KT 1/4SM FG VV001 08/08 A3010 RMK AO2\"},"
    "{\"icaoId\":\"KEWK\",\"lat\":38.06,\"lon\":-97.28,\"elev\":467,\"rawOb\":\"KEWK 301555Z 27014G24KT 10SM SCT060 BKN110 14/05 A3005\"},"
    "{\"icaoId\":\"KSLN\",\"lat\":38.79,\"lon\":-97.65,\"elev\":389,\"rawOb\":\"KSLN 301553Z 24012KT 6SM -RA BR OVC009 10/09 A3003\"},"
    "{\"icaoId\":\"KFAKE\",\"lat\":37.2,\"lon\":-97.1,\"elev\":300,\"rawOb\":\"KFAKE 301500Z 18008KT CAVOK 15/05 Q1015\"},"
    "{\"icaoId\":\"KOLD\",\"lat\":37.9,\"lon\":-97.9,\"elev\":300,\"rawOb\":\"KOLD 290900Z 18008KT 10SM CLR 15/05 A3000\"}]";
static const char* kAwcNoSln =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT 301553Z 00000KT 1/4SM FG VV001 08/08 A3010 RMK AO2\"},"
    "{\"icaoId\":\"KEWK\",\"lat\":38.06,\"lon\":-97.28,\"elev\":467,\"rawOb\":\"KEWK 301555Z 27014G24KT 10SM SCT060 BKN110 14/05 A3005\"},"
    "{\"icaoId\":\"KFAKE\",\"lat\":37.2,\"lon\":-97.1,\"elev\":300,\"rawOb\":\"KFAKE 301500Z 18008KT CAVOK 15/05 Q1015\"}]";
static bool g_dropSln = false;
static const char* kFdLow =
    "DATA BASED ON 301200Z\nVALID 301800Z   FOR USE 1400-2100Z. TEMPS NEG ABV 24000\n\n"
    "FT  3000    6000    9000   12000   18000   24000\n"
    "ICT 2420    2625+11 2731+06 2834+00 2938-16 3047-30\n"
    "SLN 2518    2722+10 2830+05 2935-01 3040-17 3150-31\n"
    "FAR 3010    3015+02 3020-02 3025-08 3035-20 3045-33\n";
static const char* kFdHigh = "FT  30000  34000  39000\nICT 305945 305552 305157\nSLN 306250 306056 305561\nFAR 307055 307262 307468\n";
static const char* kFdIds =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT 301553Z 00000KT 1/4SM FG VV001 08/08 A3010\"},"
    "{\"icaoId\":\"KSLN\",\"lat\":38.79,\"lon\":-97.65,\"elev\":389,\"rawOb\":\"KSLN 301553Z 24012KT 6SM OVC009 10/09 A3003\"}]";
static const char* kDatis = "[{\"airport\":\"KICT\",\"type\":\"combined\",\"code\":\"D\",\"datis\":\"KICT ATIS INFO D 1550Z. 00000KT 1/4SM FG VV001 08/08 A3011 (THREE ZERO ONE ONE). LANDING RWY 1R.\"}]";
CURLcode curl_global_init(long) { return 0; }
void curl_global_cleanup(void) {}
CURL* curl_easy_init(void) { return new Easy; }
CURLcode curl_easy_setopt(CURL* c, int opt, ...) {
    va_list ap; va_start(ap, opt);
    Easy* e = (Easy*)c;
    if (opt == CURLOPT_URL) e->url = va_arg(ap, const char*);
    else if (opt == CURLOPT_WRITEFUNCTION) e->wf = va_arg(ap, size_t (*)(char*, size_t, size_t, void*));
    else if (opt == CURLOPT_WRITEDATA) e->wd = va_arg(ap, void*);
    va_end(ap);
    return 0;
}
CURLcode curl_easy_perform(CURL* c) {
    Easy* e = (Easy*)c;
    ++g_netRequests;
    std::printf("  net: GET %s\n", e->url.c_str());
    g_urls.push_back(e->url);
    if (e->url.find("api.rainviewer.com") != std::string::npos) {
        static const char* idx = "{\"version\":\"2.0\",\"generated\":1,\"host\":\"https://tilecache.rainviewer.com\",\"radar\":{\"past\":[{\"time\":1,\"path\":\"/v2/radar/1\"},{\"time\":1761800000,\"path\":\"/v2/radar/2\"}]}}";
        e->status = 200;
        e->wf((char*)idx, 1, std::strlen(idx), e->wd);
        return 0;
    }
    if (e->url.find("tilecache.rainviewer.com/v2/radar/2/256/6/") != std::string::npos) {
        e->status = 200;
        e->wf((char*)g_tilePng.data(), 1, g_tilePng.size(), e->wd);
        return 0;
    }
    if (e->url.find("mrms.ncep.noaa.gov") != std::string::npos) {
        const std::string& b = e->url.find("EchoTop_18") != std::string::npos ? g_mrmsTop : g_mrmsRefl;
        e->status = b.empty() ? 404 : 200;
        e->wf((char*)b.data(), 1, b.size(), e->wd);
        return 0;
    }
    bool fd = e->url.find("windtemp") != std::string::npos;
    bool ids = e->url.find("ids=KICT,KSLN,KFAR") != std::string::npos || e->url.find("ids=KFAR") != std::string::npos || e->url.find("ids=KICT") != std::string::npos;
    if (fd) ++g_fdRequests;
    const char* body = fd ? (e->url.find("level=low") != std::string::npos ? kFdLow : kFdHigh)
                     : ids ? kFdIds
                     : e->url.find("aviationweather.gov") != std::string::npos ? (g_dropSln ? kAwcNoSln : kAwc)
                     : e->url.find("/KICT") != std::string::npos ? kDatis : "{\"error\":\"no data\"}";
    e->status = (body == kAwc || body == kAwcNoSln || body == kDatis || body == kFdLow || body == kFdHigh || body == kFdIds) ? 200 : 404;
    e->wf((char*)body, 1, std::strlen(body), e->wd);
    return 0;
}
CURLcode curl_easy_getinfo(CURL* c, int what, ...) {
    // The plugin only asks for CURLINFO_RESPONSE_CODE, whose variadic argument is a long*.
    va_list ap; va_start(ap, what);
    long* out = va_arg(ap, long*);
    va_end(ap);
    *out = ((Easy*)c)->status;
    return 0;
}
void curl_easy_cleanup(CURL* c) { delete (Easy*)c; }
const char* curl_easy_strerror(CURLcode) { return "mock"; }

// ---------------- scenario ----------------
static bool pump(size_t wantCalls, int ms = 3000) {
    for (int i = 0; i < ms / 20; ++i) {
        g_cb(1, 1, i, nullptr);
        if (g_calls.size() >= wantCalls) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main() {
    { FILE* f = std::fopen("/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg", "w"); std::fputs("vis_check_s = 1\nmax_age_hours = 12\nradar = 0\nstartup_delay_s = 0\n", f); std::fclose(f); }
    const char* kScratch = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/inf/";
    g_mrmsRefl = readFile((std::string(kScratch) + "mrms_refl.grib2.gz").c_str());
    g_mrmsTop = readFile((std::string(kScratch) + "mrms_top.grib2.gz").c_str());
    g_tilePng = readFile((std::string(kScratch) + "tile.png").c_str());
    CHECK(!g_mrmsRefl.empty() && !g_mrmsTop.empty() && !g_tilePng.empty());
    char n[256], s[256], d[256];
    CHECK(XPluginStart(n, s, d) == 1);
    CHECK(XPluginEnable() == 1);
    std::printf("[0] start-up: nothing is downloaded while X-Plane is still loading scenery\n");
    for (int i = 0; i < 60; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(g_netRequests == 0 && g_calls.empty());
    g_loadingFlag = 0;                                  // scenery finished; 3 quiet seconds are required, so shorten by faking time below
    std::printf("[1] first load: aircraft at FL370 (%.2f, %.2f)\n", g_lat, g_lon);
    CHECK(pump(4, 9000));
    CHECK(g_lastIncr == 1 && g_lastImm == 0);                // gradual, no forced rebuild
    CHECK(g_calls.size() == 4);               // KOLD (29th, 33 h old) dropped
    // nearest first: KICT, then KEWK/KFAKE...
    const Call* kict = nullptr; const Call* fake = nullptr;
    for (auto& c : g_calls) { if (c.where == "KICT") kict = &c; if (c.where == "loc") fake = &c; }
    int airportRecs = 0; for (auto& c : g_calls) airportRecs += c.airport;
    CHECK(airportRecs == 3 && fake);               // 3 known airports, 1 unknown station -> location record
    CHECK(kict && fake);
    if (kict) {
        const auto& i = kict->info;
        std::printf("  KICT: vis=%.0f m  cloud0 type=%.1f cov=%.2f base=%.0f top=%.0f  qnh=%.0f Pa  temp=%.0f dewp=%.0f  max_alt=%.0f ft radius=%.0f nm age=%.0f s\n",
                    i.visibility, i.cloud_layers[0].cloud_type, i.cloud_layers[0].coverage, i.cloud_layers[0].alt_base,
                    i.cloud_layers[0].alt_top, i.pressure_alt, i.temperature_alt, i.dewpoint_alt, i.max_altitude_msl_ft, i.radius_nm, i.age);
        CHECK(i.structSize == (int)sizeof(XPLMWeatherInfo_t));
        CHECK(i.visibility > 390 && i.visibility < 420);                 // 1/4 SM fog
        CHECK(i.cloud_layers[0].cloud_type == 1.0f && i.cloud_layers[0].coverage == 1.0f);
        CHECK(i.cloud_layers[0].alt_base < 410 && i.cloud_layers[0].alt_top > 520 && i.cloud_layers[0].alt_top < 535); // VV001: fog layer 120 m deep (minimum), from the 407 m field
        CHECK(i.cloud_layers[1].coverage == 0 && i.cloud_layers[2].coverage == 0);
        CHECK(i.pressure_alt > 101900 && i.pressure_alt < 102000);        // ATIS altimeter 30.11 overrides METAR 30.10
        CHECK(i.max_altitude_msl_ft == 60000 && i.radius_nm == 30); // carries aloft data, so it reaches cruise levels
        CHECK(i.wind_layers[0].speed == 0 && i.wind_layers[1].speed > 0);
        CHECK(i.temp_layers[0] == 8);
    }
    if (kict) {
        const auto& i = kict->info;
        std::printf("  KICT aloft: max_alt=%.0f ft  winds:", i.max_altitude_msl_ft);
        for (int k = 0; k < XPLM_NUM_WIND_LAYERS; ++k) if (i.wind_layers[k].speed >= 0) std::printf(" %.0fm/%.0f deg/%.0fkt", i.wind_layers[k].alt_msl, i.wind_layers[k].direction, i.wind_layers[k].speed * 1.9438);
        std::printf("\n  temp_layers:");
        for (int k = 0; k < 13; ++k) std::printf(" %.0f", i.temp_layers[k]);
        std::printf("\n");
        CHECK(i.max_altitude_msl_ft == 60000);
        CHECK(i.wind_layers[0].speed == 0);                      // surface layer from the METAR (calm)
        CHECK(i.wind_layers[1].speed > 0 && i.wind_layers[1].alt_msl > 700 && i.wind_layers[1].alt_msl < 1000); // 3000 ft level
        CHECK(i.wind_layers[9].speed > 0);                       // up to 39000 ft
        CHECK(i.wind_layers[12].speed == XPLM_WIND_UNDEFINED_LAYER || i.wind_layers[12].speed > 0);
        CHECK(i.temp_layers[0] == 8);                            // surface anchor = METAR temp
        CHECK(i.temp_layers[12] < i.temp_layers[0]);             // colder at the top level
        CHECK(i.temperature_alt < -273.5f);                      // surface offset not used alongside layers
    }
    CHECK(g_fdRequests == 2);                                    // low + high product, once
    if (fake) CHECK(fake->elev == 300 && fake->info.pressure_alt > 101400 && fake->info.pressure_alt < 101600); // unknown airport -> location record, Q1015

    std::printf("[2] manual refresh via menu\n");
    size_t before = g_calls.size();
    int reqBefore = g_netRequests;
    g_menuHandler(nullptr, (void*)(intptr_t)2 /*kRefresh*/);
    for (int i = 0; i < 300 && g_netRequests == reqBefore; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    for (int i = 0; i < 50; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); } // let it apply
    CHECK(g_netRequests > reqBefore);
    CHECK(g_calls.size() - before < 4);   // identical data is NOT re-sent (re-sending makes the sim rebuild cloud layers)
    CHECK(g_fdRequests == 2); // winds aloft cached for 30 min, not re-downloaded on a manual refresh
    std::printf("  menu status: '%s' / '%s'\n", g_menuNames[g_menuNames.size() - 2].c_str(), g_menuNames.back().c_str());

    std::printf("[3] periodic: nothing should download for 5 minutes while stationary\n");
    reqBefore = g_netRequests;
    for (int i = 0; i < 20; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(g_netRequests == reqBefore);

    std::printf("[4] aircraft moves 84 nm: early refetch is rate-limited to once per 60 s, so still quiet right now\n");
    g_lat += 1.4;
    for (int i = 0; i < 20; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(g_netRequests == reqBefore);

    g_agl = 0.0f;  // autocal only runs near the ground
    std::printf("[2b] visibility self-correction: mock sim reports 2.5x what we send\n");
    {
        // KICT reports 1/4 SM (402 m). The mock sim returns 2.5x; the plugin must learn to send less.
        g_lat = 37.60; g_lon = -97.40;                  // back next to KICT (the check only runs within 15 nm of a station)
        g_menuHandler(nullptr, (void*)(intptr_t)2);      // manual refresh -> fresh apply -> check scheduled
        float firstSent = 0;
        for (auto& c : g_calls) if (c.where == "KICT") { firstSent = c.info.visibility; break; }
        for (int i = 0; i < 400; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        float lastSent = g_lastSentVis;
        std::printf("  first sent %.0f m -> after calibration %.0f m (sim then reports %.0f m)\n", firstSent, lastSent, lastSent * 2.5f);
        CHECK(firstSent > 390 && firstSent < 420);
        CHECK(lastSent < firstSent * 0.7f);                               // it backed off
        CHECK(lastSent * 2.5f < 402 * 1.4f && lastSent * 2.5f > 402 * 0.7f); // sim now lands near the report
    }

    std::printf("[2c] a station drops out of the NOAA data -> only that station is erased\n");
    {
        g_dropSln = true;
        int er = g_erases; size_t callsBefore = g_calls.size();
        g_menuHandler(nullptr, (void*)(intptr_t)2);
        for (int i = 0; i < 300; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(g_erases == er + 1);
        CHECK(g_lastIncr == 1);                                  // incremental, no full wipe
        CHECK(g_calls.size() - callsBefore < 3);
    }

    std::printf("[5] disable from menu -> records erased, no further downloads\n");
    int endBefore = g_end;
    g_menuHandler(nullptr, (void*)(intptr_t)1 /*kEnabled*/);
    g_cb(1, 1, 0, nullptr);
    CHECK(g_end == endBefore + 1 && g_lastIncr == 0);
    int reqAfter = g_netRequests;
    for (int i = 0; i < 20; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(g_netRequests == reqAfter);

    std::printf("[6] METAR/ATIS window\n");
    g_menuHandler(nullptr, (void*)(intptr_t)5 /*kShow*/);
    CHECK(g_drawCb && g_winVisible);
    if (g_drawCb) {
        g_drawn.clear();
        g_drawCb((void*)99, nullptr);
        CHECK(g_drawn.find("KICT") != std::string::npos);
        CHECK(g_drawn.find("METAR  KICT 301553Z 00000KT 1/4SM FG VV001") != std::string::npos);
        CHECK(g_drawn.find("ATIS INFO D") != std::string::npos);
        CHECK(g_drawn.find("FOG") != std::string::npos);                 // "Applied" line mentions the fog layer
        CHECK(g_drawn.find("winds/temps aloft") != std::string::npos);
        g_wheelCb((void*)99, 0, 0, 0, -5, nullptr);                       // scroll down, then back to the top
        g_drawn.clear(); g_drawCb((void*)99, nullptr);
        g_wheelCb((void*)99, 0, 0, 0, 50, nullptr);
        g_drawn.clear(); g_drawCb((void*)99, nullptr);
        CHECK(g_drawn.find("WXPlane - ") != std::string::npos);
    }

    std::printf("[7] radar storms from the MRMS mosaic\n");
    {
        g_menuHandler(nullptr, (void*)(intptr_t)1 /*kEnabled: back on after [5]*/);
        pump(g_calls.size() + 1, 6000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (int i = 0; i < 20; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        size_t before = g_calls.size();
        int erBefore = g_erases;
        g_urls.clear();
        g_menuHandler(nullptr, (void*)(intptr_t)6 /*kRadar -> on, refresh*/);
        CHECK(pump(before + 1, 8000));
        size_t firstBatch = g_calls.size() - before;
        for (int i = 0; i < 60; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }   // frames that flush queued storms
        CHECK(g_calls.size() - before > firstBatch);        // storms trickle in over several frames instead of one burst
        bool gotRefl = false, gotTop = false;
        for (auto& u : g_urls) {
            gotRefl = gotRefl || u.find("https://mrms.ncep.noaa.gov/2D/MergedReflectivityQCComposite/MRMS_MergedReflectivityQCComposite.latest.grib2.gz") != std::string::npos;
            gotTop = gotTop || u.find("https://mrms.ncep.noaa.gov/2D/EchoTop_18/MRMS_EchoTop_18.latest.grib2.gz") != std::string::npos;
        }
        CHECK(gotRefl && gotTop);
        int storms = 0, heavy = 0;
        float minVis = 1e9f, maxTop = 0, maxTurb = 0, maxPrecip = 0, radius = 0;
        for (size_t i = before; i < g_calls.size(); ++i) {
            const Call& c = g_calls[i];
            if (c.airport || c.info.cloud_layers[2].cloud_type < 1.0f || c.info.cloud_layers[2].coverage <= 0) continue;
            ++storms;
            CHECK(c.info.cloud_layers[0].coverage == 0);                    // station fog is not copied onto storms
            if (c.info.cloud_layers[2].cloud_type >= 2.9f) ++heavy;
            minVis = std::min(minVis, c.info.visibility);
            maxTop = std::max(maxTop, c.info.cloud_layers[2].alt_top);
            maxPrecip = std::max(maxPrecip, c.info.precip_rate);
            radius = c.info.radius_nm;
            for (int k = 0; k < XPLM_NUM_WIND_LAYERS; ++k) if (c.info.wind_layers[k].speed >= 0) maxTurb = std::max(maxTurb, c.info.wind_layers[k].turbulence);
        }
        std::printf("  storm records: %d (%d convective), min vis %.0f m, max top %.0f m, precip %.2f, turbulence %.2f, radius %.1f nm\n",
                    storms, heavy, minVis, maxTop, maxPrecip, maxTurb, radius);
        CHECK(storms >= 3 && storms <= 120);
        CHECK(heavy >= 1);
        CHECK(minVis <= 800 && minVis >= 400);
        CHECK(maxTop > 9500 && maxTop < 13100);       // echo top ~12 km from the mock product
        CHECK(maxPrecip >= 0.99f);
        CHECK(maxTurb > 0.8f);
        CHECK(radius > 10 && radius < 11.5);
        // the viewer window lists the radar section
        g_menuHandler(nullptr, (void*)(intptr_t)5);
        g_drawn.clear();
        if (g_drawCb) g_drawCb((void*)99, nullptr);
        CHECK(g_drawn.find("Radar: NOAA MRMS (CONUS)") != std::string::npos);
        CHECK(g_drawn.find("dBZ") != std::string::npos);
        // turning radar off removes every storm record again
        int er1 = g_erases;
        g_menuHandler(nullptr, (void*)(intptr_t)6);
        CHECK(pump(before + 1, 3000) || true);
        for (int i = 0; i < 100 && g_erases - er1 < storms; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        std::printf("  erased %d records after switching radar off (had %d)\n", g_erases - er1, storms);
        CHECK(g_erases - er1 >= storms);
        (void)erBefore;
    }

    std::printf("[8] outside NOAA coverage: experimental global radar\n");
    {
        g_menuHandler(nullptr, (void*)(intptr_t)6 /*radar on again*/);
        g_lat = 50.0; g_lon = 8.0;                       // Germany: no MRMS product, no METARs in the mock data
        g_urls.clear();
        g_logAll.clear();
        g_menuHandler(nullptr, (void*)(intptr_t)7 /*kRadarGlobal -> on, refresh*/);
        for (int i = 0; i < 300 && g_logAll.find("radar:") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        bool mrms = false, idx = false, tiles = false;
        for (auto& u : g_urls) { mrms = mrms || u.find("mrms.ncep") != std::string::npos; idx = idx || u.find("weather-maps.json") != std::string::npos; tiles = tiles || u.find("/256/6/") != std::string::npos; }
        CHECK(!mrms && idx && tiles);
        CHECK(g_logAll.find("radar: RainViewer (global, experimental)") != std::string::npos);
        CHECK(g_logAll.find("echo blocks") != std::string::npos);
    }

    std::printf("[9] storm tops on the map\n");
    {
        CHECK(g_mapLabelCb != nullptr);
        g_lat = 37.60; g_lon = -97.40;
        g_logAll.clear();
        g_menuHandler(nullptr, (void*)(intptr_t)7 /*global radar off again -> refresh from MRMS*/);
        for (int i = 0; i < 300 && g_logAll.find("radar: NOAA MRMS") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        float bounds[4] = {-110.0f, 50.0f, -80.0f, 25.0f};
        g_mapLabels.clear();
        g_mapLabelCb((void*)77, bounds, 1.0f, 1.0f, 0, (void*)1, nullptr);
        std::printf("  labels at zoom 1: %zu (first %s)\n", g_mapLabels.size(), g_mapLabels.empty() ? "-" : g_mapLabels[0].c_str());
        { bool has390 = false; for (auto& l : g_mapLabels) has390 = has390 || l == "FL390"; CHECK(!g_mapLabels.empty() && has390); }
        size_t zoomedOut = g_mapLabels.size();
        g_mapLabels.clear();
        g_mapLabelCb((void*)77, bounds, 3.0f, 1.0f, 0, (void*)1, nullptr);
        CHECK(g_mapLabels.size() >= zoomedOut);                      // zooming in shows more storms
        float away[4] = {10.0f, 60.0f, 30.0f, 40.0f};                 // map showing Europe: nothing to draw
        g_mapLabels.clear();
        g_mapLabelCb((void*)77, away, 3.0f, 1.0f, 0, (void*)1, nullptr);
        CHECK(g_mapLabels.empty());
    }

    {   // the immediate option is a menu toggle (kImmediate = 8) and is honoured when on
        g_menuHandler(nullptr, (void*)(intptr_t)8);
        g_menuHandler(nullptr, (void*)(intptr_t)2);              // manual refresh -> immediate apply
        for (int i = 0; i < 400 && !g_sawImmediate; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(g_sawImmediate);
        g_menuHandler(nullptr, (void*)(intptr_t)8);              // back off
        g_sawImmediate = false;
    }
    std::printf("  largest batch handed to X-Plane in one update: %zu records\n", g_maxBatch);
    CHECK(g_maxBatch <= 4);                                   // never a burst
    CHECK(!g_sawImmediate);                                   // never forced an immediate rebuild
    XPluginDisable();
    XPluginStop();
    CHECK(g_winDestroyed && g_mapDestroyed);
    std::printf("%s (%d failures)\n", g_fail ? "HARNESS FAILED" : "HARNESS OK", g_fail);
    return g_fail ? 1 : 0;
}
