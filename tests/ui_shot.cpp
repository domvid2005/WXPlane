// Off-line picture of the WXPlane weather window: draws the real UI (src/wxui.cpp) with Dear ImGui and rasterizes ImGui's draw lists
// in software into PNG files, one per tab, so the layout can be checked without X-Plane or a GPU. Also a smoke test: every tab is
// drawn for a few frames with full data, with no data and with mostly empty data.
//   ui_shot <out-dir> [scale]
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

#ifdef WX_UI_PNG
#include <zlib.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "metar.h"
#include "wxmodel.h"
#include "wxui.h"

using namespace wx;

namespace {

struct Canvas {
    int w, h;
    std::vector<float> rgb;
    Canvas(int W, int H) : w(W), h(H), rgb((size_t)W * H * 3, 0.0f) {}
};

std::vector<unsigned char> g_logo; int g_lw = 0, g_lh = 0;
void raster(Canvas& cv, const ImDrawData* dd, const unsigned char* tex, int tw, int th, float scale) {
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int cx0 = std::max(0, (int)std::floor(cmd.ClipRect.x * scale)), cy0 = std::max(0, (int)std::floor(cmd.ClipRect.y * scale));
            const int cx1 = std::min(cv.w, (int)std::ceil(cmd.ClipRect.z * scale)), cy1 = std::min(cv.h, (int)std::ceil(cmd.ClipRect.w * scale));
            for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + e + k]];
                float X[3], Y[3];
                for (int k = 0; k < 3; ++k) { X[k] = v[k]->pos.x * scale; Y[k] = v[k]->pos.y * scale; }
                const float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
                if (std::fabs(area) < 1e-6f) continue;
                const int x0 = std::max(cx0, (int)std::floor(std::min({X[0], X[1], X[2]}))), x1 = std::min(cx1 - 1, (int)std::ceil(std::max({X[0], X[1], X[2]})));
                const int y0 = std::max(cy0, (int)std::floor(std::min({Y[0], Y[1], Y[2]}))), y1 = std::min(cy1 - 1, (int)std::ceil(std::max({Y[0], Y[1], Y[2]})));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        float b0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
                        float b1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
                        float b2 = 1.0f - b0 - b1;
                        if (b0 < -1e-4f || b1 < -1e-4f || b2 < -1e-4f) continue;
                        float col[4] = {0, 0, 0, 0};
                        for (int k = 0; k < 4; ++k) {
                            const float c0 = ((v[0]->col >> (8 * k)) & 255) / 255.0f, c1 = ((v[1]->col >> (8 * k)) & 255) / 255.0f, c2 = ((v[2]->col >> (8 * k)) & 255) / 255.0f;
                            col[k] = b0 * c0 + b1 * c1 + b2 * c2;
                        }
                        const float u = b0 * v[0]->uv.x + b1 * v[1]->uv.x + b2 * v[2]->uv.x, w = b0 * v[0]->uv.y + b1 * v[1]->uv.y + b2 * v[2]->uv.y;
                        // bilinear sample of the alpha atlas
                        const float fx = u * tw - 0.5f, fy = w * th - 0.5f;
                        const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
                        const float ax = fx - ix, ay = fy - iy;
                        auto T = [&](int a, int b) { a = std::max(0, std::min(tw - 1, a)); b = std::max(0, std::min(th - 1, b)); return tex[(size_t)b * tw + a] / 255.0f; };
                        const float ta = (T(ix, iy) * (1 - ax) + T(ix + 1, iy) * ax) * (1 - ay) + (T(ix, iy + 1) * (1 - ax) + T(ix + 1, iy + 1) * ax) * ay;
                        float a = col[3] * ta;
                        float tc[3] = {1, 1, 1};
                        if ((intptr_t)cmd.GetTexID() == 2 && g_lw > 0) {   // the logo (RGBA)
                            const int lx = std::max(0, std::min(g_lw - 1, (int)(u * g_lw))), ly = std::max(0, std::min(g_lh - 1, (int)(w * g_lh)));
                            const unsigned char* q = &g_logo[((size_t)ly * g_lw + lx) * 4];
                            for (int k = 0; k < 3; ++k) tc[k] = q[k] / 255.0f;
                            a = col[3] * q[3] / 255.0f;
                        }
                        float* o = &cv.rgb[((size_t)y * cv.w + x) * 3];
                        for (int k = 0; k < 3; ++k) o[k] = o[k] * (1 - a) + col[k] * tc[k] * a;
                    }
            }
        }
    }
}

