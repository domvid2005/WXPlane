// European radar (EUMETNET OPERA) tests (included by test_wx.cpp; uses its CHECK/NEAR macros).
// opera_head.bin is the real first 5469 bytes of OPERA@20261111T2010@0@DBZH.tiff (the whole header and tile table).
// Tiles are made here as zlib streams of stored (uncompressed) deflate blocks, so no compressor is needed.
#pragma once
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/opera.h"

#ifndef WX_TEST_DATA_DIR
#define WX_TEST_DATA_DIR "tests/data"
#endif

namespace to {
using namespace wx;

std::string readAll(const char* path) {
    std::string b;
    FILE* f = std::fopen(path, "rb");
    if (!f) return b;
    int c;
    while ((c = std::fgetc(f)) != EOF) b.push_back((char)c);
    std::fclose(f);
    return b;
}

std::string zlibStored(const std::string& raw) {
    std::string z;
    z.push_back((char)0x78); z.push_back((char)0x01);
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back((char)(last ? 1 : 0));
        z.push_back((char)(n & 255)); z.push_back((char)(n >> 8));
        z.push_back((char)(~n & 255)); z.push_back((char)((~n >> 8) & 255));
        z.append(raw, at, n);
        at += n;
    } while (at < raw.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    const uint32_t ad = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back((char)((ad >> (8 * i)) & 255));
    return z;
}

// A tile whose every pixel is `v` (DBZH) with quality 0.5, except the given (pixel, value) overrides.
std::string makeTile(const OperaHeader& h, float v, const std::map<int, float>& over = {}) {
    std::string raw((size_t)h.tileW * h.tileH * h.spp * 4, '\0');
    for (int p = 0; p < h.tileW * h.tileH; ++p) {
        auto it = over.find(p);
        const float f = it != over.end() ? it->second : v, q = 0.5f;
        std::memcpy(&raw[(size_t)p * h.spp * 4], &f, 4);
        if (h.spp > 1) std::memcpy(&raw[(size_t)p * h.spp * 4 + 4], &q, 4);
    }
    return zlibStored(raw);
}

void pixelOf(const OperaHeader& h, double lat, double lon, int& col, int& row) {
    double x, y;
    operaForward(h, lat, lon, x, y);
    col = (int)((x - h.x0) / h.dx);
    row = (int)((h.y0 - y) / h.dy);
}

void run() {
    // URLs and slots
    CHECK(operaUrl(1794427800) == "https://s3.waw3-1.cloudferro.com/openradar-24h/2026/11/11/OPERA/COMP/OPERA@20261111T2010@0@DBZH.tiff");
    CHECK(operaNewestSlot(1794427800 + 239) == 1794427800 - 300);   // 20:13:59Z: 20:10 is not published yet
    CHECK(operaNewestSlot(1794427800 + 240) == 1794427800);
    // coverage: Europe yes, the US / Africa south of the domain / Asia no
    CHECK(operaCovers(40.4722, -3.5608));   // LEMD
    CHECK(operaCovers(51.4775, -0.4614));   // EGLL
    CHECK(operaCovers(60.1939, 11.1004));   // ENGM
    CHECK(operaCovers(38.0, 23.7));         // Athens
    CHECK(!operaCovers(40.64, -73.78));     // KJFK
    CHECK(!operaCovers(5.0, 10.0));
    CHECK(!operaCovers(35.5, 139.8));       // Tokyo

    const std::string head = readAll(WX_TEST_DATA_DIR "/opera_head.bin");
    CHECK(head.size() == 5469);
    OperaHeader h;
    std::string err;
    size_t need = 0;
    CHECK(operaParseHeader(head, h, err, need));
    CHECK(h.width == 3800 && h.height == 4400 && h.tileW == 512 && h.tileH == 512 && h.spp == 2);
    CHECK(h.tileOff.size() == 72 && h.tileLen.size() == 72 && h.tilesAcross() == 8);
    CHECK(h.tileOff[34] == 2358065 && h.tileLen[34] == 10958);
    NEAR(h.dx, 1000, 1e-9); NEAR(h.dy, 1000, 1e-9); NEAR(h.x0, -500, 0.01); NEAR(h.y0, 500, 0.01);
    NEAR(h.lat0, 55, 1e-9); NEAR(h.lon0, 10, 1e-9); NEAR(h.fe, 1950000, 1e-6); NEAR(h.fn, -2100000, 1e-6);
    NEAR(h.a, 6378137.0, 1e-3); NEAR(h.invF, 298.257223563, 1e-6);
    // a cut header asks for more; garbage is refused
    {
        OperaHeader h2;
        size_t n2 = 0;
        CHECK(!operaParseHeader(head.substr(0, 400), h2, err, n2) && n2 > 400);
        CHECK(!operaParseHeader(std::string("GIF89a....."), h2, err, n2) && n2 == 0);
        std::string big = head; big[2] = 43;   // BigTIFF
        CHECK(!operaParseHeader(big, h2, err, n2));
    }
    // projection: the pixels the browser check read (2026-11-11) for LEMD and EGLL
    int col = 0, row = 0;
    pixelOf(h, 40.4722, -3.5608, col, row);
    CHECK(std::abs(col - 798) <= 1 && std::abs(row - 3605) <= 1);
    pixelOf(h, 51.4775, -0.4614, col, row);
    CHECK(std::abs(col - (2 * 512 + 203)) <= 1 && std::abs(row - (4 * 512 + 391)) <= 1);   // tile 34, pixel (203, 391)
    double x, y;
    operaForward(h, 55, 10, x, y);
    NEAR(x, 1950000, 1e-3); NEAR(y, -2100000, 1e-3);

    // tile decoding: no echo (NaN) -> -99, no radar (-9.999e6) -> -999, echoes kept; the quality sample is skipped
    {
        std::vector<float> d;
        const std::string t = makeTile(h, 20.5f, {{0, NAN}, {1, -9.999e6f}, {2, 41.5f}});
        CHECK(operaDecodeTile(t, h, d, err));
        CHECK(d.size() == 512u * 512u);
        CHECK(d[0] == -99.0f && d[1] == -999.0f && d[2] == 41.5f && d[3] == 20.5f && d[512 * 512 - 1] == 20.5f);
        CHECK(!operaDecodeTile(t.substr(0, 1000), h, d, err));
        CHECK(!operaDecodeTile("not zlib", h, d, err));
    }

    // the window around LEMD: every needed tile is fetched once, the values land on the right cells
    {
        std::map<uint64_t, int> tileAt;
        for (int t = 0; t < 72; ++t) tileAt[h.tileOff[t]] = t;
        int calls = 0;
        int lc = 0, lr = 0;
        pixelOf(h, 40.4722, -3.5608, lc, lr);
        const int lemdTile = (lr / 512) * 8 + lc / 512, lemdPix = (lr % 512) * 512 + lc % 512;
        OperaRangeFn get = [&](uint64_t off, uint64_t len, std::string& data, std::string& e) {
            ++calls;
            auto it = tileAt.find(off);
            if (it == tileAt.end() || len != h.tileLen[it->second]) { e = "bad range"; return false; }
            const int t = it->second;
            std::map<int, float> over;
            if (t == lemdTile) over[lemdPix] = 47.0f;   // a storm cell right over the airport
            data = makeTile(h, t == lemdTile ? 17.5f : NAN, over);
            return true;
        };
        GribWindow win;
        win.latMin = 40.4722 - 1.0; win.latMax = 40.4722 + 1.0; win.lonMin = -3.5608 - 1.3; win.lonMax = -3.5608 + 1.3;
        std::map<int, std::vector<float>> cache;
        GribField f;
        int fetched = 0;
        CHECK(operaWindow(h, win, 0.01, get, cache, f, fetched, err));
        CHECK(f.valid() && f.ni == 261 && f.nj == 201);
        CHECK(fetched >= 1 && fetched == calls && (int)cache.size() >= fetched);
        CHECK(cache.count(lemdTile) == 1);
        // the cell over LEMD holds the storm pixel or its 17.5 dBZ neighbours
        const float v = f.at(40.4722, -3.5608);
        CHECK(v == 47.0f || v == 17.5f);
        int storm = 0, rain = 0;
        for (float q : f.v) { if (q == 47.0f) ++storm; else if (q == 17.5f) ++rain; }
        CHECK(storm <= 3 && rain > 1000);
        // a second window reuses the cache: no download
        int fetched2 = -1;
        calls = 0;
        CHECK(operaWindow(h, win, 0.01, get, cache, f, fetched2, err));
        CHECK(fetched2 == 0 && calls == 0);
        // a failed download fails the window (so the caller can fall back)
        std::map<int, std::vector<float>> empty;
        OperaRangeFn bad = [](uint64_t, uint64_t, std::string&, std::string& e) { e = "HTTP 503"; return false; };
        CHECK(!operaWindow(h, win, 0.01, bad, empty, f, fetched, err) && err.find("503") != std::string::npos);
        // outside the composite
        GribWindow us;
        us.latMin = 40; us.latMax = 41; us.lonMin = -75; us.lonMax = -73;
        CHECK(!operaWindow(h, us, 0.01, get, cache, f, fetched, err));
    }
}

} // namespace to
