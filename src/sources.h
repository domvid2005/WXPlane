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

// The METAR request asks for the last ~70 minutes; this keeps the latest report per station and fills its recent-precipitation fields
// from the older ones and from begin/end remarks (RAB10E35, TSE48, ...), hourly amounts (P0000) and recent-weather groups (RERA).
void foldReportHistory(std::vector<Station>& st);
// Minutes since precipitation ended according to one report's remarks; -1 if the remarks say nothing, 0 if it is still going on.
double remarkPrecipEndedMinAgo(const std::string& raw, int obsMinute, bool* frozen = nullptr);

// datis.clowd.io/api/<ICAO> -> concatenated ATIS text ("" if none). `letter` gets the information letter if present.
bool parseDatisJson(const std::string& body, std::string& text);

std::string awcMetarUrl(double lat, double lon, double searchNm);
std::string datisUrl(const std::string& icao);

} // namespace wx
