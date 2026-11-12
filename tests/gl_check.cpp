// Checks the weather window's real OpenGL path (src/xpimgui.cpp) outside X-Plane: a stand-in for the XPLM window API, a GLX context on
// an X server (Xvfb + Mesa in a container), X-Plane's 2D projection (window units, origin bottom-left, optionally scaled like X-Plane's
// UI scale), then glReadPixels into a PNG. Linux only, run by hand:
//   Xvfb :99 & DISPLAY=:99 gl_check out.png [ui-scale]
#include <dlfcn.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMUtilities.h"
#include "metar.h"
#include "wxmodel.h"
#include "wxui.h"
#include "xpimgui.h"

namespace {
void* gl(const char* n) { static void* h = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL); return h ? dlsym(h, n) : nullptr; }
template <class F> F G(const char* n) { return (F)gl(n); }
XPLMCreateWindow_t g_p;
int g_l = 100, g_t = 760, g_r = 100 + 1240, g_b = 760 - 820;
const int kScreenW = 1600, kScreenH = 1000;
}

// --- the XPLM functions the window uses ----------------------------------------------------------------------------------------------
XPLMDataRef XPLMFindDataRef(const char*) { return nullptr; }   // no datarefs: the window falls back to glGet for its transforms
int XPLMGetDatai(XPLMDataRef) { return 0; }
int XPLMGetDatavf(XPLMDataRef, float*, int, int) { return 0; }
int XPLMGetDatavi(XPLMDataRef, int*, int, int) { return 0; }
void XPLMDebugString(const char* s) { std::fputs(s, stdout); }
void XPLMSetGraphicsState(int, int tex, int, int, int blend, int, int) {
    auto en = G<void (*)(unsigned)>("glEnable"); auto dis = G<void (*)(unsigned)>("glDisable");
    if (tex) en(0x0DE1); else dis(0x0DE1);
    if (blend) en(0x0BE2); else dis(0x0BE2);
    dis(0x0B71);   // depth test off
}
void XPLMGenerateTextureNumbers(int* out, int n) { auto gen = G<void (*)(int, unsigned*)>("glGenTextures"); std::vector<unsigned> v((size_t)n); gen(n, v.data()); for (int i = 0; i < n; ++i) out[i] = (int)v[(size_t)i]; }
void XPLMBindTexture2d(int t, int) { G<void (*)(unsigned, unsigned)>("glBindTexture")(0x0DE1, (unsigned)t); }
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) { g_p = *p; return (void*)0x1234; }
void XPLMDestroyWindow(XPLMWindowID) {}
void XPLMGetScreenBoundsGlobal(int* l, int* t, int* r, int* b) { *l = 0; *t = kScreenH; *r = kScreenW; *b = 0; }
void XPLMGetWindowGeometry(XPLMWindowID, int* l, int* t, int* r, int* b) { *l = g_l; *t = g_t; *r = g_r; *b = g_b; }
void XPLMSetWindowGeometry(XPLMWindowID, int l, int t, int r, int b) { g_l = l; g_t = t; g_r = r; g_b = b; }
void XPLMSetWindowIsVisible(XPLMWindowID, int) {}
int XPLMGetWindowIsVisible(XPLMWindowID) { return 1; }
void XPLMSetWindowTitle(XPLMWindowID, const char*) {}
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
void XPLMBringWindowToFront(XPLMWindowID) {}
void XPLMSetWindowPositioningMode(XPLMWindowID, XPLMWindowPositioningMode, int) {}
int XPLMWindowIsPoppedOut(XPLMWindowID) { return 0; }

