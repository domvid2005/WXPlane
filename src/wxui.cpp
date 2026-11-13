#include "wxui.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "geo.h"
#include "imgui.h"
#include "imgui_internal.h"   // the map windows: the size an auto-resizing window will have this frame
#include "opera.h"
#include "png.h"
#include "version.h"
#include "radar.h"
#include "suntime.h"
#include "viewer.h"
#include "wxmodel.h"

// fonts embedded at build time (cmake/embed_fonts.cmake)
extern "C" {
extern const unsigned char wx_font_plex_regular[]; extern const unsigned int wx_font_plex_regular_size;
extern const unsigned char wx_font_plex_semibold[]; extern const unsigned int wx_font_plex_semibold_size;
extern const unsigned char wx_font_plex_bold[]; extern const unsigned int wx_font_plex_bold_size;
extern const unsigned char wx_font_mono[]; extern const unsigned int wx_font_mono_size;
extern const unsigned char wx_font_basemap[]; extern const unsigned int wx_font_basemap_size;
extern const unsigned char wx_font_tzones[]; extern const unsigned int wx_font_tzones_size;
extern const unsigned char wx_font_logo[]; extern const unsigned int wx_font_logo_size;
}

float g_testMapRight = 0, g_testAltBoxLeft = 0;
int g_testPopups = 0, g_testPopupTouches = 0;   // for the UI test: the map windows shown, and how many touch each other or a map button
float g_testPopupRect[8][4] = {};               // their rectangles (x0, y0, x1, y1)
float g_testKeepOut[8][4] = {};                 // the map buttons'
int g_testKeepOuts = 0;   // for the UI test: the Map tab's map's right edge and the altitude value box's left edge
namespace wx {
namespace {

const double kPi = 3.14159265358979323846;

// palette
const ImU32 cBg = IM_COL32(23, 24, 27, 255), cPanel = IM_COL32(33, 35, 40, 255), cInset = IM_COL32(24, 25, 29, 255),
            cBorder = IM_COL32(46, 49, 56, 255), cText = IM_COL32(230, 231, 234, 255), cMuted = IM_COL32(154, 160, 168, 255),
            cSoft = IM_COL32(196, 200, 207, 255), cAccent = IM_COL32(76, 154, 255, 255), cAccentDim = IM_COL32(47, 86, 135, 255),
            cOk = IM_COL32(63, 185, 80, 255), cWarn = IM_COL32(242, 180, 90, 255), cDown = IM_COL32(248, 96, 96, 255),
            cIdle = IM_COL32(107, 114, 124, 255), cRing = IM_COL32(42, 47, 54, 255), cMetar = IM_COL32(207, 232, 168, 255);
const ImU32 cLockBlue = IM_COL32(110, 170, 255, 255);   // Heading Lock: the wind wheel's outline, its mode box and the maps' lock bubbles

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// magnetic variation (east positive) at a point, and a true direction as magnetic (1..360, the way pilots say it)
double magVar(const UiData& d, double lat, double lon) { return d.magVarAt ? d.magVarAt(lat, lon) : d.magVarEast; }
int toMagnetic(double trueDeg, double varEast) {
    int m = (int)std::lround(std::fmod(trueDeg - varEast + 720.0, 360.0));
    return m == 0 ? 360 : m;
}
// A small outlined box with text in it, on the current line (the magnetic direction next to a true one)
// clickable: returns true when clicked (the box then swaps which reference is shown)
bool magBox(const char* text, ImU32 col = IM_COL32(196, 200, 207, 255), bool clickable = false) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    bool clicked = false, hov = false;
    if (clickable) {
        ImGui::PushID(text);
        clicked = ImGui::InvisibleButton("##magbox", ImVec2(ts.x + 10, h));
        hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else ImGui::Dummy(ImVec2(ts.x + 10, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, p.y - 1), ImVec2(p.x + ts.x + 10, p.y + h + 1), hov ? IM_COL32(40, 46, 56, 255) : IM_COL32(28, 30, 34, 255), 3.0f);
    dl->AddRect(ImVec2(p.x, p.y - 1), ImVec2(p.x + ts.x + 10, p.y + h + 1), hov ? cAccent : IM_COL32(90, 96, 106, 255), 3.0f, 0, 1.0f);
    dl->AddText(ImVec2(p.x + 5, p.y), col, text);
    return clicked;
}

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

bool tzStdQuarterHours(double lat, double lon, int& q);   // the time zone map (below)
// Local time at a point: X-Plane's time zone near the aircraft (within 250 nm); else the time zone map's standard offset there
// plus daylight time where it is kept (suntime.h); else, off the map, an estimate from the longitude (marked ~)
double localOffsetAt(const UiData& d, double lat, double lon, bool& estimate) {
    estimate = false;
    if (d.haveLocalOffset && (!d.havePos || distNm(d.lat, d.lon, lat, lon) < 250.0)) return d.localOffsetS;
    int q = 0;
    if (tzStdQuarterHours(lat, lon, q)) return q * 900.0 + dstShiftS(lat, lon, q, d.now);
    estimate = true;
    return std::round(lon / 15.0) * 3600.0;
}
std::string utcOffsetText(double offS) {
    const int m = (int)std::lround(std::fabs(offS) / 60.0);
    return m % 60 ? fmt("UTC%s%d:%02d", offS < 0 ? "-" : "+", m / 60, m % 60) : fmt("UTC%s%d", offS < 0 ? "-" : "+", m / 60);
}
// the time zone's name where it has one (EST, CDT, GMT, CEST ...), else the offset (UTC+9)
std::string zoneText(double lat, double lon, double offS, std::time_t now) {
    const std::string n = zoneName(lat, lon, offS, now);
    return n.empty() ? utcOffsetText(offS) : n;
}
std::string clockText(std::time_t t, bool seconds) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return seconds ? fmt("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec) : fmt("%02d:%02d", tm.tm_hour, tm.tm_min);
}
std::string hhmm(std::time_t t) {
    if (t <= 0) return "--:--Z";
    return fmt("%02d:%02dZ", (int)((t / 3600) % 24), (int)((t / 60) % 60));
}

// Column headers for tables that cannot be sorted: plain text, no hover highlight (only sortable headers react to the mouse)
void plainHeaders() {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < ImGui::TableGetColumnCount(); ++c) {   // the label only (an "###id" suffix keeps a column's identity, not shown)
        ImGui::TableSetColumnIndex(c);
        const char* n = ImGui::TableGetColumnName(c);
        const char* id = n ? std::strstr(n, "##") : nullptr;
        ImGui::TextUnformatted(n, id);
    }
}

// Column widths from their widest expected text, so nothing is cut off at any UI scale (plus room for the sort arrow)
float textColW(const char* sample) { return ImGui::CalcTextSize(sample).x + 13.0f; }

bool usUnits(const std::string& icao) { return !icao.empty() && (icao[0] == 'K' || icao[0] == 'P' || icao[0] == 'C' || icao[0] == 'T'); }

ImU32 catColor(const std::string& c) {
    if (c == "VFR") return IM_COL32(111, 220, 140, 255);
    if (c == "MVFR") return IM_COL32(140, 196, 255, 255);
    if (c == "IFR") return IM_COL32(255, 120, 110, 255);
    if (c == "LIFR") return IM_COL32(232, 130, 220, 255);
    return cMuted;
}

ImU32 dbzColor(float dbz, int alpha = 230) {
    if (dbz >= 55) return IM_COL32(200, 70, 200, alpha);
    if (dbz >= 45) return IM_COL32(211, 58, 58, alpha);
    if (dbz >= 35) return IM_COL32(224, 116, 43, alpha);
    if (dbz >= 25) return IM_COL32(201, 181, 42, alpha);
    return IM_COL32(31, 122, 58, alpha);
}

std::string windText(const Report& r) {
    if (!r.hasWind) return "—";
    if (r.windKt < 1) return "Calm";
    std::string s = r.windVariable ? std::string("VRB") : fmt("%03d°T", r.windDir);
    s += fmt(" / %.0f", r.windKt);
    if (r.gustKt > r.windKt) s += fmt(" G%.0f", r.gustKt);
    return s + " kt";
}

std::string windShort(const Report& r) {
    if (!r.hasWind) return "—";
    if (r.windKt < 1) return "calm";
    std::string s = r.windVariable ? std::string("VRB") : fmt("%03d", r.windDir);
    s += fmt("/%.0f", r.windKt);
    if (r.gustKt > r.windKt) s += fmt("G%.0f", r.gustKt);
    return s;
}

// Airport winds in degrees true (as METARs report them) or magnetic (as runways, ATIS and performance figures use them): one switch
// for the whole window, next to the airport lists (windRefSwitch)
// Units (°C / °F, °T / °M) are kept per part of the window: the home page's airport card and nearby list share one set, the
// Airports tab has its own, the winds-aloft tables theirs, and the map (popups, pilot reports) its own. The part being drawn
// sets which (UnitScope); the switches and the clickable boxes change that set only.
struct Units { bool tempF = false, windMag = false; };
// One set of units for every page (2026-11-12: "sync the true / magnetic and temperature toggles on all pages"): the names stay, so
// each page still says which it uses, but they are the same set
Units g_unitsAll;
Units &g_unitsHome = g_unitsAll, &g_unitsAir = g_unitsAll, &g_unitsAloft = g_unitsAll, &g_unitsMap = g_unitsAll;
Units* g_units = &g_unitsAll;
struct UnitScope { Units* prev; explicit UnitScope(Units& u) : prev(g_units) { g_units = &u; } ~UnitScope() { g_units = prev; } };
bool& windMagnetic() { return g_units->windMag; }
// the airport card's and map popups' wind: true or magnetic, swapped by clicking the small box with the other one
bool& cardWindMagnetic() { return g_units->windMag; }
// a report's wind with its direction in the chosen reference: "030°T / 6 kt" or "044°M / 6 kt" (short: "030/6")
int windDirIn(const Report& r, double varEast, bool mag) { return mag ? toMagnetic(r.windDir, varEast) : r.windDir; }
std::string windTextRef(const Report& r, double varEast, bool mag) {
    if (!r.hasWind) return "—";
    if (r.windKt < 1) return "Calm";
    std::string s = r.windVariable ? std::string("VRB") : fmt(mag ? "%03d°M" : "%03d°T", windDirIn(r, varEast, mag));
    s += fmt(" / %.0f", r.windKt);
    if (r.gustKt > r.windKt) s += fmt(" G%.0f", r.gustKt);
    return s + " kt";
}
std::string windShortRef(const Report& r, double varEast, bool mag) {
    if (!r.hasWind || r.windKt < 1 || r.windVariable || !mag) return windShort(r);
    std::string s = fmt("%03d", windDirIn(r, varEast, mag)) + fmt("/%.0f", r.windKt);
    if (r.gustKt > r.windKt) s += fmt("G%.0f", r.gustKt);
    return s;
}
// Temperatures in degrees Celsius or Fahrenheit: one switch for the whole window, next to the wind one
bool& tempF() { return g_units->tempF; }
double tempIn(double c) { return tempF() ? c * 9.0 / 5.0 + 32.0 : c; }
const char* tempUnit() { return tempF() ? "°F" : "°C"; }
std::string tempText(double c) { return fmt("%.0f %s", tempIn(c), tempUnit()); }
long isaDevIn(double devC) { return std::lround(tempF() ? devC * 9.0 / 5.0 : devC); }   // a difference: no offset

// The units switch, right-aligned on the current line: a slightly bigger bubble holding two small labelled switches,
// TEMP (°C / °F) and WIND (°T / °M); the airport lists use it (the home page's and the Airports tab's are the same switch)
bool& dialHeadingUp();
void windRefSwitch(const char* id, float rightGap = 0.0f, bool withLock = false) {
    // one row, the height of the text: WIND °T °M | TEMP °C °F, in one bubble, right-aligned on the current line
    ImGui::PushID(id);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float lblFs = ImGui::GetFontSize() * 0.72f;
    ImFont* font = ImGui::GetFont();
    auto bw = [&](const char* t) { return ImGui::CalcTextSize(t).x + 2 * st.FramePadding.x; };
    const float lt = font->CalcTextSizeA(lblFs, FLT_MAX, 0.0f, "TEMP").x, lw = font->CalcTextSizeA(lblFs, FLT_MAX, 0.0f, "WIND").x;
    const float pad = 6, inner = 4, gapG = 12;
    // withLock: a North Lock / Heading Lock switch first, in the same bubble (the wind dial's switch: the arrows turn with it)
    const float lk = font->CalcTextSizeA(lblFs, FLT_MAX, 0.0f, "LOCK").x;
    const float lockW = withLock ? lk + inner + bw("Heading") + gapG : 0.0f;   // one button, always as wide as "Heading"
    const float boxW = pad + lockW + lt + inner + bw("°C") + 2 + bw("°F") + gapG + lw + inner + bw("°T") + 2 + bw("°M") + pad;
    const float boxH = ImGui::GetTextLineHeight() + 4;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - boxW - rightGap));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + boxW, p0.y + boxH), IM_COL32(30, 32, 37, 255), boxH * 0.5f);
    dl->AddRect(p0, ImVec2(p0.x + boxW, p0.y + boxH), IM_COL32(58, 62, 70, 255), boxH * 0.5f);
    float x = p0.x + pad;
    auto group = [&](const char* label, float labW, const char* a, const char* b, bool second, int which) {
        dl->AddText(font, lblFs, ImVec2(x, p0.y + (boxH - lblFs) * 0.5f), cIdle, label);
        x += labW + inner;
        for (int m = 0; m < 2; ++m) {
            const bool on = second == (m == 1);
            const char* t = m ? b : a;
            ImGui::SetCursorScreenPos(ImVec2(x, p0.y + 2));
            ImGui::PushStyleColor(ImGuiCol_Button, v4(on ? IM_COL32(43, 69, 102, 255) : IM_COL32(40, 43, 49, 255)));
            if (ImGui::SmallButton(t)) { if (which == 0) tempF() = m == 1; else if (which == 1) windMagnetic() = m == 1; else dialHeadingUp() = m == 1; }
            ImGui::PopStyleColor();
            x += bw(t) + 2;
        }
        x += gapG - 2;
    };
    if (withLock) {   // LOCK: one button, the units' style, that reads North or Heading (lit blue in Heading Lock); its size never changes
        dl->AddText(font, lblFs, ImVec2(x, p0.y + (boxH - lblFs) * 0.5f), cIdle, "LOCK");
        x += lk + inner;
        const bool on = dialHeadingUp();
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y + 2));
        ImGui::PushStyleColor(ImGuiCol_Button, v4(on ? IM_COL32(43, 69, 102, 255) : IM_COL32(40, 43, 49, 255)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, 0.0f));
        if (ImGui::Button(on ? "Heading###lock" : "North###lock", ImVec2(bw("Heading"), ImGui::GetTextLineHeight()))) dialHeadingUp() = !on;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        x += bw("Heading") + gapG;
    }
    group("WIND", lw, "°T", "°M", windMagnetic(), 1);
    group("TEMP", lt, "°C", "°F", tempF(), 0);
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(boxW, boxH));
    ImGui::PopID();
}

std::string visText(const Report& r, bool us) {
    if (r.cavok) return "CAVOK";
    if (!r.hasVis) return "—";
    if (us) {
        const double sm = r.visM / 1609.344;
        if (sm >= 9.95) return "10+ SM";
        if (sm < 1) return fmt("%.2g SM", sm);
        return fmt("%.0f SM", sm);
    }
    if (r.visM >= 9999) return "10 km+";
    if (r.visM >= 5000) return fmt("%.0f km", r.visM / 1000.0);
    return fmt("%.0f m", r.visM);
}

std::string ftText(int ft) { return ft >= 10000 ? fmt("%d,%03d", (ft / 100 * 100) / 1000, (ft / 100 * 100) % 1000) : fmt("%d", ft); }

int ceilingFt(const Report& r) {
    int c = -1;
    for (const auto& g : r.clouds)
        if (g.cover >= CloudGroup::BKN && (c < 0 || g.baseFtAgl < c)) c = g.baseFtAgl;
    return c;
}

std::string cloudText(const Report& r) {
    if (r.clouds.empty()) return r.cavok ? "No cloud below 5000" : (r.skyClearAll ? "Sky clear" : (r.clearSky ? "Clear below 12000" : "—"));
    static const char* cov[] = {"FEW", "SCT", "BKN", "OVC", "VV"};
    std::string s;
    for (const auto& g : r.clouds) {
        if (!s.empty()) s += " · ";
        s += std::string(cov[std::max(0, std::min(4, g.cover))]) + " " + ftText(g.baseFtAgl);
        if (g.kind == 1) s += " CB"; else if (g.kind == 2) s += " TCU";
    }
    return s;
}

std::string weatherText(const Report& r) {
    if (r.wxText.empty()) return r.thunder ? "Thunderstorm" : "None";
    struct M { const char* code; const char* word; };
    static const M kM[] = {{"TS", "thunderstorm"}, {"SH", "showers"}, {"FZ", "freezing"}, {"RA", "rain"}, {"SN", "snow"}, {"DZ", "drizzle"},
                           {"GR", "hail"}, {"GS", "small hail"}, {"PL", "ice pellets"}, {"SG", "snow grains"}, {"UP", "precipitation"},
                           {"FG", "fog"}, {"BR", "mist"}, {"HZ", "haze"}, {"FU", "smoke"}, {"DU", "dust"}, {"SA", "sand"}, {"BC", "patches of"},
                           {"MI", "shallow"}, {"BL", "blowing"}, {"DR", "drifting"}, {"PR", "partial"}, {"VA", "volcanic ash"}, {"SQ", "squalls"}};
    std::string out;
    size_t i = 0;
    const std::string& w = r.wxText;
    while (i < w.size()) {
        while (i < w.size() && w[i] == ' ') ++i;
        size_t e = w.find(' ', i);
        if (e == std::string::npos) e = w.size();
        std::string g = w.substr(i, e - i), words;
        i = e;
        if (g.empty()) continue;
        std::string lead;
        if (g[0] == '-') { lead = "light "; g = g.substr(1); }
        else if (g[0] == '+') { lead = "heavy "; g = g.substr(1); }
        bool vc = false;
        if (g.compare(0, 2, "VC") == 0) { vc = true; g = g.substr(2); }
        for (size_t k = 0; k + 1 < g.size() + 1 && k < g.size(); k += 2) {
            const std::string c = g.substr(k, 2);
            const char* wd = nullptr;
            for (const auto& m : kM) if (c == m.code) wd = m.word;
            if (!wd) { words.clear(); break; }
            if (!words.empty()) words += " ";
            words += wd;
        }
        if (words.empty()) words = g;
        if (!out.empty()) out += ", ";
        out += lead + words + (vc ? " nearby" : "");
    }
    if (!out.empty()) out[0] = (char)std::toupper((unsigned char)out[0]);
    return out;
}

void muted(const char* s) { ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted)); ImGui::TextUnformatted(s); ImGui::PopStyleColor(); }
void mutedWrapped(const std::string& s) { ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted)); ImGui::TextWrapped("%s", s.c_str()); ImGui::PopStyleColor(); }

void sectionTitle(const UiFonts& f, const char* s) {
    ImGui::PushFont(f.bold);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cSoft));
    std::string up = s;
    for (auto& ch : up) ch = (char)std::toupper((unsigned char)ch);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// A flight-category badge drawn right-aligned on the current line; the cursor does not move.
void badgeRight(const UiFonts& f, const std::string& cat, float lineY) {
    if (cat.empty()) return;
    ImGui::PushFont(f.bold);
    const ImVec2 ts = ImGui::CalcTextSize(cat.c_str());
    const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x, x0 = x1 - ts.x - 14;
    const ImU32 col = catColor(cat);
    ImVec4 bg = v4(col); bg.w = 0.16f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, lineY), ImVec2(x1, lineY + ts.y + 6), ImGui::ColorConvertFloat4ToU32(bg), 4.0f);
    dl->AddRect(ImVec2(x0, lineY), ImVec2(x1, lineY + ts.y + 6), col, 4.0f);
    dl->AddText(ImVec2(x0 + 7, lineY + 3), col, cat.c_str());
    ImGui::PopFont();
}

void statusDot(SvcState s, float r = 4.5f) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + r + 1, p.y + h * 0.5f);
    if (s == SvcState::Ok) dl->AddCircleFilled(c, r, cOk);
    else if (s == SvcState::Degraded) dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), cWarn, 1.5f);
    else if (s == SvcState::Down) dl->AddTriangleFilled(ImVec2(c.x, c.y - r - 1), ImVec2(c.x + r + 1, c.y + r), ImVec2(c.x - r - 1, c.y + r), cDown);
    else dl->AddCircle(c, r, cIdle, 0, 1.5f);
    ImGui::Dummy(ImVec2(2 * r + 4, h));
}

bool regionalNotHere(const ServiceStatus& s) {
    return s.state == SvcState::NotHere && (s.use.find("(US)") != std::string::npos || s.use.find("(Europe)") != std::string::npos ||
                                            s.use.find("(outside the US)") != std::string::npos);
}

std::vector<ServiceStatus> allServices(const UiData& d) {
    std::vector<ServiceStatus> v;
    if (d.res) for (const auto& s : d.res->services) if (!regionalNotHere(s)) v.push_back(s);
    for (const auto& s : d.extraServices) if (!regionalNotHere(s)) v.push_back(s);
    return v;
}

std::string shortService(const ServiceStatus& s) {
    if (s.use == "METARs") return "METARs";
    if (s.use == "ATIS") return "D-ATIS";
    if (s.use.find("winds") != std::string::npos) return "GFS winds";
    if (s.use.find("clouds") != std::string::npos) return "GFS clouds";
    if (s.name == "NOAA MRMS") return "MRMS radar";
    if (s.name == "EUMETNET OPERA") return "OPERA radar";
    if (s.use == "pilot reports") return "Pilot reports";
    if (s.name == "NOAA GTGN") return "GTGN turbulence";
    if (s.name == "ECMWF Open Data") return "ECMWF turbulence";
    return s.name;
}

// The station nearest the aircraft (the list is sorted at fetch time; the aircraft may have moved since)
const Station* nearestStation(const UiData& d) {
    if (!d.res || d.res->stations.empty()) return nullptr;
    if (!d.havePos) return &d.res->stations[0];
    const Station* best = nullptr; double bd = 1e9;
    for (const auto& s : d.res->stations) {
        const double dd = distNm(d.lat, d.lon, s.lat, s.lon);
        if (dd < bd) { bd = dd; best = &s; }
    }
    return best;
}

double distTo(const UiData& d, const Station& s) { return d.havePos ? distNm(d.lat, d.lon, s.lat, s.lon) : s.distNm; }

// ---- pieces -------------------------------------------------------------------------------------------------------------------

// Wind components on a heading: headwind (+) / tailwind (-) and crosswind (+ from the right), in knots.
void windComponents(double windDir, double windKt, double headingDeg, double& head, double& cross) {
    const double rel = (windDir - headingDeg) * kPi / 180.0;
    head = windKt * std::cos(rel);
    cross = windKt * std::sin(rel);
}

bool& dialHeadingUp() { static bool up = false; return up; }   // false: north up; true: the aircraft's heading up (click the dial)

// The direction at the top of the wind dial and the radar map (degrees true), eased towards its target every frame (uiDraw), so a
// switch between north up and heading up turns both smoothly instead of jumping, and heading changes are followed without jitter.
float& viewRot() { static float r = 0; return r; }
float& headingUpness() { static float u = 0; return u; }   // 0 north up .. 1 heading up (fades the aircraft's nose marker)

void updateViewRot(const UiData& d) {
    const bool up = dialHeadingUp() && d.havePos;
    const float target = up ? (float)d.headingDeg : 0.0f;
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float k = 1.0f - std::exp(-dt * 7.0f);   // about 0.4 s for most of a turn
    float diff = std::fmod(target - viewRot() + 540.0f, 360.0f) - 180.0f;
    viewRot() = std::fabs(diff) < 0.05f ? target : viewRot() + diff * k;
    viewRot() = std::fmod(viewRot() + 360.0f, 360.0f);
    headingUpness() += ((up ? 1.0f : 0.0f) - headingUpness()) * k;
}

// With winds in degrees magnetic and North Lock, the dial and the winds-aloft arrows are drawn with magnetic north up (turned by the
// variation), so an arrow points where its °M number says; eased like the lock turn. Heading Lock is the same either way (relative).
// "Heading Lock 233°M": the aircraft's heading, true or magnetic as the wind switch says
std::string headingText(const UiData& d) {
    const int t = (int)std::lround(std::fmod(d.headingDeg + 360.0, 360.0)), h = windMagnetic() ? toMagnetic(d.headingDeg, magVar(d, d.lat, d.lon)) : (t == 0 ? 360 : t);
    return fmt("Heading Lock %03d°%s", h, windMagnetic() ? "M" : "T");
}
float magTurn(int which, bool on, float varEast) {
    static float cur[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const float target = on ? varEast : 0.0f;
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float k = 1.0f - std::exp(-dt * 7.0f);
    float& c = cur[which & 7];
    c = std::fabs(target - c) < 0.05f ? target : c + (target - c) * k;
    return c;
}

void windDial(const UiData& d, const UiFonts& f, const Report& r, float size) {
    ImGui::BeginGroup();   // the dial and the small mode box under it
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // the whole dial is a button: north up <-> aircraft heading up
    ImGui::InvisibleButton("winddial", ImVec2(size, size));
    if (ImGui::IsItemClicked()) dialHeadingUp() = !dialHeadingUp();
    if (d.forceDial >= 0) dialHeadingUp() = d.forceDial == 1;
    // hovering the dial or the mode box under it flips both outlines (and the box's text), to show they can be clicked
    const float boxH = ImGui::GetFontSize() * 0.8f + 6;
    const ImVec2 boxMin(p.x, p.y + size + ImGui::GetStyle().ItemSpacing.y);
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(boxMin, ImVec2(p.x + size, boxMin.y + boxH));
    const bool up = dialHeadingUp() && d.havePos;
    // screen-up direction, degrees true (animated); magnetic north up with °M in North Lock
    // °M: the compass letters mark magnetic north (in both locks), and North Lock turns the dial so magnetic north is up; the wind
    // (a true bearing) is drawn against the same screen-up, so it points where its °M number says in either lock
    const float magN = magTurn(0, windMagnetic(), (float)magVar(d, d.lat, d.lon));
    const double rot = viewRot() + magN * (1.0f - headingUpness());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float R = size * 0.5f - 2;
    const float hub = 33.0f;   // the middle circle: the same size in both modes, big enough for the components with their gusts
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    dl->AddCircleFilled(c, R, cInset, 64);
    // the outer ring: blue in Heading Lock; hovering flips it (blue in North Lock, grey in Heading Lock) to show it can be clicked
    const bool blueRing = dialHeadingUp() != hovered;
    dl->AddCircle(c, R, blueRing ? cLockBlue : IM_COL32(58, 62, 70, 255), 64, blueRing ? 2.0f : 1.5f);
    const float mid = (R + hub) * 0.5f;   // the middle ring halfway between the outer ring and the centre circle: two equal bands
    dl->AddCircle(c, mid, cRing, 64, 1.0f);
    auto onDial = [&](double trueDeg, float rad) {
        const double a = (trueDeg - rot) * kPi / 180.0;
        return ImVec2(c.x + (float)std::sin(a) * rad, c.y - (float)std::cos(a) * rad);
    };
    const char* nesw[] = {"N", "E", "S", "W"};
    for (int k = 0; k < 4; ++k) {
        const ImVec2 ts = ImGui::CalcTextSize(nesw[k]);
        const ImVec2 q = onDial(k * 90.0 + magN, (R + mid) * 0.5f);   // in the outer band (magnetic north with °M)
        dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), k == 0 ? cSoft : cMuted, nesw[k]);
    }
    if (headingUpness() > 0.02f) {   // the aircraft's nose at the top of the dial (fades in and out with the mode)
        const int al = (int)(230 * headingUpness());
        dl->AddTriangleFilled(ImVec2(c.x, p.y + 3), ImVec2(c.x + 5, p.y + 11), ImVec2(c.x - 5, p.y + 11), IM_COL32(255, 255, 255, al));
    }
    const bool steady = r.hasWind && r.windKt >= 1 && !r.windVariable;
    if (steady) {
        // the arrow comes from the direction the wind blows from, pointing to the centre
        const double a = (r.windDir - rot) * kPi / 180.0;
        const ImVec2 dir((float)std::sin(a), -(float)std::cos(a));
        const ImVec2 n(-dir.y, dir.x);
        // the wind triangle in the inner band: its base on the middle ring, its tip at the centre circle, pointing in
        const float rb = mid - 1.0f, rt = hub + 1.5f, hw = std::min(7.0f, (rb - rt) * 0.6f);
        const ImVec2 tip(c.x + dir.x * rt, c.y + dir.y * rt), base(c.x + dir.x * rb, c.y + dir.y * rb);
        dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * hw, base.y + n.y * hw), ImVec2(base.x - n.x * hw, base.y - n.y * hw), cAccent);
    }
    dl->AddCircleFilled(c, hub, cPanel, 48);
    dl->AddCircle(c, hub, IM_COL32(58, 62, 70, 255), 48);
    auto centred = [&](ImFont* font, float px, const std::string& t, float y, ImU32 col) {
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, t.c_str());
        dl->AddText(font, px, ImVec2(c.x - ts.x * 0.5f, y - ts.y * 0.5f), col, t.c_str());
    };
    const float fs = ImGui::GetFontSize();
    // two parts on one centred line: the value, then its gust in blue
    const ImU32 cGust = IM_COL32(110, 170, 255, 255);
    auto centred2 = [&](ImFont* font, float px, const std::string& a, ImU32 ca, const std::string& b, ImU32 cb, float y) {
        const ImVec2 sa = font->CalcTextSizeA(px, FLT_MAX, 0.0f, a.c_str()), sb = b.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(px, FLT_MAX, 0.0f, b.c_str());
        const float gapX = b.empty() ? 0.0f : px * 0.25f, x0 = c.x - (sa.x + gapX + sb.x) * 0.5f;
        dl->AddText(font, px, ImVec2(x0, y - sa.y * 0.5f), ca, a.c_str());
        if (!b.empty()) dl->AddText(font, px, ImVec2(x0 + sa.x + gapX, y - sb.y * 0.5f), cb, b.c_str());
    };
    const bool gusty = steady && r.gustKt > r.windKt + 0.5;
    if (up && r.hasWind) {
        // the components on the aircraft's heading, each with its gust component (G) in blue
        double head = 0, cross = 0, gHead = 0, gCross = 0;
        if (steady) windComponents(r.windDir, r.windKt, d.headingDeg, head, cross);
        if (gusty) windComponents(r.windDir, r.gustKt, d.headingDeg, gHead, gCross);
        const std::string hw = !steady ? std::string("VRB") : (std::fabs(head) < 0.5 ? std::string("H 0") : fmt("%s %.0f", head >= 0 ? "H" : "T", std::fabs(head)));
        const std::string xw = !steady ? fmt("%.0f kt", r.windKt) : (std::fabs(cross) < 0.5 ? std::string("X 0") : fmt("X %.0f%s", std::fabs(cross), cross > 0 ? "R" : "L"));
        const std::string hg = gusty && std::fabs(gHead) >= 0.5 ? fmt("G%.0f", std::fabs(gHead)) : std::string();
        const std::string xg = gusty && std::fabs(gCross) >= 0.5 ? fmt("G%.0f", std::fabs(gCross)) : std::string();
        centred2(f.bold, fs, hw, steady && head < -0.5 ? cWarn : IM_COL32(255, 255, 255, 255), hg, cGust, c.y - fs * 0.55f);
        centred2(f.bold, fs, xw, steady && std::fabs(gusty ? gCross : cross) >= 15 ? cWarn : cSoft, xg, cGust, c.y + fs * 0.55f);
    } else {
        const std::string kt = !r.hasWind ? std::string("—") : (r.windKt < 1 ? std::string("0") : fmt("%.0f", r.windKt));
        centred(f.title, ImGui::GetFontSize() * 20.0f / 14.5f, kt, c.y - fs * (gusty ? 0.35f : 0.15f), IM_COL32(255, 255, 255, 255));
        if (gusty) centred2(f.bold, fs * 0.85f, fmt("G%.0f", r.gustKt), cGust, "kt", cMuted, c.y + fs * 0.75f);   // the gust in blue
        else centred(f.body, fs * 0.75f, "kt", c.y + fs * 0.75f, cMuted);
    }
    {   // which way the dial (and the map) is turned: a small box under the dial, its text changing with a click on the dial
        // in Heading Lock with the aircraft's heading, in the chosen reference (°T / °M)
        const std::string modeS = dialHeadingUp() && d.havePos ? headingText(d) : std::string(dialHeadingUp() ? "Heading Lock" : "North Lock");
        const char* mode = modeS.c_str();
        const float sfs = fs * 0.8f;
        const ImVec2 ts = f.body->CalcTextSizeA(sfs, FLT_MAX, 0.0f, mode);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float bw = ts.x + 14, bh = ts.y + 6, x0 = p.x + (size - bw) * 0.5f;
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        dl2->AddRectFilled(ImVec2(x0, q.y), ImVec2(x0 + bw, q.y + bh), cInset, 4.0f);
        // the same rule as the dial's outline: blue in Heading Lock, flipped while hovered
        dl2->AddRect(ImVec2(x0, q.y), ImVec2(x0 + bw, q.y + bh), blueRing ? cLockBlue : IM_COL32(58, 62, 70, 255), 4.0f);
        dl2->AddText(f.body, sfs, ImVec2(x0 + 7, q.y + 3), blueRing ? cLockBlue : cSoft, mode);
        ImGui::InvisibleButton("dialmode", ImVec2(size, bh));
        if (ImGui::IsItemClicked()) dialHeadingUp() = !dialHeadingUp();
    }
    ImGui::EndGroup();
}

