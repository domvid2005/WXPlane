#include "wxmodel.h"

#include "geo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wx {
namespace {

float coverageFor(int cover) {
    switch (cover) {
        case CloudGroup::FEW: return 0.20f;
        case CloudGroup::SCT: return 0.40f;
        case CloudGroup::BKN: return 0.70f;
        default: return 0.94f;   // a fully closed sheet renders as a dense, dark-topped slab; a little texture lets the sun light the top
    }
}

} // namespace

void mergeAtis(Station& st, const Report& a, const std::string& text) {
    Report& r = st.report;
    if (a.hasWind) { r.hasWind = true; r.windDir = a.windDir; r.windKt = a.windKt; r.gustKt = a.gustKt; r.windVariable = a.windVariable; }
    if (a.hasVis) { r.hasVis = true; r.visM = a.visM; r.cavok = a.cavok; }
    if (a.hasTemp) { r.hasTemp = true; r.tempC = a.tempC; r.hasDewp = a.hasDewp; r.dewpC = a.dewpC; }
    if (a.hasAltim) { r.hasAltim = true; r.altimHpa = a.altimHpa; }
    if (!a.clouds.empty() || a.clearSky) { r.clouds = a.clouds; r.clearSky = a.clearSky; }
    if (!a.wxText.empty() || a.hasVis) {
        // ATIS carries its own present-weather picture; replace the METAR's.
        r.fog = a.fog; r.shallowFog = a.shallowFog; r.patchyFog = a.patchyFog; r.mist = a.mist; r.haze = a.haze;
        r.thunder = a.thunder; r.freezing = a.freezing; r.snow = a.snow; r.precip = a.precip; r.wxText = a.wxText;
    }
    r.hour = a.hour >= 0 ? a.hour : r.hour;
    r.minute = a.hour >= 0 ? a.minute : r.minute;
    r.day = a.hour >= 0 ? a.day : r.day;
    st.hasAtis = true;
    st.atisLetter = a.infoLetter;
    st.atisText = text;
}

float profileTemp(const std::vector<std::pair<float, float>>& p, float altM) {
    if (p.empty()) return 0;
    if (altM <= p.front().first) return p.front().second;
    if (altM >= p.back().first) return p.back().second;
    for (size_t i = 1; i < p.size(); ++i) {
        if (altM <= p[i].first) {
            float span = p[i].first - p[i - 1].first;
            float f = span > 0 ? (altM - p[i - 1].first) / span : 0;
            return p[i - 1].second + f * (p[i].second - p[i - 1].second);
        }
    }
    return p.back().second;
}

bool stationHasFog(const Station& st, const Config& cfg) {
    const Report& r = st.report;
    if (!cfg.fogLayer) return false;
    for (const auto& g : r.clouds)
        if (g.cover == CloudGroup::VV) return true;
    return r.fog || r.shallowFog || r.patchyFog || r.mist;
}

