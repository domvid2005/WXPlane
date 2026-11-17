// The worldwide METARs for the map (worldmetar.h), downloaded on a thread of their own every 10 minutes while wanted. Display only:
// nothing here is ever sent to X-Plane.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wxmodel.h"

namespace wx {

class WorldMetars {
public:
    void start();
    void stop();
    // Called from the flight loop: whether the world METARs are wanted (setting on, plugin enabled). Off: no downloads, data dropped.
    void want(bool on);
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
};

} // namespace wx
