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

#include "../src/suntime.h"
namespace tsun {
inline void run() {
    using namespace wx;
    const std::time_t nov12 = utcOf(2026, 11, 12, 15);
    std::time_t r = 0, s = 0;
    CHECK(sunTimes(42.36, -71.01, nov12, r, s) == 1);   // Boston: sunrise 06:32 EST (11:32Z), sunset 16:24 EST (21:24Z)
    NEAR((double)(r - utcOf(2026, 11, 12, 11)) / 60.0, 32, 3);
    NEAR((double)(s - utcOf(2026, 11, 12, 21)) / 60.0, 24, 3);
    CHECK(sunTimes(78.2, 15.6, utcOf(2026, 6, 21, 12), r, s) == 0);    // Svalbard in June: midnight sun
    CHECK(sunTimes(78.2, 15.6, utcOf(2026, 12, 21, 12), r, s) == -1);  // and polar night in December
    CHECK(dayOfWeek(2026, 11, 12) == 4);                                // a Thursday
    CHECK(zoneName(42.36, -71.0, -5 * 3600, nov12) == "EST");
    CHECK(zoneName(42.36, -71.0, -4 * 3600, utcOf(2026, 7, 1, 12)) == "EDT");
    CHECK(zoneName(33.4, -112.0, -7 * 3600, utcOf(2026, 7, 1, 12)) == "MST");   // Arizona, no DST
    CHECK(zoneName(41.98, -87.9, -6 * 3600, nov12) == "CST");
    CHECK(zoneName(51.47, -0.45, 0, nov12) == "GMT");
    CHECK(zoneName(51.47, -0.45, 3600, utcOf(2026, 7, 1, 12)) == "BST");
    CHECK(zoneName(50.03, 8.57, 3600, nov12) == "CET");
    CHECK(zoneName(50.03, 8.57, 7200, utcOf(2026, 7, 1, 12)) == "CEST");
    CHECK(zoneName(35.55, 139.78, 9 * 3600, nov12).empty());           // elsewhere: shown as UTC+9
    // the US change days: 8 March 2026 (2:00 EST = 07:00Z) and 1 November 2026 (2:00 EDT = 06:00Z)
    CHECK(!usDst(utcOf(2026, 3, 8, 6), -5) && usDst(utcOf(2026, 3, 8, 7), -5));
    CHECK(usDst(utcOf(2026, 11, 1, 5), -5) && !usDst(utcOf(2026, 11, 1, 6), -5));
    // daylight time added to a standard offset (quarter hours) by place
    const std::time_t jul1 = utcOf(2026, 7, 1, 12);
    CHECK(dstShiftS(42.36, -71.0, -20, jul1) == 3600 && dstShiftS(42.36, -71.0, -20, nov12) == 0);   // Boston
    CHECK(dstShiftS(33.4, -112.0, -28, jul1) == 0);    // Phoenix
    CHECK(dstShiftS(21.3, -157.9, -40, jul1) == 0);    // Honolulu
    CHECK(dstShiftS(19.4, -99.1, -24, jul1) == 0);     // Mexico City
    CHECK(dstShiftS(25.8, -80.3, -20, jul1) == 3600);  // Miami
    CHECK(dstShiftS(50.03, 8.57, 4, jul1) == 3600 && dstShiftS(55.75, 37.6, 12, jul1) == 0);   // Frankfurt, Moscow
    CHECK(dstShiftS(-33.9, 151.2, 40, utcOf(2026, 1, 10, 0)) == 3600 && dstShiftS(-33.9, 151.2, 40, jul1) == 0);   // Sydney
    CHECK(dstShiftS(-27.4, 153.0, 40, utcOf(2026, 1, 10, 0)) == 0);   // Brisbane
    CHECK(dstShiftS(-37.0, 174.8, 48, utcOf(2026, 1, 10, 0)) == 3600);  // Auckland
    CHECK(zoneName(-33.9, 151.2, 11 * 3600, utcOf(2026, 1, 10, 0)) == "AEDT");
}
} // namespace tsun
