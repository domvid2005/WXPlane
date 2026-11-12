// Pilot report tests (included by test_wx.cpp). The JSON is real AWC API output from 2026-11-06,
// trimmed to a few entries.
#pragma once
#include "../src/hazards.h"

namespace th {
using namespace wx;

const char* kPireps = R"([
{"receiptTime":"2026-11-06T23:05:02.671Z","obsTime":1794005820,"qcField":0,"icaoId":"KWBC","acType":"A321","lat":44.3853,"lon":-74.2062,"fltLvl":290,"fltLvlType":"OTHER","clouds":null,"visib":null,"wxString":"","temp":null,"wdir":null,"wspd":null,"icgBas1":null,"icgTop1":null,"icgInt1":"","icgType1":"","icgBas2":null,"icgTop2":null,"icgInt2":"","icgType2":"","tbBas1":260,"tbTop1":290,"tbInt1":"LGT-MOD","tbType1":"CHOP","tbFreq1":"","tbBas2":null,"tbTop2":null,"tbInt2":"","tbType2":"","tbFreq2":"","vertGust":null,"brkAction":"","pirepType":"PIREP","rawOb":"SLK UA /OV SLK/TM 2257/FL290/TP A321/TB LGT-MOD CHOP 260-290"},
{"receiptTime":"2026-11-06T23:08:16.063Z","obsTime":1794006360,"qcField":0,"icaoId":"KMSC","acType":"CRJ7","lat":38.286,"lon":-76.4033,"fltLvl":130,"fltLvlType":"OTHER","clouds":null,"visib":null,"wxString":"","temp":null,"wdir":null,"wspd":null,"icgBas1":null,"icgTop1":null,"icgInt1":"","icgType1":"","icgBas2":null,"icgTop2":null,"icgInt2":"","icgType2":"","tbBas1":null,"tbTop1":null,"tbInt1":"MOD","tbType1":"","tbFreq1":"","tbBas2":null,"tbTop2":null,"tbInt2":"","tbType2":"","tbFreq2":"","vertGust":null,"brkAction":"","pirepType":"PIREP","rawOb":"NHK UA /OV NHK/TM 2306/FL130/TP CRJ7/TB MOD "},
{"receiptTime":"2026-11-06T23:08:15.644Z","obsTime":1794006300,"qcField":0,"icaoId":"KMSC","acType":"A321","lat":34.6959,"lon":-83.2977,"fltLvl":220,"fltLvlType":"DURD","clouds":null,"visib":null,"wxString":"","temp":null,"wdir":null,"wspd":null,"icgBas1":null,"icgTop1":null,"icgInt1":"","icgType1":"","icgBas2":null,"icgTop2":null,"icgInt2":"","icgType2":"","tbBas1":null,"tbTop1":null,"tbInt1":"NEG","tbType1":"","tbFreq1":"","tbBas2":null,"tbTop2":null,"tbInt2":"","tbType2":"","tbFreq2":"","vertGust":null,"brkAction":"","pirepType":"PIREP","rawOb":"1A5 UA /OV ODF/TM 2305/FL220/TP A321/TB NEG/RM DURD ZTLFD-49 "},
{"receiptTime":"2026-11-06T23:09:04.306Z","obsTime":1794006000,"qcField":0,"icaoId":"KWBC","acType":"UAL1463","lat":28.25,"lon":-74.1167,"fltLvl":320,"fltLvlType":"OTHER","clouds":null,"visib":null,"wxString":"","temp":-38,"wdir":309,"wspd":44,"icgBas1":null,"icgTop1":null,"icgInt1":"","icgType1":"","icgBas2":null,"icgTop2":null,"icgInt2":"","icgType2":"","tbBas1":null,"tbTop1":null,"tbInt1":"","tbType1":"","tbFreq1":"","tbBas2":null,"tbTop2":null,"tbInt2":"","tbType2":"","tbFreq2":"","vertGust":null,"brkAction":"","pirepType":"AIREP","rawOb":"ARP UAL1463 2815N 7407W 2300 F320 MS38 309/044KT"}
])";

