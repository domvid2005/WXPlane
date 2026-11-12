#include "fetcher.h"
#include <cctype>
#include <cstring>
#include <sstream>

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

static std::string hhmm(std::time_t t) {
    if (t <= 0) return "--:--Z";
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02dZ", (int)((t / 3600) % 24), (int)((t / 60) % 60));
    return b;
}

bool lookupStation(const std::string& icaoIn, Station& out, std::string& err) {
    std::string icao;
    for (char c : icaoIn) if (std::isalnum((unsigned char)c)) icao.push_back((char)std::toupper((unsigned char)c));
    if (icao.size() < 3 || icao.size() > 4) { err = "enter a 3- or 4-letter ICAO code"; return false; }
    std::string body, e;
    long st = 0;
    if (!httpGet("https://aviationweather.gov/api/data/metar?ids=" + icao + "&format=json", body, st, e, 12)) { err = "NOAA METAR: " + e; return false; }
    if (st == 204 || body.empty() || body == "[]") { err = "no recent METAR for " + icao; return false; }
    std::vector<Station> v = parseAwcMetarJson(body, &e);
    if (v.empty()) { err = e.empty() ? "no recent METAR for " + icao : e; return false; }
    out = v[0];
    for (const auto& s : v) if (s.obsTime > out.obsTime) out = s;   // the newest report
    out.distNm = -1;
    std::string ab, ae, text;
    long ast = 0;
    if (httpGet(datisUrl(out.icao), ab, ast, ae, 8) && parseDatisJson(ab, text)) {
        Report ar;
        if (parseReport(text, ar, true)) mergeAtis(out, ar, text);
    }
    return true;
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
        if (cfg.radar) res.radar = fetchRadar(rq, res.fetchedAt, radarStations_.empty() ? nullptr : &radarStations_);
        res.ok = res.radar.ok;
        if (!res.ok) res.error = res.radar.note.empty() ? std::string("radar refresh failed") : res.radar.note;
        copySvc(res);
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
            setSvc(kSvcMetar, SvcState::Down, err, now);
            copySvc(res);
            return res;
        }
        std::string perr;
        std::vector<Station> all = parseAwcMetarJson(body, &perr);
        if (!perr.empty() && !body.empty()) { res.error = perr; setSvc(kSvcMetar, SvcState::Down, "unreadable answer: " + perr, now); copySvc(res); return res; }
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
    setSvc(kSvcMetar, SvcState::Ok, keep.empty() ? std::string("answering; no reports within 600 nm") : std::to_string(keep.size()) + " report(s)", now);
    res.elevNote = checkStationElevations(keep);

    // 2) ATIS for the closest airports (D-ATIS covers major US airports; others simply return nothing).
    if (!cfg.useAtis) setSvc(kSvcAtis, SvcState::Off, "\"Use ATIS\" is off", now);
    if (cfg.useAtis) {
        int tried = 0, netFail = 0;
        std::string netErr;
        for (auto& s : keep) {
            if (tried >= cfg.atisMaxAirports || s.distNm > cfg.atisRangeNm) break;
            if (s.icao.size() != 4) continue;
            auto neg = std::find_if(noAtis_.begin(), noAtis_.end(), [&](const std::pair<std::string, std::time_t>& p) { return p.first == s.icao; });
            if (neg != noAtis_.end() && now - neg->second < 3600) continue;
            ++tried;
            std::string abody, aerr;
            long ast = 0;
            std::string text;
            const bool answered = httpGet(datisUrl(s.icao), abody, ast, aerr, 8);
            if (!answered && ast == 0) { ++netFail; netErr = aerr; }
            bool got = answered && parseDatisJson(abody, text);
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
        if (res.atisCount > 0) setSvc(kSvcAtis, SvcState::Ok, std::to_string(res.atisCount) + " airport(s)", now);
        else if (netFail > 0 && netFail == tried) setSvc(kSvcAtis, SvcState::Down, netErr, now);
        else setSvc(kSvcAtis, SvcState::NotHere, "no digital ATIS for the airports nearby (D-ATIS covers major US airports)", now);
    }

    // 3) Winds & temperatures aloft from the global model (Open-Meteo GFS), so the cruise levels follow real data too.
    // NOAA's FD wind/temperature tables (fetchAloft) were discontinued with the NAM model: AWC answers HTTP 400 ("Wind/temp tables have
    // been discontinued", SCN 26-47, seen 2026-11-09). The global model is now the source at every level.
    if (!cfg.useAloft) { setSvc(kSvcWinds, SvcState::Off, "the global-model option is off", now); setSvc(kSvcClouds, SvcState::Off, "the global-model option is off", now); }
    if (cfg.useAloft) {   // the global model at every level
        std::string upNote;
        std::vector<AloftStation> up = fetchUpperAir(rq, now, upNote, res.stations);
        if (up.empty()) setSvc(kSvcWinds, SvcState::Down, upNote.empty() ? std::string("no data") : upNote, now);
        else if (upNote.find("using the earlier download") != std::string::npos) setSvc(kSvcWinds, SvcState::Degraded, upNote, now), svc_[kSvcWinds].lastOk = upTime_;
        else { setSvc(kSvcWinds, SvcState::Ok, std::to_string(up.size()) + " model point(s), downloaded " + hhmm(upTime_), now); svc_[kSvcWinds].lastOk = upTime_; }
        for (auto& u : up) res.aloft.push_back(std::move(u));
        if (!upNote.empty()) res.aloftNote += (res.aloftNote.empty() ? "" : "; ") + upNote;
    }

    // 3b) Global-model clouds and surface weather on the same lattice (every 30 min): cloud layers above what airport reports can see,
    //     and the weather over open water where no airport reports within the search radius.
    if (cfg.useAloft && (cfg.modelClouds || cfg.oceanModelWeather)) {
        std::string cNote;
        std::vector<ModelPoint> mp = fetchModelClouds(rq, now, cNote);
        if (mp.empty()) setSvc(kSvcClouds, SvcState::Down, cNote.empty() ? std::string("no data") : cNote, now);
        else if (cNote.find("using the earlier download") != std::string::npos) { setSvc(kSvcClouds, SvcState::Degraded, cNote, now); svc_[kSvcClouds].lastOk = cloudTime_; }
        else { setSvc(kSvcClouds, SvcState::Ok, std::to_string(mp.size()) + " model point(s), downloaded " + hhmm(cloudTime_), now); svc_[kSvcClouds].lastOk = cloudTime_; }
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
    radarStations_ = keep;   // radar-only refreshes check convection against these
    if (cfg.radar) res.radar = fetchRadar(rq, now, &keep);
    else { setSvc(kSvcMrms, SvcState::Off, "radar is off", now); setSvc(kSvcOpera, SvcState::Off, "radar is off", now); }

    res.noaaFixes = 0;
    for (const auto& k : keep) if (!k.filled.empty()) ++res.noaaFixes;
    res.filledElements = fillMissing(keep, 80.0);   // elements neither the text nor NOAA's decoded fields had: from neighbouring stations
    res.filledElements += spreadHighCloud(keep, 80.0);   // high cloud the automatic stations cannot see
    res.stations = std::move(keep);
    res.ok = true;
    if (res.stations.empty()) res.error = "no METARs within 600 nm: the weather already set is kept";
    else if (!widened.empty()) res.error = widened;
    copySvc(res);
    return res;
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

RadarResult Fetcher::fetchRadar(const FetchRequest& rq, std::time_t now, const std::vector<Station>* stations) {
    const Config& cfg = rq.cfg;
    RadarResult out;
    out.attempted = true;
    GribWindow win = windowFor(rq.lat, rq.lon, cfg.radarRangeNm);
    std::string lastErr;
    for (const std::string& region : mrmsRegionsFor(rq.lat, rq.lon)) {
        const std::string name = region.empty() ? "CONUS" : region;
        GribField refl;
        if (!loadMrms(mrmsUrl(region, "MergedReflectivityQCComposite"), win, refl, lastErr)) {
            refl = GribField();   // one retry: a truncated download ("bad gzip data", 2026-11-06) made the first load go without storms
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
        // steady rain and snow (MRMS PrecipFlag): capped and marked, so the bright band never makes a cumulonimbus
        GribField conv;
        std::string typeNote;
        if (cfg.radarPrecipType) {
            GribField flag;
            std::string flagErr;
            bool haveFlag = loadMrms(mrmsUrl(region, "PrecipFlag"), win, flag, flagErr);
            if (!haveFlag) { flag = GribField(); haveFlag = loadMrms(mrmsUrl(region, "PrecipFlag"), win, flag, flagErr); }
            if (haveFlag && flag.refTime > 0 && refl.refTime > 0 && std::llabs((long long)(flag.refTime - refl.refTime)) > 20 * 60) { haveFlag = false; flagErr = "older than the reflectivity"; }
            if (haveFlag || stations) {
                int capped = 0;
                conv = convectiveMask(refl, haveFlag ? &flag : nullptr, stations, (float)cfg.radarStratiformMaxDbz, capped);
                char b[200];
                if (haveFlag) std::snprintf(b, sizeof b, "precipitation type from MRMS PrecipFlag: %d pixel(s) of steady rain/snow capped at %.0f dBZ", capped, cfg.radarStratiformMaxDbz);
                else std::snprintf(b, sizeof b, "MRMS PrecipFlag unavailable (%s): convection only within 60 nm of an airport reporting it; %d pixel(s) capped at %.0f dBZ", flagErr.c_str(), capped, cfg.radarStratiformMaxDbz);
                typeNote = b;
            }
        }
        out = buildRadarCells(refl, haveTops ? &tops : nullptr, rq.lat, rq.lon, cfg, &radarPrev_, conv.valid() ? &conv : nullptr);
        radarPrev_.clear();
        for (const auto& c : out.cells) radarPrev_.insert(c.key);
        out.source = "NOAA MRMS (" + name + ")";
        out.dataTime = refl.refTime;
        if (oldTops) out.note = "echo-top download failed (" + topErr + "), reusing the last good tops" + " from " + std::to_string((now - lastTopsAt_) / 60) + " min ago";
        else if (cfg.radarEchoTops && !haveTops) out.note = "echo tops unavailable (" + topErr + "), storm tops estimated";
        if (!typeNote.empty()) out.note += (out.note.empty() ? "" : "; ") + typeNote;
        if (out.ok) { lastMrms_ = out; lastMrmsAt_ = now; lastMrmsLat_ = rq.lat; lastMrmsLon_ = rq.lon; }
        setSvc(kSvcMrms, SvcState::Ok, name + ", valid " + hhmm(refl.refTime) + (typeNote.find("unavailable") != std::string::npos ? "; precipitation type unavailable" : ""), now);
        setSvc(kSvcOpera, SvcState::NotHere, "outside Europe", now);
        return out;
    }
    // Inside NOAA coverage a failed download used to fall straight through to RainViewer (log 2026-11-04, Ohio; RainViewer was removed
    // 2026-11-10): its colour-decoded tiles read the same storms as 153 cumulonimbus cells at 40+ dBZ where MRMS had 7 a few minutes earlier, in blocky colour bands. The last MRMS
    // picture is kept instead while it is under 20 minutes old and the aircraft is within 30 nm of where it was built.
    const bool mrmsArea = !mrmsRegionsFor(rq.lat, rq.lon).empty();
    if (mrmsArea && lastMrms_.ok && now - lastMrmsAt_ < 20 * 60 && distNm(rq.lat, rq.lon, lastMrmsLat_, lastMrmsLon_) < 30.0) {
        RadarResult r = lastMrms_;
        r.note = "MRMS download failed (" + lastErr + "), reusing the radar picture from " + std::to_string((now - lastMrmsAt_) / 60) + " min ago";
        setSvc(kSvcMrms, SvcState::Degraded, r.note, now);
        svc_[kSvcMrms].lastOk = lastMrmsAt_;
        return r;
    }
    if (mrmsArea) { setSvc(kSvcMrms, SvcState::Down, lastErr, now); setSvc(kSvcOpera, SvcState::NotHere, "outside Europe", now); }
    else setSvc(kSvcMrms, SvcState::NotHere, "outside the US", now);
    // Europe: the EUMETNET OPERA composite
    if (!mrmsArea && operaCovers(rq.lat, rq.lon)) {
        RadarResult eu;
        std::string euErr;
        if (fetchOpera(rq, now, stations, eu, euErr)) { setSvc(kSvcOpera, SvcState::Ok, "composite " + hhmm(eu.dataTime), now); return eu; }
        if (lastOpera_.ok && now - lastOperaAt_ < 20 * 60 && distNm(rq.lat, rq.lon, lastOperaLat_, lastOperaLon_) < 30.0) {
            RadarResult r = lastOpera_;
            r.note = "OPERA download failed (" + euErr + "), reusing the radar picture from " + std::to_string((now - lastOperaAt_) / 60) + " min ago";
            setSvc(kSvcOpera, SvcState::Degraded, r.note, now);
            svc_[kSvcOpera].lastOk = lastOperaAt_;
            return r;
        }
        out.ok = false;
        out.note = "European radar (OPERA): " + euErr;
        setSvc(kSvcOpera, SvcState::Down, euErr, now);
        return out;
    }
    setSvc(kSvcOpera, SvcState::NotHere, "outside Europe", now);
    out.ok = false;
    out.note = lastErr.empty() ? std::string("no radar composite covers this area (radar covers the US and Europe)") : lastErr;
    return out;
}

// One OPERA composite: the newest 5-minute slot that is published, then older ones up to 20 minutes back. The header (the first
// 5.5 kB) and only the tiles under the 150 nm window are downloaded; tiles are kept for as long as the slot is current.
bool Fetcher::fetchOpera(const FetchRequest& rq, std::time_t now, const std::vector<Station>* stations, RadarResult& out, std::string& err) {
    const Config& cfg = rq.cfg;
    const GribWindow win = windowFor(rq.lat, rq.lon, cfg.radarRangeNm);
    const std::time_t newest = operaNewestSlot(now);
    for (int k = 0; k < 4; ++k) {
        const std::time_t slot = newest - k * 300;
        const std::string url = operaUrl(slot);
        if (slot != operaSlot_) {
            std::string head, e;
            long st = 0;
            OperaHeader h;
            size_t need = 0;
            bool ok = httpGetRange(url, "0-8191", head, st, e, 15, 1u << 20) && (st == 206 || st == 200);
            if (ok && !operaParseHeader(head, h, e, need) && need > head.size() && need < (4u << 20)) {
                head.clear();
                ok = httpGetRange(url, "0-" + std::to_string(need - 1), head, st, e, 20, 8u << 20) && (st == 206 || st == 200) && operaParseHeader(head, h, e, need);
            } else if (ok) {
                ok = operaParseHeader(head, h, e, need);
            }
            if (!ok) {
                err = e.empty() ? ("HTTP " + std::to_string(st)) : e;
                if (st == 404 || st == 403) {
                    char tb[80];
                    std::snprintf(tb, sizeof tb, "no composite for %02d:%02dZ (HTTP %ld)", (int)((slot / 3600) % 24), (int)((slot / 60) % 60), st);
                    err = tb;
                }
                continue;
            }
            operaSlot_ = slot; operaHead_ = h; operaTiles_.clear();
        }
        int fetched = 0;
        GribField refl;
        std::string e;
        auto get = [&](uint64_t off, uint64_t len, std::string& data, std::string& ge) {
            long st = 0;
            const bool ok = httpGetRange(url, std::to_string(off) + "-" + std::to_string(off + len - 1), data, st, ge, 20, (size_t)len + 1024);
            if (ok && st != 206 && !(st == 200 && data.size() == len)) { ge = "HTTP " + std::to_string(st); return false; }
            return ok && data.size() == len;
        };
        if (!operaWindow(operaHead_, win, 0.01, get, operaTiles_, refl, fetched, e)) {
            err = e;
            operaSlot_ = 0; operaTiles_.clear();
            return false;
        }
        refl.refTime = slot;
        GribField conv;
        std::string typeNote;
        if (cfg.radarPrecipType && stations) {
            int capped = 0;
            conv = convectiveMask(refl, nullptr, stations, (float)cfg.radarStratiformMaxDbz, capped);
            char b[200];
            std::snprintf(b, sizeof b, "convection only within 60 nm of an airport reporting it; %d pixel(s) capped at %.0f dBZ", capped, cfg.radarStratiformMaxDbz);
            typeNote = b;
        }
        out = buildRadarCells(refl, nullptr, rq.lat, rq.lon, cfg, &radarPrev_, conv.valid() ? &conv : nullptr);
        radarPrev_.clear();
        for (const auto& c : out.cells) radarPrev_.insert(c.key);
        out.source = "EUMETNET OPERA (Europe)";
        out.dataTime = slot;
        char b[120];
        std::snprintf(b, sizeof b, "OPERA: %d tile(s) downloaded, %zu cached", fetched, operaTiles_.size());
        out.note = b;
        if (!typeNote.empty()) out.note += "; " + typeNote;
        if (out.ok) { lastOpera_ = out; lastOperaAt_ = now; lastOperaLat_ = rq.lat; lastOperaLon_ = rq.lon; }
        return true;
    }
    if (err.empty()) err = "no recent composite";
    if (err.find("no composite for") != std::string::npos) {   // nothing published for 20 minutes: say how long the feed has been down
        const std::time_t newest = operaNewestPublished(now);
        char b[160];
        if (newest > 0) {
            const long m = (long)((now - newest) / 60);
            std::snprintf(b, sizeof b, "feed down: newest composite is %s (%ld h %02ld min old)", hhmm(newest).c_str(), m / 60, m % 60);
        } else {
            std::snprintf(b, sizeof b, "feed down: no composite published in the last 12 h");
        }
        err = b;
    }
    return false;
}

// When the newest composites are missing, the bucket listing tells how long the feed has been stalled. Hour by hour backwards (up to
// 12 small listings), at most every 10 minutes.
std::time_t Fetcher::operaNewestPublished(std::time_t now) {
    if (operaNewestCheckedAt_ && now - operaNewestCheckedAt_ < 600) return operaNewestKnown_;
    operaNewestCheckedAt_ = now;
    operaNewestKnown_ = 0;
    for (int h = 0; h < 12; ++h) {
        std::string body, e;
        long st = 0;
        if (!httpGet(operaListUrl(now - h * 3600), body, st, e, 10, 1u << 20) || st != 200) { operaNewestCheckedAt_ = 0; break; }
        const std::time_t t = operaNewestInListing(body);
        if (t > 0) { operaNewestKnown_ = t; break; }
    }
    return operaNewestKnown_;
}

// The 3x3 GFS lattice shared by the winds and the clouds. Nodes sit on whole degrees of latitude and on multiples of `step` degrees
// of longitude (1 up to about 48 degrees latitude, 2 by 60, 3 by 70...), so the 9 points span roughly 120 x 120 nm anywhere.
// A fast aircraft (over 150 kt) gets the lattice centred about 7 minutes ahead along its track, so it stays inside the 9 points
// for at least 15 minutes. The lattice is moved only when the aircraft's (projected) node changes AND the lattice is at least 15
// minutes old, or at once when the aircraft is more than 1.6 node spacings from the centre (a reposition). That caps a jet at
// 4 downloads an hour of each kind: about 320 Open-Meteo calls an hour instead of about 970 (free allowance, 2026-11-09).
static double lonStepFor(double lat) {
    const double c = std::cos(std::fabs(lat) * 3.14159265358979323846 / 180.0);
    double st = c > 0.01 ? std::round(1.0 / c) : 6.0;
    return st < 1 ? 1 : st > 6 ? 6 : st;
}
static double wrapLon(double lo) { while (lo > 180) lo -= 360; while (lo < -180) lo += 360; return lo; }

void Fetcher::chooseLattice(const FetchRequest& rq, std::time_t now) {
    const double d2r = 3.14159265358979323846 / 180.0;
    double aLat = rq.lat, aLon = rq.lon;
    if (rq.gsKt > 150) {
        const double d = std::min(rq.gsKt * 0.125, 62.0);   // nm, 7.5 minutes ahead
        aLat += d * std::cos(rq.trackDeg * d2r) / 60.0;
        const double c = std::max(0.05, std::cos(rq.lat * d2r));
        aLon = wrapLon(aLon + d * std::sin(rq.trackDeg * d2r) / (60.0 * c));
        aLat = std::max(-88.0, std::min(88.0, aLat));
    }
    const double nLat = std::round(aLat), step = lonStepFor(nLat);
    const double nLon = wrapLon(std::round(aLon / step) * step);
    if (lattHave_) {
        double dLon = std::fabs(rq.lon - lattLon_);
        if (dLon > 180) dLon = 360 - dLon;
        const bool inside = std::fabs(rq.lat - lattLat_) <= 1.6 && dLon <= 1.6 * lattStep_;
        const bool sameNode = std::fabs(nLat - lattLat_) < 0.5 && std::fabs(wrapLon(nLon - lattLon_)) < 0.5;
        if (inside && (sameNode || now - lattTime_ < 900)) return;
    }
    lattHave_ = true; lattLat_ = nLat; lattLon_ = nLon; lattStep_ = step; lattTime_ = now;
}

std::vector<std::pair<double, double>> Fetcher::latticePoints() const {
    std::vector<std::pair<double, double>> pts;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const double la = lattLat_ + dy;
            if (la > 89 || la < -89) continue;
            pts.push_back({la, wrapLon(lattLon_ + dx * lattStep_)});
        }
    return pts;
}

std::vector<ModelPoint> Fetcher::fetchModelClouds(const FetchRequest& rq, std::time_t now, std::string& note) {
    // The same 3x3 lattice as the winds (chosen there), every 30 minutes (cloud cover changes slowly; it keeps the open-meteo.com cost low).
    if (!lattHave_) chooseLattice(rq, now);
    const double cLat = lattLat_, cLon = lattLon_;
    if (!cloudCache_.empty() && now - cloudTime_ < 1800 && cloudLat_ == cLat && cloudLon_ == cLon) return cloudCache_;
    const std::vector<std::pair<double, double>> pts = latticePoints();
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
    chooseLattice(rq, now);
    const double cLat = lattLat_, cLon = lattLon_;
    if (!upCache_.empty() && now - upTime_ < 2700 && upLat_ == cLat && upLon_ == cLon) return upCache_;
    const std::vector<std::pair<double, double>> pts = latticePoints();
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
    std::snprintf(b, sizeof b, "global-model winds aloft: %zu of %zu point(s) on the lattice around %.0f,%.0f (%.0f deg apart E-W), up to FL%.0f", got.size(), pts.size(), cLat, cLon, lattStep_, top / 100.0);
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
