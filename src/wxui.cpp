#include "wxui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "geo.h"
#include "imgui.h"
#include "opera.h"
#include "radar.h"
#include "viewer.h"
#include "wxmodel.h"

// fonts embedded at build time (cmake/embed_fonts.cmake)
extern "C" {
extern const unsigned char wx_font_plex_regular[]; extern const unsigned int wx_font_plex_regular_size;
extern const unsigned char wx_font_plex_semibold[]; extern const unsigned int wx_font_plex_semibold_size;
extern const unsigned char wx_font_plex_bold[]; extern const unsigned int wx_font_plex_bold_size;
extern const unsigned char wx_font_mono[]; extern const unsigned int wx_font_mono_size;
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

void windDial(const UiData& d, const UiFonts& f, const Report& r, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // the whole dial is a button: north up <-> aircraft heading up
    ImGui::InvisibleButton("winddial", ImVec2(size, size));
    if (ImGui::IsItemClicked()) dialHeadingUp() = !dialHeadingUp();
    if (d.forceDial >= 0) dialHeadingUp() = d.forceDial == 1;
    const bool hovered = ImGui::IsItemHovered();
    const bool up = dialHeadingUp() && d.havePos;
    const double rot = up ? d.headingDeg : 0.0;   // screen-up direction, degrees true
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
    if (up) {   // the aircraft's nose at the top of the dial
        dl->AddTriangleFilled(ImVec2(c.x, p.y + 3), ImVec2(c.x + 5, p.y + 11), ImVec2(c.x - 5, p.y + 11), IM_COL32(255, 255, 255, 230));
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
    if (hovered) ImGui::SetTooltip(dialHeadingUp() ? "Aircraft heading up (headwind/tailwind and crosswind in the middle).\nClick for north up."
                                                   : "North up.\nClick to turn the dial with the aircraft's heading.");
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
void radarCanvas(const UiData& d, const UiFonts& f, float size, double rangeNm) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float R = size * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(15, 17, 20, 255), 6.0f);
    dl->PushClipRect(p, ImVec2(p.x + size, p.y + size), true);
    const double cLat = d.havePos ? d.lat : (d.res ? d.res->centerLat : 0), cLon = d.havePos ? d.lon : (d.res ? d.res->centerLon : 0);
    const double kx = std::cos(cLat * kPi / 180.0);
    const float px = (float)(R / rangeNm);   // pixels per nm
    auto at = [&](double lat, double lon) { return ImVec2(c.x + (float)((lon - cLon) * 60.0 * kx) * px, c.y - (float)((lat - cLat) * 60.0) * px); };
    // echoes
    if (d.res && d.res->radar.ok) {
        const double cell = d.cfg ? d.cfg->radarCellNm : 6.0;
        std::vector<const RadarCell*> cs;
        for (const auto& k : d.res->radar.cells) cs.push_back(&k);
        std::sort(cs.begin(), cs.end(), [](const RadarCell* a, const RadarCell* b) { return a->dbz < b->dbz; });   // strongest on top
        for (const RadarCell* k : cs) {
            if (k->dbz < 10) continue;
            const double sz = k->natural && k->sizeNm > 0 ? k->sizeNm : cell;
            const float rr = std::max(2.0f, (float)(sz * 0.62 * px));
            const ImVec2 q = at(k->lat, k->lon);
            if (q.x < p.x - rr || q.y < p.y - rr || q.x > p.x + size + rr || q.y > p.y + size + rr) continue;
            dl->AddCircleFilled(q, rr, dbzColor(k->dbz, k->synthetic ? 120 : 200), 20);
        }
    }
    // rings
    for (int k = 1; k <= 3; ++k) dl->AddCircle(c, R * k / 3.0f, cRing, 64, 1.0f);
    dl->AddLine(ImVec2(c.x, p.y), ImVec2(c.x, p.y + size), IM_COL32(31, 36, 42, 255));
    dl->AddLine(ImVec2(p.x, c.y), ImVec2(p.x + size, c.y), IM_COL32(31, 36, 42, 255));
    for (int k = 1; k <= 3; ++k) {
        const std::string t = fmt("%.0f", rangeNm * k / 3.0);
        dl->AddText(f.body, ImGui::GetFontSize() * 0.8f, ImVec2(c.x + 4, c.y - R * k / 3.0f + 2), IM_COL32(107, 114, 124, 255), t.c_str());
    }
    // airports
    if (d.res)
        for (const auto& s : d.res->stations) {
            if (s.icao.compare(0, 3, "GFS") == 0) continue;
            const ImVec2 q = at(s.lat, s.lon);
            if (q.x < p.x || q.y < p.y || q.x > p.x + size || q.y > p.y + size) continue;
            dl->AddCircleFilled(q, 3.0f, catColor(flightCategory(s.report)), 12);
            if (rangeNm <= 60) dl->AddText(f.body, ImGui::GetFontSize() * 0.75f, ImVec2(q.x + 5, q.y - 6), cMuted, s.icao.c_str());
        }
    // aircraft
    const double a = d.trackDeg * kPi / 180.0;
    const ImVec2 dir((float)std::sin(a), -(float)std::cos(a)), n(-dir.y, dir.x);
    dl->AddTriangleFilled(ImVec2(c.x + dir.x * 10, c.y + dir.y * 10), ImVec2(c.x - dir.x * 7 + n.x * 6, c.y - dir.y * 7 + n.y * 6),
                          ImVec2(c.x - dir.x * 7 - n.x * 6, c.y - dir.y * 7 - n.y * 6), IM_COL32(255, 255, 255, 255));
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(size, size));
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
    return reg.empty() ? std::string("Radar") : "Radar · " + reg;
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

void statBox(const UiFonts& f, const char* label, const std::string& value, const std::string& sub, float w) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cInset));
    ImGui::BeginChild(label, ImVec2(w, sub.empty() ? 62 : 80), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    muted(label);
    ImGui::PushFont(f.title);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopFont();
    if (!sub.empty()) muted(sub.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

int& radarRange() { static int r = 2; return r; }   // 0: 50 nm, 1: 100 nm, 2: 150 nm
double rangeNmOf(int k) { return k == 0 ? 50.0 : (k == 1 ? 100.0 : 150.0); }

void rangeButtons() {
    const char* lab[] = {"50", "100", "150 nm"};
    for (int k = 0; k < 3; ++k) {
        if (k) ImGui::SameLine(0, 0);
        const bool on = radarRange() == k;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, v4(IM_COL32(43, 69, 102, 255)));
        if (ImGui::SmallButton(lab[k])) radarRange() = k;
        if (on) ImGui::PopStyleColor();
    }
}

void radarPanel(const UiData& d, const UiFonts& f, bool big) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    sectionTitle(f, radarHeader(d).c_str());
    ImGui::SameLine(right - 118);
    rangeButtons();
    mutedWrapped(radarSubtitle(d));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float hLeft = ImGui::GetContentRegionAvail().y - (big ? 40 : 110);
    const float size = std::max(120.0f, std::min(avail, hLeft));
    if (size < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size) * 0.5f);
    radarCanvas(d, f, size, rangeNmOf(radarRange()));
    radarLegend();
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
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18);
    ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Temp", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("ISA", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    ImGui::TableHeadersRow();
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

