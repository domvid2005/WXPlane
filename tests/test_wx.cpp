#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "../src/json_mini.h"
#include "../src/metar.h"
#include "../src/sources.h"
#include "../src/wxmodel.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a, b, t) CHECK(std::fabs((double)(a) - (double)(b)) <= (t))

using namespace wx;

static void utc(std::time_t t, std::tm* out) {
#ifdef _WIN32
    gmtime_s(out, &t);
#else
    gmtime_r(&t, out);
#endif
}

int main() {
    Report r;

    // --- plain US METAR
    CHECK(parseReport("METAR KJFK 301551Z 21008KT 10SM FEW040 BKN250 20/10 A3002 RMK AO2 SLP168 T02000100", r));
    CHECK(r.icao == "KJFK" && r.day == 30 && r.hour == 15 && r.minute == 51);
    CHECK(r.hasWind && r.windDir == 210 && r.windKt == 8 && r.gustKt == 0);
    CHECK(r.hasVis && r.visM > 16000);
    CHECK(r.clouds.size() == 2 && r.clouds[0].baseFtAgl == 4000 && r.clouds[1].cover == CloudGroup::BKN);
    CHECK(r.hasTemp && r.tempC == 20 && r.dewpC == 10);
    NEAR(r.altimHpa, 1016.6, 0.2);

    // --- dense fog with vertical visibility
    CHECK(parseReport("KSFO 301556Z 00000KT 1/4SM R28R/1800V3000FT FG VV002 12/12 A3001", r));
    CHECK(r.fog && !r.shallowFog);
    NEAR(r.visM, 402.3, 1);
    CHECK(r.clouds.size() == 1 && r.clouds[0].cover == CloudGroup::VV && r.clouds[0].baseFtAgl == 200);
    CHECK(r.windKt == 0);

    // --- mixed-fraction visibility, patchy / shallow fog, negative temps
    CHECK(parseReport("KABC 010000Z 18005KT 1 1/2SM BCFG BR OVC003 M05/M08 A2992", r));
    NEAR(r.visM, 1.5 * kSmToM, 1);
    CHECK(r.patchyFog && !r.fog && r.mist);
    CHECK(r.tempC == -5 && r.dewpC == -8);
    CHECK(parseReport("KXYZ 010000Z 00000KT 3SM MIFG SKC 05/05 A3010", r));
    CHECK(r.shallowFog && r.clearSky);

    // --- ICAO / metric METAR with gusts, rain, QNH
    CHECK(parseReport("EGLL 301550Z AUTO 24012G25KT 200V280 9999 -RA BKN012 OVC025 14/12 Q1004 NOSIG", r));
    CHECK(r.windDir == 240 && r.windKt == 12 && r.gustKt == 25 && r.windVariable);
    CHECK(r.visM == 10000);
    NEAR(r.precip, 0.225, 0.001);
    NEAR(r.altimHpa, 1004, 0.01);

    // --- VRB, CAVOK, thunderstorm with CB
    CHECK(parseReport("LFPG 301600Z VRB03KT CAVOK 22/10 Q1018", r));
    CHECK(r.windVariable && r.cavok && r.clearSky);
    CHECK(parseReport("KDFW 301553Z 18015G30KT 3SM +TSRA BKN020CB OVC040 24/22 A2985", r));
    CHECK(r.thunder && r.precip > 0.8 && r.clouds[0].kind == 1);

    // --- trend groups must not leak into the observation
    CHECK(parseReport("EGKK 301550Z 27010KT 9999 FEW030 15/09 Q1012 TEMPO 3000 RA BKN008", r));
    CHECK(r.clouds.size() == 1 && r.visM == 10000);

    // --- D-ATIS: METAR-style block and spoken fallback
    Report a;
    CHECK(parseReport("KJFK ATIS INFO A 1551Z. 21008KT 10SM FEW040 BKN250 20/10 A3002 (THREE ZERO ZERO TWO). ILS RWY 22L APPROACH IN USE. DEPARTING RWY 31L. NOTICE TO AIR MISSIONS, RWY 04/22 CLOSED.", a, true));
    CHECK(a.infoLetter == "A" && a.hour == 15 && a.minute == 51);
    CHECK(a.hasWind && a.windDir == 210 && a.hasTemp && a.tempC == 20 && a.dewpC == 10);
    CHECK(parseReport("KXYZ ATIS INFORMATION B 1453Z WIND 270 AT 12 GUSTS 20 VISIBILITY 5 ALTIMETER 2987 TEMPERATURE 8 DEWPOINT 7", a, true));
    CHECK(a.windDir == 270 && a.windKt == 12 && a.gustKt == 20 && a.visM > 8000);
    NEAR(a.altimHpa, 1011.5, 0.3);

    // --- report age
    {
        std::time_t now = std::time(nullptr);
        std::tm g;
        utc(now, &g);
        char buf[96];
        std::tm past = g;
        std::time_t t30 = now - 30 * 60;
        utc(t30, &past);
        std::snprintf(buf, sizeof buf, "KJFK %02d%02d%02dZ 00000KT 10SM CLR 20/10 A3000", past.tm_mday, past.tm_hour, past.tm_min);
        parseReport(buf, r);
        NEAR(reportAgeSeconds(r, now), 1800, 61);
    }

    // --- JSON + AWC response
    Json j;
    CHECK(parseJson("{\"a\":[1,2.5,{\"b\":\"x\\u00e9\\n\"}],\"c\":null,\"d\":true}", j));
    CHECK(j.get("a") && j.get("a")->arr.size() == 3 && j.get("a")->arr[1].num == 2.5);
    CHECK(!parseJson("[1,2", j));
    std::string awc =
        "[{\"icaoId\":\"KSFO\",\"lat\":37.619,\"lon\":-122.365,\"elev\":3,\"rawOb\":\"METAR KSFO 301556Z 00000KT 1/4SM FG VV002 12/12 A3001\"},"
        "{\"icaoId\":\"KOAK\",\"lat\":37.72,\"lon\":-122.22,\"elev\":2,\"rawOb\":\"KOAK 301553Z 28010KT 10SM CLR 17/11 A3002\"},"
        "{\"icaoId\":\"BAD\",\"lat\":1,\"lon\":2}]";
    std::string err;
    auto st = parseAwcMetarJson(awc, &err);
    CHECK(st.size() == 2 && err.empty() && st[0].icao == "KSFO" && st[0].elevM == 3);
    CHECK(parseAwcMetarJson("", &err).empty() && err.empty());
    parseAwcMetarJson("<html>", &err);
    CHECK(!err.empty());

    std::string text;
    CHECK(parseDatisJson("[{\"airport\":\"KJFK\",\"type\":\"dep\",\"code\":\"B\",\"datis\":\"DEP TEXT\"},{\"airport\":\"KJFK\",\"type\":\"arr\",\"code\":\"B\",\"datis\":\"ARR TEXT\"}]", text) && text == "ARR TEXT");
    CHECK(!parseDatisJson("{\"error\":\"not found\"}", text));
    CHECK(awcMetarUrl(37.6, -122.4, 120).find("bbox=35.600,") != std::string::npos);

    // --- model: fog + low overcast at a 3 m elevation station, even though the "aircraft" is irrelevant here
    Config cfg;
    std::time_t now = std::time(nullptr);
    XpWx x = buildXp(st[0], cfg, now);
    CHECK(x.fogLayerApplied && x.cloudCount == 1);
    CHECK(x.clouds[0].type == 1.0f && x.clouds[0].coverage == 1.0f);
    NEAR(x.clouds[0].baseM, 5, 0.1);
    NEAR(x.clouds[0].topM, 3 + 0.3048 * 200, 0.1); // VV 200 ft sets the fog depth
    NEAR(x.visM, 402.3, 1);
    NEAR(x.qnhPa, 3001 / 100.0 * 33.8639 * 100, 30);
    CHECK(x.hasWind && x.windMs == 0);
    XpWx clr = buildXp(st[1], cfg, now);
    CHECK(clr.cloudCount == 0 && !clr.fogLayerApplied && clr.visM == cfg.clearVisM);
    NEAR(clr.windMs, 10 * kKtToMs, 0.01);

    Station dfw;
    dfw.elevM = 185;
    parseReport("KDFW 301553Z 18015G30KT 3SM +TSRA BKN020CB OVC040 BKN150 BKN250 24/22 A2985", dfw.report);
    XpWx t = buildXp(dfw, cfg, now);
    CHECK(t.cloudCount == 3 && t.clouds[0].type == 3.0f); // 3 slots, lowest first

    // --- ATIS merge overrides wind/altimeter but keeps unspecified fields
    Station s = st[1];
    Report atis;
    parseReport("KOAK ATIS INFO C 1600Z 30012G20KT 6SM HZ 18/11 A2995", atis, true);
    mergeAtis(s, atis, "x");
    CHECK(s.hasAtis && s.atisLetter == "C" && s.report.windDir == 300 && s.report.gustKt == 20);
    NEAR(s.report.altimHpa, 29.95 * 33.8639, 0.1);
    CHECK(s.report.haze);

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