XpWx buildXp(const Station& st, const Config& cfg, std::time_t now, const std::vector<AloftStation>* aloft, bool fogSlot) {
    const Report& r = st.report;
    XpWx x;
    x.ageS = (float)reportAgeSeconds(r, now);

    x.hasTemp = r.hasTemp;
    x.tempC = (float)r.tempC;
    x.hasDewp = r.hasTemp && r.hasDewp;
    x.dewpC = (float)r.dewpC;
    x.qnhPa = r.hasAltim ? (float)(r.altimHpa * 100.0) : 0.0f;

    // Wind: only the surface layer is defined, so winds aloft keep coming from X-Plane's own data.
    if (r.hasWind) {
        x.hasWind = true;
        x.windAltMsl = (float)(st.elevM + 10.0);
        x.windDir = (float)r.windDir;
        x.windMs = (float)(r.windKt * kKtToMs);
        x.gustMs = r.gustKt > r.windKt ? (float)(r.gustKt * kKtToMs) : x.windMs;
        x.shearDeg = r.windVariable ? (r.windDir == 0 ? 180.0f : 60.0f) : 0.0f;
        x.turbulence = std::min(0.6f, (x.gustMs - x.windMs) / 10.0f);
        if (r.thunder) x.turbulence = std::max(x.turbulence, 0.5f);
        float hs = 0.0246f * x.windMs * x.windMs; // fully-developed sea, Pierson-Moskowitz
        x.waveHeightM = std::min(hs, 8.0f);
        x.waveDir = r.windDir;
    }

    if (x.hasWind) {
        XpWind w;
        w.altMsl = x.windAltMsl; w.dir = x.windDir; w.ms = x.windMs; w.gustMs = x.gustMs;
        w.shearDeg = x.shearDeg; w.turbulence = x.turbulence;
        x.winds.push_back(w);
    }

    // NOAA winds/temps aloft, blended from the nearest FD stations to this report's location.
    if (cfg.useAloft && aloft && !aloft->empty()) {
        std::vector<AloftSample> smp = sampleAloft(*aloft, st.lat, st.lon, cfg.aloftRangeNm, cfg.aloftStations);
        if (x.hasTemp) x.tempProfile.emplace_back((float)st.elevM, (float)r.tempC);
        // Temperatures: every NOAA/model level above the terrain.
        const double floorM = st.elevM + 300.0;
        for (const auto& a : smp) {
            const float altM = (float)(a.altFt * kFtToM);
            if (altM > floorM && a.hasTemp) x.tempProfile.emplace_back(altM, (float)a.tempC);
        }
        // Winds: the SAME 12 altitudes in every record, whatever the station's terrain or which sources reached it. X-Plane blends
        // overlapping records layer by layer, and a record with fewer layers (a high-terrain station that skips 3000 and 6000 ft, or one
        // without the model's top levels) left slots undefined, which fall back to the sim's own winds - calm after our reset. A level under
        // the terrain gets the wind of the lowest level above it, at an altitude just over the previous layer.
        std::vector<AloftSample> ws;
        for (const auto& a : smp) if (a.hasWind) ws.push_back(a);
        if (!ws.empty()) {
            static const double kGridFt[] = {3000, 6000, 9000, 12000, 18000, 24000, 30000, 34000, 39000, 45000, 53000, 60000};
            double prevM = x.winds.empty() ? st.elevM : x.winds.back().altMsl;
            for (double gft : kGridFt) {
                auto uv = [](const AloftSample& q, double& u, double& v) { const double rad = q.dirDeg * 3.14159265358979 / 180.0; u = -q.speedKt * std::sin(rad); v = -q.speedKt * std::cos(rad); };
                double u = 0, v = 0;
                if (gft <= ws.front().altFt) uv(ws.front(), u, v);
                else if (gft >= ws.back().altFt) uv(ws.back(), u, v);
                else {
                    for (size_t i = 1; i < ws.size(); ++i) {
                        if (gft <= ws[i].altFt) {
                            double u0, v0, u1, v1;
                            uv(ws[i - 1], u0, v0); uv(ws[i], u1, v1);
                            const double f = (gft - ws[i - 1].altFt) / std::max(1.0, ws[i].altFt - ws[i - 1].altFt);
                            u = u0 + (u1 - u0) * f; v = v0 + (v1 - v0) * f;
                            break;
                        }
                    }
                }
                XpWind w;
                const double kt = std::sqrt(u * u + v * v);
                double dir = std::atan2(-u, -v) * 180.0 / 3.14159265358979;
                if (dir < 0) dir += 360.0;
                w.altMsl = (float)std::max(gft * kFtToM, prevM + 1.0);
                w.dir = (float)(kt < 0.5 ? 0 : dir);
                w.ms = (float)(kt * kKtToMs); w.gustMs = w.ms;
                prevM = w.altMsl;
                if (x.winds.size() < 13) x.winds.push_back(w);
            }
        }
        x.aloft = x.winds.size() > (x.hasWind ? 1u : 0u) || x.tempProfile.size() > 1;
        if (x.tempProfile.size() < 2) x.tempProfile.clear();
    }
    x.maxAltFt = (float)(x.aloft ? cfg.cruiseCeilingFt : cfg.normalCeilingFt);

    // Visibility
    // Below 10 SM / 9999 the reported value is used exactly (e.g. 4SM = 6437 m). "10 SM or more" (10SM, 9999, CAVOK)
    // maps to clear_vis_m because those reports are a floor, not a measurement.
    double vis = r.hasVis ? r.visM : cfg.clearVisM;
    // 10 SM or more (16093 m) or the metric "9999" (10 km or more) means unlimited; 7, 8 and 9 SM are real values and stay exact.
    if (r.cavok || (r.hasVis && (vis >= 16000.0 || (vis >= 9999.0 && vis <= 10000.5)))) vis = cfg.clearVisM;
    if (cfg.fogHaze) {
        // Fog must reduce visibility too, not only add a cloud layer.
        if (r.fog) vis = std::min(vis, 1000.0);
        if (r.clouds.end() != std::find_if(r.clouds.begin(), r.clouds.end(), [](const CloudGroup& g) { return g.cover == CloudGroup::VV; })) vis = std::min(vis, 800.0);
        if (r.shallowFog || r.patchyFog) vis = std::min(vis, 3000.0);
        // Mist (BR) and haze (HZ) are left at the reported value: US reports use them up to ~6 SM.
        // Saturated air: the sim derives haze from humidity as well as visibility.
        if ((r.fog || r.shallowFog || r.patchyFog) && x.hasTemp) { x.hasDewp = true; x.dewpC = x.tempC; }
    }
    if (vis < 50) vis = 50;
    x.visM = (float)vis;
    x.precip = (float)r.precip;

    // Cloud layers. Fog first (it matters most for what the pilot sees below), then by base height.
    std::vector<XpCloud> layers;
    const CloudGroup* vv = nullptr;
    for (const auto& g : r.clouds)
        if (g.cover == CloudGroup::VV) vv = &g;

    bool anyFog = r.fog || r.shallowFog || r.patchyFog || r.mist || vv != nullptr;
    // Mist (BR) together with a low broken/overcast ceiling: the ceiling layer is the low cloud. A separate mist layer would take
    // the lowest slot and push the real ceiling up by the layer gap (100 ft showing as 800 ft), and cost a slot for the third deck.
    bool ceilingInsteadOfMist = false;
    if (r.mist && !r.fog && !r.shallowFog && !r.patchyFog && !vv)
        for (const auto& g : r.clouds)
            if (g.cover != CloudGroup::VV && g.cover >= CloudGroup::BKN && g.baseFtAgl < 600) ceilingInsteadOfMist = true;
    if (cfg.fogLayer && anyFog && !ceilingInsteadOfMist) {
        XpCloud f;
        f.type = 1.0f;
        f.baseM = (float)st.elevM; // starts at the ground, so it is part of the surface weather
        double depth = cfg.fogTopAglM;
        f.coverage = 1.0f;
        if (vv) depth = std::min(400.0, std::max(120.0, vv->baseFtAgl * kFtToM));
        else if (r.shallowFog) { depth = 100.0; f.coverage = 0.9f; }
        else if (r.patchyFog) { depth = std::max(100.0, depth * 0.6); f.coverage = 0.5f; }
        else if (r.mist && !r.fog) { depth = 90.0; f.coverage = 0.45f; }   // BR: a thin veil on the ground, so mist is visible and not only a shorter view
        f.topM = (float)(st.elevM + depth);
        layers.push_back(f);
        x.fogLayerApplied = true;
    }

    std::vector<CloudGroup> cl;
    for (const auto& g : r.clouds)
        if (g.cover != CloudGroup::VV) cl.push_back(g);
    std::sort(cl.begin(), cl.end(), [](const CloudGroup& a, const CloudGroup& b) { return a.baseFtAgl < b.baseFtAgl; });
    for (const auto& g : cl) {
        XpCloud c;
        c.coverage = coverageFor(g.cover);
        c.baseM = (float)(st.elevM + g.baseFtAgl * kFtToM);
        double thick;
        if (g.kind == 1) { c.type = 3.0f; thick = 6000; }
        else if (g.kind == 2) { c.type = 3.0f; thick = 4000; }
        else if (g.baseFtAgl >= 20000) { c.type = 0.0f; thick = 1200; }
        else if ((g.baseFtAgl < 2000 && g.cover >= CloudGroup::BKN) || (g.cover == CloudGroup::OVC && g.baseFtAgl < 12000)) {
            // Very thin solid layers render as a black void from below, very thick ones as a dark slab from above: moderate depth.
            c.type = (float)cfg.lowCloudType;   // 1.0 renders as a smooth sheet with vertical curtain edges; 2.0 gives billowy cloud
            thick = g.cover == CloudGroup::OVC ? 1400 : (g.cover == CloudGroup::BKN ? 1100 : 800);
        }
        else { c.type = (float)cfg.lowCloudType; thick = g.cover == CloudGroup::OVC ? 1600 : (g.cover == CloudGroup::BKN ? 1300 : 900); }
        c.topM = (float)(c.baseM + thick);
        layers.push_back(c);
    }
    // Layers must never overlap vertically: a lower layer whose top reaches into the one above it is shaded by it and renders
    // dark. Keep a clear gap between every top and the next base. The lower layer is trimmed first (never thinner than
    // kMinThick); if the two bases are so close that even that is not enough, the upper layer is lifted just far enough.
    {
        const float kMinThick = 120.0f;
        for (size_t i = 0; i + 1 < layers.size(); ++i) {
            // right at the ground (fog, a 100 ft ceiling) a 150 m gap would lift the next layer far above where it is reported
            const float kGap = (layers[i].baseM - (float)st.elevM < 100.0f) ? 40.0f : 150.0f;
            float limit = layers[i + 1].baseM - kGap;
            if (layers[i].topM > limit) layers[i].topM = std::max(layers[i].baseM + kMinThick, limit);
            float need = layers[i].topM + kGap;
            if (layers[i + 1].baseM < need) {
                float thick = layers[i + 1].topM - layers[i + 1].baseM;
                layers[i + 1].baseM = need;
                layers[i + 1].topM = need + std::max(thick, kMinThick);
            }
        }
    }
    if (fogSlot) {
        // slot 0 = ground fog (or empty); the two lowest real cloud layers follow in slots 1 and 2
        size_t first = x.fogLayerApplied ? 1 : 0;
        if (x.fogLayerApplied) x.clouds[0] = layers[0];
        if (ceilingInsteadOfMist && !layers.empty()) {   // the low ceiling takes the ground slot; the next two decks follow
            x.clouds[0] = layers[0];
            first = 1;
        }
        for (size_t i = 0; i < 2 && first + i < layers.size(); ++i) x.clouds[1 + i] = layers[first + i];
    } else {
        for (size_t i = 0; i < 3 && i < layers.size(); ++i) x.clouds[i] = layers[i];
    }
    x.cloudCount = 0;
    for (const auto& c : x.clouds)
        if (c.coverage > 0) ++x.cloudCount;
    return x;
}

