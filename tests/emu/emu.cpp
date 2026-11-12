// WXPlane weather window emulator: the real window (src/wxui.cpp) compiled to WebAssembly with Emscripten and drawn with WebGL2,
// fed with recorded Boston data and a simulated flight (take-off from KBOS, climb to FL350 heading south-west), so the window can be
// tried in a browser without X-Plane. Built by tests/emu/build.sh; nothing here is part of the plugin.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "metar.h"
#include "wxmodel.h"
#include "wxui.h"
#include "geo.h"

using namespace wx;

namespace {

const float W = 1240, H = 820;
FetchResult g_res;
Config g_cfg;
UiData g_d;
UiFonts g_f;
std::shared_ptr<hz::Snapshot> g_snap;
double g_t = 0;                 // seconds since the start
const std::time_t kStart = 1794451800;   // 2026-11-12 02:50Z (when the data was recorded)
double g_refreshAt = -1;        // a refresh is "downloading" until then
double g_loadUntil = 45;        // the start-up countdown
double g_lastGridAlt = -1e9; double g_gridAt = -1;
bool g_paused = false;
float g_fbScale = 1;
GLuint g_prog = 0, g_vbo = 0, g_ibo = 0, g_vao = 0, g_fontTex = 0, g_logoTex = 0;
GLint g_locProj = -1, g_locTex = -1, g_locAlpha = -1;
std::vector<unsigned char> g_logo; int g_lw = 0, g_lh = 0;
// the flight
double g_lat = 42.3606, g_lon = -71.0106, g_alt = 20, g_hdg = 220, g_gs = 0;

Station mk(const char* id, double lat, double lon, double elevM, const char* raw) {
    Station s;
    s.icao = id; s.lat = lat; s.lon = lon; s.elevM = elevM; s.rawMetar = raw;
    parseReport(raw, s.report);
    return s;
}

void buildData() {
    const std::time_t now = kStart;
    FetchResult& res = g_res;
    res.ok = true; res.fetchedAt = now - 70; res.centerLat = 42.36; res.centerLon = -71.01;
    res.stations = {
        mk("KBOS", 42.3606, -71.0106, 6, "KBOS 120154Z 03006KT 10SM OVC200 10/03 A2988 RMK AO2 SLP118"),
        mk("KBED", 42.4700, -71.2890, 40, "KBED 120156Z AUTO 00000KT 10SM CLR 08/02 A2989 RMK AO2"),
        mk("KOWD", 42.1905, -71.1729, 15, "KOWD 120153Z AUTO 36004KT 10SM CLR 08/03 A2989 RMK AO2"),
        mk("KBVY", 42.5842, -70.9165, 33, "KBVY 120153Z AUTO 00000KT 10SM CLR 07/03 A2988 RMK AO2"),
        mk("KLWM", 42.7172, -71.1234, 45, "KLWM 120154Z AUTO 32003KT 10SM CLR 06/01 A2989"),
        mk("KORH", 42.2673, -71.8757, 306, "KORH 120154Z AUTO 31005KT 10SM SCT055 BKN070 06/M01 A2990"),
        mk("KPVD", 41.7240, -71.4282, 17, "KPVD 120151Z AUTO 32004KT 10SM FEW050 09/02 A2989"),
        mk("KEWB", 41.6761, -70.9569, 24, "KEWB 120153Z AUTO 00000KT 4SM -RA BR OVC014 08/06 A2988"),
        mk("KHYA", 41.6693, -70.2804, 16, "KHYA 120156Z AUTO 02008KT 2SM RA BR OVC008 09/08 A2987"),
        mk("KCON", 43.2027, -71.5023, 104, "KCON 120151Z AUTO 00000KT 10SM CLR 03/M01 A2990"),
        mk("KBDL", 41.9389, -72.6832, 53, "KBDL 120151Z 30007KT 10SM -RA BKN045 OVC080 08/04 A2989"),
        mk("KALB", 42.7483, -73.8017, 87, "KALB 120151Z 28009G18KT 7SM -TSRA BKN035CB OVC070 09/06 A2987"),
        mk("KPSF", 42.4268, -73.2893, 361, "KPSF 120154Z AUTO 27012G22KT 3SM +RA BR BKN020 OVC045 08/07 A2988"),
        mk("KORE", 42.5701, -72.2886, 168, "KORE 120155Z AUTO 29006KT 10SM BKN060 07/02 A2990"),
        mk("KMHT", 42.9326, -71.4357, 81, "KMHT 120153Z 31005KT 10SM FEW080 06/00 A2990"),
        mk("KPWM", 43.6462, -70.3093, 23, "KPWM 120151Z 33004KT 10SM CLR 04/M02 A2991"),
        mk("KACK", 41.2531, -70.0602, 14, "KACK 120153Z 02012KT 3SM BR OVC006 10/09 A2986")};
    res.stations[0].hasAtis = true; res.stations[0].atisLetter = "Y";
    res.stations[0].atisText = "BOS ATIS INFO Y 0154Z. 04006KT 10SM OVC200 10/03 A2988 (TWO NINER EIGHT EIGHT). ILS RWY 22L APCH IN USE. DEPG RWY 22R. ADVS YOU HAVE INFO Y.";
    spreadHighCloud(res.stations, 80.0);
    for (auto& s : res.stations) s.distNm = 0;
    const double prof[][4] = {{2441, 236, 2, 4}, {4705, 285, 13, -1}, {9793, 267, 24, -7}, {18291, 259, 47, -19}, {23614, 258, 55, -31},
                              {30120, 250, 65, -45}, {34027, 255, 66, -52}, {38638, 255, 80, -57}, {44521, 262, 70, -59}, {52767, 260, 44, -60}, {60059, 274, 30, -62}};
    for (int k = 0; k < 25; ++k) {   // GFS points, 1 degree apart, the jet a little stronger to the south
        AloftStation a;
        a.id = "GFS" + std::to_string(k); a.model = true; a.located = true;
        a.lat = 40 + k / 5; a.lon = -74 + k % 5;
        for (const auto& p : prof) {
            AloftLevel L; L.altFt = p[0]; L.hasWind = true; L.dirDeg = std::fmod(p[1] + (a.lat - 42) * 6 + 360, 360); L.speedKt = p[2] * (1.0 + (42 - a.lat) * 0.08 + (a.lon + 72) * 0.03);
            L.hasTemp = true; L.tempC = p[3] + (42 - a.lat) * 1.2; a.levels.push_back(L);
        }
        res.aloft.push_back(a);
    }
    const struct { const char* id; double lat, lon, dirOff, ktMul; } fd[] = {{"BOS", 42.36, -71.0, 0, 1.0}, {"ALB", 42.75, -73.8, -15, 0.9}, {"ACK", 41.25, -70.06, 10, 1.15},
                                                                          {"PWM", 43.65, -70.31, -25, 0.8}, {"EMI", 41.55, -71.6, 5, 1.05}, {"CON", 43.2, -71.5, -10, 0.85},
                                                                          {"BDL", 41.94, -72.68, -5, 0.95}, {"JFK", 40.64, -73.78, 8, 1.2}, {"BTV", 44.47, -73.15, -20, 0.75}};
    for (const auto& q : fd) {
        AloftStation a;
        a.id = q.id; a.located = true; a.lat = q.lat; a.lon = q.lon;
        for (const auto& p : prof) {
            if (p[0] > 39500) break;
            AloftLevel L; L.altFt = p[0]; L.hasWind = true; L.dirDeg = std::fmod(p[1] + q.dirOff + 360, 360); L.speedKt = p[2] * q.ktMul; L.hasTemp = p[0] > 5000; L.tempC = p[3] + q.dirOff * 0.1;
            a.levels.push_back(L);
        }
        res.aloft.push_back(a);
    }
    res.radar.ok = true; res.radar.attempted = true; res.radar.source = "NOAA MRMS (CONUS)"; res.radar.dataTime = now - 240; res.radar.topsFromRadar = true; res.radar.natural = true;
    // a line of showers and storms west of Boston, built from small radar blocks like the real (natural) picture
    for (int i = 0; i < 70; ++i) {
        const double t = i / 69.0;
        const double la0 = 41.2 + 1.9 * t, lo0 = -72.9 + 1.35 * t + 0.12 * std::sin(t * 9);
        for (int j = -3; j <= 3; ++j) {
            RadarCell c;
            c.lat = la0 + j * 0.035 * std::cos(t * 3) + 0.02 * std::sin(i * 2.1 + j);
            c.lon = lo0 + j * 0.05 + 0.02 * std::cos(i * 1.3 - j);
            const double core = std::exp(-j * j / 3.0) * (0.6 + 0.4 * std::sin(t * 14 + 1));
            c.dbz = (float)(15 + 40 * core); c.dbzFine = c.dbz; c.rateMmH = (float)std::pow(10.0, (c.dbz - 23) / 16.0);
            c.topM = 2500 + 9000 * core; c.topKnown = true; c.natural = true; c.sizeNm = 3.2;
            c.key = "c" + std::to_string(i) + "_" + std::to_string(j);
            if (c.dbz >= 18) res.radar.cells.push_back(c);
        }
    }
    res.radar.echoBlocks = (int)res.radar.cells.size();
    res.services = {
        {"NOAA Aviation Weather Center", "METARs", SvcState::Ok, "40 report(s)", now - 70},
        {"D-ATIS (clowd.io)", "ATIS", SvcState::Ok, "1 airport(s)", now - 70},
        {"Open-Meteo (NOAA GFS)", "winds and temperatures aloft", SvcState::Ok, "25 model point(s), downloaded 02:05Z", now - 2700},
        {"Open-Meteo (NOAA GFS)", "clouds aloft, open-water weather", SvcState::Ok, "25 model point(s), downloaded 02:20Z", now - 1800},
        {"NOAA MRMS", "radar storms (US)", SvcState::Ok, "CONUS, valid 02:46Z", now - 70},
        {"EUMETNET OPERA", "radar storms (Europe)", SvcState::NotHere, "outside Europe", 0}};
    g_snap = std::make_shared<hz::Snapshot>();
    g_snap->fetched = now - 600;
    const char* ints[] = {"LGT", "", "MOD", "LGT-MOD", "NEG", "SEV", "LGT"};
    for (int k = 0; k < 7; ++k) {
        hz::Pirep p; p.lat = 41.6 + 0.35 * k; p.lon = -72.6 + 0.45 * k; p.altFt = 9000 + 5000 * k; p.intensity = ints[k]; p.urgent = k == 2 || k == 5; p.kind = "PIREP";
        p.tbType = (k == 2 || k == 5) ? "CAT" : ""; p.baseFt = k == 2 ? 19000 : -1; p.topFt = k == 2 ? 23000 : -1; p.acType = k % 2 ? "B738" : "A321"; p.obsTime = now - 700 * (k + 1);
        p.tempC = k == 3 ? -38 : -200; p.windDir = k == 3 ? 255 : -1; p.windKt = k == 3 ? 62 : -1; p.icing = k == 1 ? "LGT RIME" : "";
        p.raw = std::string(p.urgent ? "UUA" : "UA") + " /OV ORW270020/TM 0215/FL" + std::to_string((int)(p.altFt / 100)) + "/TP " + p.acType + (p.intensity.empty() ? "" : "/TB " + std::string(ints[k]));
        g_snap->pireps.push_back(p);
    }
    {
        hz::Advisory c; c.cls = hz::Advisory::Convective; c.hazard = "CONVECTIVE"; c.name = "Convective SIGMET 12E"; c.line = false;
        c.raw = "CONVECTIVE SIGMET 12E\nVALID UNTIL 0455Z\nMA CT NY\nFROM 20W ALB-30N BDL-40W PVD-20W ALB\nAREA EMBD TS MOV FROM 24025KT. TOPS TO FL380.";
        c.from = now - 1200; c.to = now + 6000; c.topFt = 38000; c.movement = "MOV 240 25 KT";
        c.pts = {{42.9, -73.6}, {42.4, -72.0}, {41.5, -72.2}, {41.6, -73.2}, {42.9, -73.6}};
        hz::Advisory t; t.cls = hz::Advisory::Sigmet; t.hazard = "TURB"; t.name = "SIGMET UNIFORM 3"; t.from = now - 3600; t.to = now + 7200; t.baseFt = 24000; t.topFt = 38000;
        t.raw = "SIGMET UNIFORM 3 VALID UNTIL 0600Z\nME NH VT MA\nOCNL SEV TURB BTN FL240 AND FL380."; t.pts = {{44.5, -72.5}, {44.6, -69.0}, {43.2, -68.8}, {42.9, -71.8}, {44.5, -72.5}};
        hz::Advisory i; i.cls = hz::Advisory::Airmet; i.hazard = "ICE"; i.name = "G-AIRMET ZULU ICE"; i.from = now - 600; i.to = now + 10000; i.baseFt = 8000; i.topFt = 18000;
        i.pts = {{43.8, -74.5}, {43.9, -70.5}, {42.6, -70.0}, {41.4, -71.0}, {41.2, -74.2}, {43.8, -74.5}};
        hz::Advisory r; r.cls = hz::Advisory::Airmet; r.hazard = "IFR"; r.name = "G-AIRMET SIERRA IFR"; r.qualifier = "CIG BLW 010/VIS BLW 3SM BR"; r.from = now - 600; r.to = now + 10000; r.baseFt = 0;
        r.pts = {{41.9, -70.9}, {41.9, -69.8}, {41.0, -69.8}, {41.0, -70.9}, {41.9, -70.9}};
        hz::Advisory m; m.cls = hz::Advisory::Airmet; m.hazard = "TURB-HI"; m.name = "G-AIRMET TANGO TURB-HI"; m.from = now - 600; m.to = now + 10000; m.baseFt = 28000; m.topFt = 41000;
        m.pts = {{40.6, -74.8}, {40.8, -71.5}, {40.0, -70.5}, {39.6, -74.6}, {40.6, -74.8}};
        g_snap->advisories = {i, r, m, t, c};
    }
    // clouds as they would be sent to X-Plane: a mid-level deck with gaps, a deep band along the storm line, low stratus south-east
    auto g = std::make_shared<UiCloudGrid>();
    g->nx = g->ny = 81; g->dLat = 0.1; g->dLon = 0.134; g->lat0 = 42.0 - 40 * g->dLat; g->lon0 = -71.6 - 40 * g->dLon;
    g->n.assign((size_t)g->nx * g->ny, 0); g->layers.assign((size_t)g->nx * g->ny * 3, UiCloudLayer{});
    for (int iy = 0; iy < g->ny; ++iy)
        for (int ix = 0; ix < g->nx; ++ix) {
            const double la = g->lat0 + iy * g->dLat, lo = g->lon0 + ix * g->dLon;
            const size_t c = (size_t)iy * g->nx + ix;
            int n = 0;
            auto add = [&](float b, float t, float cv, float ty) { if (n < 3 && cv > 0.02f) g->layers[c * 3 + n++] = UiCloudLayer{b, t, cv, ty}; };
            const double nz = std::sin(la * 7.1) * std::cos(lo * 5.3) + 0.6 * std::sin(la * 13.0 + lo * 9.0);
            const double t = std::max(0.0, std::min(1.0, (la - 41.2) / 1.9));
            const double dl = std::hypot((lo - (-72.9 + 1.35 * t + 0.12 * std::sin(t * 9))) * 0.75, 0.0);
            if (dl < 0.35 && la > 41.0 && la < 43.3) add(2500, (float)(36000 - 70000 * dl), 0.95f, 3);
            if (nz > -0.4) add(15000, (float)(20500 + 2500 * nz), (float)std::min(1.0, 0.55 + 0.3 * nz), 1);
            if (la < 42.0 && lo > -71.2) add(600, 3800, 0.9f, 1);
            if (nz > 0.5) add(30000, 32500, 0.4f, 0);
            g->n[c] = (unsigned char)n;
        }
    g_d.clouds = g;
}

// turbulence at an altitude: calm low down, a patch of clear-air turbulence near the jet (FL280-FL380) north-west, and the storms
double edrAt(double la, double lo, double ft) {
    double e = 0.03 + 0.03 * std::exp(-ft / 4000.0);
    const double jet = std::exp(-((ft - 33000) * (ft - 33000)) / (2 * 4500.0 * 4500.0));
    e += 0.32 * jet * std::exp(-((la - 43.2) * (la - 43.2) + (lo + 72.5) * (lo + 72.5) * 0.4) / 0.5);
    const double t = std::max(0.0, std::min(1.0, (la - 41.2) / 1.9));
    const double dl = std::fabs(lo - (-72.9 + 1.35 * t));
    if (la > 41 && la < 43.3) e += 0.25 * std::exp(-dl * dl / 0.02) * (ft < 38000 ? 1.0 : 0.3);
    return e;
}

void simulate(double dt) {
    if (g_paused) return;
    // the flight: rolls at 30 s, climbs at 2,200 fpm to FL350, speeds up to 450 kt, turns from 220 to 250 after 10,000 ft
    if (g_t > 30) {
        g_gs = std::min(450.0, g_gs + dt * (g_alt < 10000 ? 4.0 : 2.0));
        if (g_gs > 150) g_alt = std::min(35000.0, g_alt + dt * 2200.0 / 60.0);
        if (g_alt > 10000) g_hdg += std::max(-dt * 2, std::min(dt * 2, 250.0 - g_hdg));
        const double nm = g_gs * dt / 3600.0, a = g_hdg * 3.14159265358979 / 180.0;
        g_lat += nm * std::cos(a) / 60.0;
        g_lon += nm * std::sin(a) / (60.0 * std::cos(g_lat * 3.14159265358979 / 180.0));
    }
}

void fillUi(UiData& d) {
    d.res = &g_res; d.haveData = true; d.cfg = &g_cfg;
    d.now = kStart + (std::time_t)g_t;
    d.havePos = true; d.lat = g_lat; d.lon = g_lon; d.altFt = g_alt; d.trackDeg = g_hdg; d.headingDeg = g_hdg; d.gsKt = g_gs;
    d.busy = g_refreshAt > g_t;
    d.nextUpdateS = std::max(0.0, 300.0 - std::fmod(g_t + 108, 300.0));
    d.status1 = "Applied 17 stations, 25 GFS points, 410 storm records"; d.status2 = "emulator: recorded Boston data";
    d.extraServices = {{"NOAA Aviation Weather Center", "pilot reports", SvcState::Ok, "7 report(s), downloaded 02:40Z", kStart - 600},
                       {"NOAA GTGN", "turbulence (US)", SvcState::Ok, "analysis 02:30Z", kStart - 1200},
                       {"ECMWF Open Data", "turbulence (outside the US)", SvcState::NotHere, "GTGN covers this area", 0}};
    d.turbOn = g_cfg.enabled && g_cfg.realisticTurbulence;
    d.turbEdr = edrAt(g_lat, g_lon, g_alt) * g_cfg.maxTurbulence; d.turbSource = "GTGN"; d.turbData = "GTGN 02:30Z";
    d.turbProfile.clear();
    const double alts[] = {1000, 2000, 3000, 4500, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000, 45000};
    for (double a : alts) d.turbProfile.push_back({a, edrAt(g_lat, g_lon, a) * g_cfg.maxTurbulence, true});
    d.reports = g_snap;
    d.wsOn = g_cfg.enabled && g_cfg.windshear; d.wsSources = 2; d.wsLow = 1; d.wsMed = 1; d.wsStrongest = "storm line 38 nm W (gust front, 25 kt)";
    d.haveLocalOffset = true; d.localOffsetS = -5 * 3600;
    // magnetic variation, roughly (east positive): New England, Europe, the far east
    d.magVarEast = -14.5; d.magVarAt = [](double lat, double lon) { return lon < -30 ? -14.5 + (lon + 71.0) * 0.35 : (lon < 60 ? -0.5 + 0.45 * lon : (lat > 0 ? -8.0 : 12.5)); };
    d.xpRealWeather = false;
    d.loadingS = g_loadUntil > g_t ? g_loadUntil - g_t : -1; d.loadingWhat = "Loading weather";
}

void turbGrid(UiData& d, double altFt) {
    if (altFt < 0) return;
    if (std::fabs(altFt - g_lastGridAlt) < 150 && g_t - g_gridAt < 10) return;
    g_lastGridAlt = altFt; g_gridAt = g_t;
    d.turbGrid.clear();
    const double kx = std::cos(g_lat * 3.14159265358979 / 180.0);
    for (int gy = 0; gy < 25; ++gy)
        for (int gx = 0; gx < 25; ++gx) {
            const double la = g_lat + (gy - 12) * 0.2, lo = g_lon + (gx - 12) * 0.2 / kx;
            d.turbGrid.push_back({la, lo, edrAt(la, lo, altFt) * g_cfg.maxTurbulence});
        }
    d.turbGridDLat = 0.2; d.turbGridDLon = 0.2 / kx; d.turbGridAltFt = altFt;
    ++d.turbGridSeq;
}

const char* kVS = R"(#version 300 es
uniform mat4 proj; in vec2 pos; in vec2 uv; in vec4 col; out vec2 vUv; out vec4 vCol;
void main() { vUv = uv; vCol = col; gl_Position = proj * vec4(pos, 0.0, 1.0); })";
const char* kFS = R"(#version 300 es
precision mediump float; uniform sampler2D tex; uniform float alphaTex; in vec2 vUv; in vec4 vCol; out vec4 o;
void main() { vec4 t = texture(tex, vUv); o = alphaTex > 0.5 ? vec4(vCol.rgb, vCol.a * t.r) : vCol * t; })";

GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); std::printf("shader: %s\n", log); }
    return s;
}

void buildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    g_f = uiLoadFonts(g_fbScale);
    unsigned char* px = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
    if (!g_fontTex) glGenTextures(1, &g_fontTex);
    glBindTexture(GL_TEXTURE_2D, g_fontTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, tw, th, 0, GL_RED, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_fontTex);
    if (g_logoTex) { g_f.logo = (void*)(intptr_t)g_logoTex; g_f.logoAspect = (float)g_lw / (float)g_lh; }
}

void render(ImDrawData* dd, int fbW, int fbH) {
    glViewport(0, 0, fbW, fbH);
    glClearColor(0.09f, 0.094f, 0.106f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glEnable(GL_SCISSOR_TEST);
    glUseProgram(g_prog);
    const float L = dd->DisplayPos.x, R = L + dd->DisplaySize.x, T = dd->DisplayPos.y, B = T + dd->DisplaySize.y;
    const float proj[16] = {2 / (R - L), 0, 0, 0, 0, 2 / (T - B), 0, 0, 0, 0, -1, 0, (R + L) / (L - R), (T + B) / (B - T), 0, 1};
    glUniformMatrix4fv(g_locProj, 1, GL_FALSE, proj);
    glUniform1i(g_locTex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g_vao);
    const ImVec2 sc = dd->FramebufferScale;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, cl->VtxBuffer.Size * (int)sizeof(ImDrawVert), cl->VtxBuffer.Data, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, cl->IdxBuffer.Size * (int)sizeof(ImDrawIdx), cl->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) { cmd.UserCallback(cl, &cmd); continue; }
            const float x0 = (cmd.ClipRect.x - L) * sc.x, y0 = (cmd.ClipRect.y - T) * sc.y, x1 = (cmd.ClipRect.z - L) * sc.x, y1 = (cmd.ClipRect.w - T) * sc.y;
            if (x1 <= x0 || y1 <= y0) continue;
            glScissor((int)x0, (int)(fbH - y1), (int)(x1 - x0), (int)(y1 - y0));
            const GLuint tex = (GLuint)(intptr_t)cmd.GetTexID();
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1f(g_locAlpha, tex == g_fontTex ? 1.0f : 0.0f);
            // vertex attributes from this command's vertex offset (16-bit indices with ImGuiBackendFlags_RendererHasVtxOffset)
            const size_t base = cmd.VtxOffset * sizeof(ImDrawVert);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)(base + offsetof(ImDrawVert, pos)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)(base + offsetof(ImDrawVert, uv)));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void*)(base + offsetof(ImDrawVert, col)));
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, (void*)(cmd.IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    glDisable(GL_SCISSOR_TEST);
}