void turbulenceCard(const UiData& d, const UiFonts& f, UiActions& act) {
    sectionTitle(f, "Turbulence & windshear");
    if (!d.turbData.empty()) { ImGui::SameLine(); muted(d.turbData.c_str()); }
    const float bw = (ImGui::GetContentRegionAvail().x - 16) / 3.0f;
    int withTurb = 0, total = 0;
    if (d.reports) for (const auto& p : d.reports->pireps) { ++total; if (hz::edrForIntensity(p.intensity, p.acType) > 0) ++withTurb; }
    statBox(f, "At the aircraft", d.turbOn ? turbWord(d.turbEdr) : std::string("Off"), d.turbOn && d.turbEdr >= 0 ? fmt("EDR %.2f · %s", d.turbEdr, d.turbSource.c_str()) : std::string(" "), bw);
    ImGui::SameLine(0, 8);
    statBox(f, "Pilot reports", d.reports ? fmt("%d", total) : std::string("—"), d.reports ? fmt("%d with turbulence", withTurb) : std::string(" "), bw);
    ImGui::SameLine(0, 8);
    const char* risk = !d.wsOn ? "Off" : (d.wsHigh > 0 ? "High" : (d.wsMed > 0 ? "Medium" : (d.wsSources > 0 ? "Low" : "None")));
    statBox(f, "Windshear risk", risk, d.wsOn ? fmt("%d source(s)", d.wsSources) : std::string(" "), bw);
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

void nearbyTable(const UiData& d, const Station* skip, int maxRows, bool scroll) {
    if (!d.res) return;
    std::vector<const Station*> v;
    for (const auto& s : d.res->stations) if (&s != skip && s.icao.compare(0, 3, "GFS") != 0) v.push_back(&s);
    std::sort(v.begin(), v.end(), [&](const Station* a, const Station* b) { return distTo(d, *a) < distTo(d, *b); });
    if (maxRows > 0 && (int)v.size() > maxRows) v.resize((size_t)maxRows);
    const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | (scroll ? ImGuiTableFlags_ScrollY : 0);
    if (!ImGui::BeginTable("near", 6, fl)) return;
    ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, 30);
    ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed, 42);
    ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("Ceiling", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
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

bool panelBegin(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(cPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    const bool r = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    return r;
}
void panelEnd() { ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); }

// ---- tabs ---------------------------------------------------------------------------------------------------------------------

void tabOverview(const UiData& d, const UiFonts& f, UiActions& act) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float stripH = 70, gap = 12;
    const float colW = std::floor((W - 2 * gap) * 0.31f), col3 = W - 2 * colW - 2 * gap, bodyH = H - stripH - gap;
    const Station* st = nearestStation(d);
    // column 1
    ImGui::BeginGroup();
    const float cardH = std::min(bodyH * 0.58f, 330.0f);
    if (panelBegin("card", ImVec2(colW, cardH))) { if (st) stationCard(d, f, *st, false); else mutedWrapped("No airport reports near the aircraft."); }
    panelEnd();
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    if (panelBegin("near", ImVec2(colW, bodyH - cardH - gap))) { sectionTitle(f, "Nearby airports"); nearbyTable(d, st, 0, true); }
    panelEnd();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    if (panelBegin("radar", ImVec2(colW, bodyH))) radarPanel(d, f, false);
    panelEnd();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    const float aloftH = std::min(bodyH * 0.6f, 330.0f);
    if (panelBegin("aloft", ImVec2(col3, aloftH))) {
        sectionTitle(f, "Winds & temps aloft");
        ImGui::SameLine(); muted("GFS");
        aloftTable(d, f, {5000, 10000, 18000, 24000, 30000, 34000, 39000, 45000}, false);
    }
    panelEnd();
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    if (panelBegin("turb", ImVec2(col3, bodyH - aloftH - gap))) turbulenceCard(d, f, act);
    panelEnd();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(IM_COL32(19, 20, 23, 255)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    if (ImGui::BeginChild("svc", ImVec2(W, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding)) servicesStrip(d, f);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void tabAirports(const UiData& d, const UiFonts& f) {
    static std::string selected;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float listW = std::floor(W * 0.55f);
    if (panelBegin("alist", ImVec2(listW, H)) && d.res) {
        sectionTitle(f, fmt("Airports (%zu)", d.res->stations.size()).c_str());
        const ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("all", 8, fl)) {
            ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthFixed, 52);
            ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, 32);
            ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed, 42);
            ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthFixed, 92);
            ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_WidthFixed, 58);
            ImGui::TableSetupColumn("Weather", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Clouds", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("T/Td", ImGuiTableColumnFlags_WidthFixed, 52);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            std::vector<const Station*> v;
            for (const auto& s : d.res->stations) v.push_back(&s);
            std::sort(v.begin(), v.end(), [&](const Station* a, const Station* b) { return distTo(d, *a) < distTo(d, *b); });
            if (selected.empty() && !v.empty()) selected = v[0]->icao;
            for (const Station* s : v) {
                const Report& r = s->report;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(s->icao.c_str(), selected == s->icao, ImGuiSelectableFlags_SpanAllColumns)) selected = s->icao;
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.0f", distTo(d, *s));
                ImGui::TableSetColumnIndex(2); { const std::string c = flightCategory(r); ImGui::PushStyleColor(ImGuiCol_Text, v4(catColor(c))); ImGui::TextUnformatted(c.empty() ? "—" : c.c_str()); ImGui::PopStyleColor(); }
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(windShort(r).c_str());
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(visText(r, usUnits(s->icao)).c_str());
                ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(r.wxText.empty() ? "—" : r.wxText.c_str());
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(cloudText(r).c_str());
                ImGui::TableSetColumnIndex(7); if (r.hasTemp) ImGui::Text("%.0f/%.0f", r.tempC, r.dewpC); else muted("—");
            }
            ImGui::EndTable();
        }
    }
    panelEnd();
    ImGui::SameLine(0, 12);
    if (panelBegin("adetail", ImVec2(0, H)) && d.res) {
        const Station* sel = nullptr;
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
    if (panelBegin("rbig", ImVec2(mapW, H))) radarPanel(d, f, true);
    panelEnd();
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
                ImGui::TableSetupColumn("dBZ", ImGuiTableColumnFlags_WidthFixed, 48);
                ImGui::TableSetupColumn("Rain", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Top", ImGuiTableColumnFlags_WidthFixed, 86);
                ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
                ImGui::TableHeadersRow();
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
        std::vector<float> lv;
        for (float ft = 2000; ft <= 9000; ft += 1000) lv.push_back(ft);
        for (float ft = 10000; ft <= 60000; ft += 2000) lv.push_back(ft);
        aloftTable(d, f, lv, true);
    }
    panelEnd();
}

