#include "json_mini.h"

#include <cstring>

namespace wx {
namespace {

struct P {
    const char* s;
    const char* e;
    int depth = 0;
};

void skipWs(P& p) {
    while (p.s < p.e && (*p.s == ' ' || *p.s == '\t' || *p.s == '\n' || *p.s == '\r')) ++p.s;
}

void putUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

bool parseString(P& p, std::string& out) {
    if (p.s >= p.e || *p.s != '"') return false;
    ++p.s;
    while (p.s < p.e && *p.s != '"') {
        char c = *p.s++;
        if (c != '\\') { out += c; continue; }
        if (p.s >= p.e) return false;
        char d = *p.s++;
        switch (d) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (p.e - p.s < 4) return false;
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = *p.s++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                putUtf8(out, cp);
                break;
            }
            default: out += d; break;
        }
    }
    if (p.s >= p.e) return false;
    ++p.s; // closing quote
    return true;
}

bool parseValue(P& p, Json& v) {
    if (++p.depth > 64) return false;
    struct Guard { P& p; ~Guard() { --p.depth; } } g{p};
    skipWs(p);
    if (p.s >= p.e) return false;
    char c = *p.s;
    if (c == '{') {
        ++p.s;
        v.type = Json::Obj;
        skipWs(p);
        if (p.s < p.e && *p.s == '}') { ++p.s; return true; }
        for (;;) {
            skipWs(p);
            std::string key;
            if (!parseString(p, key)) return false;
            skipWs(p);
            if (p.s >= p.e || *p.s != ':') return false;
            ++p.s;
            Json child;
            if (!parseValue(p, child)) return false;
            v.keys.push_back(std::move(key));
            v.arr.push_back(std::move(child));
            skipWs(p);
            if (p.s >= p.e) return false;
            if (*p.s == ',') { ++p.s; continue; }
            if (*p.s == '}') { ++p.s; return true; }
            return false;
        }
    }
    if (c == '[') {
        ++p.s;
        v.type = Json::Arr;
        skipWs(p);
        if (p.s < p.e && *p.s == ']') { ++p.s; return true; }
        for (;;) {
            Json child;
            if (!parseValue(p, child)) return false;
            v.arr.push_back(std::move(child));
            skipWs(p);
            if (p.s >= p.e) return false;
            if (*p.s == ',') { ++p.s; continue; }
            if (*p.s == ']') { ++p.s; return true; }
            return false;
        }
    }
    if (c == '"') {
        v.type = Json::Str;
        return parseString(p, v.str);
    }
    if (p.e - p.s >= 4 && !std::strncmp(p.s, "true", 4)) { v.type = Json::Bool; v.b = true; p.s += 4; return true; }
    if (p.e - p.s >= 5 && !std::strncmp(p.s, "false", 5)) { v.type = Json::Bool; v.b = false; p.s += 5; return true; }
    if (p.e - p.s >= 4 && !std::strncmp(p.s, "null", 4)) { v.type = Json::Null; p.s += 4; return true; }
    // number
    const char* start = p.s;
    while (p.s < p.e && (std::strchr("+-.eE", *p.s) || (*p.s >= '0' && *p.s <= '9'))) ++p.s;
    if (p.s == start) return false;
    v.type = Json::Num;
    v.num = std::strtod(std::string(start, p.s).c_str(), nullptr);
    return true;
}

} // namespace

bool parseJson(const std::string& text, Json& out) {
    P p{text.data(), text.data() + text.size()};
    out = Json();
    if (!parseValue(p, out)) return false;
    skipWs(p);
    return true;
}

} // namespace wx