void field(const UiFonts& f, const char* label, const std::string& value) {
    ImGui::BeginGroup();
    muted(label);
    ImGui::PushFont(f.bold);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    ImGui::EndGroup();
}

void stationCard(const UiData& d, const UiFonts& f, const Station& st, bool full) {
    const Report& r = st.report;
    const bool us = usUnits(st.icao);
    const float lineY = ImGui::GetCursorScreenPos().y;
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(st.icao.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, 10);
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, lineY + 5));
    muted(fmt("%.0f nm · field %.0f ft", distTo(d, st), st.elevM / 0.3048).c_str());
    badgeRight(f, flightCategory(r), lineY + 1);
    // the report, labelled with its source and which north its winds use. On the home page, with an ATIS there, the label is a
    // bubble: a click switches the box between the METAR and the ATIS (the bubble keeps its size, only its text changes)
    static bool homeShowAtis = false;
    const bool atisHere = !full && st.hasAtis && !st.atisText.empty();
    const bool showAtis = atisHere && homeShowAtis;
    const std::string atisName = "ATIS " + (st.atisLetter.empty() ? std::string("?") : st.atisLetter);
    if (atisHere) {
        ImGui::PushFont(f.bold);
        const float pw = std::max(ImGui::CalcTextSize("METAR").x, ImGui::CalcTextSize(atisName.c_str()).x) + 16, ph = ImGui::GetTextLineHeight() + 4;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const bool click = ImGui::InvisibleButton("srcbubble", ImVec2(pw, ph));
        const bool hov = ImGui::IsItemHovered();
        if (click) homeShowAtis = !homeShowAtis;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), hov ? IM_COL32(43, 69, 102, 255) : IM_COL32(36, 40, 47, 255), ph * 0.5f);
        dl->AddRect(p0, ImVec2(p0.x + pw, p0.y + ph), showAtis ? IM_COL32(120, 180, 255, 255) : IM_COL32(90, 96, 106, 255), ph * 0.5f, 0, 1.0f);
        const char* lbl = showAtis ? atisName.c_str() : "METAR";
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(p0.x + (pw - ts.x) * 0.5f, p0.y + (ph - ts.y) * 0.5f), showAtis ? IM_COL32(150, 195, 255, 255) : cText, lbl);
        ImGui::PopFont();
        ImGui::SameLine(0, 8);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p0.y + 2));
        muted(showAtis ? "winds magnetic" : "winds true");
    } else {
        ImGui::PushFont(f.bold); ImGui::TextUnformatted("METAR"); ImGui::PopFont();
        ImGui::SameLine(0, 8); muted("winds true");
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cInset));
    ImGui::PushFont(f.mono);
    const float w = ImGui::GetContentRegionAvail().x;
    const std::string raw = st.rawMetar.empty() ? std::string("(no report text)") : st.rawMetar;
    const std::string shown = showAtis ? st.atisText : raw;
    // the box keeps the METAR's height when an ATIS is there to switch to (at least two lines), scrolling a longer ATIS inside it
    const float lh = ImGui::GetTextLineHeight();
    float th = ImGui::CalcTextSize(raw.c_str(), nullptr, false, w - 20).y;
    if (atisHere) th = std::max(th, 2 * lh);
    const float sh = ImGui::CalcTextSize(shown.c_str(), nullptr, false, w - 20).y;
    ImGui::BeginChild("raw", ImVec2(w, th + 16), ImGuiChildFlags_Borders, sh > th + 1 ? 0 : ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(showAtis ? IM_COL32(255, 217, 128, 255) : cMetar));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    // dial + decoded values
    windDial(d, f, r, 124);
    ImGui::SameLine(0, 14);
    ImGui::BeginGroup();
    if (ImGui::BeginTable("dec", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::BeginGroup();
        muted("Wind");
        const double var = magVar(d, st.lat, st.lon);
        const bool mag = cardWindMagnetic();
        if (r.hasWind && r.windKt >= 1 && !r.windVariable) {   // the other reference in the small box: a click swaps them
            ImGui::SameLine(0, 8);
            if (magBox(fmt(mag ? "%03d°T" : "%03d°M", windDirIn(r, var, !mag)).c_str(), IM_COL32(196, 200, 207, 255), true)) cardWindMagnetic() = !mag;
        }
        ImGui::PushFont(f.bold);
        ImGui::TextUnformatted(windTextRef(r, var, mag).c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::TableNextColumn(); field(f, "Visibility", visText(r, us));
        ImGui::TableNextColumn(); field(f, "Weather", weatherText(r));
        ImGui::TableNextColumn(); field(f, us ? "Altimeter" : "QNH", !r.hasAltim ? std::string("—") : (us ? fmt("%.2f inHg", r.altimHpa / 33.8639) : fmt("%.0f hPa", r.altimHpa)));
        ImGui::TableNextColumn();
        {   // temperature and dew point; the small box with the other unit switches °C / °F (for the whole window), like the wind's
            ImGui::BeginGroup();
            muted("Temp / Dew");
            if (r.hasTemp) {
                ImGui::SameLine(0, 8);
                if (magBox(tempF() ? "°C" : "°F", IM_COL32(196, 200, 207, 255), true)) tempF() = !tempF();
            }
            ImGui::PushFont(f.bold);
            ImGui::TextUnformatted((r.hasTemp ? (r.hasDewp ? fmt("%.0f / %.0f %s", tempIn(r.tempC), tempIn(r.dewpC), tempUnit()) : tempText(r.tempC)) : std::string("—")).c_str());
            ImGui::PopFont();
            ImGui::EndGroup();
        }
        const int ceil = ceilingFt(r);
        ImGui::TableNextColumn(); field(f, "Ceiling", ceil < 0 ? std::string("None") : ftText(ceil) + " ft");
        {   // local time at the airport
            bool est = false;
            const double off = localOffsetAt(d, st.lat, st.lon, est);
            const std::string zn = zoneText(st.lat, st.lon, off, d.now);
            if (full) {   // the airport page: local time and Zulu (the home page has them in the box below the card)
                ImGui::TableNextColumn(); field(f, "Local time", (est ? "~" : "") + clockText(d.now + (std::time_t)off, false) + " " + zn + (zn.rfind("UTC", 0) == 0 ? std::string() : "  (" + utcOffsetText(off) + ")"));
                ImGui::TableNextColumn(); field(f, "Zulu", clockText(d.now, false) + "Z  UTC");
            }
            {   // sunrise and sunset today (local; with Zulu on the airport page)
                std::time_t rise = 0, set = 0;
                const int sun = sunTimes(st.lat, st.lon, d.now + (std::time_t)off, rise, set);
                auto loc = [&](std::time_t t) { return clockText(t + (std::time_t)off, false) + " " + zn + (full ? "  (" + clockText(t, false) + "Z)" : std::string()); };
                ImGui::TableNextColumn(); field(f, "Sunrise", sun > 0 ? loc(rise) : (sun == 0 ? std::string("sun up all day") : std::string("no sunrise today")));
                ImGui::TableNextColumn(); field(f, "Sunset", sun > 0 ? loc(set) : (sun == 0 ? std::string("no sunset today") : std::string("sun down all day")));
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    muted("Clouds"); ImGui::SameLine(); ImGui::TextUnformatted(cloudText(r).c_str());
    if (!st.filled.empty()) mutedWrapped("Completed: " + st.filled);
    if (full && st.hasAtis) {
        ImGui::Spacing();
        ImGui::PushFont(f.bold); ImGui::TextUnformatted(("ATIS " + st.atisLetter).c_str()); ImGui::PopFont();
        ImGui::SameLine(0, 8); muted("winds magnetic");
        ImGui::PushFont(f.mono);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(IM_COL32(255, 217, 128, 255)));
        ImGui::TextWrapped("%s", st.atisText.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
}

// The radar picture: rings, storm cells, airports by flight category, the aircraft in the middle (north up).
// The map under the radar: Natural Earth 1:50m coastlines, land borders, state/province lines and lakes (public domain), embedded at
// build time (tools/make_basemap.py, cmake/embed_fonts.cmake), decoded once.
struct MapLine { int kind; float lo0, la0, lo1, la1; std::vector<ImVec2> pts; };   // pts: (lon, lat)
const std::vector<MapLine>& basemap() {
    static std::vector<MapLine> lines;
    static bool done = false;
    if (done) return lines;
    done = true;
    const unsigned char* b = wx_font_basemap;
    const size_t n = wx_font_basemap_size;
    if (n < 12 || std::memcmp(b, "WXBM", 4) != 0) return lines;
    size_t at = 12;
    auto varint = [&](uint64_t& v) { v = 0; int sh = 0; while (at < n) { const unsigned char c = b[at++]; v |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) return true; sh += 7; if (sh > 63) return false; } return false; };
    const uint32_t count = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
    lines.reserve(count);
    int64_t x = 0, y = 0;
    for (uint32_t i = 0; i < count && at < n; ++i) {
        MapLine L;
        L.kind = b[at++];
        uint64_t np = 0;
        if (!varint(np) || np > 1000000) break;
        L.pts.reserve((size_t)np);
        x = 0; y = 0;
        L.lo0 = L.la0 = 1e9f; L.lo1 = L.la1 = -1e9f;
        for (uint64_t k = 0; k < np; ++k) {
            uint64_t ux = 0, uy = 0;
            if (!varint(ux) || !varint(uy)) break;
            x += (int64_t)(ux >> 1) ^ -(int64_t)(ux & 1);
            y += (int64_t)(uy >> 1) ^ -(int64_t)(uy & 1);
            const ImVec2 q((float)(x / 2000.0), (float)(y / 2000.0));
            L.pts.push_back(q);
            L.lo0 = std::min(L.lo0, q.x); L.lo1 = std::max(L.lo1, q.x); L.la0 = std::min(L.la0, q.y); L.la1 = std::max(L.la1, q.y);
        }
        lines.push_back(std::move(L));
    }
    return lines;
}

// The time zones (Natural Earth 1:10m, public domain; tools/make_tzones.py): the boundary lines for the map's "Time Zone" layer and
// each zone's polygons with its standard UTC offset, for the local time at a point. Decoded once.
struct TzZone { int q; float lo0, la0, lo1, la1; std::vector<std::vector<ImVec2>> rings; };   // q: quarter hours; points (lon, lat)
struct TzData { std::vector<MapLine> lines; std::vector<TzZone> zones; };
const TzData& tzData() {
    static TzData t;
    static bool done = false;
    if (done) return t;
    done = true;
    const unsigned char* b = wx_font_tzones;
    const size_t n = wx_font_tzones_size;
    if (n < 12 || std::memcmp(b, "WXTZ", 4) != 0) return t;
    size_t at = 8;
    auto u32 = [&](uint32_t& v) { if (at + 4 > n) return false; v = (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24); at += 4; return true; };
    auto varint = [&](uint64_t& v) { v = 0; int sh = 0; while (at < n) { const unsigned char c = b[at++]; v |= (uint64_t)(c & 0x7F) << sh; if (!(c & 0x80)) return true; sh += 7; if (sh > 63) return false; } return false; };
    auto points = [&](std::vector<ImVec2>& out, float& lo0, float& la0, float& lo1, float& la1) {
        uint64_t np = 0;
        if (!varint(np) || np > 1000000) return false;
        out.reserve((size_t)np);
        int64_t x = 0, y = 0;
        for (uint64_t k = 0; k < np; ++k) {
            uint64_t ux = 0, uy = 0;
            if (!varint(ux) || !varint(uy)) return false;
            x += (int64_t)(ux >> 1) ^ -(int64_t)(ux & 1);
            y += (int64_t)(uy >> 1) ^ -(int64_t)(uy & 1);
            const ImVec2 q((float)(x / 2000.0), (float)(y / 2000.0));
            out.push_back(q);
            lo0 = std::min(lo0, q.x); lo1 = std::max(lo1, q.x); la0 = std::min(la0, q.y); la1 = std::max(la1, q.y);
        }
        return true;
    };
    uint32_t nl = 0;
    if (!u32(nl)) return t;
    t.lines.reserve(nl);
    for (uint32_t i = 0; i < nl; ++i) {
        MapLine L; L.kind = 4; L.lo0 = L.la0 = 1e9f; L.lo1 = L.la1 = -1e9f;
        if (!points(L.pts, L.lo0, L.la0, L.lo1, L.la1)) { t.lines.clear(); return t; }
        t.lines.push_back(std::move(L));
    }
    uint32_t nz = 0;
    if (!u32(nz)) return t;
    for (uint32_t i = 0; i < nz && at < n; ++i) {
        TzZone z; z.q = (int)(signed char)b[at++]; z.lo0 = z.la0 = 1e9f; z.lo1 = z.la1 = -1e9f;
        uint64_t nr = 0;
        if (!varint(nr) || nr > 100000) break;
        z.rings.resize((size_t)nr);
        bool ok = true;
        for (auto& r : z.rings) if (!(ok = points(r, z.lo0, z.la0, z.lo1, z.la1))) break;
        if (!ok) break;
        t.zones.push_back(std::move(z));
    }
    return t;
}
// The standard UTC offset (quarter hours) of the time zone at a point; false off the map (the data has none there)
bool tzStdQuarterHours(double lat, double lon, int& q) {
    static double lastLat = 1e9, lastLon = 1e9; static int lastQ = 0; static bool lastOk = false;
    if (std::fabs(lat - lastLat) < 0.002 && std::fabs(lon - lastLon) < 0.002) { q = lastQ; return lastOk; }   // the same point again
    lastLat = lat; lastLon = lon; lastOk = false;
    if (lon > 180) lon -= 360; else if (lon < -180) lon += 360;
    const float x = (float)lon, y = (float)lat;
    for (const TzZone& z : tzData().zones) {
        if (x < z.lo0 || x > z.lo1 || y < z.la0 || y > z.la1) continue;
        bool in = false;
        for (const auto& r : z.rings)
            for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
                if ((r[i].y > y) != (r[j].y > y) && x < (r[j].x - r[i].x) * (y - r[i].y) / (r[j].y - r[i].y) + r[i].x) in = !in;
        if (in) { lastQ = z.q; lastOk = true; break; }
    }
    q = lastQ;
    return lastOk;
}
bool& showTimeZones() { static bool v = false; return v; }   // the map's "Time Zone" layer

bool& showMap() { static bool v = true; return v; }
bool& showTurb() { static bool v = false; return v; }
bool& showPireps() { static bool v = false; return v; }

ImU32 edrColor(double edr, int alpha) {
    if (edr >= 0.35) return IM_COL32(220, 60, 60, alpha);
    if (edr >= 0.22) return IM_COL32(235, 130, 50, alpha);
    if (edr >= 0.15) return IM_COL32(230, 200, 70, alpha);
    return IM_COL32(120, 190, 120, alpha);
}

// The radar picture: map, turbulence, storm cells, pilot reports, airports by flight category, the aircraft in the middle.
// North up, or turned with the aircraft's heading together with the wind dial (viewRot).
const double kRanges[] = {1, 2, 5, 10, 20, 40, 80, 160, 320};   // nm, the mouse wheel steps through these
const int kRangeCount = 9;
int& radarRange() { static int r = 6; return r; }   // index into kRanges (80 nm)
double rangeNmOf(int k) { return kRanges[std::max(0, std::min(kRangeCount - 1, k))]; }

// the pilot report open in the radar page's info window (identified by its text and time, the list is rebuilt every update)
// What is open in the radar page's info window: a pilot report, an airport or an advisory, identified by a key that survives the
// lists being rebuilt at every update (text + time, the ICAO code, name + start time)
// Up to two of them can be open at once (the oldest closes when a third is clicked), and they never overlap each other.
struct SelItem { int kind = 0; std::string key; ImVec2 at; ImVec2 lastSize = ImVec2(300, 200); ImVec2 pos, size; bool shown = false;
                 bool dragged = false, dragging = false;
                 bool fixed = false; int frames = 0, stable = 0; ImVec2 measSize = ImVec2(-1, -1); };   // fixed: placed once (after it was measured), it stays on that spot   // dragged: moved by hand, it stays where it was put (inside the map, off the other window)   // kind: 0 none, 1 pilot report, 2 airport, 3 AIRMET/SIGMET, 4 winds aloft
struct MapSel { std::vector<SelItem> items; ImVec2 mapMin, mapMax; std::vector<std::pair<ImVec2, ImVec2>> keepOut; };   // keepOut: the map's own buttons (no window may touch them)
MapSel& mapSel() { static MapSel s; return s; }
SelItem* selFind(int kind, const std::string& key) { for (auto& it : mapSel().items) if (it.kind == kind && it.key == key) return &it; return nullptr; }
bool isSel(int kind, const std::string& key) { return selFind(kind, key) != nullptr; }
void selAt(int kind, const std::string& key, ImVec2 q) { if (SelItem* it = selFind(kind, key)) it->at = q; }
void selOpen(int kind, const std::string& key, ImVec2 at) {
    if (SelItem* it = selFind(kind, key)) { it->at = at; return; }
    auto& v = mapSel().items;
    // as many as fit: when a new one finds no room beside the others, the oldest closes (infoWindow)
    SelItem it; it.kind = kind; it.key = key; it.at = at;
    v.push_back(it);
}
std::string pirepKey(const hz::Pirep& p) { return p.raw + "|" + std::to_string((long long)p.obsTime); }
// mode 0: every column, a click opens the details under the row; 1: compact (no aircraft column, short ages), same clicks;
// 2: compact, and a click glides the Map tab's map to the report and opens its window there
void pirepTable(const UiData& d, const UiFonts& f, const char* id, ImVec2 size, std::vector<std::string>& open, int mode = 0);
std::string pirepConditions(const hz::Pirep& p);
const UiData* s_drawData = nullptr;   // the data of the frame being drawn (a pilot report's wind in °M needs the variation there)
// a pilot report's wind in the units chosen (°T or °M, the variation at the report), "255°M/82 kt"
std::string pirepWind(const hz::Pirep& p, const char* sep = "/") {
    const int t = (int)std::lround(std::max(0.0, p.windDir)) % 360;
    const bool mag = windMagnetic() && s_drawData;
    const int dir = mag ? toMagnetic(t, magVar(*s_drawData, p.lat, p.lon)) : t;
    return fmt(mag ? "%03d°M%s%.0f kt" : "%03d°T%s%.0f kt", dir, sep, p.windKt);
}
// the list's real header height and row step (measured while drawing it; the caller sizes it to whole rows from the last frame's)
struct RowMetrics { float head = 0, step = 0; int shownFrom = 0, shownTo = 0; };
RowMetrics& pirepMetrics(const std::string& id) { static std::map<std::string, RowMetrics> m; return m[id]; }
bool pirepListed(const hz::Pirep& p);   // a report the lists show (it says something)
std::string advKey(const hz::Advisory& a) { return a.name + "|" + std::to_string((long long)a.from) + "|" + std::to_string(a.pts.size()); }

bool& showAirports() { static bool v = true; return v; }
bool& showRadar() { static bool v = true; return v; }   // the radar echoes (storm cells)
// The Map tab's view can be dragged away from the aircraft: the offset of its centre, nm east and north (the Overview's map always
// stays on the aircraft)
ImVec2& mapPan() { static ImVec2 v(0, 0); return v; }
bool& recentering() { static bool v = false; return v; }   // "Center on aircraft": the map glides back (updateShownRange)
// a click on a storm cell in the list: the map glides until its centre (the +) is over that cell (nm east, north of the aircraft)
bool& panGliding() { static bool v = false; return v; }
std::string& pendingPirep() { static std::string k; return k; }   // a report picked in the Map tab's list: its window opens when the glide ends
ImVec2& panGoal() { static ImVec2 v(0, 0); return v; }
bool& mapLocalTime() { static bool v = false; return v; }   // the map's time bubble: local time instead of Zulu
double& popupFullUntil() { static double t = -1; return t; }   // "MAX AMOUNT OF POPUPS" shows until then (ImGui time)
// The range shown, eased towards the chosen one (in log space) like the North/Heading Lock turn
float& shownRange() { static float r = 80; return r; }
void updateShownRange() {
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float target = (float)rangeNmOf(radarRange());
    const float k = 1.0f - std::exp(-dt * 9.0f);
    const float a = std::log(shownRange()), b = std::log(target);
    shownRange() = std::fabs(a - b) < 0.002f ? target : std::exp(a + (b - a) * k);
    if (panGliding()) {
        if (recentering()) panGliding() = false;
        else {
            const float kp = 1.0f - std::exp(-dt * 7.0f);
            mapPan().x += (panGoal().x - mapPan().x) * kp; mapPan().y += (panGoal().y - mapPan().y) * kp;
            if (std::fabs(panGoal().x - mapPan().x) < 0.005f * target && std::fabs(panGoal().y - mapPan().y) < 0.005f * target) { mapPan() = panGoal(); panGliding() = false; }
        }
    }
    if (recentering()) {
        const float kp = 1.0f - std::exp(-dt * 7.0f);
        mapPan().x -= mapPan().x * kp; mapPan().y -= mapPan().y * kp;
        if (std::fabs(mapPan().x) < 0.01f * target && std::fabs(mapPan().y) < 0.01f * target) { mapPan() = ImVec2(0, 0); recentering() = false; }
    }
}
bool& showAirmets() { static bool v = false; return v; }
bool& showSigmets() { static bool v = false; return v; }   // SIGMETs and convective SIGMETs

// advisory colours by category
ImU32 advColor(const hz::Advisory& a, int alpha) {
    const std::string& h = a.hazard;
    if (a.cls == hz::Advisory::Convective) return IM_COL32(240, 60, 60, alpha);                         // convective: red
    if (a.cls == hz::Advisory::Sigmet) {
        if (h.find("TURB") != std::string::npos || h == "MTW") return IM_COL32(250, 140, 40, alpha);    // turbulence, mountain waves
        if (h.find("ICE") != std::string::npos) return IM_COL32(70, 170, 255, alpha);                   // icing
        if (h == "VA") return IM_COL32(190, 150, 110, alpha);                                           // volcanic ash
        if (h == "TC") return IM_COL32(230, 90, 220, alpha);                                            // tropical cyclone
        if (h == "DS" || h == "SS") return IM_COL32(220, 190, 110, alpha);                              // dust, sand
        return IM_COL32(250, 110, 90, alpha);
    }
    if (h == "IFR") return IM_COL32(160, 120, 255, alpha);                                              // AIRMET Sierra: IFR
    if (h == "MT_OBSC") return IM_COL32(225, 120, 215, alpha);                                          // mountain obscuration
    if (h.find("TURB") != std::string::npos) return IM_COL32(235, 185, 70, alpha);                      // Tango: turbulence
    if (h == "LLWS") return IM_COL32(255, 120, 170, alpha);
    if (h == "SFC_WND") return IM_COL32(150, 205, 100, alpha);
    if (h.find("ICE") != std::string::npos) return IM_COL32(90, 200, 240, alpha);                       // Zulu: icing
    return IM_COL32(170, 175, 185, alpha);
}

bool advCurrent(const hz::Advisory& a, std::time_t now) { return !(a.to > 0 && now > a.to) && !(a.from > now + 1800); }

bool pointInPoly(const std::vector<ImVec2>& v, ImVec2 q) {
    bool in = false;
    for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++)
        if (((v[i].y > q.y) != (v[j].y > q.y)) && (q.x < (v[j].x - v[i].x) * (q.y - v[i].y) / (v[j].y - v[i].y) + v[i].x)) in = !in;
    return in;
}
float distToSeg(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = std::max(0.0f, std::min(1.0f, t));
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}


// ---- winds aloft, clouds and the map's altitude ----------------------------------------------------------------------------------
bool& showWinds() { static bool v = false; return v; }    // wind barbs at the map's altitude
bool& showClouds() { static bool v = false; return v; }   // the clouds at the map's altitude
bool altLayersOn() { return showTurb() || showWinds() || showClouds(); }

// The map's altitude (feet MSL), for the turbulence, cloud and wind layers. Three modes: ACFT follows the aircraft as it climbs and
// descends (the default), SFC is the surface (the slider at the bottom), or the altitude chosen with the slider. Leaving ACFT or SFC
// (clicking it again) goes back to the altitude chosen before. What the layers use (shown) glides to the target, so they change
// smoothly while the slider moves.
const float kMapAltMax = 60000.0f;
enum class AltMode { Chosen = 0, Acft = 1, Sfc = 2, Comp = 3 };   // Comp: every altitude at once (composite)
struct MapAlt { float shown = -1, target = -1, chosen = -1; AltMode mode = AltMode::Acft; };
MapAlt& mapAlt() { static MapAlt a; return a; }
bool atSurface() { return mapAlt().mode == AltMode::Sfc && mapAlt().shown < 50.0f; }   // the surface layers (METAR winds, lowest clouds)
// the airports' reported winds instead of winds aloft: at SFC, and whenever the map's altitude is below 1,000 ft (an aircraft on
// the ground with ACFT on: winds aloft start a few thousand feet up)
bool composite() { return mapAlt().mode == AltMode::Comp; }   // all altitudes combined: the most turbulent level, every cloud layer, the strongest wind
bool surfaceWinds() { return !composite() && (atSurface() || mapAlt().shown < 1000.0f); }
void updateMapAlt(const UiData& d) {
    MapAlt& a = mapAlt();
    const float acft = d.havePos ? (float)std::max(0.0, std::min((double)kMapAltMax, d.altFt)) : 0.0f;
    if (a.chosen < 0) a.chosen = acft;
    if (d.forceMapMode >= 0) a.mode = (AltMode)d.forceMapMode;
    if (d.forceMapAltFt >= 0) { if (d.forceMapMode < 0) a.mode = AltMode::Chosen; a.chosen = (float)d.forceMapAltFt; }
    a.target = a.mode == AltMode::Acft ? acft : (a.mode == AltMode::Sfc ? 0.0f : (a.mode == AltMode::Comp ? kMapAltMax : a.chosen));   // composite: the knob goes to the top, the whole track lit
    if (a.shown < 0 || d.forceMapAltFt >= 0) a.shown = a.target;
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float k = 1.0f - std::exp(-dt * 9.0f);
    a.shown = std::fabs(a.target - a.shown) < 1.0f ? a.target : a.shown + (a.target - a.shown) * k;
}
std::string altLabel(float ft) { return ft >= 18000 ? fmt("FL%03.0f", std::round(ft / 100.0f)) : fmt("%.0f ft", std::round(ft / 100.0f) * 100.0f); }

// A slider whose knob glides (instead of jumping) to where it is clicked or dragged: `target` is what the user chose, `shown` what is
// drawn and used, eased towards it every frame. Returns true while the user holds it; `released` is set when they let go.
// The altitude slider's pull: whole thousands of feet catch the knob within 300 ft, every 5,000 ft within 700 ft, and every 10,000 ft
// within 1,200 ft, so the round levels are easy to land on
float altSnap(float v) {
    const struct { float step, radius; } tiers[] = {{10000, 1200}, {5000, 700}, {1000, 300}};
    for (const auto& t : tiers) {
        const float r = std::round(v / t.step) * t.step;
        if (std::fabs(v - r) <= t.radius) return r;
    }
    return v;
}

// snapStep / snapRadius: values within snapRadius of a multiple of snapStep are pulled onto it (the knob "clicks" into whole thousands
// of feet, or tenths), and the mouse wheel steps from one multiple to the next. snapFn, when given, decides the pull instead.
float s_lastValueBoxLeft = 0;   // the last vertical slider's value box, left edge (for the UI test)
bool smoothSlider(const char* id, float& shown, float& target, float vmin, float vmax, ImVec2 size, bool vertical, const std::string& label, bool& released,
                  bool disabled = false, float snapStep = 0.0f, float snapRadius = 0.0f, float (*snapFn)(float) = nullptr, ImU32 knobColour = 0) {
    released = false;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool active = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    const float knobR = 7.0f;
    auto toFrac = [&](float v) { return std::max(0.0f, std::min(1.0f, (v - vmin) / (vmax - vmin))); };
    if (active) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float fr = vertical ? 1.0f - (m.y - p.y - knobR) / std::max(1.0f, size.y - 2 * knobR) : (m.x - p.x - knobR) / std::max(1.0f, size.x - 2 * knobR);
        target = vmin + std::max(0.0f, std::min(1.0f, fr)) * (vmax - vmin);
        if (snapFn) target = std::max(vmin, std::min(vmax, snapFn(target)));
        else if (snapStep > 0) {
            const float snapTo = std::round(target / snapStep) * snapStep;
            if (std::fabs(target - snapTo) <= snapRadius) target = std::max(vmin, std::min(vmax, snapTo));
        }
    }
    if (ImGui::IsItemDeactivated()) released = true;
    if (hov && !active && ImGui::GetIO().MouseWheel != 0.0f) {   // the wheel nudges it by 2 % of the range per notch
        if (snapStep > 0) {   // from one multiple to the next
            const float w = ImGui::GetIO().MouseWheel;
            const float base = w > 0 ? std::floor(target / snapStep + 1e-3f) : std::ceil(target / snapStep - 1e-3f);
            target = std::max(vmin, std::min(vmax, (base + (w > 0 ? 1.0f : -1.0f)) * snapStep));
        } else target = std::max(vmin, std::min(vmax, target + ImGui::GetIO().MouseWheel * 0.02f * (vmax - vmin)));
        released = true;
    }
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float k = 1.0f - std::exp(-dt * 14.0f);
    if (shown < vmin - 1e-6f || shown > vmax + 1e-6f) shown = target;
    shown = std::fabs(target - shown) < (vmax - vmin) * 1e-4f ? target : shown + (target - shown) * k;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 trackC = cInset, fillC = disabled ? IM_COL32(70, 74, 82, 255) : cAccentDim, knobC = disabled ? IM_COL32(88, 92, 100, 255) : (active || hov ? IM_COL32(120, 180, 255, 255) : cAccent);
    const float fr = toFrac(shown);
    ImVec2 kc;
    if (vertical) {
        const float x = p.x + size.x * 0.5f, y0 = p.y + knobR, y1 = p.y + size.y - knobR;
        dl->AddRectFilled(ImVec2(x - 3, y0), ImVec2(x + 3, y1), trackC, 3);
        kc = ImVec2(x, y1 - (y1 - y0) * fr);
        dl->AddRectFilled(ImVec2(x - 3, kc.y), ImVec2(x + 3, y1), fillC, 3);
    } else {
        const float y = p.y + size.y * 0.5f, x0 = p.x + knobR, x1 = p.x + size.x - knobR;
        dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x1, y + 3), trackC, 3);
        kc = ImVec2(x0 + (x1 - x0) * fr, y);
        dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(kc.x, y + 3), fillC, 3);
    }
    dl->AddCircleFilled(kc, knobR, knobColour && !disabled ? knobColour : knobC, 20);
    dl->AddCircle(kc, knobR, IM_COL32(15, 17, 20, 255), 20, 1.5f);
    if (!label.empty()) {   // the value next to the knob (vertical: in a small box to its left; horizontal: centred in the track)
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        if (vertical) {
            const ImVec2 b0(kc.x - knobR - 6 - ts.x - 8, kc.y - ts.y * 0.5f - 3), b1(kc.x - knobR - 6, kc.y + ts.y * 0.5f + 3);
            dl->AddRectFilled(b0, b1, IM_COL32(28, 30, 34, 235), 4);
            s_lastValueBoxLeft = b0.x;
            dl->AddText(ImVec2(b0.x + 4, b0.y + 3), disabled ? cIdle : cText, label.c_str());
        } else {
            dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f - ts.y * 0.9f), disabled ? cIdle : cSoft, label.c_str());
        }
    }
    return active;
}