double g_lastTime = 0;
float g_wantScale = 1;

void frame() {
    const double nowMs = emscripten_get_now();
    double dt = g_lastTime > 0 ? (nowMs - g_lastTime) / 1000.0 : 1.0 / 60.0;
    g_lastTime = nowMs;
    dt = std::max(0.0, std::min(0.25, dt));
    if (std::fabs(g_wantScale - g_fbScale) > 0.01f) { g_fbScale = g_wantScale; buildFonts(); }
    g_t += g_paused ? 0 : dt;
    simulate(dt);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(W, H);
    io.DisplayFramebufferScale = ImVec2(g_fbScale, g_fbScale);
    io.DeltaTime = (float)std::max(1e-4, dt);
    fillUi(g_d);
    ImGui::NewFrame();
    UiActions act;
    uiDraw(g_d, g_f, W, H, false, act);
    ImGui::Render();
    render(ImGui::GetDrawData(), (int)(W * g_fbScale), (int)(H * g_fbScale));
    turbGrid(g_d, act.mapAltFt);
    // the buttons
    if (act.refresh) { g_refreshAt = g_t + 3; g_res.fetchedAt = kStart + (std::time_t)g_t; }
    if (act.toggleEnabled) { g_cfg.enabled = !g_cfg.enabled; if (g_cfg.enabled) g_loadUntil = g_t + 40; }
    if (act.toggleAtis) g_cfg.useAtis = !g_cfg.useAtis;
    if (act.toggleAloft) g_cfg.useAloft = !g_cfg.useAloft;
    if (act.toggleTurbulence) g_cfg.realisticTurbulence = !g_cfg.realisticTurbulence;
    if (act.toggleWindshear) g_cfg.windshear = !g_cfg.windshear;
    if (act.maxTurbChanged || act.maxTurbReleased) { g_cfg.maxTurbulence = std::max(0.0, std::min(1.0, act.maxTurb)); g_lastGridAlt = -1e9; }
    if (!act.search.empty()) {
        std::string q = act.search;
        for (auto& c : q) c = (char)std::toupper((unsigned char)c);
        g_d.searchError.clear(); g_d.searchResult.reset();
        struct K { const char* id; double lat, lon, el; const char* raw; const char* atis; };
        static const K known[] = {
            {"EGLL", 51.4706, -0.4619, 25, "EGLL 120250Z AUTO 24012KT 9999 BKN012 OVC025 11/09 Q1009 TEMPO 4000 -RA", ""},
            {"KJFK", 40.6398, -73.7789, 4, "KJFK 120251Z 31011KT 10SM FEW050 SCT250 09/01 A2991 RMK AO2 SLP128", "JFK ATIS INFO K 0251Z. 32011KT 10SM FEW050 SCT250 09/01 A2991. ILS RWY 4R APCH. DEPG RWY 4L, 31L."},
            {"KORD", 41.9786, -87.9048, 205, "KORD 120251Z 28015G24KT 10SM BKN040 04/M04 A2999 RMK AO2", "ORD ATIS INFO M 0251Z. 29015G24KT 10SM BKN040 04/M04 A2999. DEPG RWY 28R, 27L."},
            {"RJTT", 35.5533, 139.7811, 6, "RJTT 120230Z 01008KT 9999 FEW030 17/09 Q1021 NOSIG", ""},
            {"EDDF", 50.0333, 8.5706, 111, "EDDF 120250Z 22008KT 9999 FEW030 BKN045 10/07 Q1014 NOSIG", ""},
            {"KLAX", 33.9425, -118.408, 38, "KLAX 120253Z 25006KT 10SM FEW020 15/11 A2995 RMK AO2", "LAX ATIS INFO R 0253Z. 25006KT 10SM FEW020 15/11 A2995. SIMUL APCHS RWY 24R 25L."},
            {"YSSY", -33.9461, 151.177, 6, "YSSY 120230Z 05014KT CAVOK 25/14 Q1016 NOSIG", ""}};
        const K* hit = nullptr;
        for (const auto& k : known) if (q == k.id) hit = &k;
        for (const auto& s : g_res.stations) if (!hit && q == s.icao) { g_d.searchResult = std::make_shared<Station>(s); }
        if (hit) {
            auto s = std::make_shared<Station>(mk(hit->id, hit->lat, hit->lon, hit->el, hit->raw));
            if (*hit->atis) { s->hasAtis = true; s->atisLetter = std::string(1, hit->atis[std::string(hit->atis).find("INFO ") + 5]); s->atisText = hit->atis; }
            g_d.searchResult = s;
        }
        if (!g_d.searchResult) g_d.searchError = q + ": not in the emulator's sample (try EGLL, KJFK, KORD, RJTT, EDDF, KLAX, YSSY)";
    }
}

