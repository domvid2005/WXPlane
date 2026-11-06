// Pilot report / SIGMET / AIRMET tests (included by test_wx.cpp). The JSON is real AWC API output from 2026-11-06,
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
const char* kAirSigmet = R"([{"icaoId":"KKCI","alphaChar":"E","seriesId":"64E","receiptTime":"2026-11-06T22:53:13.130Z","creationTime":"2026-11-06T22:55:00.000Z","validTimeFrom":1794005700,"validTimeTo":1794012900,"airSigmetType":"SIGMET","hazard":"CONVECTIVE","altitudeHi1":35000,"altitudeHi2":35000,"altitudeLow1":null,"altitudeLow2":null,"movementDir":230,"movementSpd":40,"rawAirSigmet":"WSUS31 KKCI 062255\nMKCE WST 062255\nCONVECTIVE SIGMET 64E\nVALID UNTIL 0055Z","postProcessFlag":0,"severity":5,"coords":[{"lon":-75.219,"lat":38.585},{"lon":-74.671,"lat":35.72},{"lon":-76.37,"lat":36.185},{"lon":-76.586,"lat":37.014},{"lon":-75.219,"lat":38.585}]}])";
const char* kIsigmet = R"([{"icaoId":"ZSSS","firId":"ZSHA","firName":"ZSHA SHANGHAI","receiptTime":"2026-11-06T19:02:33.851Z","validTimeFrom":1793993400,"validTimeTo":1794007800,"seriesId":"12","hazard":"TURB","qualifier":"SEV","base":13000,"top":36000,"geom":"AREA","coords":[{"lon":122.995,"lat":28},{"lon":124,"lat":29},{"lon":124,"lat":30.02},{"lon":124,"lat":38},{"lon":123,"lat":38.25},{"lon":120,"lat":38.25},{"lon":118.49,"lat":37.8},{"lon":117.49,"lat":37.46},{"lon":116.18,"lat":36.95},{"lon":115.35,"lat":36.67},{"lon":115.4,"lat":35.9},{"lon":115.42,"lat":35.14},{"lon":115.93,"lat":34.5},{"lon":115.77,"lat":32.57},{"lon":115.88,"lat":31.28},{"lon":115.95,"lat":30.06},{"lon":114.57,"lat":29.06},{"lon":114.303,"lat":28},{"lon":122.995,"lat":28}],"dir":"E","spd":"10","chng":"NC","rawSigmet":"WSCI34 ZSSS 061901\nZSHA SIGMET 12 VALID 061930/062330 ZSSS-\nZSHA SHANGHAI FIR SEV TURB FCST\nN OF N28 FL130/360\nMOV E 20KMH NC="}])";
const char* kGairmet = R"([
{"tag":"5W","forecastHour":3,"validTime":"2026-11-07T00:00:00.000Z","hazard":"TURB-LO","geometryType":"AREA","latlonpairs":14,"frequency":null,"severity":"MOD","due_to":null,"status":"NRML","top":"130","base":"SFC","fzltop":null,"fzlbase":null,"level":null,"receiptTime":1794002216,"issueTime":1794002160,"expireTime":1794020400,"product":"TANGO","geom":"AREA","coords":[{"lat":"48.96","lon":"-119.69"},{"lat":"48.77","lon":"-119.62"},{"lat":"48.09","lon":"-119.97"},{"lat":"47.15","lon":"-120.45"},{"lat":"46.09","lon":"-121.22"},{"lat":"45.34","lon":"-122.30"},{"lat":"44.84","lon":"-123.62"},{"lat":"44.65","lon":"-124.46"},{"lat":"44.99","lon":"-124.96"},{"lat":"45.94","lon":"-124.27"},{"lat":"47.75","lon":"-124.07"},{"lat":"48.42","lon":"-124.81"},{"lat":"48.94","lon":"-122.22"},{"lat":"48.96","lon":"-119.69"}]},
{"tag":"5W","forecastHour":6,"validTime":"2026-11-07T03:00:00.000Z","hazard":"TURB-LO","geometryType":"AREA","latlonpairs":4,"frequency":null,"severity":"MOD","due_to":null,"status":"NRML","top":"100","base":"SFC","product":"TANGO","geom":"AREA","expireTime":1794020400,"coords":[{"lat":"40","lon":"-100"},{"lat":"41","lon":"-100"},{"lat":"41","lon":"-99"},{"lat":"40","lon":"-100"}]},
{"tag":"6W","forecastHour":3,"validTime":"2026-11-07T00:00:00.000Z","hazard":"LLWS","geometryType":"AREA","latlonpairs":5,"frequency":null,"severity":null,"due_to":"AIR SPEED LOSS OR GAIN OF 20KTS OR MORE BELOW 2000 FT AGL","status":"NRML","top":null,"base":null,"product":"TANGO","geom":"AREA","expireTime":1794020400,"coords":[{"lat":"30","lon":"-90"},{"lat":"31","lon":"-90"},{"lat":"31","lon":"-89"},{"lat":"30","lon":"-90"}]}
])";

void run() {
    using namespace hz;
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

    auto us = parseAirSigmets(kAirSigmet, &err);
    CHECK(us.size() == 1 && us[0].product == "CONVECTIVE SIGMET" && us[0].id == "64E" && nr(us[0].topFt, 35000, 1e-6) && us[0].baseFt < 0 && us[0].pts.size() == 5);
    CHECK(us[0].contains(37.0, -75.5) && !us[0].contains(39.5, -75.5) && !us[0].isTurbulence());
    auto in = parseIntlSigmets(kIsigmet, &err);
    CHECK(in.size() == 1 && in[0].hazard == "TURB" && in[0].severity == "SEV" && nr(in[0].baseFt, 13000, 1e-6) && nr(in[0].topFt, 36000, 1e-6) && in[0].isTurbulence());
    CHECK(in[0].contains(33.0, 120.0) && !in[0].contains(25.0, 120.0));
    auto ga = parseGairmets(kGairmet, now, &err);
    CHECK(ga.size() == 2);   // the 00Z snapshot (nearest to 23:10Z) only
    CHECK(ga[0].product == "G-AIRMET TANGO" && ga[0].hazard == "TURB-LO" && ga[0].severity == "MOD" && ga[0].baseFt < 0 && nr(ga[0].topFt, 13000, 1e-6));
    CHECK(ga[0].contains(47.0, -122.0) && ga[1].hazard == "LLWS");
    CHECK(parsePireps("", &err).empty() && parseAirSigmets("", &err).empty());   // HTTP 204 = nothing
    CHECK(parsePireps("<html>", &err).empty());

    Snapshot s; s.pireps = ps; s.areas = us; s.areas.insert(s.areas.end(), in.begin(), in.end()); s.areas.insert(s.areas.end(), ga.begin(), ga.end());
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
    // SIGMET and AIRMET floors
    NEAR(floorAt(s, 33.0, 120.0, 30000, now).edr, 0.30, 1e-9);
    CHECK(floorAt(s, 33.0, 120.0, 10000, now).edr == 0);                        // below FL130
    CHECK(floorAt(s, 33.0, 120.0, 30000, 1794007800 + 60).edr == 0);            // expired
    NEAR(floorAt(s, 47.0, -122.0, 8000, now).edr, 0.18, 1e-9);                  // Tango TURB-LO, SFC-130
    CHECK(floorAt(s, 47.0, -122.0, 20000, now).edr == 0);
    CHECK(floorAt(s, 37.0, -75.5, 20000, now).edr == 0);                        // a convective SIGMET is not a turbulence floor here
}
} // namespace th