// Winds and temperatures aloft at the map's altitude, one per FD station / GFS model point, eased towards the latest values so
// the barbs turn and grow smoothly when an update comes in (or the altitude changes). Keyed so a point keeps its identity across
// updates: the FD id, or the model point's position.
// rangeA: how much the point has data at the map's altitude (fading over the last 1,500 ft beyond its top or bottom level);
// show: what is drawn, eased towards rangeA (and towards 0 where the map is too crowded), so barbs come and go smoothly.
struct WindPt { std::string key, id; bool model = false, surface = false; double lat = 0, lon = 0; float u = 0, v = 0, tempC = 0, gustKt = 0; bool hasT = false; float rangeA = 0, show = 0; bool seen = false;
                float levelFt = -1;                        // composite: the level of the strongest wind
                std::vector<AloftLevel> levels; };         // composite: the point's whole profile (its decode window)
std::vector<WindPt>& windPts() { static std::vector<WindPt> v; return v; }
bool windAtLevel(const AloftStation& s, float ft, float& dir, float& kt, float& tempC, bool& hasT, float* edge = nullptr) {
    const AloftLevel *lo = nullptr, *hi = nullptr;
    for (const auto& L : s.levels) {
        if (!L.hasWind) continue;
        if (L.altFt <= ft) lo = &L;
        if (L.altFt >= ft && !hi) hi = &L;
    }
    if (!lo && !hi) return false;
    float over = 0;
    if (!lo) { over = (float)(hi->altFt - ft); if (over > 1500) return false; lo = hi; }
    if (!hi) { over = (float)(ft - lo->altFt); if (over > 1500) return false; hi = lo; }
    if (edge) *edge = 1.0f - over / 1500.0f;
    const double t = hi->altFt > lo->altFt ? (ft - lo->altFt) / (hi->altFt - lo->altFt) : 0.0;
    const double r = kPi / 180.0;
    const double ux = (1 - t) * lo->speedKt * std::sin(lo->dirDeg * r) + t * hi->speedKt * std::sin(hi->dirDeg * r);
    const double uy = (1 - t) * lo->speedKt * std::cos(lo->dirDeg * r) + t * hi->speedKt * std::cos(hi->dirDeg * r);
    kt = (float)std::sqrt(ux * ux + uy * uy);
    dir = kt > 0.05f ? (float)std::fmod(std::atan2(ux, uy) / r + 360.0, 360.0) : 0.0f;
    hasT = lo->hasTemp && hi->hasTemp;
    tempC = hasT ? (float)((1 - t) * lo->tempC + t * hi->tempC) : 0.0f;
    return true;
}
void updateWinds(const UiData& d) {
    std::vector<WindPt>& pts = windPts();
    for (auto& w : pts) { w.seen = false; w.rangeA = 0; }
    const float ft = mapAlt().shown;
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float r = (float)(kPi / 180.0);
    const float kE = 1.0f - std::exp(-dt * 3.0f);   // about a second to settle on new data
    auto find = [&](const std::string& key) -> WindPt* { for (auto& q : pts) if (q.key == key) return &q; return nullptr; };
    if (d.res && showWinds() && surfaceWinds())   // the surface: the airports' reported winds (METAR, ATIS merged), true
        for (const auto& st : d.res->stations) {
            if (st.icao.compare(0, 3, "GFS") == 0 || !st.report.hasWind) continue;
            const Report& rp = st.report;
            const float kt = rp.windKt < 1 ? 0.0f : (float)rp.windKt, dir = rp.windVariable ? 0.0f : (float)rp.windDir;
            const float tu = rp.windVariable ? 0.0f : kt * std::sin(dir * r), tv = rp.windVariable ? 0.0f : kt * std::cos(dir * r);
            const std::string key = "s" + st.icao;
            WindPt* w = find(key);
            if (!w) { pts.push_back(WindPt{}); w = &pts.back(); w->key = key; w->u = tu; w->v = tv; w->tempC = (float)rp.tempC; }
            w->id = st.icao; w->model = false; w->surface = true; w->lat = st.lat; w->lon = st.lon; w->seen = true;
            w->hasT = rp.hasTemp; w->gustKt = rp.gustKt > rp.windKt ? (float)rp.gustKt : 0.0f; w->rangeA = 1;
            w->u += (tu - w->u) * kE; w->v += (tv - w->v) * kE; w->tempC += ((float)rp.tempC - w->tempC) * kE;
        }
    if (d.res && showWinds() && !surfaceWinds())
        for (const auto& s : d.res->aloft) {
            if (!s.located) continue;
            float dir = 0, kt = 0, tc = 0, edge = 0; bool hasT = false;
            bool have = false;
            float levelFt = -1;
            if (composite()) {   // the strongest wind anywhere in the column
                for (const auto& L : s.levels)
                    if (L.hasWind && (!have || L.speedKt > kt)) { have = true; kt = (float)L.speedKt; dir = (float)L.dirDeg; hasT = L.hasTemp; tc = (float)L.tempC; levelFt = (float)L.altFt; }
                edge = 1;
            } else have = windAtLevel(s, ft, dir, kt, tc, hasT, &edge);
            const std::string key = s.model ? fmt("m%.2f,%.2f", s.lat, s.lon) : s.id;
            WindPt* w = find(key);
            if (!have && !w) continue;
            // u, v: where the wind comes from, as a vector (the barb's staff), so the easing turns it the short way round
            const float tu = kt * std::sin(dir * r), tv = kt * std::cos(dir * r);
            if (!w) { pts.push_back(WindPt{}); w = &pts.back(); w->key = key; w->u = tu; w->v = tv; w->tempC = tc; }
            w->id = s.id; w->model = s.model; w->lat = s.lat; w->lon = s.lon; w->seen = true;
            if (!have) continue;   // out of its levels' reach: it keeps its last wind while it fades out
            w->hasT = hasT;
            w->levelFt = levelFt;
            if (composite()) w->levels = s.levels; else w->levels.clear();
            w->rangeA = std::max(0.0f, std::min(1.0f, edge));
            w->u += (tu - w->u) * kE; w->v += (tv - w->v) * kE; w->tempC += (tc - w->tempC) * kE;
        }
    // gone from the data (or the layer is off): kept until faded out
    pts.erase(std::remove_if(pts.begin(), pts.end(), [](const WindPt& w) { return w.rangeA <= 0 && w.show < 0.01f; }), pts.end());
}
void windOf(const WindPt& w, float& dir, float& kt) {
    kt = std::sqrt(w.u * w.u + w.v * w.v);
    dir = kt > 0.05f ? (float)std::fmod(std::atan2(w.u, w.v) * 180.0 / kPi + 360.0, 360.0) : 0.0f;
}
// barbs, rounded to 5 kt: pennants (50), full barbs (10), a half barb (5)
void barbCounts(float kt, int& pennants, int& full, int& half) {
    int k5 = (int)std::lround(kt / 5.0f);
    pennants = k5 / 10; k5 -= pennants * 10;
    full = k5 / 2; half = k5 - full * 2;
}
// A wind barb at q: the staff points to where the wind comes from (true `dirDeg`, turned with the map), the feathers on its end.
void drawBarb(ImDrawList* dl, ImVec2 q, float dirDeg, float kt, float viewDeg, ImU32 col, float scale) {
    const double a = (dirDeg - viewDeg) * kPi / 180.0;
    const ImVec2 s((float)std::sin(a), -(float)std::cos(a)), n(-s.y, s.x);
    const float L = 26 * scale, fl = 11 * scale;
    dl->AddCircleFilled(q, 2.6f * scale, col, 10);
    if (kt < 2.5f) { dl->AddCircle(q, 5.5f * scale, col, 16, 1.4f); return; }   // calm
    const ImVec2 tail(q.x + s.x * L, q.y + s.y * L);
    dl->AddLine(q, tail, col, 1.5f);
    int pn = 0, fu = 0, ha = 0;
    barbCounts(kt, pn, fu, ha);
    auto at = [&](float t) { return ImVec2(tail.x - s.x * t, tail.y - s.y * t); };
    float t = 0;
    for (int i = 0; i < pn; ++i) { const ImVec2 b = at(t), e = at(t + 6 * scale); dl->AddTriangleFilled(b, ImVec2(b.x + n.x * fl, b.y + n.y * fl), e, col); t += 8 * scale; }
    for (int i = 0; i < fu; ++i) { const ImVec2 b = at(t); dl->AddLine(b, ImVec2(b.x + n.x * fl + s.x * 3.5f * scale, b.y + n.y * fl + s.y * 3.5f * scale), col, 1.5f); t += 4.5f * scale; }
    if (ha) { if (pn == 0 && fu == 0) t = 5 * scale; const ImVec2 b = at(t); dl->AddLine(b, ImVec2(b.x + n.x * fl * 0.5f + s.x * 1.8f * scale, b.y + n.y * fl * 0.5f + s.y * 1.8f * scale), col, 1.5f); }
}

// The cloud layer at an altitude in one grid cell: coverage (faded over the last 250 ft inside the layer's base and top, so the
// picture changes smoothly with the altitude) and the layer's thickness. 0 coverage: no cloud there at that altitude.
void cloudAtAlt(const UiCloudGrid& g, int ix, int iy, float ft, float& cov, float& thickFt, bool lowest = false) {
    cov = 0; thickFt = 0;
    if (ix < 0 || iy < 0 || ix >= g.nx || iy >= g.ny) return;
    const size_t c = (size_t)iy * (size_t)g.nx + (size_t)ix;
    if (composite()) {   // every layer at once: the cover of them all together, the depth of cloud in the column
        float clear = 1;
        for (int k = 0; k < g.n[c] && k < 3; ++k) {
            const UiCloudLayer& L = g.layers[c * 3 + (size_t)k];
            if (L.cover <= 0.01f || L.topFt <= L.baseFt) continue;
            clear *= 1.0f - std::min(1.0f, L.cover);
            thickFt += L.topFt - L.baseFt;
        }
        cov = 1.0f - clear;
        return;
    }
    if (lowest) {   // the surface view: the lowest layer wherever there is one
        float base = 1e9f;
        for (int k = 0; k < g.n[c] && k < 3; ++k) {
            const UiCloudLayer& L = g.layers[c * 3 + (size_t)k];
            if (L.cover > 0.01f && L.topFt > L.baseFt && L.baseFt < base) { base = L.baseFt; cov = L.cover; thickFt = L.topFt - L.baseFt; }
        }
        return;
    }
    for (int k = 0; k < g.n[c] && k < 3; ++k) {
        const UiCloudLayer& L = g.layers[c * 3 + (size_t)k];
        if (ft < L.baseFt || ft > L.topFt || L.cover <= 0.01f) continue;
        const float edge = std::max(1.0f, std::min(250.0f, (L.topFt - L.baseFt) * 0.5f));
        const float fe = std::max(0.0f, std::min(1.0f, std::min(ft - L.baseFt, L.topFt - ft) / edge));
        const float cv = L.cover * fe;
        if (cv > cov) { cov = cv; thickFt = L.topFt - L.baseFt; }
    }
}
ImU32 cloudColor(float cov, float thickFt, float fade) {
    // thin layers a light, see-through grey; the deeper the layer, the darker and more solid (15,000 ft and more: dark grey)
    const float th = std::max(0.0f, std::min(1.0f, thickFt / 15000.0f));
    const float c = std::max(0.0f, std::min(1.0f, cov));
    const int g = (int)(200 - 105 * th);
    const int al = (int)((55 + 120 * th) * (0.55f + 0.45f * c) * fade);
    return IM_COL32(g, g + 3, g + 8, std::max(0, std::min(255, al)));
}

void radarCanvas(const UiData& d, const UiFonts& f, float size, double rangeNm, bool interactive) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float R = size * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(15, 17, 20, 255), 6.0f);
    dl->PushClipRect(p, ImVec2(p.x + size, p.y + size), true);
    const double aLat = d.havePos ? d.lat : (d.res ? d.res->centerLat : 0), aLon = d.havePos ? d.lon : (d.res ? d.res->centerLon : 0);
    const ImVec2 pan = interactive ? mapPan() : ImVec2(0, 0);   // nm east, north of the aircraft
    const double kx = std::cos(aLat * kPi / 180.0);
    const double cLat = aLat + pan.y / 60.0, cLon = aLon + pan.x / (60.0 * std::max(0.05, kx));
    (void)rangeNm;
    rangeNm = shownRange();   // eased zoom
    const float px = (float)(R / rangeNm);   // pixels per nm
    // the map turns like the wind wheel: heading up in Heading Lock; in North Lock true north up, or magnetic north up with °M
    const float mapRotDeg = viewRot() + magTurn(interactive ? 3 : 4, windMagnetic(), (float)magVar(d, aLat, aLon)) * (1.0f - headingUpness());
    const double rot = mapRotDeg * kPi / 180.0;
    const float cr = (float)std::cos(rot), sr = (float)std::sin(rot);
    auto at = [&](double lat, double lon) {
        double dlon = lon - cLon;
        if (dlon > 180) dlon -= 360; else if (dlon < -180) dlon += 360;
        const float ex = (float)(dlon * 60.0 * kx) * px, ny = (float)((lat - cLat) * 60.0) * px;   // east, north in pixels
        return ImVec2(c.x + ex * cr - ny * sr, c.y - (ex * sr + ny * cr));
    };
    auto inside = [&](const ImVec2& q, float m) { return q.x > p.x - m && q.y > p.y - m && q.x < p.x + size + m && q.y < p.y + size + m; };
    // map
    if (showMap()) {
        const double spanLat = rangeNm * 1.5 / 60.0, spanLon = spanLat / std::max(0.15, kx);
        const float la0 = (float)(cLat - spanLat), la1 = (float)(cLat + spanLat), lo0 = (float)(cLon - spanLon), lo1 = (float)(cLon + spanLon);
        static const ImU32 kCol[4] = {IM_COL32(78, 92, 110, 255), IM_COL32(88, 80, 100, 255), IM_COL32(52, 58, 68, 255), IM_COL32(66, 84, 108, 255)};
        static const float kW[4] = {1.4f, 1.2f, 1.0f, 1.1f};
        std::vector<ImVec2> pts;
        for (int pass = 2; pass >= 0; --pass)   // states first, then borders, then coasts and lakes on top
            for (const MapLine& L : basemap()) {
                const int kind = L.kind == 3 ? 0 : L.kind;
                if (kind != pass) continue;
                if (L.lo1 < lo0 || L.lo0 > lo1 || L.la1 < la0 || L.la0 > la1) continue;
                pts.clear();
                for (const ImVec2& q : L.pts) pts.push_back(at(q.y, q.x));
                dl->AddPolyline(pts.data(), (int)pts.size(), kCol[L.kind & 3], ImDrawFlags_None, kW[L.kind & 3]);
            }
    }
    if (showTimeZones()) {   // the time zone boundaries: dashed violet lines over the map
        const double spanLat = rangeNm * 1.5 / 60.0, spanLon = spanLat / std::max(0.15, kx);
        const float la0 = (float)(cLat - spanLat), la1 = (float)(cLat + spanLat), lo0 = (float)(cLon - spanLon), lo1 = (float)(cLon + spanLon);
        const ImU32 col = IM_COL32(190, 140, 235, 200);
        for (const MapLine& L : tzData().lines) {
            if (L.lo1 < lo0 || L.lo0 > lo1 || L.la1 < la0 || L.la0 > la1) continue;
            float run = 0;   // dashes 7 px on, 5 px off, continuing along the line
            ImVec2 a = at(L.pts[0].y, L.pts[0].x);
            for (size_t k = 1; k < L.pts.size(); ++k) {
                const ImVec2 bq = at(L.pts[k].y, L.pts[k].x);
                const float len = std::hypot(bq.x - a.x, bq.y - a.y);
                if (len > size * 3) { a = bq; continue; }   // across the date line
                if ((a.x < p.x && bq.x < p.x) || (a.y < p.y && bq.y < p.y) || (a.x > p.x + size && bq.x > p.x + size) || (a.y > p.y + size && bq.y > p.y + size)) {
                    run += len; a = bq; continue;   // off the map: nothing to draw (the dashes keep their rhythm)
                }
                float s0 = 0;
                while (s0 < len) {
                    const float ph = std::fmod(run, 12.0f), step = std::min(len - s0, ph < 7 ? 7 - ph : 12 - ph);
                    if (ph < 7) {
                        const float t0 = s0 / std::max(len, 1e-3f), t1 = (s0 + step) / std::max(len, 1e-3f);
                        dl->AddLine(ImVec2(a.x + (bq.x - a.x) * t0, a.y + (bq.y - a.y) * t0), ImVec2(a.x + (bq.x - a.x) * t1, a.y + (bq.y - a.y) * t1), col, 1.4f);
                    }
                    s0 += step; run += step;
                }
                a = bq;
            }
        }
    }
    // clouds at the map's altitude: filled where a layer is (darker grey the deeper the layer), with its outline; drawn from the
    // grid's corners by marching squares, so the edges are smooth lines rather than steps
    if (showClouds() && d.clouds && d.clouds->nx > 1 && d.clouds->ny > 1) {
        const UiCloudGrid& g = *d.clouds;
        const float alt = mapAlt().shown, th = 0.08f;
        const size_t N = (size_t)g.nx * (size_t)g.ny;
        static std::vector<float> cv, tk; static std::vector<ImVec2> sp;
        cv.resize(N); tk.resize(N); sp.resize(N);
        for (int iy = 0; iy < g.ny; ++iy)
            for (int ix = 0; ix < g.nx; ++ix) {
                const size_t c = (size_t)iy * (size_t)g.nx + (size_t)ix;
                cloudAtAlt(g, ix, iy, alt, cv[c], tk[c], atSurface());
                sp[c] = at(g.lat0 + iy * g.dLat, g.lon0 + ix * g.dLon);
            }
        const ImDrawListFlags fl = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // neighbouring pieces meet edge to edge, with no seams
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        struct V { ImVec2 p; ImU32 c; bool cross; };
        std::vector<V> poly;
        std::vector<std::pair<ImVec2, ImVec2>> outline;
        const float margin = (float)(std::max(g.dLat * 60.0, g.dLon * 60.0 * kx) * px) + 4;
        for (int iy = 0; iy + 1 < g.ny; ++iy)
            for (int ix = 0; ix + 1 < g.nx; ++ix) {
                const size_t k[4] = {(size_t)iy * g.nx + ix, (size_t)iy * g.nx + ix + 1, (size_t)(iy + 1) * g.nx + ix + 1, (size_t)(iy + 1) * g.nx + ix};   // SW SE NE NW
                bool any = false, all = true;
                for (size_t q : k) { const bool in = cv[q] >= th; any |= in; all &= in; }
                if (!any) continue;
                const ImVec2 mid((sp[k[0]].x + sp[k[2]].x) * 0.5f, (sp[k[0]].y + sp[k[2]].y) * 0.5f);
                if (!inside(mid, margin)) continue;
                poly.clear();
                for (int i = 0; i < 4; ++i) {
                    const size_t a = k[i], b = k[(i + 1) & 3];
                    const bool ia = cv[a] >= th, ib = cv[b] >= th;
                    if (ia) poly.push_back({sp[a], cloudColor(cv[a], tk[a], 1.0f), false});
                    if (ia != ib) {
                        const float t = (th - cv[a]) / (cv[b] - cv[a]);
                        const ImVec2 q(sp[a].x + (sp[b].x - sp[a].x) * t, sp[a].y + (sp[b].y - sp[a].y) * t);
                        poly.push_back({q, cloudColor(th, ia ? tk[a] : tk[b], 1.0f), true});
                    }
                }
                const int m = (int)poly.size();
                if (m < 3) continue;
                dl->PrimReserve((m - 2) * 3, m);
                const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
                for (const V& v : poly) dl->PrimWriteVtx(v.p, uv, v.c);
                for (int i = 1; i + 1 < m; ++i) { dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + i)); dl->PrimWriteIdx((ImDrawIdx)(base + i + 1)); }
                if (!all)   // the outline: between consecutive crossing points (the polygon's edges that run through the square)
                    for (int i = 0; i < m; ++i) if (poly[(size_t)i].cross && poly[(size_t)((i + 1) % m)].cross) outline.push_back({poly[(size_t)i].p, poly[(size_t)((i + 1) % m)].p});
            }
        dl->Flags = fl;
        for (const auto& e : outline) dl->AddLine(e.first, e.second, IM_COL32(205, 210, 220, 200), 1.3f);
    }
    // turbulence at the map's altitude; a new grid (an update, or the altitude slider moved) fades in over the one before
    if (showTurb()) {
        static std::vector<UiTurbCell> prev, cur;
        static long seq = -1;
        static float fade = 1.0f;
        static double pdLat = 0.2, pdLon = 0.27, cdLat = 0.2, cdLon = 0.27;
        if (d.turbGridSeq != seq) {
            if (seq >= 0) { prev.swap(cur); pdLat = cdLat; pdLon = cdLon; fade = 0.0f; }
            cur = d.turbGrid; cdLat = d.turbGridDLat; cdLon = d.turbGridDLon;
            seq = d.turbGridSeq;
        }
        fade = std::min(1.0f, fade + std::max(0.0f, ImGui::GetIO().DeltaTime) * 3.0f);
        const ImDrawListFlags fl = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // the squares meet edge to edge, with no seams
        auto drawGrid = [&](const std::vector<UiTurbCell>& grid, double dLat, double dLon, float a) {
            if (a <= 0.01f) return;
            const double hy = dLat * 0.5, hx = dLon * 0.5;
            for (const UiTurbCell& g : grid) {
                if (g.edr < 0.08) continue;
                const ImVec2 q = at(g.lat, g.lon);
                if (!inside(q, 12.0f * px)) continue;
                // corners from the grid's own spacing: neighbouring squares share them exactly (no seams, no overlaps)
                dl->AddQuadFilled(at(g.lat + hy, g.lon - hx), at(g.lat + hy, g.lon + hx), at(g.lat - hy, g.lon + hx), at(g.lat - hy, g.lon - hx), edrColor(g.edr, (int)(64 * a)));
            }
        };
        if (fade < 1.0f) drawGrid(prev, pdLat, pdLon, 1.0f - fade);
        drawGrid(cur, cdLat, cdLon, fade);
        dl->Flags = fl;
    }
    // echoes, drawn like a radar picture rather than as dots: every cell is spread onto a fine screen grid (full strength in its
    // core, fading out towards its edge, the strongest cell winning where they overlap), and the grid is drawn with the colours
    // blended between its points, so the echoes have soft edges and run from green rims into yellow, orange and red cores
    if (showRadar() && d.res && d.res->radar.ok) {
        const double cell = d.cfg ? d.cfg->radarCellNm : 6.0;
        const float step = std::max(4.0f, size / 120.0f);
        const int n = (int)std::ceil(size / step) + 1;
        // Each echo block is spread as a soft square (a rounded superellipse, aligned with north like the radar's own grid) and
        // neighbouring blocks are blended by weight rather than stacked as discs; then a fine-grained texture tied to the ground
        // (it does not crawl when the map moves) breaks up the smooth blobs the way real reflectivity is speckled.
        static std::vector<float> v, sw, swv;
        v.assign((size_t)n * (size_t)n, 0.0f); sw.assign(v.size(), 0.0f); swv.assign(v.size(), 0.0f);
        bool any = false;
        // With the natural picture the map shows the radar mosaic itself (1 nm, lightly smoothed), not the records' squares: the real
        // shapes of the echoes at every zoom ("radar too blocky", 2026-11-12). Without it, the blocks are spread as before.
        const bool raster = !d.res->radar.view.empty();
        if (raster) {
            for (int iy = 0; iy < n; ++iy)
                for (int ix = 0; ix < n; ++ix) {
                    const float X = ix * step + p.x - c.x, Yp = c.y - (iy * step + p.y);
                    const double ex = (X * cr + Yp * sr) / px, ny = (-X * sr + Yp * cr) / px;
                    const float z = d.res->radar.viewAt(cLat + ny / 60.0, cLon + ex / (60.0 * kx));
                    if (z < 8.0f) continue;
                    const size_t q = (size_t)iy * n + ix;
                    sw[q] = 1.0f; swv[q] = z; any = true;
                }
        }
        for (const auto& k : d.res->radar.cells) {
            if (raster) break;
            if (k.dbz < 10) continue;
            const double sz = k.natural && k.sizeNm > 0 ? k.sizeNm : cell;
            const float half = std::max(1.0f, (float)(sz * 0.5 * px)), r1 = std::max(step * 1.5f, half * 1.45f), r0 = half * 0.7f;
            const ImVec2 q = at(k.lat, k.lon);
            if (!inside(q, r1 * 1.42f)) continue;
            const float dbz = k.synthetic ? k.dbz * 0.85f : k.dbz;
            const float ext = r1 * 1.42f;
            const int x0 = std::max(0, (int)std::floor((q.x - ext - p.x) / step)), x1 = std::min(n - 1, (int)std::ceil((q.x + ext - p.x) / step));
            const int y0 = std::max(0, (int)std::floor((q.y - ext - p.y) / step)), y1 = std::min(n - 1, (int)std::ceil((q.y + ext - p.y) / step));
            for (int iy = y0; iy <= y1; ++iy)
                for (int ix = x0; ix <= x1; ++ix) {
                    const float sx = p.x + ix * step - q.x, sy = p.y + iy * step - q.y;
                    const float gx = sx * cr + (-sy) * sr, gy = -sx * sr + (-sy) * cr;   // east / north of the block's centre
                    const float ax = gx * gx, ay = gy * gy;
                    const float dd = std::sqrt(std::sqrt(ax * ax + ay * ay));   // superellipse (p = 4): a square with rounded corners
                    if (dd >= r1) continue;
                    float t = dd <= r0 ? 1.0f : 1.0f - (dd - r0) / (r1 - r0);
                    t = t * t * (3 - 2 * t);
                    const size_t c = (size_t)iy * n + ix;
                    sw[c] += t; swv[c] += t * dbz; any = true;
                }
        }
        const ImVec2 c0 = c;   // the map's centre on the screen
        if (any) {
            // ground-tied value noise: two octaves, about 1.6 nm and 0.6 nm across
            auto hash = [](int x, int y) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return ((h ^ (h >> 16)) & 0xffff) / 65535.0f; };
            auto vnoise = [&](double gx, double gy) {
                const double fx = std::floor(gx), fy = std::floor(gy);
                const int ix = (int)fx, iy = (int)fy;
                float tx = (float)(gx - fx), ty = (float)(gy - fy);
                tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
                const float a = hash(ix, iy), b = hash(ix + 1, iy), c2 = hash(ix, iy + 1), e = hash(ix + 1, iy + 1);
                return (a + (b - a) * tx) + ((c2 + (e - c2) * tx) - (a + (b - a) * tx)) * ty;
            };
            for (int iy = 0; iy < n; ++iy)
                for (int ix = 0; ix < n; ++ix) {
                    const size_t c = (size_t)iy * n + ix;
                    if (sw[c] <= 0) continue;
                    const float cov = std::min(1.0f, sw[c]);
                    float val = swv[c] / sw[c];
                    // the ground position of this grid point (inverse of at()), in nm east / north of the map's centre
                    const float X = ix * step + p.x - c0.x, Yp = c0.y - (iy * step + p.y);
                    const double ex = (X * cr + Yp * sr) / px, ny = (-X * sr + Yp * cr) / px;
                    const double gx = (cLon * 60.0 * kx + ex), gy = (cLat * 60.0 + ny);
                    const float nz = 0.65f * (vnoise(gx / 1.6, gy / 1.6) - 0.5f) + 0.35f * (vnoise(gx / 0.6 + 17.3, gy / 0.6 - 4.1) - 0.5f);
                    if (raster) { v[c] = val + nz * 3.0f; continue; }   // the mosaic's own edges: only a fine speckle on top
                    val += nz * 9.0f * cov;
                    // the edge: weak and ragged where the coverage runs out
                    val *= 0.55f + 0.45f * cov;
                    if (cov < 0.6f) val -= (0.6f - cov) * 18.0f * (0.6f + nz);
                    v[c] = val;
                }
            // the NWS-like colour ramp, see-through at its weak end
            auto ramp = [](float z) {
                static const float st[][5] = {{10, 20, 150, 70, 0}, {15, 30, 165, 72, 95}, {22, 55, 190, 72, 170}, {32, 235, 215, 60, 195},
                                              {40, 245, 145, 40, 210}, {48, 230, 50, 40, 220}, {56, 200, 40, 70, 228}, {64, 220, 70, 220, 235}};
                if (z <= st[0][0]) return IM_COL32(0, 0, 0, 0);
                for (int i = 1; i < 8; ++i)
                    if (z <= st[i][0]) {
                        const float t = (z - st[i - 1][0]) / (st[i][0] - st[i - 1][0]);
                        auto L = [&](int c) { return (int)(st[i - 1][c] + (st[i][c] - st[i - 1][c]) * t); };
                        return IM_COL32(L(1), L(2), L(3), L(4));
                    }
                return IM_COL32(220, 70, 220, 235);
            };
            const ImDrawListFlags fl = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
            for (int iy = 0; iy + 1 < n; ++iy)
                for (int ix = 0; ix + 1 < n; ++ix) {
                    const float a = v[(size_t)iy * n + ix], b = v[(size_t)iy * n + ix + 1], c = v[(size_t)(iy + 1) * n + ix + 1], e = v[(size_t)(iy + 1) * n + ix];
                    if (a < 10 && b < 10 && c < 10 && e < 10) continue;
                    dl->PrimReserve(6, 4);
                    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
                    const float x = p.x + ix * step, y = p.y + iy * step;
                    dl->PrimWriteVtx(ImVec2(x, y), uv, ramp(a));
                    dl->PrimWriteVtx(ImVec2(x + step, y), uv, ramp(b));
                    dl->PrimWriteVtx(ImVec2(x + step, y + step), uv, ramp(c));
                    dl->PrimWriteVtx(ImVec2(x, y + step), uv, ramp(e));
                    dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 1)); dl->PrimWriteIdx((ImDrawIdx)(base + 2));
                    dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 2)); dl->PrimWriteIdx((ImDrawIdx)(base + 3));
                }
            dl->Flags = fl;
        }
    }
    // rings, and north where the map is turned
    const ImVec2 ac = at(aLat, aLon);   // the aircraft on the screen (the centre unless the map was dragged)
    const double nice = rangeNmOf(radarRange());
    for (float fr : {0.25f, 0.5f, 1.0f}) {
        const float rr = (float)(nice * fr) * px;
        dl->AddCircle(ac, rr, cRing, 64, 1.0f);
        const std::string t = fmt("%g", nice * fr);
        // the labels sit on the rings towards the lower right, clear of the N marker and the aircraft
        dl->AddText(f.body, ImGui::GetFontSize() * 0.8f, ImVec2(ac.x + rr * 0.707f + 2, ac.y + rr * 0.707f - 2), IM_COL32(107, 114, 124, 255), t.c_str());
    }
    {
        const ImVec2 nq(c.x - sr * (R - 10), c.y - cr * (R - 10));
        const ImVec2 ts = ImGui::CalcTextSize("N");
        dl->AddCircleFilled(nq, ts.y * 0.62f, IM_COL32(15, 17, 20, 220), 16);
        dl->AddText(f.bold, ImGui::GetFontSize(), ImVec2(nq.x - ts.x * 0.5f, nq.y - ts.y * 0.5f), cSoft, "N");
    }
    // AIRMETs (one toggle), SIGMETs and convective SIGMETs (the other): filled areas and lines, coloured by category
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hitKind = 0; std::string hitKey; ImVec2 hitAt; float hitArea = 1e30f;
    if (d.reports && (showAirmets() || showSigmets())) {
        std::vector<ImVec2> pts;
        for (int pass = 0; pass < 3; ++pass)   // AIRMETs underneath, SIGMETs over them, convective on top
            for (const auto& adv : d.reports->advisories) {
                if (adv.cls != pass || !advCurrent(adv, d.now)) continue;
                if (adv.cls == hz::Advisory::Airmet ? !showAirmets() : !showSigmets()) continue;
                pts.clear();
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto& q : adv.pts) { const ImVec2 v = at(q.first, q.second); pts.push_back(v); x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y); }
                if (x1 < p.x || y1 < p.y || x0 > p.x + size || y0 > p.y + size || pts.size() < 2) continue;
                const bool sel = isSel(3, advKey(adv));
                if (adv.line) {
                    dl->AddPolyline(pts.data(), (int)pts.size(), advColor(adv, sel ? 255 : 230), ImDrawFlags_None, sel ? 4.0f : 3.0f);
                    if (interactive) for (size_t k = 0; k + 1 < pts.size(); ++k) if (distToSeg(mouse, pts[k], pts[k + 1]) < 6 && 0.0f < hitArea) { hitKind = 3; hitKey = advKey(adv); hitAt = mouse; hitArea = 0.0f; }
                } else {
                    if (pts.size() >= 3 && pts.front().x == pts.back().x && pts.front().y == pts.back().y) pts.pop_back();
                    if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), advColor(adv, sel ? 70 : 38));
                    dl->AddPolyline(pts.data(), (int)pts.size(), advColor(adv, 235), ImDrawFlags_Closed, sel ? 3.0f : 1.8f);
                    const float area = (x1 - x0) * (y1 - y0);
                    if (interactive && pts.size() >= 3 && area < hitArea && pointInPoly(pts, mouse)) { hitKind = 3; hitKey = advKey(adv); hitAt = mouse; hitArea = area; }
                }
            }
    }
    // points (pilot reports, airports, wind barbs) come before the areas: they stay clickable inside an AIRMET or SIGMET
    float bestPt = 10.0f;
    // wind barbs at the map's altitude (fewer when zoomed out, so they do not pile up). Every barb fades in and out instead of
    // popping: with the altitude (a point's data ends), and when the map gets too crowded for it; barbs already shown keep their
    // place first, so the set does not flicker while the slider or the zoom moves.
    if (showWinds() || !windPts().empty()) {
        std::vector<ImVec2> placed;
        const float minSep = 34.0f;
        const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
        const float k = 1.0f - std::exp(-dt * 5.0f);
        std::vector<size_t> order(windPts().size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) { return windPts()[a].show > windPts()[b].show + 0.25f; });
        for (size_t i : order) {
            WindPt& w = windPts()[i];
            const ImVec2 q = at(w.lat, w.lon);
            if (interactive && d.forceWindSel >= 0 && (int)i == d.forceWindSel) selOpen(4, w.key, q);
            const bool sel = isSel(4, w.key);
            bool crowded = false;
            if (!sel && w.rangeA > 0) for (const ImVec2& o : placed) if (std::fabs(o.x - q.x) < minSep && std::fabs(o.y - q.y) < minSep) { crowded = true; break; }
            const float target = showWinds() ? (crowded ? 0.0f : w.rangeA) : 0.0f;
            if (d.forceWindSel >= 0 || d.forceMapAltFt >= 0) w.show = target;   // test pictures: settled
            w.show = std::fabs(target - w.show) < 0.01f ? target : w.show + (target - w.show) * k;
            if (!crowded && w.rangeA > 0) placed.push_back(q);
            if (w.show < 0.02f || !inside(q, 30)) continue;
            float dir = 0, kt = 0;
            windOf(w, dir, kt);
            const int al = (int)(235 * w.show);
            // the selected barb (its decode window open) in amber, the others white (stations) or blue (model points)
            // blue, darker and deeper the stronger the wind (light at 5 kt, deep blue at 100 kt and more); calm: a white circle;
            // the selected barb (its decode window open) amber
            const float sf = std::max(0.0f, std::min(1.0f, (kt - 5.0f) / 95.0f));
            const ImU32 blue = IM_COL32((int)(175 - 145 * sf), (int)(215 - 135 * sf), (int)(255 - 25 * sf), al);
            const ImU32 col = sel ? IM_COL32(255, 196, 70, std::max(al, 200)) : (kt < 2.5f ? IM_COL32(255, 255, 255, al) : blue);
            if (sel) selAt(4, w.key, q);
            drawBarb(dl, q, dir, kt, mapRotDeg, col, sel ? 1.12f : 1.0f);
            if (composite() && w.levelFt > 0) {   // which level that strongest wind is at
                const std::string lv = fmt("%03.0f", w.levelFt / 100.0f);
                dl->AddText(f.body, ImGui::GetFontSize() * 0.72f, ImVec2(q.x + 5, q.y + 2), IM_COL32(170, 180, 195, (int)(220 * w.show)), lv.c_str());
            }
            // clicked on its staff and feathers (not on the point itself, which belongs to the airport there, if any)
            const double ang = (dir - mapRotDeg) * kPi / 180.0;
            const ImVec2 sv((float)std::sin(ang), -(float)std::cos(ang));
            const ImVec2 s0(q.x + sv.x * 7, q.y + sv.y * 7), s1(q.x + sv.x * 30, q.y + sv.y * 30);
            // the barb: its staff and feathers (from 7 px out); the circle at its foot is the airport's (its METAR, ATIS, surface wind)
            const bool onCircle = std::hypot(mouse.x - q.x, mouse.y - q.y) <= 7.5f;
            const float dm = kt < 2.5f ? (w.surface ? 1e9f : std::fabs(std::hypot(mouse.x - q.x, mouse.y - q.y) - 5.5f) + 3.0f) : distToSeg(mouse, s0, s1);
            if (interactive && w.show > 0.5f && dm < 10.0f && dm < bestPt && !(onCircle && w.surface)) { bestPt = dm; hitKind = 4; hitKey = w.key; hitAt = q; }
        }
    }
    if (showPireps() && d.reports) {
        for (const auto& pr : d.reports->pireps) {
            const ImVec2 q = at(pr.lat, pr.lon);
            if (!inside(q, 6)) continue;
            const ImU32 col = pr.urgent ? IM_COL32(235, 64, 64, 255) : IM_COL32(240, 150, 40, 255);   // UUA red, UA orange
            if (interactive && d.forcePirep >= 0 && &pr == &d.reports->pireps[(size_t)std::min<int>(d.forcePirep, (int)d.reports->pireps.size() - 1)]) selOpen(1, pirepKey(pr), q);
            if (interactive && !panGliding() && !pendingPirep().empty() && pendingPirep() == pirepKey(pr)) { selOpen(1, pirepKey(pr), q); pendingPirep().clear(); }
            const bool sel = isSel(1, pirepKey(pr));
            if (sel) { selAt(1, pirepKey(pr), q); dl->AddCircle(q, 10.0f, IM_COL32(255, 255, 255, 220), 20, 1.5f); }
            const float dm = std::hypot(mouse.x - q.x, mouse.y - q.y);
            if (interactive && dm < bestPt) { bestPt = dm; hitKind = 1; hitKey = pirepKey(pr); hitAt = q; }
            dl->AddTriangleFilled(ImVec2(q.x, q.y - 6), ImVec2(q.x + 5.5f, q.y + 4), ImVec2(q.x - 5.5f, q.y + 4), col);
            dl->AddTriangle(ImVec2(q.x, q.y - 6), ImVec2(q.x + 5.5f, q.y + 4), ImVec2(q.x - 5.5f, q.y + 4), IM_COL32(15, 17, 20, 255));
            if (pr.altFt > 0) {
                const std::string t = fmt("%03.0f", pr.altFt / 100.0);
                dl->AddText(f.body, ImGui::GetFontSize() * 0.72f, ImVec2(q.x + 6, q.y - 4), col, t.c_str());
            }
        }
    }
    if (showAirports() && d.res)
        for (const auto& st : d.res->stations) {
            if (st.icao.compare(0, 3, "GFS") == 0) continue;
            const ImVec2 q = at(st.lat, st.lon);
            if (!inside(q, 0)) continue;
            const bool sel = isSel(2, st.icao);
            if (sel) { selAt(2, st.icao, q); dl->AddCircle(q, 8.0f, IM_COL32(255, 255, 255, 220), 16, 1.5f); }
            dl->AddCircleFilled(q, 3.5f, catColor(flightCategory(st.report)), 12);
            if (rangeNm <= 40) dl->AddText(f.body, ImGui::GetFontSize() * 0.75f, ImVec2(q.x + 5, q.y - 6), cMuted, st.icao.c_str());
            const float dm = std::hypot(mouse.x - q.x, mouse.y - q.y);
            if (interactive && (dm < bestPt || (dm <= 7.5f && hitKind == 4))) { bestPt = dm; hitKind = 2; hitKey = st.icao; hitAt = q; }
        }
    // the aircraft: its true heading (not the ground track, which swings around when slow or stopped)
    const double a = (d.headingDeg - mapRotDeg) * kPi / 180.0;
    const ImVec2 dir((float)std::sin(a), -(float)std::cos(a)), n(-dir.y, dir.x);
    dl->AddTriangleFilled(ImVec2(ac.x + dir.x * 10, ac.y + dir.y * 10), ImVec2(ac.x - dir.x * 7 + n.x * 6, ac.y - dir.y * 7 + n.y * 6),
                          ImVec2(ac.x - dir.x * 7 - n.x * 6, ac.y - dir.y * 7 - n.y * 6), IM_COL32(255, 255, 255, 255));
    // bubbles on the Map tab's map: the time (top left), Center on aircraft and North / Heading Lock (top right, clickable)
    int bubbleHit = 0;   // 1 center, 2 lock
    if (interactive) mapSel().keepOut.clear();   // the bubbles drawn below: the info windows keep off them
    // blue: Heading Lock on (its text and outline in the dial's blue, like the wind wheel's outline)
    auto bubble = [&](ImVec2 b0, const char* label, const char* text, bool blue, bool dim) {
        const ImVec2 ls = label ? ImGui::CalcTextSize(label) : ImVec2(0, 0), ts = ImGui::CalcTextSize(text);
        const float lw = label ? ls.x * 0.82f + 6 : 0;
        const ImVec2 b1(b0.x + lw + ts.x + 12, b0.y + ts.y + 6);
        const bool hov = interactive && !dim && mouse.x >= b0.x && mouse.x <= b1.x && mouse.y >= b0.y && mouse.y <= b1.y;
        if (interactive) mapSel().keepOut.push_back({b0, b1});
        dl->AddRectFilled(b0, b1, hov ? IM_COL32(34, 38, 46, 235) : IM_COL32(20, 22, 26, 220), 4.0f);
        dl->AddRect(b0, b1, blue ? cLockBlue : IM_COL32(60, 64, 72, 255), 4.0f, 0, blue ? 1.5f : 1.0f);
        if (label) dl->AddText(f.bold, ImGui::GetFontSize() * 0.82f, ImVec2(b0.x + 6, b0.y + 3 + ts.y * 0.09f), cIdle, label);
        dl->AddText(ImVec2(b0.x + 6 + lw, b0.y + 3), blue ? cLockBlue : (dim ? cIdle : cSoft), text);
        return std::make_pair(hov, b1.x - b0.x);
    };
    if (interactive) {
        const bool movedOff = std::fabs(mapPan().x) > 0.01f || std::fabs(mapPan().y) > 0.01f;
        if (movedOff && !recentering()) {   // the map moved off the aircraft: a small + marks its centre (the local time's place):
            // see-through inside, outlined in white (with a faint dark edge, so it shows on light radar colours too)
            const float a = 5.0f, w = 1.5f;   // arm length, half the arm width
            const ImVec2 pts[12] = {{c.x - w, c.y - a}, {c.x + w, c.y - a}, {c.x + w, c.y - w}, {c.x + a, c.y - w}, {c.x + a, c.y + w}, {c.x + w, c.y + w},
                                    {c.x + w, c.y + a}, {c.x - w, c.y + a}, {c.x - w, c.y + w}, {c.x - a, c.y + w}, {c.x - a, c.y - w}, {c.x - w, c.y - w}};
            dl->AddPolyline(pts, 12, IM_COL32(0, 0, 0, 90), ImDrawFlags_Closed, 1.8f);
            dl->AddPolyline(pts, 12, IM_COL32(255, 255, 255, 220), ImDrawFlags_Closed, 0.7f);
        }
        {   // the time: Zulu, or (clicked: blue) local time at the aircraft, or at the + in the middle while the map is moved off it
            bool est = false;
            const double tLat = movedOff ? cLat : d.lat, tLon = movedOff ? cLon : d.lon;
            const double off = localOffsetAt(d, tLat, tLon, est);
            const std::string t = mapLocalTime() ? clockText(d.now + (std::time_t)off, false) + " " + (est ? "~" : "") + zoneText(tLat, tLon, off, d.now) : clockText(d.now, false) + "Z";
            if (bubble(ImVec2(p.x + 8, p.y + 8), mapLocalTime() ? "LOCAL" : "TIME", t.c_str(), mapLocalTime(), false).first) bubbleHit = 3;
        }
        {   // the units, as bubbles at the bottom right (lit blue when °M / °F, like LOCAL): a click swaps them on every page
            const char* wt = windMagnetic() ? "°M" : "°T";
            const char* tt = tempF() ? "°F" : "°C";
            const float bh = ImGui::GetTextLineHeight() + 6, y = p.y + size - 8 - bh;
            // the widths, as bubble() makes them (label at 82 %, the value, padding)
            auto bwOf = [&](const char* l, const char* t) { return ImGui::CalcTextSize(l).x * 0.82f + 6 + ImGui::CalcTextSize(t).x + 12; };
            const float wT = bwOf("TEMP", "°F"), wW = bwOf("WIND", "°M");   // the wider value of each, so they do not shift when switched
            if (bubble(ImVec2(p.x + size - 8 - wT, y), "TEMP", tt, tempF(), false).first) bubbleHit = 5;
            if (bubble(ImVec2(p.x + size - 8 - wT - 6 - wW, y), "WIND", wt, windMagnetic(), false).first) bubbleHit = 4;
        }
        const std::string lkS = dialHeadingUp() && d.havePos ? headingText(d) : std::string(dialHeadingUp() ? "Heading Lock" : "North Lock");
        const char* lk = lkS.c_str();
        const char* ce = "Center on aircraft";
        const float wl = ImGui::CalcTextSize(lk).x + 12, wc = ImGui::CalcTextSize(ce).x + 12;
        const bool panned = std::fabs(mapPan().x) > 0.01f || std::fabs(mapPan().y) > 0.01f;
        if (bubble(ImVec2(p.x + size - 8 - wl, p.y + 8), nullptr, lk, dialHeadingUp(), false).first) bubbleHit = 2;
        // blue while the map is centred on the aircraft (it was pressed, or never moved), until the map is dragged away
        if (bubble(ImVec2(p.x + size - 8 - wl - 6 - wc, p.y + 8), nullptr, ce, !panned || recentering(), false).first && panned) bubbleHit = 1;   // lit as soon as it is pressed
        if (bubbleHit) { hitKind = 0; }
    }
    if (interactive && ImGui::GetTime() < popupFullUntil()) {   // the map is full of popups: a short notice in the middle, over them
        ImDrawList* fg = ImGui::GetForegroundDrawList();   // drawn after every window, so the popups cannot cover it
        fg->PushClipRect(p, ImVec2(p.x + size, p.y + size), true);
        const float left = (float)(popupFullUntil() - ImGui::GetTime());
        const int al = (int)(255 * std::min(1.0f, left / 0.3f));
        const char* t = "MAX AMOUNT OF POPUPS";
        const float fsz = ImGui::GetFontSize() * 1.9f;
        const ImVec2 ts = f.title->CalcTextSizeA(fsz, FLT_MAX, 0.0f, t);
        const ImVec2 b0(p.x + (size - ts.x) * 0.5f - 22, p.y + size * 0.5f - ts.y * 0.5f - 14), b1(b0.x + ts.x + 44, b0.y + ts.y + 28);
        fg->AddRectFilled(b0, b1, IM_COL32(28, 30, 34, (int)(245 * al / 255)), 8.0f);
        fg->AddRect(b0, b1, IM_COL32(245, 190, 80, al), 8.0f, 0, 2.0f);
        fg->AddText(f.title, fsz, ImVec2(b0.x + 22, b0.y + 14), IM_COL32(245, 190, 80, al), t);
        fg->PopClipRect();
    }
    dl->PopClipRect();
    ImGui::InvisibleButton("radarclick", ImVec2(size, size));
    if (interactive) { mapSel().mapMin = p; mapSel().mapMax = ImVec2(p.x + size, p.y + size); }   // the info window stays inside this
    const bool hov = ImGui::IsItemHovered();
    // the mouse wheel zooms: 1, 2, 5, 10, 20, 40, 80, 160, 320 nm (eased)
    if (hov && ImGui::GetIO().MouseWheel != 0.0f) radarRange() = std::max(0, std::min(kRangeCount - 1, radarRange() - (ImGui::GetIO().MouseWheel > 0 ? 1 : -1)));
    if (interactive) {
        // drag to move the map (the Map tab only); a click without a drag selects
        static bool dragged = false;
        if (ImGui::IsItemActivated()) dragged = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            dragged = true;
            recentering() = false;
            const ImVec2 dlt = ImGui::GetIO().MouseDelta;
            // the screen movement back to east/north (the map may be turned)
            const float sx = dlt.x / px, sy = -dlt.y / px;
            mapPan().x -= sx * cr + sy * sr;
            mapPan().y -= -sx * sr + sy * cr;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (hov && hitKind) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (bubbleHit && hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemDeactivated() && !dragged && hov && bubbleHit) {   // a bubble: re-centre, or switch North / Heading Lock
            if (bubbleHit == 1) recentering() = true;
            else if (bubbleHit == 3) mapLocalTime() = !mapLocalTime();
            else if (bubbleHit == 4) windMagnetic() = !windMagnetic();
            else if (bubbleHit == 5) tempF() = !tempF();
            else dialHeadingUp() = !dialHeadingUp();
        } else if (ImGui::IsItemDeactivated() && !dragged && hov) {
            if (hitKind) selOpen(hitKind, hitKey, hitAt);   // another window opens beside the others (as many as fit)
            else mapSel().items.clear();
        }
    }
}

