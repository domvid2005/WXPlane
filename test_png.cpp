#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include "../src/png.h"
int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
// usage: test_png <dir>; reads <name>.png and <name>.raw (expected px bytes) + <name>.info ("w h channels depth")
int main(int argc, char** argv) {
    std::string dir = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string nm = argv[i], png = slurp(dir + "/" + nm + ".png"), raw = slurp(dir + "/" + nm + ".raw");
        int w, h, c, d;
        FILE* f = std::fopen((dir + "/" + nm + ".info").c_str(), "r");
        if (!f || std::fscanf(f, "%d %d %d %d", &w, &h, &c, &d) != 4) { std::printf("missing info %s\n", nm.c_str()); ++fails; continue; }
        std::fclose(f);
        wx::PngImage im;
        bool ok = wx::decodePng((const uint8_t*)png.data(), png.size(), im);
        CHECK(ok);
        CHECK(ok && im.w == w && im.h == h && im.channels == c && im.depth == d);
        CHECK(ok && std::string(im.px.begin(), im.px.end()) == raw);
        std::printf("%s: %s\n", nm.c_str(), ok && im.px.size() == raw.size() ? "decoded" : "BAD");
    }
    wx::PngImage im;
    CHECK(!wx::decodePng((const uint8_t*)"nope", 4, im));
    std::printf("%s\n", fails ? "PNG FAILED" : "PNG OK");
    return fails;
}
