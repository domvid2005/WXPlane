#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "../src/geo.h"
#include "../src/json_mini.h"
#include "../src/metar.h"
#include "../src/radar.h"
#include "../src/sources.h"
#include "../src/viewer.h"
#include "../src/windsaloft.h"
#include "../src/wxmodel.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool nr(double a, double b, double t) { return std::fabs(a - b) <= t; }
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

    Station dfw;
    dfw.elevM = 185;
    parseReport("KDFW 301553Z 18015G30KT 3SM +TSRA BKN020CB OVC040 BKN150 BKN250 24/22 A2985", dfw.report);
    XpWx t = buildXp(dfw, cfg, now);
    CHECK(t.cloudCount == 3 && t.clouds[0].type == 3.0f); // 3 slots, lowest first
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
        CHECK(v6.lowCloudType == 3.0 && v6.stationRadiusFactor == 0.49 && v6.fillRadiusFactor == 1.0 && v6.radarMinutes == 5 && v6.searchNm == 80);   // stale cloud settings from earlier builds reset once
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
        Report ar; parseReport("KDEN ATIS INFO B 1553Z 18008KT 4SM BR FEW008 BKN250 08/07 A3015 (THREE ZERO ONE FIVE). ILS RWY 16R.", ar, true);
        CHECK(ar.hasVis && ar.mist);
        mergeAtis(v, ar, "x");
        NEAR(buildXp(v, cfg, now).visM, 4 * kSmToM, 1);
    }

    // --- cloud layers never overlap vertically: every top is at least 150 m below the next base
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
                    ok = ok && a.clouds[i].topM + (a.clouds[i].baseM - 100.0f < 100.0f ? 39.0f : 149.0f) <= a.clouds[i + 1].baseM && a.clouds[i].topM > a.clouds[i].baseM;
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
    // --- mist (BR) shows as a thin ground layer, and not only as reduced visibility
    {
        Station st; st.elevM = 400;
        parseReport("KICT 301553Z 00000KT 3SM BR FEW002 12/11 A3002", st.report);
        CHECK(stationHasFog(st, cfg));
        XpWx a = buildXp(st, cfg, now);
        CHECK(a.fogLayerApplied && a.clouds[0].baseM == 400 && a.clouds[0].topM - a.clouds[0].baseM >= 80 && a.clouds[0].topM - a.clouds[0].baseM <= 120 && a.clouds[0].coverage < 0.6f);
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
        Config rc; rc.stormRim = false;
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
            Config hc; hc.stormRim = false; hc.radarCellNm = 10; hc.radarRangeNm = 100; hc.radarMaxCells = 100; hc.radarMinDbz = 20;
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
        CHECK(st.clouds[2].coverage > 0.7f && st.clouds[2].coverage <= 0.95f && st.clouds[2].type == 3.0f && st.clouds[2].topM > 11000 && st.clouds[2].topM <= tropopauseKm(38) * 1000 + 2500 + 1 && st.clouds[2].baseM < 1500);
        CHECK(st.precip >= 0.99f && st.visM <= 800 && st.cloudCount == 2 && st.maxAltFt >= st.clouds[2].topM / 0.3048 - 1);
        CHECK(st.turbulence > 0.5f && st.gustMs > st.windMs + 5);
        CHECK(st.clouds[0].coverage == 0);                           // slot 0 is left for fog
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
        XpWx w4 = base; sc.radarTurbulence = 0; cell.dbz = 55; applyStorm(w4, cell, sc, 400, 38);
        CHECK(w4.turbulence == 0);

        // RainViewer-style tiles (z=6): one tile around 35N 97W with a dark blob
        int x0, x1, y0, y1;
        tileRange(35.0, -97.0, 100, 6, x0, x1, y0, y1);
        CHECK(x0 <= x1 && y0 <= y1 && (x1 - x0 + 1) * (y1 - y0 + 1) <= 4);
        TileImage t; t.z = 6; t.x = (int)std::floor((-97.0 + 180.0) / 360.0 * 64); t.w = t.h = 256;
        { double r = 35.0 * 3.14159265358979 / 180; t.y = (int)std::floor((1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / 3.14159265358979) / 2.0 * 64); }
        t.rgba.assign(256 * 256 * 4, 0);
        double fx = (-97.0 + 180.0) / 360.0 * 64, fy; { double r = 35.0 * 3.14159265358979 / 180; fy = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / 3.14159265358979) / 2.0 * 64; }
        int cx = (int)((fx - t.x) * 256), cy = (int)((fy - t.y) * 256);
        for (int dy = -6; dy <= 6; ++dy) for (int dx = -6; dx <= 6; ++dx) { uint8_t* p = &t.rgba[((size_t)(cy + dy) * 256 + (size_t)(cx + dx)) * 4]; p[0] = 20; p[1] = 20; p[2] = 90; p[3] = 255; }
        GribWindow gw; gw.latMin = 34; gw.latMax = 36; gw.lonMin = -98; gw.lonMax = -96;
        GribField tf;
        CHECK(tilesToField({t}, gw, tf) && tf.valid());
        CHECK(tf.at(35.0, -97.0) > 30 && tf.at(34.2, -97.9) < -50);   // blob found at the right place, elsewhere empty
        CHECK(dbzFromRainViewerPixel(0, 0, 0, 0) < -50 && dbzFromRainViewerPixel(20, 20, 90, 255) > dbzFromRainViewerPixel(200, 220, 255, 255));
        std::FILE* pf = std::fopen("wx_pal_test.csv", "w");
        std::fputs("# r,g,b,dbz\n200,220,255,15\n20,20,90,45\n", pf); std::fclose(pf);
        CHECK(loadRainViewerPalette("wx_pal_test.csv") == 2);
        CHECK(dbzFromRainViewerPixel(22, 18, 95, 255) == 45 && dbzFromRainViewerPixel(198, 221, 250, 255) == 15 && dbzFromRainViewerPixel(0, 255, 0, 255) < -50);
        loadRainViewerPalette("/nonexistent");                         // unreadable file leaves the palette alone
        CHECK(dbzFromRainViewerPixel(22, 18, 95, 255) == 45);

        CHECK(formatFl(11900, true) == "FL390" && formatFl(12000, false) == "~FL390" && formatFl(4700, true) == "FL150" && formatFl(0, true) == "FL000");

        // viewer text
        FetchResult fr; fr.fetchedAt = now; fr.radar = rr; fr.radar.source = "NOAA MRMS (CONUS)"; fr.radar.dataTime = now - 240;
        Station vs; vs.icao = "KVVV"; vs.rawMetar = "KVVV 301553Z 00000KT 10SM CLR 20/10 A3010"; parseReport(vs.rawMetar, vs.report); fr.stations = {vs};
        auto lines = buildViewLines(fr, true, sc, now);
        std::string all; for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar: NOAA MRMS (CONUS)") != std::string::npos && all.find("4 min old") != std::string::npos && all.find("dBZ") != std::string::npos);
        fr.radar = RadarResult(); fr.radar.note = "no NOAA radar coverage here";
        lines = buildViewLines(fr, true, sc, now);
        all.clear(); for (auto& l : lines) all += l.text + "\n";
        CHECK(all.find("Radar: none") != std::string::npos && all.find("no NOAA radar coverage here") != std::string::npos);
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
        CHECK(all.find("ABI") != std::string::npos);
        CHECK(all.find("ATIS   KAAA") == std::string::npos);            // no ATIS block for a station without one
        CHECK(buildViewLines(fr, false, cfg, now).size() == 2);         // nothing downloaded yet
        auto w = wrapText("aaaa bbbb cccc dddd eeee ffff", 10);
        CHECK(w.size() == 3 && w[0] == "aaaa bbbb");
        CHECK(wrapText("", 10).size() == 1);
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
            wk.dbz = 36; XpWx ms = ob; applyStorm(ms, wk, oc, 300, 38);
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

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