// The small info window of the selected pilot report, airport or AIRMET/SIGMET (radar page)
void infoRow(const char* k, const std::string& v) { ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); muted(k); ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(v.c_str()); }
void infoText(const UiFonts& f, const std::string& t, ImU32 col) {
    if (t.empty()) return;
    ImGui::Spacing();
    ImGui::PushFont(f.mono);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(360.0f, std::max(180.0f, mapSel().mapMax.x - mapSel().mapMin.x - 60)));
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
std::string levelText(double ft) { return ft < 0 ? std::string("?") : (ft == 0 ? std::string("SFC") : fmt("FL%03.0f", ft / 100.0)); }

// One info window. `other`: the rectangle of the window already placed this frame (none: zero size), which this one must not
// overlap; `otherPt`: that window's point, kept clear too. Returns false when there is no room for it beside the other.
// A dragged window, kept inside the map and pushed off the other window (with a gap) by the shortest way that stays inside
ImVec2 keepClear(ImVec2 pos, ImVec2 sz, ImVec2 mn, ImVec2 mx, ImVec2 oMin, ImVec2 oMax) {
    auto clampIn = [&](ImVec2 q) { return ImVec2(std::max(mn.x + 4, std::min(q.x, mx.x - 4 - sz.x)), std::max(mn.y + 4, std::min(q.y, mx.y - 4 - sz.y))); };
    pos = clampIn(pos);
    if (!(oMax.x > oMin.x)) return pos;
    const float g = 8;
    auto hits = [&](ImVec2 q) { return q.x < oMax.x + g && q.x + sz.x > oMin.x - g && q.y < oMax.y + g && q.y + sz.y > oMin.y - g; };
    if (!hits(pos)) return pos;
    const ImVec2 opts[4] = {ImVec2(oMin.x - g - sz.x, pos.y), ImVec2(oMax.x + g, pos.y), ImVec2(pos.x, oMin.y - g - sz.y), ImVec2(pos.x, oMax.y + g)};
    ImVec2 best = pos; float bestD = 1e30f;
    for (const ImVec2& o : opts) {
        const ImVec2 c = clampIn(o);
        if (hits(c)) continue;
        const float dd = std::fabs(c.x - pos.x) + std::fabs(c.y - pos.y);
        if (dd < bestD) { bestD = dd; best = c; }
    }
    return best;
}

using Rect = std::pair<ImVec2, ImVec2>;
bool infoWindowOne(const UiData& d, const UiFonts& f, SelItem& sel, const std::vector<Rect>& others, const std::vector<ImVec2>& otherPts) {
    const hz::Pirep* p = nullptr; const Station* st = nullptr; const hz::Advisory* av = nullptr;
    if (sel.kind == 1 && d.reports && showPireps()) for (const auto& q : d.reports->pireps) if (pirepKey(q) == sel.key) p = &q;
    if (sel.kind == 2 && d.res && showAirports()) for (const auto& q : d.res->stations) if (q.icao == sel.key) st = &q;
    if (sel.kind == 3 && d.reports) for (const auto& q : d.reports->advisories) if (advKey(q) == sel.key && (q.cls == hz::Advisory::Airmet ? showAirmets() : showSigmets())) av = &q;
    const WindPt* wp = nullptr;
    if (sel.kind == 4 && showWinds()) for (const auto& q : windPts()) if (q.key == sel.key) wp = &q;
    if (!p && !st && !av && !wp) { sel.kind = 0; return true; }
    const MapSel& ms = mapSel();
    const ImVec2 mn = ms.mapMin, mx = ms.mapMax, a = sel.at;
    // the size the window will have in THIS frame (ImGui fits an auto-resizing window to last frame's contents in Begin): it is placed
    // with that size before Begin, because Begin draws the background at once and moving the window after Begin left that background
    // over the map's edge for a frame ("still sometimes flickers outside the map", 2026-11-12)
    const std::string winName = "##mapinfo" + std::to_string(sel.kind) + sel.key;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(160.0f, mx.x - mn.x - 8), std::max(120.0f, mx.y - mn.y - 8)));
    ImVec2 sz = sel.lastSize;
    if (ImGuiWindow* w = ImGui::FindWindowByName(winName.c_str())) { const ImVec2 nx = ImGui::CalcWindowNextAutoFitSize(w); if (nx.x > 1 && nx.y > 1) sz = nx; }
    auto overlapsAny = [&](float cx, float cy) {
        for (const Rect& o : others) if (cx < o.second.x + 4 && cx + sz.x > o.first.x - 4 && cy < o.second.y + 4 && cy + sz.y > o.first.y - 4) return true;
        return false;
    };
    const bool measuring = !sel.dragged && !sel.fixed;   // its first frames: drawn unseen until its size settles, then placed once
    if (sel.dragged || sel.fixed) {   // put somewhere by hand, or placed when it opened: it stays there (inside the map, off the others)
        ImVec2 q = keepClear(sel.pos, sz, mn, mx, ImVec2(0, 0), ImVec2(0, 0));   // inside the map always, other windows or not
        for (int pass = 0; pass < 3; ++pass) for (const Rect& o : others) q = keepClear(q, sz, mn, mx, o.first, o.second);
        if (overlapsAny(q.x, q.y)) { GImGui->NextWindowData.ClearFlags(); return false; }
        sel.pos = q;
    } else {   // inside the map, clear of the points and never over another window: tries beside its point, beside each other window,
        // the map's corners, then a scan of the map; the first spot that covers no point wins, else the one that covers them least.
        // No spot clear of the other windows: false (the caller closes the oldest window).
        std::vector<ImVec2> cand = {ImVec2(a.x + 14, a.y - 10), ImVec2(a.x - 14 - sz.x, a.y - 10), ImVec2(a.x - sz.x * 0.5f, a.y + 14), ImVec2(a.x - sz.x * 0.5f, a.y - 14 - sz.y)};
        for (const Rect& o : others) {
            const ImVec2 oMin = o.first, oMax = o.second;
            cand.push_back(ImVec2(oMax.x + 8, a.y - 10)); cand.push_back(ImVec2(oMin.x - 8 - sz.x, a.y - 10));
            cand.push_back(ImVec2(a.x - sz.x * 0.5f, oMax.y + 8)); cand.push_back(ImVec2(a.x - sz.x * 0.5f, oMin.y - 8 - sz.y));
            cand.push_back(ImVec2(oMax.x + 8, oMin.y)); cand.push_back(ImVec2(oMin.x - 8 - sz.x, oMin.y));
            cand.push_back(ImVec2(oMin.x, oMax.y + 8)); cand.push_back(ImVec2(oMin.x, oMin.y - 8 - sz.y));
        }
        cand.push_back(ImVec2(mn.x, mn.y)); cand.push_back(ImVec2(mx.x, mn.y)); cand.push_back(ImVec2(mn.x, mx.y)); cand.push_back(ImVec2(mx.x, mx.y));
        for (float gy = mn.y; gy < mx.y; gy += 24) for (float gx = mn.x; gx < mx.x; gx += 24) cand.push_back(ImVec2(gx, gy));
        auto cover = [&](float cx, float cy, ImVec2 pt) {
            const float ox = std::min(cx + sz.x, pt.x + 10) - std::max(cx, pt.x - 10), oy = std::min(cy + sz.y, pt.y + 10) - std::max(cy, pt.y - 10);
            return (ox > 0 && oy > 0) ? ox * oy : 0.0f;
        };
        float x = 0, y = 0, bestPen = 1e30f;
        bool found = false;
        for (const ImVec2& c : cand) {
            const float cx = std::max(mn.x + 4, std::min(c.x, mx.x - 4 - sz.x)), cy = std::max(mn.y + 4, std::min(c.y, mx.y - 4 - sz.y));
            if (overlapsAny(cx, cy)) continue;
            float pen = cover(cx, cy, a);
            for (const ImVec2& q : otherPts) pen += cover(cx, cy, q);
            if (pen < bestPen - 0.5f) { bestPen = pen; x = cx; y = cy; found = true; }
            if (pen <= 0) break;
        }
        if (!found) { GImGui->NextWindowData.ClearFlags(); return false; }
        sel.pos = ImVec2(x, y);
        // shown only once its size has settled (the same for 2 frames running: wrapped text can take a few frames to fit), so it
        // appears at its final place and never jumps inside the map after it was seen ("opens outside, then jumps in", 2026-11-12)
        const bool same = std::fabs(sel.lastSize.x - sel.measSize.x) < 0.5f && std::fabs(sel.lastSize.y - sel.measSize.y) < 0.5f;
        sel.stable = same ? sel.stable + 1 : 0;
        sel.measSize = sel.lastSize;
        if ((++sel.frames >= 3 && sel.stable >= 2) || sel.frames >= 15) sel.fixed = true;
    }
    // inside the map with this frame's size (see above), then handed to Begin
    sel.pos = ImVec2(std::max(mn.x + 4, std::min(sel.pos.x, mx.x - 4 - sz.x)), std::max(mn.y + 4, std::min(sel.pos.y, mx.y - 4 - sz.y)));
    ImGui::SetNextWindowPos(sel.pos);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, measuring ? 0.0f : ImGui::GetStyle().Alpha);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(IM_COL32(28, 30, 34, 255)));
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | (measuring ? ImGuiWindowFlags_NoInputs : 0);
    if (ImGui::Begin(winName.c_str(), nullptr, fl)) {
        {   // inside the map in this very frame: its size is known now (it may have grown since the place was chosen), so it is
            // moved in before anything is drawn, never shown over the edge for a frame
            const ImVec2 ws = ImGui::GetWindowSize(), wp = ImGui::GetWindowPos();
            const ImVec2 q(std::max(mn.x + 4, std::min(wp.x, mx.x - 4 - ws.x)), std::max(mn.y + 4, std::min(wp.y, mx.y - 4 - ws.y)));
            if (std::fabs(q.x - wp.x) > 0.5f || std::fabs(q.y - wp.y) > 0.5f) { ImGui::SetWindowPos(q); sel.pos = q; }
        }
        // dragged by its background (not by its buttons): it follows the mouse, and keeps that place afterwards
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) sel.dragging = true;
        if (sel.dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // it slides only where it touches nothing: not another window, not the map's buttons, not the map's edge. A move
                // that would touch something goes as far as it can along each axis (it slides along the obstacle) and stops there,
                // so a dragged window can never be pushed onto another one (which closed a window with "max popups")
                const ImVec2 dlt = ImGui::GetIO().MouseDelta;
                if (dlt.x != 0 || dlt.y != 0) {
                    const ImVec2 ws = ImGui::GetWindowSize();
                    auto clear = [&](ImVec2 q) {
                        if (q.x < mn.x + 4 || q.y < mn.y + 4 || q.x + ws.x > mx.x - 4 || q.y + ws.y > mx.y - 4) return false;
                        auto hit = [&](ImVec2 o0, ImVec2 o1) { return q.x < o1.x + 8 && q.x + ws.x > o0.x - 8 && q.y < o1.y + 8 && q.y + ws.y > o0.y - 8; };
                        for (const Rect& o : others) if (hit(o.first, o.second)) return false;
                        // every other window too (`others` holds only the ones placed before this one in this frame)
                        for (const SelItem& o : mapSel().items)
                            if (&o != &sel && o.shown && o.kind != 0 && hit(o.pos, ImVec2(o.pos.x + o.lastSize.x, o.pos.y + o.lastSize.y))) return false;
                        return true;
                    };
                    auto slide = [&](ImVec2 from, ImVec2 step) {   // as far as it goes toward from + step, a pixel at a time at the end
                        if (clear(ImVec2(from.x + step.x, from.y + step.y))) return ImVec2(from.x + step.x, from.y + step.y);
                        const float len = std::max(std::fabs(step.x), std::fabs(step.y));
                        ImVec2 q = from;
                        for (float t = 1.0f; t <= len; t += 1.0f) {
                            const ImVec2 n(from.x + step.x * t / len, from.y + step.y * t / len);
                            if (!clear(n)) break;
                            q = n;
                        }
                        return q;
                    };
                    ImVec2 q = sel.pos;
                    if (!clear(q)) q = ImVec2(q.x + dlt.x, q.y + dlt.y);   // (already touching something: free to be dragged off it)
                    else { q = slide(q, ImVec2(dlt.x, 0)); q = slide(q, ImVec2(0, dlt.y)); }
                    if (q.x != sel.pos.x || q.y != sel.pos.y) { sel.pos = q; sel.dragged = true; }
                }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else sel.dragging = false;
        }
        auto title = [&](const std::string& t, ImU32 col) {   // the close button top left, then the title
            if (ImGui::SmallButton("x")) sel.kind = 0;
            ImGui::SameLine(0, 10);
            ImGui::PushFont(f.bold);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        };
        if (p) {
            title(p->urgent ? "UUA  urgent pilot report" : (p->kind == "AIREP" ? "AIREP" : "UA  pilot report"), p->urgent ? IM_COL32(235, 64, 64, 255) : IM_COL32(240, 150, 40, 255));
            if (ImGui::BeginTable("pr", 2, ImGuiTableFlags_SizingFixedFit)) {
                if (p->obsTime > 0) infoRow("Time", hhmm(p->obsTime) + fmt("  (%ld min ago)", (long)((d.now - p->obsTime) / 60)));
                infoRow("Level", p->altFt > 0 ? fmt("FL%03.0f", p->altFt / 100.0) : std::string("—"));
                infoRow("Aircraft", p->acType.empty() ? std::string("—") : p->acType);
                // what the pilot reported, in one line (as in the lists); the turbulence's layer after it
                std::string cond = pirepConditions(*p);
                if (!p->intensity.empty() && (p->baseFt > 0 || p->topFt > 0)) cond += "  " + levelText(p->baseFt) + "-" + levelText(p->topFt);
                infoRow("Conditions", cond);
                if (!p->icing.empty()) infoRow("Icing", p->icing);
                if (!p->sky.empty()) infoRow("Clouds", p->sky);
                if (!p->wx.empty()) infoRow("Weather", p->wx);
                if (p->tempC > -100) infoRow("Temperature", tempText(p->tempC));
                if (p->windKt >= 0) infoRow("Wind", pirepWind(*p, " / "));
                if (p->llws) infoRow("Wind shear", "low-level wind shear reported");
                if (d.havePos) infoRow("Distance", fmt("%.0f nm", distNm(d.lat, d.lon, p->lat, p->lon)));
                ImGui::EndTable();
            }
            infoText(f, p->raw, cMetar);
        } else if (st) {
            const std::string cat = flightCategory(st->report);
            title(st->icao + (cat.empty() ? "" : "  " + cat), cat.empty() ? cText : catColor(cat));
            if (ImGui::BeginTable("ap", 2, ImGuiTableFlags_SizingFixedFit)) {
                const bool us = usUnits(st->icao);
                {
                    const Report& r = st->report;
                    const double var = magVar(d, st->lat, st->lon);
                    const bool mag = cardWindMagnetic();
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); muted("Wind"); ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(windTextRef(r, var, mag).c_str());
                    if (r.hasWind && r.windKt >= 1 && !r.windVariable) {   // the other reference: a click swaps them
                        ImGui::SameLine(0, 8);
                        magBox(fmt(mag ? "%03d°T" : "%03d°M", windDirIn(r, var, !mag)).c_str(), IM_COL32(196, 200, 207, 255));   // set by the map's unit switch
                    }
                }
                infoRow("Visibility", visText(st->report, us));
                infoRow("Weather", weatherText(st->report));
                infoRow("Clouds", cloudText(st->report));
                if (st->report.hasAltim) infoRow(us ? "Altimeter" : "QNH", us ? fmt("%.2f inHg", st->report.altimHpa / 33.8639) : fmt("%.0f hPa", st->report.altimHpa));
                infoRow("From you", fmt("%.0f nm", distTo(d, *st)));
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::PushFont(f.bold); ImGui::TextUnformatted("METAR"); ImGui::PopFont(); ImGui::SameLine(0, 8); muted("winds true");
            infoText(f, st->rawMetar.empty() ? std::string("(no report text)") : st->rawMetar, cMetar);
            if (st->hasAtis) {
                ImGui::Spacing();
                ImGui::PushFont(f.bold); ImGui::TextUnformatted(("ATIS " + st->atisLetter).c_str()); ImGui::PopFont(); ImGui::SameLine(0, 8); muted("winds magnetic");
                infoText(f, st->atisText, IM_COL32(255, 217, 128, 255));
            }
        } else if (av) {
            title(av->name, advColor(*av, 255));
            if (ImGui::BeginTable("ad", 2, ImGuiTableFlags_SizingFixedFit)) {
                static const char* kCls[] = {"AIRMET", "SIGMET", "Convective SIGMET"};
                infoRow("Type", kCls[std::max(0, std::min(2, av->cls))]);
                infoRow("Hazard", av->hazard + (av->qualifier.empty() ? "" : " (" + av->qualifier + ")"));
                if (av->from || av->to) infoRow("Valid", hhmm(av->from) + " - " + hhmm(av->to));
                if (av->baseFt >= 0 || av->topFt >= 0) infoRow("Levels", levelText(av->baseFt) + " - " + levelText(av->topFt));
                if (!av->movement.empty()) infoRow("Movement", av->movement);
                ImGui::EndTable();
            }
            infoText(f, av->raw, cMetar);
        } else if (wp) {
            float dir = 0, kt = 0;
            windOf(*wp, dir, kt);
            const float ft = mapAlt().shown;
            title(wp->surface ? "Surface wind  " + wp->id : (composite() && wp->levelFt > 0 ? "Strongest wind  " + altLabel(wp->levelFt) : "Winds aloft  " + altLabel(ft)), IM_COL32(150, 200, 255, 255));
            if (ImGui::BeginTable("wa", 2, ImGuiTableFlags_SizingFixedFit)) {
                infoRow("Source", wp->surface ? "METAR " + wp->id + " (true north)" : (wp->model ? std::string("GFS model (Open-Meteo)") : "NOAA winds aloft (FD), station " + wp->id));
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); muted("Wind"); ImGui::TableSetColumnIndex(1);
                if (kt < 2.5f) ImGui::TextUnformatted("calm");
                else {
                    // true or magnetic, swapped (with the airports' cards and popups) by a click on the small box
                    const bool mag = cardWindMagnetic();
                    const int dm = toMagnetic(dir, magVar(d, wp->lat, wp->lon)), dt = (int)std::lround(dir) == 0 ? 360 : (int)std::lround(dir);
                    ImGui::Text(mag ? "%03d°M / %.0f kt%s" : "%03d°T / %.0f kt%s", mag ? dm : dt, kt, wp->gustKt > kt + 1 ? fmt(" G%.0f", wp->gustKt).c_str() : "");
                    ImGui::SameLine(0, 8);
                    magBox(fmt(mag ? "%03d°T" : "%03d°M", mag ? dt : dm).c_str(), IM_COL32(196, 200, 207, 255));   // set by the map's unit switch
                }
                int pn = 0, fu = 0, ha = 0;
                barbCounts(kt, pn, fu, ha);
                std::string dec;
                auto add = [&](int n, const char* one, const char* many, int each) { if (!n) return; if (!dec.empty()) dec += " + "; dec += fmt("%d %s (%d kt)", n, n == 1 ? one : many, n * each); };
                add(pn, "pennant", "pennants", 50); add(fu, "full barb", "full barbs", 10); add(ha, "half barb", "half barbs", 5);
                infoRow("Barb", kt < 2.5f ? std::string("circle = calm") : dec + fmt(" = %d kt", pn * 50 + fu * 10 + ha * 5));
                infoRow("Staff", kt < 2.5f ? std::string("—") : fmt("points into the wind: from %03.0f°", dir));
                if (wp->hasT && wp->surface) infoRow("Temperature", tempText(wp->tempC));
                else if (wp->hasT) {
                    infoRow("Temperature", tempText(wp->tempC));
                    const float dev = wp->tempC - isaTempC((composite() && wp->levelFt > 0 ? wp->levelFt : ft) * 0.3048f);
                    { const long dv = isaDevIn(dev); infoRow("vs standard", dv == 0 ? std::string("ISA 0") : fmt("ISA %+ld %s", dv, tempUnit())); }
                }
                if (d.havePos && kt >= 2.5f) {
                    double head = 0, cross = 0;
                    windComponents(dir, kt, d.headingDeg, head, cross);
                    infoRow("On your heading", fmt("%s %.0f kt, crosswind %.0f kt", head >= 0 ? "headwind" : "tailwind", std::fabs(head), std::fabs(cross)));
                }
                if (d.havePos) infoRow("From you", fmt("%.0f nm", distNm(d.lat, d.lon, wp->lat, wp->lon)));
                ImGui::EndTable();
            }
            if (composite() && !wp->levels.empty()) {   // the whole column, highest first, the strongest level marked
                ImGui::Spacing();
                muted("All levels (true)");
                ImGui::PushFont(f.mono);
                for (int i = (int)wp->levels.size() - 1; i >= 0; --i) {
                    const AloftLevel& L = wp->levels[(size_t)i];
                    if (!L.hasWind) continue;
                    const bool top = std::fabs(L.altFt - wp->levelFt) < 1;
                    ImGui::PushStyleColor(ImGuiCol_Text, v4(top ? IM_COL32(255, 196, 70, 255) : cSoft));
                    ImGui::Text("%-7s %03.0f/%3.0f kt%s", altLabel((float)L.altFt).c_str(), L.dirDeg, L.speedKt, L.hasTemp ? fmt("  %4.0f%s", tempIn(L.tempC), tempUnit()).c_str() : "");
                    ImGui::PopStyleColor();
                }
                ImGui::PopFont();
            }
        }
        sel.lastSize = ImGui::GetWindowSize();
        sel.size = sel.lastSize;
    }
    ImGui::End();
    // Clipped to the map, whatever happens in between: every draw command of the window (its background and border included) is
    // cut at the map's edge, so not one pixel can show outside the map in any frame, even the frame the map or the window changes
    // size (on load-in the window could still flicker over the edge for a frame, 2026-11-12). While it is being measured nothing
    // of it is drawn at all (the colours drawn straight to its draw list ignored the zero alpha).
    if (ImGuiWindow* w = ImGui::FindWindowByName(winName.c_str()))
        for (ImDrawCmd& c : w->DrawList->CmdBuffer) {
            if (measuring) { c.ClipRect = ImVec4(mn.x, mn.y, mn.x, mn.y); continue; }
            c.ClipRect.x = std::max(c.ClipRect.x, mn.x); c.ClipRect.y = std::max(c.ClipRect.y, mn.y);
            c.ClipRect.z = std::min(c.ClipRect.z, mx.x); c.ClipRect.w = std::min(c.ClipRect.w, mx.y);
            if (c.ClipRect.z < c.ClipRect.x) c.ClipRect.z = c.ClipRect.x;
            if (c.ClipRect.w < c.ClipRect.y) c.ClipRect.w = c.ClipRect.y;
        }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return true;
}

