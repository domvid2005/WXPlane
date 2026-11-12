#include "wxui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "geo.h"
#include "imgui.h"
#include "opera.h"
#include "png.h"
#include "version.h"
#include "radar.h"
#include "viewer.h"
#include "wxmodel.h"

// fonts embedded at build time (cmake/embed_fonts.cmake)
extern "C" {
extern const unsigned char wx_font_plex_regular[]; extern const unsigned int wx_font_plex_regular_size;
extern const unsigned char wx_font_plex_semibold[]; extern const unsigned int wx_font_plex_semibold_size;
extern const unsigned char wx_font_plex_bold[]; extern const unsigned int wx_font_plex_bold_size;
extern const unsigned char wx_font_mono[]; extern const unsigned int wx_font_mono_size;
extern const unsigned char wx_font_basemap[]; extern const unsigned int wx_font_basemap_size;
extern const unsigned char wx_font_logo[]; extern const unsigned int wx_font_logo_size;
}

namespace wx {
namespace {

const double kPi = 3.14159265358979323846;

// palette
const ImU32 cBg = IM_COL32(23, 24, 27, 255), cPanel = IM_COL32(33, 35, 40, 255), cInset = IM_COL32(24, 25, 29, 255),
            cBorder = IM_COL32(46, 49, 56, 255), cText = IM_COL32(230, 231, 234, 255), cMuted = IM_COL32(154, 160, 168, 255),
            cSoft = IM_COL32(196, 200, 207, 255), cAccent = IM_COL32(76, 154, 255, 255), cAccentDim = IM_COL32(47, 86, 135, 255),
            cOk = IM_COL32(63, 185, 80, 255), cWarn = IM_COL32(242, 180, 90, 255), cDown = IM_COL32(248, 96, 96, 255),
            cIdle = IM_COL32(107, 114, 124, 255), cRing = IM_COL32(42, 47, 54, 255), cMetar = IM_COL32(207, 232, 168, 255);

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::string hhmm(std::time_t t) {
    if (t <= 0) return "--:--Z";
    return fmt("%02d:%02dZ", (int)((t / 3600) % 24), (int)((t / 60) % 60));
}

// Column headers for tables that cannot be sorted: plain text, no hover highlight (only sortable headers react to the mouse)
void plainHeaders() {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < ImGui::TableGetColumnCount(); ++c) { ImGui::TableSetColumnIndex(c); ImGui::TextUnformatted(ImGui::TableGetColumnName(c)); }
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
    std::string s = r.windVariable ? std::string("VRB") : fmt("%03d°", r.windDir);
    s += fmt(" / %.0f", r.windKt);
    if (r.gustKt > r.windKt) s += fmt(" G%.0f", r.gustKt);
    return s + " kt";
}

std::string windShort(const Report& r) {
    if (!r.hasWind) return "—";
    if (r.windKt < 1) return "calm";
    std::string s = r.windVariable ? std::string("VRB") : fmt("%03d", r.windDir);
    s += fmt("/%.0f", r.windKt);
    if (r.gustKt > r.windKt) s += fmt(" G%.0f", r.gustKt);
    return s;
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

void windDial(const UiData& d, const UiFonts& f, const Report& r, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // the whole dial is a button: north up <-> aircraft heading up
    ImGui::InvisibleButton("winddial", ImVec2(size, size));
    if (ImGui::IsItemClicked()) dialHeadingUp() = !dialHeadingUp();
    if (d.forceDial >= 0) dialHeadingUp() = d.forceDial == 1;
    const bool hovered = ImGui::IsItemHovered();
    const bool up = dialHeadingUp() && d.havePos;
    const double rot = viewRot();   // screen-up direction, degrees true (animated)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float R = size * 0.5f - 2;
    const float hub = 26.0f;   // the middle circle: the same size in both modes, big enough for the components
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    dl->AddCircleFilled(c, R, cInset, 64);
    dl->AddCircle(c, R, hovered ? IM_COL32(90, 130, 190, 255) : IM_COL32(58, 62, 70, 255), 64, 1.5f);
    dl->AddCircle(c, R * 0.72f, cRing, 64, 1.0f);
    auto onDial = [&](double trueDeg, float rad) {
        const double a = (trueDeg - rot) * kPi / 180.0;
        return ImVec2(c.x + (float)std::sin(a) * rad, c.y - (float)std::cos(a) * rad);
    };
    const char* nesw[] = {"N", "E", "S", "W"};
    for (int k = 0; k < 4; ++k) {
        const ImVec2 ts = ImGui::CalcTextSize(nesw[k]);
        const ImVec2 q = onDial(k * 90.0, R - 9);
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
        const ImVec2 tail(c.x + dir.x * (R - 16), c.y + dir.y * (R - 16)), tip(c.x + dir.x * (hub + 2), c.y + dir.y * (hub + 2)),
                     base(c.x + dir.x * (hub + 15), c.y + dir.y * (hub + 15));
        dl->AddLine(tail, base, cAccent, 4.0f);
        dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * 7, base.y + n.y * 7), ImVec2(base.x - n.x * 7, base.y - n.y * 7), cAccent);
    }
    dl->AddCircleFilled(c, hub, cPanel, 48);
    dl->AddCircle(c, hub, IM_COL32(58, 62, 70, 255), 48);
    auto centred = [&](ImFont* font, float px, const std::string& t, float y, ImU32 col) {
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, t.c_str());
        dl->AddText(font, px, ImVec2(c.x - ts.x * 0.5f, y - ts.y * 0.5f), col, t.c_str());
    };
    const float fs = ImGui::GetFontSize();
    if (up && r.hasWind) {
        double head = 0, cross = 0;
        if (steady) windComponents(r.windDir, r.windKt, d.headingDeg, head, cross);
        const std::string hw = !steady ? std::string("VRB") : (std::fabs(head) < 0.5 ? std::string("H 0") : fmt("%s %.0f", head >= 0 ? "H" : "T", std::fabs(head)));
        const std::string xw = !steady ? fmt("%.0f kt", r.windKt) : (std::fabs(cross) < 0.5 ? std::string("X 0") : fmt("X %.0f%s", std::fabs(cross), cross > 0 ? "R" : "L"));
        centred(f.bold, fs, hw, c.y - fs * 0.5f, steady && head < -0.5 ? cWarn : IM_COL32(255, 255, 255, 255));
        centred(f.bold, fs, xw, c.y + fs * 0.5f, steady && std::fabs(cross) >= 15 ? cWarn : cSoft);
    } else {
        const std::string kt = !r.hasWind ? std::string("—") : (r.windKt < 1 ? std::string("0") : fmt("%.0f", r.windKt));
        centred(f.title, ImGui::GetFontSize() * 20.0f / 14.5f, kt, c.y - fs * 0.15f, IM_COL32(255, 255, 255, 255));
        centred(f.body, fs * 0.75f, "kt", c.y + fs * 0.75f, cMuted);
    }
    if (hovered) ImGui::SetTooltip(dialHeadingUp() ? "Airplane Heading Lock" : "North Lock");
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
    std::string sub = fmt("%.0f nm · field %.0f ft", distTo(d, st), st.elevM / 0.3048);
    if (st.hasAtis) sub += "  · ATIS " + (st.atisLetter.empty() ? std::string("?") : st.atisLetter);
    muted(sub.c_str());
    badgeRight(f, flightCategory(r), lineY + 1);
    // raw report
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cInset));
    ImGui::PushFont(f.mono);
    const float w = ImGui::GetContentRegionAvail().x;
    const std::string raw = st.rawMetar.empty() ? std::string("(no report text)") : st.rawMetar;
    const float th = ImGui::CalcTextSize(raw.c_str(), nullptr, false, w - 20).y;
    ImGui::BeginChild("raw", ImVec2(w, th + 16), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMetar));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(raw.c_str());
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
        ImGui::TableNextColumn(); field(f, "Wind", windText(r));
        ImGui::TableNextColumn(); field(f, "Visibility", visText(r, us));
        ImGui::TableNextColumn(); field(f, "Weather", weatherText(r));
        ImGui::TableNextColumn(); field(f, us ? "Altimeter" : "QNH", !r.hasAltim ? std::string("—") : (us ? fmt("%.2f inHg", r.altimHpa / 33.8639) : fmt("%.0f hPa", r.altimHpa)));
        ImGui::TableNextColumn(); field(f, "Temp / Dew", r.hasTemp ? (r.hasDewp ? fmt("%.0f / %.0f °C", r.tempC, r.dewpC) : fmt("%.0f °C", r.tempC)) : std::string("—"));
        const int ceil = ceilingFt(r);
        ImGui::TableNextColumn(); field(f, "Ceiling", ceil < 0 ? std::string("None") : ftText(ceil) + " ft");
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    muted("Clouds"); ImGui::SameLine(); ImGui::TextUnformatted(cloudText(r).c_str());
    if (!st.filled.empty()) mutedWrapped("Completed: " + st.filled);
    if (full && st.hasAtis) {
        ImGui::Spacing();
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
const double kRanges[] = {1, 2, 5, 10, 20, 40, 80, 160};   // nm, the mouse wheel steps through these
const int kRangeCount = 8;
int& radarRange() { static int r = 6; return r; }   // index into kRanges (80 nm)
double rangeNmOf(int k) { return kRanges[std::max(0, std::min(kRangeCount - 1, k))]; }

// the pilot report open in the radar page's info window (identified by its text and time, the list is rebuilt every update)
// What is open in the radar page's info window: a pilot report, an airport or an advisory, identified by a key that survives the
// lists being rebuilt at every update (text + time, the ICAO code, name + start time)
struct MapSel { int kind = 0; std::string key; ImVec2 at; ImVec2 mapMin, mapMax; ImVec2 lastSize = ImVec2(300, 200); };   // kind: 0 none, 1 pilot report, 2 airport, 3 AIRMET/SIGMET
MapSel& mapSel() { static MapSel s; return s; }
std::string pirepKey(const hz::Pirep& p) { return p.raw + "|" + std::to_string((long long)p.obsTime); }
std::string advKey(const hz::Advisory& a) { return a.name + "|" + std::to_string((long long)a.from) + "|" + std::to_string(a.pts.size()); }

bool& showAirports() { static bool v = true; return v; }
bool& showRadar() { static bool v = true; return v; }   // the radar echoes (storm cells)
// The Map tab's view can be dragged away from the aircraft: the offset of its centre, nm east and north (the Overview's map always
// stays on the aircraft)
ImVec2& mapPan() { static ImVec2 v(0, 0); return v; }
bool& recentering() { static bool v = false; return v; }   // "Center on aircraft": the map glides back (updateShownRange)
// The range shown, eased towards the chosen one (in log space) like the North/Heading Lock turn
float& shownRange() { static float r = 80; return r; }
void updateShownRange() {
    const float dt = std::max(0.0f, std::min(0.25f, ImGui::GetIO().DeltaTime));
    const float target = (float)rangeNmOf(radarRange());
    const float k = 1.0f - std::exp(-dt * 9.0f);
    const float a = std::log(shownRange()), b = std::log(target);
    shownRange() = std::fabs(a - b) < 0.002f ? target : std::exp(a + (b - a) * k);
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
    const double rot = viewRot() * kPi / 180.0;
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
    // turbulence at the aircraft's altitude
    if (showTurb()) {
        const ImDrawListFlags fl = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // the squares meet edge to edge, with no seams
        const double hy = d.turbGridDLat * 0.5, hx = d.turbGridDLon * 0.5;
        for (const UiTurbCell& g : d.turbGrid) {
            if (g.edr < 0.08) continue;
            const ImVec2 q = at(g.lat, g.lon);
            if (!inside(q, 12.0f * px)) continue;
            // corners from the grid's own spacing: neighbouring squares share them exactly (no seams, no overlaps)
            dl->AddQuadFilled(at(g.lat + hy, g.lon - hx), at(g.lat + hy, g.lon + hx), at(g.lat - hy, g.lon + hx), at(g.lat - hy, g.lon - hx), edrColor(g.edr, 64));
        }
        dl->Flags = fl;
    }
    // echoes
    if (showRadar() && d.res && d.res->radar.ok) {
        const double cell = d.cfg ? d.cfg->radarCellNm : 6.0;
        std::vector<const RadarCell*> cs;
        for (const auto& k : d.res->radar.cells) cs.push_back(&k);
        std::sort(cs.begin(), cs.end(), [](const RadarCell* a, const RadarCell* b) { return a->dbz < b->dbz; });   // strongest on top
        for (const RadarCell* k : cs) {
            if (k->dbz < 10) continue;
            const double sz = k->natural && k->sizeNm > 0 ? k->sizeNm : cell;
            const float rr = std::max(2.0f, (float)(sz * 0.62 * px));
            const ImVec2 q = at(k->lat, k->lon);
            if (!inside(q, rr)) continue;
            dl->AddCircleFilled(q, rr, dbzColor(k->dbz, k->synthetic ? 120 : 190), 20);
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
                const bool sel = mapSel().kind == 3 && mapSel().key == advKey(adv);
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
    // points (pilot reports, airports) come before the areas: they stay clickable inside an AIRMET or SIGMET
    float bestPt = 10.0f;
    if (showPireps() && d.reports) {
        for (const auto& pr : d.reports->pireps) {
            const ImVec2 q = at(pr.lat, pr.lon);
            if (!inside(q, 6)) continue;
            const ImU32 col = pr.urgent ? IM_COL32(235, 64, 64, 255) : IM_COL32(240, 150, 40, 255);   // UUA red, UA orange
            if (interactive && d.forcePirep >= 0 && &pr == &d.reports->pireps[(size_t)std::min<int>(d.forcePirep, (int)d.reports->pireps.size() - 1)]) { mapSel().kind = 1; mapSel().key = pirepKey(pr); }
            const bool sel = mapSel().kind == 1 && mapSel().key == pirepKey(pr);
            if (sel) { mapSel().at = q; dl->AddCircle(q, 10.0f, IM_COL32(255, 255, 255, 220), 20, 1.5f); }
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
            const bool sel = mapSel().kind == 2 && mapSel().key == st.icao;
            if (sel) { mapSel().at = q; dl->AddCircle(q, 8.0f, IM_COL32(255, 255, 255, 220), 16, 1.5f); }
            dl->AddCircleFilled(q, 3.5f, catColor(flightCategory(st.report)), 12);
            if (rangeNm <= 40) dl->AddText(f.body, ImGui::GetFontSize() * 0.75f, ImVec2(q.x + 5, q.y - 6), cMuted, st.icao.c_str());
            const float dm = std::hypot(mouse.x - q.x, mouse.y - q.y);
            if (interactive && dm < bestPt) { bestPt = dm; hitKind = 2; hitKey = st.icao; hitAt = q; }
        }
    // the aircraft: its true heading (not the ground track, which swings around when slow or stopped)
    const double a = (d.headingDeg - viewRot()) * kPi / 180.0;
    const ImVec2 dir((float)std::sin(a), -(float)std::cos(a)), n(-dir.y, dir.x);
    dl->AddTriangleFilled(ImVec2(ac.x + dir.x * 10, ac.y + dir.y * 10), ImVec2(ac.x - dir.x * 7 + n.x * 6, ac.y - dir.y * 7 + n.y * 6),
                          ImVec2(ac.x - dir.x * 7 - n.x * 6, ac.y - dir.y * 7 - n.y * 6), IM_COL32(255, 255, 255, 255));
    dl->PopClipRect();
    ImGui::InvisibleButton("radarclick", ImVec2(size, size));
    if (interactive) { mapSel().mapMin = p; mapSel().mapMax = ImVec2(p.x + size, p.y + size); }   // the info window stays inside this
    const bool hov = ImGui::IsItemHovered();
    // the mouse wheel zooms: 1, 2, 5, 10, 20, 40, 80, 160 nm (eased)
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
        if (ImGui::IsItemDeactivated() && !dragged && hov) {
            if (hitKind) { mapSel().kind = hitKind; mapSel().key = hitKey; mapSel().at = hitAt; }
            else mapSel().kind = 0;
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

void infoWindow(const UiData& d, const UiFonts& f) {
    MapSel& sel = mapSel();
    if (!sel.kind) return;
    const hz::Pirep* p = nullptr; const Station* st = nullptr; const hz::Advisory* av = nullptr;
    if (sel.kind == 1 && d.reports && showPireps()) for (const auto& q : d.reports->pireps) if (pirepKey(q) == sel.key) p = &q;
    if (sel.kind == 2 && d.res && showAirports()) for (const auto& q : d.res->stations) if (q.icao == sel.key) st = &q;
    if (sel.kind == 3 && d.reports) for (const auto& q : d.reports->advisories) if (advKey(q) == sel.key && (q.cls == hz::Advisory::Airmet ? showAirmets() : showSigmets())) av = &q;
    if (!p && !st && !av) { sel.kind = 0; return; }
    {   // inside the map: to the right of the point if it fits, else to its left; moved up or down to stay within the map
        const ImVec2 mn = sel.mapMin, mx = sel.mapMax, sz = sel.lastSize;
        float x = sel.at.x + 14, y = sel.at.y - 10;
        if (x + sz.x > mx.x - 4) x = sel.at.x - 14 - sz.x;
        x = std::max(mn.x + 4, std::min(x, mx.x - 4 - sz.x));
        y = std::max(mn.y + 4, std::min(y, mx.y - 4 - sz.y));
        ImGui::SetNextWindowPos(ImVec2(x, y));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(160.0f, mx.x - mn.x - 8), std::max(120.0f, mx.y - mn.y - 8)));
    }
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(IM_COL32(28, 30, 34, 255)));
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##mapinfo", nullptr, fl)) {
        auto title = [&](const std::string& t, ImU32 col) {
            ImGui::PushFont(f.bold);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(col));
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine(0, 24);
            if (ImGui::SmallButton("x")) sel.kind = 0;
        };
        if (p) {
            title(p->urgent ? "UUA  urgent pilot report" : (p->kind == "AIREP" ? "AIREP" : "UA  pilot report"), p->urgent ? IM_COL32(235, 64, 64, 255) : IM_COL32(240, 150, 40, 255));
            if (ImGui::BeginTable("pr", 2, ImGuiTableFlags_SizingFixedFit)) {
                if (p->obsTime > 0) infoRow("Time", hhmm(p->obsTime) + fmt("  (%ld min ago)", (long)((d.now - p->obsTime) / 60)));
                infoRow("Level", p->altFt > 0 ? fmt("FL%03.0f", p->altFt / 100.0) : std::string("—"));
                infoRow("Aircraft", p->acType.empty() ? std::string("—") : p->acType);
                std::string tb = p->intensity.empty() ? std::string("none reported") : p->intensity;
                if (!p->tbType.empty()) tb += " " + p->tbType;
                if (p->baseFt > 0 || p->topFt > 0) tb += "  " + levelText(p->baseFt) + "-" + levelText(p->topFt);
                infoRow("Turbulence", tb);
                if (!p->icing.empty()) infoRow("Icing", p->icing);
                if (!p->sky.empty()) infoRow("Clouds", p->sky);
                if (!p->wx.empty()) infoRow("Weather", p->wx);
                if (p->tempC > -100) infoRow("Temperature", fmt("%.0f °C", p->tempC));
                if (p->windKt >= 0) infoRow("Wind", fmt("%03.0f/%.0f kt", std::max(0.0, p->windDir), p->windKt));
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
                infoRow("Wind", windText(st->report));
                infoRow("Visibility", visText(st->report, us));
                infoRow("Weather", weatherText(st->report));
                infoRow("Clouds", cloudText(st->report));
                if (st->report.hasAltim) infoRow(us ? "Altimeter" : "QNH", us ? fmt("%.2f inHg", st->report.altimHpa / 33.8639) : fmt("%.0f hPa", st->report.altimHpa));
                infoRow("From you", fmt("%.0f nm", distTo(d, *st)));
                ImGui::EndTable();
            }
            infoText(f, st->rawMetar.empty() ? std::string("(no report text)") : st->rawMetar, cMetar);
            if (st->hasAtis) infoText(f, "ATIS " + st->atisLetter + ": " + st->atisText, IM_COL32(255, 217, 128, 255));
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
        }
        sel.lastSize = ImGui::GetWindowSize();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
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
    if (v) ImGui::PushStyleColor(ImGuiCol_Button, v4(IM_COL32(43, 69, 102, 255)));
    if (ImGui::SmallButton(label)) v = !v;
    if (v) ImGui::PopStyleColor();
}

void layerButtons(bool lockButton) {
    toggleButton("Radar", showRadar());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Radar echoes (NOAA MRMS in the US, EUMETNET OPERA in Europe)");
    wrapBefore("Map", 4);
    toggleButton("Map", showMap());
    wrapBefore("Airports", 4);
    toggleButton("Airports", showAirports());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Airports by flight category (radar page: click one for its METAR and ATIS)");
    wrapBefore("Turb", 4);
    toggleButton("Turb", showTurb());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turbulence at the aircraft's altitude (realistic turbulence data)");
    wrapBefore("PIREPs", 4);
    toggleButton("PIREPs", showPireps());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pilot reports: urgent (UUA) red, routine (UA) orange, label = flight level");
    wrapBefore("AIRMET", 4);
    toggleButton("AIRMET", showAirmets());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("G-AIRMETs (US): IFR purple, mountain obscuration pink, turbulence yellow, icing light blue, surface wind green, LLWS rose");
    wrapBefore("SIGMET", 4);
    toggleButton("SIGMET", showSigmets());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("SIGMETs worldwide and convective SIGMETs: convective red, turbulence orange, icing blue, volcanic ash brown, cyclone magenta");
    if (lockButton) {   // the Map tab: re-centre after dragging, and (it has no wind dial) the North / Airplane Heading Lock switch
        wrapBefore("Center on aircraft", 12);
        const bool off = std::fabs(mapPan().x) > 0.01f || std::fabs(mapPan().y) > 0.01f;
        if (!off) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Center on aircraft")) recentering() = true;
        if (!off) ImGui::EndDisabled();
        wrapBefore("Heading Lock", 4);
        if (ImGui::SmallButton(dialHeadingUp() ? "Heading Lock###lock" : "North Lock###lock")) dialHeadingUp() = !dialHeadingUp();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(dialHeadingUp() ? "Airplane Heading Lock (click for North Lock)" : "North Lock (click for Airplane Heading Lock)");
    }
}

// The range: the mouse wheel over the map, or these buttons (VR, no wheel)
void rangeButtons() {
    if (ImGui::SmallButton("-")) radarRange() = std::max(0, radarRange() - 1);
    ImGui::SameLine(0, 4);
    const std::string t = fmt("%g nm", rangeNmOf(radarRange()));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(t.c_str());
    ImGui::SameLine(0, 4);
    if (ImGui::SmallButton("+")) radarRange() = std::min(kRangeCount - 1, radarRange() + 1);
}
float rangeButtonsWidth() {
    return ImGui::CalcTextSize("-").x + ImGui::CalcTextSize("+").x + ImGui::CalcTextSize("160 nm").x + 4 * ImGui::GetStyle().FramePadding.x + 8;
}

void radarPanel(const UiData& d, const UiFonts& f, bool big) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    sectionTitle(f, radarHeader(d).c_str());
    ImGui::SameLine(right - rangeButtonsWidth());
    rangeButtons();
    if (big) layerButtons(true);   // the home page's map shows the Map tab's layers, without buttons of its own
    mutedWrapped(radarSubtitle(d));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float hLeft = ImGui::GetContentRegionAvail().y - (big ? 40 : 110);
    const float size = std::max(120.0f, std::min(avail, hLeft));
    if (size < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size) * 0.5f);
    radarCanvas(d, f, size, rangeNmOf(radarRange()), big);
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

void windArrow(float dirDeg, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 c(p.x + size * 0.5f, p.y + h * 0.5f);
    // points where the wind goes (from dirDeg towards dirDeg+180)
    const double a = (dirDeg + 180.0) * kPi / 180.0;
    const ImVec2 dir((float)std::sin(a), -(float)std::cos(a)), n(-dir.y, dir.x);
    const float L = size * 0.45f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 tip(c.x + dir.x * L, c.y + dir.y * L), tail(c.x - dir.x * L, c.y - dir.y * L);
    dl->AddLine(tail, tip, IM_COL32(127, 182, 255, 255), 1.8f);
    dl->AddLine(tip, ImVec2(tip.x - dir.x * 5 + n.x * 4, tip.y - dir.y * 5 + n.y * 4), IM_COL32(127, 182, 255, 255), 1.8f);
    dl->AddLine(tip, ImVec2(tip.x - dir.x * 5 - n.x * 4, tip.y - dir.y * 5 - n.y * 4), IM_COL32(127, 182, 255, 255), 1.8f);
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
    if (!ImGui::BeginTable("aloft", 6, fl)) return;
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, textColW("00000 ft"));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18);
    ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthFixed, textColW("000/000"));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Temp", ImGuiTableColumnFlags_WidthFixed, textColW("-00°"));
    ImGui::TableSetupColumn("ISA", ImGuiTableColumnFlags_WidthFixed, textColW("ISA +00"));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    plainHeaders();
    ImGui::PopStyleColor();
    bool tropoDone = tropoFt < 0;
    for (int i = (int)rows.size() - 1; i >= 0; --i) {   // highest first, like a sounding
        const AloftRow& r = rows[(size_t)i];
        if (!tropoDone && r.ft < tropoFt) {
            tropoDone = true;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cWarn));
            ImGui::TextUnformatted("Tropo");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("FL%03.0f · %.0f °C", tropoFt / 100.0f, tropoC);
            ImGui::PopStyleColor();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::PushFont(f.bold); ImGui::Text(r.ft >= 18000 ? "FL%03.0f" : "%.0f ft", r.ft >= 18000 ? r.ft / 100.0f : r.ft); ImGui::PopFont();
        ImGui::TableSetColumnIndex(1); windArrow(r.dir, 16);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%03.0f/%.0f", r.dir, r.kt);
        ImGui::TableSetColumnIndex(3); speedBar(r.kt, maxKt);
        ImGui::TableSetColumnIndex(4); if (r.hasT) ImGui::Text("%.0f°", r.tempC); else muted("—");
        ImGui::TableSetColumnIndex(5);
        if (r.hasT) { const float dev = r.tempC - isaTempC(r.ft * 0.3048f); ImGui::PushStyleColor(ImGuiCol_Text, v4(cSoft)); ImGui::Text(std::fabs(dev) < 0.5f ? "ISA 0" : "ISA %+.0f", dev); ImGui::PopStyleColor(); }
    }
    ImGui::EndTable();
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
    if (!d.turbData.empty()) { ImGui::SameLine(); muted(d.turbData.c_str()); }
    if (!slider) {   // the home page: three rows
        int withTurb = 0, total = 0;
        if (d.reports) for (const auto& p : d.reports->pireps) { ++total; if (hz::edrForIntensity(p.intensity, p.acType) > 0) ++withTurb; }
        statRow(f, "At the aircraft", d.turbOn ? turbWord(d.turbEdr) : std::string("Off"), d.turbOn && d.turbEdr >= 0 ? fmt("EDR %.2f · %s", d.turbEdr, d.turbSource.c_str()) : std::string());
        statRow(f, "Pilot reports", d.reports ? fmt("%d", total) : std::string("—"), d.reports ? fmt("%d with turbulence", withTurb) : std::string());
        const char* risk = !d.wsOn ? "Off" : (d.wsHigh > 0 ? "High" : (d.wsMed > 0 ? "Medium" : (d.wsSources > 0 ? "Low" : "None")));
        const ImU32 rc = !d.wsOn ? cMuted : (d.wsHigh > 0 ? IM_COL32(245, 70, 70, 255) : (d.wsMed > 0 ? IM_COL32(245, 150, 50, 255) : (d.wsSources > 0 ? IM_COL32(240, 210, 80, 255) : IM_COL32(255, 255, 255, 255))));
        statRow(f, "Windshear risk", risk, d.wsOn ? fmt("%d source(s)", d.wsSources) : std::string(), rc);
        return;
    }
    const float bw = (ImGui::GetContentRegionAvail().x - 16) / 3.0f;
    int withTurb = 0, total = 0;
    if (d.reports) for (const auto& p : d.reports->pireps) { ++total; if (hz::edrForIntensity(p.intensity, p.acType) > 0) ++withTurb; }
    statBox(f, "At the aircraft", d.turbOn ? turbWord(d.turbEdr) : std::string("Off"), d.turbOn && d.turbEdr >= 0 ? fmt("EDR %.2f · %s", d.turbEdr, d.turbSource.c_str()) : std::string(" "), bw);
    ImGui::SameLine(0, 8);
    statBox(f, "Pilot reports", d.reports ? fmt("%d", total) : std::string("—"), d.reports ? fmt("%d with turbulence", withTurb) : std::string(" "), bw);
    ImGui::SameLine(0, 8);
    const char* risk = !d.wsOn ? "Off" : (d.wsHigh > 0 ? "High" : (d.wsMed > 0 ? "Medium" : (d.wsSources > 0 ? "Low" : "None")));
    const ImU32 riskCol = !d.wsOn ? cMuted : (d.wsHigh > 0 ? IM_COL32(245, 70, 70, 255) : (d.wsMed > 0 ? IM_COL32(245, 150, 50, 255) : (d.wsSources > 0 ? IM_COL32(240, 210, 80, 255) : IM_COL32(255, 255, 255, 255))));
    statBox(f, "Windshear risk", risk, d.wsOn ? fmt("%d source(s)", d.wsSources) : std::string(" "), bw, riskCol);
    if (!slider) return;
    ImGui::Spacing();
    float v = d.cfg ? (float)d.cfg->maxTurbulence : 1.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Max turbulence");
    ImGui::SameLine(130);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::SliderFloat("##maxturb", &v, 0.0f, 1.0f, "%.2f")) { act.maxTurbChanged = true; act.maxTurb = std::round(v * 20.0f) / 20.0f; }
    if (ImGui::IsItemDeactivatedAfterEdit()) { act.maxTurbReleased = true; act.maxTurb = std::round(v * 20.0f) / 20.0f; }
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s - %s\n%s%s", s.name.c_str(), s.use.c_str(), s.detail.c_str(),
                                                      s.lastOk > 0 && s.state != SvcState::Ok ? fmt("\nlast good %s", hhmm(s.lastOk).c_str()).c_str() : "");
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
    const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | (scroll ? ImGuiTableFlags_ScrollY : 0);
    if (!ImGui::BeginTable("near", 6, fl)) return;
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, textColW("WWWW"), kSortIcao);
    ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, textColW("000"), kSortDist);
    ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed, textColW("MVFR"), kSortCat);
    ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthStretch, textColW("000/00 G00"), kSortWind);
    ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, textColW("10 km+"), kSortVis);
    ImGui::TableSetupColumn("Ceil.", ImGuiTableColumnFlags_WidthFixed, textColW("00,000"), kSortCeil);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    sortStations(v, d);
    if (maxRows > 0 && (int)v.size() > maxRows) v.resize((size_t)maxRows);
    for (const Station* s : v) {
        const Report& r = s->report;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(s->icao.c_str());
        ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f", distTo(d, *s));
        ImGui::TableSetColumnIndex(2); { const std::string c = flightCategory(r); ImGui::PushStyleColor(ImGuiCol_Text, v4(catColor(c))); ImGui::TextUnformatted(c.empty() ? "—" : c.c_str()); ImGui::PopStyleColor(); }
        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(windShort(r).c_str());
        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(visText(r, usUnits(s->icao)).c_str());
        ImGui::TableSetColumnIndex(5); { const int c = ceilingFt(r); if (c < 0) muted("none"); else ImGui::TextUnformatted(ftText(c).c_str()); }
    }
    ImGui::EndTable();
}

