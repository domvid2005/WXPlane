// Background worker: downloads METARs (NOAA AWC) and ATIS, so the sim thread never blocks on the network.
#pragma once
#include <atomic>
#include <map>
#include <set>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "radar.h"
#include "wxmodel.h"

namespace wx {

struct FetchRequest {
    double lat = 0, lon = 0;
    Config cfg;
    bool radarOnly = false; // refresh only the storm radar (stations, ATIS and winds aloft are kept)
    bool immediate = false; // apply without X-Plane's 1-2 minute fade (first load, manual refresh)
    double gsKt = 0, trackDeg = 0; // ground speed and true track: the GFS lattice is centred ahead of a fast aircraft
};

struct FetchResult {
    bool ok = false;
    std::string error;
    std::vector<Station> stations; // nearest first
    int metarCount = 0;
    int atisCount = 0;
    int noaaFixes = 0;       // reports where NOAA's decoded fields corrected or completed the text parse
    int filledElements = 0;  // elements filled from neighbouring stations
    std::vector<AloftStation> aloft; // FD stations with coordinates, near the aircraft
    std::string aloftNote;           // why winds aloft are missing, if they are
    std::string elevNote;            // station elevations that were corrected (checkStationElevations)
    RadarResult radar;               // storm cells from radar (MRMS mosaic or experimental global tiles)
    double centerLat = 0, centerLon = 0;
    bool immediate = false;
    bool radarOnly = false;
    std::time_t fetchedAt = 0;
};

class Fetcher {
public:
    void start();
    void stop();
    bool busy() const { return working_.load(); }
    void request(const FetchRequest& r);
    bool take(FetchResult& out); // true if a finished result was waiting

private:
    void run();
    FetchResult fetch(const FetchRequest& r);
    // stations (optional): the reports, for the convection check when MRMS PrecipFlag cannot be downloaded
    RadarResult fetchRadar(const FetchRequest& r, std::time_t now, const std::vector<Station>* stations = nullptr);

    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool running_ = false;
    bool hasReq_ = false;
    bool hasRes_ = false;
    FetchRequest req_;
    FetchResult res_;
    std::atomic<bool> working_{false};
    std::vector<std::pair<std::string, std::time_t>> noAtis_; // negative cache, worker thread only
    // winds-aloft cache (worker thread only)
    // global-model winds aloft (Open-Meteo GFS), worker thread only
    std::vector<AloftStation> upCache_;
    std::vector<ModelPoint> cloudCache_;
    std::time_t cloudTime_ = 0;
    double cloudLat_ = 999, cloudLon_ = 999;
    std::vector<ModelPoint> fetchModelClouds(const FetchRequest& r, std::time_t now, std::string& note);
    std::time_t upTime_ = 0;
    double upLat_ = 0, upLon_ = 0;
    // the shared 3x3 GFS lattice (winds and clouds use the same 9 nodes)
    bool lattHave_ = false;
    double lattLat_ = 0, lattLon_ = 0, lattStep_ = 1;
    std::time_t lattTime_ = 0;
    void chooseLattice(const FetchRequest& r, std::time_t now);
    std::vector<std::pair<double, double>> latticePoints() const;
    std::vector<AloftStation> fetchUpperAir(const FetchRequest& r, std::time_t now, std::string& note, const std::vector<Station>& known);
    RadarResult lastMrms_;            // last MRMS result that worked, reused for a while when a download fails (worker thread only)
    std::time_t lastMrmsAt_ = 0;
    double lastMrmsLat_ = 0, lastMrmsLon_ = 0;
    GribField lastTops_;              // last echo-top grid that downloaded fine, reused when a later download fails (worker thread only)
    std::string lastTopsRegion_;
    std::time_t lastTopsAt_ = 0;      // when it was downloaded
    std::vector<Station> radarStations_; // the last full update's reports (worker thread only)
    std::set<std::string> radarPrev_; // storm blocks applied last time (hysteresis), worker thread only
};

} // namespace wx
