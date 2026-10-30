#include "metar.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace wx {
namespace {

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        while (!cur.empty() && std::strchr(".,;:)", cur.back())) cur.pop_back();
        while (!cur.empty() && cur.front() == '(') cur.erase(cur.begin());
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (std::isspace((unsigned char)c)) flush();
        else cur += (char)std::toupper((unsigned char)c);
    }
    flush();
    return out;
}

bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool inList(const std::string& c, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (c == list[i]) return true;
    return false;
}

// Present-weather group, e.g. -RA, +TSRA, FZFG, BCFG, VCSH
bool parseWx(const std::string& tok, Report& r) {
    static const char* const desc[] = {"MI", "BC", "PR", "DR", "BL", "SH", "TS", "FZ"};
    static const char* const phen[] = {"DZ", "RA", "SN", "SG", "IC", "PL", "GR", "GS", "UP", "BR", "FG",
                                       "FU", "VA", "DU", "SA", "HZ", "PY", "PO", "SQ", "FC", "SS", "DS"};
    size_t i = 0;
    char inten = 0;
    bool vicinity = false;
    if (tok.size() < 2) return false;
    if (tok[0] == '+' || tok[0] == '-') { inten = tok[0]; i = 1; }
    else if (tok.compare(0, 2, "VC") == 0) { vicinity = true; i = 2; }

    std::vector<std::string> ds, ps;
    while (i + 2 <= tok.size()) {
        std::string c = tok.substr(i, 2);
        if (ps.empty() && inList(c, desc, 8)) ds.push_back(c);
        else if (inList(c, phen, sizeof(phen) / sizeof(phen[0]))) ps.push_back(c);
        else return false;
        i += 2;
    }
    if (i != tok.size()) return false;
    auto hasD = [&](const char* d) { return std::find(ds.begin(), ds.end(), d) != ds.end(); };
    auto hasP = [&](const char* p) { return std::find(ps.begin(), ps.end(), p) != ps.end(); };
    if (ps.empty() && !hasD("TS")) return false;

    if (!r.wxText.empty()) r.wxText += ' ';
    r.wxText += tok;
    if (vicinity) return true; // "in the vicinity" does not change conditions at the station

    if (hasD("TS")) r.thunder = true;
    if (hasD("FZ")) r.freezing = true;
    if (hasP("FG")) {
        if (hasD("MI")) r.shallowFog = true;
        else if (hasD("BC") || hasD("PR")) r.patchyFog = true;
        else r.fog = true;
    }
    if (hasP("BR")) r.mist = true;
    if (hasP("HZ") || hasP("FU") || hasP("DU") || hasP("SA") || hasP("VA")) r.haze = true;

    double base = 0;
    for (const auto& p : ps) {
        double rate = 0;
        if (p == "DZ") rate = 0.15;
        else if (p == "RA") rate = 0.45;
        else if (p == "SN") { rate = 0.45; r.snow = true; }
        else if (p == "GR" || p == "GS") rate = 0.5;
        else if (p == "SG" || p == "IC" || p == "PL") { rate = 0.3; r.snow = true; }
        else if (p == "UP") rate = 0.3;
        base = std::max(base, rate);
    }
    if (base > 0) {
        if (inten == '-') base *= 0.5;
        else if (inten == '+') base *= 1.8;
        if (hasD("SH")) base *= 1.2;
        r.precip = std::max(r.precip, std::min(1.0, base));
    }
    if (r.thunder) r.precip = std::max(r.precip, 0.7);
    return true;
}

double smValue(const std::string& tok, bool& ok) {
    // [M|P]N[/D]SM
    static const std::regex re(R"(^([MP]?)(\d+)(?:/(\d+))?SM$)");
    std::smatch m;
    ok = false;
    if (!std::regex_match(tok, m, re)) return 0;
    double v = std::atof(m[2].str().c_str());
    if (m[3].matched) {
        double d = std::atof(m[3].str().c_str());
        if (d <= 0) return 0;
        v /= d;
    }
    if (m[1].str() == "M") v *= 0.9; // "less than"
    ok = true;
    return v;
}

} // namespace

