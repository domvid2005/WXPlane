#include "fetcher.h"

#include <algorithm>
#include <cmath>

#include "geo.h"
#include "http.h"
#include "inflate.h"
#include "json_mini.h"
#include "png.h"
#include "metar.h"
#include "sources.h"
#include "windsaloft.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#else
#  include <sys/resource.h>
#endif

namespace wx {

void Fetcher::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (running_) return;
    running_ = true;
    th_ = std::thread([this] { run(); });
}

void Fetcher::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(m_);
    hasReq_ = hasRes_ = false;
    working_ = false;
}

void Fetcher::request(const FetchRequest& r) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_ || hasReq_ || working_.load()) return;
        req_ = r;
        hasReq_ = true;
        working_ = true;
    }
    cv_.notify_all();
}

bool Fetcher::take(FetchResult& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (!hasRes_) return false;
    out = std::move(res_);
    hasRes_ = false;
    return true;
}

// Downloads and radar decoding are not urgent; keep them from competing with the sim for CPU (matters most during loading).
static void lowerThreadPriority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    setpriority(PRIO_PROCESS, 0, 10); // on Linux this is per thread
#endif
}

void Fetcher::run() {
    lowerThreadPriority();
    for (;;) {
        FetchRequest r;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return !running_ || hasReq_; });
            if (!running_) return;
            r = req_;
            hasReq_ = false;
        }
        FetchResult res = fetch(r);
        {
            std::lock_guard<std::mutex> lk(m_);
            res_ = std::move(res);
            hasRes_ = running_;
            working_ = false;
        }
    }
}

