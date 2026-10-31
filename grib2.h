// Minimal GRIB2 reader for NOAA MRMS products: regular lat/lon grid (template 3.0), simple packing (5.0) or PNG
// packing (5.41). Only the part of the grid inside a requested window is kept, so a 24-million-point CONUS grid
// does not have to stay in memory.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace wx {

struct GribWindow {
    double latMin = -90, latMax = 90, lonMin = -180, lonMax = 180; // lon in -180..180; a window may not cross 180
};

struct GribField {
    int ni = 0, nj = 0;           // sub-grid size
    double lat0 = 0, lon0 = 0;    // north-west corner (centre of the first cell), lon in -180..180
    double dLat = 0, dLon = 0;    // spacing in degrees (positive); rows run north -> south, columns west -> east
    std::vector<float> v;         // ni*nj values; NaN where a bitmap says "no value"
    int discipline = -1;
    std::time_t refTime = 0;      // valid time of the data (UTC), 0 if absent
    std::string error;
    bool valid() const { return ni > 0 && nj > 0 && v.size() == (size_t)ni * (size_t)nj; }
    // Value of the cell containing the point, or NaN when outside the sub-grid.
    float at(double lat, double lon) const;
};

bool decodeGrib2(const uint8_t* data, size_t n, const GribWindow& win, GribField& out);

} // namespace wx
