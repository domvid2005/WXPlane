#include "worldfetch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <ctime>

#include "http.h"
#include "inflate.h"
#include "json_mini.h"
#include "worldmetar.h"
#include "geo.h"

namespace wx {

void WorldMetars::start() {
    stop();
    stop_ = false;
    th_ = std::thread([this]() { run(); });
}

void WorldMetars::stop() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void WorldMetars::want(bool on, const std::string& xpMetarDir) {
    std::lock_guard<std::mutex> lk(m_);
    if (xpMetarDir != xpDir_) {
        const bool newlyOn = xpDir_.empty() && !xpMetarDir.empty();
        xpDir_ = xpMetarDir;
        if (newlyOn && data_ && want_) next_ = std::chrono::steady_clock::now();   // write the file at once, not in 10 minutes
        cv_.notify_all();
    }
    if (on == want_) return;
    want_ = on;
    if (!on) data_.reset();
    else next_ = std::chrono::steady_clock::now();   // wanted again: fetch at once
    cv_.notify_all();
}

std::shared_ptr<const std::vector<Station>> WorldMetars::stations() const {
    std::lock_guard<std::mutex> lk(m_);
    return data_;
}

std::vector<std::string> WorldMetars::takeLog() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

void WorldMetars::log(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (log_.size() < 50) log_.push_back(s);
}

void WorldMetars::run() {
    using Clock = std::chrono::steady_clock;
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lk(m_);
            while (!stop_ && !(want_ && Clock::now() >= next_)) {
                if (want_) cv_.wait_until(lk, next_); else cv_.wait(lk);
            }
            if (stop_) return;
        }
        std::string body, err;
        long status = 0;
        const auto t0 = Clock::now();
        bool ok = httpGetCancellable(kWorldMetarUrl, "", body, status, err, 30, 16u * 1024u * 1024u, &stop_) && status == 200;
        if (stop_) return;
        const double kb = body.size() / 1024.0;
        std::vector<Station> st;
        if (ok) {
            std::string csv;
            if (body.size() >= 2 && (unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b) {   // gzip (else already unpacked in transit)
                std::vector<uint8_t> raw;
                if (gunzip(body, raw, 96u * 1024u * 1024u)) csv.assign(raw.begin(), raw.end());
                else { ok = false; err = "could not unpack the file"; }
            } else csv.swap(body);
            if (ok && !parseWorldMetarCsv(csv, std::time(nullptr), st)) { ok = false; err = "no METAR table in the file"; }
        }
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        char b[512];
        if (ok) {
            std::string dir;
            { std::lock_guard<std::mutex> lk(m_); dir = xpDir_; }
            std::snprintf(b, sizeof b, "world METARs (for the map%s; not sent to X-Plane's weather): %zu stations, %.0f kB in %.1f s",
                          dir.empty() ? "" : " and X-Plane's METAR file", st.size(), kb, s);
            log(b);
            if (!dir.empty()) {
                std::string werr;
                const std::string name = writeXpMetarFile(dir, st, std::time(nullptr), xpWritten_, &werr);
                if (!name.empty()) {
                    if (name != xpWritten_) {
                        std::snprintf(b, sizeof b, "X-Plane's METAR file for add-ons: %s/%s (%zu reports, rewritten every 10 min)", dir.c_str(), name.c_str(), st.size());
                        log(b);
                    }
                    xpWritten_ = name;
                } else {
                    std::snprintf(b, sizeof b, "X-Plane's METAR file for add-ons: not written to %s (%s)", dir.c_str(), werr.c_str());
                    log(b);
                }
            }
            std::lock_guard<std::mutex> lk(m_);
            if (want_) data_ = std::make_shared<const std::vector<Station>>(std::move(st));
            next_ = Clock::now() + std::chrono::minutes(10);
        } else {
            std::snprintf(b, sizeof b, "world METARs: download failed (%s) - next try in 2 min",
                          err.empty() ? ("HTTP " + std::to_string(status)).c_str() : err.c_str());
            log(b);
            std::lock_guard<std::mutex> lk(m_);
            next_ = Clock::now() + std::chrono::minutes(2);
        }
    }
}

} // namespace wx

