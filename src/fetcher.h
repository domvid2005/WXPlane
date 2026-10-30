// Background worker: downloads METARs (NOAA AWC) and ATIS, so the sim thread never blocks on the network.
#pragma once
#include <atomic>
#include <map>
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
    bool immediate = false; // apply without X-Plane's 1-2 minute fade (first load, manual refresh)
};

struct FetchResult {
    bool ok = false;
    std::string error;
    std::vector<Station> stations; // nearest first
    int metarCount = 0;
    int atisCount = 0;
    std::vector<AloftStation> aloft; // FD stations with coordinates, near the aircraft
    std::string aloftNote;           // why winds aloft are missing, if they are
    RadarResult radar;               // storm cells from radar (MRMS mosaic or experimental global tiles)
    double centerLat = 0, centerLon = 0;
    bool immediate = false;
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
    std::vector<AloftStation> fetchAloft(const FetchRequest& r, std::time_t now, std::string& note);
    RadarResult fetchRadar(const FetchRequest& r, std::time_t now);
    RadarResult fetchRainViewer(const FetchRequest& r, std::time_t now);

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
    std::string alRegion_;
    std::time_t alTime_ = 0;
    std::vector<AloftStation> alCache_;
    std::map<std::string, std::pair<double, double>> coords_; // FD id -> lat/lon
    std::map<std::string, std::time_t> coordsMiss_;
};

} // namespace wx
