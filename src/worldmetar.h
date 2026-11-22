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

// X-Plane 12's own METAR file (5.1.5, xp_metar_file). With X-Plane's real weather on, X-Plane downloads the world's METARs into
// Output/real weather/ every half hour as METAR-<year>-<month>-<day>-<hour>.00-ZULU.txt; add-ons such as Little Navmap and Pilot2ATC
// read the newest of them. WXPlane needs X-Plane's real weather off, so those files went stale (the logs of
// 2026-11 show X-Plane's real-weather folder still at 2026-10-31) and the add-ons showed METARs weeks old. WXPlane writes its own
// worldwide METARs there in the same form: for each report a line "YYYY/MM/DD HH:MM" (its observation time, UTC), then the METAR.
// The file name for the hour of `now` (UTC), as X-Plane names its files (month and day without a leading zero).
std::string xpMetarFileName(std::time_t now);
// The file's text: stations in ICAO order, each "YYYY/MM/DD HH:MM\nMETAR\n\n"; a report without an observation time takes `now`.
// Reports with a line break or a non-printable character in them are left out (the file is read line by line).
std::string formatXpMetarFile(const std::vector<Station>& stations, std::time_t now);
// Writes that file into `dir` (created if missing) through a temporary file, so a reader never sees half a file; `previous` (a name
// this session wrote before, for an earlier hour) is removed once the new one is in place. A file of the same name is replaced; no other file is touched.
// Returns the name written, or "" with *err set.
std::string writeXpMetarFile(const std::string& dir, const std::vector<Station>& stations, std::time_t now,
                             const std::string& previous, std::string* err);

} // namespace wx
