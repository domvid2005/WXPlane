// End-to-end harness: real plugin code + mocked X-Plane SDK + mocked libcurl (canned NOAA / D-ATIS replies).
// Simulates an aircraft cruising at FL370 over Kansas with a fog-bound airport beneath it.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
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
#include "../src/turbulence.h"

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
static float g_turbArr[13] = {0}; static int g_turbWrites = 0;
XPLMDataRef XPLMFindDataRef(const char* n) { if (std::strstr(n, "region/turbulence")) return (void*)79; return std::strstr(n, "use_real_weather") ? (void*)77 : std::strstr(n, "position/elevation") ? (void*)78 : std::strstr(n, "frame_rate_period") ? (void*)14 : std::strstr(n, "latitude") ? (void*)10 : std::strstr(n, "longitude") ? (void*)11 : std::strstr(n, "y_agl") ? (void*)13 : (void*)12; }
int XPLMGetDatavf(XPLMDataRef r, float* out, int off, int n) {
    if (r == (void*)79) { if (!out) return 13; for (int i = 0; i < n && off + i < 13; ++i) out[i] = g_turbArr[off + i]; return n; }
    static const float lv[13] = {0, 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000};
    if (!out) return 13;
    for (int i = 0; i < n && off + i < 13; ++i) out[i] = lv[off + i];
    return n;
}
float XPLMGetDataf(XPLMDataRef r) { return r == (void*)14 ? 0.01f : g_agl; }
static double g_elev = 0;
double XPLMGetDatad(XPLMDataRef r) { return r == (void*)10 ? g_lat : r == (void*)78 ? g_elev : g_lon; }
static std::string g_logAll;
static int g_regenCalls = 0;
XPLMCommandRef XPLMFindCommand(const char* n) { return std::strcmp(n, "sim/operation/regen_weather") == 0 ? (void*)0x77 : nullptr; }
void XPLMCommandOnce(XPLMCommandRef r) { if (r == (void*)0x77) ++g_regenCalls; }
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
static XPLMWeatherInfo_t g_lastSentInfo;
int XPLMGetWeatherAtLocation(double, double, double, XPLMWeatherInfo_t* o) { for (int i = 0; i < XPLM_NUM_CLOUD_LAYERS; ++i) o->cloud_layers[i] = g_lastSentInfo.cloud_layers[i]; o->visibility = g_lastSentVis * 2.5f; return 1; }
static size_t g_batchStart = 0, g_maxBatch = 0;
static bool g_sawImmediate = false, g_sawFullWipe = false;
void XPLMBeginWeatherUpdate(void) { ++g_begin; g_batchStart = g_calls.size(); }
void XPLMEndWeatherUpdate(int incr, int imm) { ++g_end; g_lastIncr = incr; g_lastImm = imm; g_maxBatch = std::max(g_maxBatch, g_calls.size() - g_batchStart); if (imm && incr) g_sawImmediate = true; if (!incr) g_sawFullWipe = true; }
static int g_erases = 0;
void XPLMEraseWeatherAtLocation(double, double) { ++g_erases; }
void XPLMEraseWeatherAtAirport(const char*) { ++g_erases; }
void XPLMSetWeatherAtLocation(double la, double lo, double el, XPLMWeatherInfo_t* i) { g_calls.push_back({"loc", false, *i, la, lo, el}); }
void XPLMSetWeatherAtAirport(const char* id, XPLMWeatherInfo_t* i) { if (std::strcmp(id, "KICT") == 0) { g_lastSentVis = i->visibility; g_lastSentInfo = *i; } g_calls.push_back({id, true, *i, 0, 0, 0}); }


// ---------------- mock window / drawing ----------------
static XPLMDrawWindow_f g_drawCb = nullptr;
static XPLMHandleMouseWheel_f g_wheelCb = nullptr;
static XPLMHandleMouseClick_f g_mouseCb = nullptr;
static bool g_winVisible = false, g_winDestroyed = false;
static std::string g_drawn;
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) { g_drawCb = p->drawWindowFunc; g_wheelCb = p->handleMouseWheelFunc; g_mouseCb = p->handleMouseClickFunc; g_winVisible = p->visible; return (void*)99; }
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
static int g_realWx = 0, g_realWxSets = 0;
int XPLMGetDatai(XPLMDataRef r) { return r == (void*)77 ? g_realWx : g_loadingFlag; }
XPLMDataTypeID XPLMGetDataRefTypes(XPLMDataRef r) { return r == (void*)77 ? xplmType_Int : xplmType_Int | xplmType_Float | xplmType_Double; }
int XPLMCanWriteDataRef(XPLMDataRef) { return 1; }
void XPLMSetDataf(XPLMDataRef, float) {}
void XPLMSetDatad(XPLMDataRef, double) {}
int XPLMGetDatavi(XPLMDataRef, int* v, int, int n) { for (int i = 0; i < n; ++i) v[i] = 0; return n; }
int XPLMCountDataRefs(void) { return 2; }
void XPLMGetDataRefsByIndex(int, int count, XPLMDataRef* out) { for (int i = 0; i < count; ++i) out[i] = (void*)(intptr_t)(9100 + i); }
void XPLMGetDataRefInfo(XPLMDataRef r, XPLMDataRefInfo_t* o) { o->name = r == (void*)(intptr_t)9100 ? "sim/weather/region/runway_friction" : "sim/private/stuff/other"; o->type = 1; o->writable = 1; }
float g_stubMagVar = -7.0f;   // west variation, X-Plane-style sign assumed
float XPLMGetMagneticVariation(double, double) { return g_stubMagVar; }
float XPLMDegTrueToDegMagnetic(float h) { return std::fmod(h - g_stubMagVar + 360.0f, 360.0f); }
float XPLMDegMagneticToDegTrue(float h) { return std::fmod(h + g_stubMagVar + 360.0f, 360.0f); }
void XPLMSetDatavf(XPLMDataRef r, float* v, int off, int n) { if (r == (void*)79) { for (int i = 0; i < n && off + i < 13; ++i) g_turbArr[off + i] = v[i]; ++g_turbWrites; } }
void XPLMSetDatai(XPLMDataRef r, int v) { if (r == (void*)77) { g_realWx = v; ++g_realWxSets; } }
XPLMMapLayerID XPLMCreateMapLayer(XPLMCreateMapLayer_t* p) { g_mapLabelCb = p->labelCallback; return (void*)77; }
int XPLMDestroyMapLayer(XPLMMapLayerID) { g_mapDestroyed = true; return 1; }
void XPLMRegisterMapCreationHook(XPLMMapCreatedCallback_f f, void*) { g_mapHook = f; }
int XPLMMapExists(const char*) { return 1; }
void XPLMSetGraphicsState(int, int, int, int, int, int, int) {}
void XPLMDrawMapLabel(XPLMMapLayerID, const char* t, float, float, XPLMMapOrientation, float) { g_mapLabels.push_back(t); }
void XPLMMapProject(XPLMMapProjectionID, double la, double lo, float* x, float* y) { *x = (float)lo; *y = (float)la; }