int toImKey(int code) {
    switch (code) {
        case 8: return ImGuiKey_Backspace; case 9: return ImGuiKey_Tab; case 13: return ImGuiKey_Enter; case 27: return ImGuiKey_Escape;
        case 35: return ImGuiKey_End; case 36: return ImGuiKey_Home; case 37: return ImGuiKey_LeftArrow; case 39: return ImGuiKey_RightArrow;
        case 46: return ImGuiKey_Delete; case 38: return ImGuiKey_UpArrow; case 40: return ImGuiKey_DownArrow;
        default: return ImGuiKey_None;
    }
}

} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE void emu_mouse(float x, float y, int down) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(x, y); if (down >= 0) io.AddMouseButtonEvent(0, down != 0); }
EMSCRIPTEN_KEEPALIVE void emu_wheel(float dy) { ImGui::GetIO().AddMouseWheelEvent(0, dy); }
EMSCRIPTEN_KEEPALIVE void emu_key(int code, int down) { const int k = toImKey(code); if (k != ImGuiKey_None) ImGui::GetIO().AddKeyEvent((ImGuiKey)k, down != 0); }
EMSCRIPTEN_KEEPALIVE void emu_char(int c) { ImGui::GetIO().AddInputCharacter((unsigned)c); }
EMSCRIPTEN_KEEPALIVE int emu_wants_text() { return ImGui::GetIO().WantTextInput ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void emu_scale(float s) { g_wantScale = std::max(1.0f, std::min(3.0f, s)); }
EMSCRIPTEN_KEEPALIVE void emu_pause(int p) { g_paused = p != 0; }
EMSCRIPTEN_KEEPALIVE void emu_teleport() { g_lat = 42.3606; g_lon = -71.0106; g_alt = 20; g_gs = 0; g_hdg = 220; g_t = std::max(g_t, 31.0); g_loadUntil = g_t + 40; }
EMSCRIPTEN_KEEPALIVE void emu_set_alt(float ft) { g_alt = std::max(0.0f, std::min(45000.0f, ft)); }
EMSCRIPTEN_KEEPALIVE float emu_alt() { return (float)g_alt; }
}

