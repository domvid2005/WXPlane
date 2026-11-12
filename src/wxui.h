// The WXPlane weather window (4.0): tabs for the overview, map, airports, winds and temperatures aloft, turbulence and the data
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
// The clouds WXPlane gave X-Plane, on a regular grid around the aircraft (from the records sent: at each point the record that
// covers it most closely). Up to three layers, feet MSL; the map draws the layer at its chosen altitude.
struct UiCloudLayer { float baseFt = 0, topFt = 0, cover = 0, type = 0; };
struct UiCloudGrid {
    int nx = 0, ny = 0;
    double lat0 = 0, lon0 = 0, dLat = 0, dLon = 0;   // the centre of cell (0, 0) and the spacing (degrees); row-major from the south-west
    std::vector<unsigned char> n;                    // layers per cell (0 = no record there)
    std::vector<UiCloudLayer> layers;                // 3 per cell
    long seq = 0;                                    // changes whenever the grid is rebuilt
};

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
    // magnetic variation, degrees east positive (magnetic = true - variation): at a point (nullptr: use magVarEast everywhere)
    double (*magVarAt)(double lat, double lon) = nullptr;
    double magVarEast = 0;
    // local time where the aircraft is: seconds to add to UTC (X-Plane's time zone there); false: not known (longitude estimate)
    bool haveLocalOffset = false;
    double localOffsetS = 0;
    // update state
    bool busy = false;                  // a download is running
    double nextUpdateS = -1;            // seconds to the next scheduled update, -1 unknown
    std::string status1, status2;       // the menu's status lines
    bool xpRealWeather = false;         // X-Plane's own real weather is on (it should be off)
    std::string turbConflict;           // another add-on that does turbulence (ActiveSky XP, xProTurb ...): WXPlane leaves it to it
    // turbulence at the aircraft
    bool turbOn = false;
    double turbEdr = -1;                // EDR at the aircraft (before Max turbulence), -1 = no data
    std::string turbSource, turbData;   // "GTGN", "ECMWF estimate"...; what data is loaded
    std::vector<UiTurbLayer> turbProfile;
    std::vector<UiTurbCell> turbGrid;    // turbulence around the aircraft at its altitude (radar map layer)
    double turbGridDLat = 0.2, turbGridDLon = 0.27;   // the grid's spacing (degrees), so neighbouring squares share their corners exactly
    double turbGridAltFt = 0;           // the altitude the turbulence grid was sampled at (the map's altitude slider)
    long turbGridSeq = 0;               // changes whenever the turbulence grid is rebuilt (the map cross-fades old and new)
    std::shared_ptr<const UiCloudGrid> clouds;   // clouds around the aircraft (map layer), nullptr: none yet
    // the weather is being (re)built from scratch: start-up, switched back on, a jump to a new place (header countdown)
    double loadingS = -1;               // seconds until it should all be in, -1: not loading
    std::string loadingWhat;            // e.g. "Loading weather", "New location"
    std::shared_ptr<const hz::Snapshot> reports;
    // windshear
    bool wsOn = false;
    int wsSources = 0, wsHigh = 0, wsMed = 0, wsLow = 0;
    std::string wsStrongest;
    // test hooks (tests/ui_shot.cpp); -1 leaves the window's own state alone
    int forceTab = -1;                  // select this tab (0 Overview .. 6 Settings)
    int forceDial = -1;                 // the wind dial / map: 0 North Lock, 1 Airplane Heading Lock
    int forceLayers = -1;               // map layers: 1 map, 2 turbulence, 4 pilot reports
    int forcePirep = -1;                // open this pilot report's info window on the Map tab
    int forceAdv = -1;                  // 1: AIRMET and SIGMET layers on, 0: off
    int forceAloftLayers = -1;          // bit 1 wind barbs, bit 2 clouds, bit 4 time zones (map layers)
    int forceRange = -1;                // the map's range step (0 = 1 nm .. 8 = 320 nm)
    int forceLocalTime = -1;            // the map's time bubble: 1 local time, 0 Zulu
    double forcePanEastNm = 0, forcePanNorthNm = 0;   // with forceRange: move the map off the aircraft by this much
    double forceMapAltFt = -1;          // the map's altitude slider
    int forceMapMode = -1;              // the slider's mode: 0 chosen altitude, 1 ACFT (follows the aircraft), 2 SFC, 3 composite (all altitudes)
    int forceWindSel = -1;              // open this winds-aloft point's decode window on the Map tab
    // the airport search (Airports tab)
    bool searchBusy = false;
    std::string searchError;
    std::shared_ptr<const Station> searchResult;
};

struct UiActions {
    bool refresh = false;
    bool maxTurbChanged = false;        // while dragging
    bool maxTurbReleased = false;       // drag ended: save
    double maxTurb = 1.0;
    bool popOut = false;                // move the window to its own OS window (or back)
    // the settings (the plugin's former menu toggles): flip these
    bool toggleEnabled = false, toggleAtis = false, toggleAloft = false, toggleTurbulence = false, toggleWindshear = false;
    // the one "Realistic turbulence & windshear" switch: set both to this (-1: untouched)
    int setTurbWs = -1;
    std::string search;                 // look up this ICAO code anywhere in the world (METAR + ATIS)
    double mapAltFt = -1;               // the map's altitude slider (turbulence layer), when the turbulence layer is shown; -2: composite (the worst level)
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
// ...and its smaller copies (each half the size of the one before, down to 8 px high; the colour averaged by alpha, so the edges get no
// dark fringe), for the backend to upload as mipmap levels 1, 2, ...: the logo is drawn far smaller than its 570 x 112 pixels, and
// without them the GPU skipped pixels and the edges looked jagged ("logo slightly pixelated", 2026-11-12).
struct UiImage { int w = 0, h = 0; std::vector<unsigned char> rgba; };
std::vector<UiImage> uiLogoMips(const std::vector<unsigned char>& rgba, int w, int h);
// Applies the WXPlane look to the current ImGui context.
void uiStyle();
// Loads the embedded fonts into the current context's atlas at `pixelScale` (pixels per window unit). Call before building the atlas.
UiFonts uiLoadFonts(float pixelScale);
// Draws the whole window into the current ImGui frame, filling `size` (window units) from the top-left corner.
void uiDraw(const UiData& d, const UiFonts& f, float width, float height, bool poppedOut, UiActions& act);

} // namespace wx