FetchResult Fetcher::fetch(const FetchRequest& rq) {
    FetchResult res;
    res.centerLat = rq.lat;
    res.centerLon = rq.lon;
    res.immediate = rq.immediate;
    res.fetchedAt = std::time(nullptr);
    res.radarOnly = rq.radarOnly;
    const Config& cfg = rq.cfg;
    if (rq.radarOnly) {
        if (cfg.radar) res.radar = fetchRadar(rq, res.fetchedAt);
        res.ok = res.radar.ok;
        if (!res.ok) res.error = res.radar.note.empty() ? std::string("radar refresh failed") : res.radar.note;
        return res;
    }

    // 1) METARs from NOAA's Aviation Weather Center, centred on the aircraft (not on any airport),
    //    so the weather below you stays current at cruise altitude too.
    // Over the open ocean there is no airport within the search radius (NOAA answers HTTP 204, no data); a crash report of
    // 2026-11-09 came from there. The search then widens to 300 and 600 nm, so the nearest coasts still give weather.
    std::time_t now = res.fetchedAt;
    std::vector<Station> keep;
    std::string widened;
    double prevRadius = -1;
    for (double radius : {cfg.searchNm, std::max(cfg.searchNm, 300.0), std::max(cfg.searchNm, 600.0)}) {
        if (radius <= prevRadius) continue;
        prevRadius = radius;
        std::string body, err;
        long status = 0;
        if (!httpGet(awcMetarUrl(rq.lat, rq.lon, radius), body, status, err)) {
            res.error = "NOAA METAR: " + err;
            return res;
        }
        std::string perr;
        std::vector<Station> all = parseAwcMetarJson(body, &perr);
        if (!perr.empty() && !body.empty()) { res.error = perr; return res; }
        for (auto& s : all) {
            s.distNm = distNm(rq.lat, rq.lon, s.lat, s.lon);
            if (s.distNm > radius * 1.05) continue;
            if (reportAgeSeconds(s.report, now) > cfg.maxAgeHours * 3600.0) continue;
            keep.push_back(std::move(s));
        }
        if (!keep.empty()) {
            if (radius > cfg.searchNm) widened = "no METARs within " + std::to_string((int)cfg.searchNm) + " nm (open water?): the search was widened to " + std::to_string((int)radius) + " nm";
            break;
        }
        if (radius >= 600.0) break;
    }
    std::sort(keep.begin(), keep.end(), [](const Station& a, const Station& b) { return a.distNm < b.distNm; });
    if ((int)keep.size() > cfg.maxStations) keep.resize((size_t)cfg.maxStations);
    res.metarCount = (int)keep.size();
    res.elevNote = checkStationElevations(keep);

    // 2) ATIS for the closest airports (D-ATIS covers major US airports; others simply return nothing).
    if (cfg.useAtis) {
        int tried = 0;
        for (auto& s : keep) {
            if (tried >= cfg.atisMaxAirports || s.distNm > cfg.atisRangeNm) break;
            if (s.icao.size() != 4) continue;
            auto neg = std::find_if(noAtis_.begin(), noAtis_.end(), [&](const std::pair<std::string, std::time_t>& p) { return p.first == s.icao; });
            if (neg != noAtis_.end() && now - neg->second < 3600) continue;
            ++tried;
            std::string abody, aerr;
            long ast = 0;
            std::string text;
            bool got = httpGet(datisUrl(s.icao), abody, ast, aerr, 8) && parseDatisJson(abody, text);
            Report ar;
            if (got && parseReport(text, ar, true)) {
                mergeAtis(s, ar, text);
                ++res.atisCount;
                if (neg != noAtis_.end()) noAtis_.erase(neg);
            } else if (neg != noAtis_.end()) {
                neg->second = now;
            } else {
                noAtis_.emplace_back(s.icao, now);
            }
        }
    }

    // 3) NOAA winds & temperatures aloft (FD winds), so the cruise levels follow real data too.
    // NOAA's FD wind/temperature tables (fetchAloft) were discontinued with the NAM model: AWC answers HTTP 400 ("Wind/temp tables have
    // been discontinued", SCN 26-47, seen 2026-11-09). The global model is now the source at every level.
    if (cfg.useAloft) res.aloftNote = "NOAA wind/temperature tables discontinued (NAM retired)";
    if (cfg.useAloft) {   // the global model at every level
        std::string upNote;
        std::vector<AloftStation> up = fetchUpperAir(rq, now, upNote, res.stations);
        for (auto& u : up) res.aloft.push_back(std::move(u));
        if (!upNote.empty()) res.aloftNote += (res.aloftNote.empty() ? "" : "; ") + upNote;
    }

    // 3b) Global-model clouds and surface weather on the same lattice (every 30 min): cloud layers above what airport reports can see,
    //     and the weather over open water where no airport reports within the search radius.
    if (cfg.useAloft && (cfg.modelClouds || cfg.oceanModelWeather)) {
        std::string cNote;
        std::vector<ModelPoint> mp = fetchModelClouds(rq, now, cNote);
        if (!cNote.empty()) res.aloftNote += (res.aloftNote.empty() ? "" : "; ") + cNote;
        if (cfg.modelClouds)
            for (auto& a : res.aloft) {
                if (!a.model) continue;
                const ModelPoint* best = nullptr; double bd = 1e9;
                for (const auto& p : mp) { const double d = distNm(a.lat, a.lon, p.lat, p.lon); if (d < bd) { bd = d; best = &p; } }
                if (best && bd < 30.0) a.cloudLevels = best->cloudLevels;
            }
        // Open water: no airport within the search radius. The model's surface weather at the lattice nodes becomes reports of its own
        // ("GFS..." stations, location records), so the weather around the aircraft follows the model instead of distant coasts.
        const bool noNear = keep.empty() || keep.front().distNm > cfg.searchNm;
        if (cfg.oceanModelWeather && noNear) {
            int added = 0;
            for (const auto& p : mp) {
                if (!p.sfc) continue;
                Station st = modelStation(p, now);
                st.distNm = distNm(rq.lat, rq.lon, st.lat, st.lon);
                keep.push_back(std::move(st));
                ++added;
            }
            std::sort(keep.begin(), keep.end(), [](const Station& a, const Station& b) { return a.distNm < b.distNm; });
            if (added) widened += std::string(widened.empty() ? "" : "; ") + "no airport within " + std::to_string((int)cfg.searchNm) + " nm: " +
                                  std::to_string(added) + " global-model (GFS) points give the weather around the aircraft";
        }
    }

    // 4) Radar storm cells.
    if (cfg.radar) res.radar = fetchRadar(rq, now);

    res.noaaFixes = 0;
    for (const auto& k : keep) if (!k.filled.empty()) ++res.noaaFixes;
    res.filledElements = fillMissing(keep, 80.0);   // elements neither the text nor NOAA's decoded fields had: from neighbouring stations
    res.stations = std::move(keep);
    res.ok = true;
    if (res.stations.empty()) res.error = "no METARs within 600 nm: the weather already set is kept";
    else if (!widened.empty()) res.error = widened;
    return res;
}