// The open info windows, as many as fit: placed oldest first, each clear of the windows already placed and of every window's point.
// Windows already on their spot (placed once when they opened, or dragged by hand) go first and stay put. When the newest finds no room, it does not open ("MAX AMOUNT
// OF POPUPS" shows over the map for a moment).
void infoWindow(const UiData& d, const UiFonts& f) {
    auto& v = mapSel().items;
    if (v.empty()) return;
    std::vector<size_t> order;
    for (size_t i = 0; i < v.size(); ++i) if (v[i].dragged || v[i].fixed) order.push_back(i);   // the ones already on their spot first
    for (size_t i = 0; i < v.size(); ++i) if (!v[i].dragged && !v[i].fixed) order.push_back(i);
    std::vector<Rect> placed = mapSel().keepOut;   // the map's buttons first: no window may touch them, like the other windows
    bool noRoom = false;
    for (size_t i : order) {
        std::vector<ImVec2> pts;
        for (size_t j = 0; j < v.size(); ++j) if (j != i) pts.push_back(v[j].at);
        // kept off: the map's buttons, the windows already placed this frame, and the ones still to come that already have their
        // spot (a window dragged first in the frame was only kept off the ones before it, and could be pushed onto a later one,
        // which then found no room and closed with "max popups")
        std::vector<Rect> others = placed;
        bool after = false;
        for (size_t j : order) {
            if (j == i) { after = true; continue; }
            if (after && v[j].shown && (v[j].fixed || v[j].dragged)) others.push_back({v[j].pos, ImVec2(v[j].pos.x + v[j].lastSize.x, v[j].pos.y + v[j].lastSize.y)});
        }
        if (!infoWindowOne(d, f, v[i], others, pts)) { v[i].shown = false; noRoom = true; continue; }
        v[i].shown = v[i].kind != 0;
        if (v[i].shown) placed.push_back({v[i].pos, ImVec2(v[i].pos.x + v[i].lastSize.x, v[i].pos.y + v[i].lastSize.y)});
    }
    if (noRoom && v.size() > 1) {   // no room for the newest without overlapping: it does not open, and the map says so for a second
        v.back().kind = 0;
        popupFullUntil() = ImGui::GetTime() + 1.2;
    }
    v.erase(std::remove_if(v.begin(), v.end(), [](const SelItem& it) { return it.kind == 0; }), v.end());
    {   // the UI test's view: the windows shown and whether any of them touch (each other, or a button of the map)
        std::vector<Rect> w;
        for (const auto& it : v) if (it.shown && it.fixed) w.push_back({it.pos, ImVec2(it.pos.x + it.lastSize.x, it.pos.y + it.lastSize.y)});
        auto touch = [](const Rect& a, const Rect& b) { return a.first.x < b.second.x && a.second.x > b.first.x && a.first.y < b.second.y && a.second.y > b.first.y; };
        int t = 0;
        for (size_t i = 0; i < w.size(); ++i) {
            for (size_t j = i + 1; j < w.size(); ++j) if (touch(w[i], w[j])) ++t;
            for (const Rect& k : mapSel().keepOut) if (touch(w[i], k)) ++t;
        }
        g_testPopups = (int)w.size(); g_testPopupTouches = t;
        for (size_t i = 0; i < w.size() && i < 8; ++i) { g_testPopupRect[i][0] = w[i].first.x; g_testPopupRect[i][1] = w[i].first.y; g_testPopupRect[i][2] = w[i].second.x; g_testPopupRect[i][3] = w[i].second.y; }
        g_testKeepOuts = (int)std::min<size_t>(8, mapSel().keepOut.size());
        for (int i = 0; i < g_testKeepOuts; ++i) { const Rect& k = mapSel().keepOut[(size_t)i]; g_testKeepOut[i][0] = k.first.x; g_testKeepOut[i][1] = k.first.y; g_testKeepOut[i][2] = k.second.x; g_testKeepOut[i][3] = k.second.y; }
    }
}

void radarLegend() {
    const float vals[] = {15, 25, 35, 45, 55};
    const char* labels[] = {"15", "25", "35", "45", "55 dBZ"};
    for (int k = 0; k < 5; ++k) {
        if (k) ImGui::SameLine(0, 10);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.3f), ImVec2(p.x + 20, p.y + h * 0.7f), dbzColor(vals[k], 255), 2);
        ImGui::Dummy(ImVec2(22, h));
        ImGui::SameLine(0, 4);
        muted(labels[k]);
    }
}

std::string radarHeader(const UiData& d) {
    const double la = d.havePos ? d.lat : (d.res ? d.res->centerLat : 0), lo = d.havePos ? d.lon : (d.res ? d.res->centerLon : 0);
    const std::string reg = radarRegion(la, lo);
    return reg.empty() ? std::string("Map") : "Map · " + reg;
}

std::string radarSubtitle(const UiData& d) {
    if (!d.res) return "";
    const RadarResult& r = d.res->radar;
    if (!d.cfg || !d.cfg->radar) return "Radar is off";
    if (r.ok) {
        std::string s = r.source;
        if (r.dataTime > 0) s += fmt(" · valid %s · %ld min old", hhmm(r.dataTime).c_str(), (long)((d.now - r.dataTime) / 60));
        return s;
    }
    const double la = d.havePos ? d.lat : d.res->centerLat, lo = d.havePos ? d.lon : d.res->centerLon;
    if (radarRegion(la, lo).empty()) return "No radar composite covers this area: thunderstorms come from the METARs (TS, CB).";
    std::string n = r.note;
    const std::string pre = "European radar (OPERA): ";
    if (n.compare(0, pre.size(), pre) == 0) n = n.substr(pre.size());
    return radarProvider(radarRegion(la, lo)) + " unavailable: " + (n.empty() ? std::string("not fetched yet") : n);
}

void statBox(const UiFonts& f, const char* label, const std::string& value, const std::string& sub, float w, ImU32 valueCol = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cInset));
    ImGui::BeginChild(label, ImVec2(w, sub.empty() ? 62 : 80), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    muted(label);
    ImGui::PushFont(f.title);
    if (valueCol) ImGui::PushStyleColor(ImGuiCol_Text, v4(valueCol));
    ImGui::TextUnformatted(value.c_str());
    if (valueCol) ImGui::PopStyleColor();
    ImGui::PopFont();
    if (!sub.empty()) muted(sub.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}



// A row of small buttons that wraps onto a new line instead of running past the panel's edge: call before each button but the first
void wrapBefore(const char* label, float gap) {
    const float w = ImGui::CalcTextSize(label).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(0, gap);
    if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
}

void toggleButton(const char* label, bool& v) {
    // the colour is pushed and popped on the state from before the click: flipping `v` in between left one push unmatched for a
    // frame, and the buttons after it (Center on aircraft, North Lock) flickered in the wrong colour
    const bool on = v;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, v4(IM_COL32(43, 69, 102, 255)));
    if (ImGui::SmallButton(label)) v = !v;
    if (on) ImGui::PopStyleColor();
}

void layerButtons(bool lockButton) {
    toggleButton("Map", showMap());
    wrapBefore("Radar", 4);
    toggleButton("Radar", showRadar());
    wrapBefore("Airports", 4);
    toggleButton("Airports", showAirports());
    wrapBefore("Turb", 4);
    toggleButton("Turb", showTurb());
    wrapBefore("Winds", 4);
    toggleButton("Winds", showWinds());
    wrapBefore("Clouds", 4);
    toggleButton("Clouds", showClouds());
    wrapBefore("Time Zone", 4);
    toggleButton("Time Zone", showTimeZones());
    wrapBefore("PIREPs", 4);
    toggleButton("PIREPs", showPireps());
    wrapBefore("AIRMET", 4);
    toggleButton("AIRMET", showAirmets());
    wrapBefore("SIGMET", 4);
    toggleButton("SIGMET", showSigmets());
    (void)lockButton;   // Center on aircraft and North / Heading Lock are bubbles on the map itself (radarCanvas)
}

// The range: the mouse wheel over the map, or these buttons (VR, no wheel)
// The zoom: - and + with the range between them, in one bubble like the unit switches (all on one text-high row)
float rangeButtonsWidth() {
    const ImGuiStyle& st = ImGui::GetStyle();
    return 6 + (ImGui::CalcTextSize("-").x + 2 * st.FramePadding.x) + 6 + ImGui::CalcTextSize("320 nm").x + 6 + (ImGui::CalcTextSize("+").x + 2 * st.FramePadding.x) + 6;
}
void rangeButtons() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float boxW = rangeButtonsWidth(), boxH = ImGui::GetTextLineHeight() + 4;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + boxW, p0.y + boxH), IM_COL32(30, 32, 37, 255), boxH * 0.5f);
    dl->AddRect(p0, ImVec2(p0.x + boxW, p0.y + boxH), IM_COL32(58, 62, 70, 255), boxH * 0.5f);
    const float bm = ImGui::CalcTextSize("-").x + 2 * st.FramePadding.x, bp = ImGui::CalcTextSize("+").x + 2 * st.FramePadding.x;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 2));
    if (ImGui::SmallButton("-##zoom")) radarRange() = std::max(0, radarRange() - 1);
    const std::string t = fmt("%g nm", rangeNmOf(radarRange()));
    const float tw = ImGui::CalcTextSize("320 nm").x, tx = p0.x + 6 + bm + 6 + (tw - ImGui::CalcTextSize(t.c_str()).x) * 0.5f;
    dl->AddText(ImVec2(tx, p0.y + 2), cText, t.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x + boxW - 6 - bp, p0.y + 2));
    if (ImGui::SmallButton("+##zoom")) radarRange() = std::min(kRangeCount - 1, radarRange() + 1);
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(boxW, boxH));
}

// The map's altitude slider (beside the Map tab's map): for the turbulence, cloud and wind layers; greyed out without them
void altitudeSlider(const UiData& d, float w, float h) {
    MapAlt& a = mapAlt();
    const bool off = !altLayersOn();
    if (off) ImGui::BeginDisabled();
    ImGui::BeginGroup();
    const float bh = ImGui::GetFrameHeight();
    // ACFT and SFC: on/off switches; switching one off goes back to the altitude chosen with the slider
    auto modeButton = [&](const char* label, AltMode m, const char*) {
        const bool on = a.mode == m;
        ImGui::PushStyleColor(ImGuiCol_Button, v4(on && !off ? IM_COL32(43, 69, 102, 255) : IM_COL32(40, 43, 49, 255)));
        if (ImGui::Button(label, ImVec2(w, bh))) a.mode = on ? AltMode::Chosen : m;
        ImGui::PopStyleColor();
    };
    modeButton("ACFT##alt", AltMode::Acft, "Follow the aircraft's altitude as it climbs and descends (click again: back to the altitude you chose)");
    modeButton("SFC##alt", AltMode::Sfc, "The surface: airport winds (METAR), the lowest clouds (click again: back to the altitude you chose)");
    modeButton("COMP##alt", AltMode::Comp, "Composite: all altitudes at once - every cloud layer together, the worst turbulence in the column, the strongest wind (click again: back to the altitude you chose)");
    bool released = false;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sh = std::max(60.0f, h - 3 * bh - 3 * ImGui::GetStyle().ItemSpacing.y);
    // scale marks every 10,000 ft on the right of the track; the mark the slider sits on lights amber
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int k = 0; k <= 6; ++k) {
        const float y = p.y + 7 + (sh - 14) * (1.0f - k / 6.0f);
        const bool here = !off && a.mode != AltMode::Comp && k > 0 && std::fabs(a.shown - k * 10000.0f) < 50.0f;
        const ImU32 col = here ? cWarn : cIdle;
        dl->AddLine(ImVec2(p.x + w * 0.5f + 5, y), ImVec2(p.x + w * 0.5f + (here ? 12 : 10), y), here ? cWarn : cRing, here ? 2.0f : 1.0f);
        const std::string t = k == 0 ? std::string("0") : fmt("%d", k * 10);
        dl->AddText(here ? ImGui::GetFont() : ImGui::GetFont(), ImGui::GetFontSize() * (here ? 0.8f : 0.72f), ImVec2(p.x + w * 0.5f + 13, y - ImGui::GetFontSize() * (here ? 0.4f : 0.36f)), col, t.c_str());
    }
    float tgt = a.target < 0 ? 0.0f : a.target;
    const std::string label = a.mode == AltMode::Sfc ? std::string("SFC") : (a.mode == AltMode::Comp ? std::string("ALL") : altLabel(a.shown));
    // dragging (or the wheel) chooses an altitude: ACFT / SFC end there
    const bool on10k = !off && a.mode != AltMode::Comp && a.shown > 50.0f && std::fabs(a.shown - std::round(a.shown / 10000.0f) * 10000.0f) < 50.0f;   // a 10,000 ft level: amber
    if (smoothSlider("##mapalt", a.shown, tgt, 0.0f, kMapAltMax, ImVec2(w, sh), true, label, released, off, 1000.0f, 300.0f, altSnap, on10k ? cWarn : 0) || released) { a.mode = AltMode::Chosen; a.chosen = tgt; a.target = tgt; }
    g_testAltBoxLeft = s_lastValueBoxLeft;
    (void)d;
    ImGui::EndGroup();
    if (off) ImGui::EndDisabled();
}

void radarPanel(const UiData& d, const UiFonts& f, bool big) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    sectionTitle(f, radarHeader(d).c_str());
    if (big) {   // the Map tab: the layer buttons, with the range on their line (the units are bubbles on the map itself)
        layerButtons(true);
        ImGui::SameLine(right - rangeButtonsWidth());
        rangeButtons();
    } else {     // the home page's map shows the Map tab's layers, without buttons of its own
        ImGui::SameLine(right - rangeButtonsWidth());
        rangeButtons();
    }
    mutedWrapped(radarSubtitle(d));
    // the slider's value box sits left of its knob (knob radius 7 + 6 + text + 8 from the track's centre): the gap leaves room for the
    // widest value ("00000 ft"; FL, SFC and ALL are narrower) plus 6 px, so the box never covers the map
    const float sliderW = big ? 50.0f : 0.0f;
    const float gap = big ? std::max(10.0f, 7 + 6 + ImGui::CalcTextSize("00000 ft").x + 8 + 6 - sliderW * 0.5f) : 0.0f;
    const float avail = ImGui::GetContentRegionAvail().x - sliderW - gap;
    const float hLeft = ImGui::GetContentRegionAvail().y - (big ? 40 : 84);   // (home: the legend and the three storm figures under it)
    const float size = std::max(120.0f, std::min(avail, hLeft));
    if (size < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size) * 0.5f);
    if (big) g_testMapRight = ImGui::GetCursorScreenPos().x + size;
    radarCanvas(d, f, size, rangeNmOf(radarRange()), big);
    if (big) {   // the altitude slider beside the map, as tall as the map
        ImGui::SameLine(0, gap);
        altitudeSlider(d, sliderW, size);
    }
    radarLegend();
    ImGui::SameLine(0, 14);
    muted(big ? "Scroll: zoom · drag: move · click an airport, report or area for its details" : "Scroll: zoom");
    if (showTurb() && d.turbGrid.empty()) { ImGui::SameLine(0, 12); muted(d.turbOn ? "no turbulence data yet" : "turbulence off"); }
    if (!big && d.res) {
        const RadarResult& r = d.res->radar;
        int cb = 0; double nearest = -1;
        for (const auto& k : r.cells) {
            if (k.dbz >= 40) ++cb;
            const double dd = d.havePos ? distNm(d.lat, d.lon, k.lat, k.lon) : k.distNm;
            if (k.dbz >= 15 && (nearest < 0 || dd < nearest)) nearest = dd;
        }
        const float bw = (ImGui::GetContentRegionAvail().x - 16) / 3.0f;
        statBox(f, "Storm cells", r.ok ? fmt("%zu", r.cells.size()) : std::string("—"), "", bw);
        ImGui::SameLine(0, 8);
        statBox(f, "40+ dBZ", r.ok ? fmt("%d", cb) : std::string("—"), "", bw);
        ImGui::SameLine(0, 8);
        statBox(f, "Nearest echo", nearest < 0 ? std::string("—") : fmt("%.0f nm", nearest), "", bw);
    }
}

// The home page's pilot reports: their own panel under the map (every one, nearest first, scrolls like the airport list)
void homePireps(const UiData& d, const UiFonts& f) {
    sectionTitle(f, "Pilot reports");
    size_t shown = 0;   // the ones the list shows (a report that says nothing is not listed)
    if (d.reports) for (const auto& q : d.reports->pireps) if (pirepListed(q)) ++shown;
    if (d.reports) { ImGui::SameLine(); muted(fmt("%zu", shown).c_str()); }   // how many, beside the title
    // whole rows only: the list is as tall as its rows (all of them when they fit; otherwise as many whole rows as fit, with a
    // scroll bar beside them), so no half-cut row hangs at the bottom of the panel
    // the real header height and row step, measured the last time it was drawn (the first frame: an estimate)
    const RowMetrics& rm = pirepMetrics("homepireps");
    const float est = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
    const float rowH = rm.step > 1 ? rm.step : est, headH = rm.head > 1 ? rm.head : est;
    const float avail = ImGui::GetContentRegionAvail().y - 2;
    const int fitRows = std::max(1, (int)std::floor((avail - headH - 1) / rowH));
    if (!d.reports || shown == 0) mutedWrapped("No pilot reports in the last 90 minutes.");
    else if (avail > 40) { static std::vector<std::string> open; pirepTable(d, f, "homepireps", ImVec2(0, -(float)std::min<int>(fitRows, (int)shown)), open, 1); }
}


// ---- pilot reports, as a list (home page and Turbulence tab) ---------------------------------------------------------------------
// What a report says, short, whatever kind it is: turbulence, icing, wind shear, weather, sky, and failing those its wind/temperature
std::string pirepConditions(const hz::Pirep& p) {
    std::string c;
    auto add = [&](const std::string& t) { if (t.empty()) return; if (!c.empty()) c += " · "; c += t; };
    if (!p.intensity.empty()) add(p.intensity + (p.tbType.empty() ? "" : " " + p.tbType));
    if (!p.icing.empty()) add("ice " + p.icing);
    if (p.llws) add("LLWS");
    if (!p.wx.empty()) add(p.wx);
    if (!p.sky.empty()) add(p.sky);
    if (c.empty() && p.windKt >= 0) add("wind " + pirepWind(p));
    if (c.empty() && p.tempC > -100) add("temp " + tempText(p.tempC));
    return c.empty() ? std::string("—") : c;
}
// A report the lists show: it says something (turbulence, icing, weather, clouds, wind or temperature) and has a place. Ones that
// only carry a remark or nothing at all made rows of dashes ("blank PIREP rows", 2026-11-12).
bool pirepListed(const hz::Pirep& p) {
    if (std::fabs(p.lat) > 90 || std::fabs(p.lon) > 360) return false;
    const std::string c = pirepConditions(p);
    if (c == "—") return false;
    for (char ch : c) if (!std::isspace((unsigned char)ch) && ch != '-' && ch != '/' && ch != '.') return true;
    return false;
}
// how much attention a report needs (colour and the Conditions sort): turbulence by EDR, icing and wind shear count too
double pirepSeverity(const hz::Pirep& p) {
    double e = std::max(0.0, hz::edrForIntensity(p.intensity, p.acType));
    if (!p.icing.empty()) e = std::max(e, p.icing.find("SEV") != std::string::npos ? 0.45 : (p.icing.find("MOD") != std::string::npos ? 0.25 : 0.12));
    if (p.llws) e = std::max(e, 0.3);
    return e;
}
enum PirepSort { kPrDist = 1, kPrLevel, kPrType, kPrCond, kPrAcft, kPrAge };
// The sortable list: a click on a column header sorts it (again: the other way, a third time: back to nearest first); equal values
// are listed nearest first. A click on a row opens its details under it (another click closes them).
void pirepTable(const UiData& d, const UiFonts& f, const char* id, ImVec2 size, std::vector<std::string>& open, int mode) {
    const bool compact = mode > 0;
    const float tableTop = ImGui::GetCursorScreenPos().y;
    float row0 = -1;
    // size.y < 0: a page of -size.y whole rows, no scroll area (a scroll area always showed a cut row at its bottom edge, read as a
    // blank row on the home page): the mouse wheel over it turns the page a row at a time
    const int pageRows = size.y < 0 ? (int)std::lround(-size.y) : 0;
    static std::map<std::string, int> firstOf;
    int& first = firstOf[id];
    if (pageRows) size.y = 0;
    size_t total = 0;   // (the list's length, for the page's scroll bar)
    if (d.reports) for (const auto& q : d.reports->pireps) if (pirepListed(q)) ++total;
    const bool pageBar = pageRows > 0 && (int)total > pageRows;
    const float sbW = ImGui::GetStyle().ScrollbarSize;
    if (pageBar) size.x = ImGui::GetContentRegionAvail().x - sbW - 2;   // room for the scroll bar on the right, like the airport list's
    const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | (pageRows ? 0 : ImGuiTableFlags_ScrollY) | ImGuiTableFlags_PadOuterX |
                               ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_NoClip;
    const float tableRight = ImGui::GetCursorScreenPos().x + (size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x);
    if (!ImGui::BeginTable(id, 6, fl, size)) return;
    ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, textColW("000"), kPrDist);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, textColW("FL000"), kPrLevel);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, textColW("UUA"), kPrType);
    // the conditions take the rest (compact lists leave out the aircraft and shorten the ages, so the header "Conditions" fits;
    // it was cut to "Conditi..." on the home page)
    ImGui::TableSetupColumn("Conditions", ImGuiTableColumnFlags_WidthStretch, 1.0f, kPrCond);
    ImGui::TableSetupColumn("Acft", ImGuiTableColumnFlags_WidthFixed | (compact ? ImGuiTableColumnFlags_Disabled | ImGuiTableColumnFlags_DefaultHide : 0), textColW("B77W"), kPrAcft);
    ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, textColW(compact ? "00m" : "00 min"), kPrAge);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    std::vector<const hz::Pirep*> v;
    // reports that say nothing (no turbulence, icing, weather, clouds, wind or temperature: an empty line of dashes) are left out
    if (d.reports) for (const auto& q : d.reports->pireps) if (pirepListed(q)) v.push_back(&q);
    auto dd = [&](const hz::Pirep* q) { return d.havePos ? distNm(d.lat, d.lon, q->lat, q->lon) : 0.0; };
    int col = kPrDist; bool desc = false;
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())
        if (sp->SpecsCount > 0) { col = (int)sp->Specs[0].ColumnUserID; desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending; }
    auto key = [&](const hz::Pirep* q) -> double {
        switch (col) {
            case kPrLevel: return q->altFt;
            case kPrType: return q->urgent ? 0 : (q->kind == "AIREP" ? 2 : 1);
            case kPrCond: return pirepSeverity(*q);
            case kPrAge: return q->obsTime > 0 ? (double)(d.now - q->obsTime) : 1e9;
            default: return dd(q);
        }
    };
    std::stable_sort(v.begin(), v.end(), [&](const hz::Pirep* a, const hz::Pirep* b) {
        if (col == kPrAcft && a->acType != b->acType) return desc ? a->acType > b->acType : a->acType < b->acType;
        const double ka = key(a), kb = key(b);
        if (col != kPrAcft && ka != kb) return desc ? ka > kb : ka < kb;
        return dd(a) < dd(b);   // ties: nearest first
    });
    if (pageRows) first = std::max(0, std::min(first, (int)v.size() - pageRows));
    const size_t i0 = pageRows ? (size_t)first : 0, i1 = pageRows ? std::min(v.size(), i0 + (size_t)pageRows) : std::min<size_t>(v.size(), 200);
    for (size_t i = i0; i < i1; ++i) {
        const hz::Pirep& p = *v[i];
        const std::string k = pirepKey(p);
        const auto it = std::find(open.begin(), open.end(), k);
        const bool isOpen = it != open.end();
        ImGui::PushID(k.c_str());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        {   // measure: where the first row starts (below the header), and the step to the next
            const float y = ImGui::GetCursorScreenPos().y + ImGui::GetScrollY();
            if (i == i0) { row0 = y; pirepMetrics(id).head = y - ImGui::GetStyle().CellPadding.y - tableTop; }
            else if (i == i0 + 1 && row0 >= 0 && !(std::find(open.begin(), open.end(), pirepKey(*v[i0])) != open.end())) pirepMetrics(id).step = y - row0;
        }
        if (ImGui::Selectable("##row", mode != 2 && isOpen, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
            if (mode == 2) {   // to the report on the map, and its window
                if (d.havePos) { panGoal() = ImVec2((float)((p.lon - d.lon) * 60.0 * std::cos(d.lat * kPi / 180.0)), (float)((p.lat - d.lat) * 60.0)); panGliding() = true; recentering() = false; }
                showPireps() = true;
                pendingPirep() = k;
            } else if (isOpen) open.erase(it); else open.push_back(k);
        }
        if (mode == 2 && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SameLine(0, 0); ImGui::Text("%.0f", dd(&p));
        ImGui::TableSetColumnIndex(1); if (p.altFt > 0) ImGui::Text("FL%03.0f", p.altFt / 100.0); else muted("—");
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(p.urgent ? IM_COL32(235, 64, 64, 255) : IM_COL32(240, 150, 40, 255)));
        ImGui::TextUnformatted(p.urgent ? "UUA" : (p.kind == "AIREP" ? "ARP" : "UA"));
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3);
        {   // one line: clipped to the column
            const double e = pirepSeverity(p);
            const std::string c = pirepConditions(p);
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            const float cw = ImGui::GetContentRegionAvail().x;
            ImGui::PushClipRect(cp, ImVec2(cp.x + cw, cp.y + ImGui::GetTextLineHeight() + 2), true);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(e >= 0.22 ? cWarn : (c == "—" ? cMuted : cSoft)));
            ImGui::TextUnformatted(c.c_str());
            ImGui::PopStyleColor();
            ImGui::PopClipRect();
        }
        if (ImGui::TableSetColumnIndex(4)) ImGui::TextUnformatted(p.acType.empty() ? "—" : p.acType.c_str());
        ImGui::TableSetColumnIndex(5); if (p.obsTime > 0) ImGui::Text(compact ? "%ldm" : "%ld min", (long)((d.now - p.obsTime) / 60)); else muted("—");
        if (isOpen && mode != 2) {   // the details, across the row, until the row is clicked again
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(12);
            std::string det;
            if (p.obsTime > 0) det += "Time " + hhmm(p.obsTime) + "    ";
            if (!p.intensity.empty()) det += "Turbulence " + p.intensity + (p.tbType.empty() ? "" : " " + p.tbType) + ((p.baseFt > 0 || p.topFt > 0) ? " " + levelText(p.baseFt) + "-" + levelText(p.topFt) : "") + "    ";
            if (!p.icing.empty()) det += "Icing " + p.icing + "    ";
            if (!p.sky.empty()) det += "Clouds " + p.sky + "    ";
            if (!p.wx.empty()) det += "Weather " + p.wx + "    ";
            if (p.tempC > -100) det += "Temp " + tempText(p.tempC) + "    ";
            if (p.windKt >= 0) det += "Wind " + pirepWind(p) + "    ";
            if (p.llws) det += "Low-level wind shear reported";
            // the details span the whole table width (the table does not clip its cells)
            const float tw = tableRight - ImGui::GetCursorScreenPos().x - 18;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.0f, tw));
            if (!det.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, v4(cSoft)); ImGui::TextUnformatted(det.c_str()); ImGui::PopStyleColor(); }
            ImGui::PushFont(f.mono);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMetar));
            ImGui::TextUnformatted(p.raw.empty() ? "(no report text)" : p.raw.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
            ImGui::Unindent(12);
            ImGui::Dummy(ImVec2(0, 2));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    const ImVec2 tMin = ImGui::GetItemRectMin(), tMax = ImGui::GetItemRectMax();
    const bool overList = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(tMin, tMax);
    if (pageBar) {   // the scroll bar: a track beside the rows and a thumb as long as the page; drag it, click the track, or wheel
        const float head = pirepMetrics(id).head > 1 ? pirepMetrics(id).head : ImGui::GetTextLineHeight() + 4;
        const ImVec2 a(tMax.x + 2, tMin.y + head), b(tMax.x + 2 + sbW, tMax.y);
        const int n = (int)v.size(), maxFirst = std::max(1, n - pageRows);
        const float trackH = b.y - a.y, thumbH = std::max(18.0f, trackH * (float)pageRows / (float)n);
        const float ty = a.y + (trackH - thumbH) * (float)first / (float)maxFirst;
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##pagebar", ImVec2(sbW, std::max(1.0f, trackH)));
        const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        if (act) {
            const float my = ImGui::GetIO().MousePos.y - a.y - thumbH * 0.5f;
            first = (int)std::lround(std::max(0.0f, std::min(1.0f, my / std::max(1.0f, trackH - thumbH))) * maxFirst);
        }
        ImDrawList* sd = ImGui::GetWindowDrawList();
        sd->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ScrollbarBg), sbW * 0.5f);
        sd->AddRectFilled(ImVec2(a.x + 2, ty + 2), ImVec2(b.x - 2, ty + thumbH - 2),
                          ImGui::GetColorU32(act ? ImGuiCol_ScrollbarGrabActive : (hov ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab)), sbW * 0.5f);
        if (hov && ImGui::GetIO().MouseWheel != 0.0f) first -= (int)std::lround(ImGui::GetIO().MouseWheel);
    }
    if (pageRows && overList && ImGui::GetIO().MouseWheel != 0.0f) first -= (int)std::lround(ImGui::GetIO().MouseWheel);   // a row at a time
    if (pageRows) first = std::max(0, std::min(first, (int)v.size() - pageRows));
    pirepMetrics(id).shownFrom = (int)i0; pirepMetrics(id).shownTo = (int)i1;
}