int main() {
    EmscriptenWebGLContextAttributes at;
    emscripten_webgl_init_context_attributes(&at);
    at.majorVersion = 2; at.alpha = 0; at.antialias = 0; at.premultipliedAlpha = 0;
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &at);
    if (ctx <= 0) { std::printf("WebGL2 is not available\n"); return 1; }
    emscripten_webgl_make_context_current(ctx);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, shader(GL_VERTEX_SHADER, kVS));
    glAttachShader(g_prog, shader(GL_FRAGMENT_SHADER, kFS));
    glBindAttribLocation(g_prog, 0, "pos"); glBindAttribLocation(g_prog, 1, "uv"); glBindAttribLocation(g_prog, 2, "col");
    glLinkProgram(g_prog);
    g_locProj = glGetUniformLocation(g_prog, "proj"); g_locTex = glGetUniformLocation(g_prog, "tex"); g_locAlpha = glGetUniformLocation(g_prog, "alphaTex");
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glGenBuffers(1, &g_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigDebugHighlightIdConflicts = false;
    uiStyle();
    if (uiLogoRgba(g_logo, g_lw, g_lh)) {
        glGenTextures(1, &g_logoTex);
        glBindTexture(GL_TEXTURE_2D, g_logoTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_lw, g_lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_logo.data());
    }
    g_fbScale = g_wantScale = (float)EM_ASM_DOUBLE({ return window.emuScale ? window.emuScale() : 1; });
    buildFonts();
    buildData();
    g_cfg.enabled = true;
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