namespace {
wx::FetchResult g_res;
wx::Config g_cfg;
void build(const wx::UiFonts& f, float w, float h, bool popped) {
    wx::UiData d;
    d.res = &g_res; d.haveData = true; d.cfg = &g_cfg; d.now = g_res.fetchedAt + 60;
    d.havePos = true; d.lat = 42.36; d.lon = -71.01; d.trackDeg = 40; d.nextUpdateS = 200;
    wx::UiActions a;
    wx::uiDraw(d, f, w, h, popped, a);
}
void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back((char)((v >> (8 * i)) & 255)); }
void chunk(std::string& png, const char* type, const std::string& data) { be32(png, (uint32_t)data.size()); std::string td = std::string(type, 4) + data; png += td; be32(png, (uint32_t)crc32(0, (const Bytef*)td.data(), (uInt)td.size())); }
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "gl_check.png";
    const float ui = argc > 2 ? (float)std::atof(argv[2]) : 1.0f;
    void* x11 = dlopen("libX11.so.6", RTLD_NOW);
    if (!x11 || !gl("glXChooseVisual")) { std::printf("no X11/GLX\n"); return 2; }
    auto XOpenDisplay = (void* (*)(const char*))dlsym(x11, "XOpenDisplay");
    auto XDefaultScreen = (int (*)(void*))dlsym(x11, "XDefaultScreen");
    auto XRootWindow = (unsigned long (*)(void*, int))dlsym(x11, "XRootWindow");
    auto XCreatePixmap = (unsigned long (*)(void*, unsigned long, unsigned, unsigned, unsigned))dlsym(x11, "XCreatePixmap");
    void* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::printf("no display\n"); return 2; }
    int attr[] = {4 /*GLX_RGBA*/, 8, 8, 9, 8, 10, 8, 11, 8, 0};   // RED/GREEN/BLUE/ALPHA sizes
    auto chooseVisual = G<void* (*)(void*, int, int*)>("glXChooseVisual");
    void* vi = chooseVisual(dpy, XDefaultScreen(dpy), attr);
    if (!vi) { std::printf("no visual\n"); return 2; }
    auto createCtx = G<void* (*)(void*, void*, void*, int)>("glXCreateContext");
    auto glxPixmap = G<unsigned long (*)(void*, void*, unsigned long)>("glXCreateGLXPixmap");
    auto makeCurrent = G<int (*)(void*, unsigned long, void*)>("glXMakeCurrent");
    void* ctx = createCtx(dpy, vi, nullptr, 1);   // direct (Mesa llvmpipe)
    const int depth = 24;
    const unsigned long pm = XCreatePixmap(dpy, XRootWindow(dpy, XDefaultScreen(dpy)), kScreenW, kScreenH, depth);
    const unsigned long gp = glxPixmap(dpy, vi, pm);
    if (!ctx || !makeCurrent(dpy, gp, ctx)) { std::printf("no GL context\n"); return 2; }
    std::printf("GL: %s / %s\n", G<const unsigned char* (*)(unsigned)>("glGetString")(0x1F01), G<const unsigned char* (*)(unsigned)>("glGetString")(0x1F02));
    // X-Plane's 2D drawing set-up: window units, origin bottom-left; at a UI scale above 1 one unit covers several pixels
    G<void (*)(int, int, int, int)>("glViewport")(0, 0, kScreenW, kScreenH);
    G<void (*)(unsigned)>("glMatrixMode")(0x1701);
    G<void (*)()>("glLoadIdentity")();
    G<void (*)(double, double, double, double, double, double)>("glOrtho")(0, kScreenW / ui, 0, kScreenH / ui, -1, 1);
    G<void (*)(unsigned)>("glMatrixMode")(0x1700);
    G<void (*)()>("glLoadIdentity")();
    G<void (*)(float, float, float, float)>("glClearColor")(0.35f, 0.55f, 0.75f, 1);   // "sky"
    // the window at its position in window units
    g_l = (int)(60 / ui); g_r = g_l + 1240; g_t = (int)(kScreenH / ui) - (int)(40 / ui); g_b = g_t - 820;
    if (ui > 1.01f) { g_r = g_l + 760; g_b = g_t - 470; }

    g_res.fetchedAt = 1794451800;
    g_res.centerLat = 42.36; g_res.centerLon = -71.01;
    wx::Station s; s.icao = "KBOS"; s.lat = 42.36; s.lon = -71.01; s.elevM = 6; s.rawMetar = "KBOS 120154Z 03006KT 10SM OVC200 10/03 A2988";
    wx::parseReport(s.rawMetar, s.report);
    wx::Station e = s; e.icao = "KEWB"; e.lat = 41.68; e.lon = -70.96; e.rawMetar = "KEWB 120153Z AUTO 00000KT 4SM -RA BR OVC014 08/06 A2988"; wx::parseReport(e.rawMetar, e.report);
    g_res.stations = {s, e};
    g_res.radar.ok = true; g_res.radar.source = "NOAA MRMS (CONUS)"; g_res.radar.dataTime = g_res.fetchedAt - 200;
    for (int i = 0; i < 12; ++i) { wx::RadarCell c; c.lat = 41.9 + 0.05 * i; c.lon = -71.9 + 0.04 * i; c.dbz = (float)(20 + 3 * i); c.key = std::to_string(i); g_res.radar.cells.push_back(c); }
    g_res.services = {{"NOAA Aviation Weather Center", "METARs", wx::SvcState::Ok, "2 report(s)", g_res.fetchedAt}};

    wx::xpui::open(build);
    for (int frame = 0; frame < 3; ++frame) {
        G<void (*)(unsigned)>("glClear")(0x4000);
        g_p.drawWindowFunc(g_p.refcon ? nullptr : (XPLMWindowID)0x1234, nullptr);
        if (frame == 1) g_p.handleCursorFunc((XPLMWindowID)0x1234, g_l + 1000, g_t - 20, nullptr);   // hover the Refresh button
    }
    G<void (*)()>("glFinish")();
    const unsigned err = G<unsigned (*)()>("glGetError")();
    std::vector<unsigned char> px((size_t)kScreenW * kScreenH * 3);
    G<void (*)(unsigned, int)>("glPixelStorei")(0x0D05, 1);
    G<void (*)(int, int, int, int, unsigned, unsigned, void*)>("glReadPixels")(0, 0, kScreenW, kScreenH, 0x1907, 0x1401, px.data());
    std::string raw;
    for (int y = kScreenH - 1; y >= 0; --y) { raw.push_back(0); raw.append((const char*)&px[(size_t)y * kScreenW * 3], (size_t)kScreenW * 3); }
    uLongf zn = compressBound(raw.size());
    std::string z(zn, '\0');
    compress2((Bytef*)&z[0], &zn, (const Bytef*)raw.data(), raw.size(), 6);
    z.resize(zn);
    std::string png = "\x89PNG\r\n\x1a\n", ihdr;
    be32(ihdr, kScreenW); be32(ihdr, kScreenH); ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr); chunk(png, "IDAT", z); chunk(png, "IEND", "");
    FILE* f = std::fopen(out, "wb");
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    // a few pixels: the window's background where the UI is, the sky outside it
    auto at = [&](int x, int y) { const unsigned char* p = &px[((size_t)(kScreenH - 1 - y) * kScreenW + x) * 3]; return (p[0] << 16) | (p[1] << 8) | p[2]; };
    std::printf("glGetError 0x%x, pixel outside 0x%06x, inside 0x%06x\n", err, at(5, 5), at((int)((g_l + 200) * ui), (int)((kScreenH / ui - g_t + 300) * ui)));
    wx::xpui::destroy();
    std::printf("wrote %s\n", out);
    return err == 0 ? 0 : 1;
}