std::vector<AloftStation> Fetcher::fetchAloft(const FetchRequest& rq, std::time_t now, std::string& note, const std::vector<Station>& known) {
    const Config& cfg = rq.cfg;
    std::string region;
    char prefix = 'K';
    aloftRegionFor(rq.lat, rq.lon, region, prefix);

    if (region != alRegion_ || alCache_.empty() || now - alTime_ > 1800) { // FD products change a few times a day
        std::vector<AloftStation> all;
        std::string firstErr;
        for (const char* level : {"low", "high"}) {
            std::string body, err;
            long st = 0;
            if (!httpGet(windTempUrl(region, level, cfg.aloftFcstHours), body, st, err, 15)) {
                if (firstErr.empty()) firstErr = err;
                continue;
            }
            std::vector<AloftStation> got = parseWindTemp(body);
            if (got.empty() && firstErr.empty()) {
                // Every update of the 2026-11-05 flight said "no data parsed": the download worked but nothing in it read as FD lines. The
                // answer's status, size and start now go in the log so the cause (an error page, a format change) can be seen.
                std::string head = body.substr(0, 160);
                for (char& c : head) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                char b[96];
                std::snprintf(b, sizeof b, "%s: HTTP %ld, %zu bytes, nothing read as FD lines, start: ", level, st, body.size());
                firstErr = std::string(b) + "\"" + head + "\"";
            }
            mergeAloft(all, got);
        }
        if (all.empty()) {
            note = firstErr.empty() ? "NOAA winds aloft: no data parsed" : "NOAA winds aloft: " + firstErr;
            if (alCache_.empty() || region != alRegion_) return {};
            all = alCache_; // keep using the previous set rather than dropping to sim winds
        } else {
            alCache_ = std::move(all);
            alRegion_ = region;
            alTime_ = now;
        }
    }

    // FD stations come without coordinates. Many of them are airports we already have a report for ("ABI" -> "KABI"): take the position from there.
    for (const auto& k : known)
        if (k.icao.size() == 4 && k.icao[0] == prefix && !coords_.count(k.icao.substr(1))) coords_[k.icao.substr(1)] = {k.lat, k.lon};
    // The rest are looked up once through their METAR.
    std::vector<std::string> need;
    for (const auto& s : alCache_) {
        if (coords_.count(s.id)) continue;
        auto miss = coordsMiss_.find(s.id);
        if (miss != coordsMiss_.end() && now - miss->second < 6 * 3600) continue;
        need.push_back(s.id);
    }
    for (size_t i = 0; i < need.size(); i += 30) {   // small batches: one bad id or one failed request must not stop the others
        std::string ids;
        size_t end = std::min(need.size(), i + 30);
        for (size_t j = i; j < end; ++j) ids += (j > i ? "," : "") + std::string(1, prefix) + need[j];
        std::string body, err;
        long st = 0;
        if (!httpGet("https://aviationweather.gov/api/data/metar?format=json&ids=" + ids, body, st, err, 15)) continue;   // tried again at the next update
        for (const auto& m : parseAwcMetarJson(body))
            if (m.icao.size() == 4) coords_[m.icao.substr(1)] = {m.lat, m.lon};
        for (size_t j = i; j < end; ++j)
            if (!coords_.count(need[j])) coordsMiss_[need[j]] = now;
    }

    std::vector<AloftStation> out;
    for (auto s : alCache_) {
        auto c = coords_.find(s.id);
        if (c == coords_.end()) continue;
        s.lat = c->second.first;
        s.lon = c->second.second;
        s.located = true;
        if (distNm(rq.lat, rq.lon, s.lat, s.lon) <= std::max(1200.0, cfg.aloftRangeNm + cfg.searchNm + 60)) out.push_back(std::move(s));   // far ones too: places without a closer station use the nearest
    }
    if (out.empty() && note.empty()) note = "no NOAA winds-aloft stations near this position (US/Pacific coverage only)";
    return out;
}

} // namespace wx