int validateXp(XpWx& x, bool allowOverlap, std::string* note) {
    int fixes = 0;
    auto say = [&](const char* what, int slot) {
        ++fixes;
        if (note && note->empty()) { char b[96]; std::snprintf(b, sizeof b, "%s (slot %d)", what, slot); *note = b; }
    };
    for (int i = 0; i < 3; ++i) {
        XpCloud& c = x.clouds[i];
        if (!(c.coverage > 0.0f)) { if (c.coverage != 0.0f) { c = XpCloud(); say("invalid coverage", i); } continue; }
        if (c.coverage > 1.0f) { c.coverage = 1.0f; say("coverage above 1", i); }
        if (!(c.topM > c.baseM)) { c.topM = c.baseM + 60.0f; say("top at or below base", i); }
        else if (c.topM - c.baseM < 30.0f) { c.topM = c.baseM + 30.0f; say("layer thinner than 30 m", i); }
        else if (c.topM - c.baseM > 16000.0f) { c.topM = c.baseM + 16000.0f; say("layer thicker than 16 km", i); }
    }
    for (int i = 0; i < 3; ++i) {   // the sim's own limits: the top (45,000 ft) and the maximum thickness of each cloud type
        XpCloud& c = x.clouds[i];
        if (!(c.coverage > 0)) continue;
        if (c.topM > kXpMaxTopM) { c.topM = std::max(kXpMaxTopM, c.baseM + 30.0f); say("cloud top above 45000 ft", i); }   // every type: the manual weather screen stops tops at 45,000 ft
        const float maxThick = xpMaxThicknessM(c.type);
        if (c.topM - c.baseM > maxThick) { c.topM = c.baseM + maxThick; say("layer thicker than X-Plane allows for its cloud type", i); }
    }
    if (!allowOverlap) {
        // vertical order of the non-empty layers (slot 0 may be an empty fog slot)
        int prev = -1;
        for (int i = 0; i < 3; ++i) {
            if (!(x.clouds[i].coverage > 0.0f)) continue;
            if (prev >= 0 && x.clouds[i].baseM < x.clouds[prev].topM + 40.0f) {
                const float thick = x.clouds[i].topM - x.clouds[i].baseM;
                x.clouds[i].baseM = x.clouds[prev].topM + 40.0f;
                x.clouds[i].topM = x.clouds[i].baseM + thick;
                say("layers overlapping or out of order", i);
            }
            prev = i;
        }
    }
    float highestFt = 0;
    for (const auto& c : x.clouds)
        if (c.coverage > 0 && c.topM / 0.3048f > highestFt) highestFt = c.topM / 0.3048f;
    if (x.maxAltFt < highestFt) { x.maxAltFt = highestFt; say("record ceiling below its highest cloud", 0); }
    x.cloudCount = 0;
    for (const auto& c : x.clouds) if (c.coverage > 0) ++x.cloudCount;
    return fixes;
}

