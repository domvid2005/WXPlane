// Parsing of the NOAA Aviation Weather Center (METAR) and D-ATIS JSON responses. No network code here.
#pragma once
#include <string>
#include <vector>

#include "wxmodel.h"

namespace wx {

// aviationweather.gov /api/data/metar?format=json
std::vector<Station> parseAwcMetarJson(const std::string& body, std::string* error = nullptr);

// datis.clowd.io/api/<ICAO> -> concatenated ATIS text ("" if none). `letter` gets the information letter if present.
bool parseDatisJson(const std::string& body, std::string& text);

std::string awcMetarUrl(double lat, double lon, double searchNm);
std::string datisUrl(const std::string& icao);

} // namespace wx