namespace wx {

namespace {
// Downloads, gunzips and decodes one MRMS product inside the window.
bool loadMrms(const std::string& url, const GribWindow& win, GribField& f, std::string& err) {
    std::string gz;
    long status = 0;
    if (!httpGet(url, gz, status, err, 60, 40u * 1024u * 1024u)) { err = "download failed (" + err + ")"; return false; }
    std::vector<uint8_t> raw;
    if (!gunzip(gz, raw, 400u * 1024u * 1024u)) { err = "bad gzip data"; return false; }
    gz.clear();
    gz.shrink_to_fit();
    if (!decodeGrib2(raw.data(), raw.size(), win, f)) { err = f.error; return false; }
    return true;
}

GribWindow windowFor(double lat, double lon, double rangeNm) {
    GribWindow w;
    double dLat = rangeNm / 60.0 + 0.1, dLon = rangeNm / (60.0 * std::max(0.2, std::cos(lat * 3.14159265358979 / 180.0))) + 0.1;
    w.latMin = lat - dLat; w.latMax = lat + dLat;
    w.lonMin = std::max(-180.0, lon - dLon); w.lonMax = std::min(180.0, lon + dLon);
    return w;
}
} // namespace

RadarResult Fetcher::fetchRadar(const FetchRequest& rq, std::time_t now) {
    const Config& cfg = rq.cfg;
    RadarResult out;
    out.attempted = true;
    GribWindow win = windowFor(rq.lat, rq.lon, cfg.radarRangeNm);
    std::string lastErr;
    for (const std::string& region : mrmsRegionsFor(rq.lat, rq.lon)) {
        const std::string name = region.empty() ? "CONUS" : region;
        GribField refl;
        if (!loadMrms(mrmsUrl(region, "MergedReflectivityQCComposite"), win, refl, lastErr)) {
            refl = GribField();   // one retry: a truncated download ("bad gzip data", 2026-11-06) made the first load fall back to RainViewer
            if (!loadMrms(mrmsUrl(region, "MergedReflectivityQCComposite"), win, refl, lastErr)) { lastErr = name + ": " + lastErr; continue; }
        }
        if (refl.refTime > 0 && now - refl.refTime > 30 * 60) {
            lastErr = name + ": radar mosaic is " + std::to_string((now - refl.refTime) / 60) + " min old, ignoring";
            continue;
        }
        GribField tops;
        std::string topErr;
        bool haveTops = false;
        bool oldTops = false;
        if (cfg.radarEchoTops) {
            haveTops = loadMrms(mrmsUrl(region, "EchoTop_18"), win, tops, topErr);
            if (!haveTops) { tops = GribField(); haveTops = loadMrms(mrmsUrl(region, "EchoTop_18"), win, tops, topErr); }   // one retry (truncated downloads happen)
            if (haveTops) { lastTops_ = tops; lastTopsRegion_ = region; lastTopsAt_ = now; }
            else if (lastTops_.valid() && lastTopsRegion_ == region && now - lastTopsAt_ < 45 * 60) {
                // Falling back to heights estimated from reflectivity changes every storm's height at once (13.5 km measured became 10.5 km estimated
                // in one update, and the whole sky was rebuilt): keep the last measured tops instead.
                tops = lastTops_; haveTops = true; oldTops = true;
            }
        }
        out = buildRadarCells(refl, haveTops ? &tops : nullptr, rq.lat, rq.lon, cfg, &radarPrev_);
        radarPrev_.clear();
        for (const auto& c : out.cells) radarPrev_.insert(c.key);
        out.source = "NOAA MRMS (" + name + ")";
        out.dataTime = refl.refTime;
        if (oldTops) out.note = "echo-top download failed (" + topErr + "), reusing the last good tops" + " from " + std::to_string((now - lastTopsAt_) / 60) + " min ago";
        else if (cfg.radarEchoTops && !haveTops) out.note = "echo tops unavailable (" + topErr + "), storm tops estimated";
        if (out.ok) { lastMrms_ = out; lastMrmsAt_ = now; lastMrmsLat_ = rq.lat; lastMrmsLon_ = rq.lon; }
        return out;
    }
    // Inside NOAA coverage a failed download used to fall straight through to RainViewer (log 2026-11-04, Ohio): its colour-decoded tiles read
    // the same storms as 153 cumulonimbus cells at 40+ dBZ where MRMS had 7 a few minutes earlier, in blocky colour bands. The last MRMS
    // picture is kept instead while it is under 20 minutes old and the aircraft is within 30 nm of where it was built.
    const bool mrmsArea = !mrmsRegionsFor(rq.lat, rq.lon).empty();
    if (mrmsArea && lastMrms_.ok && now - lastMrmsAt_ < 20 * 60 && distNm(rq.lat, rq.lon, lastMrmsLat_, lastMrmsLon_) < 30.0) {
        RadarResult r = lastMrms_;
        r.note = "MRMS download failed (" + lastErr + "), reusing the radar picture from " + std::to_string((now - lastMrmsAt_) / 60) + " min ago";
        return r;
    }
    if (cfg.radarGlobal) {
        RadarResult g = fetchRainViewer(rq, now);
        if (mrmsArea && !lastErr.empty()) g.note = "MRMS failed (" + lastErr + "), using RainViewer instead" + (g.note.empty() ? "" : "; " + g.note);
        if (g.ok || g.note.size()) return g;
    }
    out.ok = false;
    out.note = lastErr.empty() ? std::string("no NOAA radar coverage here") + (cfg.radarGlobal ? "" : " (enable the global option for other regions)") : lastErr;
    return out;
}

RadarResult Fetcher::fetchRainViewer(const FetchRequest& rq, std::time_t now) {
    (void)now;
    const Config& cfg = rq.cfg;
    RadarResult out;
    out.attempted = true;
    out.source = "RainViewer (global, experimental)";
    std::string body, err;
    long status = 0;
    if (!httpGet("https://api.rainviewer.com/public/weather-maps.json", body, status, err)) { out.note = "RainViewer index failed: " + err; return out; }
    Json j;
    if (!parseJson(body, j)) { out.note = "RainViewer index unreadable"; return out; }
    std::string host = j.get("host") ? j.get("host")->asString() : "";
    const Json* radar = j.get("radar");
    const Json* past = radar ? radar->get("past") : nullptr;
    if (host.empty() || !past || past->arr.empty()) { out.note = "RainViewer has no radar frames"; return out; }
    const Json& frame = past->arr.back();
    std::string path = frame.get("path") ? frame.get("path")->asString() : "";
    if (path.empty()) { out.note = "RainViewer frame has no path"; return out; }
    out.dataTime = frame.get("time") ? (std::time_t)frame.get("time")->asNum() : 0;

    const int z = 6;
    int x0, x1, y0, y1;
    tileRange(rq.lat, rq.lon, cfg.radarRangeNm, z, x0, x1, y0, y1);
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > 16) { out.note = "radar range needs too many tiles"; return out; }
    std::vector<TileImage> tiles;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            std::string url = host + path + "/256/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + "/2/0_0.png";
            std::string png, terr;
            if (!httpGet(url, png, status, terr, 20, 2u * 1024u * 1024u)) continue;
            PngImage im;
            if (!decodePng((const uint8_t*)png.data(), png.size(), im, 1000000) || im.depth != 8 || im.channels != 4) continue;
            TileImage t;
            t.z = z; t.x = x; t.y = y; t.w = im.w; t.h = im.h; t.rgba = std::move(im.px);
            tiles.push_back(std::move(t));
        }
    if (tiles.empty()) { out.note = "RainViewer tiles unavailable"; return out; }
    GribWindow win = windowFor(rq.lat, rq.lon, cfg.radarRangeNm);
    GribField f;
    if (!tilesToField(tiles, win, f)) { out.note = "RainViewer tiles unusable: " + f.error; return out; }
    std::string src = out.source;
    std::time_t dt = out.dataTime;
    out = buildRadarCells(f, nullptr, rq.lat, rq.lon, cfg, &radarPrev_);
    radarPrev_.clear();
    for (const auto& c : out.cells) radarPrev_.insert(c.key);
    out.source = src;
    out.dataTime = dt;
    out.note = "colour-decoded image tiles: intensities and tops are approximate";
    return out;
}

