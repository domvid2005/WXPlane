// METAR / D-ATIS text parser. No X-Plane dependencies.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace wx {

struct CloudGroup {
    enum Cover { FEW = 0, SCT = 1, BKN = 2, OVC = 3, VV = 4 };
    int cover = FEW;
    int baseFtAgl = 0;
    int kind = 0; // 0 none, 1 CB, 2 TCU
};

struct Report {
    std::string icao;
    int day = 0, hour = -1, minute = 0; // observation time (UTC); day==0 means "today/most recent"

    bool hasWind = false, windVariable = false;
    bool windMagnetic = false;   // windDir is relative to magnetic north (a spoken/D-ATIS wind); converted to true before use
    int windDir = 0;
    double windKt = 0, gustKt = 0;

    bool hasVis = false, cavok = false;
    double visM = 0;

    bool hasTemp = false, hasDewp = false;
    double tempC = 0, dewpC = 0;

    bool hasAltim = false;
    double altimHpa = 0;

    std::vector<CloudGroup> clouds;
    bool clearSky = false;
    bool skyClearAll = false;   // SKC: an observer saw no cloud at any height (CLR/NCD only cover what an automatic sensor can see, up to 12,000 ft)

    // weather phenomena
    bool fog = false, shallowFog = false, patchyFog = false, mist = false, haze = false;
    bool thunder = false, freezing = false, snow = false;
    double precip = 0; // 0..1 ratio
    std::string wxText;

    std::string infoLetter; // ATIS only
};

// Parses a METAR (rawOb) or a D-ATIS text. Returns true if at least one weather element was recognised.
bool parseReport(const std::string& text, Report& out, bool isAtis = false);

// Age of the report in seconds relative to `now` (0 if the time group is missing).
double reportAgeSeconds(const Report& r, std::time_t now);

constexpr double kSmToM = 1609.344;
constexpr double kFtToM = 0.3048;
constexpr double kKtToMs = 0.514444;
constexpr double kInHgToHpa = 33.8639;

} // namespace wx