// AWC data API answers of 2026-11-12 (trimmed): a convective SIGMET, an international SIGMET, G-AIRMETs (two forecast hours)
const char* kAirSigmet = R"([{"icaoId":"KKCI","alphaChar":"E","seriesId":"17E","receiptTime":"2026-11-12T03:46:19.302Z","creationTime":"2026-11-12T03:55:00.000Z","validTimeFrom":1794455700,"validTimeTo":1794462900,"airSigmetType":"SIGMET","hazard":"CONVECTIVE","altitudeHi1":32000,"altitudeHi2":32000,"altitudeLow1":null,"altitudeLow2":null,"movementDir":null,"movementSpd":0,"rawAirSigmet":"WSUS31 KKCI 120355
MKCE WST 120355
CONVECTIVE SIGMET 17E
VALID UNTIL 0555Z
FL AND CSTL WTRS
FROM 20SSE SRQ-40E RSW-70NW EYW-100SSW SRQ-20SSE SRQ
DMSHG AREA TS MOV LTL. TOPS TO FL320.","postProcessFlag":0,"severity":5,"coords":[{"lon":-82.404,"lat":27.094},{"lon":-81.036,"lat":26.528},{"lon":-82.712,"lat":25.411},{"lon":-83.242,"lat":25.856},{"lon":-82.404,"lat":27.094}]}])";
const char* kISigmet = R"([{"icaoId":"WAAA","firId":"WAAF","firName":"WAAF UJUNG PANDANG","receiptTime":"2026-11-11T22:25:00.793Z","validTimeFrom":1794435600,"validTimeTo":1794457200,"seriesId":"27","hazard":"VA","qualifier":"ERUPTION MT DUKONO","base":0,"top":9000,"geom":"AREA","coords":[{"lon":127.967,"lat":1.7},{"lon":127.467,"lat":1.133},{"lon":127.083,"lat":1.283},{"lon":127.117,"lat":1.65},{"lon":127.917,"lat":1.8},{"lon":127.967,"lat":1.7}],"dir":"SW","spd":"10","chng":"INTSF","rawSigmet":"WVID21 WAAA 112220
WAAF SIGMET 27 VALID 112220/120420 WAAA-"},
{"icaoId":"EGRR","firId":"EGTT","firName":"EGTT LONDON","validTimeFrom":1794435600,"validTimeTo":1794457200,"seriesId":"3","hazard":"TS","qualifier":"EMBD","base":null,"top":35000,"geom":"AREA","coords":[{"lon":-1,"lat":51},{"lon":1,"lat":51},{"lon":1,"lat":52},{"lon":-1,"lat":51}],"rawSigmet":"EGTT SIGMET 3"},
{"icaoId":"XXXX","hazard":"TC","geom":"POINT","coords":[{"lon":130,"lat":15}]}])";
const char* kGAirmet = R"([{"tag":"1C","forecastHour":0,"validTime":"2026-11-12T03:00:00.000Z","hazard":"FZLVL","geometryType":"LINE","product":"ZULU","level":"SFC","issueTime":1794451500,"expireTime":1794474000,"coords":[{"lat":"56.13","lon":"-128.33"},{"lat":"55.88","lon":"-127.83"}]},
{"tag":"2S","forecastHour":0,"validTime":"2026-11-12T03:00:00.000Z","hazard":"IFR","geometryType":"AREA","product":"SIERRA","due_to":"CIG BLW 010/VIS BLW 3SM BR","severity":null,"base":null,"top":null,"issueTime":1794451500,"expireTime":1794474000,"coords":[{"lat":"42.0","lon":"-71.0"},{"lat":"42.0","lon":"-70.0"},{"lat":"41.0","lon":"-70.0"},{"lat":"42.0","lon":"-71.0"}]},
{"tag":"3T","forecastHour":0,"validTime":"2026-11-12T03:00:00.000Z","hazard":"TURB-HI","geometryType":"AREA","product":"TANGO","severity":"MOD","base":"240","top":"390","issueTime":1794451500,"expireTime":1794474000,"coords":[{"lat":"45","lon":"-75"},{"lat":"45","lon":"-70"},{"lat":"43","lon":"-70"},{"lat":"45","lon":"-75"}]},
{"tag":"3T","forecastHour":3,"validTime":"2026-11-12T06:00:00.000Z","hazard":"TURB-HI","geometryType":"AREA","product":"TANGO","severity":"MOD","base":"240","top":"390","coords":[{"lat":"45","lon":"-75"},{"lat":"45","lon":"-70"},{"lat":"43","lon":"-70"},{"lat":"45","lon":"-75"}]}])";