// ---- the map beyond the loaded weather (display only) ------------------------------------------------------------------------------
namespace wx {

namespace {
double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Open-Meteo answers one object per point (an array for several): cloud cover low / mid / high now
std::vector<FarData::Cloud> parseCloudBands(const std::string& body, const std::vector<std::pair<double, double>>& pts, long long now) {
    std::vector<FarData::Cloud> out;
    Json root;
    if (!parseJson(body, root)) return out;
    std::vector<const Json*> locs;
    if (root.type == Json::Arr) for (const auto& o : root.arr) locs.push_back(&o);
    else if (root.type == Json::Obj && !root.get("error")) locs.push_back(&root);
    for (size_t i = 0; i < locs.size() && i < pts.size(); ++i) {
        const Json* h = locs[i]->get("hourly");
        if (!h || h->type != Json::Obj) continue;
        size_t idx = 0;
        const Json* tm = h->get("time");
        if (tm && tm->type == Json::Arr && !tm->arr.empty()) {
            double best = 1e18;
            for (size_t k = 0; k < tm->arr.size(); ++k) { const double d = std::fabs(tm->arr[k].asNum(0) - (double)now); if (d < best) { best = d; idx = k; } }
        }
        auto val = [&](const char* name, double& v) {
            const Json* a = h->get(name);
            if (!a || a->type != Json::Arr || idx >= a->arr.size() || a->arr[idx].type != Json::Num) return false;
            v = std::max(0.0, std::min(1.0, a->arr[idx].num / 100.0));
            return true;
        };
        FarData::Cloud c;
        c.lat = pts[i].first; c.lon = pts[i].second;
        if (val("cloud_cover_low", c.low) | val("cloud_cover_mid", c.mid) | val("cloud_cover_high", c.high)) out.push_back(c);
    }
    return out;
}
} // namespace

void FarField::start() {
    stop();
    stop_ = false;
    th_ = std::thread([this]() { run(); });
}

void FarField::stop() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void FarField::view(double lat, double lon, double rangeNm, double nearLat, double nearLon, double nearNm) {
    std::lock_guard<std::mutex> lk(m_);
    want_ = V{lat, lon, rangeNm, nearLat, nearLon, nearNm};
}

std::shared_ptr<const FarData> FarField::data() const {
    std::lock_guard<std::mutex> lk(m_);
    return data_;
}

std::vector<std::string> FarField::takeLog() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

void FarField::log(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (log_.size() < 50) log_.push_back(s);
}

void FarField::run() {
    double lastStep = -1e9;
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [&]() { return stop_.load(); });
        }
        if (stop_) return;
        V v;
        { std::lock_guard<std::mutex> lk(m_); v = want_; }
        const double t = nowS();
        if (v.range <= 0) { last_ = v; continue; }
        // a new view: wait until it has stood still for 1.5 s (dragging, zooming), then fetch what it needs
        const bool moved = std::fabs(v.range - last_.range) > 0.05 * std::max(1.0, last_.range) ||
                           distNm(v.lat, v.lon, last_.lat, last_.lon) > 0.15 * std::max(20.0, v.range);
        if (moved) { last_ = v; changedAt_ = t; fresh_ = true; continue; }
        if ((fresh_ && t - changedAt_ >= 1.5) || t - lastStep >= 120.0) {
            fresh_ = false;
            lastStep = t;
            if (t >= failUntil_) step(v);
        }
    }
}

