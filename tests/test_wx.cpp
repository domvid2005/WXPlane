#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "../src/json_mini.h"
#include "../src/metar.h"
#include "../src/sources.h"
#include "../src/windsaloft.h"
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
    CHECK(clr.cloudCount == 0 && !clr.fogLayerApplied);
    NEAR(clr.visM, cfg.clearVisM, 1); // 10 SM or more -> clear_vis_m (40 km), as before
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


    // --- visibility: exact below 10 SM, clear_vis_m for "10 SM or more"
    {
        Station v; v.elevM = 0;
        parseReport("KXXX 301500Z 00000KT 10SM CLR 20/10 A3000", v.report);
        NEAR(buildXp(v, cfg, now).visM, 40000, 1);
        parseReport("EGLL 301500Z 00000KT 9999 CLR 20/10 Q1013", v.report);
        NEAR(buildXp(v, cfg, now).visM, 40000, 1);
        parseReport("LFPG 301500Z 00000KT CAVOK 20/10 Q1013", v.report);
        NEAR(buildXp(v, cfg, now).visM, 40000, 1);
        parseReport("KXXX 301500Z 00000KT 6SM HZ CLR 20/10 A3000", v.report);
        NEAR(buildXp(v, cfg, now).visM, 6 * kSmToM, 1);
        parseReport("KDEN 301553Z 18008KT 4SM BR FEW008 BKN250 08/07 A3015", v.report);
        NEAR(buildXp(v, cfg, now).visM, 4 * kSmToM, 1);   // the KDEN case: 4 SM stays 6437 m
        Report ar; parseReport("KDEN ATIS INFO B 1553Z 18008KT 4SM BR FEW008 BKN250 08/07 A3015 (THREE ZERO ONE FIVE). ILS RWY 16R.", ar, true);
        CHECK(ar.hasVis && ar.mist);
        mergeAtis(v, ar, "x");
        NEAR(buildXp(v, cfg, now).visM, 4 * kSmToM, 1);
    }

    // --- NOAA winds / temps aloft (FD winds)
    const std::string fd =
        "000\nFBUS31 KWNO 301359\nFD1US1\nDATA BASED ON 301200Z\nVALID 301800Z   FOR USE 1400-2100Z. TEMPS NEG ABV 24000\n\n"
        "FT  3000    6000    9000   12000   18000   24000  30000  34000  39000\n"
        "ABI 2420    2625+11 2731+06 2834+00 2938-16 3047-30 305945 305552 305157\n"
        "DEN             9900+05 2215-02 2420-08 2530-20 2640-33 265648 266452 267254\n"
        "SEA      0314+02 2820+00 3122-04 3227-15 3135-28 303743 304354 761253\n";
    auto al = parseWindTemp(fd);
    CHECK(al.size() == 3 && al[0].id == "ABI" && al[0].levels.size() == 9);
    CHECK(al[0].levels[0].altFt == 3000 && al[0].levels[0].dirDeg == 240 && al[0].levels[0].speedKt == 20 && !al[0].levels[0].hasTemp);
    CHECK(al[0].levels[1].hasTemp && al[0].levels[1].tempC == 11);
    CHECK(al[0].levels[4].dirDeg == 290 && al[0].levels[4].speedKt == 38 && al[0].levels[4].tempC == -16);
    CHECK(al[0].levels[6].altFt == 30000 && al[0].levels[6].speedKt == 59 && al[0].levels[6].tempC == -45); // implied minus
    CHECK(al[1].levels.size() == 8 && al[1].levels[0].altFt == 6000 && al[1].levels[0].speedKt == 0); // light & variable, 3000 ft under terrain
    CHECK(al[2].levels.size() == 8 && al[2].levels.back().altFt == 39000);
    CHECK(al[2].levels.back().dirDeg == 260 && al[2].levels.back().speedKt == 112 && al[2].levels.back().tempC == -53); // >=100 kt coding
    CHECK(parseWindTemp("garbage\n<html>").empty());

    {   // blending: two stations either side of a point; closer one dominates
        std::vector<AloftStation> st2(2);
        st2[0].id = "AAA"; st2[0].lat = 40; st2[0].lon = -100; st2[0].located = true;
        st2[1].id = "BBB"; st2[1].lat = 40; st2[1].lon = -97;  st2[1].located = true;
        AloftLevel a; a.altFt = 30000; a.hasWind = a.hasTemp = true; a.dirDeg = 270; a.speedKt = 100; a.tempC = -40;
        AloftLevel b = a; b.dirDeg = 270; b.speedKt = 50; b.tempC = -50;
        st2[0].levels = {a}; st2[1].levels = {b};
        auto nearA = sampleAloft(st2, 40, -99.5, 250);
        auto nearB = sampleAloft(st2, 40, -97.5, 250);
        CHECK(nearA.size() == 1 && nearB.size() == 1);
        CHECK(nearA[0].speedKt > 85 && nearB[0].speedKt < 65 && nearA[0].dirDeg > 269 && nearA[0].dirDeg < 271);
        CHECK(nearA[0].tempC < -40 && nearA[0].tempC > -45);
        CHECK(sampleAloft(st2, 10, -50, 250).empty()); // nothing in range -> sim keeps its own data
        // opposite winds must cancel through the vector blend, not average the compass numbers
        st2[1].levels[0].dirDeg = 90; st2[1].levels[0].speedKt = 100;
        auto mid = sampleAloft(st2, 40, -98.5, 250);
        CHECK(mid.size() == 1 && mid[0].speedKt < 60);
    }

    {   // model: aloft data lands on the METAR station, surface layer first, temps anchored to the METAR
        std::vector<AloftStation> st2(1);
        st2[0] = al[0]; st2[0].lat = 32.4; st2[0].lon = -99.7; st2[0].located = true;
        Station kabi; kabi.icao = "KABI"; kabi.lat = 32.41; kabi.lon = -99.68; kabi.elevM = 546;
        parseReport("KABI 301553Z 24012G20KT 10SM CLR 30/10 A2990", kabi.report);
        XpWx xa = buildXp(kabi, cfg, now, &st2);
        CHECK(xa.aloft && xa.maxAltFt == 60000 && xa.winds.size() >= 8);
        NEAR(xa.winds.back().altMsl, 60000 * kFtToM, 1);       // top NOAA wind is held up to FL600
        NEAR(xa.winds.back().ms, xa.winds[xa.winds.size() - 2].ms, 0.001);
        CHECK(xa.winds[0].altMsl < 570 && xa.winds[0].gustMs > xa.winds[0].ms);
        for (size_t i = 1; i < xa.winds.size(); ++i) CHECK(xa.winds[i].altMsl > xa.winds[i - 1].altMsl);
        CHECK(xa.winds.size() <= 13);
        CHECK(xa.tempProfile.size() >= 6 && xa.tempProfile[0].second == 30);       // METAR surface temp anchors the profile
        NEAR(profileTemp(xa.tempProfile, 546), 30, 0.01);
        NEAR(profileTemp(xa.tempProfile, 9144), -45, 0.5);                           // 30 000 ft
        CHECK(profileTemp(xa.tempProfile, 20000) == profileTemp(xa.tempProfile, 12000)); // constant above the top level
        Config off = cfg; off.useAloft = false;
        XpWx xo = buildXp(kabi, off, now, &st2);
        CHECK(!xo.aloft && xo.maxAltFt == 10000 && xo.winds.size() == 1);
    }

    CHECK(windTempUrl("all", "low", 6) == "https://aviationweather.gov/api/data/windtemp?region=all&level=low&fcst=06");
    { std::string rg; char pf; aloftRegionFor(61.2, -149.9, rg, pf); CHECK(rg == "alaska" && pf == 'P'); aloftRegionFor(21.3, -157.9, rg, pf); CHECK(rg == "hawaii"); aloftRegionFor(40, -100, rg, pf); CHECK(rg == "all" && pf == 'K'); }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