void run() {
    using namespace hz;
    {   // AIRMETs, SIGMETs and convective SIGMETs for the radar map
        std::string e;
        auto a = parseAirSigmets(kAirSigmet, &e);
        CHECK(a.size() == 1 && a[0].cls == Advisory::Convective && a[0].name == "Convective SIGMET 17E" && a[0].topFt == 32000 && a[0].baseFt < 0);
        CHECK(a[0].pts.size() == 5 && !a[0].line && a[0].from == 1794455700 && a[0].to == 1794462900 && a[0].raw.find("TOPS TO FL320") != std::string::npos);
        auto i = parseISigmets(kISigmet, &e);
        CHECK(i.size() == 2);   // the point SIGMET has no outline
        CHECK(i[0].cls == Advisory::Sigmet && i[0].hazard == "VA" && i[0].qualifier == "ERUPTION MT DUKONO" && i[0].baseFt == 0 && i[0].topFt == 9000 && i[0].movement == "MOV SW 10 KT INTSF");
        CHECK(i[1].cls == Advisory::Convective && i[1].name.find("EGTT LONDON") != std::string::npos);
        const std::time_t at = 1794456000;   // 03:20Z: the 03Z forecast hour
        auto g = parseGAirmets(kGAirmet, at, &e);
        CHECK(g.size() == 2);   // the freezing level line and the 06Z hour are left out
        CHECK(g[0].cls == Advisory::Airmet && g[0].hazard == "IFR" && g[0].qualifier == "CIG BLW 010/VIS BLW 3SM BR" && g[0].pts.size() == 4);
        CHECK(g[1].hazard == "TURB-HI" && g[1].baseFt == 24000 && g[1].topFt == 39000 && g[1].qualifier == "MOD");
        CHECK(parseAirSigmets("[]", &e).empty() && parseGAirmets("not json", at, &e).empty());
    }
    const std::time_t now = 1794006600;   // 2026-11-06 23:10Z
    std::string err;
    auto ps = parsePireps(kPireps, &err);
    CHECK(ps.size() == 4);
    CHECK(ps[0].intensity == "LGT-MOD" && nr(ps[0].baseFt, 26000, 1e-6) && nr(ps[0].topFt, 29000, 1e-6) && nr(ps[0].altFt, 29000, 1e-6) && ps[0].acType == "A321" && ps[0].tbType == "CHOP");
    CHECK(ps[1].intensity == "MOD" && ps[1].baseFt < 0 && ps[1].obsTime == 1794006360);
    CHECK(ps[3].kind == "AIREP" && ps[3].intensity.empty());
    // the reporter's size (the owner's rule: unknown type -> lightest class)
    CHECK(std::string(reporterClass("C172").name) == "light" && nr(reporterClass("C172").factor, 0.6, 1e-9));
    CHECK(std::string(reporterClass("A320").name) == "medium" && std::string(reporterClass("A20N").name) == "medium" && std::string(reporterClass("B38M").name) == "medium");
    CHECK(std::string(reporterClass("B77W").name) == "heavy" && std::string(reporterClass("A359").name) == "heavy");
    CHECK(std::string(reporterClass("CRJ7").name) == "regional" && std::string(reporterClass("C56X").name) == "regional" && std::string(reporterClass("C130").name) == "regional");
    CHECK(nr(reporterClass("").factor, 0.6, 1e-9) && nr(reporterClass("UNKN").factor, 0.6, 1e-9) && nr(reporterClass("UAL1463").factor, 0.6, 1e-9) && nr(reporterClass("ZZZZ").factor, 0.6, 1e-9));
    // an extreme report from a C172 is less air than a moderate one from an A320
    CHECK(edrForIntensity("EXTRM", "C172") < edrForIntensity("MOD-SEV", "A320"));
    NEAR(edrForIntensity("MOD", "A320"), 0.30, 1e-9); NEAR(edrForIntensity("MOD", "B744"), 0.375, 1e-9); NEAR(edrForIntensity("MOD", "PA28"), 0.18, 1e-9);
    CHECK(edrForIntensity("NEG", "A320") == 0 && edrForIntensity("", "A320") == 0);

    CHECK(parsePireps("", &err).empty());   // HTTP 204 = nothing
    CHECK(parsePireps("<html>", &err).empty());

    Snapshot s; s.pireps = ps;
    // PIREP floors: at the report, inside its layer; fading with distance, height and age
    Floor f = floorAt(s, 44.3853, -74.2062, 27000, now);
    NEAR(f.edr, 0.22, 1e-9);
    CHECK(f.why.find("LGT-MOD") != std::string::npos && f.why.find("medium") != std::string::npos);
    CHECK(floorAt(s, 44.3853, -74.2062, 33000, now).edr == 0);     // 4,000 ft above the reported layer
    CHECK(floorAt(s, 44.3853, -74.2062, 30000, now).edr < 0.22);   // in the 2,000 ft taper
    double d30 = floorAt(s, 44.3853 + 30.0 / 60.0, -74.2062, 27000, now).edr;   // 30 nm north
    CHECK(d30 > 0.05 && d30 < 0.15);
    CHECK(floorAt(s, 44.3853 + 1.0, -74.2062, 27000, now).edr == 0);            // 60 nm
    CHECK(floorAt(s, 44.3853, -74.2062, 27000, now + 3 * 3600).edr == 0);       // 3 h later
    NEAR(floorAt(s, 38.286, -76.4033, 13000, now).edr, 0.30 * 0.85, 1e-9);     // MOD from a CRJ7
    CHECK(floorAt(s, 34.6959, -83.2977, 22000, now).edr == 0);                  // NEG raises nothing
    // low-level wind shear reports: LLWS remark, urgent flag; near a point below 4,000 ft above the ground
    {
        const char* kLlws = R"([{"obsTime":1794006000,"acType":"B738","lat":40.64,"lon":-73.78,"fltLvl":8,"tbBas1":null,"tbTop1":null,"tbInt1":"","tbInt2":"","pirepType":"PIREP","rawOb":"JFK UUA /OV JFK/TM 2300/FL008/TP B738/RM LLWS +/-15KT SFC-008 DURGC RWY22L"},
{"obsTime":1794006000,"acType":"A320","lat":40.70,"lon":-73.80,"fltLvl":340,"tbBas1":null,"tbTop1":null,"tbInt1":"","tbInt2":"","pirepType":"PIREP","rawOb":"JFK UA /OV JFK/TM 2300/FL340/TP A320/RM LLWS"}])";
        Snapshot w; w.pireps = parsePireps(kLlws, &err);
        CHECK(w.pireps.size() == 2 && w.pireps[0].llws && w.pireps[0].urgent && w.pireps[1].llws && !w.pireps[1].urgent);
        const Pirep* near1 = llwsNear(w, 40.70, -73.70, 13, now);
        CHECK(near1 && near1->acType == "B738");                       // the low report, 5 nm away
        CHECK(llwsNear(w, 41.10, -73.70, 13, now) == nullptr);           // 28 nm away
        CHECK(llwsNear(w, 40.70, -73.70, 13, now + 2 * 3600) == nullptr); // 2 h later
        CHECK(!ps[0].llws);
    }
}
} // namespace th
