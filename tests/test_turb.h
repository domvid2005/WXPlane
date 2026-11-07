// Turbulence data tests (included by test_wx.cpp; uses its CHECK/NEAR macros).
#pragma once
#include <cstring>
#include <memory>
#include <vector>

#include "../src/gribx.h"
#include "../src/turbulence.h"

extern "C" {
#include <libaec.h>
}

#ifndef WX_TEST_DATA_DIR
#define WX_TEST_DATA_DIR "tests/data"
#endif

namespace tt {
using namespace wx;

std::vector<uint8_t> readAll(const char* path) {
    std::vector<uint8_t> b;
    FILE* f = std::fopen(path, "rb");
    if (!f) return b;
    int c;
    while ((c = std::fgetc(f)) != EOF) b.push_back((uint8_t)c);
    std::fclose(f);
    return b;
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { b[at] = v >> 24; b[at + 1] = (v >> 16) & 255; b[at + 2] = (v >> 8) & 255; b[at + 3] = v & 255; }
void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) { b[at] = (v >> 8) & 255; b[at + 1] = v & 255; }
void sec(std::vector<uint8_t>& msg, int num, std::vector<uint8_t> body) {   // body excludes the 5-byte section header
    size_t at = msg.size();
    msg.resize(at + 5);
    put32(msg, at, (uint32_t)(5 + body.size()));
    msg[at + 4] = (uint8_t)num;
    msg.insert(msg.end(), body.begin(), body.end());
}
uint32_t sm32(long v) { return v < 0 ? (0x80000000u | (uint32_t)(-v)) : (uint32_t)v; }

// A lat/lon 3.0 grid message holding `vals` with CCSDS packing (5.42), encoded by libaec as ECMWF does
// (flags 14 = 3-byte samples allowed | MSB | preprocessing, block 32, RSI 128), R = 100, D = 1: value = (100 + X) / 10.
std::vector<uint8_t> ccsdsMessage(int nx, int ny, const std::vector<uint32_t>& X, int nbits) {
    std::vector<uint8_t> m = {'G', 'R', 'I', 'B', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    {   std::vector<uint8_t> b(16, 0); put16(b, 0, 98); b[4] = 2; put16(b, 7, 2026); b[9] = 11; b[10] = 6; b[11] = 12; sec(m, 1, b); }   // 2026-11-06 12:00
    {   std::vector<uint8_t> b(67, 0);
        put32(b, 1, (uint32_t)(nx * ny)); put16(b, 7, 0); b[9] = 6;
        put32(b, 25, (uint32_t)nx); put32(b, 29, (uint32_t)ny); put32(b, 33, 0); put32(b, 37, 0xFFFFFFFFu);
        put32(b, 41, sm32(50000000)); put32(b, 45, sm32(350000000)); b[49] = 48;
        put32(b, 50, sm32(50000000 - (ny - 1) * 250000)); put32(b, 54, sm32(350000000 + (nx - 1) * 250000));
        put32(b, 58, 250000); put32(b, 62, 250000); b[66] = 0;
        sec(m, 3, b); }
    {   std::vector<uint8_t> b(29, 0); put16(b, 2, 0); b[4] = 2; b[5] = 2; b[12] = 1; put32(b, 13, 6); b[17] = 100; b[18] = 0; put32(b, 19, 25000); b[23] = 255;
        sec(m, 4, b); }
    std::vector<uint8_t> packed(X.size() * 4 + 1024);
    {   std::vector<uint8_t> raw;
        for (uint32_t v : X) { raw.push_back((v >> 8) & 255); raw.push_back(v & 255); }
        aec_stream s; std::memset(&s, 0, sizeof s);
        s.next_in = raw.data(); s.avail_in = raw.size(); s.next_out = packed.data(); s.avail_out = packed.size();
        s.bits_per_sample = (unsigned)nbits; s.block_size = 32; s.rsi = 128; s.flags = AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS;
        if (aec_buffer_encode(&s) != AEC_OK) packed.clear(); else packed.resize(s.total_out); }
    {   std::vector<uint8_t> b(20, 0); put32(b, 0, (uint32_t)X.size()); put16(b, 4, 42);
        float R = 100.0f; uint32_t rb; std::memcpy(&rb, &R, 4); put32(b, 6, rb); put16(b, 10, 0); put16(b, 12, 1); b[14] = (uint8_t)nbits; b[15] = 0;
        b[16] = AEC_DATA_3BYTE | AEC_DATA_MSB | AEC_DATA_PREPROCESS; b[17] = 32; put16(b, 18, 128);
        sec(m, 5, b); }
    sec(m, 6, {255});
    sec(m, 7, packed);
    m.insert(m.end(), {'7', '7', '7', '7'});
    put32(m, 12, (uint32_t)m.size());
    return m;
}

void run() {
    using namespace gribx;
    // --- JPEG2000 + bitmap: a real NOAA GRIB2 message (WAVEWATCH III wind speed, NCEPLIBS-g2c test data).
    //     The reference (degrib2) gives 11041 points, MIN 0.09999999, AVE 5.64625025, MAX 16.43000031.
    {
        auto b = readAll(WX_TEST_DATA_DIR "/gdaswave_wind_jpeg2000.grib2");
        CHECK(b.size() == 15254);
        Message m; std::vector<float> v;
        CHECK(decode(b.data(), b.size(), m, v));
        double mn = 1e9, mx = -1e9, s = 0; size_t n = 0;
        for (float x : v) if (!std::isnan(x)) { mn = std::min(mn, (double)x); mx = std::max(mx, (double)x); s += x; ++n; }
        CHECK(m.packing == 40 && m.grid.tmpl == 0 && m.grid.nx == 241 && m.grid.ny == 151 && v.size() == 241u * 151u);
        CHECK(n == 11041);
        NEAR(mn, 0.1, 1e-5); NEAR(mx, 16.43, 1e-4); NEAR(s / n, 5.64625, 1e-3);
        CHECK(m.discipline == 0 && m.category == 2 && m.number == 1 && m.levelType == 1);
        // The header alone (a byte-range read of the first 200 bytes) gives the same identification and the length.
        Message h;
        CHECK(readHeader(b.data(), 200, h) && h.length == 15254 && h.packing == 40 && h.category == 2 && h.number == 1);
        CHECK(messageLength(b.data(), 16) == 15254);
        // A truncated message is refused for decoding.
        CHECK(!decode(b.data(), 5000, h, v));
    }
    // --- CCSDS (template 5.42), the packing of ECMWF Open Data, encoded with libaec.
    {
        const int nx = 37, ny = 23;
        std::vector<uint32_t> X(nx * ny);
        for (size_t k = 0; k < X.size(); ++k) X[k] = (uint32_t)((k * 37 + (k / nx) * 11) % 2000);
        auto msg = ccsdsMessage(nx, ny, X, 11);
        Message m; std::vector<float> v;
        CHECK(decode(msg.data(), msg.size(), m, v));
        CHECK(m.packing == 42 && v.size() == X.size() && m.levelType == 100 && nr(m.level, 25000, 1e-6) && m.category == 2 && m.number == 2);
        bool same = v.size() == X.size();
        for (size_t k = 0; same && k < X.size(); ++k) same = nr(v[k], (100.0 + X[k]) / 10.0, 1e-3);
        CHECK(same);
        CHECK(m.validTime() == m.refTime + 6 * 3600);
        double fi, fj;   // lat/lon locate on a grid starting at 50N 350E (= 10W), 0.25 deg
        CHECK(m.grid.locate(49.5, -9.5, fi, fj) && nr(fi, 2, 1e-9) && nr(fj, 2, 1e-9));
        CHECK(!m.grid.locate(52, -9.5, fi, fj));
    }
    // --- Lambert conformal (3.30): the HRRR grid GTGN uses. Corners from NOAA's HRRR grid description.
    {
        Grid g; g.tmpl = 30; g.nx = 1799; g.ny = 1059; g.la1 = 21.138123; g.lo1 = 237.280472; g.lov = 262.5; g.lad = 38.5;
        g.latin1 = g.latin2 = 38.5; g.dx = g.dy = 3000; g.scan = 0x40; g.radius = 6371229.0;
        double lat, lon, fi, fj;
        g.latLonOf(1798, 1058, lat, lon);
        NEAR(lat, 47.842195, 0.01); NEAR(lon, 299.082807 - 360, 0.01);
        CHECK(g.locate(39.0, -97.0, fi, fj));          // Kansas: inside, near the middle
        g.latLonOf(fi, fj, lat, lon);
        NEAR(lat, 39.0, 1e-6); NEAR(lon, -97.0, 1e-6);
        CHECK(fi > 800 && fi < 1100 && fj > 500 && fj < 800);
        CHECK(!g.locate(51.5, -0.1, fi, fj));          // London: outside
        CHECK(g.locate(21.138123, 237.280472, fi, fj) && nr(fi, 0, 1e-6) && nr(fj, 0, 1e-6));
    }

    using namespace turb;
    // --- calibration: monotonic, 0 at calm, the measured points, saturation at 3
    {
        CHECK(edrToXpLevel(0) == 0);
        NEAR(xpLevelForSigma(0.56), 2.0, 1e-9); NEAR(xpLevelForSigma(0.40), 1.0, 1e-9); NEAR(xpLevelForSigma(1.0), 3.0, 1e-9);
        bool mono = true; double prev = -1;
        for (double e = 0; e <= 1.0; e += 0.01) { double l = edrToXpLevel(e); if (l < prev - 1e-12) mono = false; prev = l; }
        CHECK(mono);
        CHECK(edrToXpLevel(0.02) == 0 && edrToXpLevel(0.07) == 0);     // smooth air (and the ECMWF 0.05 of the first flight): nothing
        CHECK(edrToXpLevel(0.15) > 0.08 && edrToXpLevel(0.15) < 0.2); // light
        CHECK(edrToXpLevel(0.35) > 0.4 && edrToXpLevel(0.35) < 0.5);   // moderate: the A319 climb of 2026-11-06 gets ~0.45, not ~0.75
        CHECK(edrToXpLevel(0.60) > 0.8 && edrToXpLevel(0.60) < 0.95);  // severe
        CHECK(ellrodToEdrEquivalent(0) == 0 && nr(ellrodToEdrEquivalent(8), 0.22, 1e-9) && ellrodToEdrEquivalent(100) <= 0.40 && edrToXpLevel(ellrodToEdrEquivalent(1)) == 0);
        NEAR(isaAltFt(1013.25), 0, 1); NEAR(isaAltFt(250), 34000, 100); NEAR(isaAltFt(200), 38662, 100);
        NEAR(isaPressureHpa(isaAltFt(300)), 300, 0.01); NEAR(isaPressureHpa(isaAltFt(150)), 150, 0.01);
    }
    // --- ECMWF index lines
    {
        auto idx = parseEcmwfIndex("{\"domain\": \"g\", \"date\": \"20261106\", \"time\": \"0000\", \"step\": \"6\", \"levelist\": \"250\", \"levtype\": \"pl\", \"param\": \"u\", \"_offset\": 96461314, \"_length\": 599453}\n"
                                   "{\"domain\": \"g\", \"levtype\": \"sfc\", \"param\": \"2t\", \"step\": \"6\", \"_offset\": 0, \"_length\": 1000}\n");
        CHECK(idx.size() == 2 && idx[0].param == "u" && idx[0].level == 250 && idx[0].offset == 96461314u && idx[0].length == 599453u && idx[0].levtype == "pl" && idx[0].step == 6);
        CHECK(idx[1].param == "2t" && idx[1].level == 0);
    }
    // --- Ellrod TI1 on a synthetic field: u = a*x, v = -a*y (pure stretching deformation, DEF = 2a) and a
    //     vertical shear of 10 m/s between 300 and 250 hPa gives TI1 = (10/dz) * 2a.
    {
        Grid g; g.tmpl = 0; g.nx = 81; g.ny = 41; g.la1 = 50; g.lo1 = 0; g.di = g.dj = 0.25; g.scan = 0;
        const double a = 2e-5, R = 6371229.0, kD = 3.14159265358979 / 180;
        std::vector<float> u3(g.nx * g.ny), v3(u3.size()), u25(u3.size()), v25(u3.size());
        for (int j = 0; j < g.ny; ++j) for (int i = 0; i < g.nx; ++i) {
            double lat = 50 - j * 0.25, lon = i * 0.25;
            double x = R * std::cos(lat * kD) * (lon - 10) * kD, y = R * (lat - 45) * kD;
            size_t k = g.index(i, j);
            u3[k] = (float)(a * x); v3[k] = (float)(-a * y); u25[k] = (float)(a * x + 10); v25[k] = (float)(-a * y);
        }
        EcCube c; std::string err;
        CHECK(buildEcCube(g, {{300, &u3, &v3}, {250, &u25, &v25}}, 45, 10, 3, c, err));
        CHECK(c.layers.size() == 1);
        double dz = (isaAltFt(250) - isaAltFt(300)) * 0.3048, want = 10.0 / dz * 2 * a * 1e7, ti = -1;
        CHECK(c.sampleTi(45, 10, c.layers[0].altFt, ti));
        NEAR(ti, want, want * 0.03);
        CHECK(!c.sampleTi(45, 10, 5000, ti));   // below FL100 the index is not used
        CHECK(!c.sampleTi(60, 10, 32000, ti));  // outside the crop
    }
    // --- GTGN cube: vertical interpolation, levels below ground, edge coverage, cross-fade, staleness, blending
    {
        Grid g; g.tmpl = 30; g.nx = 300; g.ny = 200; g.la1 = 30; g.lo1 = -110; g.lov = -100; g.latin1 = g.latin2 = 38.5;
        g.dx = g.dy = 3000; g.scan = 0x40;
        auto cube = std::make_shared<GtgnCube>();
        cube->grid = g;
        std::vector<float> f(g.nx * g.ny);
        double clat, clon; g.latLonOf(150, 100, clat, clon);
        auto level = [&](double alt, float val, bool holeAtCentre) {
            std::fill(f.begin(), f.end(), val);
            if (holeAtCentre) for (int j = 95; j <= 105; ++j) for (int i = 145; i <= 155; ++i) f[g.index(i, j)] = NAN;
            cube->addLevel(alt, f, clat, clon, 70);
        };
        level(1000, 0.5f, true);   // below ground at the centre
        level(30000, 0.10f, false);
        level(31000, 0.30f, false);
        cube->valid = 1000000;
        cube->file = "a";
        double e, cov;
        CHECK(cube->sample(clat, clon, 30500, e, cov)); NEAR(e, 0.20, 1e-6); NEAR(cov, 1.0, 1e-9);
        CHECK(cube->sample(clat, clon, 30000, e, cov)); NEAR(e, 0.10, 1e-6);
        CHECK(cube->sample(clat, clon, 1000, e, cov)); NEAR(e, 0.10, 1e-6);   // the missing level is skipped
        CHECK(!cube->sample(clat, clon, 40000, e, cov));                       // far above the levels held
        double elat, elon; g.latLonOf(150, 82, elat, elon);
        CHECK(cube->sample(elat, elon, 30500, e, cov));
        CHECK(cube->cropCovers(clat, clon, 25) && !cube->cropCovers(elat - 2.5, elon, 25));
        // the grid edge: coverage falls off (crop at the edge of a small grid)
        auto edge = std::make_shared<GtgnCube>(); edge->grid = g;
        double la0, lo0; g.latLonOf(5, 100, la0, lo0);
        std::fill(f.begin(), f.end(), 0.2f);
        edge->addLevel(30000, f, la0, lo0, 70); edge->addLevel(31000, f, la0, lo0, 70);
        CHECK(edge->sample(la0, lo0, 30500, e, cov) && cov < 0.2);
        double la1, lo1; g.latLonOf(30, 100, la1, lo1);
        CHECK(edge->sample(la1, lo1, 30500, e, cov) && nr(cov, 1.0, 1e-9));

        Inputs in; in.gtgn = cube; in.gtgnArrived = 1000000;
        Sample s = sampleAt(in, clat, clon, 30500, 1000000 + 600, 0);
        CHECK(std::string(s.source) == "GTGN" && nr(s.gtgnEdr, 0.20, 1e-6) && s.noise > 0.75 && s.noise < 1.33 && nr(s.edr, 0.20 * s.noise, 1e-6));
        CHECK(std::string(sampleAt(in, clat, clon, 30500, 1000000 + kGtgnMaxAgeS + 1, 0).source) == "none");   // stale: not used
        // a new analysis is faded in over 3 minutes
        auto c2 = std::make_shared<GtgnCube>(*cube); c2->file = "b"; c2->valid = 1000900;
        for (auto& L : c2->levels) for (auto& x : L.v) if (!std::isnan(x)) x = 0.6f;
        Inputs in2; in2.gtgn = c2; in2.gtgnPrev = cube; in2.gtgnArrived = 1001000;
        NEAR(sampleAt(in2, clat, clon, 30500, 1001000, 0).gtgnEdr, 0.20, 1e-6);
        NEAR(sampleAt(in2, clat, clon, 30500, 1001090, 0).gtgnEdr, 0.40, 1e-6);
        NEAR(sampleAt(in2, clat, clon, 30500, 1001180, 0).gtgnEdr, 0.60, 1e-6);
        // the variation is smooth in space and time and stays a modest factor
        double mn = 9, mx = 0, jump = 0, last = pocketNoise(40, -100, 35000, 0, 0.25);
        for (int k = 1; k < 2000; ++k) {
            double n = pocketNoise(40 + k * 0.0005, -100, 35000, k * 0.5, 0.25);   // ~55 m and 0.5 s per step
            mn = std::min(mn, n); mx = std::max(mx, n); jump = std::max(jump, std::fabs(n - last)); last = n;
        }
        CHECK(mn > 0.6 && mx < 1.7 && jump < 0.02);
        // no data at all: nothing invented
        Inputs none; Sample z = sampleAt(none, 40, -100, 35000, 1000000, 0);
        CHECK(z.edr == 0 && std::string(z.source) == "none");
    }
}
} // namespace tt
