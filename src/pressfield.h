#pragma once
// The pressure field WXPlane sends to X-Plane (5.1.9). See smoothPressure in plugin.cpp for how it is used.
#include <algorithm>
#include <cmath>
#include <vector>

#include "geo.h"

namespace wx {

// A field over the stations: the smooth Gaussian-weighted mean (sigma 50 nm), plus each station's own difference from it, carried
// around the station and faded out with distance. At a station the field is (nearly) the station's own value; it goes back to the smooth mean
// over about anchorNm (half of the difference is left at anchorNm, nothing beyond 3 x anchorNm). anchorNm = 0 is the smooth mean alone.
// 2026-11-26, Yellowknife (CYZF, the other airports 74-110 nm away): the smooth mean alone gave the stations 100 nm off a large share of
// the weight, and the gap-fill tiles around CYZF carried that mean while CYZF's own record carried CYZF's pressure. On the approach
// X-Plane's altimeter setting read 30.14 inHg at 17 nm (where the tiles still reach in) and came down to the METAR's 29.92 inside
// 12 nm, with the mean moving between updates as stations came into and went out of the search radius.
struct PField {
    struct Q { double lat, lon, v, r; };
    std::vector<Q> q;
    double anchorNm = 0;
    bool smooth(double lat, double lon, double& out) const {
        double sw = 0, sv = 0;
        const double sig = 50.0;
        for (const auto& e : q) {
            const double d = distNm(lat, lon, e.lat, e.lon);
            const double w = std::exp(-d * d / (2.0 * sig * sig)) + 1e-9 / (1.0 + d);   // far from all stations: the nearest still counts a little
            sw += w; sv += w * e.v;
        }
        if (q.size() < 2 || sw <= 0) return false;
        out = sv / sw;
        return true;
    }
    // each station's difference from the smooth mean; capped at 1 hPa plus 0.06 hPa per nm to its nearest neighbour, so two airports
    // close together cannot make a steep slope between them (10 nm apart: 1.6 hPa; 30 nm: 2.8 hPa; 74 nm: 5.4 hPa). A station's own record
    // may still differ from the field by up to 2 hPa more (smoothPressure), as in 5.1.5-5.1.8.
    void finish() {
        for (auto& e : q) {
            double m;
            e.r = 0;
            if (anchorNm <= 0 || !smooth(e.lat, e.lon, m)) continue;
            double nn = 1e9;
            for (const auto& o : q) if (&o != &e) nn = std::min(nn, distNm(e.lat, e.lon, o.lat, o.lon));
            const double cap = 1.0 + 0.06 * std::min(nn, 200.0);
            e.r = std::max(-cap, std::min(cap, e.v - m));
        }
    }
    bool at(double lat, double lon, double& out) const {
        if (!smooth(lat, lon, out)) return false;
        if (anchorNm <= 0) return true;
        double sw = 1.0 / (anchorNm * anchorNm), sv = 0;   // the smooth mean's own share: at anchorNm from one station half of its difference is left
        // softened by a fifth of anchorNm, at most 6 nm: with weights of 1/d^2 alone the field stays flat around each station and then
        // changes steeply halfway to the next one; softened, it slopes evenly. At the station itself about 96 % of its difference is there
        // (its own record carries all of it).
        const double soft = std::min(0.2 * anchorNm, 6.0), soft2 = soft * soft;
        for (const auto& e : q) {
            const double d = distNm(lat, lon, e.lat, e.lon);
            if (d >= 3.0 * anchorNm || e.r == 0) continue;
            double t = (d - 1.5 * anchorNm) / (1.5 * anchorNm);   // 1 up to 1.5 x anchorNm, smoothly down to 0 at 3 x anchorNm
            const double taper = t <= 0 ? 1.0 : 1.0 - t * t * (3.0 - 2.0 * t);
            const double w = taper / (d * d + soft2);
            sw += w; sv += w * e.r;
        }
        out += sv / sw;
        return true;
    }
};

}  // namespace wx
