#include "turbfetch.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

#include "geo.h"
#include "gribx.h"
#include "http.h"

namespace wx {
namespace turb {
namespace {

std::string utc(std::time_t t, const char* fmt) {
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[64];
    std::strftime(b, sizeof b, fmt, &g);
    return b;
}
std::string range(size_t a, size_t b) { return std::to_string(a) + "-" + std::to_string(b); }
bool inGtgnBox(double lat, double lon) { return lat > 19 && lat < 56 && lon > -139 && lon < -57; }

// Crops a decoded global lat/lon field to a smaller regular grid (no wrap-around in the result).
bool cropLatLon(const gribx::Grid& g, const std::vector<float>& full, double clat, double clon, double halfDeg, gribx::Grid& sub, std::vector<float>& out) {
    if (g.tmpl != 0 || g.di <= 0) return false;
    double north = std::min(90.0, clat + halfDeg), south = std::max(-90.0, clat - halfDeg);
    double halfLon = std::min(179.0, halfDeg / std::max(0.2, std::cos(clat * 3.14159265358979 / 180.0)));
    double fi, fjN, fjS, tmp;
    if (!g.locate(north, clon - halfLon, fi, fjN) || !g.locate(south, clon - halfLon, tmp, fjS)) return false;
    int i0 = (int)std::floor(fi), j0 = (int)std::floor(std::min(fjN, fjS));
    int nj = std::min(g.ny - j0, (int)std::ceil(std::fabs(fjS - fjN)) + 2);
    int ni = (int)std::ceil(2 * halfLon / g.di) + 2;
    if (!g.periodicI()) ni = std::min(ni, g.nx - i0);
    if (ni < 3 || nj < 3) return false;
    sub = g;
    sub.nx = ni; sub.ny = nj; sub.scan = 0;
    double la, lo;
    g.latLonOf(i0, j0, la, lo);
    sub.la1 = la; sub.lo1 = lo;
    out.assign((size_t)ni * nj, 0);
    const bool jPos = (g.scan & 0x40) != 0;
    for (int r = 0; r < nj; ++r) {
        int j = jPos ? j0 + (nj - 1 - r) : j0 + r;   // rows of the result run north -> south
        for (int c = 0; c < ni; ++c) {
            int i = i0 + c; if (g.periodicI()) i %= g.nx;
            out[(size_t)r * ni + c] = full[g.index(i, j)];
        }
    }
    if (jPos) { g.latLonOf(i0, j0 + nj - 1, la, lo); sub.la1 = la; }
    return true;
}

} // namespace

void Fetcher::start() {
    stop_ = false;
    th_ = std::thread([this] { run(); });
}

void Fetcher::stop() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Fetcher::update(double lat, double lon, double alt, bool enabled, bool reports) {
    std::lock_guard<std::mutex> l(mx_);
    bool wake = (enabled && !enabled_) || (reports && !reports_);
    lat_ = lat; lon_ = lon; alt_ = alt; enabled_ = enabled; reports_ = reports; havePos_ = true;
    if (wake) cv_.notify_all();
}

Inputs Fetcher::inputs() const { std::lock_guard<std::mutex> l(mx_); return pub_; }
std::shared_ptr<const hz::Snapshot> Fetcher::reports() const { std::lock_guard<std::mutex> l(mx_); return hz_; }

static std::string clockZ(std::time_t t) {
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02dZ", (int)((t / 3600) % 24), (int)((t / 60) % 60));
    return b;
}

std::vector<ServiceStatus> Fetcher::services(std::time_t now) const {
    std::lock_guard<std::mutex> l(mx_);
    ServiceStatus pr{"NOAA Aviation Weather Center", "pilot reports"}, gt{"NOAA GTGN", "turbulence (US)"}, ec{"ECMWF Open Data", "turbulence (outside the US)"};
    if (!reports_ && !enabled_) { pr.state = SvcState::Off; pr.detail = "turbulence and windshear are off"; }
    else if (!hz_) pr.detail = "not downloaded yet";
    else {
        pr.lastOk = hz_->fetched;
        pr.state = hzProbPub_.empty() ? SvcState::Ok : SvcState::Degraded;
        pr.detail = std::to_string(hz_->pireps.size()) + " report(s), downloaded " + clockZ(hz_->fetched) + (hzProbPub_.empty() ? "" : "; failed:" + hzProbPub_);
    }
    const bool inUs = havePos_ && inGtgnBox(lat_, lon_);
    if (!enabled_) { gt.state = ec.state = SvcState::Off; gt.detail = ec.detail = "realistic turbulence is off"; }
    else {
        if (!inUs && !pub_.gtgn) { gt.state = SvcState::NotHere; gt.detail = "outside the US"; }
        else if (pub_.gtgn && now - pub_.gtgn->valid <= kGtgnMaxAgeS) { gt.state = gProbPub_.empty() ? SvcState::Ok : SvcState::Degraded; gt.lastOk = pub_.gtgnArrived; gt.detail = "analysis " + clockZ(pub_.gtgn->valid) + (gProbPub_.empty() ? "" : "; " + gProbPub_); }
        else if (!inUs) { gt.state = SvcState::NotHere; gt.detail = "outside the US"; }
        else if (!gProbPub_.empty()) { gt.state = SvcState::Down; gt.detail = gProbPub_; }
        else gt.detail = "not downloaded yet";
        if (pub_.ecA) { ec.state = eProbPub_.empty() ? SvcState::Ok : SvcState::Degraded; ec.detail = pub_.ecA->run + (eProbPub_.empty() ? "" : "; " + eProbPub_); }
        else if (!eProbPub_.empty()) { ec.state = SvcState::Down; ec.detail = eProbPub_; }
        else if (inUs && pub_.gtgn) { ec.state = SvcState::NotHere; ec.detail = "GTGN covers this area"; }
        else ec.detail = "not downloaded yet";
    }
    return {pr, gt, ec};
}

std::vector<std::string> Fetcher::takeLog() {
    std::lock_guard<std::mutex> l(mx_);
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

void Fetcher::log(const std::string& s) { std::lock_guard<std::mutex> l(mx_); if (log_.size() < 200) log_.push_back(s); }

bool Fetcher::get(const std::string& url, const std::string& rg, std::string& body, long& st, std::string& err, long timeout, size_t maxBytes) {
    // NOMADS throttles clients that send many requests in a burst (an HTTP 302 to its "over the limit" page was seen on
    // 2026-11-06 during the GTGN header walk). Requests to it are spaced at least 0.6 s apart (at most 100 a minute).
    if (url.find("nomads.ncep.noaa.gov") != std::string::npos) {
        static std::chrono::steady_clock::time_point last{};
        const auto due = last + std::chrono::milliseconds(600);
        std::unique_lock<std::mutex> l(mx_);
        cv_.wait_until(l, due, [&] { return stop_.load(); });
        if (stop_) return false;
        last = std::chrono::steady_clock::now();
    }
    bool ok = httpGetCancellable(url, rg, body, st, err, timeout, maxBytes, &stop_);
    if (!ok && (st == 302 || st == 429 || st == 503) && url.find("nomads.ncep.noaa.gov") != std::string::npos) {
        // Throttled: wait 20 s and try once more.
        std::unique_lock<std::mutex> l(mx_);
        cv_.wait_for(l, std::chrono::seconds(20), [&] { return stop_.load(); });
        if (stop_) return false;
        l.unlock();
        ok = httpGetCancellable(url, rg, body, st, err, timeout, maxBytes, &stop_);
        if (!ok) err += " (NOMADS throttling, retried once)";
    }
    return ok;
}

void Fetcher::run() {
    while (!stop_) {
        double lat, lon, alt; bool en, pos, rep;
        {
            std::unique_lock<std::mutex> l(mx_);
            cv_.wait_for(l, std::chrono::seconds(5), [&] { return stop_.load(); });
            if (stop_) break;
            lat = lat_; lon = lon_; alt = alt_; en = enabled_; pos = havePos_; rep = reports_;
        }
        if ((!en && !rep) || !pos) continue;
        std::time_t now = std::time(nullptr);
        if (rep) reportsStep(lat, lon, now);
        if (stop_ || !en) continue;
        gtgnStep(lat, lon, alt, now);
        if (stop_) break;
        ecStep(lat, lon, alt, now);
        std::lock_guard<std::mutex> l(mx_);
        gProbPub_ = gLastProblem_; eProbPub_ = eLastProblem_;
    }
}

// ---- Pilot reports (NOAA AWC data API), every 10 minutes ---------------------------------------
void Fetcher::reportsStep(double lat, double lon, std::time_t now) {
    const bool moved = hzLat_ > 900 || distNm(lat, lon, hzLat_, hzLon_) > 300;
    if (now < hzNext_ && !moved) return;
    hzNext_ = now + 600;
    auto snap = std::make_shared<hz::Snapshot>();
    std::shared_ptr<const hz::Snapshot> old = reports();
    std::string body, err, problems;
    long st = 0;
    const std::string base = "https://aviationweather.gov/api/data/";
    char bbox[96];
    std::snprintf(bbox, sizeof bbox, "%.1f,%.1f,%.1f,%.1f", std::max(-90.0, lat - 8), std::max(-180.0, lon - 12), std::min(90.0, lat + 8), std::min(180.0, lon + 12));
    // Reports around the aircraft (the API returns at most a few hundred, so the box is kept to ~1,000 km).
    if (get(base + "pirep?format=json&age=1.5&bbox=" + bbox, "", body, st, err, 30, 8u << 20)) snap->pireps = hz::parsePireps(body, &err);
    else { problems += " PIREPs: " + (st ? "HTTP " + std::to_string(st) : err) + ";"; if (old) snap->pireps = old->pireps; }
    // AIRMETs, SIGMETs and convective SIGMETs, for the radar map (the simulation does not use them)
    {
        auto one = [&](const std::string& url, const char* what, auto parse) {
            std::string b, e;
            long s = 0;
            if (get(url, "", b, s, e, 30, 16u << 20)) { auto v = parse(b, &e); snap->advisories.insert(snap->advisories.end(), v.begin(), v.end()); return true; }
            problems += std::string(" ") + what + ": " + (s ? "HTTP " + std::to_string(s) : e) + ";";
            return false;
        };
        bool ok = one(base + "airsigmet?format=json", "SIGMETs", [](const std::string& b, std::string* e) { return hz::parseAirSigmets(b, e); });
        ok &= one(base + "isigmet?format=json", "international SIGMETs", [](const std::string& b, std::string* e) { return hz::parseISigmets(b, e); });
        ok &= one(base + "gairmet?format=json", "G-AIRMETs", [&](const std::string& b, std::string* e) { return hz::parseGAirmets(b, now, e); });
        if (!ok && old) for (const auto& a : old->advisories) { bool have = false; for (const auto& x : snap->advisories) if (x.name == a.name && x.from == a.from) have = true; if (!have) snap->advisories.push_back(a); }
    }
    snap->fetched = now;
    hzLat_ = lat; hzLon_ = lon;
    int turbP = 0, llws = 0;
    for (const auto& p : snap->pireps) { if (hz::edrForIntensity(p.intensity, p.acType) > 0) ++turbP; if (p.llws) ++llws; }
    if (!problems.empty()) { ++hzFail_; hzNext_ = now + std::min(600, 60 * hzFail_); } else hzFail_ = 0;
    char b[300];
    int nA = 0, nS = 0, nC = 0;
    for (const auto& a : snap->advisories) { if (a.cls == hz::Advisory::Airmet) ++nA; else if (a.cls == hz::Advisory::Convective) ++nC; else ++nS; }
    std::snprintf(b, sizeof b, "reports: %zu PIREPs/AIREPs within ~500 nm (%d with turbulence, %d with low-level wind shear); map: %d AIRMET, %d SIGMET, %d convective%s",
                  snap->pireps.size(), turbP, llws, nA, nS, nC, problems.empty() ? "" : ("; failed:" + problems).c_str());
    log(b);
    std::lock_guard<std::mutex> l(mx_);
    hz_ = snap;
    hzProbPub_ = problems;
}

// ---- NOAA GTGN ----------------------------------------------------------------------------------------------------
void Fetcher::gtgnStep(double lat, double lon, double alt, std::time_t now) {
    if (!inGtgnBox(lat, lon) || now < gNextTry_) return;
    std::string err, body;
    long st = 0;

    // 1) The newest analysis (every 5 minutes): one is published every 15 minutes.
    if (now >= gNextCheck_) {
        gNextCheck_ = now + 300;
        // Files are in hour folders: gtgn.YYYYMMDD/HH/gtgn.tHHMMz.3km.grib2 (seen on NOMADS 2026-11-06; no .idx files), each
        // about 27 MB, posted 1-2 minutes after its time. The newest of the last 2 hours is tried by name.
        std::string found, foundHead, codes;
        for (int k = 0; k < 8 && !stop_; ++k) {
            std::time_t t = (now / 900) * 900 - (std::time_t)k * 900;
            if (now - t < 300) continue;   // still being uploaded (2026-11-06: read at +2 min, it held only the levels up to 5,000 ft)
            std::string url = "https://nomads.ncep.noaa.gov/pub/data/nccf/com/gtgn/prod/gtgn." + utc(t, "%Y%m%d") + "/" + utc(t, "%H") + "/gtgn.t" +
                              utc(t, "%H%M") + "z.3km.grib2";
            if (url == gFile_) { found = url; break; }
            auto miss = gMissing_.find(url);
            if (miss != gMissing_.end() && now < miss->second) continue;
            if (get(url, "0-4095", body, st, err, 20, 64 * 1024)) { found = url; foundHead = body; break; }
            codes += (codes.empty() ? "" : ", ") + utc(t, "%H%MZ") + " " + (st ? "HTTP " + std::to_string(st) : err);
            if (st == 404) gMissing_[url] = now + (k < 2 ? 240 : 3600);
        }
        if (found.empty()) {
            ++gFail_;
            std::string p = "GTGN: no analysis in the last 2 hours (" + codes + ")";
            if (p != gLastProblem_) { log(p); gLastProblem_ = p; }
            gNextTry_ = now + std::min(900, 60 * gFail_);
            return;
        }
        if (found != gFile_) log("GTGN: newest analysis " + found.substr(found.find("gtgn.2")));
        std::time_t foundT = 0;
        if (found != gFile_) {
            // 2) Its layout: the header of every message, read with small byte ranges.
            std::vector<GMsg> lay;
            std::set<int> otherTypes;
            gribx::Grid grid;
            int packing = -1;
            size_t off = 0;
            std::string head = foundHead;
            for (int n = 0; n < 160 && !stop_; ++n) {
                if (head.empty() && !get(found, range(off, off + 4095), head, st, err, 20, 64 * 1024)) break;
                gribx::Message m;
                if (!gribx::readHeader((const uint8_t*)head.data(), head.size(), m)) {
                    if (n == 0) log("GTGN: " + found + ": first message unreadable: " + m.error);
                    break;
                }
                if (isEdr(m)) {
                    double ft = gtgnLevelFt(m);
                    if (std::isnan(ft)) otherTypes.insert(m.levelType);
                    else { lay.push_back({off, m.length, ft}); grid = m.grid; packing = m.packing; if (!foundT) foundT = m.validTime(); }
                }
                off += m.length;
                head.clear();
            }
            if (lay.empty()) {
                ++gFail_;
                std::string p = "GTGN: " + found + " has no usable EDR levels" + (otherTypes.empty() ? "" : " (level types found: " + std::to_string(*otherTypes.begin()) + ")");
                if (p != gLastProblem_) { log(p); gLastProblem_ = p; }
                gNextTry_ = now + 600;
                return;
            }
            std::sort(lay.begin(), lay.end(), [](const GMsg& a, const GMsg& b) { return a.altFt < b.altFt; });
            if (lay.back().altFt < 45000) {   // the file ends early: not completely written yet
                log("GTGN: " + found.substr(found.rfind('/') + 1) + " is incomplete (" + std::to_string(lay.size()) + " levels, up to " +
                    std::to_string((int)lay.back().altFt) + " ft); " + (gFile_.empty() ? "trying the one before" : "keeping the previous analysis") + ", rechecking in 2 min");
                gMissing_[found] = now + 120;
                gNextCheck_ = gFile_.empty() ? now : now + 120;
                return;
            }
            if (gFile_.empty() || gLayout_.size() != lay.size())
                log("GTGN: " + found.substr(found.rfind('/') + 1) + ": " + std::to_string(lay.size()) + " EDR levels " + std::to_string((int)lay.front().altFt) +
                    ".." + std::to_string((int)lay.back().altFt) + " ft, " + grid.describe() + ", packing 5." + std::to_string(packing) +
                    (otherTypes.empty() ? "" : ", other level types skipped"));
            gFile_ = found; gValid_ = foundT ? foundT : now; gLayout_ = lay; gRaw_.clear();
            gCube_.reset();
        }
    }
    if (gLayout_.empty()) return;

    // 3) The levels near the aircraft (+-9,500 ft and the ones just beyond), cropped to ~200 km around it.
    std::vector<const GMsg*> need;
    for (size_t k = 0; k < gLayout_.size(); ++k) {
        const GMsg& g = gLayout_[k];
        bool inBand = std::fabs(g.altFt - alt) <= 9500;
        bool next = (k + 1 < gLayout_.size() && std::fabs(gLayout_[k + 1].altFt - alt) <= 9500) || (k > 0 && std::fabs(gLayout_[k - 1].altFt - alt) <= 9500);
        if (inBand || next) need.push_back(&g);
    }
    const bool recrop = gCube_ && !gCube_->cropCovers(lat, lon, 25);
    bool missing = !gCube_ || recrop;
    if (!missing) for (auto* g : need) if (!gCube_->hasLevel(g->altFt)) { missing = true; break; }
    if (!missing) return;

    auto cube = std::make_shared<GtgnCube>();
    if (gCube_ && !recrop) *cube = *gCube_;
    std::vector<const GMsg*> todo = need;
    if (recrop && gCube_)   // a new crop keeps the levels it had
        for (auto& g : gLayout_) if (gCube_->hasLevel(g.altFt) && std::find(todo.begin(), todo.end(), &g) == todo.end()) todo.push_back(&g);
    int added = 0;
    for (auto* g : todo) {
        if (stop_) return;
        if (cube->hasLevel(g->altFt) && !recrop) continue;
        auto it = gRaw_.find(g->off);
        if (it == gRaw_.end()) {
            std::string raw;
            if (!get(gFile_, range(g->off, g->off + g->len - 1), raw, st, err, 60, g->len + 1024)) {
                ++gFail_;
                std::string p = "GTGN: level " + std::to_string((int)g->altFt) + " ft download failed: " + err;
                if (p != gLastProblem_) { log(p); gLastProblem_ = p; }
                gNextTry_ = now + std::min(600, 60 * gFail_);
                break;
            }
            it = gRaw_.emplace(g->off, std::move(raw)).first;
        }
        gribx::Message m;
        std::vector<float> full;
        if (!gribx::decode((const uint8_t*)it->second.data(), it->second.size(), m, full)) {
            std::string p = "GTGN: level " + std::to_string((int)g->altFt) + " ft would not decode: " + m.error;
            if (p != gLastProblem_) { log(p); gLastProblem_ = p; }
            gNextTry_ = now + 900;
            break;
        }
        if (cube->levels.empty()) { cube->grid = m.grid; cube->ni = 0; }
        if (m.validTime()) cube->valid = m.validTime();
        cube->addLevel(g->altFt, full, lat, lon, 70);
        ++added;
    }
    if (!added) return;
    gFail_ = 0;
    if (!cube->valid) cube->valid = gValid_;
    cube->file = gFile_.substr(gFile_.rfind('/') + 1);
    // Raw messages far from the aircraft are dropped (they are downloaded again if it climbs back).
    for (auto it = gRaw_.begin(); it != gRaw_.end();) {
        bool keep = false;
        for (auto& g : gLayout_) if (g.off == it->first && std::fabs(g.altFt - alt) <= 20000) keep = true;
        it = keep ? std::next(it) : gRaw_.erase(it);
    }
    {
        std::lock_guard<std::mutex> l(mx_);
        const bool newFile = !pub_.gtgn || pub_.gtgn->file != cube->file;
        if (newFile) { pub_.gtgnPrev = pub_.gtgn; pub_.gtgnArrived = now; }
        pub_.gtgn = cube;
    }
    gCube_ = cube;
    gLastProblem_.clear();
    char b[200];
    std::snprintf(b, sizeof b, "GTGN: %s valid %s loaded: %d level(s) %s, %zu in memory, crop %dx%d around %.2f,%.2f", cube->file.c_str(),
                  utc(cube->valid, "%H:%MZ").c_str(), added, recrop ? "(new crop)" : "", cube->levels.size(), cube->ni, cube->nj, lat, lon);
    log(b);
}

// ---- ECMWF Open Data ----------------------------------------------------------------------------------------------
void Fetcher::ecStep(double lat, double lon, double alt, std::time_t now) {
    (void)alt;
    // Needed outside the US, near the GTGN edge, or when GTGN has nothing current.
    {
        Inputs in = inputs();
        double e, c = 0;
        bool gOk = in.gtgn && now - in.gtgn->valid <= kGtgnMaxAgeS && in.gtgn->sample(lat, lon, std::max(alt, 12000.0), e, c);
        if (gOk && c >= 0.999) return;
    }
    if (now < eNextTry_) return;
    std::string err, body;
    long st = 0;
    auto base = [](std::time_t run, int step) {
        int hh = std::atoi(utc(run, "%H").c_str());
        std::string stream = (hh == 0 || hh == 12) ? "oper" : "scda";
        return "https://data.ecmwf.int/forecasts/" + utc(run, "%Y%m%d") + "/" + utc(run, "%H") + "z/ifs/0p25/" + stream + "/" + utc(run, "%Y%m%d%H") +
               "0000-" + std::to_string(step) + "h-" + stream + "-fc";
    };
    auto maxStep = [](std::time_t run) { int hh = std::atoi(utc(run, "%H").c_str()); return (hh == 0 || hh == 12) ? 144 : 90; };
    auto stepsFor = [&](std::time_t run, int& a, int& b) {
        a = (int)std::max<long>(0, (long)((now - run) / 10800) * 3);
        b = std::min(a + 3, maxStep(run));
    };

    // 1) The newest run whose files are up (checked every 30 minutes).
    if (!eRun_ || now >= eNextRunCheck_) {
        eNextRunCheck_ = now + 1800;
        std::time_t newest = ((now - 6 * 3600) / 21600) * 21600;
        std::string codes;
        for (int k = 0; k < 4 && !stop_; ++k) {
            std::time_t run = newest - (std::time_t)k * 21600;
            if (run == eRun_) break;
            if (eRun_ && run < eRun_) break;
            int a, b; stepsFor(run, a, b);
            if (get(base(run, b) + ".index", "", body, st, err, 30, 4u << 20)) {
                if (run != eRun_) { eRun_ = run; log("ECMWF: using the " + utc(run, "%Y-%m-%d %HZ") + " IFS run"); }
                break;
            }
            codes += (codes.empty() ? "" : ", ") + utc(run, "%HZ") + " " + (st ? "HTTP " + std::to_string(st) : err);
        }
        if (!eRun_) {
            ++eFail_;
            std::string p = "ECMWF: no run found (" + codes + ")";
            if (p != eLastProblem_) { log(p); eLastProblem_ = p; }
            eNextTry_ = now + std::min(1800, 120 * eFail_);
            eNextRunCheck_ = now;
            return;
        }
    }

    // 2) The two forecast steps around now: u and v on the pressure levels, by byte range from the index.
    int a, b; stepsFor(eRun_, a, b);
    std::shared_ptr<const EcCube> cubes[2];
    bool changed = false;
    for (int s = 0; s < 2; ++s) {
        int step = s == 0 ? a : b;
        std::string key = utc(eRun_, "%Y%m%d%H") + "+" + std::to_string(step);
        EStep& E = eSteps_[key];
        E.key = key;
        E.valid = eRun_ + (std::time_t)step * 3600;
        if (E.raw.empty()) {
            if (!get(base(eRun_, step) + ".index", "", body, st, err, 30, 4u << 20)) {
                ++eFail_;
                std::string p = "ECMWF: index for +" + std::to_string(step) + " h failed: " + err;
                if (p != eLastProblem_) { log(p); eLastProblem_ = p; }
                eNextTry_ = now + std::min(1800, 120 * eFail_);
                eSteps_.erase(key);
                return;
            }
            auto idx = parseEcmwfIndex(body);
            size_t bytes = 0;
            for (int lev : ecmwfLevels())
                for (const char* p : {"u", "v"}) {
                    if (stop_) return;
                    auto it = std::find_if(idx.begin(), idx.end(), [&](const IndexEntry& x) { return x.param == p && x.levtype == "pl" && x.level == lev; });
                    if (it == idx.end()) continue;
                    std::string raw;
                    if (!get(base(eRun_, step) + ".grib2", range(it->offset, it->offset + it->length - 1), raw, st, err, 60, it->length + 1024)) {
                        ++eFail_;
                        std::string pr = std::string("ECMWF: ") + p + std::to_string(lev) + " +" + std::to_string(step) + " h download failed: " + err;
                        if (pr != eLastProblem_) { log(pr); eLastProblem_ = pr; }
                        eNextTry_ = now + std::min(1800, 120 * eFail_);
                        eSteps_.erase(key);
                        return;
                    }
                    bytes += raw.size();
                    E.raw[std::string(p) + std::to_string(lev)] = std::move(raw);
                }
            log("ECMWF: " + utc(eRun_, "%HZ") + " run +" + std::to_string(step) + " h (valid " + utc(E.valid, "%d %H:%MZ") + "): " +
                std::to_string(E.raw.size()) + " wind fields, " + std::to_string(bytes / 1024) + " KB");
        }
        if (!E.cube || !E.cube->cropCovers(lat, lon, 3.0)) {
            // Decode level by level, cropping each to +-12 degrees so whole global fields are never all held at once.
            std::vector<std::vector<float>> us, vs;
            std::vector<int> levs;
            gribx::Grid sub;
            std::string problem;
            for (int lev : ecmwfLevels()) {
                auto iu = E.raw.find("u" + std::to_string(lev)), iv = E.raw.find("v" + std::to_string(lev));
                if (iu == E.raw.end() || iv == E.raw.end()) continue;
                gribx::Message mu, mv;
                std::vector<float> fu, fv, cu, cv;
                if (!gribx::decode((const uint8_t*)iu->second.data(), iu->second.size(), mu, fu)) { problem = mu.error; break; }
                if (!gribx::decode((const uint8_t*)iv->second.data(), iv->second.size(), mv, fv)) { problem = mv.error; break; }
                gribx::Grid g2;
                if (!cropLatLon(mu.grid, fu, lat, lon, 12.0, sub, cu) || !cropLatLon(mv.grid, fv, lat, lon, 12.0, g2, cv)) { problem = "crop failed"; break; }
                us.push_back(std::move(cu)); vs.push_back(std::move(cv)); levs.push_back(lev);
            }
            auto cube = std::make_shared<EcCube>();
            std::vector<EcLevelField> f;
            for (size_t k = 0; k < levs.size(); ++k) f.push_back({levs[k], &us[k], &vs[k]});
            if (problem.empty() && !buildEcCube(sub, f, lat, lon, 11.0, *cube, problem)) {}
            if (!problem.empty()) {
                std::string p = "ECMWF: +" + std::to_string(step) + " h would not decode: " + problem;
                if (p != eLastProblem_) { log(p); eLastProblem_ = p; }
                eNextTry_ = now + 1800;
                return;
            }
            cube->valid = E.valid;
            cube->run = utc(eRun_, "%Y-%m-%d %HZ") + " +" + std::to_string(step) + " h";
            E.cube = cube;
            changed = true;
        }
        cubes[s] = E.cube;
    }
    // Steps no longer needed are dropped.
    for (auto it = eSteps_.begin(); it != eSteps_.end();) {
        bool keep = it->second.cube == cubes[0] || it->second.cube == cubes[1] || it->second.cube == nullptr;
        it = keep ? std::next(it) : eSteps_.erase(it);
    }
    eFail_ = 0;
    eLastProblem_.clear();
    if (changed) {
        std::lock_guard<std::mutex> l(mx_);
        pub_.ecA = cubes[0];
        pub_.ecB = cubes[1] != cubes[0] ? cubes[1] : nullptr;
    }
}

} // namespace turb
} // namespace wx
