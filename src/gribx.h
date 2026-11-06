// General GRIB2 reader for the turbulence data (NOAA GTGN and ECMWF Open Data).
// Grids: regular lat/lon (3.0) and Lambert conformal (3.30). Packing: simple (5.0), JPEG2000 (5.40, OpenJPEG),
// PNG (5.41) and CCSDS/AEC (5.42, libaec). Bitmaps (section 6) are honoured.
// The header can be read from the first part of a message only (a byte-range download), so the level of each
// message in a large file can be found without downloading the data.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace wx {
namespace gribx {

struct Grid {
    int tmpl = -1;                 // 0 = lat/lon, 30 = Lambert conformal
    int nx = 0, ny = 0;
    int scan = 0;                  // scanning mode flags (0x80 i negative, 0x40 j positive, 0x20 j consecutive)
    // lat/lon
    double la1 = 0, lo1 = 0, di = 0, dj = 0;
    // Lambert conformal
    double lad = 0, lov = 0, dx = 0, dy = 0, latin1 = 0, latin2 = 0, radius = 6371229.0;
    bool southPole = false;
    // Fractional grid position of a point: fi along the stored rows (0..nx-1), fj along the stored columns (0..ny-1).
    // False when the point is outside the grid (for a global lat/lon grid, fi wraps).
    bool locate(double lat, double lon, double& fi, double& fj) const;
    // Inverse of locate (for tests and logs).
    bool latLonOf(double fi, double fj, double& lat, double& lon) const;
    size_t index(int i, int j) const { return (scan & 0x20) ? (size_t)i * (size_t)ny + (size_t)j : (size_t)j * (size_t)nx + (size_t)i; }
    bool periodicI() const;        // a global lat/lon grid whose rows close around the earth
    std::string describe() const;
};

struct Message {
    size_t offset = 0, length = 0; // position of the message in its file
    int discipline = -1, category = -1, number = -1, pdt = -1;
    int levelType = -1;            // type of first fixed surface (100 isobaric Pa, 102 m above MSL, 103 m above ground, ...)
    double level = 0;              // its value (Pa, m, ...)
    std::time_t refTime = 0;
    int ftUnit = -1;
    long ftValue = 0;              // forecast time in ftUnit
    Grid grid;
    int packing = -1;              // template 5.x
    int nbits = 0;
    size_t nvalues = 0;            // number of packed values (section 5)
    std::string error;
    std::time_t validTime() const; // refTime + forecast time (0 when unknown)
};

// Reads sections 0-5 of a message beginning at data[0]. `n` may be smaller than the whole message.
bool readHeader(const uint8_t* data, size_t n, Message& m);
// Total length of the message beginning at data[0] (from section 0), 0 if not a GRIB2 message.
uint64_t messageLength(const uint8_t* data, size_t n);
// Decodes a whole message: `values` gets nx*ny floats in storage order (Grid::index), NaN where the bitmap says missing.
bool decode(const uint8_t* data, size_t n, Message& m, std::vector<float>& values);

// Bilinear sample of a decoded field at a fractional grid position; NaN when any of the 4 points is missing.
float bilinear(const Grid& g, const std::vector<float>& v, double fi, double fj);

} // namespace gribx
} // namespace wx