struct AloftRow { float ft; float dir, kt; float tempC; bool hasT; };

bool aloftAt(const UiData& d, std::vector<AloftRow>& rows, float& tropoFt, float& tropoC, const std::vector<float>& levelsFt) {
    rows.clear(); tropoFt = -1; tropoC = 0;
    const Station* st = nearestStation(d);
    if (!st || !d.cfg || !d.res || d.res->aloft.empty()) return false;
    XpWx x = buildXp(*st, *d.cfg, d.now, &d.res->aloft, true);
    if (x.winds.size() < 2) return false;
    for (float ft : levelsFt) {
        const float m = ft * 0.3048f;
        const XpWind w = windAt(x.winds, m);
        AloftRow r{ft, w.dir, w.ms / 0.514444f, 0, false};
        if (!x.tempProfile.empty()) { r.tempC = profileTemp(x.tempProfile, m); r.hasT = true; }
        rows.push_back(r);
    }
    float ta = 0, tt = 0;
    if (findTropopause(x.tempProfile, ta, tt)) { tropoFt = ta / 0.3048f; tropoC = tt; }
    return true;
}

// points where the wind goes (from dirDeg towards dirDeg+180). North Lock: north is up, in white; Heading Lock: the aircraft's heading
// is up (like the wind dial), in blue (as the dial's lock is lit blue)
void windArrow(float dirDeg, float size, float upDeg = 0.0f, bool headingUp = false) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 c(p.x + size * 0.5f, p.y + h * 0.5f);
    const double a = (dirDeg + 180.0 - upDeg) * kPi / 180.0;
    const ImU32 col = headingUp ? IM_COL32(127, 182, 255, 255) : IM_COL32(236, 239, 244, 255);
    const ImVec2 dir((float)std::sin(a), -(float)std::cos(a)), n(-dir.y, dir.x);
    const float L = size * 0.45f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tip(c.x + dir.x * L, c.y + dir.y * L), tail(c.x - dir.x * L, c.y - dir.y * L);
    dl->AddLine(tail, tip, col, 1.8f);
    dl->AddLine(tip, ImVec2(tip.x - dir.x * 5 + n.x * 4, tip.y - dir.y * 5 + n.y * 4), col, 1.8f);
    dl->AddLine(tip, ImVec2(tip.x - dir.x * 5 - n.x * 4, tip.y - dir.y * 5 - n.y * 4), col, 1.8f);
    ImGui::Dummy(ImVec2(size, h));
}

void speedBar(float kt, float maxKt) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y0 = p.y + h * 0.5f - 4, y1 = p.y + h * 0.5f + 4;
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y1), cInset, 4);
    const float f = std::max(0.02f, std::min(1.0f, kt / maxKt));
    const ImU32 col = kt >= 80 ? cAccent : (kt >= 50 ? IM_COL32(58, 111, 176, 255) : cAccentDim);
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w * f, y1), col, 4);
    ImGui::Dummy(ImVec2(w, h));
}

void aloftTable(const UiData& d, const UiFonts& f, const std::vector<float>& levels, bool scroll) {
    std::vector<AloftRow> rows;
    float tropoFt = -1, tropoC = 0;
    if (!aloftAt(d, rows, tropoFt, tropoC, levels)) { mutedWrapped(d.cfg && !d.cfg->useAloft ? "Winds aloft are off." : "No winds aloft yet."); return; }
    float maxKt = 100;
    for (const auto& r : rows) maxKt = std::max(maxKt, r.kt);
    const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | (scroll ? ImGuiTableFlags_ScrollY : 0);
    UnitScope us(g_unitsAloft);   // the winds-aloft tables (home page and Winds & Temps tab) have their own units
    if (!ImGui::BeginTable("aloft", 6, fl)) return;
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, textColW("00000 ft"));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18);
    // winds in degrees true or magnetic: a click on a row's wind (or its small box) swaps them for the whole table
    bool& aloftMag = windMagnetic();
    bool flipMag = false, flipTemp = false;
    ImGui::TableSetupColumn(aloftMag ? "Wind °M###w" : "Wind °T###w", ImGuiTableColumnFlags_WidthFixed, textColW("Wind °T  "));
    // the other wind reference (°M / °T box) next to the wind, the temperature / ISA bubble at the end
    ImGui::TableSetupColumn(aloftMag ? "°T###b" : "°M###b", ImGuiTableColumnFlags_WidthFixed, textColW("000°M") + 10);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tempF() ? "Temp / ISA °F###t" : "Temp / ISA °C###t", ImGuiTableColumnFlags_WidthFixed, textColW("-000°F  ISA +00") + 18);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    plainHeaders();
    ImGui::PopStyleColor();
    bool tropoDone = tropoFt < 0;
    const float aloftMagRot = magTurn(1, aloftMag, (float)magVar(d, d.lat, d.lon)) * (1.0f - headingUpness());
    for (int i = (int)rows.size() - 1; i >= 0; --i) {   // highest first, like a sounding
        const AloftRow& r = rows[(size_t)i];
        if (!tropoDone && r.ft < tropoFt) {
            tropoDone = true;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cWarn));
            ImGui::TextUnformatted("Tropo");
            ImGui::TableSetColumnIndex(2);   // its level under the winds, its temperature under the temperatures (fits a narrow table)
            ImGui::Text("FL%03.0f", tropoFt / 100.0f);
            ImGui::TableSetColumnIndex(5);   // with its unit, as plain text (no bubble, no switch)
            ImGui::Text("%.0f %s", tempIn(tropoC), tempUnit());
            ImGui::PopStyleColor();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::PushFont(f.bold); ImGui::Text(r.ft >= 18000 ? "FL%03.0f" : "%.0f ft", r.ft >= 18000 ? r.ft / 100.0f : r.ft); ImGui::PopFont();
        ImGui::TableSetColumnIndex(1);   // turns with the dial's North / Heading Lock (and to magnetic north with °M in North Lock)
        windArrow(r.dir, 16, viewRot() + aloftMagRot, dialHeadingUp() && d.havePos);
        {
            const int dT = (int)std::lround(r.dir) % 360 == 0 ? 360 : (int)std::lround(r.dir) % 360, dM = toMagnetic(r.dir, magVar(d, d.lat, d.lon));
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i);
            const std::string w = fmt("%03d/%.0f", aloftMag ? dM : dT, r.kt);
            ImGui::TextUnformatted(w.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::TableSetColumnIndex(3);
            magBox(fmt(aloftMag ? "%03d°T" : "%03d°M", aloftMag ? dT : dM).c_str(), cMuted);
            ImGui::PopID();
        }
        ImGui::TableSetColumnIndex(4); speedBar(r.kt, maxKt);
        // temperature and ISA deviation in one bubble (like the wind's): a click swaps °C / °F for the whole table
        ImGui::PushID(i + 10000);
        ImGui::TableSetColumnIndex(5);
        if (r.hasT) {
            const long dv = isaDevIn(r.tempC - isaTempC(r.ft * 0.3048f));
            const std::string t = fmt("%.0f%s  ", tempIn(r.tempC), tempUnit()) + (dv == 0 ? std::string("ISA 0") : fmt("ISA %+ld", dv));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cSoft)); ImGui::TextUnformatted(t.c_str()); ImGui::PopStyleColor();   // plain text, no bubble
        } else muted("—");
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (flipMag) aloftMag = !aloftMag;
    if (flipTemp) tempF() = !tempF();
}

std::string turbWord(double edr) {
    if (edr < 0) return "No data";
    if (edr < 0.08) return "None";
    if (edr < 0.15) return "Light";
    if (edr < 0.22) return "Light-moderate";
    if (edr < 0.35) return "Moderate";
    if (edr < 0.5) return "Severe";
    return "Extreme";
}

// One line of the home page's turbulence card: label on the left, value and detail on the right
void statRow(const UiFonts& f, const char* label, const std::string& value, const std::string& sub, ImU32 valueCol = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cInset));
    ImGui::BeginChild(label, ImVec2(0, ImGui::GetFrameHeight() + 22), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    muted(label);
    ImGui::PushFont(f.bold);
    const float vw = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::PopFont();
    const float sw = sub.empty() ? 0 : ImGui::CalcTextSize(sub.c_str()).x + 12;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, right - vw - sw));
    if (!sub.empty()) { muted(sub.c_str()); ImGui::SameLine(0, 12); }
    ImGui::PushFont(f.bold);
    if (valueCol) ImGui::PushStyleColor(ImGuiCol_Text, v4(valueCol));
    ImGui::TextUnformatted(value.c_str());
    if (valueCol) ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void turbulenceCard(const UiData& d, const UiFonts& f, UiActions& act, bool slider) {
    sectionTitle(f, "Turbulence & windshear");
    if (!slider && d.cfg && !d.cfg->realisticTurbulence && !d.cfg->windshear) {   // the home page with both off: say where to switch them on
        const ImVec2 av = ImGui::GetContentRegionAvail();
        const char* t = "Turbulence/Windshear Disabled in Settings.";
        ImGui::PushFont(f.title);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + std::max(0.0f, (av.x - ts.x) * 0.5f), ImGui::GetCursorPosY() + std::max(0.0f, (av.y - ts.y) * 0.4f)));
        if (ts.x > av.x) { ImGui::PushTextWrapPos(0); ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted)); ImGui::TextWrapped("%s", t); ImGui::PopStyleColor(); ImGui::PopTextWrapPos(); }
        else muted(t);
        ImGui::PopFont();
        return;
    }
    if (!d.turbData.empty()) { ImGui::SameLine(); muted(d.turbData.c_str()); }
    if (slider && !d.turbNote.empty()) {   // an aircraft's own plugin: a note only, WXPlane's turbulence stays on
        ImGui::SameLine(0, 16);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(cWarn));
        ImGui::TextUnformatted(fmt("Note: %s (the aircraft's plugin) may add turbulence effects of its own", d.turbNote.c_str()).c_str());
        ImGui::PopStyleColor();
    }
    if (!slider) {   // the home page: three rows
        int withTurb = 0, total = 0;
        if (d.reports) for (const auto& p : d.reports->pireps) { if (!pirepListed(p)) continue; ++total; if (hz::edrForIntensity(p.intensity, p.acType) > 0) ++withTurb; }
        if (!d.turbConflict.empty()) statRow(f, "At the aircraft", "Other add-on", d.turbConflict + " controls it", cDown);
        else statRow(f, "At the aircraft", d.turbOn ? turbWord(d.turbEdr) : std::string("Off"), d.turbOn && d.turbEdr >= 0 ? fmt("EDR %.2f · %s", d.turbEdr, d.turbSource.c_str()) : std::string());
        (void)total;
        statRow(f, "Pilot reports", d.reports ? fmt("%d", withTurb) : std::string("—"), d.reports ? std::string("with turbulence") : std::string());
        const char* risk = !d.wsOn ? "Off" : (d.wsHigh > 0 ? "High" : (d.wsMed > 0 ? "Medium" : (d.wsSources > 0 ? "Low" : "None")));
        const ImU32 rc = !d.wsOn ? cMuted : (d.wsHigh > 0 ? IM_COL32(245, 70, 70, 255) : (d.wsMed > 0 ? IM_COL32(245, 150, 50, 255) : (d.wsSources > 0 ? IM_COL32(240, 210, 80, 255) : IM_COL32(255, 255, 255, 255))));
        statRow(f, "Windshear risk", risk, d.wsOn ? fmt("%d source(s)", d.wsSources) : std::string(), rc);
        return;
    }
    const float bw = (ImGui::GetContentRegionAvail().x - 16) / 3.0f;
    int withTurb = 0, total = 0;
    if (d.reports) for (const auto& p : d.reports->pireps) { if (!pirepListed(p)) continue; ++total; if (hz::edrForIntensity(p.intensity, p.acType) > 0) ++withTurb; }
    statBox(f, "At the aircraft", d.turbOn ? turbWord(d.turbEdr) : std::string("Off"), d.turbOn && d.turbEdr >= 0 ? fmt("EDR %.2f · %s", d.turbEdr, d.turbSource.c_str()) : std::string(" "), bw);
    ImGui::SameLine(0, 8);
    statBox(f, "Pilot reports", d.reports ? fmt("%d", total) : std::string("—"), d.reports ? fmt("%d with turbulence", withTurb) : std::string(" "), bw);
    ImGui::SameLine(0, 8);
    const char* risk = !d.wsOn ? "Off" : (d.wsHigh > 0 ? "High" : (d.wsMed > 0 ? "Medium" : (d.wsSources > 0 ? "Low" : "None")));
    const ImU32 riskCol = !d.wsOn ? cMuted : (d.wsHigh > 0 ? IM_COL32(245, 70, 70, 255) : (d.wsMed > 0 ? IM_COL32(245, 150, 50, 255) : (d.wsSources > 0 ? IM_COL32(240, 210, 80, 255) : IM_COL32(255, 255, 255, 255))));
    statBox(f, "Windshear risk", risk, d.wsOn ? fmt("%d source(s)", d.wsSources) : std::string(" "), bw, riskCol);
    if (!slider) return;
    ImGui::Spacing();
    // Max turbulence: the knob glides to where it is clicked or dragged; the plugin follows the value as it moves and saves it when let go
    static float shown = -1, target = -1;
    static bool held = false;
    const float cfgV = d.cfg ? (float)d.cfg->maxTurbulence : 1.0f;
    if (shown < 0 || (!held && std::fabs(target - shown) < 1e-4f && std::fabs(cfgV - target) > 0.005f)) shown = target = cfgV;   // first time, or changed elsewhere
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Max turbulence");
    ImGui::SameLine(130);
    const float before = shown;
    bool released = false;
    const float w = ImGui::GetContentRegionAvail().x - 50;
    held = smoothSlider("##maxturb", shown, target, 0.0f, 1.0f, ImVec2(w, ImGui::GetFrameHeight()), false, "", released, false, 0.1f, 0.025f);   // clicks into tenths
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.2f", shown);
    if (std::fabs(shown - before) > 1e-5f) { act.maxTurbChanged = true; act.maxTurb = shown; }
    if (released) { act.maxTurbReleased = true; act.maxTurb = target; }
    // Windshear odds, the same kind of slider: how often gust fronts and microbursts form (1 = the realistic rate, 0 = none)
    static float wShown = -1, wTarget = -1;
    static bool wHeld = false;
    const float wCfg = d.cfg ? (float)d.cfg->windshearOdds : 1.0f;
    if (wShown < 0 || (!wHeld && std::fabs(wTarget - wShown) < 1e-4f && std::fabs(wCfg - wTarget) > 0.005f)) wShown = wTarget = wCfg;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Windshear odds");
    ImGui::SameLine(130);
    const float wBefore = wShown;
    bool wRel = false;
    wHeld = smoothSlider("##wsodds", wShown, wTarget, 0.0f, 1.0f, ImVec2(w, ImGui::GetFrameHeight()), false, "", wRel, false, 0.1f, 0.025f);
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.2f", wShown);
    if (std::fabs(wShown - wBefore) > 1e-5f) { act.wsOddsChanged = true; act.wsOdds = wShown; }
    if (wRel) { act.wsOddsReleased = true; act.wsOdds = wTarget; }
}

void servicesStrip(const UiData& d, const UiFonts& f) {
    sectionTitle(f, "Data services");
    for (const auto& s : allServices(d)) {
        std::string label = shortService(s);
        std::string detail = s.state == SvcState::NotHere ? std::string("not used here") : (s.state == SvcState::Off ? std::string("off") : s.detail);
        if (detail.size() > 34) detail = detail.substr(0, 32) + "…";
        const float need = 16 + ImGui::CalcTextSize(label.c_str()).x + 6 + ImGui::CalcTextSize(detail.c_str()).x;
        ImGui::SameLine(0, 18);
        if (ImGui::GetContentRegionAvail().x < need) ImGui::NewLine();
        ImGui::BeginGroup();
        statusDot(s.state);
        ImGui::SameLine(0, 2);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(0, 6);
        muted(detail.c_str());
        ImGui::EndGroup();
    }
}


// Sorting of the airport lists: by any column (click its header; again for the other direction), equal values then by ICAO
// (A-Z) or, with that switched off, by distance.
enum SortCol { kSortIcao = 1, kSortDist, kSortCat, kSortWind, kSortVis, kSortWx, kSortClouds, kSortCeil, kSortTemp };

int catRank(const std::string& c) { return c == "VFR" ? 0 : c == "MVFR" ? 1 : c == "IFR" ? 2 : c == "LIFR" ? 3 : 4; }
void sortStations(std::vector<const Station*>& v, const UiData& d) {
    int col = kSortDist;   // not sorted by the user (no arrow shown): nearest first
    bool desc = false;
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())
        if (sp->SpecsCount > 0) { col = (int)sp->Specs[0].ColumnUserID; desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending; }
    auto key = [&](const Station* s) -> double {
        const Report& r = s->report;
        switch (col) {
            case kSortDist: return distTo(d, *s);
            case kSortCat: return catRank(flightCategory(r));
            case kSortWind: return r.hasWind ? std::max(r.windKt, r.gustKt) : -1;
            case kSortVis: return r.cavok ? 99999 : (r.hasVis ? r.visM : -1);
            case kSortCeil: { const int c = ceilingFt(r); return c < 0 ? 1e7 : c; }
            case kSortClouds: { double lo = 1e7; for (const auto& g : r.clouds) lo = std::min(lo, (double)g.baseFtAgl); return lo; }
            case kSortTemp: return r.hasTemp ? r.tempC : -999;
            default: return 0;
        }
    };
    std::stable_sort(v.begin(), v.end(), [&](const Station* a, const Station* b) {
        int c = 0;
        if (col == kSortIcao) c = a->icao.compare(b->icao);
        else if (col == kSortWx) c = a->report.wxText.compare(b->report.wxText);
        else { const double ka = key(a), kb = key(b); c = ka < kb - 1e-9 ? -1 : (ka > kb + 1e-9 ? 1 : 0); }
        if (c != 0) return desc ? c > 0 : c < 0;
        const double da = distTo(d, *a), db = distTo(d, *b);   // equal values: nearest first (then A-Z)
        if (std::fabs(da - db) > 1e-6) return da < db;
        return a->icao < b->icao;
    });
}

void nearbyTable(const UiData& d, const Station* skip, int maxRows, bool scroll) {
    if (!d.res) return;
    std::vector<const Station*> v;
    for (const auto& s : d.res->stations) if (&s != skip && s.icao.compare(0, 3, "GFS") != 0) v.push_back(&s);
    const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | (scroll ? ImGuiTableFlags_ScrollY : 0);
    // no category column: the ICAO itself is coloured by its flight category (VFR green, MVFR blue, IFR red, LIFR magenta)
    if (!ImGui::BeginTable("near", 5, fl)) return;
    // the ICAO header is one button for two orders: A-Z, Z-A, then flight category (VFR first), worst first, then off. While it
    // sorts by category its label reads "Cat", in blue. (It sorts on its own: the other columns' sorting is ignored meanwhile.)
    static int icaoMode = 0;   // 0 off, 1 A-Z, 2 Z-A, 3 category VFR first, 4 category worst first
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, textColW("WWWW") + 6, kSortIcao);
    ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, textColW("000"), kSortDist);
    // the wind gets the room for a gusting wind and its reference (true or magnetic in the header)
    ImGui::TableSetupColumn(windMagnetic() ? "Wind °M###wind" : "Wind °T###wind", ImGuiTableColumnFlags_WidthFixed, textColW("000/00G00"), kSortWind);
    ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, textColW("10+ SM"), kSortVis);
    ImGui::TableSetupColumn("Ceiling", ImGuiTableColumnFlags_WidthStretch, textColW("00,000"), kSortCeil);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < 5; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        if (c == 0) {
            // the label, then a small arrow drawn like the other columns' sort arrows (the font has no arrow characters)
            const char* lbl = icaoMode >= 3 ? "Cat###icao" : "ICAO###icao";
            const ImU32 col = icaoMode >= 3 ? cAccent : cMuted;
            ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
            ImGui::TableHeader(lbl);
            ImGui::PopStyleColor();
            if (icaoMode > 0) {
                const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                const float tw = ImGui::CalcTextSize(icaoMode >= 3 ? "Cat" : "ICAO").x, fs = ImGui::GetFontSize();
                const float cx = r0.x + tw + ImGui::GetStyle().ItemInnerSpacing.x + fs * 0.32f, cy = (r0.y + r1.y) * 0.5f, a = fs * 0.27f;
                const bool up = icaoMode == 1 || icaoMode == 3;   // ascending, as ImGui draws it
                ImDrawList* hd = ImGui::GetWindowDrawList();
                if (up) hd->AddTriangleFilled(ImVec2(cx, cy - a), ImVec2(cx + a, cy + a * 0.7f), ImVec2(cx - a, cy + a * 0.7f), col);
                else hd->AddTriangleFilled(ImVec2(cx - a, cy - a * 0.7f), ImVec2(cx + a, cy - a * 0.7f), ImVec2(cx, cy + a), col);
            }
            if (ImGui::IsItemClicked()) icaoMode = (icaoMode + 1) % 5;
        } else ImGui::TableHeader(ImGui::TableGetColumnName(c));
    }
    ImGui::PopStyleColor();
    sortStations(v, d);
    if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())   // another column clicked: the ICAO order makes way
        if (sp->SpecsDirty) { if (sp->SpecsCount > 0) icaoMode = 0; sp->SpecsDirty = false; }
    if (icaoMode > 0)
        std::stable_sort(v.begin(), v.end(), [&](const Station* x, const Station* y) {
            int c = 0;
            if (icaoMode <= 2) c = x->icao.compare(y->icao);
            else c = catRank(flightCategory(x->report)) - catRank(flightCategory(y->report));
            if (c != 0) return (icaoMode == 2 || icaoMode == 4) ? c > 0 : c < 0;
            const double da = distTo(d, *x), db = distTo(d, *y);   // equal: nearest first
            return da < db - 1e-6;
        });
    if (maxRows > 0 && (int)v.size() > maxRows) v.resize((size_t)maxRows);
    for (const Station* s : v) {
        const Report& r = s->report;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        { const std::string c = flightCategory(r); ImGui::PushStyleColor(ImGuiCol_Text, v4(c.empty() ? cText : catColor(c))); ImGui::TextUnformatted(s->icao.c_str()); ImGui::PopStyleColor(); }
        ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f", distTo(d, *s));
        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(windShortRef(r, magVar(d, s->lat, s->lon), windMagnetic()).c_str());
        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(visText(r, usUnits(s->icao)).c_str());
        ImGui::TableSetColumnIndex(4); { const int c = ceilingFt(r); if (c < 0) muted("none"); else ImGui::TextUnformatted(ftText(c).c_str()); }
    }
    ImGui::EndTable();
}

// The tabs along the top: a little larger (padding, the bold font) and clearer than ImGui's defaults
ImFont*& tabFont() { static ImFont* f = nullptr; return f; }
ImFont*& tabFontBig() { static ImFont* f = nullptr; return f; }   // the semibold face a size up (loaded with the others)
const ImVec2 kTabPad(17, 4);   // the same text, a shorter tab
float tabWidth(const char* label) {
    ImFont* tf = tabFont() ? tabFont() : ImGui::GetFont();
    const float fs = tf->FontSize * ImGui::GetIO().FontGlobalScale;   // the atlas is made at the screen's pixel scale
    // the tab bar keeps the frame padding it was begun with (ImGui), so the tabs' side padding is the window's FramePadding.x
    return tf->CalcTextSizeA(fs, FLT_MAX, 0.0f, label).x + 2 * ImGui::GetStyle().FramePadding.x;
}
// Every tab the same width (the widest label, "Winds & Temps", plus padding), its label on the left (2026-11-12: one style)
float uniformTabWidth() { return tabWidth("Winds & Temps") + 10.0f; }
bool bigTab(const char* label, bool* open, ImGuiTabItemFlags flags, float width = -1.0f) {
    ImGui::SetNextItemWidth(width >= 0 ? width : uniformTabWidth());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kTabPad);
    if (tabFont()) ImGui::PushFont(tabFont());
    const bool r = ImGui::BeginTabItem(label, open, flags);
    if (tabFont()) ImGui::PopFont();
    ImGui::PopStyleVar();
    return r;
}

bool panelBegin(const char* id, ImVec2 size, ImGuiWindowFlags extra = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    const bool r = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, extra);
    return r;
}
void panelEnd() { ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); }

// ---- tabs ---------------------------------------------------------------------------------------------------------------------

// Zulu and local time (where the aircraft is), side by side
void timeBox(const UiData& d, const UiFonts& f) {
    const float w = ImGui::GetContentRegionAvail().x * 0.5f;
    const float x0 = ImGui::GetCursorPosX();
    bool est = false;
    const double off = localOffsetAt(d, d.lat, d.lon, est);
    ImGui::BeginGroup();
    muted("Zulu  UTC");
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted((clockText(d.now, true) + "Z").c_str());
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine(x0 + w);
    ImGui::BeginGroup();
    const std::string zn = zoneName(d.lat, d.lon, off, d.now);
    muted(("Local  " + std::string(est ? "~" : "") + (zn.empty() ? utcOffsetText(off) : zn + "  (" + utcOffsetText(off) + ")")).c_str());
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(clockText(d.now + (std::time_t)off, true).c_str());
    ImGui::PopFont();
    ImGui::EndGroup();
}

