// Parsing of the NOAA Aviation Weather Center (METAR) and D-ATIS JSON responses. No network code here.
#pragma once
#include <string>
#include <vector>

#include "wxmodel.h"

namespace wx {

// aviationweather.gov /api/data/metar?format=json
std::vector<Station> parseAwcMetarJson(const std::string& body, std::string* error = nullptr);

// Station elevations that cannot be right (log 2026-11-05: KC24, Creede CO, came in as 8,680 "m", its 8,680 ft read as metres). Out of the
// physical range (-450..5,600 m), or far from every neighbour while the same number in feet fits them: converted from feet; otherwise
// out-of-range values take the neighbours' median. Returns one log line per corrected station (empty when nothing changed).
std::string checkStationElevations(std::vector<Station>& st);

// datis.clowd.io/api/<ICAO> -> concatenated ATIS text ("" if none). `letter` gets the information letter if present.
bool parseDatisJson(const std::string& body, std::string& text);

std::string awcMetarUrl(double lat, double lon, double searchNm);
std::string datisUrl(const std::string& icao);

} // namespace wx
