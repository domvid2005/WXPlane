#pragma once
// The state of each data service WXPlane downloads from, for the "Data services" list at the bottom of the METAR/ATIS window.
// No X-Plane dependencies.
#include <ctime>
#include <string>

namespace wx {

enum class SvcState { Ok, Degraded, Down, NotHere, Off, Waiting };

struct ServiceStatus {
    std::string name;      // "NOAA Aviation Weather Center"
    std::string use;       // what it gives: "METARs"
    SvcState state = SvcState::Waiting;
    std::string detail;    // "7 reports", "feed down: newest composite 20:10Z (4 h 48 min old)"
    std::time_t lastOk = 0;
};

inline const char* svcLabel(SvcState s) {
    switch (s) {
        case SvcState::Ok: return "OK";
        case SvcState::Degraded: return "DEGRADED";
        case SvcState::Down: return "DOWN";
        case SvcState::NotHere: return "not used here";
        case SvcState::Off: return "off";
        default: return "waiting";
    }
}

} // namespace wx
