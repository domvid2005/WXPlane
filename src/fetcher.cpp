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
    const Config& cfg = rq.cfg;

    // 1) METARs from NOAA's Aviation Weather Center, centred on the aircraft (not on any airport),
    //    so the weather below you stays current at cruise altitude too.
    std::string body, err;
    long status = 0;
    if (!httpGet(awcMetarUrl(rq.lat, rq.lon, cfg.searchNm), body, status, err)) {
        res.error = "NOAA METAR: " + err;
        return res;
    }
    std::string perr;
    std::vector<Station> all = parseAwcMetarJson(body, &perr);
    if (!perr.empty()) { res.error = perr; return res; }

    std::time_t now = res.fetchedAt;
    std::vector<Station> keep;
    for (auto& s : all) {
        s.distNm = distNm(rq.lat, rq.lon, s.lat, s.lon);
        if (s.distNm > cfg.searchNm * 1.05) continue;
        if (reportAgeSeconds(s.report, now) > cfg.maxAgeHours * 3600.0) continue;
        keep.push_back(std::move(s));
    }
    std::sort(keep.begin(), keep.end(), [](const Station& a, const Station& b) { return a.distNm < b.distNm; });
    if ((int)keep.size() > cfg.maxStations) keep.resize((size_t)cfg.maxStations);
    res.metarCount = (int)keep.size();

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
    if (cfg.useAloft) res.aloft = fetchAloft(rq, now, res.aloftNote);

    // 4) Radar storm cells.
    if (cfg.radar) res.radar = fetchRadar(rq, now);

    res.noaaFixes = 0;
    for (const auto& k : keep) if (!k.filled.empty()) ++res.noaaFixes;
    res.filledElements = fillMissing(keep, 80.0);   // elements neither the text nor NOAA's decoded fields had: from neighbouring stations
    res.stations = std::move(keep);
    res.ok = true;
    if (res.stations.empty()) res.error = "no METARs within search radius";
    return res;
}

std::vector<AloftStation> Fetcher::fetchAloft(const FetchRequest& rq, std::time_t now, std::string& note) {
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
            mergeAloft(all, parseWindTemp(body));
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

    // FD stations come without coordinates; look them up once through their METAR ("ABI" -> "KABI").
    std::vector<std::string> need;
    for (const auto& s : alCache_) {
        if (coords_.count(s.id)) continue;
        auto miss = coordsMiss_.find(s.id);
        if (miss != coordsMiss_.end() && now - miss->second < 6 * 3600) continue;
        need.push_back(s.id);
    }
    for (size_t i = 0; i < need.size(); i += 90) {
        std::string ids;
        size_t end = std::min(need.size(), i + 90);
        for (size_t j = i; j < end; ++j) ids += (j > i ? "," : "") + std::string(1, prefix) + need[j];
        std::string body, err;
        long st = 0;
        if (!httpGet("https://aviationweather.gov/api/data/metar?format=json&ids=" + ids, body, st, err, 15)) break;
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
        if (distNm(rq.lat, rq.lon, s.lat, s.lon) <= cfg.aloftRangeNm + cfg.searchNm + 60) out.push_back(std::move(s));
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
        if (!loadMrms(mrmsUrl(region, "MergedReflectivityQCComposite"), win, refl, lastErr)) { lastErr = name + ": " + lastErr; continue; }
        if (refl.refTime > 0 && now - refl.refTime > 30 * 60) {
            lastErr = name + ": radar mosaic is " + std::to_string((now - refl.refTime) / 60) + " min old, ignoring";
            continue;
        }
        GribField tops;
        std::string topErr;
        bool haveTops = false;
        if (cfg.radarEchoTops) haveTops = loadMrms(mrmsUrl(region, "EchoTop_18"), win, tops, topErr);
        out = buildRadarCells(refl, haveTops ? &tops : nullptr, rq.lat, rq.lon, cfg, &radarPrev_);
        radarPrev_.clear();
        for (const auto& c : out.cells) radarPrev_.insert(c.key);
        out.source = "NOAA MRMS (" + name + ")";
        out.dataTime = refl.refTime;
        if (cfg.radarEchoTops && !haveTops) out.note = "echo tops unavailable (" + topErr + "), storm tops estimated";
        return out;
    }
    if (cfg.radarGlobal) {
        RadarResult g = fetchRainViewer(rq, now);
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

} // namespace wx
