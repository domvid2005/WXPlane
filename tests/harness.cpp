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
XPLMDataRef XPLMFindDataRef(const char* n) { return std::strstr(n, "latitude") ? (void*)10 : std::strstr(n, "longitude") ? (void*)11 : (void*)12; }
int XPLMGetDatavf(XPLMDataRef, float* out, int off, int n) {
    static const float lv[13] = {0, 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 15000};
    if (!out) return 13;
    for (int i = 0; i < n && off + i < 13; ++i) out[i] = lv[off + i];
    return n;
}
double XPLMGetDatad(XPLMDataRef r) { return r == (void*)10 ? g_lat : g_lon; }
void XPLMDebugString(const char* s) { std::printf("  log: %s", s); }
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
void XPLMBeginWeatherUpdate(void) { ++g_begin; }
void XPLMEndWeatherUpdate(int incr, int imm) { ++g_end; g_lastIncr = incr; g_lastImm = imm; }
void XPLMSetWeatherAtLocation(double la, double lo, double el, XPLMWeatherInfo_t* i) { g_calls.push_back({"loc", false, *i, la, lo, el}); }
void XPLMSetWeatherAtAirport(const char* id, XPLMWeatherInfo_t* i) { if (std::strcmp(id, "KICT") == 0) g_lastSentVis = i->visibility; g_calls.push_back({id, true, *i, 0, 0, 0}); }

// ---------------- mock libcurl ----------------
struct Easy { long status = 200; std::string url; size_t (*wf)(char*, size_t, size_t, void*) = nullptr; void* wd = nullptr; };
static int g_netRequests = 0, g_fdRequests = 0;
static const char* kAwc =
    "[{\"icaoId\":\"KICT\",\"lat\":37.65,\"lon\":-97.43,\"elev\":407,\"rawOb\":\"KICT 301553Z 00000KT 1/4SM FG VV001 08/08 A3010 RMK AO2\"},"
    "{\"icaoId\":\"KEWK\",\"lat\":38.06,\"lon\":-97.28,\"elev\":467,\"rawOb\":\"KEWK 301555Z 27014G24KT 10SM SCT060 BKN110 14/05 A3005\"},"
    "{\"icaoId\":\"KSLN\",\"lat\":38.79,\"lon\":-97.65,\"elev\":389,\"rawOb\":\"KSLN 301553Z 24012KT 6SM -RA BR OVC009 10/09 A3003\"},"
    "{\"icaoId\":\"KFAKE\",\"lat\":37.2,\"lon\":-97.1,\"elev\":300,\"rawOb\":\"KFAKE 301500Z 18008KT CAVOK 15/05 Q1015\"},"
    "{\"icaoId\":\"KOLD\",\"lat\":37.9,\"lon\":-97.9,\"elev\":300,\"rawOb\":\"KOLD 290900Z 18008KT 10SM CLR 15/05 A3000\"}]";
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
    bool fd = e->url.find("windtemp") != std::string::npos;
    bool ids = e->url.find("ids=KICT,KSLN,KFAR") != std::string::npos || e->url.find("ids=KFAR") != std::string::npos || e->url.find("ids=KICT") != std::string::npos;
    if (fd) ++g_fdRequests;
    const char* body = fd ? (e->url.find("level=low") != std::string::npos ? kFdLow : kFdHigh)
                     : ids ? kFdIds
                     : e->url.find("aviationweather.gov") != std::string::npos ? kAwc
                     : e->url.find("/KICT") != std::string::npos ? kDatis : "{\"error\":\"no data\"}";
    e->status = (body == kAwc || body == kDatis || body == kFdLow || body == kFdHigh || body == kFdIds) ? 200 : 404;
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
    { FILE* f = std::fopen("/tmp/claude-0/-home-claude/c23185cf-c8b3-5194-9c46-5d7884d08eaf/scratchpad/WXPlane.cfg", "w"); std::fputs("vis_check_s = 1\n", f); std::fclose(f); }
    char n[256], s[256], d[256];
    CHECK(XPluginStart(n, s, d) == 1);
    CHECK(XPluginEnable() == 1);
    std::printf("[1] first load: aircraft at FL370 (%.2f, %.2f)\n", g_lat, g_lon);
    CHECK(pump(4));
    CHECK(g_begin == 1 && g_end == 1 && g_lastIncr == 0 && g_lastImm == 1);
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
        CHECK(i.cloud_layers[0].alt_base < 410 && i.cloud_layers[0].alt_top > 430 && i.cloud_layers[0].alt_top < 445); // VV001 = 100 ft deep, above the 407 m field
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
    CHECK(pump(before + 4));
    CHECK(g_end == 2 && g_lastImm == 1 && g_lastIncr == 0);
    CHECK(g_netRequests > reqBefore);
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

    std::printf("[5] disable from menu -> records erased, no further downloads\n");
    int endBefore = g_end;
    g_menuHandler(nullptr, (void*)(intptr_t)1 /*kEnabled*/);
    g_cb(1, 1, 0, nullptr);
    CHECK(g_end == endBefore + 1 && g_lastIncr == 0);
    int reqAfter = g_netRequests;
    for (int i = 0; i < 20; ++i) { g_cb(1, 1, i, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(g_netRequests == reqAfter);

    XPluginDisable();
    XPluginStop();
    std::printf("%s (%d failures)\n", g_fail ? "HARNESS FAILED" : "HARNESS OK", g_fail);
    return g_fail ? 1 : 0;
}
