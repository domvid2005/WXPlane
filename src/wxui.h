// The WXPlane weather window (4.0): tabs for the overview, airports, radar, winds and temperatures aloft, turbulence and the data
// services, drawn with Dear ImGui. Pure ImGui: no X-Plane calls, so it can be drawn and checked off-line (tests/ui_shot.cpp). The
// plugin fills UiData from what it already has every frame the window is open, and acts on UiActions.
#pragma once
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "fetcher.h"
#include "hazards.h"
#include "status.h"

struct ImFont;

namespace wx {

struct UiTurbLayer { double altFt = 0, edr = 0; bool has = false; };   // has: the layer's own data (false: filled from the layers around it)
struct UiTurbCell { double lat = 0, lon = 0, edr = 0; };

struct UiData {
    const FetchResult* res = nullptr;   // the weather in use (nullptr or !haveData: nothing yet)
    bool haveData = false;
    const Config* cfg = nullptr;
    std::vector<ServiceStatus> extraServices;   // the turbulence worker's (pilot reports, GTGN, ECMWF)
    std::time_t now = 0;
    // aircraft
    bool havePos = false;
    double lat = 0, lon = 0, altFt = 0, trackDeg = 0, gsKt = 0;
    double headingDeg = 0;              // true heading (the wind dial turns with it, and the components use it)
    // update state
    bool busy = false;                  // a download is running
    double nextUpdateS = -1;            // seconds to the next scheduled update, -1 unknown
    std::string status1, status2;       // the menu's status lines
    bool xpRealWeather = false;         // X-Plane's own real weather is on (it should be off)
    // turbulence at the aircraft
    bool turbOn = false;
    double turbEdr = -1;                // EDR at the aircraft (before Max turbulence), -1 = no data
    std::string turbSource, turbData;   // "GTGN", "ECMWF estimate"...; what data is loaded
    std::vector<UiTurbLayer> turbProfile;
    std::vector<UiTurbCell> turbGrid;    // turbulence around the aircraft at its altitude (radar map layer)
    double turbGridDLat = 0.2, turbGridDLon = 0.27;   // the grid's spacing (degrees), so neighbouring squares share their corners exactly
    std::shared_ptr<const hz::Snapshot> reports;
    // windshear
    bool wsOn = false;
    int wsSources = 0, wsHigh = 0, wsMed = 0, wsLow = 0;
    std::string wsStrongest;
    int forceTab = -1;
    int forceDial = -1;
    int forceLayers = -1;
    int forcePirep = -1;
    int forceAdv = -1;                  // 1: AIRMET and SIGMET layers on, 0: off (tests)                // open this pilot report's info window on the radar page (tests)               // radar map layers: 1 map, 2 turbulence, 4 pilot reports; -1 leave them (tests)                 // the wind dial: 0 north up, 1 heading up, -1 leave it (tests)                  // select this tab (0 Overview .. 5 Services) this frame, -1 leave it
};

struct UiActions {
    bool refresh = false;
    bool maxTurbChanged = false;        // while dragging
    bool maxTurbReleased = false;       // drag ended: save
    double maxTurb = 1.0;
    bool popOut = false;                // move the window to its own OS window (or back)
    // the settings (the plugin's former menu toggles): flip these
    bool toggleEnabled = false, toggleAtis = false, toggleAloft = false, toggleTurbulence = false, toggleWindshear = false;
};

struct UiFonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* mono = nullptr;
    void* logo = nullptr;               // the logo's texture (ImTextureID), set by the backend; nullptr: the name in text
    float logoAspect = 1;               // width / height
};

// Flight category from a report: "VFR", "MVFR", "IFR", "LIFR", or "" when the report says too little.
std::string flightCategory(const Report& r);

// The logo's pixels (RGBA, from the embedded assets/wxplane_logo.png), for the backend to upload as a texture.
bool uiLogoRgba(std::vector<unsigned char>& rgba, int& w, int& h);
// Applies the WXPlane look to the current ImGui context.
void uiStyle();
// Loads the embedded fonts into the current context's atlas at `pixelScale` (pixels per window unit). Call before building the atlas.
UiFonts uiLoadFonts(float pixelScale);
// Draws the whole window into the current ImGui frame, filling `size` (window units) from the top-left corner.
void uiDraw(const UiData& d, const UiFonts& f, float width, float height, bool poppedOut, UiActions& act);

} // namespace wx