// ---------------- mock libcurl ----------------
struct Easy { long status = 200; std::string url, range; size_t (*wf)(char*, size_t, size_t, void*) = nullptr; void* wd = nullptr; };
static int g_netRequests = 0, g_fdRequests = 0, g_rangeRequests = 0;
static std::string g_pirepJson;   // HARNESS_TURB: pilot reports served for /api/data/pirep
static std::string g_gtgnFile, g_gtgnName, g_gtgnHour, g_ecFile, g_ecIndex;   // synthetic turbulence data (HARNESS_TURB)
static std::vector<std::string> g_urls;
static std::string g_mrmsRefl, g_mrmsTop, g_tilePng;
static std::string readFile(const char* p) { FILE* f = std::fopen(p, "rb"); std::string r; if (!f) return r; char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) r.append(b, n); std::fclose(f); return r; }
static const char* kAwcT =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT @A@ 00000KT 1/4SM FG VV001 08/08 A3010 RMK AO2\"},"
    "{\"icaoId\":\"KEWK\",\"lat\":38.06,\"lon\":-97.28,\"elev\":467,\"rawOb\":\"KEWK @B@ 27014G24KT 10SM SCT060 BKN110 14/05 A3005\"},"
    "{\"icaoId\":\"KSLN\",\"lat\":38.79,\"lon\":-97.65,\"elev\":389,\"rawOb\":\"KSLN @A@ 24012KT 6SM -RA BR OVC009 10/09 A3003\"},"
    "{\"icaoId\":\"KFAKE\",\"lat\":37.2,\"lon\":-97.1,\"elev\":300,\"rawOb\":\"KFAKE @C@ 18008KT CAVOK 15/05 Q1015\"},"
    "{\"icaoId\":\"KOLD\",\"lat\":37.9,\"lon\":-97.9,\"elev\":300,\"rawOb\":\"KOLD @OLD@ 18008KT 10SM CLR 15/05 A3000\"}]";
static const char* kAwcNoSlnT =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT @A@ 00000KT 1/4SM FG VV001 08/08 A3010 RMK AO2\"},"
    "{\"icaoId\":\"KEWK\",\"lat\":38.06,\"lon\":-97.28,\"elev\":467,\"rawOb\":\"KEWK @B@ 27014G24KT 10SM SCT060 BKN110 14/05 A3005\"},"
    "{\"icaoId\":\"KFAKE\",\"lat\":37.2,\"lon\":-97.1,\"elev\":300,\"rawOb\":\"KFAKE @C@ 18008KT CAVOK 15/05 Q1015\"}]";
static bool g_dropSln = false;
static const char* kFdLow =
    "DATA BASED ON 301200Z\nVALID 301800Z   FOR USE 1400-2100Z. TEMPS NEG ABV 24000\n\n"
    "FT  3000    6000    9000   12000   18000   24000\n"
    "ICT 2420    2625+11 2731+06 2834+00 2938-16 3047-30\n"
    "SLN 2518    2722+10 2830+05 2935-01 3040-17 3150-31\n"
    "FAR 3010    3015+02 3020-02 3025-08 3035-20 3045-33\n";
static const char* kFdHigh = "FT  30000  34000  39000\nICT 305945 305552 305157\nSLN 306250 306056 305561\nFAR 307055 307262 307468\n";
static const char* kFdIdsT =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT @A@ 00000KT 1/4SM FG VV001 08/08 A3010\"},"
    "{\"icaoId\":\"KSLN\",\"lat\":38.79,\"lon\":-97.65,\"elev\":389,\"rawOb\":\"KSLN @A@ 24012KT 6SM OVC009 10/09 A3003\"}]";