void tabOverview(const UiData& d, const UiFonts& f, UiActions& act) {
    UnitScope us(g_unitsHome);   // the home page's airport card and nearby list share their units (the winds-aloft table keeps its own)
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12;
    const float colW = std::floor((W - 2 * gap) * 0.31f), col3 = W - 2 * colW - 2 * gap, bodyH = H;
    const Station* st = nearestStation(d);
    // column 1
    ImGui::BeginGroup();
    const float cardH = std::floor(std::min(bodyH * 0.58f, 330.0f));
    if (panelBegin("card", ImVec2(colW, cardH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) { if (st) stationCard(d, f, *st, false); else mutedWrapped("No airport reports near the aircraft."); }
    panelEnd();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + gap);   // exactly `gap` between stacked panels, so every column ends level
    // the time: zulu and local where the aircraft is
    const float timeH = std::floor(ImGui::GetTextLineHeight() * 2 + ImGui::GetFontSize() * 0.6f + 24);
    if (panelBegin("time", ImVec2(colW, timeH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) timeBox(d, f);
    panelEnd();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + gap);   // exactly `gap` between stacked panels, so every column ends level
    if (panelBegin("near", ImVec2(colW, bodyH - cardH - timeH - 2 * gap), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
        sectionTitle(f, "Nearby airports");
        if (d.res) {   // how many, beside the title (the list's own count: the airports in use, without the nearest one shown above)
            size_t n = 0;
            for (const auto& s2 : d.res->stations) if (&s2 != st && s2.icao.compare(0, 3, "GFS") != 0) ++n;
            ImGui::SameLine(); muted(fmt("%zu", n).c_str());
        }
        nearbyTable(d, st, 0, true);
    }
    panelEnd();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    // column 2: the map with its storm figures, and the pilot reports in a panel of their own under it
    ImGui::BeginGroup();
    const float radarH = std::floor(std::min(bodyH * 0.62f, colW + 102.0f)), pirepH = bodyH - radarH - gap;
    if (panelBegin("radar", ImVec2(colW, radarH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) radarPanel(d, f, false);
    panelEnd();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + gap);   // exactly `gap` between stacked panels, so every column ends level
    if (panelBegin("hpireps", ImVec2(colW, pirepH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) homePireps(d, f);
    panelEnd();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float rowH = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
    // the turbulence card just fits its three rows; winds & temps aloft takes the rest, with as many levels as fit
    const float turbH = 3.0f * (ImGui::GetFrameHeight() + 22.0f + ImGui::GetStyle().ItemSpacing.y) + ImGui::GetTextLineHeightWithSpacing() + 34.0f;
    const float aloftH = std::floor(std::max(160.0f, bodyH - turbH - gap));
    if (panelBegin("aloft", ImVec2(col3, aloftH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
        sectionTitle(f, "Winds & temps aloft");
        ImGui::SameLine(); muted("GFS");
        { UnitScope us(g_unitsAloft); windRefSwitch("wrAloftHome"); }
        const int fit = std::max(4, (int)((ImGui::GetContentRegionAvail().y - 6) / rowH) - 2);   // header and tropopause rows
        static const float kAll[] = {3000, 5000, 7000, 10000, 12000, 15000, 18000, 21000, 24000, 27000, 30000, 32000, 34000, 36000, 39000, 41000, 45000, 50000};
        static const int kPick[][18] = {{1, 3, 6, 8, 10, 12, 14, 16}, {1, 3, 6, 8, 10, 12, 14, 15, 16}, {0, 1, 3, 6, 8, 10, 12, 14, 15, 16},
                                        {0, 1, 3, 5, 6, 8, 10, 12, 14, 15, 16}, {0, 1, 3, 5, 6, 8, 10, 11, 12, 14, 15, 16},
                                        {0, 1, 2, 3, 5, 6, 8, 10, 11, 12, 14, 15, 16}, {0, 1, 2, 3, 4, 5, 6, 8, 10, 11, 12, 14, 15, 16},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 14, 15, 16}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16},
                                        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}};
        static const int kLen[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
        int row = 0;
        for (int k = 0; k < 11; ++k) if (kLen[k] <= fit) row = k;
        std::vector<float> lv;
        for (int i = 0; i < kLen[row]; ++i) lv.push_back(kAll[kPick[row][i]]);
        aloftTable(d, f, lv, false);
    }
    panelEnd();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + gap);   // exactly `gap` between stacked panels, so every column ends level
    if (panelBegin("turb", ImVec2(col3, bodyH - aloftH - gap), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) turbulenceCard(d, f, act, false);
    panelEnd();
    ImGui::EndGroup();
}

void tabAirports(const UiData& d, const UiFonts& f, UiActions& act) {
    UnitScope us(g_unitsAir);   // the Airports tab: its own units
    static std::string selected;
    static char query[16] = "";
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float listW = std::floor(W * 0.56f);
    if (panelBegin("alist", ImVec2(listW, H)) && d.res) {
        // any airport in the world: its METAR and ATIS (only shown here, X-Plane's weather is not changed)
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - textColW("Search") - 10));
        const bool enter = ImGui::InputTextWithHint("##search", "Search any airport worldwide (ICAO or FAA code, e.g. EGLL, 35D)", query, sizeof query,
                                                    ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, 6);
        if (d.searchBusy) ImGui::BeginDisabled();
        const bool go = ImGui::Button("Search") || enter;
        if (d.searchBusy) ImGui::EndDisabled();
        if (go && query[0]) { act.search = query; selected = "@search"; }
        if (d.searchBusy) muted("Searching...");
        else if (!d.searchError.empty() && selected == "@search") { ImGui::PushStyleColor(ImGuiCol_Text, v4(cWarn)); ImGui::TextWrapped("%s", d.searchError.c_str()); ImGui::PopStyleColor(); }
        ImGui::Spacing();
        sectionTitle(f, fmt("Airports in use (%zu)", d.res->stations.size()).c_str());
        windRefSwitch("wrAir", 0.0f, true);   // with North / Heading Lock in the same bubble (the arrows turn with it)
        const float airMagRot = magTurn(2, windMagnetic(), (float)magVar(d, d.lat, d.lon)) * (1.0f - headingUpness());
        const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
        if (ImGui::BeginTable("all", 9, fl)) {
            ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, textColW("WWWW"), kSortIcao);
            ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, textColW("000"), kSortDist);
            ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed, textColW("MVFR"), kSortCat);
            ImGui::TableSetupColumn("###arrow", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 18);
            ImGui::TableSetupColumn(windMagnetic() ? "Wind °M###wind" : "Wind °T###wind", ImGuiTableColumnFlags_WidthFixed, textColW("VRB/00 G00 "), kSortWind);
            ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, textColW("10 km+"), kSortVis);
            ImGui::TableSetupColumn("Weather", ImGuiTableColumnFlags_WidthFixed, textColW("+TSRA BR"), kSortWx);
            ImGui::TableSetupColumn("Ceiling", ImGuiTableColumnFlags_WidthStretch, textColW("00,000 ft"), kSortCeil);
            ImGui::TableSetupColumn(tempF() ? "T/Td °F###ttd" : "T/Td °C###ttd", ImGuiTableColumnFlags_WidthFixed, textColW("T/Td °C "), kSortTemp);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            std::vector<const Station*> v;
            for (const auto& s : d.res->stations) v.push_back(&s);
            sortStations(v, d);
            if (selected.empty() && !v.empty()) selected = v[0]->icao;
            for (const Station* s : v) {
                const Report& r = s->report;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID((const void*)s);   // two reports can share an ICAO code: the row's own ID
                if (ImGui::Selectable(s->icao.c_str(), selected == s->icao, ImGuiSelectableFlags_SpanAllColumns)) selected = s->icao;
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f", distTo(d, *s));
                ImGui::TableSetColumnIndex(2); { const std::string c = flightCategory(r); ImGui::PushStyleColor(ImGuiCol_Text, v4(catColor(c))); ImGui::TextUnformatted(c.empty() ? "—" : c.c_str()); ImGui::PopStyleColor(); }
                ImGui::TableSetColumnIndex(3);   // the wind's arrow, as on the winds-aloft tables (North / Heading Lock, °T / °M)
                if (r.hasWind && r.windKt >= 1 && !r.windVariable) windArrow((float)r.windDir, 16, viewRot() + airMagRot, dialHeadingUp() && d.havePos);
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(windShortRef(r, magVar(d, s->lat, s->lon), windMagnetic()).c_str());
                ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(visText(r, usUnits(s->icao)).c_str());
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(r.wxText.empty() ? "—" : r.wxText.c_str());
                ImGui::TableSetColumnIndex(7); { const int c = ceilingFt(r); if (c < 0) muted("none"); else ImGui::TextUnformatted((ftText(c) + " ft").c_str()); }
                ImGui::TableSetColumnIndex(8); if (r.hasTemp) ImGui::Text("%.0f/%.0f", tempIn(r.tempC), tempIn(r.dewpC)); else muted("—");
            }
            ImGui::EndTable();
        }
    }
    panelEnd();
    ImGui::SameLine(0, 12);
    if (panelBegin("adetail", ImVec2(0, H)) && d.res) {
        const Station* sel = nullptr;
        if (selected == "@search") {
            if (d.searchResult) {
                sel = d.searchResult.get();
                muted("Search result - shown here only, X-Plane's weather is not changed");
                stationCard(d, f, *sel, true);
            } else mutedWrapped(d.searchBusy ? "Searching..." : (d.searchError.empty() ? "Type an ICAO code and press Search." : d.searchError));
            sel = nullptr;
            panelEnd();
            return;
        }
        for (const auto& s : d.res->stations) if (s.icao == selected) sel = &s;
        if (sel) {
            stationCard(d, f, *sel, true);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            sectionTitle(f, "Sent to X-Plane");
            if (d.cfg) {
                XpWx x = buildXp(*sel, *d.cfg, d.now, &d.res->aloft, true);
                mutedWrapped(describeXp(x, sel->elevM));
            }
        } else {
            mutedWrapped("Select an airport.");
        }
    }
    panelEnd();
}

// The Map tab's strongest cells: cells within 3 nm of a stronger one merged into it (one row per storm, the highest of each value,
// the strongest one's place). Made once per radar picture (it compares the cells with each other), on a 3 nm grid.
struct StormCell { double lat, lon; float dbz, rate, topM; bool topKnown; double dist, brg; };
const std::vector<StormCell>& mergedStormCells(const RadarResult& r) {
    static std::vector<StormCell> merged;
    static const void* mergedOf = nullptr;
    static size_t mergedN = (size_t)-1;
    static double mergedSig = 0;
    const double sig = r.cells.empty() ? 0.0 : r.cells.front().lat + r.cells.back().lon * 7.0 + r.cells.back().dbz;
    if (mergedOf != (const void*)r.cells.data() || mergedN != r.cells.size() || mergedSig != sig) {
        mergedOf = r.cells.data(); mergedN = r.cells.size(); mergedSig = sig;
        merged.clear();
        std::vector<const RadarCell*> raw;
        for (const auto& k : r.cells) if (!k.synthetic) raw.push_back(&k);
        std::stable_sort(raw.begin(), raw.end(), [](const RadarCell* a, const RadarCell* b) { return a->dbz > b->dbz; });
        std::map<std::pair<int, int>, std::vector<size_t>> grid;   // 3 nm squares -> the storms in them
        auto sq = [](double lat, double lon) { return std::make_pair((int)std::floor(lat * 20.0), (int)std::floor(lon * 60.0 * std::cos(lat * kPi / 180.0) / 3.0)); };
        for (const RadarCell* k : raw) {
            const auto c0 = sq(k->lat, k->lon);
            StormCell* into = nullptr;
            for (int dy = -1; dy <= 1 && !into; ++dy)
                for (int dx = -1; dx <= 1 && !into; ++dx) {
                    const auto it = grid.find({c0.first + dy, c0.second + dx});
                    if (it == grid.end()) continue;
                    for (size_t m : it->second) if (distNm(merged[m].lat, merged[m].lon, k->lat, k->lon) <= 3.0) { into = &merged[m]; break; }
                }
            if (into) {
                into->rate = std::max(into->rate, k->rateMmH);
                if (k->topM > into->topM) { into->topM = k->topM; into->topKnown = k->topKnown; }
                continue;
            }
            grid[c0].push_back(merged.size());
            merged.push_back(StormCell{k->lat, k->lon, k->dbz, k->rateMmH, k->topM, k->topKnown, k->distNm, -1});
        }
    }
    return merged;
}

void tabRadar(const UiData& d, const UiFonts& f) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float mapW = std::min(W * 0.62f, H + 40);
    if (panelBegin("rbig", ImVec2(mapW, H), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) radarPanel(d, f, true);
    panelEnd();
    { UnitScope us(g_unitsAir); infoWindow(d, f); }   // the popups use the map's units (the Airports tab's)
    ImGui::SameLine(0, 12);
    // the right side: the strongest cells over every pilot report (a click on one glides the map to it and opens its window)
    const float gapP = 12, cellsH = std::floor((H - gapP) * 0.5f);
    ImGui::BeginGroup();
    if (panelBegin("rcells", ImVec2(0, cellsH)) && d.res) {
        const RadarResult& r = d.res->radar;
        sectionTitle(f, "Strongest cells");
        if (r.ok) { ImGui::SameLine(); muted(fmt("%zu", mergedStormCells(r).size()).c_str()); }   // how many, beside the title (as for the pilot reports)
        if (!r.ok) mutedWrapped(radarSubtitle(d));
        else {
            muted(r.topsFromRadar ? "Storm tops from radar echo tops" : "Storm tops estimated from reflectivity");
            // cells within 3 nm of a stronger one are merged into it: one row per storm, with the highest of each value (reflectivity,
            // rain, top) and the strongest one's place. The merge is made once per radar picture (not every frame: it compares the
            // cells with each other), on a 3 nm grid so each cell only looks at the storms in the squares around it.
            std::vector<StormCell> cs = mergedStormCells(r);
            // where from the aircraft, now (the bearing in the units chosen: °T, or °M with the variation at the aircraft)
            const bool magB = windMagnetic();
            const double varA = d.havePos ? magVar(d, d.lat, d.lon) : 0.0;
            for (auto& c : cs) {
                if (!d.havePos) { c.brg = -1; continue; }
                c.dist = distNm(d.lat, d.lon, c.lat, c.lon);
                const double dy = c.lat - d.lat, dx = (c.lon - d.lon) * std::cos(d.lat * kPi / 180.0);
                const double t = std::fmod(std::atan2(dx, dy) * 180.0 / kPi + 360.0, 360.0);
                c.brg = magB ? (double)toMagnetic(t, varA) : std::fmod(std::round(t), 360.0);
            }
            enum { kCDbz = 1, kCRain, kCTop, kCWhere };
            // whole rows only, as in the pilot report lists: as many rows as fit, a scroll bar beside them, the wheel a row at a time
            static RowMetrics cm;
            static int cFirst = 0;
            const float est = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
            const float rowH = cm.step > 1 ? cm.step : est, headH = cm.head > 1 ? cm.head : est;
            const int pageRows = std::max(1, (int)std::floor((ImGui::GetContentRegionAvail().y - 2 - headH - 1) / rowH));
            const bool bar = (int)cs.size() > pageRows;
            const float sbW = ImGui::GetStyle().ScrollbarSize;
            const float tableTop = ImGui::GetCursorScreenPos().y;
            if (ImGui::BeginTable("cells", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX |
                                              ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate,
                                  ImVec2(bar ? ImGui::GetContentRegionAvail().x - sbW - 2 : 0.0f, 0.0f))) {
                // sortable like the pilot reports: a click on a header (again: the other way, a third time: back to strongest first)
                ImGui::TableSetupColumn("dBZ", ImGuiTableColumnFlags_WidthFixed, textColW("dBZ") + 8, kCDbz);
                ImGui::TableSetupColumn("Rain", ImGuiTableColumnFlags_WidthFixed, textColW("000 mm/h") + 8, kCRain);
                ImGui::TableSetupColumn("Top", ImGuiTableColumnFlags_WidthFixed, textColW("FL000 est") + 8, kCTop);
                ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.0f, kCWhere);
                ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
                ImGui::TableHeadersRow();
                ImGui::PopStyleColor();
                int col = kCDbz; bool desc = true;
                if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs())
                    if (sp->SpecsCount > 0) { col = (int)sp->Specs[0].ColumnUserID; desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending; }
                auto key = [&](const StormCell& c) -> double { return col == kCRain ? c.rate : (col == kCTop ? c.topM : (col == kCWhere ? c.dist : c.dbz)); };
                std::stable_sort(cs.begin(), cs.end(), [&](const StormCell& a, const StormCell& b) {
                    const double ka = key(a), kb = key(b);
                    if (ka != kb) return desc ? ka > kb : ka < kb;
                    return a.dist < b.dist;   // ties: nearest first
                });
                cFirst = std::max(0, std::min(cFirst, (int)cs.size() - pageRows));
                const size_t r0 = (size_t)cFirst, r1 = std::min(cs.size(), r0 + (size_t)pageRows);
                float y0 = -1;
                for (size_t i = r0; i < r1; ++i) {
                    const StormCell& k = cs[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    {   // measure the header and the row step (for the page size next frame)
                        const float y = ImGui::GetCursorScreenPos().y;
                        if (i == r0) { y0 = y; cm.head = y - ImGui::GetStyle().CellPadding.y - tableTop; }
                        else if (i == r0 + 1 && y0 >= 0) cm.step = y - y0;
                    }
                    // a click on a row: the map glides there (its + over the cell)
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable("##cell", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && d.havePos) {
                        panGoal() = ImVec2((float)((k.lon - d.lon) * 60.0 * std::cos(d.lat * kPi / 180.0)), (float)((k.lat - d.lat) * 60.0));
                        panGliding() = true; recentering() = false;
                    }
                    if (d.havePos && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::PopID();
                    ImGui::SameLine(0, 0);
                    ImGui::PushStyleColor(ImGuiCol_Text, v4(dbzColor(k.dbz, 255))); ImGui::Text("%.0f", k.dbz); ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f mm/h", k.rate);
                    ImGui::TableSetColumnIndex(2); if (k.topM > 0) ImGui::Text("FL%03.0f%s", k.topM / 0.3048 / 100.0, k.topKnown ? "" : " est"); else muted("—");
                    ImGui::TableSetColumnIndex(3);
                    if (k.brg < 0) ImGui::Text("%.0f nm  —", k.dist);   // no aircraft position: no bearing
                    else ImGui::Text(magB ? "%.0f nm  %03.0f°M" : "%.0f nm  %03.0f°T", k.dist, k.brg);
                }
                ImGui::EndTable();
                const ImVec2 tMin = ImGui::GetItemRectMin(), tMax = ImGui::GetItemRectMax();
                const bool overList = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(tMin, tMax);
                if (bar) {   // the scroll bar, as beside the pilot reports: drag the thumb, click the track, or wheel
                    const float head = cm.head > 1 ? cm.head : est;
                    const ImVec2 a(tMax.x + 2, tMin.y + head), b(tMax.x + 2 + sbW, tMax.y);
                    const int n = (int)cs.size(), maxFirst = std::max(1, n - pageRows);
                    const float trackH = b.y - a.y, thumbH = std::max(18.0f, trackH * (float)pageRows / (float)n);
                    const float ty = a.y + (trackH - thumbH) * (float)cFirst / (float)maxFirst;
                    ImGui::SetCursorScreenPos(a);
                    ImGui::InvisibleButton("##cellsbar", ImVec2(sbW, std::max(1.0f, trackH)));
                    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
                    if (act) {
                        const float my = ImGui::GetIO().MousePos.y - a.y - thumbH * 0.5f;
                        cFirst = (int)std::lround(std::max(0.0f, std::min(1.0f, my / std::max(1.0f, trackH - thumbH))) * maxFirst);
                    }
                    ImDrawList* sd = ImGui::GetWindowDrawList();
                    sd->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ScrollbarBg), sbW * 0.5f);
                    sd->AddRectFilled(ImVec2(a.x + 2, ty + 2), ImVec2(b.x - 2, ty + thumbH - 2),
                                      ImGui::GetColorU32(act ? ImGuiCol_ScrollbarGrabActive : (hov ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab)), sbW * 0.5f);
                    if (hov && ImGui::GetIO().MouseWheel != 0.0f) cFirst -= (int)std::lround(ImGui::GetIO().MouseWheel);
                }
                if (overList && ImGui::GetIO().MouseWheel != 0.0f) cFirst -= (int)std::lround(ImGui::GetIO().MouseWheel);   // a row at a time
                cFirst = std::max(0, std::min(cFirst, (int)cs.size() - pageRows));
            }
        }
    }
    panelEnd();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + gapP);   // exactly the gap: both columns end level
    if (panelBegin("rpireps", ImVec2(0, H - cellsH - gapP), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
        sectionTitle(f, "Pilot reports");
        size_t shown = 0;
        if (d.reports) for (const auto& q : d.reports->pireps) if (pirepListed(q)) ++shown;
        if (d.reports) { ImGui::SameLine(); muted(fmt("%zu", shown).c_str()); }   // how many, beside the title (as on the home page)
        // whole rows only, as on the home page (no half-cut row at the bottom of the panel)
        const RowMetrics& rm = pirepMetrics("mappireps");
        const float est = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
        const float rowH = rm.step > 1 ? rm.step : est, headH = rm.head > 1 ? rm.head : est;
        const float avail = ImGui::GetContentRegionAvail().y - 2;
        const int fitRows = std::max(1, (int)std::floor((avail - headH - 1) / rowH));
        if (!d.reports || shown == 0) mutedWrapped("No pilot reports in the last 90 minutes.");
        else if (avail > 40) { static std::vector<std::string> open; pirepTable(d, f, "mappireps", ImVec2(0, -(float)std::min<int>(fitRows, (int)shown)), open, 2); }
    }
    panelEnd();
    ImGui::EndGroup();
}

void tabAloft(const UiData& d, const UiFonts& f) {
    const float H = ImGui::GetContentRegionAvail().y;
    if (panelBegin("aloftfull", ImVec2(0, H))) {
        sectionTitle(f, "Winds & temperatures aloft at the aircraft");
        const Station* st = nearestStation(d);
        if (st) { ImGui::SameLine(); muted(fmt("GFS, blended for %s", st->icao.c_str()).c_str()); }
        { UnitScope us(g_unitsAloft); windRefSwitch("wrAloftTab", 0.0f, true); }   // with North / Heading Lock in the same bubble
        // as many levels as fit without scrolling (the densest set of these that fits the window's height)
        const float rowH = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
        const int rows = std::max(6, (int)((ImGui::GetContentRegionAvail().y - 4) / rowH) - 2);   // header and tropopause rows
        static const float kSteps[][2] = {{1000, 2000}, {2000, 2000}, {2000, 3000}, {3000, 4000}, {3000, 5000}, {5000, 6000}};   // below/above 18,000 ft
        std::vector<float> lv;
        for (const auto& st : kSteps) {
            lv.clear();
            for (float ft = 2000; ft < 18000; ft += st[0]) lv.push_back(ft);
            for (float ft = 18000; ft <= 60000; ft += st[1]) lv.push_back(ft);
            if ((int)lv.size() <= rows) break;
        }
        aloftTable(d, f, lv, false);
    }
    panelEnd();
}

// The g meter's record: sampled every frame the window is drawn, whichever tab is open, so the trace is there when the Turbulence tab
// is opened (the last minute)
struct GSample { double t; float g; };
std::vector<GSample>& gHistory() { static std::vector<GSample> h; return h; }
void gSample(const UiData& d) {
    auto& hist = gHistory();
    const double now = ImGui::GetTime();
    if (d.haveG && (hist.empty() || now - hist.back().t > 1.0 / 30.0)) hist.push_back({now, d.gNormal});
    size_t drop = 0;
    while (drop < hist.size() && now - hist[drop].t > 60.0) ++drop;
    if (drop) hist.erase(hist.begin(), hist.begin() + (long)drop);
}

// The Turbulence tab's live panel: the EDR WXPlane is giving X-Plane at the aircraft now (after Max turbulence), as a bar over the
// intensity bands, and a g meter of what the aircraft feels (X-Plane's normal load factor): the value, its range over the last
// minute, and a 30 s trace around 1 g.
void turbLive(const UiData& d, const UiFonts& f) {
    sectionTitle(f, "Live at the aircraft");
    muted("What the turbulence is doing to the aircraft now");
    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    // 1) the EDR being pushed
    const double maxT = d.cfg ? d.cfg->maxTurbulence : 1.0;
    const double edr = d.turbOn && d.turbEdr >= 0 ? d.turbEdr * maxT : -1.0;
    {
        ImGui::PushFont(f.bold); ImGui::TextUnformatted("EDR applied"); ImGui::PopFont();
        ImGui::SameLine();
        if (edr < 0) muted(d.turbOn ? "no turbulence data yet" : "realistic turbulence is off");
        else {
            const ImU32 col = edr < 0.08 ? cAccentDim : (edr < 0.15 ? IM_COL32(80, 160, 110, 255) : (edr < 0.35 ? cWarn : cDown));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(col)); ImGui::Text("%.2f  %s", edr, turbWord(edr).c_str()); ImGui::PopStyleColor();
            ImGui::SameLine(); muted(fmt("(%s, x%.2f Max turbulence)", d.turbSource.c_str(), maxT).c_str());
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = 16, full = 0.6f;
        struct Band { float a, b; ImU32 c; const char* t; };
        const Band bands[] = {{0, 0.08f, IM_COL32(40, 52, 70, 255), "None"}, {0.08f, 0.15f, IM_COL32(40, 70, 56, 255), "Light"},
                              {0.15f, 0.35f, IM_COL32(84, 70, 34, 255), "Moderate"}, {0.35f, 0.6f, IM_COL32(88, 40, 40, 255), "Severe"}};
        for (const Band& b : bands) {
            const float x0 = p.x + W * b.a / full, x1 = p.x + W * b.b / full;
            dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h), b.c);
            dl->AddText(f.body, ImGui::GetFontSize() * 0.75f, ImVec2(x0 + 4, p.y + h + 3), cMuted, b.t);
        }
        if (edr >= 0) {
            const float x = p.x + W * (float)std::min(1.0, edr / full);
            dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(std::max(p.x + 2, x), p.y + h - 4), IM_COL32(236, 239, 244, 230), 2.0f);
            dl->AddLine(ImVec2(x, p.y - 3), ImVec2(x, p.y + h + 3), IM_COL32(255, 255, 255, 255), 2.0f);
        }
        ImGui::Dummy(ImVec2(W, h + ImGui::GetFontSize() * 0.75f + 8));
    }
    ImGui::Spacing(); ImGui::Spacing();
    // 2) the g meter
    const auto& hist = gHistory();
    const double now = ImGui::GetTime();
    float gMin = 1e9f, gMax = -1e9f;
    for (const auto& q : hist) { gMin = std::min(gMin, q.g); gMax = std::max(gMax, q.g); }
    ImGui::PushFont(f.bold); ImGui::TextUnformatted("Load factor"); ImGui::PopFont();
    ImGui::SameLine();
    if (!d.haveG) { muted("not available"); return; }
    const float dev = std::fabs(d.gNormal - 1.0f);
    const ImU32 gc = dev < 0.15f ? IM_COL32(236, 239, 244, 255) : (dev < 0.35f ? cWarn : cDown);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(gc)); ImGui::Text("%.2f g", d.gNormal); ImGui::PopStyleColor();
    ImGui::SameLine(); muted(fmt("last minute %.2f to %.2f g", gMin, gMax).c_str());
    // the graph: g on the left (labels in their own margin, never over the trace), seconds back along the bottom, newest on the right
    const float lfs = ImGui::GetFontSize() * 0.75f;
    const float axisW = f.body->CalcTextSizeA(lfs, FLT_MAX, 0, "2.0 g").x + 8, axisH = lfs + 6;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float totalH = std::max(90.0f, std::min(ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() - 6, 260.0f));
    const float gh = totalH - axisH, gw = W - axisW;
    const ImVec2 p(p0.x + axisW, p0.y);
    dl->AddRectFilled(p, ImVec2(p.x + gw, p.y + gh), cInset, 4.0f);
    // 0..2 g, 1 g in the middle (fixed, so a bump looks as big as it is)
    auto yOf = [&](float g) { return p.y + 4 + (gh - 8) * (1.0f - g / 2.0f); };
    for (float g : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
        const float y = yOf(g);
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + gw, y), g == 1.0f ? IM_COL32(90, 96, 108, 255) : IM_COL32(44, 47, 54, 255), 1.0f);
        const std::string t = fmt("%.1f g", g);
        const ImVec2 ts = f.body->CalcTextSizeA(lfs, FLT_MAX, 0, t.c_str());
        dl->AddText(f.body, lfs, ImVec2(p0.x + axisW - 6 - ts.x, y - ts.y * 0.5f), cMuted, t.c_str());
    }
    for (int sec = 0; sec <= 30; sec += 5) {   // the time axis: how far back the trace goes (30 s)
        const float x = p.x + gw * (1.0f - sec / 30.0f);
        if (sec > 0 && sec < 30) dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + gh), IM_COL32(38, 41, 47, 255), 1.0f);
        const std::string t = sec == 0 ? std::string("now") : fmt("-%ds", sec);
        const ImVec2 ts = f.body->CalcTextSizeA(lfs, FLT_MAX, 0, t.c_str());
        const float tx = std::max(p.x, std::min(p.x + gw - ts.x, x - ts.x * 0.5f));
        dl->AddText(f.body, lfs, ImVec2(tx, p.y + gh + 3), cMuted, t.c_str());
    }
    std::vector<ImVec2> pts;
    for (const auto& q : hist) {
        const double age = now - q.t;
        if (age > 30.0) continue;
        pts.push_back(ImVec2(p.x + gw * (float)(1.0 - age / 30.0), std::max(p.y + 1, std::min(p.y + gh - 1, yOf(q.g)))));
    }
    dl->PushClipRect(p, ImVec2(p.x + gw, p.y + gh), true);
    if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), cAccent, ImDrawFlags_None, 1.8f);
    if (!pts.empty()) dl->AddCircleFilled(pts.back(), 3.5f, gc);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(W, totalH));
    muted("1 g is level flight; X-Plane's load factor at the aircraft, the last 30 s");
}

// Another add-on controls the turbulence (ActiveSky XP, xProTurb ...): a red caution over the Turbulence tab, whose controls are
// locked meanwhile (like X-Plane's real weather over the whole window)
void turbConflictCaution(const UiFonts& f, const std::string& who, ImVec2 a, ImVec2 b) {
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->PushClipRect(a, b, true);
    fg->AddRectFilled(a, b, IM_COL32(10, 10, 12, 170), 6.0f);
    const float w = b.x - a.x, h = b.y - a.y;
    const float bw = std::min(w - 40, 700.0f), bh = 160;
    const ImVec2 b0(a.x + (w - bw) * 0.5f, a.y + (h - bh) * 0.5f), b1(b0.x + bw, b0.y + bh);
    fg->AddRectFilled(b0, b1, IM_COL32(150, 24, 24, 245), 10.0f);
    fg->AddRect(b0, b1, IM_COL32(255, 120, 120, 255), 10.0f, 0, 2.0f);
    const float big = ImGui::GetFontSize() * 1.7f, mid = ImGui::GetFontSize() * 1.05f;
    const char* l1 = "TURBULENCE CONFLICT";
    const std::string l2 = who + " is running and controls the turbulence.";
    const std::string l3 = "WXPlane leaves turbulence to it. Disable " + who + "'s turbulence (or the add-on) to use WXPlane's.";
    const ImVec2 t1 = f.title->CalcTextSizeA(big, FLT_MAX, 0, l1), t2 = f.body->CalcTextSizeA(mid, FLT_MAX, 0, l2.c_str()), t3 = f.body->CalcTextSizeA(mid, bw - 30, bw - 30, l3.c_str());
    fg->AddText(f.title, big, ImVec2(b0.x + (bw - t1.x) * 0.5f, b0.y + 26), IM_COL32(255, 255, 255, 255), l1);
    fg->AddText(f.body, mid, ImVec2(b0.x + (bw - t2.x) * 0.5f, b0.y + 26 + t1.y + 16), IM_COL32(255, 225, 225, 255), l2.c_str());
    fg->AddText(f.body, mid, ImVec2(b0.x + (bw - t3.x) * 0.5f, b0.y + 26 + t1.y + 16 + t2.y + 6), IM_COL32(255, 225, 225, 255), l3.c_str(), nullptr, bw - 30);
    fg->PopClipRect();
}

