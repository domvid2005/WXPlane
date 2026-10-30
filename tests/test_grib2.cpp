#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include "../src/grib2.h"
int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
// Files written by gen_grib.py: <name>.grib2 with a known formula  v(lat,lon) = round(lat*3 + (lon+180)*0.5, 1)
int main(int argc, char** argv) {
    std::string dir = argv[1];
    for (int a = 2; a < argc; ++a) {
        std::string nm = argv[a], g = slurp(dir + "/" + nm + ".grib2");
        wx::GribField f;
        wx::GribWindow w;
        w.latMin = 33.0; w.latMax = 36.0; w.lonMin = -100.0; w.lonMax = -95.0;
        bool ok = wx::decodeGrib2((const uint8_t*)g.data(), g.size(), w, f);
        if (!ok) std::printf("%s: error: %s\n", nm.c_str(), f.error.c_str());
        CHECK(ok);
        if (!ok) continue;
        CHECK(f.ni > 400 && f.nj > 250 && f.ni < 600 && f.nj < 400);
        CHECK(std::fabs(f.dLat - 0.01) < 1e-9 && std::fabs(f.dLon - 0.01) < 1e-9);
        int bad = 0, nan = 0, n = 0;
        for (double lat = 33.2; lat < 35.8; lat += 0.173)
            for (double lon = -99.8; lon < -95.6; lon += 0.211) {
                float v = f.at(lat, lon);
                ++n;
                if (std::isnan(v)) { ++nan; continue; }
                // cell centre of the containing cell
                double cl = f.lat0 - std::lround((f.lat0 - lat) / f.dLat) * f.dLat, co = f.lon0 + std::lround((lon - f.lon0) / f.dLon) * f.dLon;
                double want = std::round((cl * 3 + (co + 180) * 0.5) * 10) / 10;
                if (std::fabs(v - want) > 0.051) { ++bad; if (bad < 4) std::printf("  mismatch at %.3f,%.3f: got %f want %f\n", lat, lon, v, want); }
            }
        std::printf("%s: grid %dx%d lat0=%.3f lon0=%.3f  checked %d points, %d bad, %d nan\n", nm.c_str(), f.ni, f.nj, f.lat0, f.lon0, n, bad, nan);
        CHECK(bad == 0);
        CHECK(nan == 0 || nm.find("bitmap") != std::string::npos);
        CHECK(std::isnan(f.at(50.0, -97.0)) && std::isnan(f.at(34.0, -80.0)));      // outside the window
    }
    std::printf("%s\n", fails ? "GRIB2 FAILED" : "GRIB2 OK");
    return fails;
}