void tabTurbulence(const UiData& d, const UiFonts& f, UiActions& act) {
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float leftW = std::floor(W * 0.45f);
    ImGui::BeginGroup();
    if (panelBegin("tcard", ImVec2(leftW, 190))) turbulenceCard(d, f, act);
    panelEnd();
    ImGui::Dummy(ImVec2(0, 4));
    if (panelBegin("tprof", ImVec2(leftW, H - 190 - 12))) {
        sectionTitle(f, "Turbulence by altitude here (EDR)");
        if (d.turbProfile.empty()) mutedWrapped(d.turbOn ? "No turbulence data yet." : "Realistic turbulence is off.");
        for (int i = (int)d.turbProfile.size() - 1; i >= 0; --i) {
            const UiTurbLayer& L = d.turbProfile[(size_t)i];
            ImGui::Text(L.altFt >= 18000 ? "FL%03.0f" : "%5.0f ft", L.altFt >= 18000 ? L.altFt / 100.0 : L.altFt);
            ImGui::SameLine(80);
            if (!L.has) { muted("X-Plane's own"); continue; }
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x - 120, h = ImGui::GetTextLineHeight();
            const float fr = (float)std::min(1.0, L.edr / 0.5);
            const ImU32 col = L.edr < 0.08 ? cAccentDim : (L.edr < 0.15 ? IM_COL32(80, 160, 110, 255) : (L.edr < 0.35 ? cWarn : cDown));
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + w, p.y + h * 0.75f), cInset, 3);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + std::max(2.0f, w * fr), p.y + h * 0.75f), col, 3);
            ImGui::Dummy(ImVec2(w, h));
            ImGui::SameLine(0, 8);
            ImGui::Text("%.2f %s", L.edr, turbWord(L.edr).c_str());
        }
    }
    panelEnd();
    ImGui::EndGroup();
    ImGui::SameLine(0, 12);
    if (panelBegin("treports", ImVec2(0, H))) {
        sectionTitle(f, "Pilot reports nearby");
        if (!d.reports || d.reports->pireps.empty()) mutedWrapped("No pilot reports in the last 90 minutes.");
        else if (ImGui::BeginTable("pireps", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("nm", ImGuiTableColumnFlags_WidthFixed, 34);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 56);
            ImGui::TableSetupColumn("Turbulence", ImGuiTableColumnFlags_WidthFixed, 96);
            ImGui::TableSetupColumn("Aircraft", ImGuiTableColumnFlags_WidthFixed, 56);
            ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            std::vector<const hz::Pirep*> v;
            for (const auto& p : d.reports->pireps) v.push_back(&p);
            auto dd = [&](const hz::Pirep* p) { return d.havePos ? distNm(d.lat, d.lon, p->lat, p->lon) : 0.0; };
            std::sort(v.begin(), v.end(), [&](const hz::Pirep* a, const hz::Pirep* b) { return dd(a) < dd(b); });
            for (size_t i = 0; i < v.size() && i < 80; ++i) {
                const hz::Pirep& p = *v[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%.0f", dd(&p));
                ImGui::TableSetColumnIndex(1); if (p.altFt > 0) ImGui::Text("FL%03.0f", p.altFt / 100.0); else muted("—");
                ImGui::TableSetColumnIndex(2);
                const double e = hz::edrForIntensity(p.intensity, p.acType);
                if (p.intensity.empty()) muted(p.llws ? "LLWS" : "—");
                else { ImGui::PushStyleColor(ImGuiCol_Text, v4(e >= 0.22 ? cWarn : (e > 0 ? cSoft : cMuted))); ImGui::TextUnformatted(p.intensity.c_str()); ImGui::PopStyleColor(); }
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(p.acType.c_str());
                ImGui::TableSetColumnIndex(4); if (p.obsTime > 0) ImGui::Text("%ld min", (long)((d.now - p.obsTime) / 60)); else muted("—");
                if (ImGui::IsItemHovered() && !p.raw.empty()) ImGui::SetTooltip("%s", p.raw.c_str());
            }
            ImGui::EndTable();
        }
        if (d.wsOn && !d.wsStrongest.empty()) { ImGui::Spacing(); mutedWrapped("Windshear: strongest source " + d.wsStrongest); }
    }
    panelEnd();
}

