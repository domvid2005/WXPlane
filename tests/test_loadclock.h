// The weather window's start-up countdown (src/loadclock.h). Included by test_wx.cpp.
#pragma once
#include "../src/loadclock.h"

namespace tl {
inline void run() {
    using namespace wx;
    LoadClock c;
    LoadPhase p;
    CHECK(c.update(p, 0.25) < 0);                       // not started: no countdown
    c.start("Loading weather");
    p.waitingToStart = true; p.startInS = 5; p.downloadEstS = 20; p.fadeS = 60;
    const double t0 = c.update(p, 0.25);
    NEAR(t0, 5 + 20 + 3 + 60, 0.01);
    // the download runs: the estimate drops by the elapsed time, the countdown follows it
    p.waitingToStart = false; p.downloading = true; p.downloadElapsedS = 10;
    const double t1 = c.update(p, 0.25);
    CHECK(t1 < t0 && t1 > 0);
    NEAR(t1, std::max(t0 - 0.5, 10 + 3 + 60.0), 0.01);  // never faster than twice the clock
    // the download took longer than thought: never below 3 s for it, and the countdown can go up
    p.downloadElapsedS = 40;
    CHECK(c.update(p, 0.25) >= 3 + 60 - 1e-9);
    // records sent (immediate start-up load): 3 s left after the last send
    p.downloading = false; p.recordsQueued = false; p.sinceLastSendS = 0; p.fadeS = 3;
    double t = c.update(p, 0.25);
    for (int i = 0; i < 400 && t >= 0; ++i) { p.sinceLastSendS += 0.25; t = c.update(p, 0.25); }
    CHECK(t < 0 && !c.active());                        // ends once nothing is pending and the fade is over
    // the storms deferred: the countdown waits for them
    c.start("Loading weather");
    p = LoadPhase{}; p.radarDeferred = true; p.radarInS = 30; p.downloadEstS = 15; p.fadeS = 60; p.sinceLastSendS = 0;
    NEAR(c.update(p, 0.25), 30 + 15 + 3 + 60, 0.01);
    c.stop();
    CHECK(c.update(p, 0.25) < 0);
}
} // namespace tl