void FarField::step(const V& v) {
    const double t = nowS();
    const long long unixNow = (long long)std::time(nullptr);
    // only once the map shows more than the loaded weather
    const double reach = distNm(v.lat, v.lon, v.nLat, v.nLon) + v.range * 1.42;
    if (reach <= v.nNm) return;
    double stepDeg = v.range <= 200 ? 2 : v.range <= 400 ? 3 : v.range <= 800 ? 5 : v.range <= 1600 ? 10 : 15;
    const double kx = std::max(0.2, std::cos(v.lat * 3.14159265358979 / 180.0));
    const double la0 = std::max(-80.0, v.lat - v.range * 1.15 / 60.0), la1 = std::min(80.0, v.lat + v.range * 1.15 / 60.0);
    const double halfLon = std::min(180.0, v.range * 1.15 / (60.0 * kx));
    std::vector<std::pair<double, double>> grid;
    for (int tries = 0; tries < 3; ++tries) {
        grid.clear();
        for (double a = std::ceil(la0 / stepDeg) * stepDeg; a <= la1 + 1e-9; a += stepDeg)
            for (double o = std::ceil((v.lon - halfLon) / stepDeg) * stepDeg; o <= v.lon + halfLon + 1e-9; o += stepDeg) {
                double on = o;
                while (on > 180) on -= 360;
                while (on <= -180) on += 360;
                if (distNm(a, on, v.nLat, v.nLon) < v.nNm) continue;   // the loaded weather has its own there
                grid.push_back({a, on});
            }
        if (grid.size() <= 120) break;
        stepDeg *= 1.5;
    }
    auto key = [&](double a, double o) { char b[64]; std::snprintf(b, sizeof b, "%g:%.2f:%.2f", stepDeg, a, o); return std::string(b); };
    std::vector<std::pair<double, double>> need;
    for (const auto& g : grid) {
        auto it = nodes_.find(key(g.first, g.second));
        if (it == nodes_.end() || t - it->second.at > 3600) need.push_back(g);
    }
    std::string problems;
    int gotW = 0, gotC = 0;
    // in batches of 60 points: winds at 7 levels (28 values a point) and the three cloud-cover bands
    static const std::vector<int> kLv = {850, 700, 500, 400, 300, 250, 200};
    for (size_t b0 = 0; b0 < need.size() && !stop_; b0 += 60) {
        const std::vector<std::pair<double, double>> pts(need.begin() + (long)b0, need.begin() + (long)std::min(need.size(), b0 + 60));
        std::string body, err;
        long st = 0;
        if (httpGetCancellable(openMeteoUrl(pts, kLv, false), "", body, st, err, 25, 8u << 20, &stop_) && st == 200) {
            for (auto& w : parseOpenMeteo(body, pts, kLv, unixNow)) {
                Node& n = nodes_[key(w.lat, w.lon)];
                w.model = true; w.located = true;
                n.wind = w; n.hasWind = true; n.at = t; ++gotW;
            }
        } else problems += " winds " + (st ? "HTTP " + std::to_string(st) : err) + ";";
        if (stop_) return;
        std::string la, lo;
        char q[32];
        for (size_t i = 0; i < pts.size(); ++i) {
            std::snprintf(q, sizeof q, "%.2f", pts[i].first); la += (i ? "," : "") + std::string(q);
            std::snprintf(q, sizeof q, "%.2f", pts[i].second); lo += (i ? "," : "") + std::string(q);
        }
        const std::string cu = "https://api.open-meteo.com/v1/gfs?latitude=" + la + "&longitude=" + lo +
                               "&hourly=cloud_cover_low,cloud_cover_mid,cloud_cover_high&forecast_hours=2&timeformat=unixtime";
        body.clear(); err.clear(); st = 0;
        if (httpGetCancellable(cu, "", body, st, err, 25, 8u << 20, &stop_) && st == 200) {
            for (const auto& c : parseCloudBands(body, pts, unixNow)) {
                Node& n = nodes_[key(c.lat, c.lon)];
                n.cloud = c; n.hasCloud = true; n.at = t; ++gotC;
            }
        } else problems += " clouds " + (st ? "HTTP " + std::to_string(st) : err) + ";";
    }
    // pilot reports over the view (the loaded weather has its own around the aircraft), every 10 minutes or when the view leaves the box
    int gotP = -1;
    const double lo0 = std::max(-180.0, v.lon - halfLon), lo1 = std::min(180.0, v.lon + halfLon);
    if (!stop_ && (t - pAt_ > 600 || la0 < pLat0_ || la1 > pLat1_ || lo0 < pLon0_ || lo1 > pLon1_)) {
        char bb[96];
        std::snprintf(bb, sizeof bb, "%.1f,%.1f,%.1f,%.1f", la0, lo0, la1, lo1);
        std::string body, err;
        long st = 0;
        if (httpGetCancellable(std::string("https://aviationweather.gov/api/data/pirep?format=json&age=1.5&bbox=") + bb, "", body, st, err, 30, 16u << 20, &stop_) && st == 200) {
            pireps_ = hz::parsePireps(body, &err);
            pLat0_ = la0; pLat1_ = la1; pLon0_ = lo0; pLon1_ = lo1; pAt_ = t;
            gotP = (int)pireps_.size();
        } else if (st != 204) problems += " PIREPs " + (st ? "HTTP " + std::to_string(st) : err) + ";";
        else { pireps_.clear(); pAt_ = t; gotP = 0; }
    }
    if (!problems.empty()) failUntil_ = t + 120;
    if (!stop_) radarStep(v, la0, la1, lo0, lo1);
    // what the view shows now
    auto d = std::make_shared<FarData>();
    d->gridDeg = stepDeg;
    for (const auto& g : grid) {
        auto it = nodes_.find(key(g.first, g.second));
        if (it == nodes_.end()) continue;
        if (it->second.hasWind) d->aloft.push_back(it->second.wind);
        if (it->second.hasCloud) d->clouds.push_back(it->second.cloud);
    }
    d->pireps = pireps_;
    d->radar = radar_; d->radarSource = radarSrc_;
    if (nodes_.size() > 4000) {   // old views: dropped after 2 hours
        for (auto it = nodes_.begin(); it != nodes_.end();) it = t - it->second.at > 7200 ? nodes_.erase(it) : std::next(it);
    }
    if (!need.empty() || gotP >= 0 || !problems.empty()) {
        char b[300];
        std::snprintf(b, sizeof b, "map beyond the loaded weather (display only): %.0f nm view at %.1f,%.1f, %g deg grid: %d wind / %d cloud point(s) fetched, %zu shown%s%s",
                      v.range, v.lat, v.lon, stepDeg, gotW, gotC, d->aloft.size(),
                      gotP >= 0 ? (", " + std::to_string(gotP) + " PIREP(s)").c_str() : "",
                      problems.empty() ? "" : ("; failed:" + problems).c_str());
        log(b);
    }
    std::lock_guard<std::mutex> lk(m_);
    data_ = d;
}

} // namespace wx