std::string describeXp(const XpWx& x, double elevM) {
    char b[256];
    std::string out;
    std::snprintf(b, sizeof b, "vis %.0f m", x.visM);
    out += b;
    if (x.hasWind) {
        std::snprintf(b, sizeof b, " | wind %03.0f/%.0f kt", x.windDir, x.windMs / kKtToMs);
        out += b;
        if (x.gustMs > x.windMs + 0.5f) { std::snprintf(b, sizeof b, " G%.0f", x.gustMs / kKtToMs); out += b; }
    }
    if (x.qnhPa > 0) { std::snprintf(b, sizeof b, " | QNH %.1f hPa", x.qnhPa / 100.0f); out += b; }
    if (x.hasTemp) { std::snprintf(b, sizeof b, " | %.0f/%.0f C", x.tempC, x.hasDewp ? x.dewpC : x.tempC); out += b; }
    static const char* names[] = {"cirrus", "stratus", "cumulus", "CB"};
    for (int i = 0; i < 3; ++i) {
        const XpCloud& c = x.clouds[i];
        if (c.coverage <= 0) continue;
        int t = (int)(c.type + 0.5f);
        t = t < 0 ? 0 : (t > 3 ? 3 : t);
        std::snprintf(b, sizeof b, " | %s%s %.0f%% %.0f-%.0f ft AGL", (i == 0 && x.fogLayerApplied) ? "FOG " : "", names[t],
                      c.coverage * 100.0f, (c.baseM - elevM) / kFtToM, (c.topM - elevM) / kFtToM);
        out += b;
    }
    if (x.aloft) { std::snprintf(b, sizeof b, " | NOAA winds aloft to FL%.0f", x.winds.back().altMsl / kFtToM / 100.0f); out += b; }
    return out;
}