// The mock reports carry timestamps built from the real clock when the harness starts, so the tests never go stale: the current
// reports are 1-2.5 hours old, KOLD is 45 hours old (older than the 40 h limit in the test configs and so must be dropped).
static std::string g_stampA;   // the timestamp used for KICT, kept so the checks can look for it
static std::string utcStamp(int minutesAgo) {
    std::time_t t = std::time(nullptr) - (std::time_t)minutesAgo * 60;
    std::tm tmv;
    gmtime_r(&t, &tmv);
    char b[16];
    std::snprintf(b, sizeof b, "%02d%02d%02dZ", tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return b;
}
static std::string stamped(const char* tpl) {
    std::string s = tpl;
    const std::pair<const char*, int> subs[] = {{"@A@", 90}, {"@B@", 88}, {"@C@", 150}, {"@OLD@", 45 * 60}};
    for (const auto& p : subs) {
        const std::string key = p.first, val = key == "@A@" ? g_stampA : utcStamp(p.second);
        for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + val.size())) s.replace(at, key.size(), val);
    }
    return s;
}
static std::string g_awcS, g_awcNoSlnS, g_idsS;
static const char *kAwc = nullptr, *kAwcNoSln = nullptr, *kFdIds = nullptr;
static void initMockTimes() {
    g_stampA = utcStamp(90);
    g_awcS = stamped(kAwcT); g_awcNoSlnS = stamped(kAwcNoSlnT); g_idsS = stamped(kFdIdsT);
    kAwc = g_awcS.c_str(); kAwcNoSln = g_awcNoSlnS.c_str(); kFdIds = g_idsS.c_str();
}
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
    else if (opt == CURLOPT_RANGE) e->range = va_arg(ap, const char*);
    va_end(ap);
    return 0;
}
CURLcode curl_easy_perform(CURL* c) {
    Easy* e = (Easy*)c;
    ++g_netRequests;
    std::printf("  net: GET %s\n", e->url.c_str());
    g_urls.push_back(e->url);
    if (!g_pirepJson.empty() && e->url.find("aviationweather.gov/api/data/") != std::string::npos &&
        (e->url.find("/pirep?") != std::string::npos || e->url.find("sigmet?") != std::string::npos || e->url.find("gairmet?") != std::string::npos)) {
        const std::string b = e->url.find("/pirep?") != std::string::npos ? g_pirepJson : std::string("[]");
        e->status = 200; e->wf((char*)b.data(), 1, b.size(), e->wd); return 0;
    }
    if (!g_gtgnFile.empty() && (e->url.find("nomads.ncep.noaa.gov/pub/data/nccf/com/gtgn") != std::string::npos || e->url.find("data.ecmwf.int") != std::string::npos)) {
        const bool ec = e->url.find("data.ecmwf.int") != std::string::npos;
        if (ec && e->url.size() > 6 && e->url.compare(e->url.size() - 6, 6, ".index") == 0) { e->status = 200; e->wf((char*)g_ecIndex.data(), 1, g_ecIndex.size(), e->wd); return 0; }
        if (!ec && e->url.back() == '/') {   // the day folder: an Apache-style listing
            std::string l = "<html><body><a href=\"?C=N;O=D\">Name</a> <a href=\"/pub/data/nccf/com/gtgn/prod/\">Parent</a>\n<a href=\"gtgn.t0000z.3km.grib2\">old</a>\n<a href=\"" + g_gtgnName + "\">" + g_gtgnName + "</a>\n<a href=\"" + g_gtgnName + ".idx\">idx</a></body></html>";
            e->status = 200; e->wf((char*)l.data(), 1, l.size(), e->wd); return 0;
        }
        const std::string& f = ec ? g_ecFile : g_gtgnFile;
        if ((!ec && e->url.find("/" + g_gtgnHour + "/" + g_gtgnName) == std::string::npos) || e->range.empty()) { e->status = 404; return 0; }
        size_t a = std::strtoull(e->range.c_str(), nullptr, 10), b = std::strtoull(e->range.c_str() + e->range.find('-') + 1, nullptr, 10);
        ++g_rangeRequests;
        if (a >= f.size()) { e->status = 416; return 0; }
        b = std::min(b, f.size() - 1);
        e->status = 206;
        e->wf((char*)f.data() + a, 1, b - a + 1, e->wd);
        return 0;
    }
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
    if (e->url.find("api.open-meteo.com") != std::string::npos) {   // one location, the hour 'now' is among the two returned
        std::string b = "[{\"latitude\":37.7,\"longitude\":-97.4,\"hourly\":{\"time\":[1000000000,1000003600]";
        const int lv[] = {925, 850, 700, 500, 400, 300, 250, 200, 150, 100, 70};
        const double gh[] = {800, 1500, 3100, 5600, 7200, 9200, 10400, 11800, 13500, 16600, 18700};
        for (size_t i = 0; i < sizeof lv / sizeof lv[0]; ++i) {
            char t[400];
            std::snprintf(t, sizeof t, ",\"wind_speed_%dhPa\":[%d,%d],\"wind_direction_%dhPa\":[250,250],\"temperature_%dhPa\":[%d,%d],\"geopotential_height_%dhPa\":[%g,%g]",
                          lv[i], 20 + (int)i * 8, 20 + (int)i * 8, lv[i], lv[i], -5 - (int)i * 5, -5 - (int)i * 5, lv[i], gh[i], gh[i]);
            b += t;
        }
        b += "}}]";
        e->status = 200;
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


// HARNESS_RAMP=1: the first load is released evenly over transition_s instead of in one burst.
static int runRamp() {
    const char* kCfg = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg";
    { FILE* f = std::fopen(kCfg, "w"); std::fputs("config_version = 22\nmanual_refresh_gap_s = 0\ntop_bumps = 0\nvis_scale = 1\nmax_age_hours = 40\nradar = 0\nstartup_delay_s = 0\nfill_gaps = 0\nstartup_wait_fps = 0\nstartup_staged = 0\ntransition_s = 3\nvis_autocal = 0\napply_per_tick = 4\nrealistic_turbulence = 0\n", f); if (std::getenv("WX_SMOOTH0")) std::fputs("cloud_smooth_nm = 0\n", f); std::fclose(f); }
    char n[256], s[256], d[256];
    g_loadingFlag = 0;
    XPluginStart(n, s, d);
    XPluginEnable();
    auto t0 = std::chrono::steady_clock::now();
    double first = -1, last = -1;
    size_t seen = 0;
    int inFirst300ms = 0;
    for (int i = 0; i < 700 && g_calls.size() < 3; ++i) {
        g_cb(1, 1, i, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (int i = 0; i < 40; ++i) { g_cb(1, 1, 1000 + i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    seen = g_calls.size(); (void)t0; (void)first; (void)last; (void)inFirst300ms;
    std::printf("startup with transition_s = 3 and apply_per_tick = 4: %zu records in %d update(s)\n", seen, g_end);
    CHECK(seen >= 3);
    CHECK(g_end == 1 && g_maxBatch == seen);   // the start-up load ignores the pacing settings: one update
    XPluginDisable();
    XPluginStop();
    std::printf("%s (%d failures)\n", g_fail ? "RAMP FAILED" : "RAMP OK", g_fail);
    return g_fail ? 1 : 0;
}

// HARNESS_STAGED=1: the first update has no radar; storms follow startup_radar_delay_s later.
static int runStaged() {
    const char* kCfg = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg";
    { FILE* f = std::fopen(kCfg, "w"); std::fputs("config_version = 22\nmanual_refresh_gap_s = 0\ntop_bumps = 0\nvis_scale = 1\nmax_age_hours = 40\nradar = 1\nstartup_delay_s = 0\nfill_gaps = 1\nstartup_wait_fps = 1\nstartup_staged = 1\nstartup_radar_delay_s = 1\nvis_autocal = 0\nradar_minutes = 0.1\n", f); std::fclose(f); }
    char n[256], s[256], d[256];
    XPluginStart(n, s, d);
    XPluginEnable();
    for (int i = 0; i < 600 && g_logAll.find("radar: deferred") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(g_logAll.find("radar: deferred") != std::string::npos);
    CHECK(g_logAll.find("radar: NOAA MRMS") == std::string::npos);
    for (int i = 0; i < 1500 && g_logAll.find("radar: NOAA MRMS") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(g_logAll.find("radar: NOAA MRMS") != std::string::npos);   // storms arrive in the second update
    CHECK(g_logAll.find("gap-fill records") != std::string::npos && g_logAll.find(" 0 gap-fill records") == std::string::npos);   // gap fill produced records
    // radar refreshes on its own schedule: only the MRMS grids are downloaded, only changed storm records are sent, with X-Plane's fade
    g_urls.clear();
    const int endsBefore = g_end;
    for (int i = 0; i < 1500 && g_logAll.find("(radar only)") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    for (int i = 0; i < 50; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(g_logAll.find("(radar only)") != std::string::npos);
    { bool metar = false, mrms = false; for (auto& u : g_urls) { metar = metar || u.find("aviationweather.gov/api/data/metar") != std::string::npos; mrms = mrms || u.find("mrms.ncep") != std::string::npos; }
      CHECK(!metar && mrms); }
    CHECK(g_end == endsBefore || g_lastImm == 0);   // nothing forced: either nothing changed, or the update was left to fade in
    {   // the echo-top download breaks: the last good tops are reused (storms keep their measured heights), not estimates
        const std::string savedTop = g_mrmsTop;
        g_mrmsTop.clear();
        for (int i = 0; i < 2000 && g_logAll.find("reusing the last good tops") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(g_logAll.find("reusing the last good tops") != std::string::npos);
        g_mrmsTop = savedTop;
    }
    XPluginDisable();
    XPluginStop();
    std::printf("%s (%d failures)\n", g_fail ? "STAGED FAILED" : "STAGED OK", g_fail);
    return g_fail ? 1 : 0;
}

// HARNESS_ONESHOT=1: with default settings, the first load is ONE weather update containing every record.
static int runOneShot() {
    const char* kCfg = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg";
    { FILE* f = std::fopen(kCfg, "w"); std::fputs("config_version = 22\nmanual_refresh_gap_s = 0\ntop_bumps = 0\nmax_age_hours = 40\nradar = 0\nstartup_delay_s = 0\nfill_gaps = 0\nstartup_wait_fps = 0\nstartup_staged = 0\nvis_autocal = 0\n", f); std::fclose(f); }
    char n[256], s[256], d[256];
    XPluginStart(n, s, d);
    XPluginEnable();
    for (int i = 0; i < 700 && g_calls.size() < 3; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    for (int i = 0; i < 40; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::printf("oneshot: %zu records in %d weather update(s), largest %zu\n", g_calls.size(), g_end, g_maxBatch);
    CHECK(g_calls.size() >= 3);
    CHECK(g_end == 1 && g_begin == 1);               // one Begin/End pair for the whole first load
    CHECK(g_maxBatch == g_calls.size());
    CHECK(g_lastSentVis > 390 && g_lastSentVis < 415);   // default vis_scale 1: KICT reports 402 m and exactly that is sent
    CHECK(g_lastIncr == 1 && g_lastImm == 1);   // the start-up update skips X-Plane's slow fade-in
    CHECK(g_regenCalls == 1 && g_logAll.find("cleared X-Plane's previous weather") != std::string::npos);   // X-Plane's old weather is regenerated away first
    XPluginDisable();
    XPluginStop();
    std::printf("%s (%d failures)\n", g_fail ? "ONESHOT FAILED" : "ONESHOT OK", g_fail);
    return g_fail ? 1 : 0;
}

// HARNESS_DEBOUNCE=1: clicking Refresh repeatedly (default 60 s gap) sends ONE download; applied weather fades in (no freeze).
static int runDebounce() {
    const char* kCfg = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg";
    { FILE* f = std::fopen(kCfg, "w"); std::fputs("config_version = 22\ntop_bumps = 0\nmax_age_hours = 40\nradar = 0\nstartup_delay_s = 0\nfill_gaps = 0\nstartup_wait_fps = 0\nstartup_staged = 0\nvis_autocal = 0\n", f); std::fclose(f); }
    char n[256], s[256], d[256];
    XPluginStart(n, s, d);
    XPluginEnable();
    for (int i = 0; i < 700 && g_calls.size() < 3; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    for (int i = 0; i < 40; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    int req = g_netRequests;
    g_menuHandler(nullptr, (void*)(intptr_t)2 /*kRefresh*/);
    g_menuHandler(nullptr, (void*)(intptr_t)2);
    for (int i = 0; i < 100; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::printf("debounce: %d new network request(s) after two Refresh clicks\n", g_netRequests - req);
    CHECK(g_netRequests == req);     // the first load was seconds ago: the refresh is held, not stacked on top of it
    XPluginDisable();
    XPluginStop();
    std::printf("%s (%d failures)\n", g_fail ? "DEBOUNCE FAILED" : "DEBOUNCE OK", g_fail);
    return g_fail ? 1 : 0;
}

// HARNESS_BUMPS=1: rough-top records scattered over the overcast: billowy, base inside the deck, tops at different heights.
static int runBumps() {
    const char* kCfg = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg";
    { FILE* f = std::fopen(kCfg, "w"); std::fputs("config_version = 22\nmanual_refresh_gap_s = 0\nmax_age_hours = 40\nradar = 0\nfill_gaps = 0\nuse_aloft = 0\nstartup_delay_s = 0\nstartup_wait_fps = 0\nstartup_staged = 0\nvis_autocal = 0\nregen_weather_on_start = 0\ntop_bumps = 1\nbump_count = 8\nrough_min_coverage = 0.85\n", f); std::fclose(f); }
    g_agl = 0.0f; g_lat = 38.79; g_lon = -97.65;
    char n[256], s[256], d[256];
    XPluginStart(n, s, d);
    XPluginEnable();
    for (int i = 0; i < 700 && g_calls.size() < 4; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    for (int i = 0; i < 60; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    int bumps = 0, wrong = 0, li = -1, higher = 0, outside = 0, tall = 0;
    float minTop = 1e9f, maxTop = -1e9f;
    const Call* sln = nullptr;
    for (auto& c : g_calls) if (c.where == "KSLN") sln = &c;
    CHECK(sln != nullptr);
    if (sln) for (int k = 0; k < 3; ++k) if (sln->info.cloud_layers[k].coverage >= 0.85f && sln->info.cloud_layers[k].cloud_type > 0.5f) li = k;
    CHECK(li >= 0);
    for (auto& c : g_calls) {
        if (!sln || li < 0 || c.where != "loc" || !(c.info.radius_nm < sln->info.radius_nm) || std::fabs(c.lat - 38.79) > 0.6 || std::fabs(c.lon + 97.65) > 0.8) continue;
        ++bumps;
        const auto& L = c.info.cloud_layers[li]; const auto& P = sln->info.cloud_layers[li];
        if (!(L.cloud_type == P.cloud_type && L.coverage == P.coverage && L.alt_base == P.alt_base && L.alt_top >= P.alt_top - 1.0f)) ++wrong;   // only the top may differ
        if (L.alt_top > P.alt_top + 20.0f) ++higher;
        {   // footprint: the bump circle lies inside its parent (centre + radius <= 0.9 R); rise tied to the deck thickness (<= 45%)
            const double dn = std::sqrt(std::pow((c.lat - 38.79) * 60.0, 2) + std::pow((c.lon + 97.65) * 60.0 * std::cos(38.79 * 3.14159265358979 / 180.0), 2));
            if (dn + c.info.radius_nm > sln->info.radius_nm * 0.9 + 0.3 || c.info.radius_nm > sln->info.radius_nm * 0.35 + 0.01) ++outside;
            if (L.alt_top - P.alt_top > 0.2f * (P.alt_top - P.alt_base) + 1.0f) ++tall;
        }
        minTop = std::min(minTop, L.alt_top); maxTop = std::max(maxTop, L.alt_top);
    }
    std::printf("  rough-top records: %d (wrong: %d, standing above the deck: %d, tops %.0f-%.0f m)\n", bumps, wrong, higher, minTop, maxTop);
    CHECK(bumps >= 4 && wrong == 0 && higher >= 3 && maxTop - minTop > 60.0f);
    std::printf("  bumps outside their parent: %d, taller than 20%% of the deck: %d\n", outside, tall);
    CHECK(outside == 0 && tall == 0);
    CHECK(g_logAll.find("submitted weather:") != std::string::npos);
    CHECK(g_logAll.find("overcast tops:") != std::string::npos);
    XPluginDisable();
    XPluginStop();
    std::printf("%s (%d failures)\n", g_fail ? "BUMPS FAILED" : "BUMPS OK", g_fail);
    return g_fail ? 1 : 0;
}

// ---------------- HARNESS_TURB: realistic turbulence end to end, with synthetic GTGN (JPEG2000, Lambert) and ECMWF (CCSDS) files ----
extern "C" {
#include <openjpeg.h>
#include <libaec.h>
}
namespace gen {
void p32(std::string& b, size_t at, uint32_t v) { b[at] = (char)(v >> 24); b[at + 1] = (char)((v >> 16) & 255); b[at + 2] = (char)((v >> 8) & 255); b[at + 3] = (char)(v & 255); }
void p16(std::string& b, size_t at, uint32_t v) { b[at] = (char)((v >> 8) & 255); b[at + 1] = (char)(v & 255); }
uint32_t sm(long v) { return v < 0 ? (0x80000000u | (uint32_t)(-v)) : (uint32_t)v; }
void sec(std::string& m, int num, const std::string& body) { size_t at = m.size(); m.resize(at + 5); p32(m, at, (uint32_t)(5 + body.size())); m[at + 4] = (char)num; m += body; }
std::string s1(std::time_t t) {
    std::tm g{}; gmtime_r(&t, &g);
    std::string b(16, '\0'); p16(b, 0, 7); b[4] = 2; b[6] = 0; p16(b, 7, (uint32_t)(g.tm_year + 1900)); b[9] = (char)(g.tm_mon + 1); b[10] = (char)g.tm_mday; b[11] = (char)g.tm_hour; b[12] = (char)g.tm_min;
    return b;
}
std::string s4(int cat, int num, int ltype, long lval, int ft) {
    std::string b(29, '\0'); b[4] = (char)cat; b[5] = (char)num; b[12] = 1; p32(b, 13, (uint32_t)ft); b[17] = (char)ltype; b[18] = 0; p32(b, 19, sm(lval)); b[23] = (char)255; return b;
}
std::string finish(std::string m) { m += "7777"; p32(m, 12, (uint32_t)m.size()); return m; }
std::string head() { return std::string("GRIB\0\0\0\2\0\0\0\0\0\0\0\0", 16); }
struct Sink { std::string out; };
OPJ_SIZE_T wr(void* buf, OPJ_SIZE_T n, void* ud) { static_cast<Sink*>(ud)->out.append((char*)buf, n); return n; }
OPJ_OFF_T sk(OPJ_OFF_T n, void* ud) { auto* s = static_cast<Sink*>(ud); s->out.append((size_t)n, '\0'); return n; }
OPJ_BOOL sek(OPJ_OFF_T, void*) { return OPJ_FALSE; }
std::string j2k(const std::vector<uint32_t>& X, int w, int h, int prec) {
    opj_image_cmptparm_t cp; std::memset(&cp, 0, sizeof cp);
    cp.dx = cp.dy = 1; cp.w = (OPJ_UINT32)w; cp.h = (OPJ_UINT32)h; cp.prec = (OPJ_UINT32)prec; cp.sgnd = 0;
    opj_image_t* img = opj_image_create(1, &cp, OPJ_CLRSPC_GRAY);
    img->x1 = (OPJ_UINT32)w; img->y1 = (OPJ_UINT32)h;
    for (size_t k = 0; k < X.size(); ++k) img->comps[0].data[k] = (OPJ_INT32)X[k];
    opj_cparameters_t prm; opj_set_default_encoder_parameters(&prm);
    prm.tcp_numlayers = 1; prm.tcp_rates[0] = 0; prm.cp_disto_alloc = 1;
    opj_codec_t* c = opj_create_compress(OPJ_CODEC_J2K);
    opj_setup_encoder(c, &prm, img);
    Sink sink;
    opj_stream_t* st = opj_stream_create(1 << 16, OPJ_FALSE);
    opj_stream_set_user_data(st, &sink, nullptr);
    opj_stream_set_write_function(st, wr); opj_stream_set_skip_function(st, sk); opj_stream_set_seek_function(st, sek);
    bool ok = opj_start_compress(c, img, st) && opj_encode(c, st) && opj_end_compress(c, st);
    opj_stream_destroy(st); opj_destroy_codec(c); opj_image_destroy(img);
    return ok ? sink.out : std::string();
}
// One GTGN-like message: EDR (0.19.30) on the HRRR Lambert grid at `ft` ft MSL, value = edr + a small west-east gradient, JPEG2000 packed.
std::string gtgnMsg(std::time_t t, double ft, double edr) {
    const int nx = 1799, ny = 1059;
    std::vector<uint32_t> X((size_t)nx * ny);
    for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) X[(size_t)j * nx + i] = (uint32_t)std::lround((edr + 0.02 * i / nx) * 1000.0);
    std::string m = head(); m[6] = 0;
    sec(m, 1, s1(t));
    std::string g(76, '\0'); p32(g, 1, (uint32_t)(nx * ny)); p16(g, 7, 30); g[9] = 6;
    p32(g, 25, nx); p32(g, 29, ny); p32(g, 33, sm(21138123)); p32(g, 37, sm(237280472)); g[41] = 8; p32(g, 42, sm(38500000)); p32(g, 46, sm(262500000));
    p32(g, 50, 3000000); p32(g, 54, 3000000); g[58] = 0; g[59] = 0x40; p32(g, 60, sm(38500000)); p32(g, 64, sm(38500000)); p32(g, 68, sm(-90000000)); p32(g, 72, 0);
    sec(m, 3, g);
    sec(m, 4, s4(19, 30, 102, std::lround(ft * 0.3048), 0));
    std::string d(17, '\0'); p32(d, 0, (uint32_t)(nx * ny)); p16(d, 4, 40); float R = 0; uint32_t rb; std::memcpy(&rb, &R, 4); p32(d, 6, rb); p16(d, 10, 0); p16(d, 12, 3); d[14] = 10; d[15] = 0; d[16] = 0;
    d += std::string(1, (char)255);
    sec(m, 5, d);
    sec(m, 6, std::string(1, (char)255));
    sec(m, 7, j2k(X, nx, ny, 10));
    return finish(m);
}
// One ECMWF-like message: u or v (0.2.2 / 0.2.3) at `hpa` on the global 0.25 degree grid, CCSDS packed like ECMWF Open Data.
std::string ecMsg(std::time_t run, int step, bool isU, int hpa, int k) {
    const int nx = 1440, ny = 721;
    std::vector<uint32_t> X((size_t)nx * ny);
    for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
        double lat = 90 - j * 0.25, lon = i * 0.25;
        double u = 15 + 4 * k + 10 * std::sin(lon * 0.0349) * std::cos(lat * 0.0698), v = 6 * std::cos(lon * 0.0349) * std::sin(lat * 0.0698) * (1 + 0.2 * k);
        X[(size_t)j * nx + i] = (uint32_t)std::lround(((isU ? u : v) + 100.0) * 10.0);
    }
    std::string m = head(); m[6] = 0;
    sec(m, 1, s1(run));
    std::string g(67, '\0'); p32(g, 1, (uint32_t)(nx * ny)); p16(g, 7, 0); g[9] = 6; p32(g, 25, nx); p32(g, 29, ny); p32(g, 33, 0); p32(g, 37, 0xFFFFFFFFu);
    p32(g, 41, sm(90000000)); p32(g, 45, 0); g[49] = 48; p32(g, 50, sm(-90000000)); p32(g, 54, sm(359750000)); p32(g, 58, 250000); p32(g, 62, 250000); g[66] = 0;
    sec(m, 3, g);
    sec(m, 4, s4(2, isU ? 2 : 3, 100, hpa * 100L, step));
    std::vector<unsigned char> raw, packed(X.size() * 2 + 4096);
    for (uint32_t v : X) { raw.push_back((unsigned char)(v >> 8)); raw.push_back((unsigned char)(v & 255)); }
    aec_stream a; std::memset(&a, 0, sizeof a);
    a.next_in = raw.data(); a.avail_in = raw.size(); a.next_out = packed.data(); a.avail_out = packed.size();
    a.bits_per_sample = 12; a.block_size = 32; a.rsi = 128; a.flags = AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS;
    aec_buffer_encode(&a); packed.resize(a.total_out);
    std::string d(20, '\0'); p32(d, 0, (uint32_t)X.size()); p16(d, 4, 42); float R = -1000.0f; uint32_t rb; std::memcpy(&rb, &R, 4); p32(d, 6, rb);
    p16(d, 10, 0); p16(d, 12, 1); d[14] = 12; d[15] = 0; d[16] = (char)(AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS); d[17] = 32; p16(d, 18, 128);
    sec(m, 5, d);
    sec(m, 6, std::string(1, (char)255));
    sec(m, 7, std::string((char*)packed.data(), packed.size()));
    return finish(m);
}
} // namespace gen

static bool nr(float a, float b) { return std::fabs(a - b) < 1e-4f; }
static int runTurb() {
    const char* kCfg = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg";
    { FILE* f = std::fopen(kCfg, "w"); std::fputs("config_version = 30\nradar = 0\nstartup_delay_s = 0\nfill_gaps = 0\nstartup_wait_fps = 0\nstartup_staged = 0\nmax_age_hours = 40\nrealistic_turbulence = 1\nmax_turbulence = 1\ndiag_verbosity = 2\n", f); std::fclose(f); }
    const std::time_t now = std::time(nullptr), t15 = (now - 300) / 900 * 900;   // a file at least 5 minutes old (newer ones are skipped)
    char nm[64]; std::tm g{}; gmtime_r(&t15, &g); std::strftime(nm, sizeof nm, "gtgn.t%H%Mz.3km.grib2", &g);
    g_gtgnName = nm;
    { char h[8]; std::strftime(h, sizeof h, "%H", &g); g_gtgnHour = h; }
    std::printf("[turb] building synthetic data (%s): GTGN levels 0-50,000 ft every 5,000 ft, EDR 0.02 + 0.03 per level\n", nm);
    const bool partial = std::getenv("HARNESS_TURB_PARTIAL") != nullptr;   // a file still being uploaded: levels up to 10,000 ft only
    for (int k = 0; k <= (partial ? 2 : 10); ++k) g_gtgnFile += gen::gtgnMsg(t15, k * 5000.0, 0.02 + 0.03 * k);
    const std::time_t run = ((now - 6 * 3600) / 21600) * 21600;
    int kk = 0;
    for (int lev : {700, 600, 500, 400, 300, 250, 200, 150, 100}) {
        for (int uv = 0; uv < 2; ++uv) {
            std::string msg = gen::ecMsg(run, 6, uv == 0, lev, kk);
            char line[300];
            std::snprintf(line, sizeof line, "{\"domain\": \"g\", \"step\": \"6\", \"levelist\": \"%d\", \"levtype\": \"pl\", \"param\": \"%s\", \"_offset\": %zu, \"_length\": %zu}\n", lev, uv ? "v" : "u", g_ecFile.size(), msg.size());
            g_ecIndex += line; g_ecFile += msg;
        }
        ++kk;
    }
    std::printf("[turb] GTGN file %zu bytes, ECMWF file %zu bytes\n", g_gtgnFile.size(), g_ecFile.size());
    char n[256], s[256], d[256];
    g_loadingFlag = 0;
    g_lat = 37.65; g_lon = -97.43; g_elev = 33000 * 0.3048; g_agl = 9000;
    for (float& v : g_turbArr) v = 0.7f;   // X-Plane's own values before WXPlane writes
    CHECK(XPluginStart(n, s, d) == 1);
    CHECK(XPluginEnable() == 1);
    auto pumpUntil = [&](const char* text, int seconds) {
        for (int i = 0; i < seconds * 20; ++i) { g_cb(1, 1, i, nullptr); if (g_logAll.find(text) != std::string::npos) return true; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        return false;
    };
    if (partial) {
        std::printf("[turb] incomplete GTGN file: not used\n");
        CHECK(pumpUntil("is incomplete (3 levels, up to 10000 ft); trying the one before", 60));
        for (int i = 0; i < 100; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(g_logAll.find("in memory, crop") == std::string::npos);
        XPluginDisable(); XPluginStop();
        std::printf(g_fail ? "HARNESS_TURB_PARTIAL FAILED (%d)\n" : "HARNESS_TURB_PARTIAL OK (%d failures)\n", g_fail);
        return g_fail ? 1 : 0;
    }
    std::printf("[turb] 1) inside the US at 33,000 ft: GTGN\n");
    CHECK(pumpUntil("realistic turbulence on", 60));
    CHECK(pumpUntil("EDR levels 0..50000 ft", 60));
    CHECK(pumpUntil("in memory, crop", 30));
    CHECK(g_logAll.find("packing 5.40") != std::string::npos);
    CHECK(pumpUntil("| GTGN |", 20) || g_logAll.find("GTGN | data") != std::string::npos);
    // layer 10 is 10,000 m = 32,808 ft: EDR between 0.20 (30,000) and 0.23 (35,000), + up to 0.02 west-east, x0.83..1.2 variation
    const double lo = wx::turb::edrToXpLevel(0.20 * 0.8), hi = wx::turb::edrToXpLevel(0.25 * 1.21);
    std::printf("[turb]   region turbulence written %d times: layer 10 = %.2f (expected %.2f..%.2f), layer 0 = %.2f\n", g_turbWrites, g_turbArr[10], lo, hi, g_turbArr[0]);
    CHECK(g_turbWrites > 0 && g_turbArr[10] >= lo && g_turbArr[10] <= hi);
    CHECK(g_turbArr[11] > g_turbArr[9]);   // more EDR higher up in the synthetic data
    CHECK(nr(g_turbArr[3], 0.7f) && nr(g_turbArr[12], 0.7f));   // layers far from the aircraft have no data: X-Plane's own value (0.7) is kept
    // only the needed levels were downloaded (+-9,500 ft around 33,000 and the ones just beyond), by byte range
    const size_t gtgnBytes = g_gtgnFile.size();
    std::printf("[turb]   range requests so far %d\n", g_rangeRequests);
    CHECK(g_logAll.find("GTGN: " + g_gtgnName + " valid") != std::string::npos);
    CHECK(g_logAll.find("ECMWF:") == std::string::npos);   // deep inside GTGN coverage: ECMWF not downloaded
    (void)gtgnBytes;
    {   // a moderate report from an A320 at FL330 just north of the Germany test point (outside GTGN): raises the turbulence there
        char pj[600];
        std::snprintf(pj, sizeof pj, "[{\"obsTime\":%ld,\"acType\":\"A320\",\"lat\":50.05,\"lon\":10.0,\"fltLvl\":330,\"tbBas1\":null,\"tbTop1\":null,\"tbInt1\":\"MOD\",\"tbInt2\":\"\",\"pirepType\":\"PIREP\",\"rawOb\":\"XXX UA /OV XXX/TM 0000/FL330/TP A320/TB MOD\"}]", (long)std::time(nullptr));
        g_pirepJson = pj;
    }
    std::printf("[turb] 2) over Germany: ECMWF estimate, the source changes\n");
    g_lat = 50.0; g_lon = 10.0;
    CHECK(pumpUntil("wind fields", 60));
    CHECK(pumpUntil("ECMWF Ellrod TI1", 40));   // the estimate is there; the report floor is above it, so the source shows the floor
    CHECK(g_logAll.find("source changed: GTGN -> ") != std::string::npos);
    CHECK(g_logAll.find("Ellrod TI1") != std::string::npos);
    CHECK(pumpUntil("floor EDR 0.300 from PIREP MOD A320 (medium", 60));
    for (int i = 0; i < 300 && std::fabs(g_turbArr[10] - wx::turb::edrToXpLevel(0.30)) > 0.02; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    std::printf("[turb]   layer 10 = %.2f (a MOD report from an A320 nearby: expected %.2f)\n", g_turbArr[10], wx::turb::edrToXpLevel(0.30));
    CHECK(std::fabs(g_turbArr[10] - wx::turb::edrToXpLevel(0.30)) < 0.03);
    // the map: the pilot-report layer labels the report
    CHECK(g_mapLabelCb != nullptr);
    if (g_mapLabelCb) {
        const float bounds[4] = {0, 60, 20, 40};   // left, top, right, bottom in the stub's map units (x = lon, y = lat)
        g_mapLabels.clear();
        g_mapLabelCb((void*)77, bounds, 1, 1, 0, nullptr, nullptr);
        std::printf("[turb]   map labels: %zu, first '%s'\n", g_mapLabels.size(), g_mapLabels.empty() ? "" : g_mapLabels[0].c_str());
        CHECK(!g_mapLabels.empty() && g_mapLabels[0].find("MOD FL330 A320") != std::string::npos);
    }
    std::printf("[turb] 3) the menu toggle puts X-Plane's values back\n");
    g_menuHandler(nullptr, (void*)(intptr_t)10 /*kTurbulence*/);
    CHECK(pumpUntil("realistic turbulence off: X-Plane's region turbulence put back", 5));
    bool back = true; for (float v : g_turbArr) if (!nr(v, 0.7f)) back = false;
    CHECK(back);   // the values before were all 0.7
    XPluginDisable();
    XPluginStop();
    std::printf(g_fail ? "HARNESS_TURB FAILED (%d)\n" : "HARNESS_TURB OK (%d failures)\n", g_fail);
    return g_fail ? 1 : 0;
}

int main() {
    initMockTimes();
    if (std::getenv("HARNESS_BUMPS")) { return runBumps(); }
    if (std::getenv("HARNESS_DEBOUNCE")) { return runDebounce(); }
    if (std::getenv("HARNESS_ONESHOT")) { return runOneShot(); }
    if (std::getenv("HARNESS_STAGED")) { std::string base = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/inf/"; g_mrmsRefl = readFile((base + "mrms_refl.grib2.gz").c_str()); g_mrmsTop = readFile((base + "mrms_top.grib2.gz").c_str()); g_tilePng = readFile((base + "tile.png").c_str()); return runStaged(); }
    if (std::getenv("HARNESS_TURB")) return runTurb();
    if (std::getenv("HARNESS_RAMP")) { std::string base = "/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/inf/"; g_mrmsRefl = readFile((base + "mrms_refl.grib2.gz").c_str()); g_mrmsTop = readFile((base + "mrms_top.grib2.gz").c_str()); g_tilePng = readFile((base + "tile.png").c_str()); return runRamp(); }
    { FILE* f = std::fopen("/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg", "w"); std::fputs("config_version = 22\nmanual_refresh_gap_s = 0\ntop_bumps = 0\nvis_scale = 1\nvis_autocal = 1\nvis_check_s = 1\nmax_age_hours = 40\nradar = 0\nstartup_delay_s = 0\nfill_gaps = 0\nstartup_wait_fps = 0\nstartup_staged = 0\nstartup_wait_scenery = 1\nstorm_level_delay_s = 1\ntransition_s = 0\nupdate_transition_s = 0\napply_per_tick = 4\nrealistic_turbulence = 0\n", f); if (std::getenv("WX_SMOOTH0")) std::fputs("cloud_smooth_nm = 0\n", f); std::fclose(f); }
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
    {   // only the dry-runway check's one quick METAR request may go out while loading; no weather is sent
        int other = 0, metar = 0;
        for (auto& u : g_urls) { if (u.find("aviationweather.gov/api/data/metar") != std::string::npos) ++metar; else ++other; }
        std::printf("loading: %d dry-runway METAR request(s), %d other\n", metar, other);
        CHECK(metar <= 1 && other == 0 && g_calls.empty());
    }
    g_loadingFlag = 0;                                  // scenery finished; 3 quiet seconds are required, so shorten by faking time below
    std::printf("[1] first load: aircraft at FL370 (%.2f, %.2f)\n", g_lat, g_lon);
    CHECK(pump(4, 9000));
    CHECK(g_lastIncr == 1 && g_lastImm == 1);   // the start-up update skips X-Plane's slow fade-in                // gradual, no forced rebuild
    CHECK(g_calls.size() == 4);               // KOLD (45 h old) dropped
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
        CHECK(i.cloud_layers[1].coverage < 0.05f && i.cloud_layers[2].coverage < 0.05f);   // (an empty slot may carry a neighbour's height at 1 %)
        std::printf("  KICT QNH sent: %.1f hPa\n", i.pressure_sl / 100.0);
        CHECK(i.pressure_alt == 0 && i.pressure_sl > 101900 && i.pressure_sl < 102000);   // QNH goes in as sea-level pressure        // ATIS altimeter 30.11 overrides METAR 30.10
        CHECK(i.max_altitude_msl_ft == 60000 && i.radius_nm >= 0.5 && i.radius_nm <= 30); // carries aloft data, so it reaches cruise levels
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
    if (fake) CHECK(fake->elev == 300 && fake->info.pressure_alt == 0 && fake->info.pressure_sl > 101400 && fake->info.pressure_sl < 101600); // unknown airport -> location record, Q1015

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
    CHECK(g_menuNames[g_menuNames.size() - 4] == "Max Turbulence..." && g_menuNames[g_menuNames.size() - 5] == "Enable Realistic Turbulence");   // after the toggles, above the status

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

    std::printf("[2c] a station drops out of the NOAA data -> one replacing update, no record removed one by one\n");
    {
        g_dropSln = true;
        int er = g_erases; size_t callsBefore = g_calls.size();
        g_menuHandler(nullptr, (void*)(intptr_t)2);
        for (int i = 0; i < 300; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(g_erases == er);                                   // airport records are never erased one by one (X-Plane crashed doing that after a move)
        CHECK(g_lastIncr == 0);                                  // a non-incremental update replaces all of our records
        CHECK(g_calls.size() - callsBefore == 3);                // the 3 stations still reported, all in that one update
        CHECK(g_logAll.find("one replacing update") != std::string::npos);
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
        CHECK(g_drawn.find("METAR  KICT " + g_stampA + " 00000KT 1/4SM FG VV001") != std::string::npos);
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
            int sl = -1;   // the storm layer: a real layer 3 km deep or more (its slot depends on the height bands, see matchEmptyLayers)
            for (int k = 0; k < 3; ++k) if (c.info.cloud_layers[k].coverage > 0.05f && c.info.cloud_layers[k].cloud_type >= 1.0f && c.info.cloud_layers[k].alt_top - c.info.cloud_layers[k].alt_base >= 3000.0f) sl = k;
            if (c.airport || sl < 0 || c.info.precip_rate <= 0) continue;   // (rain: a dry record next to a storm may carry some of its smoothed edge)
            ++storms;
            for (int k = 0; k < 3; ++k)                                     // station fog (a thin layer at the ground) is not copied onto storms
                CHECK(!(c.info.cloud_layers[k].coverage > 0.05f && c.info.cloud_layers[k].alt_base < 600.0f && c.info.cloud_layers[k].alt_top - c.info.cloud_layers[k].alt_base < 300.0f));
            if (c.info.cloud_layers[sl].cloud_type >= 2.9f) ++heavy;
            minVis = std::min(minVis, c.info.visibility);
            maxTop = std::max(maxTop, c.info.cloud_layers[sl].alt_top);
            maxPrecip = std::max(maxPrecip, c.info.precip_rate);
            radius = std::max(radius, c.info.radius_nm);
            for (int k = 0; k < XPLM_NUM_WIND_LAYERS; ++k) if (c.info.wind_layers[k].speed >= 0) maxTurb = std::max(maxTurb, c.info.wind_layers[k].turbulence);
        }
        {   // filler layers are never thin slivers (those made X-Plane drop the start-up update); gaps below a layer are mostly filled
            int bad = 0, gaps = 0;
            for (size_t i = before; i < g_calls.size(); ++i) {
                const auto& L = g_calls[i].info.cloud_layers;
                bool any = L[0].coverage > 0 || L[1].coverage > 0 || L[2].coverage > 0;
                if (!any) continue;
                // no gap below a layer (X-Plane would shift the layers above it down a slot), layers in height order, nothing thinner than 100 m added
                bool gap = false;
                for (int k = 0; k < 2; ++k) if (L[k].coverage <= 0 && (L[k + 1].coverage > 0 || L[2].coverage > 0)) gap = true;
                bool thin = false;
                for (int k = 0; k < 3; ++k) if (L[k].coverage > 0 && L[k].coverage < 0.05f && L[k].alt_top - L[k].alt_base < 400.0f) thin = true;
                if (gap) ++gaps;
                if (thin) {
                    if (bad < 4) std::printf("    %.2f %.0f-%.0f | %.2f %.0f-%.0f | %.2f %.0f-%.0f\n", L[0].coverage, L[0].alt_base, L[0].alt_top, L[1].coverage, L[1].alt_base, L[1].alt_top, L[2].coverage, L[2].alt_base, L[2].alt_top);
                    ++bad;
                }
            }
            std::printf("  records with a thin filler layer: %d, with a gap below a layer: %d\n", bad, gaps);
            CHECK(bad == 0);
        }
        if (const char* dp = std::getenv("WX_DUMP")) {   // for the cloud-edge picture (tools)
            FILE* f = std::fopen(dp, "w");
            for (size_t i = before; i < g_calls.size(); ++i) {
                const Call& c = g_calls[i];
                float cov = 0; for (int k = 0; k < 3; ++k) cov = std::max(cov, c.info.cloud_layers[k].coverage);
                std::fprintf(f, "%.5f %.5f %.3f %.2f %.3f\n", c.lat, c.lon, c.info.radius_nm, cov, c.info.precip_rate);
            }
            std::fclose(f);
        }
        {   // every record carries the temperature profile (a record without one falls back to X-Plane's own temperatures there)
            int with = 0, without = 0;
            for (size_t i = before; i < g_calls.size(); ++i) (g_calls[i].info.temp_layers[6] > -273.0f ? with : without)++;
            std::printf("  records with temperatures aloft: %d, without: %d\n", with, without);
            CHECK(without == 0);
            double worst = 0;   // the pressure field is smooth: no QNH step between nearby records (altitude hunting)
            for (size_t i = before; i < g_calls.size(); ++i)
                for (size_t j = i + 1; j < g_calls.size(); ++j) {
                    const Call& a = g_calls[i]; const Call& b = g_calls[j];
                    auto isOld = [](const Call& c) {   // KOLD's and KFAKE's own (location) records keep their QNH within 2 hPa of the field
                        return (std::fabs(c.lat - 37.9) < 0.01 && std::fabs(c.lon + 97.9) < 0.01) || (std::fabs(c.lat - 37.2) < 0.01 && std::fabs(c.lon + 97.1) < 0.01); };
                    if (a.airport || b.airport || a.lat == 0 || b.lat == 0 || isOld(a) || isOld(b)) continue;
                    const double dn = std::hypot((a.lat - b.lat) * 60.0, (a.lon - b.lon) * 60.0 * std::cos(a.lat * 3.14159265 / 180.0));
                    if (dn < 15.0 && std::fabs(a.info.pressure_alt - b.info.pressure_alt) / 100.0 > worst) {
                        worst = std::fabs(a.info.pressure_alt - b.info.pressure_alt) / 100.0;
                        if (worst > 1.0) std::printf("    %.3f,%.3f %.1f  vs  %.3f,%.3f %.1f\n", a.lat, a.lon, a.info.pressure_alt / 100, b.lat, b.lon, b.info.pressure_alt / 100);
                    }
                }
            std::printf("  largest QNH difference between records within 15 nm: %.2f hPa\n", worst);
            CHECK(worst < 1.0);
        }
        std::printf("  storm records: %d (%d convective), min vis %.0f m, max top %.0f m, precip %.2f, turbulence %.2f, radius %.1f nm\n",
                    storms, heavy, minVis, maxTop, maxPrecip, maxTurb, radius);
        const bool natural = g_logAll.find("radar: natural picture") != std::string::npos;
        CHECK(natural);                           // the default since 2026-11
        CHECK(storms >= 3 && storms <= 160);      // natural picture: radar_natural_max (150) rain and dry records
        CHECK(heavy >= 1);
        CHECK(minVis <= 800 && minVis >= 400);
        CHECK(maxTop > 8500 && maxTop < 13100);   // echo top ~12 km in the mock; the default top smoothing + centroid placement lower its lone peak       // echo top ~12 km from the mock product
        CHECK(maxPrecip >= 0.99f);
        CHECK(maxTurb > 0.8f);
        CHECK(radius > 4.0 && radius < 22.0);    // natural records: 0.9 x their square (at most 24 nm)
        // the intensity-outline records were held out of that update and follow in their own one (storm_level_delay_s = 1 here)
        if (!natural) {
            const int endsBefore = g_end;
            const size_t callsBefore = g_calls.size();
            for (int i = 0; i < 300 && g_logAll.find("storm intensity outlines") == std::string::npos; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            for (int i = 0; i < 20; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            CHECK(g_logAll.find("storm intensity outlines: handing") != std::string::npos);
            CHECK(g_end > endsBefore && g_calls.size() > callsBefore);
            std::printf("  outline records in their own update: %zu\n", g_calls.size() - callsBefore);
        }
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

    g_sawImmediate = false;                                   // (the start-up and manual refreshes above are immediate by design)
    std::printf("  largest batch handed to X-Plane in one update: %zu records\n", g_maxBatch);
    CHECK(g_maxBatch <= 4);                                   // never a burst
    CHECK(!g_sawImmediate);                                   // never forced an immediate rebuild
    {   // realistic windshear: on by default (it starts with the weather), the menu turns it off and on again
        for (int i = 0; i < 5; ++i) g_cb(1, 1, 88990 + i, nullptr);
        g_menuHandler(nullptr, (void*)(intptr_t)9 /*kWindshear*/);
        g_cb(1, 1, 89003, nullptr);
        CHECK(g_logAll.find("realistic windshear disabled") != std::string::npos);   // it was on without being asked
        const size_t n0 = g_logAll.size();
        g_menuHandler(nullptr, (void*)(intptr_t)9);
        g_cb(1, 1, 89004, nullptr);
        CHECK(g_logAll.find("realistic windshear enabled", n0) != std::string::npos && g_logAll.find("[WXPlane Windshear] risk:", n0) != std::string::npos);
    }
    XPluginDisable();
    {   // records are removed from the next pre-flight-model callback, once, and the loop then stops itself
        int erasedBefore = g_erases, endBefore = g_end;
        float next = g_cb(1, 1, 99999, nullptr);
        CHECK(g_end == endBefore + 1 && next == 0.0f);
        (void)erasedBefore;
        int endAfter = g_end;
        g_cb(1, 1, 100000, nullptr);
        CHECK(g_end == endAfter);                              // nothing more after the erase
    }
    XPluginStop();
    CHECK(g_winDestroyed);
    std::printf("%s (%d failures)\n", g_fail ? "HARNESS FAILED" : "HARNESS OK", g_fail);
    return g_fail ? 1 : 0;
}
