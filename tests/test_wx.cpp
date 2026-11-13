#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "../src/geo.h"
#include "../src/json_mini.h"
#include "../src/metar.h"
#include "../src/radar.h"
#include "../src/sources.h"
#include "../src/windshear.h"
#include "../src/viewer.h"
#include "../src/windsaloft.h"
#include "../src/wxmodel.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool nr(double a, double b, double t) { return std::fabs(a - b) <= t; }
#define NEAR(a, b, t) CHECK(std::fabs((double)(a) - (double)(b)) <= (t))

using namespace wx;
#include "test_turb.h"
#include "test_hazards.h"
#include "test_opera.h"
#include "test_loadclock.h"

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
    CHECK(a.hasTemp && a.tempC == 8 && a.dewpC == 7);
    CHECK(parseReport("KXYZ ATIS INFO C 0153Z WIND 090 AT 5 TEMPERATURE MINUS 3, DEW POINT M7 ALTIMETER 3011", a, true));
    CHECK(a.windDir == 90 && a.windKt == 5 && a.gustKt <= 0 && a.tempC == -3 && a.dewpC == -7);
    {   // a very long token (junk in a feed) is no coded group and never reaches the regex matcher (it recursed on the stack)
        const std::string junk(20000, '9');
        CHECK(parseReport("KXYZ 121651Z 27012KT " + junk + " 10SM OVC020 05/02 A2990", a, false));
        CHECK(a.hasWind && a.windDir == 270 && a.hasVis && a.hasTemp);
    }

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
    NEAR(x.clouds[0].baseM, 3, 0.1);                 // starts at the field elevation
    NEAR(x.clouds[0].topM, 3 + 120, 0.1); // VV 200 ft (61 m) is below the 120 m minimum, so 120 m
    NEAR(x.visM, 402.3, 1);
    NEAR(x.qnhPa, 3001 / 100.0 * 33.8639 * 100, 30);
    CHECK(x.hasWind && x.windMs == 0);
    XpWx clr = buildXp(st[1], cfg, now);
    CHECK(clr.cloudCount == 0 && !clr.fogLayerApplied);
    NEAR(clr.visM, cfg.clearVisM, 1); // 10 SM or more -> clear_vis_m (40 km), as before
    {   // 8 SM (12874 m) is below 10 SM: exact, not "clear"
        Station v8; v8.elevM = 100;
        parseReport("KSEA 302153Z 04007KT 8SM -RA FEW007 BKN022 08/07 A2991", v8.report);
        NEAR(buildXp(v8, cfg, now).visM, 8 * 1609.344, 5);
        Station v9; v9.elevM = 100;
        parseReport("EDDF 302153Z 04007KT 9999 FEW007 08/07 Q1015", v9.report);
        NEAR(buildXp(v9, cfg, now).visM, cfg.clearVisM, 1);
        Station v10; v10.elevM = 100;
        parseReport("KSEA 302153Z 04007KT 10SM FEW007 08/07 A2991", v10.report);
        NEAR(buildXp(v10, cfg, now).visM, cfg.clearVisM, 1);
    }
    NEAR(clr.windMs, 10 * kKtToMs, 0.01);

    {   // cloud types: low overcast and overcast aloft look like stratus, a low broken deck like stratocumulus, the rest cumulus
        Config tc;
        Station s1; s1.elevM = 100; parseReport("KXXX 301553Z 18005KT 10SM OVC008 10/08 A2990", s1.report);
        Station s2; s2.elevM = 100; parseReport("KXXX 301553Z 18005KT 10SM BKN010 10/08 A2990", s2.report);
        Station s3; s3.elevM = 100; parseReport("KXXX 301553Z 18005KT 10SM SCT030 10/08 A2990", s3.report);
        Station s4; s4.elevM = 100; parseReport("KXXX 301553Z 18005KT 10SM FEW020TCU 10/08 A2990", s4.report);
        auto lowest = [](const XpWx& w) { for (const auto& c : w.clouds) if (c.coverage > 0) return c.type; return -1.0f; };
        CHECK(lowest(buildXp(s1, tc, now)) == 1.5f && lowest(buildXp(s2, tc, now)) == 1.75f && lowest(buildXp(s3, tc, now)) == 2.0f && lowest(buildXp(s4, tc, now)) == 2.5f);
        // cirrus from the model's high cloud: added under CLR (an automatic station sees only 12,000 ft), not under SKC, not when the
        // report has a high layer of its own
        std::vector<AloftStation> mdl(1);
        mdl[0].model = true; mdl[0].located = true; mdl[0].lat = 38.0; mdl[0].lon = -98.0; mdl[0].highCloud = 0.6;
        Station c1; c1.lat = 38.1; c1.lon = -98.1; c1.elevM = 400; parseReport("KXXX 301553Z 18005KT 10SM CLR 10/08 A2990", c1.report);
        Station c2 = c1; parseReport("KXXX 301553Z 18005KT 10SM SKC 10/08 A2990", c2.report);
        Station c3 = c1; parseReport("KXXX 301553Z 18005KT 10SM SCT030 BKN250 10/08 A2990", c3.report);
        XpWx w1 = buildXp(c1, tc, now, &mdl), w2 = buildXp(c2, tc, now, &mdl), w3 = buildXp(c3, tc, now, &mdl);
        int ci = -1; for (int k = 0; k < 3; ++k) if (w1.clouds[k].coverage > 0 && w1.clouds[k].type == 0.0f) ci = k;
        CHECK(ci >= 0 && w1.modelCirrus && std::fabs(w1.clouds[ci].coverage - 0.51f) < 1e-3 && w1.clouds[ci].topM - w1.clouds[ci].baseM <= 549.0f && w1.clouds[ci].baseM >= 6000.0f);
        CHECK(!w2.modelCirrus && w2.cloudCount == 0);
        CHECK(!w3.modelCirrus);
        mdl[0].highCloud = 0.05; CHECK(!buildXp(c1, tc, now, &mdl).modelCirrus);   // hardly any high cloud: none
        Config off; off.modelCirrus = false; mdl[0].highCloud = 0.9; CHECK(!buildXp(c1, off, now, &mdl).modelCirrus);
    }
    {   // Clouds from the global model's cloud cover on pressure levels (2026-11-09)
        Config tc;
        std::vector<AloftStation> mdl(1);
        mdl[0].model = true; mdl[0].located = true; mdl[0].lat = 38.0; mdl[0].lon = -98.0;
        // a mid-level deck at 500-400 hPa (~18,300-23,700 ft) and cirrus at 250 hPa (~34,000 ft); nothing low
        mdl[0].cloudLevels = {{2500, 0.0}, {5000, 0.1}, {10000, 0.05}, {14000, 0.1}, {18300, 0.7}, {23700, 0.45}, {30000, 0.1}, {34000, 0.6}, {38700, 0.2}};
        Station c1; c1.lat = 38.1; c1.lon = -98.1; c1.elevM = 400; parseReport("KXXX 301553Z 18005KT 10SM CLR 10/08 A2990", c1.report);
        XpWx w = buildXp(c1, tc, now, &mdl);
        std::vector<XpCloud> got; for (const auto& c : w.clouds) if (c.coverage > 0) got.push_back(c);
        CHECK(got.size() == 2 && w.modelCirrus);
        if (got.size() == 2) {
            CHECK(got[0].type == (float)tc.stratusCloudType && got[0].baseM > 5200 && got[0].baseM < 5600 && got[0].topM - got[0].baseM <= 1500.0f);
            NEAR(got[0].coverage, 0.595, 1e-3);   // 0.7 x 0.85
            CHECK(got[1].type == 0.0f && got[1].baseM > 10000 && got[1].topM - got[1].baseM <= 549.0f);
        }
        // never below 12,000 ft above the station, never under or into the reported layers, not under SKC
        Station c2 = c1; parseReport("KXXX 301553Z 18005KT 10SM SKC 10/08 A2990", c2.report);
        CHECK(!buildXp(c2, tc, now, &mdl).modelCirrus);
        Station c3 = c1; parseReport("KXXX 301553Z 18005KT 10SM SCT030 BKN200 OVC250 10/08 A2990", c3.report);   // all three slots taken
        CHECK(!buildXp(c3, tc, now, &mdl).modelCirrus);
        mdl[0].cloudLevels = {{8000, 0.9}, {11000, 0.9}};   // only low/mid cloud below 12,000 ft AGL: left to the report
        CHECK(!buildXp(c1, tc, now, &mdl).modelCirrus);
        Config off; off.modelClouds = false; off.modelCirrus = false;
        mdl[0].cloudLevels = {{18300, 0.9}};
        CHECK(!buildXp(c1, off, now, &mdl).modelCirrus);

        // Open-Meteo's answer for two mid-Atlantic points (real, 2026-11-10 04Z): a low stratocumulus deck at 925 hPa, drizzle
        const char* om = R"([{"latitude":45.04393,"longitude":-35.039062,"elevation":0.0,"hourly":{"time":[1794283200,1794286800],"cloud_cover_1000hPa":[5,5],"geopotential_height_1000hPa":[53.00,52.00],"cloud_cover_925hPa":[92,98],"geopotential_height_925hPa":[705.00,704.00],"cloud_cover_850hPa":[8,40],"geopotential_height_850hPa":[1403.00,1403.00],"cloud_cover_700hPa":[0,0],"geopotential_height_700hPa":[2971.00,2972.00],"cloud_cover_600hPa":[0,0],"geopotential_height_600hPa":[4190.00,4191.00],"cloud_cover_500hPa":[0,0],"geopotential_height_500hPa":[5589.00,5589.00],"cloud_cover_400hPa":[0,0],"geopotential_height_400hPa":[7222.22,7223.46],"cloud_cover_300hPa":[0,0],"geopotential_height_300hPa":[9214.52,9214.52],"cloud_cover_250hPa":[0,0],"geopotential_height_250hPa":[10419.05,10417.14],"cloud_cover_200hPa":[0,0],"geopotential_height_200hPa":[11853.49,11848.84],"cloud_cover_150hPa":[0,0],"geopotential_height_150hPa":[13692.54,13686.57],"temperature_2m":[14.6,14.5],"dew_point_2m":[12.8,12.9],"pressure_msl":[1006.3,1006.2],"wind_speed_10m":[18.2,18.3],"wind_direction_10m":[340,345],"wind_gusts_10m":[20.2,20.0],"visibility":[24140.00,24140.00],"precipitation":[0.10,0.10],"snowfall":[0.00,0.00],"weather_code":[51,51]}},{"latitude":45.981125,"longitude":-35.039062,"elevation":0.0,"location_id":1,"hourly":{"time":[1794283200,1794286800],"cloud_cover_1000hPa":[5,4],"geopotential_height_1000hPa":[52.00,53.00],"cloud_cover_925hPa":[54,27],"geopotential_height_925hPa":[703.00,703.00],"cloud_cover_850hPa":[8,4],"geopotential_height_850hPa":[1401.00,1401.00],"cloud_cover_700hPa":[0,0],"geopotential_height_700hPa":[2971.00,2971.00],"temperature_2m":[13.7,13.6],"dew_point_2m":[12.2,11.9],"pressure_msl":[1006.2,1006.3],"wind_speed_10m":[16.7,17.4],"wind_direction_10m":[351,350],"wind_gusts_10m":[19.2,19.0],"visibility":[24140.00,24140.00],"precipitation":[0.00,0.00],"snowfall":[0.00,0.00],"weather_code":[3,3]}}])";
        std::vector<std::pair<double, double>> pts = {{45, -35}, {46, -35}};
        std::string err;
        auto mp = parseOpenMeteoClouds(om, pts, 1794283300, &err);
        CHECK(mp.size() == 2 && mp[0].sfc && mp[0].cloudLevels.size() == 11 && mp[1].cloudLevels.size() == 4);
        if (mp.size() == 2) {
            CHECK(mp[0].lat == 45 && mp[0].lon == -35);   // the requested node, not the model's snapped cell
            NEAR(mp[0].cloudLevels[1].first, 705.0 / 0.3048, 0.1); NEAR(mp[0].cloudLevels[1].second, 0.92, 1e-9);
            NEAR(mp[0].mslHpa, 1006.3, 1e-9); NEAR(mp[0].windKt, 18.2, 1e-9); CHECK(mp[0].weatherCode == 51);
            Station os = modelStation(mp[0], (std::time_t)1794283300);
            CHECK(os.icao == "GFSN45W035" && os.report.hasWind && os.report.windDir == 340 && os.report.windKt == 18 && os.report.gustKt == 0);
            CHECK(os.report.clouds.size() == 1 && os.report.clouds[0].cover == CloudGroup::OVC && os.report.clouds[0].baseFtAgl == 1300);
            CHECK(os.report.hasAltim && nr(os.report.altimHpa, 1006.3, 1e-9) && os.report.visM == 9999 && os.report.precip > 0 && os.report.wxText == "-RA");
            CHECK(reportAgeSeconds(os.report, (std::time_t)1794283300) < 120);
            XpWx ow = buildXp(os, tc, (std::time_t)1794283300, &mdl);
            CHECK(!ow.modelCirrus && ow.cloudCount >= 1);   // its own layers, no second copy from the model
        }
        CHECK(parseOpenMeteoClouds("{\"error\":true,\"reason\":\"Too many requests\"}", pts, 0, &err).empty() && err.find("Too many") != std::string::npos);
    }
    Station dfw;
    dfw.elevM = 185;
    parseReport("KDFW 301553Z 18015G30KT 3SM +TSRA BKN020CB OVC040 BKN150 BKN250 24/22 A2985", dfw.report);
    XpWx t = buildXp(dfw, cfg, now);
    CHECK(t.cloudCount == 3 && t.clouds[0].type == 3.0f); // 3 slots, lowest first
    NEAR(t.clouds[2].baseM, 185 + 15000 * 0.3048, 1);      // BKN150 and BKN250 are not joined: the highest broken deck is the one left out
    // layers never overlap: each top stays below the next layer's base
    CHECK(t.clouds[0].topM < t.clouds[1].baseM && t.clouds[1].topM < t.clouds[2].baseM);
    CHECK(t.clouds[0].topM > t.clouds[0].baseM && t.clouds[1].topM > t.clouds[1].baseM);

    // --- config migration: an old file (no config_version) takes the new start-up defaults once, other settings stay
    {
        const char* fn = "wxplane_cfg_test.cfg";
        { FILE* f = std::fopen(fn, "w"); std::fputs("update_minutes = 5\nstartup_delay_s = 15\nstartup_staged = 1\nvis_autocal = 1\ntransition_s = 90\napply_per_tick = 4\nsearch_nm = 90\n", f); std::fclose(f); }
        Config old; loadConfig(fn, old);
        CHECK(old.updateMinutes == 5 && old.startupDelayS == 0 && !old.startupStaged && old.visAutocal && old.transitionS == 0 && old.applyPerTick == 500 && old.searchNm == 90);
        { FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 22\nupdate_minutes = 10\nstartup_staged = 1\nlow_cloud_type = 2\nstation_radius_factor = 0.65\n", f); std::fclose(f); }
        Config cur; loadConfig(fn, cur);
        CHECK(cur.updateMinutes == 10 && cur.startupStaged && cur.lowCloudType == 2.0 && cur.stationRadiusFactor == 0.65);   // current files keep their values
        { FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 6\nlow_cloud_type = 2\nstation_radius_factor = 0.65\nfill_radius_factor = 0.95\nradar_minutes = 30\nsearch_nm = 80\n", f); std::fclose(f); }
        Config v6; loadConfig(fn, v6);
        { Config r25; FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 25\nregion_winds = 0\n", f); std::fclose(f); loadConfig(fn, r25); CHECK(r25.regionWinds); }
        { Config r26; FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 26\nregion_winds = 0\n", f); std::fclose(f); loadConfig(fn, r26); CHECK(!r26.regionWinds); }
        CHECK(v6.lowCloudType == 2.0 && v6.stationRadiusFactor == 0.49 && v6.fillRadiusFactor == 1.0 && v6.radarMinutes == 5 && v6.searchNm == 80);   // stale cloud settings from earlier builds reset once
        { FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 18\ntop_bumps = 1\nbump_count = 30\nupdate_minutes = 10\nsearch_nm = 77\n", f); std::fclose(f); }
        Config v18; loadConfig(fn, v18);
        CHECK(!v18.topBumps && v18.bumpCount == 10 && v18.updateMinutes == 5 && v18.searchNm == 77);   // rough tops switched off once; other settings kept
        { FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 22\ntop_bumps = 1\nbump_count = 30\n", f); std::fclose(f); }
        Config v19; loadConfig(fn, v19);
        CHECK(v19.topBumps && v19.bumpCount == 30);   // a later opt-in is respected
        { FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 21\nradar_cell_nm = 6\nradar_max_cells = 150\nsearch_nm = 77\n", f); std::fclose(f); }
        Config v20; loadConfig(fn, v20);
        CHECK(v20.radarCellNm == 12 && v20.radarMaxCells == 60 && v20.stormCentroid && v20.stormRim && v20.searchNm == 77);   // 12 nm storm blocks taken once (v22)
        { FILE* f = std::fopen(fn, "w"); std::fputs("config_version = 22\nradar_cell_nm = 8\nradar_max_cells = 100\n", f); std::fclose(f); }
        Config v21; loadConfig(fn, v21);
        CHECK(v21.radarCellNm == 8 && v21.radarMaxCells == 100);   // and a later choice is kept
        { Config d; CHECK(!d.topBumps); }
        std::remove(fn);
    }

    // --- NOAA decoded-field crosscheck, neighbour fill and smooth blending
    {
        std::string js = "[{\"icaoId\":\"KTST\",\"rawOb\":\"KTST 301553Z 18010KT 5SM BKN020 15/10 A2992\",\"lat\":30.0,\"lon\":-97.0,\"elev\":100,"
                         "\"visib\":3,\"wdir\":200,\"wspd\":14,\"wgst\":22,\"temp\":16,\"dewp\":10,\"altim\":1013.2},"
                         "{\"icaoId\":\"KTS2\",\"rawOb\":\"KTS2 301553Z 18010KT 10SM 15/10 A2992\",\"lat\":30.5,\"lon\":-97.0,\"elev\":100,"
                         "\"visib\":\"10+\",\"wdir\":180,\"wspd\":10,\"temp\":15,\"dewp\":10,\"altim\":1012.6,\"clouds\":[{\"cover\":\"BKN\",\"base\":2500}]},"
                         "{\"icaoId\":\"KTS3\",\"rawOb\":\"KTS3 301553Z 18010KT 4SM BKN020 15/10 A2992\",\"lat\":30.2,\"lon\":-97.0,\"elev\":100,"
                         "\"visib\":4,\"wdir\":180,\"wspd\":10,\"temp\":15,\"dewp\":10,\"altim\":1012.6}]";
        std::string e2;
        auto v = parseAwcMetarJson(js, &e2);
        CHECK(v.size() == 3);
        NEAR(v[0].report.visM, 3 * 1609.344, 2);                                  // text said 5 SM, NOAA's decoded field says 3 SM
        CHECK(v[0].report.windKt == 14 && v[0].report.gustKt == 22 && v[0].report.windDir == 200);
        CHECK(v[0].filled.find("visibility corrected") != std::string::npos && v[0].filled.find("wind corrected") != std::string::npos);
        CHECK(v[1].report.clouds.size() == 1 && v[1].report.clouds[0].cover == CloudGroup::BKN && v[1].report.clouds[0].baseFtAgl == 2500);
        CHECK(v[1].filled.find("clouds filled") != std::string::npos);
        CHECK(v[2].filled.empty());                                               // agrees with NOAA: nothing touched
        // neighbour fill: a station with no wind or pressure
        Station gap; gap.icao = "KGAP"; gap.lat = 30.1; gap.lon = -97.0; parseReport("KGAP 301553Z 10SM CLR 20/10", gap.report);
        std::vector<Station> g = {v[0], v[2], gap};
        int filled = fillMissing(g, 80.0);
        CHECK(filled >= 2 && g[2].report.hasWind && g[2].report.hasAltim && g[2].filled.find("wind from") != std::string::npos);
        // high cloud from a manned report spreads to automatic neighbours (OVC200 at KBOS was a 6 nm circle of overcast cirrus)
        {
            auto mk = [](const char* id, double lat, double lon, double elevM, const char* raw) { Station x; x.icao = id; x.lat = lat; x.lon = lon; x.elevM = elevM; x.rawMetar = raw; parseReport(raw, x.report); return x; };
            std::vector<Station> h = {
                mk("KBOS", 42.36, -71.01, 6, "KBOS 120054Z 03006KT 10SM OVC200 10/03 A2988"),
                mk("KBED", 42.47, -71.29, 40, "KBED 120052Z AUTO 00000KT 10SM CLR 08/02 A2989"),          // 14 nm: same cover
                mk("KORH", 42.27, -71.88, 300, "KORH 120054Z AUTO 31005KT 10SM CLR 07/01 A2990"),         // 39 nm: one step thinner
                mk("KPVD", 41.72, -71.43, 17, "KPVD 120051Z AUTO 32004KT 10SM FEW050 09/02 A2989"),       // 42 nm, low cloud only
                mk("KEWB", 41.68, -70.96, 24, "KEWB 120053Z AUTO 00000KT 10SM CLR 08/03 A2988"),          // 41 nm
                mk("KCON", 43.20, -71.50, 104, "KCON 120051Z AUTO 00000KT 10SM CLR 03/M01 A2990"),        // 55 nm: two steps thinner
                mk("KXSK", 42.40, -71.10, 10, "KXSK 120053Z 00000KT 10SM SKC 08/02 A2989"),               // SKC: an observer saw nothing
                mk("KMAN", 42.30, -71.20, 30, "KMAN 120053Z 00000KT 10SM FEW030 08/02 A2989")};           // manned, low cloud only
            const int n = spreadHighCloud(h, 80.0);
            auto high = [](const Station& x) { for (const auto& g : x.report.clouds) if (g.baseFtAgl >= 12000) return g.cover; return -1; };
            CHECK(n == 5);
            CHECK(high(h[1]) == CloudGroup::OVC && h[1].filled.find("high cloud from KBOS") != std::string::npos);
            CHECK(high(h[2]) == CloudGroup::BKN && high(h[3]) == CloudGroup::BKN && high(h[4]) == CloudGroup::BKN && high(h[5]) == CloudGroup::SCT);
            CHECK(high(h[6]) == -1 && high(h[7]) == -1 && h[0].report.clouds.size() == 1);
            for (const auto& g : h[2].report.clouds) if (g.baseFtAgl >= 12000) CHECK(std::abs(g.baseFtAgl - (20000 + 20 - 984)) <= 2);   // same height above sea level
            CHECK(h[3].report.clouds.size() == 2 && h[3].report.clouds[0].baseFtAgl == 5000);
            CHECK(!h[1].report.clearSky);
        }
        // smooth blend: the closest station dominates and values change gradually between two stations
        std::vector<Station> two = {v[2], v[1]};          // 4 SM at lat 30.2, clear-ish at 30.5
        two[1].report.visM = 16093; two[0].report.visM = 6437;
        double nearA = blendAt(two, 30.21, -97.0).report.visM, mid = blendAt(two, 30.35, -97.0).report.visM, nearB = blendAt(two, 30.49, -97.0).report.visM;
        CHECK(nearA < mid && nearB >= 16000);   // values at or above 10 SM count as clear
        CHECK(std::fabs(nearA - 6437) < 0.15 * (16093 - 6437));
        CHECK(blendAt(two, 30.21, -97.0).hasAtis == false);
    }

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
        // "10 km or more" in precipitation or under a low ceiling is capped (2026-11-11, LEMD 2130Z: 40 km under drizzle)
        v.elevM = 610;
        parseReport("LEMD 112130Z 16012KT 130V190 9999 -DZ SCT010 BKN030 BKN055 14/13 Q0995 NOSIG", v.report);
        NEAR(buildXp(v, cfg, now).visM, 20000, 1);
        parseReport("LEMD 111900Z 12005KT 9999 RA FEW008 BKN010 OVC020 12/12 Q0997", v.report);
        NEAR(buildXp(v, cfg, now).visM, 15000, 1);
        parseReport("LEMD 112230Z 16009KT 9999 +DZRA SCT020 BKN030 14/13 Q0995", v.report);
        NEAR(buildXp(v, cfg, now).visM, 12000, 1);        // "+" applies to the whole group: heavy drizzle and rain
        parseReport("EGLL 301500Z 00000KT 9999 +RA BKN012 10/09 Q1003", v.report);
        NEAR(buildXp(v, cfg, now).visM, 12000, 1);
        parseReport("EFHK 301500Z 00000KT 9999 -SN BKN015 M02/M03 Q1003", v.report);
        NEAR(buildXp(v, cfg, now).visM, 12000, 1);
        parseReport("KSEA 301500Z 20010KT 10SM -RA BKN040 11/09 A2999", v.report);
        NEAR(buildXp(v, cfg, now).visM, 20000, 1);        // the US "10 SM or more" too
        parseReport("EGLL 301500Z 00000KT 9999 OVC008 10/09 Q1013", v.report);
        NEAR(buildXp(v, cfg, now).visM, 20000, 1);        // dry, ceiling below 1,000 ft
        parseReport("EGLL 301500Z 00000KT 9999 BKN015 10/09 Q1013", v.report);
        NEAR(buildXp(v, cfg, now).visM, 25000, 1);        // dry, ceiling below 2,000 ft
        parseReport("EGLL 301500Z 00000KT 9999 SCT008 BKN030 10/09 Q1013", v.report);
        NEAR(buildXp(v, cfg, now).visM, 40000, 1);        // scattered low cloud, ceiling at 3,000 ft: unchanged
        parseReport("LEMD 112000Z 15006KT 6000 RA SCT007 OVC025 13/12 Q0996", v.report);
        NEAR(buildXp(v, cfg, now).visM, 6000, 1);         // a measured value is never touched
        Config nocap = cfg; nocap.visCapWeather = false;
        parseReport("LEMD 112130Z 16012KT 9999 -DZ SCT010 BKN030 BKN055 14/13 Q0995", v.report);
        NEAR(buildXp(v, nocap, now).visM, 40000, 1);
        v.elevM = 0;
        Report ar; parseReport("KDEN ATIS INFO B 1553Z 18008KT 4SM BR FEW008 BKN250 08/07 A3015 (THREE ZERO ONE FIVE). ILS RWY 16R.", ar, true);
        CHECK(ar.hasVis && ar.mist);
        mergeAtis(v, ar, "x");
        NEAR(buildXp(v, cfg, now).visM, 4 * kSmToM, 1);
    }

    // --- cloud layers never overlap vertically: every top is at least 150 m below the next base (at least 40 m between decks
    //     reported closer than that: they share the space and keep their bases)
    {
        const char* raws[] = {"KTST 301553Z 18008KT 10SM SCT020 BKN022 OVC024 20/10 A3002",
                              "KTST 301553Z 18008KT 10SM FEW030 SCT032 BKN035 20/10 A3002",
                              "KTST 301553Z 18008KT 3SM BR OVC002 BKN003 OVC005 12/11 A3002"};
        for (const char* raw : raws) {
            Station st; st.elevM = 100;
            parseReport(raw, st.report);
            XpWx a = buildXp(st, cfg, now);
            bool ok = a.cloudCount >= 2;
            for (int i = 0; i + 1 < 3; ++i)
                if (a.clouds[i].coverage > 0 && a.clouds[i + 1].coverage > 0)
                    ok = ok && a.clouds[i].topM + (a.clouds[i].baseM - 100.0f < 100.0f || a.clouds[i + 1].baseM - a.clouds[i].baseM < 270.0f ? 39.0f : 149.0f) <= a.clouds[i + 1].baseM && a.clouds[i].topM > a.clouds[i].baseM;
            CHECK(ok);
        }
    }
    // --- storm tops: lone or much-taller-than-neighbours tops are tamed (spikes), ordinary clusters are untouched
    {
        std::vector<RadarCell> v(5);
        for (int i = 0; i < 4; ++i) { v[i].lat = 35.0 + 0.1 * i; v[i].lon = -97.0; v[i].topM = 10000; }   // a cluster at ~10 km
        v[3].topM = 15000;                                    // one spike in the cluster
        v[4].lat = 40.0; v[4].lon = -90.0; v[4].topM = 14000; // a lone block far away
        smoothStormTops(v, 12.0);
        CHECK(v[0].topM == 10000 && v[1].topM == 10000);
        CHECK(v[3].topM <= 12500 && v[3].topM > 10000);
        CHECK(v[4].topM == 8000);
    }
    // --- corner cut: open sides shrink a block's circle and add taper; the interior block of a 3x3 keeps full size
    {
        std::vector<RadarCell> v;
        const double dl = 12.0 / 60.0, dn = dl / std::cos(35.0 * 3.14159265 / 180.0);
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) { RadarCell c; c.lat = 35.0 + j * dl; c.lon = -97.0 + i * dn; v.push_back(c); }
        markStormEdges(v, 12.0);
        CHECK(v[4].radiusScale == 1.0f && v[4].edge == 0.0f);              // centre: all 8 sides covered
        CHECK(v[0].radiusScale < 0.85f && v[0].edge >= 0.6f);              // corner: 5 of 8 sides open
        CHECK(v[1].radiusScale < 1.0f && v[1].radiusScale > v[0].radiusScale);   // side block: 3 open sides, less than a corner
        std::vector<RadarCell> one(1); markStormEdges(one, 12.0);
        CHECK(one[0].radiusScale <= 0.75f + 1e-4f && one[0].edge == 1.0f);   // lone block: fully open
    }
    // --- pits: a low block ringed by tall ones is lifted (unless switched off); a low block at the edge of a cluster is left alone
    {
        std::vector<RadarCell> v;
        const double dl = 12.0 / 60.0, dn = dl / std::cos(35.0 * 3.14159265 / 180.0);
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) { RadarCell c; c.lat = 35.0 + j * dl; c.lon = -97.0 + i * dn; c.topM = (i == 0 && j == 0) ? 3500.0f : 11000.0f; v.push_back(c); }
        std::vector<RadarCell> off = v;
        smoothStormTops(off, 12.0, 0.0, 500.0, false);
        smoothStormTops(v, 12.0, 0.0, 500.0, true);
        CHECK(off[4].topM == 3500 && v[4].topM == 11000);   // lifted to the exact mean of the ring around it
        CHECK(v[0].topM >= 10000 && v[8].topM >= 10000);   // corners keep (nearly) their height; the existing spike limit may trim a little
        std::vector<RadarCell> e = v; e[0].topM = 3500;      // a low corner block has only 3 neighbours: not a pit
        smoothStormTops(e, 12.0, 0.0, 500.0, true);
        CHECK(e[0].topM == 3500);
    }
    // --- optional storm-top smoothing: a blend pulls neighbouring tops together; 0 leaves ordinary clusters exactly as before
    {
        auto row = [](float a, float b) { std::vector<RadarCell> v(2); v[0].lat = 35.0; v[0].lon = -97.0; v[0].topM = a; v[1].lat = 35.0; v[1].lon = -96.8; v[1].topM = b; return v; };
        auto r0 = row(8000, 10000), r1 = row(8000, 10000);
        smoothStormTops(r0, 12.0, 0.0, 500.0);
        smoothStormTops(r1, 12.0, 0.4, 250.0);
        CHECK(r0[0].topM == 8000 && r0[1].topM == 10000);          // blend 0 = unchanged
        CHECK(r1[1].topM - r1[0].topM < 2000 && r1[0].topM > 8000 && r1[1].topM < 10000);   // the 2 km step is reduced
        CHECK(std::fmod(r1[0].topM, 250.0f) == 0 && std::fmod(r1[1].topM, 250.0f) == 0);
    }
    // --- holes inside a storm are interpolated from the surrounding cells; edge notches and far-away gaps are not
    {
        std::vector<RadarCell> ring;
        const double lat0 = 35.0, lon0 = -97.0, dl = 12.0 / 60.0, dn = dl / std::cos(lat0 * 3.14159265 / 180.0);
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) {
                if ((i == 0 && j == 0) || (i == 2 && j == 2)) continue;     // centre hole, and a missing corner (an edge notch)
                RadarCell c; c.lat = lat0 + j * dl; c.lon = lon0 + i * dn; c.dbz = 50; c.frac = 1; c.topM = 11000; c.topKnown = true; c.rateMmH = 30;
                char k[24]; std::snprintf(k, sizeof k, "R%d:%d", j + 9, i + 9); c.key = k;
                ring.push_back(c);
            }
        std::vector<RadarCell> none;
        const size_t before = ring.size();
        int n = fillStormHoles(ring, none, 12.0, lat0, lon0, 500.0, 20.0, 5, 10);
        CHECK(n == 1 && ring.size() == before + 1);
        const RadarCell& h = ring.back();
        CHECK(std::fabs(h.lat - lat0) < 0.01 && std::fabs(h.lon - lon0) < 0.01 && h.dbz == 50 && h.topM == 11000 && h.topKnown && !h.key.empty());
        // a hole that only the cap removed keeps its own key
        std::vector<RadarCell> ring2;
        for (const auto& c : ring) if (!(std::fabs(c.lat - lat0) < 0.01 && std::fabs(c.lon - lon0) < 0.01)) ring2.push_back(c);
        std::vector<RadarCell> drop(1); drop[0].lat = lat0; drop[0].lon = lon0; drop[0].dbz = 25; drop[0].key = "R9:9"; drop[0].topM = 5000;
        CHECK(fillStormHoles(ring2, drop, 12.0, lat0, lon0, 500.0, 20.0, 5, 10) == 1 && ring2.back().key == "R9:9" && ring2.back().dbz == 50);
        {   // a one-block-wide channel between two columns of blocks, open at both ends, is filled; the outside of a single column is not
            std::vector<RadarCell> cols;
            for (int j = -2; j <= 2; ++j) for (int i : {-1, 1}) {
                RadarCell c; c.lat = lat0 + j * dl; c.lon = lon0 + i * dn; c.dbz = 45; c.dbzFine = 45; c.frac = 1; c.topM = 10000; c.topKnown = true; c.rateMmH = 20;
                char k[24]; std::snprintf(k, sizeof k, "R%d:%d", j + 9, i + 9); c.key = k; cols.push_back(c);
            }
            std::vector<RadarCell> one = cols; one.erase(std::remove_if(one.begin(), one.end(), [&](const RadarCell& c) { return c.lon > lon0; }), one.end());
            const size_t b0 = cols.size();
            const int nc = fillStormHoles(cols, none, 12.0, lat0, lon0, 500.0, 20.0, 5, 30);
            CHECK(nc >= 3 && cols.size() == b0 + (size_t)nc);
            bool mid = true; for (size_t i = b0; i < cols.size(); ++i) mid = mid && std::fabs(cols[i].lon - lon0) < dn * 0.6;
            CHECK(mid);                                                       // every added cell sits in the channel, none outside
            CHECK(fillStormHoles(one, none, 12.0, lat0, lon0, 500.0, 20.0, 5, 30) == 0);
        }
        // the budget is respected, and a lone pair of cells has no holes
        std::vector<RadarCell> r3 = ring; std::vector<RadarCell> r4(2); r4[1].lat = 35.2;
        CHECK(fillStormHoles(r3, none, 12.0, lat0, lon0, 500.0, 20.0, 5, 0) == 0);
        CHECK(fillStormHoles(r4, none, 12.0, lat0, lon0, 500.0, 20.0, 5, 10) == 0);
    }
    // --- stations and fill tiles inside a storm take its rain (no round dry spots); far away they get none
    {
        std::vector<RadarCell> v(2);
        v[0].lat = 35.0; v[0].lon = -97.0; v[0].rateMmH = 40; v[1].lat = 35.0; v[1].lon = -96.8; v[1].rateMmH = 40;
        CHECK(stormPrecipAt(v, 35.0, -96.9, 12.0, 40.0f) > 0.95f);
        CHECK(stormPrecipAt(v, 36.5, -97.0, 12.0, 40.0f) == 0.0f);
        v[1].rateMmH = 4;   // a lighter neighbour pulls the mean down but never below the weakest cell's value
        const float m = stormPrecipAt(v, 35.0, -96.9, 12.0, 40.0f);
        CHECK(m > 0.09f && m < 1.0f);
    }
    // --- rain smoothing: neighbouring cells' reflectivity is pulled together; blend 0 changes nothing
    {
        std::vector<RadarCell> v(2);
        v[0].lat = 35.0; v[0].lon = -97.0; v[0].dbzFine = 30; v[1].lat = 35.0; v[1].lon = -96.9; v[1].dbzFine = 50;
        std::vector<RadarCell> w = v;
        smoothStormRain(w, 6.0, 0.0);
        CHECK(w[0].dbzFine == 30 && w[1].dbzFine == 50);
        smoothStormRain(w, 6.0, 0.4);
        CHECK(w[0].dbzFine > 30 && w[1].dbzFine < 50 && w[1].dbzFine - w[0].dbzFine < 20 && w[0].rateMmH > 0);
    }
    // --- ragged rim: an L of three confirmed blocks gets lighter, smaller, uneven cells around its outside and a full cell in its inner corner
    {
        const double lat0 = 35.0, lon0 = -97.0, dl = 12.0 / 60.0, dn = dl / std::cos(lat0 * 3.14159265 / 180.0);
        std::vector<RadarCell> L(3);
        const int ji[3][2] = {{0, 0}, {0, 1}, {1, 0}};
        for (int q = 0; q < 3; ++q) {
            L[q].lat = lat0 + ji[q][0] * dl; L[q].lon = lon0 + ji[q][1] * dn; L[q].dbz = 50; L[q].dbzFine = 50; L[q].frac = 1; L[q].topM = 11000; L[q].topKnown = true; L[q].rateMmH = 30;
            L[q].key = "R" + std::to_string(q);
        }
        std::vector<RadarCell> a = L, b = L;
        const int na = addStormRim(a, 12.0, lat0, lon0, 500.0, 20.0, 20, 1), nb = addStormRim(b, 12.0, lat0, lon0, 500.0, 20.0, 20, 1);
        CHECK(na >= 6 && na <= 20 && na == nb);                                          // some, not all, directions; stable between builds
        bool same = true, tapered = true, small = false, corner = false, lumpy = true;
        for (int i = 0; i < na; ++i) {
            const RadarCell& r = a[3 + i]; const RadarCell& r2 = b[3 + i];
            same = same && r.key == r2.key && r.lat == r2.lat && r.lon == r2.lon && r.radiusScale == r2.radiusScale;
            CHECK(r.synthetic && r.key[0] == 'Q' && r.dbzFine >= 20 && r.dbzFine <= 50 && r.rateMmH > 0 && r.radiusScale >= 0.5f && r.radiusScale <= 1.05f);
            if (r.radiusScale < 0.9f) { small = true; tapered = tapered && r.dbzFine < 50 && r.topM < 11000 && r.frac < 1.0f; }
            else corner = true;
            for (int j = 0; j < 3; ++j) lumpy = lumpy && wx::distNm(r.lat, r.lon, L[j].lat, L[j].lon) > 12.0 * 0.69;   // never on top of a confirmed block
        }
        CHECK(same && tapered && small && corner && lumpy);
        {   // a second ring fades further out: smaller, lighter, lower than the first, never on top of a block or another cell
            std::vector<RadarCell> r2 = L;
            const int n2 = addStormRim(r2, 12.0, lat0, lon0, 500.0, 20.0, 60, 2);
            CHECK(n2 > na && r2.size() == 3 + (size_t)n2);
            bool outer = false, fades = true, apart = true;
            for (int i = 0; i < n2; ++i) {
                const RadarCell& r = r2[3 + i];
                if (r.key.size() > 1 && r.key[1] == '2') { outer = true; fades = fades && r.radiusScale <= 0.86f && r.dbzFine < 50 && r.topM < 11000; }
                for (int j = 0; j < 3; ++j) apart = apart && wx::distNm(r.lat, r.lon, L[j].lat, L[j].lon) > 12.0 * 0.49;
            }
            CHECK(outer && fades && apart);
            bool thin = true; for (int i = 0; i < n2; ++i) thin = thin && r2[3 + i].coverScale <= 0.85f && r2[3 + i].coverScale >= 0.3f;
            CHECK(thin);
        }
        std::vector<RadarCell> c0 = L;
        CHECK(addStormRim(c0, 12.0, lat0, lon0, 500.0, 20.0, 0) == 0 && c0.size() == 3);   // budget 0
    }
    // --- cloud coverage: each cover in its range (FEW 10-25, SCT 35-50, BKN 70-85, OVC 97-100 %), steady, and leaning to the heavy end
    //     next to heavier sky, to the light end next to lighter or clear sky
    {
        auto mk = [](const char* icao, double lat, double lon, const char* metar) {
            Station s; s.icao = icao; s.lat = lat; s.lon = lon; s.elevM = 10; parseReport(metar, s.report); return s;
        };
        const char* covers[] = {"FEW030", "SCT030", "BKN030", "OVC030"};
        const float lo[] = {0.10f, 0.35f, 0.70f, 0.97f}, hi[] = {0.25f, 0.50f, 0.85f, 1.00f};
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < 40; ++i) {
                char id[8]; std::snprintf(id, sizeof id, "K%03d", i);
                const Station s = mk(id, 40, -74, (std::string(id) + " 131251Z 27010KT 10SM " + covers[c] + " 10/05 A3000").c_str());
                const float cv = buildXp(s, cfg, now).clouds[0].coverage;
                CHECK(cv >= lo[c] - 1e-4f && cv <= hi[c] + 1e-4f);
                CHECK(buildXp(s, cfg, now).clouds[0].coverage == cv);   // the same every time
            }
        // a broken layer amid overcast at that height leans heavy; amid clear sky, light (the same station both times)
        const Station bkn = mk("KJFK", 40.64, -73.78, "KJFK 131251Z 27010KT 10SM BKN030 10/05 A3000");
        std::vector<Station> heavy = {bkn, mk("KLGA", 40.78, -73.87, "KLGA 131251Z 27010KT 10SM OVC031 10/05 A3000"),
                                      mk("KEWR", 40.69, -74.17, "KEWR 131251Z 27010KT 10SM OVC029 10/05 A3000")};
        std::vector<Station> light = {bkn, mk("KLGA", 40.78, -73.87, "KLGA 131251Z 27010KT 10SM CLR 10/05 A3000"),
                                      mk("KEWR", 40.69, -74.17, "KEWR 131251Z 27010KT 10SM FEW030 10/05 A3000")};
        const float alone = buildXp(bkn, cfg, now).clouds[0].coverage;
        const float h = buildXp(bkn, cfg, now, nullptr, false, &heavy).clouds[0].coverage;
        const float l = buildXp(bkn, cfg, now, nullptr, false, &light).clouds[0].coverage;
        CHECK(h > alone && l < alone && h >= 0.80f && l <= 0.75f && h <= 0.85f && l >= 0.70f);
        // overcast elsewhere (far away, or at another height) does not count
        std::vector<Station> distant = {bkn, mk("KBOS", 42.36, -71.01, "KBOS 131251Z 27010KT 10SM OVC030 10/05 A3000"),
                                    mk("KLGA", 40.78, -73.87, "KLGA 131251Z 27010KT 10SM OVC250 10/05 A3000")};
        CHECK(buildXp(bkn, cfg, now, nullptr, false, &distant).clouds[0].coverage < alone);   // KLGA is clear at 3000 ft: lighter
    }
    // --- more decks than X-Plane's three slots: never joined; the lightest (and of those the highest) deck is left out, never a storm
    {
        Station st; st.icao = "KBOS"; st.lat = 42.36; st.lon = -71.01; st.elevM = 6;
        parseReport("KBOS 131654Z 36008KT 10SM FEW018 SCT030 OVC048 04/01 A2959", st.report);
        XpWx a = buildXp(st, cfg, now, nullptr, false);   // three decks, three slots
        CHECK(a.cloudCount == 3 && a.clouds[2].coverage >= 0.97f);
        XpWx b = buildXp(st, cfg, now, nullptr, true);    // slot 0 kept for fog nearby: two slots, FEW018 left out
        CHECK(b.cloudCount == 2 && b.clouds[0].coverage == 0.0f);
        NEAR(b.clouds[1].baseM, 6 + 3000 * 0.3048, 1);    // SCT030 as reported (not joined with FEW018)
        CHECK(b.clouds[1].coverage >= 0.35f && b.clouds[1].coverage <= 0.50f);
        CHECK(b.clouds[2].coverage >= 0.97f);               // the overcast is there
        NEAR(b.clouds[2].baseM, 6 + 4800 * 0.3048, 1);
        Station many; many.icao = "KXYZ"; many.elevM = 0;
        parseReport("KXYZ 131654Z 36008KT 10SM FEW010 SCT020 BKN035 BKN050 OVC080 04/01 A2959", many.report);
        XpWx m = buildXp(many, cfg, now, nullptr, false);
        CHECK(m.cloudCount == 3);
        NEAR(m.clouds[0].baseM, 3500 * 0.3048, 1);          // FEW010 and SCT020 left out: the broken and overcast decks stay
        NEAR(m.clouds[1].baseM, 5000 * 0.3048, 1);
        CHECK(m.clouds[2].coverage >= 0.97f);
    }
    // --- mist with a low ceiling: the ceiling stays where it is reported and all three decks are kept
    {
        Station st; st.elevM = 132;
        parseReport("KSEA 301739Z 28003KT 1SM BR BKN001 BKN050 BKN120 06/06 A3001", st.report);
        XpWx a = buildXp(st, cfg, now, nullptr, true);        // fog slot layout, as when fog is reported nearby
        CHECK(!a.fogLayerApplied && a.cloudCount == 3);
        NEAR(a.clouds[0].baseM, 132 + 100 * 0.3048, 1);       // BKN001 at 100 ft AGL, not lifted above a mist layer
        NEAR(a.clouds[1].baseM, 132 + 5000 * 0.3048, 1);
        NEAR(a.clouds[2].baseM, 132 + 12000 * 0.3048, 1);
        NEAR(a.visM, 1609, 2);
        Station fg; fg.elevM = 132;
        parseReport("KSEA 301739Z 28003KT 1/4SM FG BKN002 BKN050 06/06 A3001", fg.report);
        XpWx b = buildXp(fg, cfg, now, nullptr, true);
        CHECK(b.fogLayerApplied && b.clouds[1].baseM < b.clouds[0].topM + 41.0f);   // only a 40 m gap above the ground fog, not 150 m
    }
    // --- cloud tops from the global model: a reported deck reaches the model's top where the model has cloud at its base
    {
        std::vector<std::pair<float, float>> p = {{100, 0.0f}, {700, 0.9f}, {1400, 0.8f}, {3000, 0.1f}, {4200, 0.0f}};
        NEAR(modelCloudTopM(p, 610), 2200, 1);                 // halfway between the last cloudy level (1400) and the next (3000)
        CHECK(modelCloudTopM(p, 3500) < 0);                    // no cloud at that base in the model
        std::vector<AloftStation> al(1);
        al[0].model = true; al[0].located = true; al[0].lat = 40.0; al[0].lon = -100.0;
        al[0].cloudLevels = {{328, 0.0}, {2297, 0.9}, {4593, 0.8}, {9843, 0.1}, {13780, 0.0}};   // the same profile in ft
        Station st; st.elevM = 0; st.lat = 40.0; st.lon = -100.0;
        parseReport("KTST 301553Z 18005KT 10SM BKN020 12/05 A3002", st.report);
        Config mc = cfg; mc.modelClouds = false;
        const XpWx m = buildXp(st, mc, now, &al);
        NEAR(m.clouds[0].baseM, 610, 1);
        NEAR(m.clouds[0].topM, 2200, 5);                       // the model's top, not the rule's 610 + 1300 m
        al[0].cloudLevels = {{328, 0.0}, {2297, 0.0}, {4593, 0.0}, {9843, 0.0}, {13780, 0.0}};   // the model has no cloud there
        const XpWx r = buildXp(st, mc, now, &al);
        NEAR(r.clouds[0].topM - r.clouds[0].baseM, 1300, 1);   // the rule decides
        mc.modelTops = false;
        al[0].cloudLevels = {{328, 0.0}, {2297, 0.9}, {4593, 0.8}, {9843, 0.1}, {13780, 0.0}};
        NEAR(buildXp(st, mc, now, &al).clouds[0].topM - 610, 1300, 1);   // model_tops = 0: the rule
        // steady rain: the model raises a radar top that reads low; a storm keeps the radar's
        XpWx s; s.modelProfM = {{500, 0.9f}, {1500, 0.9f}, {4000, 0.9f}, {6000, 0.1f}};
        RadarCell c; c.dbz = 30; c.dbzFine = 30; c.rateMmH = 3; c.stratiform = true; c.topM = 2500; c.topKnown = true; c.lat = 40.0;
        XpWx s1 = s; applyStorm(s1, c, cfg, 0.0, 40.0, 800.0f);
        float t1 = 0; for (const auto& k : s1.clouds) t1 = std::max(t1, k.topM);
        NEAR(t1, 5000, 1);
        c.stratiform = false; c.dbz = c.dbzFine = 45;
        XpWx s2 = s; applyStorm(s2, c, cfg, 0.0, 40.0, 800.0f);
        float t2 = 0; for (const auto& k : s2.clouds) t2 = std::max(t2, k.topM);
        NEAR(t2, 2500, 1);
    }
    // --- steady rain goes into the station's broken/overcast deck: no layer of its own, every deck keeps its slot and base (KCDW)
    {
        XpWx w;
        w.clouds[0].type = 2.0f; w.clouds[0].coverage = 0.80f; w.clouds[0].baseM = 662; w.clouds[0].topM = 1030;
        w.clouds[1].type = 1.5f; w.clouds[1].coverage = 0.98f; w.clouds[1].baseM = 1180; w.clouds[1].topM = 2478;
        w.clouds[2].type = 1.5f; w.clouds[2].coverage = 0.32f; w.clouds[2].baseM = 4038; w.clouds[2].topM = 4438;
        w.cloudCount = 3;
        RadarCell c; c.dbz = 25; c.dbzFine = 25; c.rateMmH = 1; c.stratiform = true; c.topM = 6000; c.topKnown = true; c.lat = 40.8;
        XpWx a = w; applyStorm(a, c, cfg, 50.0, 40.8);
        CHECK(a.cloudCount == 3 && a.precip > 0);
        CHECK(a.clouds[0].baseM == 662 && a.clouds[0].topM == 1030 && a.clouds[0].coverage == 0.80f);   // the broken deck as it was
        CHECK(a.clouds[1].baseM == 1180 && a.clouds[1].coverage >= 0.98f);                              // the overcast takes the rain
        NEAR(a.clouds[1].topM, 4038 - 150, 1);                                                         // raised, clear of the layer above
        CHECK(a.clouds[2].baseM == 4038 && a.clouds[2].coverage == 0.32f);
        // KSMF: heavy steady rain under an overcast of type 1.5 at 2,141 m with a 7.2 km radar top. Sent 5,084 m deep (the cumulus
        // limit) at type 1.5, X-Plane dropped the layer; the deck now becomes cumulus, and 1.5 has a limit between stratus and cumulus
        NEAR(xpMaxThicknessM(1.5f), 0.5f * (kXpMaxThickM[1] + kXpMaxThickM[2]), 1);
        {
            XpWx k; k.clouds[0].type = 1.5f; k.clouds[0].coverage = 0.97f; k.clouds[0].baseM = 2141; k.clouds[0].topM = 3541; k.cloudCount = 1;
            RadarCell h = c; h.dbz = h.dbzFine = 35; h.topM = 7225;
            applyStorm(k, h, cfg, 9.0, 38.7);
            CHECK(k.clouds[0].type == 2.0f && k.clouds[0].baseM == 2141 && k.clouds[0].topM > 5000);
            CHECK(k.clouds[0].topM - k.clouds[0].baseM <= xpMaxThicknessM(k.clouds[0].type) + 1.0f);
        }
        RadarCell cb = c; cb.stratiform = false; cb.dbz = cb.dbzFine = 50;                              // a thunderstorm still builds its own cloud
        XpWx b = w; applyStorm(b, cb, cfg, 50.0, 40.8);
        bool tall = false; for (const auto& k : b.clouds) tall = tall || (k.type >= 2.9f && k.topM > 5000);
        CHECK(tall);
    }
    // --- close decks keep their reported bases (KEWR BKN016 BKN020 OVC035: BKN020 went out at 2,490 ft); they share the space between
    {
        Station st; st.elevM = 3;
        parseReport("KEWR 131951Z 35012G20KT 10SM -RA BKN016 BKN020 OVC035 06/03 A2965", st.report);
        const XpWx a = buildXp(st, cfg, now);
        NEAR(a.clouds[0].baseM, 3 + 1600 * 0.3048, 1);
        NEAR(a.clouds[1].baseM, 3 + 2000 * 0.3048, 1);
        NEAR(a.clouds[2].baseM, 3 + 3500 * 0.3048, 1);
        CHECK(a.clouds[0].topM <= a.clouds[1].baseM - 40.0f && a.clouds[0].topM >= a.clouds[0].baseM + 40.0f);
        CHECK(a.clouds[1].topM <= a.clouds[2].baseM - 40.0f);
    }
    // --- mist (BR) is shown by the visibility alone: no ground layer, no fog slot, and the deck keeps slot 0
    {
        Station st; st.elevM = 400;
        parseReport("KICT 301553Z 00000KT 3SM BR FEW002 12/11 A3002", st.report);
        CHECK(!stationHasFog(st, cfg));
        XpWx a = buildXp(st, cfg, now);
        CHECK(!a.fogLayerApplied && a.cloudCount == 1);
        NEAR(a.clouds[0].baseM, 400 + 200 * 0.3048, 1);       // FEW002 is the only layer
        NEAR(a.visM, 4828, 5);                                // the 3 SM is still sent exactly
        Station clear; clear.elevM = 400;
        parseReport("KICT 301553Z 00000KT 10SM FEW040 12/05 A3002", clear.report);
        CHECK(!stationHasFog(clear, cfg));
    }
    // --- fog adds haze: visibility capped, air saturated, layer starts at the ground and is not razor-thin
    {
        Station f; f.elevM = 1655;
        parseReport("KDEN 301553Z 18008KT 9999 BCFG FEW008 08/05 A3015", f.report);
        XpWx a = buildXp(f, cfg, now);
        NEAR(a.visM, 3000, 1);                                 // patchy fog caps a "clear" 9999
        CHECK(a.hasDewp && a.dewpC == a.tempC);                // saturated
        CHECK(a.fogLayerApplied && a.clouds[0].baseM == 1655 && a.clouds[0].topM - a.clouds[0].baseM >= 100);
        parseReport("KDEN 301553Z 00000KT 1/4SM FG VV002 05/05 A3015", f.report);
        CHECK(buildXp(f, cfg, now).visM < 450);
        parseReport("KDEN 301553Z 00000KT 5SM BR 08/07 A3015", f.report);
        NEAR(buildXp(f, cfg, now).visM, 5 * kSmToM, 1);        // mist / haze keep the reported value
        parseReport("KDEN 301553Z 00000KT 6SM HZ 25/05 A3015", f.report);
        NEAR(buildXp(f, cfg, now).visM, 6 * kSmToM, 1);
        parseReport("KDEN 301553Z 00000KT 3/8SM FG OVC001 05/05 A3015", f.report);
        NEAR(buildXp(f, cfg, now).visM, 0.375 * kSmToM, 2);     // never raises a lower reported value
        parseReport("KDEN 301553Z 00000KT 2SM FG OVC001 05/05 A3015", f.report);
        NEAR(buildXp(f, cfg, now).visM, 1000, 1);               // FG with a larger number is inconsistent -> 1 km
        Config nohaze = cfg; nohaze.fogHaze = false;
        parseReport("KDEN 301553Z 18008KT 9999 BCFG 08/05 A3015", f.report);
        NEAR(buildXp(f, nohaze, now).visM, 40000, 1);
    }

    // --- fog slot: slot 0 is always fog (or empty) when any station has fog, so neighbours blend fog-with-fog
    {
        Station fogS, clearS;
        fogS.elevM = clearS.elevM = 100;
        parseReport("KAAA 301553Z 00000KT 1/4SM FG OVC001 08/08 A3010", fogS.report);
        parseReport("KBBB 301553Z 18010KT 10SM FEW030 BKN080 15/05 A3010", clearS.report);
        CHECK(stationHasFog(fogS, cfg) && !stationHasFog(clearS, cfg));
        const Config& cfgNc = cfg;
        XpWx f = buildXp(fogS, cfgNc, now, nullptr, true), c = buildXp(clearS, cfgNc, now, nullptr, true);
        CHECK(f.clouds[0].type == 1.0f && f.clouds[0].coverage == 1.0f && f.clouds[0].topM - f.clouds[0].baseM >= 120);
        CHECK(c.clouds[0].coverage == 0);                          // reserved, empty
        CHECK(c.clouds[1].coverage > 0 && c.clouds[2].coverage > 0 && c.cloudCount == 2);
        CHECK(f.clouds[1].coverage > 0 && f.clouds[2].coverage == 0);  // OVC001 in slot 1
        XpWx c2 = buildXp(clearS, cfgNc, now, nullptr, false);       // no fog anywhere: normal packing, nothing reserved
        CHECK(c2.clouds[0].coverage > 0 && c2.cloudCount == 2);
        CHECK(describeXp(f, 100).find("FOG stratus") != std::string::npos);
        CHECK(describeXp(c, 100).find("vis 40000 m") != std::string::npos);
    }

    // --- radar
    {
        CHECK(nr(rateFromDbz(20), 0.65, 0.05) && nr(rateFromDbz(40), 11.5, 0.8) && rateFromDbz(50) > 50 && rateFromDbz(50) < 75);
        CHECK(rateFromDbz(2) == 0 && rateFromDbz(80) <= 200);
        CHECK(nr(tropopauseKm(0), 17, 0.01) && nr(tropopauseKm(35), 11.05, 0.01) && nr(tropopauseKm(80), 9, 0.01));
        CHECK(nr(estimateTopM(55, 35), 11050, 1) && estimateTopM(25, 35) > 3000 && estimateTopM(25, 35) < 5000);
        CHECK(mrmsRegionsFor(39, -98).size() == 1 && mrmsRegionsFor(39, -98)[0].empty());
        CHECK(mrmsRegionsFor(21, -157)[0] == "HAWAII" && mrmsRegionsFor(61, -150)[0] == "ALASKA" && mrmsRegionsFor(18, -66)[0] == "CARIB");
        CHECK(mrmsRegionsFor(13.5, 144.8)[0] == "GUAM" && mrmsRegionsFor(50, 8).empty());
        CHECK(mrmsUrl("", "EchoTop_18") == "https://mrms.ncep.noaa.gov/2D/EchoTop_18/MRMS_EchoTop_18.latest.grib2.gz");
        CHECK(mrmsUrl("GUAM", "X") == "https://mrms.ncep.noaa.gov/2D/GUAM/X/MRMS_X.latest.grib2.gz");

        // synthetic mosaic: 100 x 100 pixels (1 x 1 degree) with a 50 dBZ core and a weak 25 dBZ patch, missing codes elsewhere
        GribField refl, tops;
        refl.ni = refl.nj = tops.ni = tops.nj = 100;
        refl.lat0 = tops.lat0 = 38.495; refl.lon0 = tops.lon0 = -98.495;
        refl.dLat = refl.dLon = tops.dLat = tops.dLon = 0.01;
        refl.v.assign(10000, -99.0f); tops.v.assign(10000, -3.0f);
        for (int j = 40; j < 60; ++j) for (int i = 40; i < 60; ++i) { refl.v[(size_t)j * 100 + i] = 50.0f; tops.v[(size_t)j * 100 + i] = 12.0f; }
        for (int j = 5; j < 15; ++j) for (int i = 5; i < 15; ++i) refl.v[(size_t)j * 100 + i] = 25.0f;
        for (int j = 80; j < 90; ++j) for (int i = 80; i < 90; ++i) refl.v[(size_t)j * 100 + i] = 12.0f;   // below threshold
        {   // widespread rain over the whole range: the farthest squares are left out so the picture near the aircraft still gets split
            GribField wide = refl;
            wide.ni = wide.nj = 600; wide.lat0 = 41.0; wide.lon0 = -101.5; wide.v.assign(360000, 25.0f);
            for (int j = 290; j < 310; ++j) for (int i = 290; i < 310; ++i) wide.v[(size_t)j * 600 + i] = 50.0f;   // a core under the aircraft
            Config wc; wc.radarRangeNm = 150; wc.radarMinDbz = 20; wc.radarNaturalMax = 150;
            RadarResult w = buildRadarCells(wide, nullptr, 38.0, -98.5, wc);
            float wmx = 0, wmin = 99;
            for (const auto& c : w.cells) { wmx = std::max(wmx, c.dbzFine); wmin = std::min(wmin, c.sizeNm); }
            std::printf("  wide rain: ok=%d natural=%d cells=%zu dry=%zu min %.1f nm max %.0f dBZ note: %s\n", (int)w.ok, (int)w.natural, w.cells.size(), w.dryCells.size(), wmin, wmx, w.note.c_str());
            CHECK(w.ok && w.note.find("left out to keep detail") != std::string::npos);
            CHECK(wmin < 20.0f && wmx >= 45.0f);   // split near the aircraft, and the core keeps its strength
            CHECK((int)(w.cells.size() + w.dryCells.size()) <= wc.radarNaturalMax);
        }
        {   // uniform rain with an airport under it: the square over the airport is split down to 6 nm (one record, one airport's decks)
            GribField flat = refl;
            flat.ni = flat.nj = 200; flat.lat0 = 39.0; flat.lon0 = -99.5; flat.v.assign(40000, 25.0f);
            Config fc; fc.radarRangeNm = 60; fc.radarMinDbz = 20;
            std::vector<Station> ap(1);
            ap[0].icao = "KTST"; ap[0].lat = 38.25; ap[0].lon = -98.65;
            RadarResult f0 = buildRadarCells(flat, nullptr, 38.0, -98.5, fc), f1 = buildRadarCells(flat, nullptr, 38.0, -98.5, fc, nullptr, nullptr, &ap);
            const RadarCell *a0 = naturalCellAt(f0, ap[0].lat, ap[0].lon), *a1 = naturalCellAt(f1, ap[0].lat, ap[0].lon);
            std::printf("  airport under uniform rain: square %.0f nm without the airport, %.0f nm with it\n", a0 ? a0->sizeNm : -1.0f, a1 ? a1->sizeNm : -1.0f);
            CHECK(a0 && a1 && a0->sizeNm > 6.5f && a1->sizeNm <= 6.01f);
        }
        {   // natural picture (the default): fine records along the edges, dry ones around, stable between runs
            Config nc; nc.radarRangeNm = 100; nc.radarMinDbz = 20;
            RadarResult n1 = buildRadarCells(refl, &tops, 38.0, -98.0, nc), n2 = buildRadarCells(refl, &tops, 38.0, -98.0, nc);
            CHECK(n1.ok && n1.natural && !n1.cells.empty() && !n1.dryCells.empty());
            CHECK((int)(n1.cells.size() + n1.dryCells.size()) <= nc.radarNaturalMax);
            bool allNat = true, same = n1.cells.size() == n2.cells.size(); float mx = 0, minSize = 99;
            for (size_t i = 0; i < n1.cells.size(); ++i) {
                allNat = allNat && n1.cells[i].natural && n1.cells[i].dbzFine >= 20;
                mx = std::max(mx, n1.cells[i].dbzFine); minSize = std::min(minSize, n1.cells[i].sizeNm);
                if (same) same = n1.cells[i].key == n2.cells[i].key && n1.cells[i].dbzFine == n2.cells[i].dbzFine;
            }
            CHECK(allNat && same);
            CHECK(mx >= 46 && mx <= 52);                     // the 50 dBZ core keeps its strength
            CHECK(minSize <= 3.01f);                         // fine records near the aircraft
            const RadarCell* core = naturalCellAt(n1, 38.05, -98.05);
            CHECK(core && core->dbzFine >= 45 && core->topKnown && core->topM >= 11000);
            CHECK(radarCovers(n1, 38.05, -98.05) && !radarCovers(n1, 36.0, -98.0));
            CHECK(naturalCellAt(n1, 37.65, -97.65) == nullptr);   // the 12 dBZ patch is no rain
            std::printf("  natural picture: %zu rain + %zu dry records, strongest %.0f dBZ, smallest %.1f nm\n", n1.cells.size(), n1.dryCells.size(), mx, minSize);
        }
        Config rc; rc.stormRim = false; rc.stormContour = false; rc.radarNatural = false;
        rc.radarCellNm = 10; rc.radarRangeNm = 100; rc.radarMaxCells = 100; rc.radarMinDbz = 20;
        RadarResult rr = buildRadarCells(refl, &tops, 38.0, -98.0, rc);
        CHECK(rr.ok && rr.topsFromRadar && rr.cells.size() >= 3 && rr.cells.size() <= 12);
        float maxD = 0, maxTop = 0; int weak = 0;
        for (auto& c : rr.cells) { maxD = std::max(maxD, c.dbz); maxTop = std::max(maxTop, c.topM); if (c.dbz < 30) { ++weak; CHECK(!c.topKnown && c.topM > 3000); } CHECK(c.rateMmH > 0 && !c.key.empty()); }
        CHECK(nr(maxD, 50, 0.01) && nr(maxTop, 12000, 1) && weak >= 1);
        CHECK(rr.cells[0].dbz > 40);                                 // strong and close sorts first
        for (auto& c : rr.cells) {
            CHECK(std::fmod(c.dbz, 5.0f) == 0 && std::fmod(c.topM, 500.0f) == 0 && std::fmod(c.frac * 4.0f, 1.0f) == 0);
        }
        // keys are tied to the block grid, not to the aircraft position
        RadarResult rr2 = buildRadarCells(refl, &tops, 38.1, -98.1, rc);
        std::string k1, k2; for (auto& c : rr.cells) if (c.dbz > 40) k1 += c.key + ";"; for (auto& c : rr2.cells) if (c.dbz > 40) k2 += c.key + ";";
        CHECK(!k1.empty() && k1 == k2);
        {   // cells sit at the weighted centre of their echo (not the block centre) and keep their keys; off = block centre
            Config cc = rc; cc.radarMaxCells = 100; cc.stormCentroid = true;
            Config nc = cc; nc.stormCentroid = false;
            RadarResult ca = buildRadarCells(refl, &tops, 38.0, -98.0, cc), cb = buildRadarCells(refl, &tops, 38.0, -98.0, nc);
            bool same = ca.cells.size() == cb.cells.size(), moved = false, near = true;
            for (size_t i = 0; same && i < ca.cells.size(); ++i) {
                same = same && ca.cells[i].key == cb.cells[i].key;
                const double d = wx::distNm(ca.cells[i].lat, ca.cells[i].lon, cb.cells[i].lat, cb.cells[i].lon);
                moved = moved || d > 0.05; near = near && d < cc.radarCellNm;
            }
            CHECK(same && moved && near);
        }
        rc.radarMaxCells = 2; RadarResult rr3 = buildRadarCells(refl, nullptr, 38.0, -98.0, rc);
        CHECK(rr3.cells.size() == 2 && rr3.echoBlocks >= 3 && !rr3.topsFromRadar && rr3.cells[0].topM > 3000 && !rr3.cells[0].topKnown);
        {   // estimated tops (no echo-top grid) vary per block by a fixed amount: not one plateau, stable between builds, bounded
            Config vc = rc; vc.radarMaxCells = 100; vc.stormSmoothTops = false; vc.stormTopVariationM = 1500;
            Config fc = vc; fc.stormTopVariationM = 0;
            RadarResult va = buildRadarCells(refl, nullptr, 38.0, -98.0, vc), vb = buildRadarCells(refl, nullptr, 38.0, -98.0, vc), vf = buildRadarCells(refl, nullptr, 38.0, -98.0, fc);
            bool same = va.cells.size() == vb.cells.size() && va.cells.size() == vf.cells.size(), bounded = true, differs = false;
            for (size_t i = 0; same && i < va.cells.size(); ++i) {
                same = same && va.cells[i].topM == vb.cells[i].topM;
                bounded = bounded && std::fabs(va.cells[i].topM - vf.cells[i].topM) <= 2000.0f;
                differs = differs || va.cells[i].topM != vf.cells[i].topM;
            }
            CHECK(same && bounded && differs);
        }
        {   // measured tops: a finer step gives non-500 m heights; variation for measured tops is opt-in, stable and bounded
            Config sc = rc; sc.radarMaxCells = 100; sc.stormSmoothTops = false;
            Config fine = sc; fine.stormTopStepM = 100;
            Config jit = sc; jit.stormTopVariationKnownM = 400; jit.stormTopStepM = 100;
            Config base100 = sc; base100.stormTopStepM = 100;
            RadarResult b0 = buildRadarCells(refl, &tops, 38.0, -98.0, base100), b1 = buildRadarCells(refl, &tops, 38.0, -98.0, fine);
            RadarResult j1 = buildRadarCells(refl, &tops, 38.0, -98.0, jit), j2 = buildRadarCells(refl, &tops, 38.0, -98.0, jit);
            bool okStep = !b1.cells.empty(), okJit = b0.cells.size() == j1.cells.size() && j1.cells.size() == j2.cells.size(), anyDiff = false;
            for (auto& c : b1.cells) okStep = okStep && std::fmod(c.topM, 100.0f) == 0;
            for (size_t i = 0; okJit && i < j1.cells.size(); ++i) {
                okJit = okJit && j1.cells[i].topM == j2.cells[i].topM && std::fabs(j1.cells[i].topM - b0.cells[i].topM) <= 450.0f;
                anyDiff = anyDiff || j1.cells[i].topM != b0.cells[i].topM;
            }
            std::printf("  known-top test: step %d jitter %d diff %d (cells %zu/%zu)\n", (int)okStep, (int)okJit, (int)anyDiff, b1.cells.size(), j1.cells.size());
            CHECK(okStep && okJit && anyDiff);
        }
        rc.radarMaxCells = 100; rc.radarRangeNm = 20; RadarResult rr4 = buildRadarCells(refl, &tops, 38.0, -98.0, rc);
        CHECK(rr4.cells.size() < rr.cells.size());                   // range limit applies
        rc.radarRangeNm = 100; rc.radarMinDbz = 40; RadarResult rr5 = buildRadarCells(refl, &tops, 38.0, -98.0, rc);
        for (auto& c : rr5.cells) CHECK(c.dbz >= 40);

        // hysteresis: an 18 dBZ patch is ignored when new, but kept if it was a storm cell before
        {
            GribField r2 = refl;
            for (int j = 5; j < 15; ++j) for (int i = 5; i < 15; ++i) r2.v[(size_t)j * 100 + i] = 18.0f;
            Config hc; hc.radarNatural = false; hc.stormRim = false; hc.stormContour = false; hc.radarCellNm = 10; hc.radarRangeNm = 100; hc.radarMaxCells = 100; hc.radarMinDbz = 20;
            RadarResult fresh = buildRadarCells(r2, nullptr, 38.0, -98.0, hc, nullptr);
            std::set<std::string> prevKeys; for (auto& c : rr.cells) prevKeys.insert(c.key);
            RadarResult kept = buildRadarCells(r2, nullptr, 38.0, -98.0, hc, &prevKeys);
            CHECK(kept.cells.size() == fresh.cells.size() + 1);
        }

        // storm applied on a clear station
        Station cs; cs.elevM = 400;
        parseReport("KCCC 301553Z 18010KT 10SM FEW040 BKN090 25/10 A3010", cs.report);
        Config sc;
        XpWx base = buildXp(cs, sc, now, nullptr, true);
        RadarCell cell; cell.lat = 38; cell.lon = -98; cell.dbz = 52; cell.frac = 1; cell.topKnown = true; cell.topM = 13000; cell.rateMmH = rateFromDbz(52);
        XpWx st = base;
        applyStorm(st, cell, sc, 400, 38);
        CHECK(st.clouds[2].coverage > 0.7f && st.clouds[2].coverage <= 1.0f && st.clouds[2].type == 3.0f && st.clouds[2].topM > 11000 && st.clouds[2].topM <= tropopauseKm(38) * 1000 + 2500 + 1 && st.clouds[2].baseM < 1500);
        CHECK(st.precip >= 0.99f && st.visM <= 800 && st.cloudCount == 2 && st.maxAltFt >= st.clouds[2].topM / 0.3048 - 1);
        CHECK(st.turbulence > 0.5f && st.gustMs > st.windMs + 5);
        CHECK(st.clouds[0].coverage == 0);                           // slot 0 is left for fog
        {   // global model: ISA heights, parsing, and merging above the FD top
            CHECK(std::fabs(isaAltFtFromHpa(1013.25)) < 1 && std::fabs(isaAltFtFromHpa(500) - 18289.0) < 150);       // 500 hPa = 5574 m
            CHECK(std::fabs(isaAltFtFromHpa(200) - 38662.0) < 300 && std::fabs(isaAltFtFromHpa(100) - 53083.0) < 400);
            std::vector<std::pair<double, double>> pts{{38.0, -98.0}};
            std::vector<int> lv{500, 250, 100};
            const std::string url = openMeteoUrl(pts, lv);
            CHECK(url.find("latitude=38.00&longitude=-98.00") != std::string::npos && url.find("wind_speed_250hPa") != std::string::npos && url.find("wind_speed_unit=kn") != std::string::npos);
            const std::string body = "[{\"latitude\":38.0,\"longitude\":-98.0,\"hourly\":{\"time\":[1000,4600],"
                "\"wind_speed_500hPa\":[40,50],\"wind_direction_500hPa\":[270,280],\"temperature_500hPa\":[-20,-21],\"geopotential_height_500hPa\":[5600,5600],"
                "\"wind_speed_250hPa\":[90,95],\"wind_direction_250hPa\":[270,275],\"temperature_250hPa\":[-50,-51],\"geopotential_height_250hPa\":[10400,10400],"
                "\"wind_speed_100hPa\":[30,35],\"wind_direction_100hPa\":[90,95],\"temperature_100hPa\":[-60,-61],\"geopotential_height_100hPa\":[16600,16600]}}]";
            std::string perr;
            auto m = parseOpenMeteo(body, pts, lv, 4500, &perr);          // the second hour is the closer one
            CHECK(m.size() == 1 && m[0].model && m[0].located && m[0].levels.size() == 3);
            CHECK(std::fabs(m[0].levels[0].altFt - 5600 / 0.3048) < 1 && std::fabs(m[0].levels[0].speedKt - 50) < 0.01 && m[0].levels[2].tempC == -61);
            std::string e2; CHECK(parseOpenMeteo("{\"error\":true,\"reason\":\"bad\"}", pts, lv, 0, &e2).empty() && e2.find("bad") != std::string::npos);
            {   // high cloud cover comes along (and the URL can leave it out)
                CHECK(url.find("cloud_cover_high") != std::string::npos && openMeteoUrl(pts, lv, false).find("cloud_cover_high") == std::string::npos);
                std::string b2 = body; b2.insert(b2.find("\"wind_speed_500hPa\""), "\"cloud_cover_high\":[10,60],");
                auto mh = parseOpenMeteo(b2, pts, lv, 4500, &perr);
                CHECK(mh.size() == 1 && std::fabs(mh[0].highCloud - 0.6) < 1e-9 && m[0].highCloud < 0);
            }
            CHECK(parseOpenMeteo("garbage", pts, lv, 0, &e2).empty());
            // merged: FD gives the low levels, the model only what is more than 1000 ft above the FD top
            auto lw = [](double alt, double dir, double kt, double tc) { AloftLevel L; L.altFt = alt; L.hasWind = true; L.dirDeg = dir; L.speedKt = kt; L.hasTemp = true; L.tempC = tc; return L; };
            AloftStation F; F.id = "FFF"; F.lat = 38; F.lon = -98; F.located = true;
            F.levels = {lw(6000, 270, 20, -2), lw(18000, 270, 40, -20), lw(39000, 270, 100, -56)};
            std::vector<AloftStation> mix{F, m[0]};
            auto smp = sampleAloft(mix, 38, -98, 250, 4);
            bool low = false, high = false, dup = false;
            for (const auto& q : smp) { if (q.altFt == 18000) low = true; if (q.altFt > 53000) high = true; if (q.altFt > 18289 - 100 && q.altFt < 18289 + 100) dup = true; }
            CHECK(low && high && !dup);                                          // the model's 500 hPa (18.3 kft) is not mixed into the FD levels; its 100 hPa is above FL390
            for (size_t i = 1; i < smp.size(); ++i) CHECK(smp[i].altFt > smp[i - 1].altFt);
            std::vector<AloftStation> only{m[0]};
            auto g = sampleAloft(only, 50, -70, 250, 4);                          // no FD at all, far from the point: the model alone, from up to 2500 nm
            CHECK(g.size() == 3);
        }
        {   // every record gets the same 12 wind altitudes above its surface wind, whatever the terrain
            auto lv = [](double alt, double dir, double kt) { AloftLevel L; L.altFt = alt; L.hasWind = true; L.dirDeg = dir; L.speedKt = kt; return L; };
            AloftStation F; F.id = "FFF"; F.lat = 40; F.lon = -100; F.located = true;
            F.levels = {lv(3000, 270, 10), lv(9000, 270, 30), lv(18000, 270, 60), lv(34000, 270, 100), lv(39000, 270, 110)};
            std::vector<AloftStation> fd{F};
            Config wc;
            Station lo, hi;
            lo.lat = 40; lo.lon = -100; lo.elevM = 100; hi.lat = 40.1; hi.lon = -100; hi.elevM = 2200;   // the second one is above the 3000 and 9000 ft levels
            parseReport("KLOW 301553Z 18010KT 10SM FEW040 20/10 A3010", lo.report);
            parseReport("KHIG 301553Z 18010KT 10SM FEW040 20/10 A3010", hi.report);
            XpWx a = buildXp(lo, wc, 0, &fd, false), b = buildXp(hi, wc, 0, &fd, false);
            CHECK(a.winds.size() == 13 && b.winds.size() == 13);
            bool asc = true; for (size_t i = 1; i < b.winds.size(); ++i) if (b.winds[i].altMsl <= b.winds[i - 1].altMsl) asc = false;
            CHECK(asc);
            CHECK(std::fabs(a.winds[9].altMsl - b.winds[9].altMsl) < 1.0f && std::fabs(a.winds[12].altMsl - 60000 * 0.3048f) < 1.0f);   // FL390+ slots at the same height
            CHECK(a.winds[8].ms > 55 * 0.514f && a.winds[12].ms > 100 * 0.514f);   // ~34000 ft: 100 kt, held above the top level
            CHECK(b.winds[1].ms == b.winds[2].ms || b.winds[1].ms > 8 * 0.514f);   // under-terrain levels carry the lowest level's wind, not nothing
        }
        {   // winds aloft without gaps
            auto lv = [](double alt, double dir, double kt, bool wind, bool temp, double tc) { AloftLevel L; L.altFt = alt; L.hasWind = wind; L.dirDeg = dir; L.speedKt = kt; L.hasTemp = temp; L.tempC = tc; return L; };
            AloftStation A; A.id = "AAA"; A.lat = 38; A.lon = -98; A.located = true;
            A.levels = {lv(3000, 270, 10, true, false, 0), lv(6000, 270, 20, true, true, -2), /* 9000: nothing reported */ lv(12000, 270, 40, true, true, -15), lv(39000, 270, 100, true, true, -56)};
            AloftStation B = A; B.id = "BBB"; B.lat = 38.5; B.lon = -98.5;
            B.levels = {lv(3000, 270, 10, true, false, 0), lv(6000, 270, 20, true, true, -2), lv(9000, 270, 30, true, true, -8), lv(12000, 270, 40, true, true, -15)};   // no high-level data at all
            std::vector<AloftStation> st{A, B};
            auto smp = sampleAloft(st, 38.2, -98.2, 250, 4);
            auto at = [&](double alt) { for (const auto& q : smp) if (q.altFt == alt) return q; return AloftSample(); };
            CHECK(at(9000).hasWind && at(9000).hasTemp && std::fabs(at(9000).speedKt - 30.0) < 1.0);   // A's missing 9000 ft is interpolated (30 kt), B reports it: no jump
            CHECK(at(39000).hasWind && std::fabs(at(39000).speedKt - 100.0) < 1.0);                     // B has nothing up there: only A speaks, B does not drag it to a held low-level value
            // a station that is out of range is still used when nothing else is
            auto far = sampleAloft(st, 45.0, -98.0, 100, 4);
            CHECK(!far.empty() && far.size() == smp.size());
            auto none = sampleAloft(st, 70.0, -98.0, 100, 4);                                           // 2000+ nm away: nothing to use
            CHECK(none.empty());
        }
        {   // storm outline (the user's drawing): blocks in an L/T shape; skin cells sit just outside the blocks, never deep inside or far outside
            const double cell = 12.0, cl = std::cos(38 * 3.14159265 / 180);
            auto mk = [&](int i, int j) { RadarCell c; c.lat = 38 + j * cell / 60.0; c.lon = -98 + i * cell / (60.0 * cl); c.dbz = 45; c.dbzFine = 45; c.frac = 1; c.topM = 9000; c.topKnown = true; c.rateMmH = rateFromDbz(45); c.key = "R" + std::to_string(i) + ":" + std::to_string(j); return c; };
            std::vector<RadarCell> v;
            for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i) v.push_back(mk(i, j));
            for (int i = 3; i < 7; ++i) v.push_back(mk(i, 1));
            v.push_back(mk(6, 0)); v.push_back(mk(6, 2)); v.push_back(mk(0, -1));
            const size_t nReal = v.size();
            int added = addStormContour(v, cell, 15.6, 38, -98, 250, 20, 60);
            CHECK(added >= 12 && added <= 60);
            bool allOk = true; int outside = 0;
            for (size_t k = nReal; k < v.size(); ++k) {
                CHECK(v[k].synthetic && v[k].key[0] == 'C');
                double dmin = 1e9;   // distance to the nearest block's square
                for (size_t q = 0; q < nReal; ++q) {
                    const double dx = std::fabs((v[k].lon - v[q].lon) * 60.0 * cl) - cell / 2, dy = std::fabs((v[k].lat - v[q].lat) * 60.0) - cell / 2;
                    dmin = std::min(dmin, std::hypot(std::max(dx, 0.0), std::max(dy, 0.0)));
                }
                if (dmin > 0.5 * cell) allOk = false;      // skin cells hug the blocks: within half a cell of a square
                if (dmin > 0.0) ++outside;
            }
            CHECK(allOk);
            CHECK(outside < (int)(v.size() - nReal));      // and some of them sit just inside the squares' edge, not all outside it
            bool shrunk = false, full = false;
            for (size_t q = 0; q < nReal; ++q) { if (v[q].contourScale < 0.95f) shrunk = true; if (v[q].contourScale > 0.99f) full = true; CHECK(v[q].contourScale >= 0.4f && v[q].contourScale <= 1.0f); }
            CHECK(shrunk && full);                          // rim blocks are pulled in, the middle of the block keeps its full circle
            std::vector<RadarCell> w; for (size_t q = 0; q < nReal; ++q) w.push_back(mk(0, 0)), w.back() = v[q];
            for (auto& c : w) c.contourScale = 1.0f;
            addStormContour(w, cell, 15.6, 38, -98, 250, 20, 60);
            // stable: the same blocks give the same skin cells (keys and positions)
            std::vector<RadarCell> v2(v.begin(), v.begin() + nReal); for (auto& c : v2) c.contourScale = 1.0f;
            int added2 = addStormContour(v2, cell, 15.6, 38, -98, 250, 20, 60);
            CHECK(added2 == added);
            bool same = true; for (int k = 0; k < added; ++k) if (v2[nReal + k].key != v[nReal + k].key) same = false;
            CHECK(same);
            std::vector<RadarCell> one(1, mk(0, 0)); CHECK(addStormContour(one, cell, 15.6, 38, -98, 250, 20, 60) >= 4);   // a lone block gets a ring
        }
        {   // rain pits: a weak block inside a ring of strong ones is lifted, an edge block (few neighbours) is not
            std::vector<RadarCell> v;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                RadarCell c; c.lat = 38 + j * 12.0 / 60.0; c.lon = -98 + i * 12.0 / (60.0 * std::cos(38 * 3.14159265 / 180)); c.dbz = 45; c.dbzFine = 45; c.rateMmH = rateFromDbz(45);
                if (!i && !j) { c.dbzFine = 25; c.rateMmH = rateFromDbz(25); }
                v.push_back(c);
            }
            std::vector<RadarCell> a2 = v; smoothStormRain(a2, 12.0, 0.0, false);
            CHECK(a2[4].dbzFine < 26.0f);
            std::vector<RadarCell> b2 = v; smoothStormRain(b2, 12.0, 0.0, true);
            CHECK(b2[4].dbzFine > 42.0f && b2[4].rateMmH > rateFromDbz(40));
            std::vector<RadarCell> e2(v.begin(), v.begin() + 3); e2[1].dbzFine = 25; smoothStormRain(e2, 12.0, 0.0, true);
            CHECK(e2[1].dbzFine == 25.0f);
        }
        {   // top slope limit: a 11 km core next to a 4 km cell, one cell apart, ends up within the allowed difference
            std::vector<RadarCell> v(3);
            for (int i = 0; i < 3; ++i) { v[i].lat = 38; v[i].lon = -98 + i * 12.0 / (60.0 * std::cos(38 * 3.14159265 / 180)); v[i].topM = i == 0 ? 11000.0f : (i == 1 ? 8000.0f : 4000.0f); v[i].dbz = 45; }
            std::vector<RadarCell> w = v;
            limitStormTopSlope(w, 12.0, 1500.0, 250.0);
            CHECK(w[0].topM - w[1].topM <= 1500.0f + 300.0f && w[1].topM - w[2].topM <= 1500.0f + 300.0f);
            CHECK(w[0].topM >= 8500.0f && w[2].topM <= 6500.0f);                // moved toward each other, not flattened to one value
            std::vector<RadarCell> z = v; limitStormTopSlope(z, 12.0, 0.0, 250.0);
            CHECK(z[0].topM == 11000.0f && z[2].topM == 4000.0f);               // 0 = off
            v[1].synthetic = true; std::vector<RadarCell> y = v; limitStormTopSlope(y, 12.0, 1500.0, 250.0);
            CHECK(y[1].topM == 8000.0f);                                         // rim/synthetic cells keep their own height
        }
        {   // core cells (no open side) reach the core coverage, edge cells keep the normal one, 0.95 switches the core rule off
            RadarCell k = cell; k.key = "R9:9"; k.dbz = 45; k.frac = 0.5f; k.edge = 0; k.topM = 0; k.topKnown = false;
            sc.stormCoreCoverage = 1.0; XpWx a1 = base; applyStorm(a1, k, sc, 400, 38);
            CHECK(a1.clouds[2].coverage >= 0.999f);
            k.edge = 0.5f; XpWx a2 = base; applyStorm(a2, k, sc, 400, 38);
            CHECK(a2.clouds[2].coverage < 0.95f);
            k.edge = 0; sc.stormCoreCoverage = 0.95; XpWx a3 = base; applyStorm(a3, k, sc, 400, 38);
            CHECK(a3.clouds[2].coverage <= 0.95f); sc.stormCoreCoverage = 0.95;
        }
        {   // a shared base replaces the station-derived one, and the top still stays 1500 m above it
            XpWx ov = base; applyStorm(ov, cell, sc, 400, 38, 1300.0f);
            CHECK(std::fabs(ov.clouds[2].baseM - 1300.0f) < 0.5f && ov.clouds[2].topM > 1300.0f + 1500.0f);
            CHECK(std::fabs(stormBaseFor(base, cell, 400) - st.clouds[2].baseM) < 0.5f);
        }
        {   // a measured echo top is used as it is, and the map label shows the same number
            RadarCell m = cell; m.key = "R5:5"; m.dbz = 48; m.topM = 11277; m.topKnown = true; m.frac = 0.5f;
            XpWx a = base; applyStorm(a, m, sc, 400, 38);
            NEAR(a.clouds[2].topM, 11277, 0.5);
            NEAR(stormTopM(m, sc, 900.0f), 11277, 0.5);
            m.topM = 30000; applyStorm(a, m, sc, 400, 38);                     // capped at tropopause + 2.5 km
            CHECK(a.clouds[2].topM < 22000 && stormTopM(m, sc, 900.0f) == a.clouds[2].topM);
        }
        {   // weaker/smaller cells are thinner and lower than strong ones
            XpWx lo = base, hi = base; RadarCell e = cell; e.key = "R1:1"; e.topM = 0; e.topKnown = false;
            e.dbz = 28; e.frac = 0.25f; applyStorm(lo, e, sc, 400, 38);
            e.dbz = 55; e.frac = 1.0f; applyStorm(hi, e, sc, 400, 38);
            CHECK(lo.clouds[2].topM < hi.clouds[2].topM && lo.clouds[2].coverage < hi.clouds[2].coverage);
            sc.stormCoverageMax = 1.0; XpWx full = base; applyStorm(full, e, sc, 400, 38);
            CHECK(full.clouds[2].coverage > hi.clouds[2].coverage); sc.stormCoverageMax = 0.95;
        }
        XpWx w2 = base; cell.dbz = 24; cell.rateMmH = rateFromDbz(24); cell.topKnown = false; cell.topM = estimateTopM(24, 38);
        applyStorm(w2, cell, sc, 400, 38);
        CHECK(w2.clouds[2].type == 2.0f && w2.clouds[2].coverage < 0.8f && w2.precip < 0.1f && w2.visM >= 8000 && w2.turbulence == 0);
        XpWx w3 = base; cell.topM = 99000; cell.dbz = 45; cell.topKnown = true; applyStorm(w3, cell, sc, 400, 38);
        CHECK(w3.clouds[2].topM <= tropopauseKm(38) * 1000 + 2500 + 1);   // absurd tops are clamped
        XpWx w4 = base; sc.maxTurbulence = 0; cell.dbz = 55; applyStorm(w4, cell, sc, 400, 38);
        CHECK(w4.turbulence == 0);

        {   // steady rain (MRMS PrecipFlag, 2026-11-10 bright band): a 45 dBZ composite over light rain is capped and never a cumulonimbus
            Config sc2 = sc; sc2.maxTurbulence = 1;
            RadarCell sr = cell; sr.dbz = 45; sr.dbzFine = 45; sr.rateMmH = rateFromDbz(45); sr.stratiform = true; sr.topM = 11000; sr.topKnown = true;
            XpWx w5 = base; applyStorm(w5, sr, sc2, 400, 47);
            CHECK(w5.clouds[2].type == 2.0f && w5.turbulence == 0);
            RadarCell cv = sr; cv.stratiform = false;
            XpWx w6 = base; applyStorm(w6, cv, sc2, 400, 47);
            CHECK(w6.clouds[2].type == 3.0f && w6.turbulence > 0);
            // the mask: a 3 x 3 field, a convective pixel (6), steady rain (1, 10), snow (3), hail (7), tropical convection (96)
            GribField rf; rf.ni = 3; rf.nj = 2; rf.lat0 = 47.02; rf.lon0 = -119.0; rf.dLat = rf.dLon = 0.01;
            rf.v = {45, 45, 45, 45, 25, 45};
            GribField fl = rf; fl.v = {6, 1, 10, 3, 1, 96};
            int capped = 0;
            GribField m = convectiveMask(rf, &fl, nullptr, 33.0f, capped);
            CHECK(capped == 3 && rf.v[0] == 45 && rf.v[1] == 33 && rf.v[2] == 33 && rf.v[3] == 33 && rf.v[4] == 25 && rf.v[5] == 45);
            CHECK(m.v[0] == 1 && m.v[1] == 0 && m.v[5] == 1);
            fl.v[1] = 7; GribField rf2 = rf; rf2.v[1] = 50; convectiveMask(rf2, &fl, nullptr, 33.0f, capped); CHECK(rf2.v[1] == 50);   // hail stays
            // cells: one over the convective pixel stays convective, one far from it is steady rain
            std::vector<RadarCell> cs(2); cs[0].lat = 47.02; cs[0].lon = -119.0; cs[1].lat = 46.5; cs[1].lon = -118.0;
            markStratiform(cs, m, 2.0);
            CHECK(!cs[0].stratiform && cs[1].stratiform);
            // without PrecipFlag: convective only within 60 nm of an airport reporting convection
            std::vector<Station> sts(2);
            sts[0].icao = "KEPH"; sts[0].lat = 47.31; sts[0].lon = -119.52; sts[0].rawMetar = "METAR KEPH 100553Z AUTO 27003KT 10SM OVC031 08/07 A3002 RMK AO2 RAE0457B21E34";
            parseReport(sts[0].rawMetar, sts[0].report);
            sts[1].icao = "KALW"; sts[1].lat = 46.09; sts[1].lon = -118.29; sts[1].rawMetar = "METAR KALW 100553Z AUTO 15006KT 6SM RA BR FEW047 OVC060 11/08 A3007 RMK AO2 PK WND 27029/0514";
            parseReport(sts[1].rawMetar, sts[1].report);
            CHECK(!convectionReported(sts[0]) && !convectionReported(sts[1]));   // tonight: rain, no convection anywhere
            GribField rf3 = rf; rf3.v = {45, 45, 45, 45, 25, 45};
            convectiveMask(rf3, nullptr, &sts, 33.0f, capped);
            CHECK(capped == 5 && rf3.v[0] == 33);
            Station ts = sts[0]; ts.icao = "KMWH"; ts.lat = 47.2; ts.lon = -119.3; ts.rawMetar = "METAR KMWH 100552Z AUTO 31006KT 8SM -TSRA FEW035CB 07/07 A3004 RMK AO2 LTG DSNT W";
            parseReport(ts.rawMetar, ts.report);
            CHECK(convectionReported(ts));
            sts.push_back(ts);
            GribField rf4 = rf; rf4.v = {45, 45, 45, 45, 25, 45};
            convectiveMask(rf4, nullptr, &sts, 33.0f, capped);
            CHECK(capped == 0 && rf4.v[0] == 45);
            Station cbOnly = sts[0]; cbOnly.rawMetar = "METAR KXYZ 100552Z 31006KT 10SM BKN040 07/07 A3004 RMK CB DSNT NW"; CHECK(convectionReported(cbOnly));
            Station tsno = sts[0]; tsno.icao = "KATS"; tsno.rawMetar = "METAR KATS 100552Z AUTO 31006KT 10SM CLR 07/07 A3004 RMK AO2 TSNO"; parseReport(tsno.rawMetar, tsno.report);
            CHECK(!convectionReported(tsno));   // the identifier and "sensor not operating" are not thunder
        }

        CHECK(formatFl(11900, true) == "FL390" && formatFl(12000, false) == "~FL390" && formatFl(4700, true) == "FL150" && formatFl(0, true) == "FL000");

        // viewer text
        FetchResult fr; fr.fetchedAt = now; fr.radar = rr; fr.radar.source = "NOAA MRMS (CONUS)"; fr.radar.dataTime = now - 240; fr.centerLat = 37.65; fr.centerLon = -97.43;
        Station vs; vs.icao = "KVVV"; vs.rawMetar = "KVVV 301553Z 00000KT 10SM CLR 20/10 A3010"; parseReport(vs.rawMetar, vs.report); fr.stations = {vs};
        auto lines = buildViewLines(fr, true, sc, now);
        std::string all; for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar - US: NOAA MRMS (CONUS)") != std::string::npos && all.find("4 min old") != std::string::npos && all.find("dBZ") != std::string::npos);
        fr.radar = RadarResult(); fr.radar.note = "MRMS: HTTP 503";
        lines = buildViewLines(fr, true, sc, now);
        all.clear(); for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar - US: NOAA MRMS unavailable") != std::string::npos && all.find("MRMS: HTTP 503") != std::string::npos);
        // Europe: the header follows the region
        fr.centerLat = 58.45; fr.centerLon = -3.08; fr.radar.note = "European radar (OPERA): feed down: newest composite is 20:10Z (4 h 48 min old)";
        lines = buildViewLines(fr, true, sc, now);
        all.clear(); for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar - Europe: EUMETNET OPERA unavailable") != std::string::npos && all.find("\nfeed down: newest composite is 20:10Z") != std::string::npos);
        fr.radar = rr; fr.radar.source = "EUMETNET OPERA (Europe)";
        lines = buildViewLines(fr, true, sc, now);
        all.clear(); for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar - Europe: EUMETNET OPERA (Europe) - ") != std::string::npos);
        // elsewhere: no network
        fr.radar = RadarResult(); fr.radar.note = "no radar composite covers this area"; fr.centerLat = 35.55; fr.centerLon = 139.78;
        lines = buildViewLines(fr, true, sc, now);
        all.clear(); for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar: no coverage here") != std::string::npos && all.find("MRMS unavailable") == std::string::npos);
        CHECK(radarRegion(40.47, -3.56) == "Europe" && radarRegion(40.64, -73.78) == "US" && radarRegion(61.17, -150.0) == "US" && radarRegion(-33.9, 151.2).empty());
        sc.radar = false; lines = buildViewLines(fr, true, sc, now);
        all.clear(); for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar: off") != std::string::npos);
    }

    // --- METAR/ATIS window text
    {
        FetchResult fr;
        fr.fetchedAt = now;
        Station a, b;
        a.icao = "KAAA"; a.elevM = 100; a.distNm = 4; a.rawMetar = "KAAA 301553Z 00000KT 1/4SM FG OVC001 08/08 A3010";
        parseReport(a.rawMetar, a.report);
        b.icao = "KBBB"; b.elevM = 50; b.distNm = 20; b.rawMetar = "KBBB 301553Z 18010KT 10SM FEW030 15/05 A3010";
        parseReport(b.rawMetar, b.report);
        b.hasAtis = true; b.atisLetter = "C"; b.atisText = "KBBB ATIS INFO C 1553Z. 18010KT 10SM FEW030 A3010.";
        fr.stations = {a, b};
        fr.atisCount = 1;
        AloftStation fd; fd.id = "ABI"; fd.located = true; fr.aloft.push_back(fd);
        auto v = buildViewLines(fr, true, cfg, now);
        std::string all; for (auto& l : v) all += l.text + "\n";
        CHECK(all.find("KAAA") != std::string::npos && all.find("KBBB") != std::string::npos);
        CHECK(all.find("METAR  KAAA 301553Z") != std::string::npos);
        CHECK(all.find("ATIS   KBBB ATIS INFO C") != std::string::npos);
        CHECK(all.find("ATIS C") != std::string::npos);                 // letter in the header
        CHECK(all.find("ABI") == std::string::npos && all.find("winds/temps aloft points nearby") == std::string::npos);   // the GFS points line was removed
        CHECK(all.find("ATIS   KAAA") == std::string::npos);            // no ATIS block for a station without one
        CHECK(buildViewLines(fr, false, cfg, now).size() == 2);         // nothing downloaded yet
        // data services at the bottom, coloured by state; a report that already starts with "METAR" is not labelled twice
        {
            FetchResult sr = fr;
            Station u; u.icao = "EGPC"; u.rawMetar = "METAR EGPC 120050Z AUTO 32033G47KT 6000 -RA FEW024/// 05/02 Q1002"; parseReport(u.rawMetar, u.report);
            sr.stations = {u};
            ServiceStatus m{"NOAA Aviation Weather Center", "METARs", SvcState::Ok, "7 report(s)", now};
            ServiceStatus o{"EUMETNET OPERA", "radar storms (Europe)", SvcState::Down, "feed down: newest composite is 20:10Z (4 h 48 min old)", 0};
            ServiceStatus w{"Open-Meteo (NOAA GFS)", "winds and temperatures aloft", SvcState::Degraded, "unavailable, using the earlier download", now - 1800};
            sr.services = {m, o, w};
            std::vector<ServiceStatus> extra = {ServiceStatus{"D-ATIS (clowd.io)", "ATIS", SvcState::NotHere, "no digital ATIS for the airports nearby", 0},
                                                ServiceStatus{"NOAA GTGN", "turbulence (US)", SvcState::NotHere, "outside the US", 0}};   // hidden: the other region
            auto sv = buildViewLines(sr, true, cfg, now, &extra);
            std::string t; int ok = 0, down = 0, warn = 0, idle = 0; size_t hdr = 0, last = 0;
            for (size_t i = 0; i < sv.size(); ++i) {
                t += sv[i].text + "\n";
                if (sv[i].text == "Data services") hdr = i;
                if (sv[i].kind == LineKind::SvcOk) ++ok; if (sv[i].kind == LineKind::SvcDown) ++down; if (sv[i].kind == LineKind::SvcWarn) ++warn; if (sv[i].kind == LineKind::SvcIdle) ++idle;
                last = i;
            }
            CHECK(hdr > 0 && last == hdr + 4 && ok == 1 && down == 1 && warn == 1 && idle == 1);
            CHECK(t.find("DOWN   EUMETNET OPERA - radar storms (Europe): feed down: newest composite is 20:10Z") != std::string::npos);
            CHECK(t.find("(last good ") != std::string::npos && t.find("30 min ago)") != std::string::npos);
            CHECK(t.find("not used here   D-ATIS") != std::string::npos && t.find("NOAA GTGN") == std::string::npos);
            CHECK(t.find("METAR METAR") == std::string::npos && t.find("METAR EGPC 120050Z") != std::string::npos);
            CHECK(buildViewLines(sr, false, cfg, now, &extra).size() == 2 + 2 + 4);   // also before the first download
        }
        auto w = wrapText("aaaa bbbb cccc dddd eeee ffff", 10);
        CHECK(w.size() == 3 && w[0] == "aaaa bbbb");
        CHECK(wrapText("", 10).size() == 1);
    }

    // --- winds / temps aloft profile (the shape a global-model point gives: 9 levels, the top one at 39,000 ft)
    std::vector<AloftStation> al(1);
    {
        const double lv[9][4] = {{3000, 240, 20, 99}, {6000, 260, 25, 11}, {9000, 270, 31, 6}, {12000, 280, 34, 0}, {18000, 290, 38, -16},
                                 {24000, 300, 47, -30}, {30000, 300, 59, -45}, {34000, 300, 55, -52}, {39000, 300, 51, -57}};
        al[0].id = "ABI";
        for (const auto& r : lv) { AloftLevel L; L.altFt = r[0]; L.hasWind = true; L.dirDeg = r[1]; L.speedKt = r[2]; L.hasTemp = r[3] != 99; L.tempC = r[3]; al[0].levels.push_back(L); }
    }

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
        CHECK(xa.winds.back().altMsl >= 60000 * kFtToM - 1);   // top NOAA wind is held up to FL600 (66,000 ft when a low level was skipped)
        NEAR(xa.winds.back().ms, xa.winds[xa.winds.size() - 2].ms, 0.001);
        CHECK(xa.winds[0].altMsl < 570 && xa.winds[0].gustMs > xa.winds[0].ms);
        for (size_t i = 1; i < xa.winds.size(); ++i) CHECK(xa.winds[i].altMsl > xa.winds[i - 1].altMsl);
        CHECK(xa.winds.size() <= 13);
        CHECK(xa.tempProfile.size() >= 6 && xa.tempProfile[0].second == 30);       // METAR surface temp anchors the profile
        NEAR(profileTemp(xa.tempProfile, 546), 30, 0.01);
        {   // the tropopause of the profile (WMO lapse-rate rule)
            float ta = 0, tt = 0;
            CHECK(!findTropopause(xa.tempProfile, ta, tt));   // 34,000 ft -52 C, 39,000 -57: still 3.3 C/km at the top, so none can be told
            // a typical mid-latitude profile: 6.5 C/km to 10.5 km, then nearly isothermal
            std::vector<std::pair<float, float>> mp = {{300, 15}, {3000, -2.5f}, {6000, -22}, {9000, -41.5f}, {10500, -51}, {11800, -53}, {13500, -54}, {16000, -56}};
            CHECK(findTropopause(mp, ta, tt) && ta > 10000 && ta < 11400 && tt < -49 && tt > -53);
            std::printf("  tropopause of a mid-latitude profile: %.0f m %.1f C\n", ta, tt);
            // a low inversion (below 5 km) is not the tropopause
            std::vector<std::pair<float, float>> inv = {{300, 5}, {1500, 8}, {3000, 0}, {6000, -20}, {9000, -40}, {10500, -50}, {11800, -51}, {14000, -52}};
            CHECK(findTropopause(inv, ta, tt) && ta > 9500);
            // the profile ends too soon to tell: none (X-Plane then derives its own)
            std::vector<std::pair<float, float>> shortP = {{300, 15}, {6000, -22}, {9000, -41.5f}, {10500, -51}, {11000, -51.5f}};
            CHECK(!findTropopause(shortP, ta, tt));
            // a smooth change in the profile moves the tropopause smoothly (no snap to a level)
            std::vector<std::pair<float, float>> mp2 = mp; mp2[4].second = -50.5f;
            float ta2 = 0, tt2 = 0;
            CHECK(findTropopause(mp2, ta2, tt2) && std::fabs(ta2 - ta) < 400);
        }
        NEAR(profileTemp(xa.tempProfile, 9144), -45, 0.5);                           // 30 000 ft
        CHECK(profileTemp(xa.tempProfile, 20000) == profileTemp(xa.tempProfile, 12000)); // constant above the top level
        Config off = cfg; off.useAloft = false;
        XpWx xo = buildXp(kabi, off, now, &st2);
        CHECK(!xo.aloft && xo.maxAltFt == 10000 && xo.winds.size() == 1);
    }

    // ---- validateXp: sanity fixes that must leave legitimate weather alone
    {
        auto mk = [](float cov, float b, float tp) { XpCloud c; c.type = 1; c.coverage = cov; c.baseM = b; c.topM = tp; return c; };
        XpWx ok; ok.clouds[1] = mk(0.4f, 1300, 1500); ok.clouds[2] = mk(0.94f, 3000, 4200); ok.maxAltFt = 20000;
        XpWx ref = ok;
        CHECK(validateXp(ok, false) == 0 && ok.clouds[2].topM == ref.clouds[2].topM && ok.clouds[1].baseM == ref.clouds[1].baseM);   // widespread overcast over broken: untouched
        XpWx bad; bad.clouds[1] = mk(1.4f, 2000, 1900); bad.maxAltFt = 1000;
        std::string note;
        CHECK(validateXp(bad, false, &note) >= 3 && bad.clouds[1].coverage <= 1.0f && bad.clouds[1].topM > bad.clouds[1].baseM && bad.maxAltFt >= bad.clouds[1].topM / 0.3048f - 1 && !note.empty());
        XpWx ov; ov.clouds[1] = mk(0.7f, 1000, 2000); ov.clouds[2] = mk(0.7f, 1500, 2500);   // overlapping layers
        CHECK(validateXp(ov, false) >= 1 && ov.clouds[2].baseM >= ov.clouds[1].topM + 40 - 0.5f && ov.clouds[2].topM - ov.clouds[2].baseM >= 999);
        XpWx ov2; ov2.clouds[1] = mk(0.7f, 1000, 2000); ov2.clouds[2] = mk(0.7f, 1500, 2500); ov2.maxAltFt = 20000; CHECK(validateXp(ov2, true) == 0 && ov2.clouds[2].baseM == 1500);  // storms may overlap on purpose
        { XpWx hi; hi.clouds[2] = mk(0.9f, 600, 15000); hi.clouds[2].type = 3; hi.maxAltFt = 60000; CHECK(validateXp(hi, true) == 1 && hi.clouds[2].topM == kXpTopM); }
        { RadarCell tall; tall.lat = 28; tall.dbz = 55; tall.topKnown = true; tall.topM = 15500; Config tc; CHECK(stormTopM(tall, tc, 900.0f) == kXpTopM); }
        {   // per-type maximum thickness (X-Plane's manual weather limits); cumulonimbus is not limited by thickness
            XpWx ty; ty.maxAltFt = 60000;
            ty.clouds[0] = mk(0.5f, 9000, 10000); ty.clouds[0].type = 0;      // cirrus 1000 m -> 548.6 m
            ty.clouds[1] = mk(0.9f, 1000, 3000); ty.clouds[1].type = 1;       // stratus 2000 m -> 1499.6 m
            ty.clouds[2] = mk(0.9f, 4000, 10000); ty.clouds[2].type = 2;      // cumulus 6000 m -> 5084 m
            CHECK(validateXp(ty, true) >= 3);
            NEAR(ty.clouds[0].topM - ty.clouds[0].baseM, 548.64, 0.1);
            NEAR(ty.clouds[1].topM - ty.clouds[1].baseM, 1499.6, 0.2);
            NEAR(ty.clouds[2].topM - ty.clouds[2].baseM, 5084.06, 0.2);
            XpWx hc; hc.maxAltFt = 60000; hc.clouds[2] = mk(0.5f, 13500, 14000); hc.clouds[2].type = 0;   // any type: tops stop at 45,000 ft
            CHECK(validateXp(hc, true) >= 1 && hc.clouds[2].topM <= kXpMaxTopM + 0.1f);
            XpWx lo; lo.maxAltFt = 60000; lo.clouds[2] = mk(0.9f, -50, 1200); lo.clouds[2].type = 3;      // bases are not limited (ground level and below are allowed)
            CHECK(validateXp(lo, true) == 0 && lo.clouds[2].baseM == -50.0f);
            XpWx cbk; cbk.clouds[2] = mk(0.9f, 100, 13000); cbk.clouds[2].type = 3; cbk.maxAltFt = 60000;
            CHECK(validateXp(cbk, true) == 0 && cbk.clouds[2].topM == 13000);
        }
        { XpWx ci; ci.clouds[2] = mk(0.5f, 13400, 14200); ci.clouds[2].type = 0; ci.maxAltFt = 60000; CHECK(validateXp(ci, true) >= 1 && ci.clouds[2].topM <= kXpTopM + 0.01f); }   // cirrus top is limited too
        { XpWx lo; lo.clouds[2] = mk(0.9f, -50, 400); lo.clouds[2].type = 3; lo.maxAltFt = 5000; CHECK(validateXp(lo, true) == 0 && lo.clouds[2].baseM == -50); }   // bases are free: low or below sea level is fine
        XpWx deep; deep.clouds[2] = mk(0.9f, 600, 13000); deep.clouds[2].type = 3; deep.maxAltFt = 50000;   // deep convection is legitimate
        CHECK(validateXp(deep, true) == 0 && deep.clouds[2].topM == 13000);
        XpWx fogOver; fogOver.clouds[0] = mk(0.9f, 0, 80); fogOver.clouds[1] = mk(0.94f, 150, 900); fogOver.maxAltFt = 5000;   // fog beneath overcast
        CHECK(validateXp(fogOver, false) == 0);
        XpWx thin; thin.clouds[1] = mk(0.5f, 1000, 1005); thin.maxAltFt = 9000;
        CHECK(validateXp(thin, false) == 1 && thin.clouds[1].topM - thin.clouds[1].baseM >= 30);
    }
    // ---- applyStorm keeps the station's own layers when a slot is free, and embeds the storm in an overcast
    {
        Station os; os.elevM = 300;
        parseReport("KOOO 301553Z 18010KT 6SM OVC020", os.report);
        Config oc;
        XpWx ob = buildXp(os, oc, now, nullptr, true);
        const XpCloud deckBefore = ob.clouds[2].coverage > 0 ? ob.clouds[2] : ob.clouds[1];
        RadarCell cc; cc.lat = 38; cc.lon = -98; cc.dbz = 45; cc.frac = 1; cc.topKnown = true; cc.topM = 11000; cc.rateMmH = rateFromDbz(45);
        XpWx es = ob; applyStorm(es, cc, oc, 300, 38);
        int deckSlot = -1, stormSlot = -1;
        for (int i = 0; i < 3; ++i) { if (es.clouds[i].type == 3.0f && es.clouds[i].topM > 9000) stormSlot = i; else if (es.clouds[i].coverage > 0.9f) deckSlot = i; }
        CHECK(stormSlot >= 1 && deckSlot >= 1 && deckSlot != stormSlot && es.clouds[deckSlot].baseM == deckBefore.baseM && es.clouds[deckSlot].topM == deckBefore.topM);   // overcast survives next to the storm
        CHECK(es.clouds[stormSlot].topM == 11000 && es.clouds[stormSlot].baseM <= deckBefore.baseM + 1);                                    // genuine deep convection is not capped
        CHECK(validateXp(es, true) == 0);
        {   // a storm replacing an overcast keeps at least the overcast's coverage
            Station ov3; ov3.elevM = 380;
            parseReport("KVVV 301553Z 18010KT 4SM FEW010 SCT015 OVC030", ov3.report);
            XpWx b5 = buildXp(ov3, oc, now, nullptr, false);
            RadarCell c5 = cc; c5.dbz = 32; c5.topKnown = false; c5.topM = 0;
            XpWx s5 = b5; applyStorm(s5, c5, oc, 380, 38);
            CHECK(b5.clouds[2].coverage >= 0.9f && s5.clouds[2].coverage >= b5.clouds[2].coverage);
        }
        {   // rim of a storm cluster slopes off: isolated blocks are lower and thinner, interior blocks are untouched
            std::vector<RadarCell> grid;
            for (int a = -1; a <= 1; ++a) for (int b = -1; b <= 1; ++b) { RadarCell g = cc; g.lat = 38 + a * 0.2; g.lon = -98 + b * 0.2 / std::cos(38 * 3.14159265 / 180); g.dbz = 50; g.topKnown = true; g.topM = 11000; grid.push_back(g); }
            RadarCell lone = cc; lone.lat = 40; lone.lon = -98; lone.dbz = 50; lone.topKnown = true; lone.topM = 11000; grid.push_back(lone);
            markStormEdges(grid, 12.0);
            CHECK(grid[4].edge == 0.0f);          // centre of the 3 x 3 cluster
            CHECK(grid[0].edge > 0.0f && grid[0].edge < 1.0f);   // a corner has 3 neighbours
            CHECK(grid[9].edge == 1.0f);          // lone block
            XpWx mid = ob, rim = ob, iso = ob;
            applyStorm(mid, grid[4], oc, 300, 38); applyStorm(rim, grid[0], oc, 300, 38); applyStorm(iso, grid[9], oc, 300, 38);
            CHECK(rim.clouds[2].topM < mid.clouds[2].topM && iso.clouds[2].topM < rim.clouds[2].topM);
            CHECK(iso.clouds[2].coverage <= mid.clouds[2].coverage);
            CHECK(mid.clouds[2].topM == 11000 && iso.clouds[2].topM >= iso.clouds[2].baseM + 1500);
            CHECK(iso.clouds[2].topM > 6000);     // still a real tower, only lower
        }
        {   // weak echoes are cumulus, strong ones cumulonimbus; a weak cell next to a strong one does not get a deep tower
            RadarCell wk = cc; wk.dbz = 24; wk.topKnown = false; wk.topM = 0; wk.key = "R9:9";
            XpWx ws = ob; applyStorm(ws, wk, oc, 300, 38);
            int sl = ws.clouds[2].coverage > 0 && ws.clouds[2].topM > ws.clouds[1].topM ? 2 : 1;
            CHECK(ws.clouds[sl].type == 2.0f && ws.clouds[sl].topM < 9000);
            wk.dbz = 42; XpWx ms = ob; applyStorm(ms, wk, oc, 300, 38);   // a thunderstorm core (40 dBZ and up)
            CHECK(ms.clouds[2].type == 3.0f || ms.clouds[1].type == 3.0f);
        }
        // three layers: the storm replaces the highest and grows from ITS base, not from the lowest layer (slot-wise blending in X-Plane)
        Station t3; t3.elevM = 380;
        parseReport("KTTT 301553Z 18010KT 5SM SCT018 BKN040 OVC080", t3.report);
        XpWx b3 = buildXp(t3, oc, now, nullptr, false);
        XpWx s3 = b3; applyStorm(s3, cc, oc, 380, 38);
        CHECK(s3.clouds[0].baseM == b3.clouds[0].baseM && s3.clouds[1].baseM == b3.clouds[1].baseM && s3.clouds[2].type == 3.0f);
    }
    {
        Station t4; t4.elevM = 380; Config oc;
        parseReport("KUUU 301553Z 18010KT 5SM FEW012 BKN040", t4.report);
        XpWx b4 = buildXp(t4, oc, now, nullptr, true);
        RadarCell c4; c4.lat = 38; c4.lon = -98; c4.dbz = 45; c4.frac = 1; c4.topKnown = true; c4.topM = 11000; c4.rateMmH = rateFromDbz(45);
        XpWx s4 = b4; applyStorm(s4, c4, oc, 380, 38);
        CHECK(s4.clouds[1].baseM == b4.clouds[1].baseM && s4.clouds[2].topM == 11000);
    }

    // ---- 2026-11-06: FL350 excursion near Creede, CO (KC24) and the checks added for it ----------------------------------------------
    {   // station elevation sanity: 8,680 "m" is 8,680 ft; ordinary mountain neighbours stay as they are
        std::vector<Station> st(4);
        st[0].icao = "KC24"; st[0].lat = 37.82; st[0].lon = -106.93; st[0].elevM = 8680;     // the bad value from the log
        st[1].icao = "KDRO"; st[1].lat = 37.15; st[1].lon = -107.75; st[1].elevM = 2038;
        st[2].icao = "KPSO"; st[2].lat = 37.29; st[2].lon = -107.06; st[2].elevM = 2343;
        st[3].icao = "KLXV"; st[3].lat = 39.22; st[3].lon = -106.32; st[3].elevM = 3027;     // a genuinely high field
        const std::string note = checkStationElevations(st);
        NEAR(st[0].elevM, 2645.7, 1.0);
        CHECK(st[1].elevM == 2038 && st[2].elevM == 2343 && st[3].elevM == 3027);
        CHECK(note.find("KC24") != std::string::npos && note.find("KDRO") == std::string::npos);
        std::vector<Station> lone(1); lone[0].icao = "XXXX"; lone[0].elevM = 9000;    // no neighbours, still impossible: read as feet
        checkStationElevations(lone); NEAR(lone[0].elevM, 2743.2, 1.0);
        std::vector<Station> nan1(1); nan1[0].elevM = NAN; checkStationElevations(nan1); CHECK(nan1[0].elevM == 0.0);
    }
    // forecast levels as the global model gave them in the log, plus FD-style levels
    std::vector<AloftSample> prof;
    {
        const double lv[][4] = {{3000, 140, 6, 18}, {6000, 150, 7, 10}, {9000, 135, 8, 4}, {12000, 136, 3, -2}, {18000, 305, 16, -13},
                                {24000, 299, 32, -26}, {30000, 288, 40, -40}, {34000, 282, 40, -50}, {39000, 272, 49, -57}, {45354, 289, 45, -57}, {53614, 283, 38, -58}};
        for (const auto& l : lv) { AloftSample a; a.altFt = l[0]; a.hasWind = true; a.dirDeg = l[1]; a.speedKt = l[2]; a.hasTemp = true; a.tempC = l[3]; prof.push_back(a); }
    }
    auto checkProfile = [&](const XpWx& x, double elevM, const char* what) {
        bool asc = true, spaced = true;
        for (size_t i = 1; i < x.winds.size(); ++i) { asc = asc && x.winds[i].altMsl > x.winds[i - 1].altMsl; spaced = spaced && x.winds[i].altMsl - x.winds[i - 1].altMsl >= 150.0f; }
        if (!(asc && spaced && x.winds.size() <= 13 && x.winds.size() >= 12)) std::printf("  profile check failed: %s\n", what);
        CHECK(asc && spaced && x.winds.size() <= 13 && x.winds.size() >= 12);
        XpWx v = x; std::string vw; const int vb = validateProfile(v, false, &vw); if (vb) std::printf("  %s: %s\n", what, vw.c_str()); CHECK(vb == 0);
        (void)elevM;
    };
    {   // high-elevation station (KC24 at its real 2,646 m): no squeezed layers, the forecast temperatures kept below and above the ground
        Station s; s.icao = "KC24"; s.lat = 37.82; s.lon = -106.93; s.elevM = 2646;
        parseReport("KC24 052315Z AUTO 00000KT 10SM CLR 03/M10 A3027", s.report);
        XpWx x; x.hasWind = true; XpWind w0; w0.altMsl = 2656; x.winds.push_back(w0); x.hasTemp = true; x.tempC = 3;
        buildAloftProfile(x, prof, s.elevM, true, 3.0f);
        checkProfile(x, s.elevM, "KC24 real elevation");
        NEAR(profileTemp(x.tempProfile, 18000 * 0.3048f), -13.0, 0.5);   // was +3 (the surface temperature held up to FL240)
        NEAR(profileTemp(x.tempProfile, 24000 * 0.3048f), -26.0, 0.5);
        const XpWind f350 = windAt(x.winds, 35000 * 0.3048f);
        CHECK(f350.ms / 0.514444f > 38 && f350.ms / 0.514444f < 46);      // the jet-level wind, not a calm band
        const XpWind below = windAt(x.winds, 1000.0f);                    // under the ground: the surface wind, held
        CHECK(below.ms < 0.5f);
        // the same station with the elevation the log had (8,690 m): still ordered and spaced; no 7 layers within 20 ft
        XpWx y; y.hasWind = true; XpWind w1; w1.altMsl = 8690; y.winds.push_back(w1);
        buildAloftProfile(y, prof, 8680, true, 3.0f);
        checkProfile(y, 8680, "KC24 bad elevation");
    }
    {   // missing and malformed winds-aloft data: NaN, impossible speed, impossible temperature are dropped, never turned into 0
        std::vector<AloftSample> bad = prof;
        bad[6].speedKt = NAN; bad[7].speedKt = 900; bad[8].tempC = 45; bad[2].tempC = NAN;
        XpWx x; buildAloftProfile(x, bad, 300, false, 0);
        CHECK(x.aloftRejected == 4);
        const XpWind f300 = windAt(x.winds, 30000 * 0.3048f);
        CHECK(f300.ms / 0.514444f > 20);                                   // interpolated across the gap, not 0
        XpWx none; buildAloftProfile(none, std::vector<AloftSample>(), 300, false, 0);
        CHECK(none.winds.empty() && !none.aloft);                          // no data: nothing invented
    }
    {   // directions across north interpolate as vectors
        std::vector<XpWind> p(2);
        p[0].altMsl = 1000; p[0].dir = 350; p[0].ms = 10; p[1].altMsl = 2000; p[1].dir = 10; p[1].ms = 10;
        const XpWind m = windAt(p, 1500);
        CHECK((m.dir > 359.0f || m.dir < 1.0f) && m.ms > 9.7f && m.ms < 9.9f);
        p[0].dir = 270; p[1].dir = 90; p[1].ms = 0;                        // strong gradient: speed fades, direction holds
        CHECK(std::fabs(windAt(p, 1500).dir - 270.0f) < 0.5f);
    }
    {   // magnetic to true
        CHECK(magneticToTrue(270, 10, false) == 280);
        CHECK(magneticToTrue(270, -10, false) == 260);
        CHECK(magneticToTrue(5, -10, false) == 355);
        CHECK(magneticToTrue(355, 10, false) == 5);
        CHECK(magneticToTrue(360, 0.2, false) == 360);                     // near-zero variation
        CHECK(magneticToTrue(10, -10, false) == 360);                      // north stays 360, never 0 (0 means calm)
        CHECK(magneticToTrue(0, 15, true) == 0);                           // calm / variable: untouched
        CHECK(magneticToTrue(150, -20.5, false) == 130);                   // large west variation
        Station a; parseReport("KCMH 061951Z 15013G19KT 10SM OVC100 17/15 A2984", a.report);
        Report atis; parseReport("CMH ATIS INFO P 1951Z. 15013G19KT 5SM RA BR OVC100 17/15 A2984", atis, true);
        mergeAtis(a, atis, "x");
        CHECK(a.report.windMagnetic && a.report.windDir == 150);           // flagged for the sim-thread conversion
        Station m; parseReport("KCMH 061951Z 15013G19KT 10SM OVC100 17/15 A2984", m.report);
        CHECK(!m.report.windMagnetic);                                     // METAR wind is true: never converted
    }
    {   // validation: compressed layers, NaN and impossible values never pass
        XpWx v; for (int i = 0; i < 4; ++i) { XpWind w; w.altMsl = 8690.0f + i; v.winds.push_back(w); }
        XpWind ok; ok.altMsl = 9144; ok.ms = 18; ok.dir = 302; v.winds.push_back(ok);
        std::string why;
        CHECK(validateProfile(v, true, &why) == 3 && v.winds.size() == 2 && why.find("100 m") != std::string::npos);
        XpWx t; t.tempProfile = {{1000, 10}, {5000, NAN}, {7000, 3}, {9000, 10}}; t.qnhPa = 50000; t.precip = NAN;
        CHECK(validateProfile(t, true) == 4 && t.tempProfile.size() == 2 && t.qnhPa == 0 && t.precip == 0);
        // the KC24 column (+3 C at FL240 = ISA+36) is unusual but possible, so it is the elevation fix, not this check, that removes it
        XpWx w; w.tempProfile = {{2646, 3}, {7315, 3}, {9144, -28}};
        CHECK(validateProfile(w, false) == 0);
    }
    {   // a dry METAR gives no rain; a rain METAR does
        Config dc; Station d; d.elevM = 200; parseReport("KAAA 061951Z 27005KT 10SM CLR 15/05 A3000", d.report);
        CHECK(buildXp(d, dc, now).precip == 0.0f);
        Station r; r.elevM = 200; parseReport("KAAA 061951Z 27005KT 3SM RA OVC010 15/14 A3000", r.report);
        CHECK(buildXp(r, dc, now).precip > 0.0f);
    }

    // ---- dry runways (2026-11-06): recent precipitation from report history and remarks ---------------------------------------------
    {
        NEAR(remarkPrecipEndedMinAgo("KAAA 061953Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 RAB10E35 SLP155", 53), 18.0, 0.01);
        NEAR(remarkPrecipEndedMinAgo("KAAA 061953Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 RAB1912E1938 SLP155", 53), 15.0, 0.01);
        NEAR(remarkPrecipEndedMinAgo("KAAA 061953Z AUTO 27005KT 10SM -RA OVC020 20/18 A3000 RMK AO2 RAB45 SLP155", 53), 0.0, 0.01);   // began, still going
        NEAR(remarkPrecipEndedMinAgo("KAAA 061953Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 TSB10E30 RAE25 SLP155", 53), 28.0, 0.01);
        NEAR(remarkPrecipEndedMinAgo("KAAA 061905Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 RAE50 SLP155", 5), 15.0, 0.01);          // ended in the previous hour
        CHECK(remarkPrecipEndedMinAgo("KAAA 061953Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 SLP155", 53) < 0);
        bool fz = false; remarkPrecipEndedMinAgo("KAAA 061953Z AUTO 27005KT 10SM CLR M02/M05 A3000 RMK AO2 SNE20 SLP155", 53, &fz); CHECK(fz);
        auto st = [](const char* raw, long long t) { Station s; s.icao = "KAAA"; s.lat = 30; s.lon = -90; s.rawMetar = raw; parseReport(raw, s.report); s.obsTime = t; return s; };
        const long long T = 1793994780;   // 2026-11-06 19:53Z, the time of the reports below
        {   // rain in the report 30 min earlier: not dry for a 45 min rule
            std::vector<Station> v = {st("KAAA 061953Z 27005KT 10SM CLR 20/10 A3000", T), st("KAAA 061923Z 27005KT 5SM -RA OVC020 18/16 A3000", T - 1800)};
            foldReportHistory(v);
            CHECK(v.size() == 1 && v[0].obsTime == T && std::fabs(v[0].rainMinAgo - 30) < 0.01 && v[0].historyMin >= 30);
            std::string why;
            CHECK(!runwayShouldBeDry(v[0], (std::time_t)T, 45, &why) && why.find("30 min ago") != std::string::npos);
            CHECK(runwayShouldBeDry(v[0], (std::time_t)(T + 20 * 60), 45, &why));   // 20 minutes later the report is 20 min old: 50 min dry
        }
        {   // an hour of clean reports: dry; the same with snow: left alone; rain now: not dry
            std::vector<Station> v = {st("KAAA 061953Z 27005KT 10SM CLR 20/10 A3000", T), st("KAAA 061853Z 27005KT 10SM CLR 20/10 A3000", T - 3600)};
            foldReportHistory(v);
            CHECK(v[0].rainMinAgo < 0 && runwayShouldBeDry(v[0], (std::time_t)T, 45, nullptr));
            std::vector<Station> sn = {st("KAAA 061953Z 27005KT 10SM CLR M01/M05 A3000", T), st("KAAA 061853Z 27005KT 2SM -SN OVC010 M01/M03 A3000", T - 3600)};
            foldReportHistory(sn);
            CHECK(sn[0].frozenRecent && !runwayShouldBeDry(sn[0], (std::time_t)T, 45, nullptr));
            std::vector<Station> now = {st("KAAA 061953Z 27005KT 3SM RA OVC010 20/18 A3000", T)};
            foldReportHistory(now);
            CHECK(now[0].rainMinAgo == 0 && !runwayShouldBeDry(now[0], (std::time_t)T, 45, nullptr));
        }
        {   // no history to judge by (a single report, no AO2 remarks): not dried; an AO2 station's remarks cover an hour
            std::vector<Station> one = {st("KAAA 061953Z 27005KT 10SM CLR 20/10 A3000", T)};
            foldReportHistory(one);
            std::string why;
            CHECK(!runwayShouldBeDry(one[0], (std::time_t)T, 45, &why) && why.find("history") != std::string::npos);
            std::vector<Station> ao2 = {st("KAAA 061953Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 SLP155", T)};
            foldReportHistory(ao2);
            CHECK(runwayShouldBeDry(ao2[0], (std::time_t)T, 45, nullptr));
            std::vector<Station> tr = {st("KAAA 061953Z AUTO 27005KT 10SM CLR 20/10 A3000 RMK AO2 SLP155 P0000", T)};   // a trace in the last hour
            foldReportHistory(tr);
            CHECK(!runwayShouldBeDry(tr[0], (std::time_t)T, 45, nullptr));
            std::vector<Station> vc = {st("KAAA 061953Z 27005KT 10SM VCSH SCT030 20/10 A3000", T), st("KAAA 061853Z 27005KT 10SM CLR 20/10 A3000", T - 3600)};
            foldReportHistory(vc);
            CHECK(vc[0].rainMinAgo < 0);   // showers in the vicinity are not rain at the airport
        }
    }

    // ---- windshear model (2026-11-06): scenarios A-K of the specification --------------------------------------------------------------
    {
        using namespace wx::shear;
        NEAR(intervalProbability(0.10, 3600), 0.10, 1e-9);                               // the hazard rate gives the hourly target back
        NEAR(1.0 - std::pow(1.0 - intervalProbability(0.10, 30), 120), 0.10, 1e-9);      // and 120 x 30 s make an hour
        auto mkStation = [&](const char* metar, double la, double lo) { Station s; s.lat = la; s.lon = lo; s.elevM = 50; parseReport(metar, s.report); s.icao = s.report.icao; return s; };
        auto cell = [](double la, double lo, float dbz) { RadarCell c; c.lat = la; c.lon = lo; c.dbz = dbz; c.dbzFine = dbz; return c; };
        auto run = [](Engine& e, const std::vector<Source>& src, const Env& env, double hours, double dt) {
            for (double t = 0; t <= hours * 3600.0; t += dt) e.step(t, src, env);
        };
        // A: clear, calm weather - no source, no field, nothing added anywhere
        std::vector<Station> calm = {mkStation("KAAA 061951Z 00000KT 10SM CLR 20/10 A3000", 30, -90), mkStation("KBBB 061951Z 00000KT 10SM CLR 20/10 A3000", 30.3, -90)};
        std::vector<RadarCell> none;
        Env eA; eA.stations = &calm; eA.cells = &none;
        CHECK(assess(eA).empty());
        { Engine e(1); run(e, assess(eA), eA, 100, 30); CHECK(e.fields().empty()); const Vec3 p = e.perturbation(30, -90, 500, 1000); CHECK(p.east == 0 && p.north == 0 && p.up == 0); }
        // B: low-level jet (light surface wind, 35 kt at 1,500-2,000 ft): sustained vertical shear comes from the data, no synthetic field
        {
            XpWx x; x.hasWind = true; XpWind w0; w0.altMsl = 60; w0.ms = 5 * 0.514444f; w0.dir = 180; x.winds.push_back(w0);
            std::vector<AloftSample> llj(3);
            llj[0].altFt = 2000; llj[0].hasWind = true; llj[0].dirDeg = 200; llj[0].speedKt = 35;
            llj[1].altFt = 6000; llj[1].hasWind = true; llj[1].dirDeg = 230; llj[1].speedKt = 25;
            llj[2].altFt = 12000; llj[2].hasWind = true; llj[2].dirDeg = 250; llj[2].speedKt = 30;
            buildAloftProfile(x, llj, 50, false, 0);
            const XpWind lo = windAt(x.winds, 60), hi = windAt(x.winds, 2000 * 0.3048f);
            CHECK(lo.ms / 0.514444f < 6 && hi.ms / 0.514444f > 25);           // 5 kt at the ground, ~30 kt at 2,000 ft: the shear is there
            CHECK(assess(eA).empty());                                          // and nothing is added on top
        }
        // C: gusty frontal passage between two stations: the wind change is in the station records already
        {
            std::vector<Station> front = {mkStation("KAAA 061951Z 20015G25KT 10SM BKN030 20/15 A3000", 30, -90), mkStation("KBBB 061951Z 32020G32KT 10SM BKN030 12/05 A3005", 30.3, -90)};
            Env eC; eC.stations = &front; eC.cells = &none;
            CHECK(assess(eC).empty());
        }
        // D: moderate thunderstorms (40-45 dBZ): gust fronts possible, microbursts rare, nothing guaranteed
        std::vector<Station> tsSt = {mkStation("KAAA 061951Z 24012G22KT 5SM -TSRA BKN040CB 28/22 A2990", 30, -90)};
        std::vector<RadarCell> mod = {cell(30.05, -90.05, 42)};
        Env eD; eD.stations = &tsSt; eD.cells = &mod;
        const std::vector<Source> sD = assess(eD);
        CHECK(sD.size() == 1 && sD[0].confidence == kMedium && std::fabs(sD[0].pHourGustFront - 0.10) < 1e-9 && std::fabs(sD[0].pHourMicroburst - 0.007) < 1e-9);
        int gD = 0, mD = 0;
        {
            Engine e(7);
            for (double t = 0; t <= 200 * 3600.0; t += 30) for (const auto& l : e.step(t, sD, eD)) { if (l.find("formed: gust front") != std::string::npos) ++gD; if (l.find("formed: microburst") != std::string::npos) ++mD; }
            std::printf("  windshear calibration, moderate storm, 200 h: %d gust fronts, %d microbursts\n", gD, mD);
            CHECK(gD >= 6 && gD <= 30 && mD <= 6 && mD < gD);
        }
        // E: intense storms (55 dBZ, TS, gusts 35 kt): more, and occasionally intense; extreme ones stay uncommon
        std::vector<Station> sevSt = {mkStation("KAAA 061951Z 27025G35KT 2SM +TSRA BKN020CB 24/22 A2985", 30, -90)};
        std::vector<RadarCell> sev = {cell(30.02, -90.02, 55)};
        Env eE; eE.stations = &sevSt; eE.cells = &sev;
        const std::vector<Source> sE = assess(eE);
        CHECK(sE.size() == 1 && sE[0].confidence == kHigh && std::fabs(sE[0].pHourMicroburst - 0.02) < 1e-9);
        {
            int mb = 0, ext = 0, strongMb = 0;
            Engine e(3);
            for (double t = 0; t <= 2000 * 3600.0; t += 30) {
                e.step(t, sE, eE);
                for (const auto& f : e.fields()) if (f.type == kMicroburst && f.t0 == std::floor(t / 30.0) * 30.0) { ++mb; if (f.peakKt >= 40) ++ext; if (f.peakKt >= 20) ++strongMb; }
            }
            std::printf("  windshear calibration, intense storm, 2000 h: %d microbursts, %d with 20+ kt outflow, %d with 40+ kt\n", mb, strongMb, ext);
            CHECK(mb >= 20 && mb <= 60 && ext * 10 <= mb && ext >= 0);
        }
        // F: dry microburst environment: convective echo without heavy rain, 27 C spread, cloud base 12,000 ft
        {
            std::vector<Station> drySt = {mkStation("KAAA 061951Z 22010KT 10SM SCT120 32/05 A2995", 35, -110)};
            std::vector<RadarCell> virga = {cell(35.03, -110.0, 35)};
            Env eF; eF.stations = &drySt; eF.cells = &virga;
            const std::vector<Source> sF = assess(eF);
            CHECK(sF.size() == 1 && sF[0].dry && sF[0].confidence == kLow && sF[0].pHourMicroburst > 0 && sF[0].pHourMicroburst < 0.005);
            std::vector<Station> moist = {mkStation("KAAA 061951Z 22010KT 10SM SCT120 32/25 A2995", 35, -110)};   // spread alone is not enough...
            Env eF2; eF2.stations = &moist; eF2.cells = &virga;
            CHECK(assess(eF2).empty());                                                                          // ...and a moist one gives none
            std::vector<Station> noCloud = {mkStation("KAAA 061951Z 22010KT 10SM CLR 32/05 A2995", 35, -110)};
            Env eF3; eF3.stations = &noCloud; eF3.cells = &none;
            CHECK(assess(eF3).empty());                                                                          // dry air without a convective source: none
        }
        // G/H: an approach through a mature microburst, straight through and offset
        {
            Engine e(1);
            const Field& f = e.force(kMicroburst, 30.0, -90.0, 1000.0, 0.0, eD);
            const double t = 1000.0;
            CHECK(Engine::intensity(f, t) > 0.99);
            auto headAt = [&](double dNorth, double dEast, double agl) {   // aircraft flying north
                const Vec3 p = e.perturbation(30.0 + dNorth / 60.0, -90.0 + dEast / 60.0 / std::cos(30.0 * 3.14159265 / 180.0), agl, t);
                return -p.north;   // headwind for a northbound aircraft
            };
            const double hIn = headAt(-1.0, 0, 500), hOut = headAt(1.0, 0, 500);
            const Vec3 core = e.perturbation(30.0, -90.0, 1500, t);
            std::printf("  microburst centreline at 500 ft: headwind %+.0f kt 1 nm before, %+.0f kt 1 nm after, downdraft %.0f fpm at 1,500 ft (modelled)\n", hIn, hOut, core.up);
            CHECK(hIn > 15 && hOut < -15 && core.up < -2000);                   // headwind, downdraft, tailwind
            const double oIn = headAt(-1.0, 1.2, 500), oOut = headAt(1.0, 1.2, 500);
            const Vec3 side = e.perturbation(30.0, -90.0 + 1.2 / 60.0 / std::cos(30.0 * 3.14159265 / 180.0), 500, t);
            CHECK(oIn - oOut < hIn - hOut && std::fabs(side.east) > 10);       // offset: less head/tail change, a crosswind instead
            // a fast and a slow aircraft on the same line: same field, the transition takes 140 kt / 250 kt times as long
            auto crossTime = [&](double gsKt) { double t0 = -1, t1 = -1; for (double s = 0; s < 400; s += 0.5) { const double d = -2.0 + gsKt * s / 3600.0; const double h = headAt(d, 0, 500); if (t0 < 0 && h < 10) t0 = s; if (t1 < 0 && h < -10) { t1 = s; break; } } return t1 - t0; };
            const double slow = crossTime(140), fast = crossTime(250);
            CHECK(slow > fast * 1.6 && slow < fast * 1.9);
            // J: FL350 above the storm - no low-level effect at all
            const Vec3 hiP = e.perturbation(30.0, -90.0 + 1.0 / 60.0, 35000, t);
            CHECK(hiP.east == 0 && hiP.north == 0 && hiP.up == 0);
            // records: a ring of six outward 1 nm records, scaled for X-Plane's blending, only while the field lives
            // records: fixed 1.2 nm cells around the centre blowing outward, scaled for X-Plane's blending, only while the field lives
            const auto recs = e.records(t, 1.4);
            float mx = 0; bool outward = true;
            for (const auto& r : recs) {
                mx = std::max(mx, r.kt);
                const double b = std::atan2((r.lon + 90.0) * std::cos(30.0 * 3.14159265 / 180.0), r.lat - 30.0) * 180.0 / 3.14159265;
                double d = std::fabs(std::fmod(r.towardDeg - b + 540.0, 360.0) - 180.0);
                if (d > 30.0) outward = false;
            }
            CHECK(recs.size() >= 8 && recs.size() <= 60 && mx > 30.0f && mx <= 42.5f && outward);
            CHECK(e.records(t + 900.0, 1.4).empty());
            {   // the cells do not move: later records sit on the same keys/positions (only their winds change)
                const auto later = e.records(t + 120.0, 1.4);
                bool same = true;
                for (const auto& r : later) { bool found = false; for (const auto& q : recs) if (q.key == r.key) { found = std::fabs(q.lat - r.lat) < 1e-9 && std::fabs(q.lon - r.lon) < 1e-9; break; } if (!found && !later.empty()) { /* new cell: fine */ } else if (!found) same = false; }
                CHECK(same && !later.empty());
            }
        }
        {   // gust front across the track: behind it the outflow, ahead of it nothing; FL350 nothing
            Engine e(1);
            e.force(kGustFront, 30.0, -90.0, 0.0, 0.0, eD);   // aircraft flying north, front 0 nm ahead at 30,-90, outflow toward the south
            const Vec3 behind = e.perturbation(30.0 + 2.0 / 60.0, -90.0, 800, 0.0), ahead = e.perturbation(30.0 - 2.0 / 60.0, -90.0, 800, 0.0);
            CHECK(-behind.north > 15 && std::fabs(ahead.north) < 3);
            CHECK(e.perturbation(30.0 + 2.0 / 60.0, -90.0, 35000, 0.0).north == 0);
            const auto gr = e.records(0.0, 1.0);
            bool south = !gr.empty();
            for (const auto& r : gr) if (std::fabs(std::fmod(r.towardDeg - 180.0 + 540.0, 360.0) - 180.0) > 20.0) south = false;
            CHECK(gr.size() >= 10 && south);   // cells behind the front, all blowing toward the south
        }
        {   // I: a weather refresh during an event (new station list, same evidence): no reset, no duplicate
            Engine e(5);
            double tForm = -1; uint32_t id = 0;
            for (double t = 0; t <= 50 * 3600.0 && tForm < 0; t += 30) { e.step(t, sD, eD); for (const auto& f : e.fields()) if (f.type == kGustFront) { tForm = t; id = f.id; } }
            CHECK(tForm >= 0);
            std::vector<Station> refreshed = tsSt; refreshed[0].report.gustKt = 24;
            Env eI = eD; eI.stations = &refreshed;
            const std::vector<Source> sI = assess(eI);
            e.step(tForm + 30, sI, eI);
            int n = 0; for (const auto& f : e.fields()) if (f.type == kGustFront) { ++n; CHECK(f.id == id); }
            CHECK(n == 1);
        }
        {   // K: the same weather gives the same fields whatever the evaluation rate (frame rate), and the seed decides them
            Engine a(9), b(9), c(10);
            std::vector<Source> many;
            for (int k = 0; k < 20; ++k) { Source s = sD[0]; s.key = "k" + std::to_string(k); s.lat += k * 0.2; many.push_back(s); }
            for (double t = 0; t <= 20 * 3600.0; t += 30) a.step(t, many, eD);
            for (double t = 0; t <= 20 * 3600.0; t += 0.7) b.step(t, many, eD);
            for (double t = 0; t <= 20 * 3600.0; t += 30) c.step(t, many, eD);
            auto sig = [](const Engine& e) { std::string s; for (const auto& f : e.fields()) s += f.source + ":" + std::to_string((long)f.t0) + ";"; return s; };
            CHECK(sig(a) == sig(b) && sig(a) != sig(c));
            // no frame-rate burst after a long jump in time, and nothing invalid comes out
            Engine j(9); j.step(0, many, eD); j.step(400000, many, eD);
            for (const auto& r : j.records(400000, 1.4)) CHECK(std::isfinite(r.lat) && std::isfinite(r.kt) && r.kt <= 60.0f && r.radiusNm > 0);
            CHECK(j.fields().size() <= 12);
            Engine nanE(1); CHECK(nanE.step(NAN, many, eD).size() == 1 && nanE.fields().empty());
        }
    }

    tt::run();
    th::run();
    to::run();
    tl::run();
    tsun::run();

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
