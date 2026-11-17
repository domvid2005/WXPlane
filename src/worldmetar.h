// Worldwide METARs for the map, display only (5.1.3, for aircraft such as the Falcon 8X flown far from WXPlane's loaded area):
// aviationweather.gov's cache file with every current METAR in the world. Nothing from it is sent to X-Plane.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "wxmodel.h"

namespace wx {

constexpr const char* kWorldMetarUrl = "https://aviationweather.gov/data/cache/metars.cache.csv.gz";

// Reads the cache file's CSV (its preamble lines, then a header row starting "raw_text,"): one Station per row with a position and a
// METAR, its report parsed. Rows older than maxAgeS before `now` are left out. Returns false when no header row is found.
bool parseWorldMetarCsv(const std::string& csv, std::time_t now, std::vector<Station>& out, double maxAgeS = 3 * 3600.0);

} // namespace wx
