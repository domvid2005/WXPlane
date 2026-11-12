#pragma once
// European radar: the EUMETNET OPERA maximum-reflectivity composite (DBZH), published every 5 minutes as a Cloud-Optimised GeoTIFF
// (1 km, Lambert azimuthal equal-area, 512 x 512 deflate tiles, float32, two samples: DBZH and a quality index) in the open 24-hour
// store of EUMETNET's Open Radar Data (CC BY 4.0). Only the header and the tiles around the aircraft are downloaded (HTTP ranges).
// No X-Plane dependencies.
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "grib2.h"

namespace wx {

struct OperaHeader {
    int width = 0, height = 0, tileW = 0, tileH = 0, spp = 0;
    std::vector<uint64_t> tileOff, tileLen;
    double x0 = 0, y0 = 0, dx = 0, dy = 0;     // projected corner of pixel (0,0) and pixel size (m); rows run south
    double lat0 = 0, lon0 = 0, fe = 0, fn = 0; // LAEA centre and false easting / northing
    double a = 6378137.0, invF = 298.257223563;
    int tilesAcross() const { return tileW > 0 ? (width + tileW - 1) / tileW : 0; }
};

// Where the composites are: 0 the main bucket (CloudFerro), 1 the backup EUMETNET moved to while the main one has a cloud-provider
// problem (ECMWF; same keys). The fetcher tries the one that last worked first, then the other.
constexpr int kOperaStores = 2;
// The file for the 5-minute slot that ends at t (UTC, rounded down to 5 minutes).
std::string operaUrl(std::time_t t, int store = 0);
// The slot to try first: composites appear about 4 minutes after their time.
std::time_t operaNewestSlot(std::time_t now);
// The bucket listing of the composites of one UTC hour (S3 ListObjectsV2), for finding the newest one when the feed stalls.
std::string operaListUrl(std::time_t hour, int store = 0);
// The newest DBZH GeoTIFF composite named in a listing, 0 if none.
std::time_t operaNewestInListing(const std::string& xml);
// Inside the composite's grid (the standard OPERA domain; coverage inside it is decided by the data).
bool operaCovers(double lat, double lon);
// LAEA (ellipsoidal) forward projection with the header's parameters.
void operaForward(const OperaHeader& h, double lat, double lon, double& x, double& y);
// Parses the TIFF header in `head` (the first bytes of the file). Returns false with `err` set; `needBytes` > head.size() when the
// header continues past what was downloaded (fetch that many bytes and parse again).
bool operaParseHeader(const std::string& head, OperaHeader& h, std::string& err, size_t& needBytes);
// Decodes one tile (zlib/deflate, float32, `spp` samples per pixel) into its DBZH values (tileW * tileH).
bool operaDecodeTile(const std::string& data, const OperaHeader& h, std::vector<float>& dbz, std::string& err);

// Downloads a byte range of the composite: (offset, length) -> data. Returns false with an error.
using OperaRangeFn = std::function<bool(uint64_t off, uint64_t len, std::string& data, std::string& err)>;
// Builds the window around the aircraft on a regular latitude/longitude grid (stepDeg), like an MRMS field: dBZ, -99 where the radars
// see no echo, -999 where no radar covers. Tiles already in `cache` (tile index -> DBZH) are reused, new ones are added to it.
// `tilesFetched` counts the tiles downloaded this time.
bool operaWindow(const OperaHeader& h, const GribWindow& win, double stepDeg, const OperaRangeFn& get, std::map<int, std::vector<float>>& cache,
                 GribField& out, int& tilesFetched, std::string& err);

} // namespace wx