namespace wx {

// Europe's radar over the view (display only): the newest OPERA composite, only its tiles under the view, every 10 minutes or when the
// view leaves the last box; at 0.03 degrees (about 2 nm), like the US mosaic beyond the loaded weather.
void FarField::radarStep(const V& v, double la0, double la1, double lo0, double lo1) {
    const double t = nowS();
    // the view must reach into the OPERA domain
    bool touches = false;
    for (double a : {la0, (la0 + la1) / 2, la1})
        for (double o : {lo0, (lo0 + lo1) / 2, lo1}) touches = touches || operaCovers(a, o);
    if (!touches) { radar_.reset(); return; }
    if (t < rFailUntil_) return;
    if (t - rAt_ < 600 && la0 >= rLat0_ && la1 <= rLat1_ && lo0 >= rLon0_ && lo1 <= rLon1_) return;
    const std::time_t now = std::time(nullptr);
    const std::time_t newest = operaNewestSlot(now);
    std::string err;
    for (int k = 0; k < 4 * kOperaStores && !stop_; ++k) {
        const int store = (opStore_ + k / 4) % kOperaStores;
        const std::time_t slot = newest - (k % 4) * 300;
        const std::string url = operaUrl(slot, store);
        if (slot != opSlot_ || store != opStore_ || opHead_.width == 0) {
            std::string head, e;
            long st = 0;
            OperaHeader h;
            size_t need = 0;
            bool ok = httpGetRange(url, "0-8191", head, st, e, 15, 1u << 20) && (st == 206 || st == 200);
            if (ok && !operaParseHeader(head, h, e, need) && need > head.size() && need < (4u << 20)) {
                head.clear();
                ok = httpGetRange(url, "0-" + std::to_string(need - 1), head, st, e, 20, 8u << 20) && (st == 206 || st == 200) && operaParseHeader(head, h, e, need);
            } else if (ok) ok = operaParseHeader(head, h, e, need);
            if (!ok) { err = e.empty() ? "HTTP " + std::to_string(st) : e; continue; }
            opSlot_ = slot; opStore_ = store; opHead_ = h; opTiles_.clear();
        }
        GribWindow win;
        win.latMin = std::max(la0, 25.0); win.latMax = std::min(la1, 75.0);
        win.lonMin = std::max(lo0, -45.0); win.lonMax = std::min(lo1, 70.0);
        if (win.latMin >= win.latMax || win.lonMin >= win.lonMax) { radar_.reset(); return; }
        auto get = [&](uint64_t off, uint64_t len, std::string& data, std::string& ge) {
            long st = 0;
            const bool ok = httpGetRange(url, std::to_string(off) + "-" + std::to_string(off + len - 1), data, st, ge, 20, (size_t)len + 1024);
            if (ok && st != 206 && !(st == 200 && data.size() == len)) { ge = "HTTP " + std::to_string(st); return false; }
            return ok && data.size() == len;
        };
        GribField f;
        int fetched = 0;
        std::string e;
        if (!operaWindow(opHead_, win, 0.03, get, opTiles_, f, fetched, e)) { err = e; opSlot_ = 0; opTiles_.clear(); continue; }
        auto m = std::make_shared<RadarMosaic>();
        m->lat0 = f.lat0; m->lon0 = f.lon0; m->dLat = f.dLat; m->dLon = f.dLon; m->ni = f.ni; m->nj = f.nj;
        m->v.resize(f.v.size());
        for (size_t i = 0; i < f.v.size(); ++i) {
            const float z = f.v[i];
            m->v[i] = (unsigned char)(std::isnan(z) || z < 5.0f ? 0 : std::min(255.0f, z * 3.0f + 0.5f));
        }
        radar_ = m;
        radarSrc_ = "EUMETNET OPERA";
        rLat0_ = win.latMin; rLat1_ = win.latMax; rLon0_ = win.lonMin; rLon1_ = win.lonMax; rAt_ = t;
        char b[160];
        std::snprintf(b, sizeof b, "map beyond the loaded weather: European radar (OPERA %02d:%02dZ) over the view, %d tile(s) downloaded, %zu kept",
                      (int)((slot / 3600) % 24), (int)((slot / 60) % 60), fetched, opTiles_.size());
        log(b);
        return;
    }
    if (!stop_) { log("map beyond the loaded weather: European radar unavailable (" + err + ")"); rFailUntil_ = t + 300; }
}

} // namespace wx