#ifdef WX_UI_PNG
void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back((char)((v >> (8 * i)) & 255)); }
void chunk(std::string& png, const char* type, const std::string& data) {
    be32(png, (uint32_t)data.size());
    std::string td = std::string(type, 4) + data;
    png += td;
    be32(png, (uint32_t)crc32(0, (const Bytef*)td.data(), (uInt)td.size()));
}

bool writePng(const Canvas& cv, const std::string& path) {
    std::string raw;
    raw.reserve((size_t)cv.h * (cv.w * 3 + 1));
    for (int y = 0; y < cv.h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < cv.w; ++x)
            for (int k = 0; k < 3; ++k) raw.push_back((char)(unsigned char)std::lround(std::min(1.0f, std::max(0.0f, cv.rgb[((size_t)y * cv.w + x) * 3 + k])) * 255));
    }
    uLongf zn = compressBound(raw.size());
    std::string z(zn, '\0');
    if (compress2((Bytef*)&z[0], &zn, (const Bytef*)raw.data(), raw.size(), 6) != Z_OK) return false;
    z.resize(zn);
    std::string png = "\x89PNG\r\n\x1a\n", ihdr;
    be32(ihdr, (uint32_t)cv.w); be32(ihdr, (uint32_t)cv.h);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return true;
}
#else
bool writePng(const Canvas&, const std::string&) { return true; }   // no zlib here: the frames are still drawn and rasterized
#endif

