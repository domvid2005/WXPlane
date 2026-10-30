#include "fetcher.h"

#include <algorithm>

#include "geo.h"
#include "http.h"
#include "metar.h"
#include "sources.h"

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

void Fetcher::run() {
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

    res.stations = std::move(keep);
    res.ok = true;
    if (res.stations.empty()) res.error = "no METARs within search radius";
    return res;
}

} // namespace wx