bool parseReport(const std::string& text, Report& r, bool isAtis) {
    std::vector<std::string> t = tokenize(text);
    if (t.empty()) return false;
    r = Report();

    size_t i = 0;
    while (i < t.size() && (t[i] == "METAR" || t[i] == "SPECI")) ++i;
    if (i < t.size() && t[i].size() == 4 && std::all_of(t[i].begin(), t[i].end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
        r.icao = t[i];
        ++i;
    }

    static const std::regex reTime(R"(^(\d{2})(\d{2})(\d{2})Z$)");
    static const std::regex reTimeShort(R"(^(\d{2})(\d{2})Z$)");
    static const std::regex reWind(R"(^(\d{3}|VRB)(\d{2,3})(?:G(\d{2,3}))?(KT|MPS|KMH)$)");
    static const std::regex reCloud(R"(^(FEW|SCT|BKN|OVC)(\d{3}|///)(CB|TCU)?$)");
    static const std::regex reVV(R"(^VV(\d{3}|///)$)");
    static const std::regex reTemp(R"(^(M?)(\d{2})/(M?)(\d{2})?$)");
    static const std::regex reAltA(R"(^A(\d{4})$)");
    static const std::regex reAltQ(R"(^Q(\d{4})$)");

    bool sawTime = false;
    for (; i < t.size(); ++i) {
        const std::string& tok = t[i];
        if (tok == "RMK" || tok == "TEMPO" || tok == "BECMG") break;
        std::smatch m;

        if (!sawTime && std::regex_match(tok, m, reTime)) {
            r.day = std::atoi(m[1].str().c_str());
            r.hour = std::atoi(m[2].str().c_str());
            r.minute = std::atoi(m[3].str().c_str());
            sawTime = true;
            continue;
        }
        if (!sawTime && isAtis && std::regex_match(tok, m, reTimeShort)) {
            r.hour = std::atoi(m[1].str().c_str());
            r.minute = std::atoi(m[2].str().c_str());
            sawTime = true;
            continue;
        }
        if (isAtis && tok == "INFO" && i + 1 < t.size() && t[i + 1].size() == 1 && std::isalpha((unsigned char)t[i + 1][0]) && r.infoLetter.empty()) {
            r.infoLetter = t[i + 1];
            continue;
        }
        if (!r.hasWind && std::regex_match(tok, m, reWind)) {
            r.hasWind = true;
            double mult = 1.0;
            if (m[4].str() == "MPS") mult = 1.0 / kKtToMs;
            else if (m[4].str() == "KMH") mult = 0.539957;
            r.windKt = std::atof(m[2].str().c_str()) * mult;
            if (m[3].matched) r.gustKt = std::atof(m[3].str().c_str()) * mult;
            if (m[1].str() == "VRB") { r.windVariable = true; r.windDir = 0; }
            else r.windDir = std::atoi(m[1].str().c_str());
            continue;
        }
        if (r.hasWind && !r.hasVis && !r.windVariable && tok.size() == 7 && tok[3] == 'V' && isDigits(tok.substr(0, 3)) && isDigits(tok.substr(4))) {
            r.windVariable = true; // dddVddd
            continue;
        }
        if (!r.hasVis) {
            if (tok == "CAVOK") {
                r.hasVis = true; r.cavok = true; r.visM = 10000; r.clearSky = true;
                continue;
            }
            bool ok = false;
            double sm = smValue(tok, ok);
            if (ok) {
                if (i > 0 && t[i - 1].size() == 1 && std::isdigit((unsigned char)t[i - 1][0])) sm += t[i - 1][0] - '0';
                r.hasVis = true;
                r.visM = sm * kSmToM;
                if (tok[0] == 'P') r.visM = std::max(r.visM, 10.0 * kSmToM);
                continue;
            }
            std::string core = tok;
            if (core.size() == 7 && core.compare(4, 3, "NDV") == 0) core.resize(4);
            if (r.hasWind && r.clouds.empty() && !r.hasTemp && core.size() == 4 && isDigits(core)) {
                r.hasVis = true;
                r.visM = std::atof(core.c_str());
                if (r.visM >= 9999) r.visM = 10000;
                continue;
            }
        }
        if (tok.size() > 2 && (tok[0] == 'R') && std::isdigit((unsigned char)tok[1]) && tok.find('/') != std::string::npos) continue; // RVR
        if (tok == "SKC" || tok == "CLR" || tok == "NSC" || tok == "NCD") { r.clearSky = true; continue; }
        if (std::regex_match(tok, m, reCloud)) {
            CloudGroup g;
            const std::string c = m[1].str();
            g.cover = c == "FEW" ? CloudGroup::FEW : c == "SCT" ? CloudGroup::SCT : c == "BKN" ? CloudGroup::BKN : CloudGroup::OVC;
            if (m[2].str() == "///") continue;
            g.baseFtAgl = std::atoi(m[2].str().c_str()) * 100;
            g.kind = m[3].matched ? (m[3].str() == "CB" ? 1 : 2) : 0;
            r.clouds.push_back(g);
            continue;
        }
        if (std::regex_match(tok, m, reVV)) {
            if (m[1].str() != "///") {
                CloudGroup g;
                g.cover = CloudGroup::VV;
                g.baseFtAgl = std::atoi(m[1].str().c_str()) * 100;
                r.clouds.push_back(g);
            }
            continue;
        }
        if (!r.hasTemp && std::regex_match(tok, m, reTemp)) {
            double tc = std::atof(m[2].str().c_str()) * (m[1].str() == "M" ? -1 : 1);
            bool hasD = m[4].matched && !m[4].str().empty();
            double dc = hasD ? std::atof(m[4].str().c_str()) * (m[3].str() == "M" ? -1 : 1) : 0;
            if (hasD && dc > tc + 0.5) continue; // e.g. runway pair "04/22" in ATIS text
            r.hasTemp = true; r.tempC = tc;
            if (hasD) { r.hasDewp = true; r.dewpC = dc; }
            continue;
        }
        if (!r.hasAltim && std::regex_match(tok, m, reAltA)) {
            double hpa = std::atof(m[1].str().c_str()) / 100.0 * kInHgToHpa;
            if (hpa > 850 && hpa < 1090) { r.hasAltim = true; r.altimHpa = hpa; }
            continue;
        }
        if (!r.hasAltim && std::regex_match(tok, m, reAltQ)) {
            double hpa = std::atof(m[1].str().c_str());
            if (hpa > 850 && hpa < 1090) { r.hasAltim = true; r.altimHpa = hpa; }
            continue;
        }
        parseWx(tok, r);
    }

    if (isAtis) {
        // Spoken-format fallbacks used by some ATIS texts.
        std::string up;
        for (const auto& tk : t) { up += tk; up += ' '; }
        std::smatch m;
        if (!r.hasAltim && std::regex_search(up, m, std::regex(R"(ALTIMETER (\d{4}) )"))) {
            double hpa = std::atof(m[1].str().c_str()) / 100.0 * kInHgToHpa;
            if (hpa > 850 && hpa < 1090) { r.hasAltim = true; r.altimHpa = hpa; }
        }
        if (!r.hasWind && std::regex_search(up, m, std::regex(R"(WIND (\d{3}) AT (\d{1,3})(?: GUSTS? (\d{1,3}))? )"))) {
            r.hasWind = true;
            r.windDir = std::atoi(m[1].str().c_str());
            r.windKt = std::atof(m[2].str().c_str());
            if (m[3].matched) r.gustKt = std::atof(m[3].str().c_str());
        }
        if (!r.hasTemp && std::regex_search(up, m, std::regex(R"(TEMPERATURE (M|MINUS )?(\d{1,2}),? DEW ?POINT (M|MINUS )?(\d{1,2}) )"))) {
            r.hasTemp = r.hasDewp = true;
            r.tempC = std::atof(m[2].str().c_str()) * (m[1].matched && !m[1].str().empty() ? -1 : 1);
            r.dewpC = std::atof(m[4].str().c_str()) * (m[3].matched && !m[3].str().empty() ? -1 : 1);
        }
        if (!r.hasVis && std::regex_search(up, m, std::regex(R"(VISIBILITY (\d{1,2}) )"))) {
            r.hasVis = true;
            r.visM = std::atof(m[1].str().c_str()) * kSmToM;
        }
    }

    return r.hasWind || r.hasVis || r.hasTemp || r.hasAltim || !r.clouds.empty() || r.clearSky;
}

double reportAgeSeconds(const Report& r, std::time_t now) {
    if (r.hour < 0) return 0;
    std::tm g;
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    std::tm o = g;
    o.tm_hour = r.hour; o.tm_min = r.minute; o.tm_sec = 0;
    if (r.day > 0) o.tm_mday = r.day;
#ifdef _WIN32
    std::time_t obs = _mkgmtime(&o);
#else
    std::time_t obs = timegm(&o);
#endif
    if (r.day > 0) {
        if (obs > now + 3600) { // day number belongs to last month
            o = g; o.tm_mon -= 1; o.tm_mday = r.day; o.tm_hour = r.hour; o.tm_min = r.minute; o.tm_sec = 0;
#ifdef _WIN32
            obs = _mkgmtime(&o);
#else
            obs = timegm(&o);
#endif
        }
    } else if (obs > now + 300) {
        obs -= 24 * 3600;
    }
    double age = difftime(now, obs);
    return age < 0 ? 0 : age;
}

} // namespace wx