void tabTurbulence(const UiData& d, const UiFonts& f, UiActions& act) {
    const bool conflict = !d.turbConflict.empty();
    const ImVec2 area0 = ImGui::GetCursorScreenPos();
    if (conflict) ImGui::BeginDisabled();
    struct Lock { bool on; const UiData& d; const UiFonts& f; ImVec2 a, b;
                  ~Lock() { if (on) { ImGui::EndDisabled(); turbConflictCaution(f, d.turbConflict, a, b); } } };
    const Lock lock{conflict, d, f, area0, ImVec2(area0.x + ImGui::GetContentRegionAvail().x, area0.y + ImGui::GetContentRegionAvail().y)};
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    // the turbulence figures and both sliders across the top of the page; under them the profile and the pilot reports, ending level
    const float leftW = std::floor(W * 0.45f), gapT = 12;
    const float topH = std::floor(2 * ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().ItemSpacing.y + 128);
    if (panelBegin("tcard", ImVec2(W, topH), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) turbulenceCard(d, f, act, true);
    panelEnd();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + gapT);
    const float lowH = H - topH - gapT;
    ImGui::BeginGroup();
    if (panelBegin("tprof", ImVec2(leftW, lowH))) {
        sectionTitle(f, "Turbulence by altitude here (EDR)");
        if (d.turbProfile.empty()) mutedWrapped(d.turbOn ? "No turbulence data yet." : "Realistic turbulence is off.");
        for (int i = (int)d.turbProfile.size() - 1; i >= 0; --i) {
            const UiTurbLayer& L = d.turbProfile[(size_t)i];
            ImGui::Text(L.altFt >= 18000 ? "FL%03.0f" : "%5.0f ft", L.altFt >= 18000 ? L.altFt / 100.0 : L.altFt);
            ImGui::SameLine(80);
            const bool est = !L.has;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x - 120, h = ImGui::GetTextLineHeight();
            const float fr = (float)std::min(1.0, L.edr / 0.5);
            const ImU32 col = L.edr < 0.08 ? cAccentDim : (L.edr < 0.15 ? IM_COL32(80, 160, 110, 255) : (L.edr < 0.35 ? cWarn : cDown));
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + w, p.y + h * 0.75f), cInset, 3);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + std::max(2.0f, w * fr), p.y + h * 0.75f), col, 3);
            ImGui::Dummy(ImVec2(w, h));
            ImGui::SameLine(0, 8);
            ImGui::Text("%.2f %s%s", L.edr, turbWord(L.edr).c_str(), est ? " (from the levels around)" : "");
        }
    }
    panelEnd();
    ImGui::EndGroup();
    ImGui::SameLine(0, 12);
    if (panelBegin("tlive", ImVec2(0, lowH), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) turbLive(d, f);
    panelEnd();
}

void settingRow(const char* label, bool on, const char* help, bool& flip) {
    bool v = on;
    if (ImGui::Checkbox(label, &v)) flip = true;
    ImGui::Indent(30);
    mutedWrapped(help);
    ImGui::Unindent(30);
    ImGui::Spacing();
}

void tabSettings(const UiData& d, const UiFonts& f, UiActions& act) {
    // two columns, so the whole page fits without scrolling: the switches on the left, the reference text on the right
    const float H = ImGui::GetContentRegionAvail().y, W = ImGui::GetContentRegionAvail().x;
    const ImGuiWindowFlags nf = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const float leftW = std::floor((W - 12) * 0.5f);
    if (panelBegin("settings", ImVec2(leftW, H), nf) && d.cfg) {
        sectionTitle(f, "Weather");
        settingRow("WXPlane weather on", d.cfg->enabled, "Off hands the weather back to X-Plane (WXPlane's records are removed).", act.toggleEnabled);
        // with the weather off, every other setting is greyed out (dark grey instead of blue) and cannot be changed
        const bool off = !d.cfg->enabled;
        if (off) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_CheckMark, v4(IM_COL32(88, 92, 100, 255)));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(IM_COL32(32, 34, 38, 255)));
        }
        settingRow("Use ATIS", d.cfg->useAtis, "Merge the digital ATIS (D-ATIS, major US airports) over the METAR.", act.toggleAtis);
        ImGui::Separator(); ImGui::Spacing();
        sectionTitle(f, "Turbulence & windshear");
        {   // one switch for both: turbulence at every altitude, and windshear near storms
            const bool on = d.cfg->realisticTurbulence || d.cfg->windshear;
            bool flip = false;
            // another add-on does the turbulence: the switch stays as it is (blue, and can still be switched off), with a red note
            const bool conflict = !d.turbConflict.empty();
            settingRow("Realistic turbulence & windshear", on, "Turbulence at every altitude from NOAA GTGN (US) / ECMWF (elsewhere) and pilot reports, and gust fronts and microbursts near convective storms. Off: X-Plane's own turbulence, no windshear, and no Turbulence tab. Max turbulence is on the Turbulence tab.", flip);
            if (conflict) {
                ImGui::Indent(30);
                ImGui::PushStyleColor(ImGuiCol_Text, v4(cDown));
                ImGui::TextWrapped("%s is running and controls the turbulence, so WXPlane leaves it alone while it runs (windshear still works).", d.turbConflict.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(30);
                ImGui::Spacing();
            }
            if (flip) act.setTurbWs = on ? 0 : 1;
        }
        if (off) { ImGui::PopStyleColor(2); ImGui::EndDisabled(); }
        ImGui::Separator(); ImGui::Spacing();
        sectionTitle(f, "Status");
        mutedWrapped(d.status1 + (d.status2.empty() ? "" : "  ·  " + d.status2));
        mutedWrapped("Advanced settings are in WXPlane.cfg (X-Plane's Output/preferences folder).");
    }
    panelEnd();
    ImGui::SameLine(0, 12);
    if (panelBegin("settings2", ImVec2(0, H), nf)) {
        sectionTitle(f, "Map & radar");
        mutedWrapped("Radar is always on: NOAA MRMS over the US, EUMETNET OPERA over Europe. The buttons above the map choose its layers (radar, map, "
                     "airports, turbulence, winds, clouds, pilot reports, AIRMETs, SIGMETs); the slider beside it sets the altitude of the turbulence, "
                     "wind and cloud layers. The mouse wheel zooms (1 to 320 nm), and a click on the wind dial turns the dial and the map between North Lock "
                     "and Airplane Heading Lock.");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        sectionTitle(f, "Keyboard");
        mutedWrapped("To open and close this window with a key or a joystick button: X-Plane's Settings > Keyboard (or Joystick), search for \"WXPlane\" and bind \"WXPlane: open/close the weather window (GUI)\" (command wxplane/weather_window). \"WXPlane: refresh the weather now\" (wxplane/refresh) is there too.");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        // data and credits, in a darker grey: there for reference, not for attention
        ImGui::PushStyleColor(ImGuiCol_Text, v4(IM_COL32(112, 117, 126, 255)));
        ImGui::TextUnformatted("DATA & CREDITS");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("WXPlane " WX_VERSION ". Weather: NOAA / National Weather Service - Aviation Weather Center (METARs, pilot reports, AIRMETs and SIGMETs), MRMS radar, "
                               "GTGN turbulence (GTG by NCAR for the FAA); public domain. Open-Meteo.com (NOAA GFS model data), CC BY 4.0. EUMETNET OPERA radar "
                               "composite (Open Radar Data), CC BY 4.0. ECMWF Open Data, CC BY 4.0. D-ATIS by clowd.io.");
        ImGui::TextUnformatted("Map: Natural Earth (public domain). Window: Dear ImGui (MIT). Fonts: IBM Plex Sans and JetBrains Mono (SIL Open Font License). "
                               "Downloads: libcurl, OpenSSL (Linux). Decoding: OpenJPEG, libaec. Full licence texts in LICENSES.txt next to the plugin.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    panelEnd();
}

bool isRadarService(const ServiceStatus& s) { return s.use.find("radar") != std::string::npos || s.name.find("MRMS") != std::string::npos || s.name.find("OPERA") != std::string::npos; }

void serviceTable(const UiData& d, const char* id, const std::vector<ServiceStatus>& list, bool radar = false) {
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 16);
    ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, textColW("NOAA Aviation Weather Center"));
    ImGui::TableSetupColumn("Gives", ImGuiTableColumnFlags_WidthFixed, textColW("clouds aloft, open-water weather"));
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    plainHeaders();
    ImGui::PopStyleColor();
    for (const auto& s : list) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); statusDot(s.state);
        ImGui::TableSetColumnIndex(1); if (s.state == SvcState::NotHere) muted(s.name.c_str()); else ImGui::TextUnformatted(s.name.c_str());
        ImGui::TableSetColumnIndex(2); muted(s.use.c_str());
        ImGui::TableSetColumnIndex(3);
        std::string t = std::string(svcLabel(s.state)) + (s.detail.empty() ? "" : ": " + s.detail);
        if ((s.state == SvcState::Down || s.state == SvcState::Degraded) && s.lastOk > 0)
            t += fmt(" (last good %s, %ld min ago)", hhmm(s.lastOk).c_str(), (long)((d.now - s.lastOk) / 60));
        if (radar) {   // the radar sources say it short: where they do not reach, or when they last came in
            const std::string last = s.lastOk > 0 ? hhmm(s.lastOk) : std::string();   // (hhmm ends in Z)
            if (s.state == SvcState::NotHere) t = "Outside of Service Area";
            else if (s.state == SvcState::Ok) t = "OK: Last Update " + (last.empty() ? hhmm(d.now) : last);
            else if (s.state == SvcState::Waiting) t = "Waiting for the first update";
            else if (s.state == SvcState::Off) t = "Off";
            else t = std::string(svcLabel(s.state)) + (last.empty() ? "" : ": Last Update " + last);
        }
        if (s.state == SvcState::NotHere) mutedWrapped(t); else ImGui::TextWrapped("%s", t.c_str());
    }
    ImGui::EndTable();
}

void tabServices(const UiData& d, const UiFonts& f) {
    const float H = ImGui::GetContentRegionAvail().y;
    if (panelBegin("svcfull", ImVec2(0, H))) {
        sectionTitle(f, "Data services");
        std::vector<ServiceStatus> data;
        for (const auto& s : allServices(d)) if (!isRadarService(s)) data.push_back(s);
        serviceTable(d, "svct", data);
        // the radar sources, both always listed: the one that does not cover this place says so
        ImGui::Spacing(); ImGui::Spacing();
        sectionTitle(f, "Radar");
        const double la = d.havePos ? d.lat : (d.res ? d.res->centerLat : 0), lo = d.havePos ? d.lon : (d.res ? d.res->centerLon : 0);
        const std::string reg = radarRegion(la, lo);
        std::vector<ServiceStatus> radar;
        auto pick = [&](const char* key, const char* name, const char* use, const char* region) {
            ServiceStatus r;
            bool found = false;
            if (d.res) for (const auto& s : d.res->services) if (s.name.find(key) != std::string::npos) { r = s; found = true; }
            if (!found) { r.name = name; r.use = use; r.state = SvcState::Waiting; r.detail = "waiting for the first download"; }
            if (reg != region) {   // not used: the aircraft is outside this radar's coverage
                r.state = SvcState::NotHere;
                r.detail = std::string("this place is outside it; it covers ") + (std::string(region) == "US" ? "the United States only" : "Europe only") +
                           (reg.empty() ? " (no radar composite covers this place)" : "");
            }
            radar.push_back(r);
        };
        pick("MRMS", "NOAA MRMS", "radar storms (US)", "US");
        pick("OPERA", "EUMETNET OPERA", "radar storms (Europe)", "Europe");
        serviceTable(d, "svcr", radar, true);
    }
    panelEnd();
}

} // namespace

std::string flightCategory(const Report& r) {
    const bool haveVis = r.hasVis || r.cavok;
    const int ceil = ceilingFt(r);
    if (!haveVis && ceil < 0 && r.clouds.empty() && !r.clearSky) return "";
    const double sm = r.cavok ? 10.0 : (r.hasVis ? r.visM / 1609.344 : 10.0);
    const int c = ceil < 0 ? 100000 : ceil;
    if (c < 500 || sm < 1.0) return "LIFR";
    if (c < 1000 || sm < 3.0) return "IFR";
    if (c <= 3000 || sm <= 5.0) return "MVFR";
    return "VFR";
}

void uiStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(6, 3);
    s.WindowRounding = 0; s.ChildRounding = 6; s.FrameRounding = 4; s.TabRounding = 5; s.GrabRounding = 4; s.PopupRounding = 4; s.ScrollbarRounding = 6;
    s.WindowBorderSize = 0; s.ChildBorderSize = 1; s.FrameBorderSize = 1; s.TabBorderSize = 0;
    s.ScrollbarSize = 10; s.GrabMinSize = 12;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = v4(cText);
    c[ImGuiCol_TextDisabled] = v4(cMuted);
    c[ImGuiCol_WindowBg] = v4(cBg);
    c[ImGuiCol_ChildBg] = v4(cPanel);
    c[ImGuiCol_PopupBg] = v4(IM_COL32(28, 30, 34, 250));
    c[ImGuiCol_Border] = v4(cBorder);
    c[ImGuiCol_FrameBg] = v4(IM_COL32(38, 40, 45, 255));
    c[ImGuiCol_FrameBgHovered] = v4(IM_COL32(48, 51, 58, 255));
    c[ImGuiCol_FrameBgActive] = v4(IM_COL32(54, 58, 66, 255));
    c[ImGuiCol_Button] = v4(IM_COL32(38, 40, 45, 255));
    c[ImGuiCol_ButtonHovered] = v4(IM_COL32(52, 56, 64, 255));
    c[ImGuiCol_ButtonActive] = v4(IM_COL32(43, 69, 102, 255));
    c[ImGuiCol_Header] = v4(IM_COL32(43, 69, 102, 160));
    c[ImGuiCol_HeaderHovered] = v4(IM_COL32(52, 82, 120, 180));
    c[ImGuiCol_HeaderActive] = v4(IM_COL32(60, 96, 140, 200));
    c[ImGuiCol_Tab] = v4(IM_COL32(40, 44, 52, 255));            // the tabs stand out from the background
    c[ImGuiCol_TabHovered] = v4(IM_COL32(60, 68, 84, 255));
    c[ImGuiCol_TabSelected] = v4(IM_COL32(46, 64, 94, 255));
    c[ImGuiCol_TabSelectedOverline] = v4(cAccent);
    c[ImGuiCol_TabDimmed] = v4(IM_COL32(19, 20, 23, 255));
    c[ImGuiCol_TabDimmedSelected] = v4(cBg);
    c[ImGuiCol_SliderGrab] = v4(cAccent);
    c[ImGuiCol_SliderGrabActive] = v4(IM_COL32(127, 182, 255, 255));
    c[ImGuiCol_CheckMark] = v4(cAccent);
    c[ImGuiCol_TableHeaderBg] = v4(cPanel);
    c[ImGuiCol_TableBorderLight] = v4(cBorder);
    c[ImGuiCol_TableBorderStrong] = v4(cBorder);
    c[ImGuiCol_TableRowBg] = v4(IM_COL32(0, 0, 0, 0));
    c[ImGuiCol_TableRowBgAlt] = v4(IM_COL32(255, 255, 255, 8));
    c[ImGuiCol_Separator] = v4(cBorder);
    c[ImGuiCol_ScrollbarBg] = v4(IM_COL32(0, 0, 0, 0));
    c[ImGuiCol_ScrollbarGrab] = v4(IM_COL32(64, 68, 76, 255));
}

bool uiLogoRgba(std::vector<unsigned char>& rgba, int& w, int& h) {
    PngImage img;
    if (!decodePng(wx_font_logo, wx_font_logo_size, img) || img.depth != 8 || (img.channels != 4 && img.channels != 3)) return false;
    w = img.w; h = img.h;
    rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i)
        for (int k = 0; k < 4; ++k) rgba[i * 4 + k] = k < img.channels ? img.px[i * img.channels + k] : 255;
    return true;
}

std::vector<UiImage> uiLogoMips(const std::vector<unsigned char>& rgba, int w, int h) {
    std::vector<UiImage> out;
    UiImage cur; cur.w = w; cur.h = h; cur.rgba = rgba;
    while (cur.h > 8 && cur.w > 8) {
        UiImage nx; nx.w = std::max(1, cur.w / 2); nx.h = std::max(1, cur.h / 2);
        nx.rgba.assign((size_t)nx.w * nx.h * 4, 0);
        for (int y = 0; y < nx.h; ++y)
            for (int x = 0; x < nx.w; ++x) {
                double a = 0, r = 0, g = 0, b = 0; int n = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(cur.w - 1, 2 * x + dx), sy = std::min(cur.h - 1, 2 * y + dy);
                    const unsigned char* q = &cur.rgba[((size_t)sy * cur.w + sx) * 4];
                    const double al = q[3] / 255.0;
                    r += q[0] * al; g += q[1] * al; b += q[2] * al; a += al; ++n;
                }
                unsigned char* o = &nx.rgba[((size_t)y * nx.w + x) * 4];
                if (a > 0) { o[0] = (unsigned char)std::lround(r / a); o[1] = (unsigned char)std::lround(g / a); o[2] = (unsigned char)std::lround(b / a); }
                o[3] = (unsigned char)std::lround(255.0 * a / n);
            }
        out.push_back(nx);
        cur = std::move(nx);
    }
    return out;
}

UiFonts uiLoadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   // the data is static
    cfg.OversampleH = 2; cfg.OversampleV = 1;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x2212, 0x2212, 0x2264, 0x2265, 0x25B2, 0x25B2, 0x25BC, 0x25BC, 0};
    const float s = std::max(1.0f, pixelScale);
    UiFonts f;
    f.body = io.Fonts->AddFontFromMemoryTTF((void*)wx_font_plex_regular, (int)wx_font_plex_regular_size, 14.5f * s, &cfg, ranges);
    f.bold = io.Fonts->AddFontFromMemoryTTF((void*)wx_font_plex_semibold, (int)wx_font_plex_semibold_size, 14.5f * s, &cfg, ranges);
    f.title = io.Fonts->AddFontFromMemoryTTF((void*)wx_font_plex_bold, (int)wx_font_plex_bold_size, 20.0f * s, &cfg, ranges);
    f.mono = io.Fonts->AddFontFromMemoryTTF((void*)wx_font_mono, (int)wx_font_mono_size, 13.0f * s, &cfg, ranges);
    tabFontBig() = io.Fonts->AddFontFromMemoryTTF((void*)wx_font_plex_semibold, (int)wx_font_plex_semibold_size, 16.0f * s, &cfg, ranges);
    io.FontDefault = f.body;
    io.FontGlobalScale = 1.0f / s;
    return f;
}

// Refresh and Pop out: two tabs at the right end of the tab row (standing on the line, like the others: the same width, font, colours
// and text offset), clickable as buttons. Narrower (to their text) when the window is too narrow for both rows of tabs side by side.
float sideTabW() { return uniformTabWidth(); }
float sideTabsWidth() { return 2 * sideTabW() + ImGui::GetStyle().ItemInnerSpacing.x; }
void sideTabs(float top, float bottom, float right, float leftLimit, bool poppedOut, UiActions& act) {
    float w = sideTabW();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    if (right - 2 * w - gap < leftLimit) w = std::max(tabWidth("Pop out"), (right - gap - leftLimit) * 0.5f);
    ImFont* tf = tabFont() ? tabFont() : ImGui::GetFont();
    const float fs = tf->FontSize * ImGui::GetIO().FontGlobalScale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 keep = ImGui::GetCursorScreenPos();
    auto tab = [&](const char* id, const char* label, float x0) {
        const ImVec2 r0(x0, top), r1(x0 + w, bottom);
        ImGui::SetCursorScreenPos(r0);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, bottom - top));
        const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        dl->AddRectFilled(r0, r1, ImGui::GetColorU32(held ? ImGuiCol_TabSelected : (hov ? ImGuiCol_TabHovered : ImGuiCol_Tab)), ImGui::GetStyle().TabRounding, ImDrawFlags_RoundCornersTop);
        dl->PushClipRect(r0, ImVec2(r1.x - 1, r1.y), true);
        dl->AddText(tf, fs, ImVec2(r0.x + ImGui::GetStyle().FramePadding.x, r0.y + kTabPad.y), ImGui::GetColorU32(ImGuiCol_Text), label);   // where the page tabs' labels start (ImGui uses the tab bar's FramePadding)
        dl->PopClipRect();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return clicked;
    };
    if (tab("##refreshtab", "Refresh", right - 2 * w - gap)) act.refresh = true;
    if (tab("##popouttab", poppedOut ? "Pop in" : "Pop out", right - w)) act.popOut = true;
    ImGui::SetCursorScreenPos(keep);
}

void uiDraw(const UiData& d, const UiFonts& f, float width, float height, bool poppedOut, UiActions& act) {
    s_drawData = &d;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;   // the window itself never scrolls (only lists inside it do)
    ImGui::Begin("WXPlane", nullptr, wf);
    if (d.forceLayers >= 0) { showMap() = (d.forceLayers & 1) != 0; showTurb() = (d.forceLayers & 2) != 0; showPireps() = (d.forceLayers & 4) != 0; }
    if (d.forceDial >= 0) dialHeadingUp() = d.forceDial == 1;
    if (d.forceAdv >= 0) showAirmets() = showSigmets() = d.forceAdv == 1;
    if (d.forceAloftLayers >= 0) { showWinds() = (d.forceAloftLayers & 1) != 0; showClouds() = (d.forceAloftLayers & 2) != 0; showTimeZones() = (d.forceAloftLayers & 4) != 0; }
    if (d.forceRange >= 0) { radarRange() = d.forceRange; mapPan() = ImVec2((float)d.forcePanEastNm, (float)d.forcePanNorthNm); recentering() = false; }
    if (d.forceLocalTime >= 0) mapLocalTime() = d.forceLocalTime == 1;
    updateViewRot(d);
    gSample(d);   // the Turbulence tab's g meter keeps recording on every tab
    updateShownRange();
    updateMapAlt(d);
    updateWinds(d);
    act.mapAltFt = !showTurb() ? -1.0 : (composite() ? -2.0 : mapAlt().shown);   // -2: the composite (worst level in the column)
    // X-Plane's own real weather on: nothing in the window can be clicked, and a red warning covers it (finish(), below)
    if (d.xpRealWeather) ImGui::BeginDisabled();
    auto finish = [&]() {
        if (!d.xpRealWeather) { ImGui::End(); return; }
        ImGui::EndDisabled();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        fg->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(10, 10, 12, 170));
        const float bw = std::min(ws.x - 40, 720.0f), bh = 170;
        const ImVec2 b0(wp.x + (ws.x - bw) * 0.5f, wp.y + (ws.y - bh) * 0.5f), b1(b0.x + bw, b0.y + bh);
        fg->AddRectFilled(b0, b1, IM_COL32(150, 24, 24, 245), 10.0f);
        fg->AddRect(b0, b1, IM_COL32(255, 120, 120, 255), 10.0f, 0, 2.0f);
        const float big = ImGui::GetFontSize() * 1.9f, mid = ImGui::GetFontSize() * 1.05f;
        const char* l1 = "X-PLANE REAL WEATHER IS ON";
        const char* l2 = "WXPlane cannot work while X-Plane downloads its own weather.";
        const char* l3 = "Open X-Plane's weather settings and choose a weather source other than Real Weather.";
        const ImVec2 t1 = f.title->CalcTextSizeA(big, FLT_MAX, 0, l1), t2 = f.body->CalcTextSizeA(mid, FLT_MAX, 0, l2), t3 = f.body->CalcTextSizeA(mid, FLT_MAX, 0, l3);
        fg->AddText(f.title, big, ImVec2(b0.x + (bw - t1.x) * 0.5f, b0.y + 30), IM_COL32(255, 255, 255, 255), l1);
        fg->AddText(f.body, mid, ImVec2(b0.x + (bw - t2.x) * 0.5f, b0.y + 30 + t1.y + 18), IM_COL32(255, 225, 225, 255), l2);
        fg->AddText(f.body, mid, ImVec2(b0.x + (bw - t3.x) * 0.5f, b0.y + 30 + t1.y + 18 + t2.y + 6), IM_COL32(255, 225, 225, 255), l3);
        ImGui::End();
    };
    // header: where, when, refresh, pop out
    {
        const Station* st = nearestStation(d);
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        if (f.logo) {   // the WXPLANE logo (assets/wxplane_logo.png), about the height of a button row
            const float h = ImGui::GetFrameHeight() + 2;
            // on whole pixels, so its edges are not smeared over two
            const float ps = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            cp.x = std::round(cp.x * ps) / ps; cp.y = std::round(cp.y * ps) / ps;
            ImGui::SetCursorScreenPos(cp);
            ImGui::Image((ImTextureID)(intptr_t)f.logo, ImVec2(std::round(h * f.logoAspect * ps) / ps, std::round(h * ps) / ps));
        } else {
            ImGui::PushFont(f.title);
            ImGui::TextUnformatted("WXPlane");
            ImGui::PopFont();
        }
        (void)st;
        // the status in a dark bubble over the Refresh and Pop out tabs (as wide as the two of them), like the map's bubbles
        const float bw = sideTabsWidth();
        const bool loading = d.loadingS >= 0;
        std::string live;
        if (loading) {   // the weather is being built from scratch: count down to when it should all be in
            const int t = (int)std::ceil(d.loadingS);
            live = (d.loadingWhat.empty() ? std::string("Loading weather") : d.loadingWhat) + fmt(" %d:%02d", t / 60, t % 60);
        } else {
            live = d.busy ? std::string("Downloading…") : (d.nextUpdateS >= 0 ? fmt("Next update %d:%02d", (int)d.nextUpdateS / 60, (int)d.nextUpdateS % 60) : std::string(""));
            if (d.haveData && d.res) live += (live.empty() ? "" : " ") + std::string("(updated ") + hhmm(d.res->fetchedAt) + ")";
        }
        ImGui::SameLine(right - bw);
        {
            const ImVec2 b0 = ImGui::GetCursorScreenPos();
            const float bh = ImGui::GetFrameHeight();
            const ImVec2 b1(b0.x + bw, b0.y + bh);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(b0, b1, IM_COL32(20, 22, 26, 220), 4.0f);
            dl->AddRect(b0, b1, IM_COL32(60, 64, 72, 255), 4.0f, 0, 1.0f);
            dl->PushClipRect(b0, ImVec2(b1.x - 4, b1.y), true);
            const float tx = b0.x + ImGui::GetStyle().FramePadding.x, ty = b0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f;   // the tabs' left text offset
            if (loading) {   // a turning ring, then the countdown in amber
                const float r = 6.0f;
                const ImVec2 c(tx + r, b0.y + bh * 0.5f);
                dl->AddCircle(c, r, IM_COL32(80, 66, 40, 255), 24, 2.0f);
                const float a0 = (float)std::fmod(ImGui::GetTime() * 5.0, 2.0 * kPi);
                dl->PathArcTo(c, r, a0, a0 + 1.9f, 12);
                dl->PathStroke(cWarn, ImDrawFlags_None, 2.0f);
                dl->AddText(ImVec2(tx + 2 * r + 8, ty), cWarn, live.c_str());
            } else if (!live.empty()) {
                const SvcState sv = d.busy ? SvcState::Degraded : SvcState::Ok;
                dl->AddCircleFilled(ImVec2(tx + 4, b0.y + bh * 0.5f), 4.0f, sv == SvcState::Ok ? IM_COL32(76, 175, 80, 255) : cWarn, 16);
                dl->AddText(ImVec2(tx + 14, ty), cMuted, live.c_str());
            }
            dl->PopClipRect();
            ImGui::Dummy(ImVec2(bw, bh));
        }
    }

    if (!d.haveData || !d.res) {
        {   // no tabs yet: the tab row with only Refresh and Pop out, on the line
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
            ImFont* tf = tabFont() ? tabFont() : (tabFontBig() ? tabFontBig() : f.bold);
            const float h = tf->FontSize * ImGui::GetIO().FontGlobalScale + 2 * kTabPad.y;
            const ImVec2 q = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImFont*& tfr = tabFont(); ImFont* was = tfr; tfr = tf;
            sideTabs(q.y, q.y + h, q.x + w, q.x, poppedOut, act);
            tfr = was;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(q.x, q.y + h + 0.5f), ImVec2(q.x + w, q.y + h + 0.5f), IM_COL32(58, 62, 70, 255), 1.0f);
            ImGui::Dummy(ImVec2(w, h));
        }
        ImGui::Spacing();
        mutedWrapped(d.status1.empty() ? std::string("No weather downloaded yet.") : d.status1 + (d.status2.empty() ? "" : "  ·  " + d.status2));
        ImGui::Spacing();
        tabSettings(d, f, act);
        finish();
        return;
    }
    int bad = 0;
    for (const auto& s : allServices(d)) if (s.state == SvcState::Down || s.state == SvcState::Degraded) ++bad;
    tabFont() = tabFontBig() ? tabFontBig() : f.bold;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);   // the gap between the logo row and the tabs as between the tabs and the panels
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
    // the tab bar ends where Refresh and Pop out begin, so in a narrow window ImGui narrows the page tabs instead of running them under those
    bool tabBarOpen = false;
    {
        ImGuiContext& g = *GImGui;
        ImGuiWindow* win = g.CurrentWindow;
        const ImGuiID id = win->GetID("tabs");
        ImGuiTabBar* tb = g.TabBars.GetOrAddByKey(id);
        const float maxX = std::max(win->DC.CursorPos.x + 200.0f, win->WorkRect.Max.x - sideTabsWidth() - 12.0f);
        const ImRect bb(win->DC.CursorPos.x, win->DC.CursorPos.y, maxX, win->DC.CursorPos.y + g.FontSize + g.Style.FramePadding.y * 2);
        tb->ID = id;
        tb->SeparatorMinX = tb->BarRect.Min.x - IM_TRUNC(win->WindowPadding.x * 0.5f);
        tb->SeparatorMaxX = tb->BarRect.Max.x + IM_TRUNC(win->WindowPadding.x * 0.5f);
        tabBarOpen = !win->SkipItems && ImGui::BeginTabBarEx(tb, bb, ImGuiTabBarFlags_None | ImGuiTabBarFlags_IsFocused);
    }
    ImGui::PopStyleVar();
    ImRect tabsBar;
    float tabsEnd = 0;
    if (tabBarOpen) {
        {   // the line across the window runs right along the bottom of the tabs (the tabs stand on it)
            const ImRect bar = GImGui->CurrentTabBar->BarRect;
            tabsBar = bar;
            tabsEnd = bar.Min.x + GImGui->CurrentTabBar->WidthAllTabs;
            const float x0 = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x;
            const float x1 = ImGui::GetCurrentWindow()->WorkRect.Max.x;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, bar.Max.y + 0.5f), ImVec2(x1, bar.Max.y + 0.5f), IM_COL32(58, 62, 70, 255), 1.0f);
        }
        if (bigTab("Overview", nullptr, d.forceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabOverview(d, f, act); ImGui::EndTabItem(); }
        if (bigTab("Map", nullptr, d.forceTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabRadar(d, f); ImGui::EndTabItem(); }
        if (bigTab("Airports###air", nullptr, d.forceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabAirports(d, f, act); ImGui::EndTabItem(); }
        if (bigTab("Winds & Temps", nullptr, d.forceTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabAloft(d, f); ImGui::EndTabItem(); }
        {   // the Turbulence tab only while turbulence & windshear are on; it fades out (and back in) instead of vanishing
            static float vis = -1;
            const bool want = !d.cfg || d.cfg->realisticTurbulence || d.cfg->windshear;
            if (vis < 0) vis = want ? 1.0f : 0.0f;
            const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 20.0f);   // a slow frame does not make it jump
            vis = want ? std::min(1.0f, vis + dt / 0.45f) : std::max(0.0f, vis - dt / 0.45f);   // 0.45 s each way
            if (vis > 0.0f) {
                // the tab narrows (and widens as it comes back) at exactly the pace the tabs after it slide over: it is not faded
                // separately, its label is simply cut off by its edge as it closes (drawn here, clipped to the tab, so ImGui's "..."
                // never shows); eased at both ends (smootherstep)
                const float full = uniformTabWidth();
                const float e = vis * vis * vis * (vis * (vis * 6 - 15) + 10);
                const bool open = bigTab("###turbtab", nullptr, d.forceTab == 4 ? ImGuiTabItemFlags_SetSelected : 0, std::max(1.0f, full * e));
                {
                    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                    ImFont* tf = tabFont() ? tabFont() : ImGui::GetFont();
                    ImDrawList* tdl = ImGui::GetWindowDrawList();
                    tdl->PushClipRect(r0, ImVec2(r1.x - 1, r1.y), true);
                    const float fs = tf->FontSize * ImGui::GetIO().FontGlobalScale;
                    tdl->AddText(tf, fs, ImVec2(r0.x + ImGui::GetStyle().FramePadding.x, r0.y + (r1.y - r0.y - fs) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), "Turbulence");   // where the other tabs' labels start
                    tdl->PopClipRect();
                }
                if (open) { ImGui::Spacing(); tabTurbulence(d, f, act); ImGui::EndTabItem(); }
            }
        }
        if (bigTab("Services###svc", nullptr, d.forceTab == 5 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabServices(d, f); ImGui::EndTabItem(); }
        if (bigTab("Settings", nullptr, d.forceTab == 6 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabSettings(d, f, act); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
        sideTabs(tabsBar.Min.y, tabsBar.Max.y, ImGui::GetCurrentWindow()->WorkRect.Max.x, tabsEnd + 12, poppedOut, act);
    }
    finish();
}

} // namespace wx
