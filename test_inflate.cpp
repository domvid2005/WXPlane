#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include "../src/inflate.h"
int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static std::string slurp(const char* p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
int main(int argc, char** argv) {
    // argv: pairs of <file.gz> <file.raw>
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string gz = slurp(argv[i]), raw = slurp(argv[i + 1]);
        std::vector<uint8_t> out;
        bool ok = wx::gunzip(gz, out, 1u << 30);
        CHECK(ok);
        CHECK(out.size() == raw.size() && std::string(out.begin(), out.end()) == raw);
        std::printf("%s: %zu -> %zu bytes %s\n", argv[i], gz.size(), out.size(), ok ? "ok" : "FAILED");
    }
    std::vector<uint8_t> o;
    CHECK(!wx::gunzip(std::string("not gzip data at all, definitely"), o, 1000));
    std::printf("%s\n", fails ? "INFLATE FAILED" : "INFLATE OK");
    return fails;
}
