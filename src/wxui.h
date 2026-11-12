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

struct UiTurbLayer { double altFt = 0, edr = 0; bool has = false; };

struct UiData {
    const FetchResult* res = nullptr;   // the weather in use (nullptr or !haveData: nothing yet)
    bool haveData = false;
    const Config* cfg = nullptr;
    std::vector<ServiceStatus> extraServices;   // the turbulence worker's (pilot reports, GTGN, ECMWF)
    std::time_t now = 0;
    // aircraft
    bool havePos = false;
    double lat = 0, lon = 0, altFt = 0, trackDeg = 0, gsKt = 0;
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
    std::shared_ptr<const hz::Snapshot> reports;
    // windshear
    bool wsOn = false;
    int wsSources = 0, wsHigh = 0, wsMed = 0, wsLow = 0;
    std::string wsStrongest;
    int forceTab = -1;                  // select this tab (0 Overview .. 5 Services) this frame, -1 leave it
};

struct UiActions {
    bool refresh = false;
    bool maxTurbChanged = false;        // while dragging
    bool maxTurbReleased = false;       // drag ended: save
    double maxTurb = 1.0;
    bool popOut = false;                // move the window to its own OS window (or back)
    bool toggleTurbulence = false, toggleWindshear = false;
};

struct UiFonts {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    ImFont* mono = nullptr;
};

// Flight category from a report: "VFR", "MVFR", "IFR", "LIFR", or "" when the report says too little.
std::string flightCategory(const Report& r);

// Applies the WXPlane look to the current ImGui context.
void uiStyle();
// Loads the embedded fonts into the current context's atlas at `pixelScale` (pixels per window unit). Call before building the atlas.
UiFonts uiLoadFonts(float pixelScale);
// Draws the whole window into the current ImGui frame, filling `size` (window units) from the top-left corner.
void uiDraw(const UiData& d, const UiFonts& f, float width, float height, bool poppedOut, UiActions& act);

} // namespace wx