void tabServices(const UiData& d, const UiFonts& f) {
    const float H = ImGui::GetContentRegionAvail().y;
    if (panelBegin("svcfull", ImVec2(0, H))) {
        sectionTitle(f, "Data services");
        if (ImGui::BeginTable("svct", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 16);
            ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, 210);
            ImGui::TableSetupColumn("Gives", ImGuiTableColumnFlags_WidthFixed, 230);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(cMuted));
            ImGui::TableHeadersRow();
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
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
    ImGui::Begin("WXPlane", nullptr, wf);
    // header: where, when, refresh, pop out
    {
        const Station* st = nearestStation(d);
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(f.title);
        ImGui::TextUnformatted("WXPlane");
        ImGui::PopFont();
        ImGui::SameLine(0, 12);
        ImGui::AlignTextToFramePadding();
        std::string where = st ? st->icao : std::string("—");
        if (d.havePos) where += fmt(" · %.2f%c %.2f%c", std::fabs(d.lat), d.lat >= 0 ? 'N' : 'S', std::fabs(d.lon), d.lon >= 0 ? 'E' : 'W');
        if (d.haveData && d.res) where += " · updated " + hhmm(d.res->fetchedAt);
        muted(where.c_str());
        const float bw = 84 + 86 + 8;
        std::string live = d.busy ? std::string("Downloading…") : (d.nextUpdateS >= 0 ? fmt("Next update %d:%02d", (int)d.nextUpdateS / 60, (int)d.nextUpdateS % 60) : std::string(""));
        const float lw = ImGui::CalcTextSize(live.c_str()).x + 20;
        ImGui::SameLine(right - bw - lw - 12);
        if (!live.empty()) { statusDot(d.busy ? SvcState::Degraded : SvcState::Ok, 4); ImGui::SameLine(0, 2); muted(live.c_str()); }
        ImGui::SameLine(right - bw);
        if (ImGui::Button("Refresh", ImVec2(84, 0))) act.refresh = true;
        ImGui::SameLine(0, 8);
        if (ImGui::Button(poppedOut ? "Pop in" : "Pop out", ImVec2(86, 0))) act.popOut = true;
    }
    if (d.xpRealWeather) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(IM_COL32(59, 42, 18, 255)));
        ImGui::BeginChild("rw", ImVec2(0, 30), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(cWarn));
        ImGui::TextUnformatted("X-Plane's real-world weather is ON - turn it off in X-Plane's weather settings, or it fights WXPlane.");
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!d.haveData || !d.res) {
        ImGui::Spacing();
        mutedWrapped(d.status1.empty() ? std::string("No weather downloaded yet.") : d.status1 + (d.status2.empty() ? "" : "  ·  " + d.status2));
        ImGui::End();
        return;
    }
    int bad = 0;
    for (const auto& s : allServices(d)) if (s.state == SvcState::Down || s.state == SvcState::Degraded) ++bad;
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Overview", nullptr, d.forceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabOverview(d, f, act); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(fmt("Airports (%zu)###air", d.res->stations.size()).c_str(), nullptr, d.forceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabAirports(d, f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Radar", nullptr, d.forceTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabRadar(d, f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Winds & Temps", nullptr, d.forceTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabAloft(d, f); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Turbulence", nullptr, d.forceTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabTurbulence(d, f, act); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(bad ? fmt("Services (%d)###svc", bad).c_str() : "Services###svc", nullptr, d.forceTab == 5 ? ImGuiTabItemFlags_SetSelected : 0)) { ImGui::Spacing(); tabServices(d, f); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace wx
