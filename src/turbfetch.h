// Worker thread that keeps the turbulence data current (NOAA GTGN in the US, ECMWF Open Data elsewhere).
// No network access ever happens on the sim's thread: the flight loop only reads the published cubes.
#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hazards.h"
#include "status.h"
#include "turbulence.h"

namespace wx {
namespace turb {

class Fetcher {
public:
    void start();
    void stop();
    // From the flight loop: where the aircraft is, and whether realistic turbulence is on.
    // `reports`: also keep pilot reports, SIGMETs and AIRMETs current (for the map and the turbulence floor).
    void update(double lat, double lon, double altFt, bool enabled, bool reports = false);
    Inputs inputs() const;
    std::shared_ptr<const hz::Snapshot> reports() const;
    std::vector<std::string> takeLog();
    // Pilot reports, NOAA GTGN and ECMWF Open Data, for the window's "Data services" list (any thread).
    std::vector<ServiceStatus> services(std::time_t now) const;

private:
    void run();
    void gtgnStep(double lat, double lon, double alt, std::time_t now);
    void ecStep(double lat, double lon, double alt, std::time_t now);
    void reportsStep(double lat, double lon, std::time_t now);
    bool get(const std::string& url, const std::string& range, std::string& body, long& status, std::string& err, long timeout, size_t maxBytes);
    void log(const std::string& s);

    std::thread th_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mx_;
    std::condition_variable cv_;
    double lat_ = 0, lon_ = 0, alt_ = 0;
    bool enabled_ = false, havePos_ = false, reports_ = false;
    Inputs pub_;
    std::shared_ptr<const hz::Snapshot> hz_;
    std::time_t hzNext_ = 0; double hzLat_ = 999, hzLon_ = 999; int hzFail_ = 0;
    std::vector<std::string> log_;
    std::string gProbPub_, eProbPub_, hzProbPub_;   // the workers' last problems, copied under mx_ for services()

    // GTGN state (worker only)
    struct GMsg { size_t off = 0, len = 0; double altFt = 0; };
    std::string gFile_;                       // URL of the file the layout and cube belong to
    std::time_t gValid_ = 0;
    std::vector<GMsg> gLayout_;
    std::map<size_t, std::string> gRaw_;      // raw messages of gFile_, by offset
    std::shared_ptr<const GtgnCube> gCube_;
    std::time_t gNextCheck_ = 0, gNextTry_ = 0, gLastListing_ = 0;
    std::map<std::string, std::time_t> gMissing_;
    int gFail_ = 0;
    std::string gLastProblem_;

    // ECMWF state (worker only)
    struct EStep {
        std::string key;                      // run + step
        std::time_t valid = 0;
        std::map<std::string, std::string> raw;   // "u700" -> GRIB message
        std::shared_ptr<const EcCube> cube;
    };
    std::time_t eRun_ = 0, eNextRunCheck_ = 0, eNextTry_ = 0;
    std::map<std::string, EStep> eSteps_;
    int eFail_ = 0;
    std::string eLastProblem_;
};

} // namespace turb
} // namespace wx