std::vector<ModelPoint> Fetcher::fetchModelClouds(const FetchRequest& rq, std::time_t now, std::string& note) {
    // The same 3x3 lattice as the winds, every 30 minutes (cloud cover changes slowly; it keeps the open-meteo.com cost low).
    const double cLat = std::round(rq.lat), cLon = std::round(rq.lon);
    if (!cloudCache_.empty() && now - cloudTime_ < 1800 && std::fabs(cLat - cloudLat_) < 0.5 && std::fabs(cLon - cloudLon_) < 0.5) return cloudCache_;
    std::vector<std::pair<double, double>> pts;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            double la = cLat + dy, lo = cLon + dx;
            if (la > 89 || la < -89) continue;
            if (lo > 180) lo -= 360;
            if (lo < -180) lo += 360;
            pts.push_back({la, lo});
        }
    std::string body, err;
    long st = 0;
    std::vector<ModelPoint> got;
    if (httpGet(openMeteoCloudUrl(pts), body, st, err, 20)) got = parseOpenMeteoClouds(body, pts, (long long)now, &err);
    if (got.empty()) {
        note = "global-model clouds unavailable (" + (err.empty() ? std::string("no data") : err) + ")";
        if (!cloudCache_.empty() && now - cloudTime_ < 3 * 3600) { note += ", using the earlier download"; return cloudCache_; }
        return {};
    }
    cloudCache_ = got; cloudTime_ = now; cloudLat_ = cLat; cloudLon_ = cLon;
    char b[120];
    std::snprintf(b, sizeof b, "global-model clouds and surface weather: %zu of %zu point(s)", got.size(), pts.size());
    note = b;
    return got;
}