namespace {
constexpr double kPiW = 3.14159265358979323846;
double idwWeight(double dNm) { double d = dNm < 0.5 ? 0.5 : dNm; return 1.0 / (d * d * d); }
double effectiveVisM(const Report& r) { return r.cavok ? 40000.0 : (r.visM >= 16000.0 ? 40000.0 : r.visM); }
bool hasVisAny(const Report& r) { return r.hasVis || r.cavok; }
} // namespace

int fillMissing(std::vector<Station>& st, double rangeNm) {
    int filled = 0;
    for (size_t i = 0; i < st.size(); ++i) {
        Station& a = st[i];
        struct Near { const Station* s; double w; };
        std::vector<Near> order;
        for (size_t j = 0; j < st.size(); ++j) {
            if (j == i) continue;
            double d = distNm(a.lat, a.lon, st[j].lat, st[j].lon);
            if (d <= rangeNm) order.push_back({&st[j], d});
        }
        std::sort(order.begin(), order.end(), [](const Near& x, const Near& y) { return x.w < y.w; });   // w holds the distance for now
        auto pick = [&](auto has) {
            std::vector<Near> v;
            for (const auto& n : order) if (has(n.s->report)) { v.push_back({n.s, idwWeight(n.w)}); if (v.size() >= 4) break; }
            return v;
        };
        auto note = [&](const char* what, const std::vector<Near>& v) {
            if (!a.filled.empty()) a.filled += ", ";
            a.filled += std::string(what) + " from " + v[0].s->icao;
            ++filled;
        };
        Report& r = a.report;
        if (!r.hasWind) {
            auto v = pick([](const Report& q) { return q.hasWind; });
            if (!v.empty()) {
                double sw = 0, u = 0, w = 0, g = 0;
                for (const auto& n : v) {
                    const Report& q = n.s->report;
                    double rad = q.windDir * kPiW / 180.0;
                    u += n.w * q.windKt * std::sin(rad); w += n.w * q.windKt * std::cos(rad);
                    g += n.w * std::max(q.gustKt, q.windKt); sw += n.w;
                }
                u /= sw; w /= sw; g /= sw;
                double kt = std::sqrt(u * u + w * w);
                r.hasWind = true; r.windVariable = false;
                r.windKt = kt;
                r.windDir = kt < 0.5 ? 0 : (int)std::lround(std::fmod(std::atan2(u, w) * 180.0 / kPiW + 360.0, 360.0));
                r.gustKt = g > kt + 3.0 ? g : 0;
                note("wind", v);
            }
        }
        if (!hasVisAny(r)) {
            auto v = pick([](const Report& q) { return hasVisAny(q); });
            if (!v.empty()) {
                double sw = 0, m = 0;
                for (const auto& n : v) { m += n.w * effectiveVisM(n.s->report); sw += n.w; }
                r.hasVis = true; r.visM = m / sw;
                note("visibility", v);
            }
        }
        if (!r.hasTemp) {
            auto v = pick([](const Report& q) { return q.hasTemp; });
            if (!v.empty()) { double sw = 0, m = 0; for (const auto& n : v) { m += n.w * n.s->report.tempC; sw += n.w; } r.hasTemp = true; r.tempC = m / sw; note("temperature", v); }
        }
        if (!r.hasDewp && r.hasTemp) {
            auto v = pick([](const Report& q) { return q.hasDewp && q.hasTemp; });
            if (!v.empty()) {   // dew point depression of the neighbours, applied to this station's temperature
                double sw = 0, m = 0; for (const auto& n : v) { m += n.w * (n.s->report.tempC - n.s->report.dewpC); sw += n.w; }
                r.hasDewp = true; r.dewpC = r.tempC - m / sw; note("dew point", v);
            }
        }
        if (!r.hasAltim) {
            auto v = pick([](const Report& q) { return q.hasAltim; });
            if (!v.empty()) { double sw = 0, m = 0; for (const auto& n : v) { m += n.w * n.s->report.altimHpa; sw += n.w; } r.hasAltim = true; r.altimHpa = m / sw; note("pressure", v); }
        }
    }
    return filled;
}

