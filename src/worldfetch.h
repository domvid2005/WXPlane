// The worldwide METARs for the map (worldmetar.h), downloaded on a thread of their own every 10 minutes while wanted. Display only:
// nothing here is ever sent to X-Plane.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hazards.h"
#include "opera.h"
#include "radar.h"
#include "windsaloft.h"
#include "wxmodel.h"

namespace wx {

class WorldMetars {
public:
    void start();
    void stop();
    // Called from the flight loop: whether the world METARs are wanted (setting on, plugin enabled). Off: no downloads, data dropped.
    // xpMetarDir not empty: after each download they are also written there as X-Plane's METAR file (worldmetar.h, xp_metar_file).
    void want(bool on, const std::string& xpMetarDir = "");
    std::shared_ptr<const std::vector<Station>> stations() const;
    std::vector<std::string> takeLog();   // lines for Log.txt, from the worker

private:
    void run();
    void log(const std::string& s);
    std::thread th_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    bool want_ = false;
    std::chrono::steady_clock::time_point next_{};   // the next download (under m_)
    std::shared_ptr<const std::vector<Station>> data_;
    std::vector<std::string> log_;
    std::string xpDir_;        // under m_
    std::string xpWritten_;    // worker thread only: the file name written last
};

// The map beyond the weather loaded into X-Plane (display only, nothing here is sent to X-Plane): for the part of the map being looked
// at, global-model winds aloft and cloud cover (Open-Meteo GFS) on a coarse grid that grows with the zoom, and the pilot reports there.
struct FarData {
    std::vector<AloftStation> aloft;   // model = true
    struct Cloud { double lat = 0, lon = 0, low = 0, mid = 0, high = 0; };   // cover 0..1: low (below ~6,500 ft), mid, high (above ~20,000 ft)
    std::vector<Cloud> clouds;
    std::vector<hz::Pirep> pireps;
    double gridDeg = 0;
    std::shared_ptr<const RadarMosaic> radar;   // Europe (EUMETNET OPERA) over the view, about 2 nm
    std::string radarSource;
};

class FarField {
public:
    void start();
    void stop();
    // From the flight loop: the map's centre and range, and where the loaded weather is (its centre; within nearNm nothing is fetched).
    // range <= 0: the map is not shown, nothing is fetched.
    void view(double lat, double lon, double rangeNm, double nearLat, double nearLon, double nearNm);
    std::shared_ptr<const FarData> data() const;
    std::vector<std::string> takeLog();

private:
    struct V { double lat = 0, lon = 0, range = 0, nLat = 0, nLon = 0, nNm = 0; };
    void run();
    void log(const std::string& s);
    void step(const V& v);
    std::thread th_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    V want_, last_;
    double changedAt_ = 0;
    bool fresh_ = false;
    std::shared_ptr<const FarData> data_;
    std::vector<std::string> log_;
    // worker thread only: what was fetched, by grid node ("step:lat:lon"), and when
    struct Node { AloftStation wind; FarData::Cloud cloud; bool hasWind = false, hasCloud = false; double at = 0; };
    std::map<std::string, Node> nodes_;
    std::vector<hz::Pirep> pireps_;
    double pLat0_ = 0, pLat1_ = 0, pLon0_ = 0, pLon1_ = 0, pAt_ = -1e9;
    double failUntil_ = 0;
    // Europe's radar for the view (OPERA): its composite's header and tiles, kept while the slot is current
    void radarStep(const V& v, double la0, double la1, double lo0, double lo1);
    std::shared_ptr<const RadarMosaic> radar_;
    std::string radarSrc_;
    std::time_t opSlot_ = 0;
    int opStore_ = 0;
    OperaHeader opHead_;
    std::map<int, std::vector<float>> opTiles_;
    double rLat0_ = 0, rLat1_ = 0, rLon0_ = 0, rLon1_ = 0, rAt_ = -1e9, rFailUntil_ = 0;
};

} // namespace wx