std::vector<AloftStation> Fetcher::fetchUpperAir(const FetchRequest& rq, std::time_t now, std::string& note, const std::vector<Station>& known) {
    // A fixed 1-degree lattice, 3 x 3 nodes around the aircraft (the aircraft is always inside the middle cell; 9 points as before, so
    // the open-meteo.com cost is unchanged). NOAA's FD wind/temperature tables were discontinued with the NAM
    // model (AWC answers HTTP 400, SCN 26-47), so this is now the only source of winds and temperatures aloft. Before, the points were
    // picked from the stations - and the call came before the stations were known, so it was one point at the aircraft, refetched every
    // 60 nm: the whole temperature column changed at once and the pressure surfaces with it (altitude fluctuations reported in cruise,
    // 2026-11-09). On a fixed lattice the same place always uses the same nodes, so the profile changes smoothly along the route.
    (void)known;
    const double cLat = std::round(rq.lat), cLon = std::round(rq.lon);
    if (!upCache_.empty() && now - upTime_ < 2700 && std::fabs(cLat - upLat_) < 0.5 && std::fabs(cLon - upLon_) < 0.5) return upCache_;
    std::vector<std::pair<double, double>> pts;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            double la = cLat + dy, lo = cLon + dx;
            if (la > 89 || la < -89) continue;
            if (lo > 180) lo -= 360;
            if (lo < -180) lo += 360;
            pts.push_back({la, lo});
        }
    const std::vector<int> full = {925, 850, 700, 500, 400, 300, 250, 200, 150, 100, 70};
    const std::vector<int> safe = {925, 850, 700, 500, 400, 300, 250, 200};   // if the endpoint refuses the highest levels
    std::vector<AloftStation> got;
    std::string err, why, sample;
    // high cloud first; if the endpoint refuses that variable, the winds still come without it
    struct Try { const std::vector<int>* lv; bool high; };
    for (const Try& tr : {Try{&full, true}, Try{&full, false}, Try{&safe, false}}) {
        const auto* lv = tr.lv;
        std::string body;
        long st = 0;
        if (!httpGet(openMeteoUrl(pts, *lv, tr.high), body, st, err, 20)) { if (why.empty()) why = err; continue; }
        std::string perr;
        got = parseOpenMeteo(body, pts, *lv, (long long)now, &perr);
        sample = body.substr(0, 200);
        if (!got.empty()) break;
        if (why.empty()) why = perr;
    }
    if (got.empty()) {
        note = "global-model winds aloft unavailable (" + (why.empty() ? std::string("no data") : why) + ")";
        if (!upCache_.empty() && now - upTime_ < 3 * 3600) { note += ", using the earlier download"; return upCache_; }
        return {};
    }
    upCache_ = got; upTime_ = now; upLat_ = cLat; upLon_ = cLon;
    double top = 0;
    for (const auto& g : got) for (const auto& L : g.levels) if (L.hasWind) top = std::max(top, L.altFt);
    char b[160];
    std::snprintf(b, sizeof b, "global-model winds aloft: %zu of %zu point(s) on the 1-degree lattice around %.0f,%.0f, up to FL%.0f", got.size(), pts.size(), cLat, cLon, top / 100.0);
    note = b;
    {   // what the global model says at the first point, level by level, so a wrong profile can be traced to its source
        std::string lv;
        for (const auto& L : got[0].levels) if (L.hasWind) { char q[64]; std::snprintf(q, sizeof q, " %.0fft %03.0f/%.0fkt", L.altFt, L.dirDeg, L.speedKt); lv += q; }
        note += "; model profile at the first point:" + lv;
    }
    if (got.size() < pts.size()) note += " [answer starts: " + sample + "]";   // fewer points than asked for: show what came back
    return got;
}

} // namespace wx
