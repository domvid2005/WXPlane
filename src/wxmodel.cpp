#include "wxmodel.h"

#include <algorithm>
#include <cmath>

namespace wx {
namespace {

float coverageFor(int cover) {
    switch (cover) {
        case CloudGroup::FEW: return 0.20f;
        case CloudGroup::SCT: return 0.40f;
        case CloudGroup::BKN: return 0.70f;
        default: return 1.0f;
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

XpWx buildXp(const Station& st, const Config& cfg, std::time_t now, const std::vector<AloftStation>* aloft) {
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
        const double floorM = st.elevM + 300.0; // skip levels at/under the terrain
        if (x.hasTemp) x.tempProfile.emplace_back((float)st.elevM, (float)r.tempC);
        for (const auto& a : smp) {
            float altM = (float)(a.altFt * kFtToM);
            if (altM <= floorM) continue;
            if (a.hasWind && x.winds.size() < 13) {
                XpWind w;
                w.altMsl = altM; w.dir = (float)a.dirDeg; w.ms = (float)(a.speedKt * kKtToMs); w.gustMs = w.ms;
                x.winds.push_back(w);
            }
            if (a.hasTemp) x.tempProfile.emplace_back(altM, (float)a.tempC);
        }
        // Hold the highest NOAA wind up to the ceiling so X-Plane's own winds never take over above the top FD level.
        if (!x.winds.empty() && x.winds.size() > (x.hasWind ? 1u : 0u) && x.winds.size() < 13) {
            float ceilM = (float)(cfg.cruiseCeilingFt * kFtToM);
            if (x.winds.back().altMsl < ceilM - 100.0f) {
                XpWind top = x.winds.back();
                top.altMsl = ceilM;
                x.winds.push_back(top);
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
    if (r.cavok || (r.hasVis && vis >= 9999.0)) vis = cfg.clearVisM;
    if (vis < 50) vis = 50;
    x.visM = (float)vis;
    x.precip = (float)r.precip;

    // Cloud layers. Fog first (it matters most for what the pilot sees below), then by base height.
    std::vector<XpCloud> layers;
    const CloudGroup* vv = nullptr;
    for (const auto& g : r.clouds)
        if (g.cover == CloudGroup::VV) vv = &g;

    bool anyFog = r.fog || r.shallowFog || r.patchyFog || vv != nullptr;
    if (cfg.fogLayer && anyFog) {
        XpCloud f;
        f.type = 1.0f;
        f.baseM = (float)(st.elevM + 2.0);
        double depth = cfg.fogTopAglM;
        f.coverage = 1.0f;
        if (vv) depth = std::min(400.0, std::max(30.0, vv->baseFtAgl * kFtToM));
        else if (r.shallowFog) { depth = std::min(depth, 25.0); f.coverage = 0.9f; }
        else if (r.patchyFog) { depth *= 0.6; f.coverage = 0.5f; }
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
        else if (g.kind == 2) { c.type = 2.6f; thick = 4000; }
        else if (g.baseFtAgl >= 20000) { c.type = 0.0f; thick = 1200; }
        else if ((g.baseFtAgl < 2000 && g.cover >= CloudGroup::BKN) || (g.cover == CloudGroup::OVC && g.baseFtAgl < 12000)) { c.type = 1.0f; thick = 600; }
        else { c.type = 2.0f; thick = g.cover >= CloudGroup::BKN ? 1500 : 1000; }
        c.topM = (float)(c.baseM + thick);
        layers.push_back(c);
    }
    x.cloudCount = (int)std::min<size_t>(3, layers.size());
    for (int i = 0; i < x.cloudCount; ++i) x.clouds[i] = layers[(size_t)i];
    return x;
}

} // namespace wx
