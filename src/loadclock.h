// The weather window's start-up countdown: how long until a weather build from scratch (start-up, switched back on, a jump to a new
// place) should be all in and faded in. Pure arithmetic on what the plugin tells it every frame, so it can be tested off-line.
#pragma once
#include <algorithm>
#include <string>

namespace wx {

struct LoadPhase {
    bool waitingToStart = false;   // the download for the new weather has not been asked for yet
    double startInS = 0;           // ... and is expected to be asked for in this many seconds
    bool downloading = false;      // the download is running
    double downloadElapsedS = 0;
    double downloadEstS = 25;      // how long downloads take (learned)
    bool recordsQueued = false;    // records still to be handed to X-Plane (or a download that came in, waiting for its airports)
    bool radarDeferred = false;    // the storms follow in a second update
    double radarInS = 0;           // ... in this many seconds (from now)
    bool heldOutlines = false;     // the storm outline records are held for their own update
    double heldInS = 0;
    double sinceLastSendS = 1e9;   // seconds since records were last handed to X-Plane
    double fadeS = 60;             // how long X-Plane takes to blend the last update in
};

class LoadClock {
public:
    void start(const std::string& what) { active_ = true; what_ = what; shown_ = -1; }
    void stop() { active_ = false; }
    bool active() const { return active_; }
    const std::string& what() const { return what_; }
    // Estimated seconds left (never negative). The countdown shown never jumps down: when the estimate drops it runs at most
    // twice as fast as the clock until it meets it; when the estimate grows it jumps up to it.
    double update(const LoadPhase& p, double dt) {
        if (!active_) return -1;
        double est = 0;
        const bool pending = p.waitingToStart || p.downloading || p.recordsQueued || p.radarDeferred || p.heldOutlines;
        if (p.waitingToStart) est += std::max(0.0, p.startInS) + p.downloadEstS + 3;
        else if (p.downloading) est += std::max(3.0, p.downloadEstS - p.downloadElapsedS) + 3;
        else if (p.recordsQueued) est += 3;
        if (p.radarDeferred) est = std::max(est, std::max(0.0, p.radarInS) + p.downloadEstS + 3);
        if (p.heldOutlines) est = std::max(est, std::max(0.0, p.heldInS) + 3);
        est += pending ? p.fadeS : std::max(0.0, p.fadeS - p.sinceLastSendS);
        if (shown_ < 0 || est > shown_) shown_ = est;
        else shown_ = std::max(est, shown_ - 2.0 * std::max(0.0, dt));
        if (!pending && shown_ <= 0.0) { active_ = false; return -1; }
        return shown_;
    }
private:
    bool active_ = false;
    std::string what_;
    double shown_ = -1;
};

} // namespace wx