Station mk(const char* id, double lat, double lon, double elevM, const char* raw) {
    Station s;
    s.icao = id; s.lat = lat; s.lon = lon; s.elevM = elevM; s.rawMetar = raw;
    parseReport(raw, s.report);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const float scale = argc > 2 ? (float)std::atof(argv[2]) : 1.0f;
    const float W = 1240, H = 820;
    const std::time_t now = 1794451800;   // 2026-11-12 02:50Z

    // Boston, 2026-11-12 (from the user's log), with a made-up line of showers to the west so the radar has something to draw
    FetchResult res;
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
        mk("KCON", 43.2027, -71.5023, 104, "KCON 120151Z AUTO 00000KT 10SM CLR 03/M01 A2990")};
    res.stations[0].hasAtis = true; res.stations[0].atisLetter = "Y";
    res.stations[0].atisText = "BOS ATIS INFO Y 0154Z. 03006KT 10SM OVC200 10/03 A2988. ILS 22L, DEP 22R. RWY 27 IS AP";
    spreadHighCloud(res.stations, 80.0);
    for (auto& s : res.stations) s.distNm = 0;
    // winds aloft from the log (3x3 GFS points, here all the same profile)
    const double prof[][4] = {{2441, 236, 2, 4}, {4705, 285, 13, -1}, {9793, 267, 24, -7}, {18291, 259, 47, -19}, {23614, 258, 55, -31},
                              {30120, 250, 65, -45}, {34027, 255, 66, -52}, {38638, 255, 80, -57}, {44521, 262, 70, -59}, {52767, 260, 44, -60}, {60059, 274, 30, -62}};
    for (int k = 0; k < 9; ++k) {
        AloftStation a;
        a.id = "GFS" + std::to_string(k); a.model = true; a.located = true;
        a.lat = 41 + k / 3; a.lon = -72 + k % 3;
        for (const auto& p : prof) { AloftLevel L; L.altFt = p[0]; L.hasWind = true; L.dirDeg = p[1]; L.speedKt = p[2]; L.hasTemp = true; L.tempC = p[3]; a.levels.push_back(L); }
        res.aloft.push_back(a);
    }
    {   // NOAA FD stations around Boston, each its own winds (they turn and strengthen with height)
        const struct { const char* id; double lat, lon, dirOff, ktMul; } fd[] = {{"BOS", 42.36, -71.0, 0, 1.0}, {"ALB", 42.75, -73.8, -15, 0.9}, {"ACK", 41.25, -70.06, 10, 1.15},
                                                                              {"PWM", 43.65, -70.31, -25, 0.8}, {"EMI", 41.55, -71.6, 5, 1.05}, {"CON", 43.2, -71.5, -10, 0.85}, {"BDL", 41.94, -72.68, -5, 0.95}};
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
    }
    res.radar.ok = true; res.radar.attempted = true; res.radar.source = "NOAA MRMS (CONUS)"; res.radar.dataTime = now - 240; res.radar.topsFromRadar = true;
    for (int i = 0; i < 26; ++i) {
        RadarCell c;
        c.lat = 41.75 + 0.05 * i + 0.04 * std::sin(i * 1.7); c.lon = -72.30 + 0.035 * i + 0.05 * std::cos(i * 2.3);
        c.dbz = (float)(20 + 5 * ((i * 7) % 6)); c.dbzFine = c.dbz; c.rateMmH = (float)std::pow(10.0, (c.dbz - 23) / 16.0);
        c.topM = 3000 + 250 * (i % 9); c.topKnown = true; c.key = "c" + std::to_string(i);
        res.radar.cells.push_back(c);
    }
    res.radar.echoBlocks = (int)res.radar.cells.size();
    res.services = {
        {"NOAA Aviation Weather Center", "METARs", SvcState::Ok, "40 report(s)", now - 70},
        {"D-ATIS (clowd.io)", "ATIS", SvcState::Ok, "1 airport(s)", now - 70},
        {"Open-Meteo (NOAA GFS)", "winds and temperatures aloft", SvcState::Ok, "9 model point(s), downloaded 02:05Z", now - 2700},
        {"Open-Meteo (NOAA GFS)", "clouds aloft, open-water weather", SvcState::Ok, "9 model point(s), downloaded 02:20Z", now - 1800},
        {"NOAA MRMS", "radar storms (US)", SvcState::Ok, "CONUS, valid 02:46Z", now - 70},
        {"EUMETNET OPERA", "radar storms (Europe)", SvcState::NotHere, "outside Europe", 0}};
    Config cfg;

    UiData d;
    d.res = &res; d.haveData = true; d.cfg = &cfg; d.now = now;
    d.havePos = true; d.lat = 42.3606; d.lon = -71.0106; d.altFt = 22; d.trackDeg = 220; d.headingDeg = 220;
    d.nextUpdateS = 192;
    d.extraServices = {{"NOAA Aviation Weather Center", "pilot reports", SvcState::Degraded, "5 report(s), downloaded 02:40Z; failed: PIREPs: HTTP 502;", now - 600},
                       {"NOAA GTGN", "turbulence (US)", SvcState::Ok, "analysis 02:30Z", now - 1200},
                       {"ECMWF Open Data", "turbulence (outside the US)", SvcState::NotHere, "GTGN covers this area", 0}};
    d.turbOn = true; d.turbEdr = 0.055; d.turbSource = "GTGN"; d.turbData = "GTGN 02:30Z";
    const double alts[] = {1000, 2000, 3000, 4500, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000, 45000};
    const double edrs[] = {0.06, 0.07, 0.05, 0.04, 0.03, 0.05, 0.08, 0.10, 0.14, 0.19, 0.12, 0.07, 0.04};
    for (int k = 0; k < 13; ++k) d.turbProfile.push_back({alts[k], edrs[k], true});
    auto snap = std::make_shared<hz::Snapshot>();
    snap->fetched = now - 600;
    const char* ints[] = {"LGT", "", "MOD", "LGT-MOD", "NEG"};
    for (int k = 0; k < 5; ++k) { hz::Pirep p; p.lat = 42 + 0.4 * k; p.lon = -72 + 0.5 * k; p.altFt = 9000 + 6000 * k; p.intensity = ints[k]; p.urgent = k == 2; p.kind = "PIREP"; p.tbType = k == 2 ? "CAT" : ""; p.baseFt = k == 2 ? 19000 : -1; p.topFt = k == 2 ? 23000 : -1; p.acType = k % 2 ? "B738" : "A321"; p.obsTime = now - 900 * (k + 1); p.raw = std::string(k == 2 ? "UUA" : "UA") + " /OV ORW270020/TM 0215/FL" + std::to_string((int)(p.altFt / 100)) + "/TP " + p.acType + "/TB " + std::string(ints[k]); snap->pireps.push_back(p); }
    {   // advisories around Boston: a convective SIGMET line, a turbulence SIGMET, AIRMETs for IFR and icing
        hz::Advisory c; c.cls = hz::Advisory::Convective; c.hazard = "CONVECTIVE"; c.name = "Convective SIGMET 12E"; c.line = false;
        c.raw = "CONVECTIVE SIGMET 12E\nVALID UNTIL 0455Z\nMA CT NY\nFROM 20W ALB-30N BDL-40W PVD-20W ALB\nAREA EMBD TS MOV FROM 24025KT. TOPS TO FL380.";
        c.from = now - 1200; c.to = now + 6000; c.topFt = 38000; c.movement = "MOV 240 25 KT";
        c.pts = {{42.7, -73.9}, {42.3, -72.4}, {41.7, -72.0}, {41.6, -73.2}, {42.7, -73.9}};
        hz::Advisory t; t.cls = hz::Advisory::Sigmet; t.hazard = "TURB"; t.name = "SIGMET UNIFORM 3"; t.from = now - 3600; t.to = now + 7200; t.baseFt = 24000; t.topFt = 38000;
        t.raw = "SIGMET UNIFORM 3 VALID UNTIL 0600Z\nME NH VT MA\nOCNL SEV TURB BTN FL240 AND FL380."; t.pts = {{44.5, -72.5}, {44.6, -69.0}, {43.2, -68.8}, {42.9, -71.8}, {44.5, -72.5}};
        hz::Advisory i; i.cls = hz::Advisory::Airmet; i.hazard = "ICE"; i.name = "G-AIRMET ZULU ICE"; i.from = now - 600; i.to = now + 10000; i.baseFt = 8000; i.topFt = 18000;
        i.pts = {{43.8, -74.5}, {43.9, -70.5}, {42.6, -70.0}, {41.4, -71.0}, {41.2, -74.2}, {43.8, -74.5}};
        hz::Advisory r; r.cls = hz::Advisory::Airmet; r.hazard = "IFR"; r.name = "G-AIRMET SIERRA IFR"; r.qualifier = "CIG BLW 010/VIS BLW 3SM BR"; r.from = now - 600; r.to = now + 10000; r.baseFt = 0;
        r.pts = {{41.9, -70.9}, {41.9, -69.8}, {41.3, -69.8}, {41.3, -70.9}, {41.9, -70.9}};
        snap->advisories = {i, r, t, c};
    }
    d.reports = snap;
    for (int gy = 0; gy < 25; ++gy)
        for (int gx = 0; gx < 25; ++gx) {
            const double la = d.lat + (gy - 12) * 0.2, lo = d.lon + (gx - 12) * 0.27;
            const double e = 0.05 + 0.3 * std::exp(-((la - 43.2) * (la - 43.2) + (lo + 72.5) * (lo + 72.5) * 0.4) / 0.5);
            d.turbGrid.push_back({la, lo, e});
        }
    d.turbGridDLat = 0.2; d.turbGridDLon = 0.27;
    d.haveLocalOffset = true; d.localOffsetS = -5 * 3600;   // Boston in November: EST
    {   // the clouds sent to X-Plane: a mid-level deck with gaps, a deep band along the storm line, low stratus to the south-east
        auto g = std::make_shared<UiCloudGrid>();
        g->nx = g->ny = 61; g->dLat = 0.1; g->dLon = 0.134; g->lat0 = 42.36 - 30 * g->dLat; g->lon0 = -71.0 - 30 * g->dLon;
        g->n.assign((size_t)g->nx * g->ny, 0); g->layers.assign((size_t)g->nx * g->ny * 3, UiCloudLayer{});
        for (int iy = 0; iy < g->ny; ++iy)
            for (int ix = 0; ix < g->nx; ++ix) {
                const double la = g->lat0 + iy * g->dLat, lo = g->lon0 + ix * g->dLon;
                const size_t c = (size_t)iy * g->nx + ix;
                int n = 0;
                auto add = [&](float b, float t, float cv, float ty) { if (n < 3 && cv > 0.02f) g->layers[c * 3 + n++] = UiCloudLayer{b, t, cv, ty}; };
                const double nz = std::sin(la * 7.1) * std::cos(lo * 5.3) + 0.6 * std::sin(la * 13.0 + lo * 9.0);
                const double dl = (la - 41.75) - (lo + 72.30) * (1.3 / 0.9);   // distance from the storm line (rough)
                if (std::fabs(dl) < 0.45 && la > 41.5 && la < 43.3) add(2500, (float)(33000 - 30000 * std::fabs(dl)), 0.95f, 3);
                if (nz > -0.4) add(15000, (float)(20500 + 2500 * nz), (float)std::min(1.0, 0.55 + 0.3 * nz), 1);
                if (la < 42.0 && lo > -71.2) add(800, 3800, 0.9f, 1);
                g->n[c] = (unsigned char)n;
            }
        d.clouds = g;
    }
    d.wsOn = true;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(W, H);
    uiStyle();
    UiFonts f = uiLoadFonts(scale);
    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex, &tw, &th);
    io.Fonts->SetTexID((ImTextureID)1);
    if (uiLogoRgba(g_logo, g_lw, g_lh)) { f.logo = (void*)(intptr_t)2; f.logoAspect = (float)g_lw / (float)g_lh; }

    const char* names[] = {"overview", "airports", "radar", "winds", "turbulence", "services", "settings"};
    int frames = 0;
    for (int tab = 0; tab < 7; ++tab) {
        for (int fr = 0; fr < 3; ++fr) {
            io.DeltaTime = 1.0f / 30.0f;
            io.AddMousePosEvent(-1000, -1000);
            ImGui::NewFrame();
            d.forceTab = fr == 0 ? tab : -1;
            UiActions act;
            uiDraw(d, f, W, H, false, act);
            ImGui::Render();
            ++frames;
        }
        Canvas cv((int)(W * scale), (int)(H * scale));
        raster(cv, ImGui::GetDrawData(), tex, tw, th, scale);
        const std::string p = out + "/ui_" + names[tab] + ".png";
        if (!writePng(cv, p)) { std::printf("cannot write %s\n", p.c_str()); return 1; }
        std::printf("wrote %s\n", p.c_str());
    }
    // the wind dial turned with the aircraft: KBOS 030/06 on a heading of 220 (runway 22) and of 040
    for (double hdg : {220.0, 40.0}) {
        d.headingDeg = hdg;
        for (int fr = 0; fr < 3; ++fr) {
            io.DeltaTime = 1.0f / 30.0f;
            ImGui::NewFrame();
            d.forceTab = fr == 0 ? 0 : -1;
            d.forceDial = 1;
            UiActions act;
            uiDraw(d, f, W, H, false, act);
            ImGui::Render();
            ++frames;
        }
        Canvas cv((int)(W * scale), (int)(H * scale));
        raster(cv, ImGui::GetDrawData(), tex, tw, th, scale);
        const std::string p = out + "/ui_dial_hdg" + std::to_string((int)hdg) + ".png";
        (void)0;
        if (!writePng(cv, p)) return 1;
        std::printf("wrote %s\n", p.c_str());
    }
    // the radar tab heading up with every layer on (the rotation eases in: run frames until it has arrived)
    d.headingDeg = 220; d.forceDial = 1; d.forceLayers = 7; d.forcePirep = 2; d.forceAdv = 1;
    for (int fr = 0; fr < 40; ++fr) {
        io.DeltaTime = 1.0f / 30.0f;
        ImGui::NewFrame();
        d.forceTab = fr == 0 ? 2 : -1;
        UiActions act;
        uiDraw(d, f, W, H, false, act);
        ImGui::Render();
        ++frames;
    }
    {
        Canvas cv((int)(W * scale), (int)(H * scale));
        raster(cv, ImGui::GetDrawData(), tex, tw, th, scale);
        if (!writePng(cv, out + "/ui_radar_layers.png")) return 1;
        std::printf("wrote %s\n", (out + "/ui_radar_layers.png").c_str());
    }
    d.forceDial = 0; d.forceLayers = 1; d.forcePirep = -1; d.forceAdv = 0;
    // winds aloft and clouds at FL180 (the altitude slider), one wind barb decoded; then clouds alone low down
    // (pass 2: the surface - SFC: airport winds and the lowest clouds, one airport's wind decoded)
    d.magVarEast = -14.5;   // Boston: 14.5 degrees west
    for (int pass = 0; pass < 3; ++pass) {
        d.forceLayers = pass == 0 ? 1 : (pass == 1 ? 3 : 1); d.forceAloftLayers = pass == 1 ? 2 : 3; d.forceMapAltFt = pass == 0 ? 18000 : (pass == 1 ? 3000 : -1);
        d.forceMapMode = pass == 2 ? 2 : -1;
        for (int fr = 0; fr < 40; ++fr) {
            d.forceWindSel = (pass == 0 && fr == 0) ? 9 : ((pass == 2 && fr == 20) ? 3 : -1);
            io.DeltaTime = 1.0f / 30.0f;
            ImGui::NewFrame();
            d.forceTab = fr == 0 ? 2 : -1;
            UiActions act;
            uiDraw(d, f, W, H, false, act);
            ImGui::Render();
            ++frames;
        }
        d.forceWindSel = -1;
        Canvas cv((int)(W * scale), (int)(H * scale));
        raster(cv, ImGui::GetDrawData(), tex, tw, th, scale);
        const std::string p = out + (pass == 0 ? "/ui_map_aloft.png" : (pass == 1 ? "/ui_map_clouds_low.png" : "/ui_map_sfc.png"));
        if (!writePng(cv, p)) return 1;
        std::printf("wrote %s\n", p.c_str());
    }
    d.forceAloftLayers = 0; d.forceMapAltFt = -1; d.forceMapMode = 1; d.forceLayers = 1;
    // a full reset: the countdown in the header
    d.loadingS = 83; d.loadingWhat = "Loading weather";
    for (int fr = 0; fr < 3; ++fr) { io.DeltaTime = 1.0f / 30.0f; ImGui::NewFrame(); d.forceTab = fr == 0 ? 0 : -1; UiActions act; uiDraw(d, f, W, H, false, act); ImGui::Render(); ++frames; }
    {
        Canvas cv((int)(W * scale), (int)(H * scale));
        raster(cv, ImGui::GetDrawData(), tex, tw, th, scale);
        if (!writePng(cv, out + "/ui_loading.png")) return 1;
        std::printf("wrote %s\n", (out + "/ui_loading.png").c_str());
    }
    d.loadingS = -1;
    // X-Plane's real weather on: the warning over a locked window; then the weather off: the settings greyed out
    for (int pass = 0; pass < 2; ++pass) {
        d.xpRealWeather = pass == 0;
        Config off = cfg; off.enabled = false;
        if (pass == 1) d.cfg = &off;
        for (int fr = 0; fr < 3; ++fr) {
            io.DeltaTime = 1.0f / 30.0f;
            ImGui::NewFrame();
            d.forceTab = fr == 0 ? (pass == 0 ? 0 : 6) : -1;
            UiActions act;
            uiDraw(d, f, W, H, false, act);
            ImGui::Render();
            ++frames;
        }
        Canvas cv((int)(W * scale), (int)(H * scale));
        raster(cv, ImGui::GetDrawData(), tex, tw, th, scale);
        const std::string p = out + (pass == 0 ? "/ui_realweather.png" : "/ui_settings_off.png");
        if (!writePng(cv, p)) return 1;
        std::printf("wrote %s\n", p.c_str());
        d.cfg = &cfg;
    }
    d.xpRealWeather = false;
    // duplicate ImGui IDs (ImGui outlines them in red when one is hovered): sweep the mouse over every tab with every map layer on
    {
        d.forceLayers = 7; d.forceAdv = 1; d.forceAloftLayers = 3;
        int conflicts = 0;
        for (int tab = 0; tab < 7; ++tab) {
            for (int fr = 0; fr < 2; ++fr) { io.DeltaTime = 1.0f / 30.0f; ImGui::NewFrame(); d.forceTab = fr == 0 ? tab : -1; UiActions act; uiDraw(d, f, W, H, false, act); ImGui::Render(); }
            d.forceTab = -1;
            for (float y = 4; y < H; y += 9)
                for (float x = 4; x < W; x += 23) {
                    io.AddMousePosEvent(x, y);
                    io.DeltaTime = 1.0f / 30.0f;
                    ImGui::NewFrame();
                    UiActions act;
                    uiDraw(d, f, W, H, false, act);
                    ImGui::Render();
                    if (GImGui->DebugDrawIdConflicts != 0 && conflicts < 12) {
                        ++conflicts;
                        std::printf("ID conflict on tab %d at %.0f,%.0f (id 0x%08X, %s)\n", tab, x, y, GImGui->DebugDrawIdConflicts, GImGui->HoveredWindow ? GImGui->HoveredWindow->Name : "?");
                    }
                }
        }
        io.AddMousePosEvent(-1000, -1000);
        d.forceLayers = 1; d.forceAdv = 0; d.forceAloftLayers = 0;
        if (conflicts) { std::printf("UI: %d ID conflict(s)\n", conflicts); return 1; }
    }
    // no data yet, and data with nothing in it
    UiData empty; empty.cfg = &cfg; empty.now = now; empty.status1 = "Starting...";
    FetchResult bare; bare.fetchedAt = now;
    UiData thin; thin.res = &bare; thin.haveData = true; thin.cfg = &cfg; thin.now = now;
    for (UiData* x : {&empty, &thin})
        for (int tab = 0; tab < 7; ++tab)
            for (int fr = 0; fr < 2; ++fr) {
                io.DeltaTime = 1.0f / 30.0f;
                ImGui::NewFrame();
                x->forceTab = fr == 0 ? tab : -1;
                UiActions act;
                uiDraw(*x, f, W, H, false, act);
                ImGui::Render();
                ++frames;
            }
    ImGui::DestroyContext();
    std::printf("UI OK (%d frames)\n", frames);
    return 0;
}