bool panelBegin(const char* id, ImVec2 size, ImGuiWindowFlags extra = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    const bool r = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, extra);
    return r;
}
void panelEnd() { ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); }

// ---- tabs ---------------------------------------------------------------------------------------------------------------------

void tabOverview(const UiData& d, const UiFonts& f, UiActions& act) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12;
    const float colW = std::floor((W - 2 * gap) * 0.31f), col3 = W - 2 * colW - 2 * gap, bodyH = H;
    const Station* st = nearestStation(d);
    // column 1
    ImGui::BeginGroup();
    const float cardH = std::min(bodyH * 0.58f, 330.0f);
    if (panelBegin("card", ImVec2(colW, cardH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) { if (st) stationCard(d, f, *st, false); else mutedWrapped("No airport reports near the aircraft."); }
    panelEnd();
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    if (panelBegin("near", ImVec2(colW, bodyH - cardH - gap), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) { sectionTitle(f, "Nearby airports"); nearbyTable(d, st, 0, true); }
    panelEnd();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    if (panelBegin("radar", ImVec2(colW, bodyH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) radarPanel(d, f, false);
    panelEnd();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float rowH = ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().CellPadding.y;
    const float aloftH = std::min(bodyH - 230.0f, 24.0f + 12.0f + ImGui::GetTextLineHeightWithSpacing() + rowH * 10.0f + 20.0f);   // title, header, 8 levels, tropopause
    if (panelBegin("aloft", ImVec2(col3, aloftH), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
        sectionTitle(f, "Winds & temps aloft");
        ImGui::SameLine(); muted("GFS");
        aloftTable(d, f, {5000, 10000, 18000, 24000, 30000, 34000, 39000, 45000}, false);
    }
    panelEnd();
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    if (panelBegin("turb", ImVec2(col3, bodyH - aloftH - gap), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) turbulenceCard(d, f, act, false);
    panelEnd();
    ImGui::EndGroup();
}

void tabAirports(const UiData& d, const UiFonts& f, UiActions& act) {
    static std::string selected;
    static char query[16] = "";
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float listW = std::floor(W * 0.56f);
    if (panelBegin("alist", ImVec2(listW, H)) && d.res) {
        // any airport in the world: its METAR and ATIS (only shown here, X-Plane's weather is not changed)
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - textColW("Search") - 10));
        const bool enter = ImGui::InputTextWithHint("##search", "Search any airport worldwide (ICAO, e.g. EGLL)", query, sizeof query,
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
        const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
        if (ImGui::BeginTable("all", 8, fl)) {
            ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, textColW("WWWW"), kSortIcao);
            ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, textColW("000"), kSortDist);
            ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed, textColW("MVFR"), kSortCat);
            ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthFixed, textColW("VRB/00 G00"), kSortWind);
            ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, textColW("10 km+"), kSortVis);
            ImGui::TableSetupColumn("Weather", ImGuiTableColumnFlags_WidthFixed, textColW("+TSRA BR"), kSortWx);
            ImGui::TableSetupColumn("Ceiling", ImGuiTableColumnFlags_WidthStretch, textColW("00,000 ft"), kSortCeil);
            ImGui::TableSetupColumn("T/Td", ImGuiTableColumnFlags_WidthFixed, textColW("-00/-00"), kSortTemp);
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
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(windShort(r).c_str());
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(visText(r, usUnits(s->icao)).c_str());
                ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(r.wxText.empty() ? "—" : r.wxText.c_str());
                ImGui::TableSetColumnIndex(6); { const int c = ceilingFt(r); if (c < 0) muted("none"); else ImGui::TextUnformatted((ftText(c) + " ft").c_str()); }
                ImGui::TableSetColumnIndex(7); if (r.hasTemp) ImGui::Text("%.0f/%.0f", r.tempC, r.dewpC); else muted("—");
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

void tabRadar(const UiData& d, const UiFonts& f) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float mapW = std::min(W * 0.62f, H + 40);
    if (panelBegin("rbig", ImVec2(mapW, H), ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) radarPanel(d, f, true);
    panelEnd();
    infoWindow(d, f);
    ImGui::SameLine(0, 12);
    if (panelBegin("rcells", ImVec2(0, H)) && d.res) {
        const RadarResult& r = d.res->radar;
        sectionTitle(f, "Strongest cells");
        if (!r.ok) mutedWrapped(radarSubtitle(d));
        else {
            muted(r.topsFromRadar ? "Storm tops from radar echo tops" : "Storm tops estimated from reflectivity");
            std::vector<const RadarCell*> cs;
            for (const auto& k : r.cells) if (!k.synthetic) cs.push_back(&k);
            std::sort(cs.begin(), cs.end(), [](const RadarCell* a, const RadarCell* b) { return a->dbz > b->dbz; });
            if (ImGui::BeginTable("cells", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("dBZ", ImGuiTableColumnFlags_WidthFixed, textColW("dBZ"));
                ImGui::TableSetupColumn("Rain", ImGuiTableColumnFlags_WidthFixed, textColW("000 mm/h"));
                ImGui::TableSetupColumn("Top", ImGuiTableColumnFlags_WidthFixed, textColW("FL000 est"));
                ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
                plainHeaders();
                ImGui::PopStyleColor();
                for (size_t i = 0; i < cs.size() && i < 60; ++i) {
                    const RadarCell& k = *cs[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::PushStyleColor(ImGuiCol_Text, v4(dbzColor(k.dbz, 255))); ImGui::Text("%.0f", k.dbz); ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f mm/h", k.rateMmH);
                    ImGui::TableSetColumnIndex(2); if (k.topM > 0) ImGui::Text("FL%03.0f%s", k.topM / 0.3048 / 100.0, k.topKnown ? "" : " est"); else muted("—");
                    ImGui::TableSetColumnIndex(3);
                    const double dd = d.havePos ? distNm(d.lat, d.lon, k.lat, k.lon) : k.distNm;
                    double brg = 0;
                    if (d.havePos) { const double dy = k.lat - d.lat, dx = (k.lon - d.lon) * std::cos(d.lat * kPi / 180.0); brg = std::fmod(std::atan2(dx, dy) * 180.0 / kPi + 360.0, 360.0); }
                    ImGui::Text("%.0f nm  %03.0f°", dd, brg);
                }
                ImGui::EndTable();
            }
        }
    }
    panelEnd();
}

void tabAloft(const UiData& d, const UiFonts& f) {
    const float H = ImGui::GetContentRegionAvail().y;
    if (panelBegin("aloftfull", ImVec2(0, H))) {
        sectionTitle(f, "Winds & temperatures aloft at the aircraft");
        const Station* st = nearestStation(d);
        if (st) { ImGui::SameLine(); muted(fmt("GFS, blended for %s", st->icao.c_str()).c_str()); }
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

void tabTurbulence(const UiData& d, const UiFonts& f, UiActions& act) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float leftW = std::floor(W * 0.45f);
    ImGui::BeginGroup();
    if (panelBegin("tcard", ImVec2(leftW, 190))) turbulenceCard(d, f, act, true);
    panelEnd();
    ImGui::Dummy(ImVec2(0, 4));
    if (panelBegin("tprof", ImVec2(leftW, H - 190 - 12))) {
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
    if (panelBegin("treports", ImVec2(0, H))) {
        sectionTitle(f, "Pilot reports nearby");
        if (!d.reports || d.reports->pireps.empty()) mutedWrapped("No pilot reports in the last 90 minutes.");
        else {
            // every report, nearest first; a click on a row opens (and closes) its details below it
            static std::vector<std::string> open;
            const float x0 = ImGui::GetCursorPosX();
            const float cx[6] = {0, textColW("000"), textColW("000") + textColW("FL000"), textColW("000") + textColW("FL000") + textColW("UUA"),
                                 textColW("000") + textColW("FL000") + textColW("UUA") + textColW("MOD-SEV CAT"), textColW("000") + textColW("FL000") + textColW("UUA") + textColW("MOD-SEV CAT") + textColW("B77W")};
            const char* hd[6] = {"nm", "Level", "Type", "Turbulence", "Aircraft", "Age"};
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
            for (int k = 0; k < 6; ++k) { if (k) ImGui::SameLine(x0 + cx[k]); ImGui::TextUnformatted(hd[k]); }
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::BeginChild("pirepList", ImVec2(0, d.wsOn && !d.wsStrongest.empty() ? -40.0f : 0.0f), ImGuiChildFlags_None, 0);
            std::vector<const hz::Pirep*> v;
            for (const auto& p : d.reports->pireps) v.push_back(&p);
            auto dd = [&](const hz::Pirep* p) { return d.havePos ? distNm(d.lat, d.lon, p->lat, p->lon) : 0.0; };
            std::sort(v.begin(), v.end(), [&](const hz::Pirep* a, const hz::Pirep* b) { return dd(a) < dd(b); });
            for (size_t i = 0; i < v.size() && i < 150; ++i) {
                const hz::Pirep& p = *v[i];
                const std::string key = pirepKey(p);
                const auto it = std::find(open.begin(), open.end(), key);
                const bool isOpen = it != open.end();
                ImGui::PushID((int)i);
                const float rx = ImGui::GetCursorPosX();
                if (ImGui::Selectable("##row", isOpen, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    if (isOpen) open.erase(it); else open.push_back(key);
                }
                ImGui::SameLine(rx + cx[0]); ImGui::Text("%.0f", dd(&p));
                ImGui::SameLine(rx + cx[1]); if (p.altFt > 0) ImGui::Text("FL%03.0f", p.altFt / 100.0); else muted("—");
                ImGui::SameLine(rx + cx[2]);
                ImGui::PushStyleColor(ImGuiCol_Text, v4(p.urgent ? IM_COL32(235, 64, 64, 255) : IM_COL32(240, 150, 40, 255)));
                ImGui::TextUnformatted(p.urgent ? "UUA" : (p.kind == "AIREP" ? "ARP" : "UA"));
                ImGui::PopStyleColor();
                ImGui::SameLine(rx + cx[3]);
                const double e = hz::edrForIntensity(p.intensity, p.acType);
                if (p.intensity.empty()) muted(p.llws ? "LLWS" : "—");
                else { ImGui::PushStyleColor(ImGuiCol_Text, v4(e >= 0.22 ? cWarn : (e > 0 ? cSoft : cMuted))); ImGui::TextUnformatted((p.intensity + (p.tbType.empty() ? "" : " " + p.tbType)).c_str()); ImGui::PopStyleColor(); }
                ImGui::SameLine(rx + cx[4]); ImGui::TextUnformatted(p.acType.empty() ? "—" : p.acType.c_str());
                ImGui::SameLine(rx + cx[5]); if (p.obsTime > 0) ImGui::Text("%ld min", (long)((d.now - p.obsTime) / 60)); else muted("—");
                if (isOpen) {   // the details, until the row is clicked again
                    ImGui::Indent(14);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cInset));
                    const float tw = ImGui::GetContentRegionAvail().x - 6;
                    std::string det;
                    if (p.obsTime > 0) det += "Time " + hhmm(p.obsTime) + "    ";
                    if (p.baseFt > 0 || p.topFt > 0) det += "Turbulence layer " + levelText(p.baseFt) + "-" + levelText(p.topFt) + "    ";
                    if (!p.icing.empty()) det += "Icing " + p.icing + "    ";
                    if (!p.sky.empty()) det += "Clouds " + p.sky + "    ";
                    if (!p.wx.empty()) det += "Weather " + p.wx + "    ";
                    if (p.tempC > -100) det += fmt("Temp %.0f °C    ", p.tempC);
                    if (p.windKt >= 0) det += fmt("Wind %03.0f/%.0f kt    ", std::max(0.0, p.windDir), p.windKt);
                    if (p.llws) det += "Low-level wind shear reported";
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
                    if (!det.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, v4(cSoft)); ImGui::TextUnformatted(det.c_str()); ImGui::PopStyleColor(); }
                    ImGui::PushFont(f.mono);
                    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMetar));
                    ImGui::TextUnformatted(p.raw.empty() ? "(no report text)" : p.raw.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    ImGui::Unindent(14);
                    ImGui::Spacing();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        if (d.wsOn && !d.wsStrongest.empty()) { ImGui::Spacing(); mutedWrapped("Windshear: strongest source " + d.wsStrongest); }
    }
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
    const float H = ImGui::GetContentRegionAvail().y, W = ImGui::GetContentRegionAvail().x;
    if (panelBegin("settings", ImVec2(std::min(W, 760.0f), H)) && d.cfg) {
        sectionTitle(f, "Weather");
        settingRow("WXPlane weather on", d.cfg->enabled, "Off hands the weather back to X-Plane (WXPlane's records are removed).", act.toggleEnabled);
        // with the weather off, every other setting is greyed out (dark grey instead of blue) and cannot be changed
        const bool off = !d.cfg->enabled;
        if (off) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_CheckMark, v4(IM_COL32(88, 92, 100, 255)));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, v4(IM_COL32(88, 92, 100, 255)));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(IM_COL32(32, 34, 38, 255)));
        }
        settingRow("Use ATIS", d.cfg->useAtis, "Merge the digital ATIS (D-ATIS, major US airports) over the METAR.", act.toggleAtis);
        settingRow("Global winds/temps & clouds aloft (GFS)", d.cfg->useAloft, "Winds and temperatures from the surface to FL600, clouds above what airports report, and the weather over open water.", act.toggleAloft);
        ImGui::Separator(); ImGui::Spacing();
        sectionTitle(f, "Turbulence & windshear");
        settingRow("Realistic turbulence", d.cfg->realisticTurbulence, "WXPlane sets the turbulence at every altitude from NOAA GTGN (US) / ECMWF (elsewhere) and pilot reports.", act.toggleTurbulence);
        settingRow("Realistic windshear", d.cfg->windshear, "Gust fronts and microbursts near convective storms.", act.toggleWindshear);
        float v = (float)d.cfg->maxTurbulence;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Max turbulence");
        ImGui::SameLine(150);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::SliderFloat("##maxturb2", &v, 0.0f, 1.0f, "%.2f")) { act.maxTurbChanged = true; act.maxTurb = std::round(v * 20.0f) / 20.0f; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { act.maxTurbReleased = true; act.maxTurb = std::round(v * 20.0f) / 20.0f; }
        ImGui::Indent(30);
        mutedWrapped("Scales all of WXPlane's turbulence: real data, storms, gusts. 0 = none, 1.0 = full realism.");
        ImGui::Unindent(30);
        if (off) { ImGui::PopStyleColor(3); ImGui::EndDisabled(); }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        sectionTitle(f, "Radar");
        mutedWrapped("Always on: NOAA MRMS over the US, EUMETNET OPERA over Europe. The Map / Turb / PIREPs buttons choose the map's layers, the mouse wheel zooms (1 to 160 nm), and a click on the wind dial turns the dial and the map between North Lock and Airplane Heading Lock.");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        sectionTitle(f, "Status");
        mutedWrapped(d.status1 + (d.status2.empty() ? "" : "  ·  " + d.status2));
        mutedWrapped("Advanced settings are in WXPlane.cfg (X-Plane's Output/preferences folder).");
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

void tabServices(const UiData& d, const UiFonts& f) {
    const float H = ImGui::GetContentRegionAvail().y;
    if (panelBegin("svcfull", ImVec2(0, H))) {
        sectionTitle(f, "Data services");
        if (ImGui::BeginTable("svct", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 16);
            ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, textColW("NOAA Aviation Weather Center"));
            ImGui::TableSetupColumn("Gives", ImGuiTableColumnFlags_WidthFixed, textColW("clouds aloft, open-water weather"));
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
            plainHeaders();
            ImGui::PopStyleColor();
            for (const auto& s : allServices(d)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); statusDot(s.state);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableSetColumnIndex(2); muted(s.use.c_str());
                ImGui::TableSetColumnIndex(3);
                std::string t = std::string(svcLabel(s.state)) + (s.detail.empty() ? "" : ": " + s.detail);
                if ((s.state == SvcState::Down || s.state == SvcState::Degraded) && s.lastOk > 0)
                    t += fmt(" (last good %s, %ld min ago)", hhmm(s.lastOk).c_str(), (long)((d.now - s.lastOk) / 60));
                ImGui::TextWrapped("%s", t.c_str());
            }
            ImGui::EndTable();
        }
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
    c[ImGuiCol_Tab] = v4(IM_COL32(19, 20, 23, 255));
    c[ImGuiCol_TabHovered] = v4(IM_COL32(38, 40, 45, 255));
    c[ImGuiCol_TabSelected] = v4(cBg);
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
    io.FontDefault = f.body;
    io.FontGlobalScale = 1.0f / s;
    return f;
}

void uiDraw(const UiData& d, const UiFonts& f, float width, float height, bool poppedOut, UiActions& act) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;   // the window itself never scrolls (only lists inside it do)
    ImGui::Begin("WXPlane", nullptr, wf);
    if (d.forceLayers >= 0) { showMap() = (d.forceLayers & 1) != 0; showTurb() = (d.forceLayers & 2) != 0; showPireps() = (d.forceLayers & 4) != 0; }
    if (d.forceDial >= 0) dialHeadingUp() = d.forceDial == 1;
    if (d.forceAdv >= 0) showAirmets() = showSigmets() = d.forceAdv == 1;
    updateViewRot(d);
    updateShownRange();
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
            ImGui::Image((ImTextureID)(intptr_t)f.logo, ImVec2(h * f.logoAspect, h));
        } else {
            ImGui::PushFont(f.title);
            ImGui::TextUnformatted("WXPlane");
            ImGui::PopFont();
        }
        (void)st;
        const float bw = 84 + 86 + 8;
        std::string live = d.busy ? std::string("Downloading…") : (d.nextUpdateS >= 0 ? fmt("Next update %d:%02d", (int)d.nextUpdateS / 60, (int)d.nextUpdateS % 60) : std::string(""));
        if (d.haveData && d.res) live += (live.empty() ? "" : " ") + std::string("(updated ") + hhmm(d.res->fetchedAt) + ")";
        const float lw = ImGui::CalcTextSize(live.c_str()).x + 20;
        ImGui::SameLine(right - bw - lw - 12);
        if (!live.empty()) { statusDot(d.busy ? SvcState::Degraded : SvcState::Ok, 4); ImGui::SameLine(0, 2); muted(live.c_str()); }
        ImGui::SameLine(right - bw);
        if (ImGui::Button("Refresh", ImVec2(84, 0))) act.refresh = true;
        ImGui::SameLine(0, 8);
        if (ImGui::Button(poppedOut ? "Pop in" : "Pop out", ImVec2(86, 0))) act.popOut = true;
    }

    if (!d.haveData || !d.res) {
        ImGui::Spacing();
        mutedWrapped(d.status1.empty() ? std::string("No weather downloaded yet.") : d.status1 + (d.status2.empty() ? "" : "  ·  " + d.status2));
        ImGui::Spacing();
        tabSettings(d, f, act);
        finish();
        return;
    }
    int bad = 0;
    for (const auto& s : allServices(d)) if (s.state == SvcState::Down || s.state == SvcState::Degraded) ++bad;
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Overview", nullptr, d.forceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabOverview(d, f, act); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(fmt("Airports (%zu)###air", d.res->stations.size()).c_str(), nullptr, d.forceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabAirports(d, f, act); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Map", nullptr, d.forceTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabRadar(d, f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Winds & Temps", nullptr, d.forceTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabAloft(d, f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Turbulence", nullptr, d.forceTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabTurbulence(d, f, act); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(bad ? fmt("Services (%d)###svc", bad).c_str() : "Services###svc", nullptr, d.forceTab == 5 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabServices(d, f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings", nullptr, d.forceTab == 6 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabSettings(d, f, act); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    finish();
}

} // namespace wx