Station blendAt(const std::vector<Station>& st, double lat, double lon) {
    struct Near { const Station* s; double d; };
    std::vector<Near> order;
    for (const auto& s : st) order.push_back({&s, distNm(lat, lon, s.lat, s.lon)});
    std::sort(order.begin(), order.end(), [](const Near& x, const Near& y) { return x.d < y.d; });
    Station out = *order[0].s;
    out.lat = lat; out.lon = lon;
    out.hasAtis = false;                         // the blend is not any one airport's ATIS
    out.report.fog = out.report.shallowFog = out.report.patchyFog = false;   // ground fog stays at its airport
    const double d0 = order[0].d;
    std::vector<Near> use;
    for (const auto& n : order) {
        if (use.size() >= 4) break;
        if (use.empty() || n.d <= d0 * 2.5 + 5.0) use.push_back(n);
    }
    if (use.size() < 2) return out;
    for (auto& n : use) n.d = idwWeight(n.d);    // d now holds the weight
    Report& r = out.report;
    auto sumW = [&](auto has) { double sw = 0; for (const auto& n : use) if (has(n.s->report)) sw += n.d; return sw; };
    if (r.hasWind && !r.windVariable) {
        double sw = 0, u = 0, w = 0, g = 0;
        for (const auto& n : use) {
            const Report& q = n.s->report;
            if (!q.hasWind) continue;
            double rad = q.windDir * kPiW / 180.0, kt = q.windKt;
            u += n.d * kt * std::sin(rad); w += n.d * kt * std::cos(rad); g += n.d * std::max(q.gustKt, q.windKt); sw += n.d;
        }
        if (sw > 0) {
            u /= sw; w /= sw; g /= sw;
            double kt = std::sqrt(u * u + w * w);
            r.windKt = kt;
            if (kt >= 0.5) r.windDir = (int)std::lround(std::fmod(std::atan2(u, w) * 180.0 / kPiW + 360.0, 360.0));
            r.gustKt = g > kt + 3.0 ? g : 0;
        }
    }
    if (hasVisAny(r)) {
        double sw = sumW([](const Report& q) { return hasVisAny(q); }), m = 0;
        if (sw > 0) {
            for (const auto& n : use) if (hasVisAny(n.s->report)) m += n.d * effectiveVisM(n.s->report);
            r.hasVis = true; r.cavok = false; r.visM = m / sw;
        }
    }
    if (r.hasTemp) { double sw = sumW([](const Report& q) { return q.hasTemp; }), m = 0; if (sw > 0) { for (const auto& n : use) if (n.s->report.hasTemp) m += n.d * n.s->report.tempC; r.tempC = m / sw; } }
    if (r.hasDewp && r.hasTemp) {
        double sw = 0, m = 0;
        for (const auto& n : use) if (n.s->report.hasDewp && n.s->report.hasTemp) { m += n.d * (n.s->report.tempC - n.s->report.dewpC); sw += n.d; }
        if (sw > 0) r.dewpC = r.tempC - m / sw;
    }
    if (r.hasAltim) { double sw = sumW([](const Report& q) { return q.hasAltim; }), m = 0; if (sw > 0) { for (const auto& n : use) if (n.s->report.hasAltim) m += n.d * n.s->report.altimHpa; r.altimHpa = m / sw; } }
    return out;
}

} // namespace wx
