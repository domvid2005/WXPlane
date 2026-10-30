// Minimal JSON reader (enough for the NOAA AWC and D-ATIS responses).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wx {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Json> arr;        // array items, or object values
    std::vector<std::string> keys; // object keys (parallel to arr)

    const Json* get(const std::string& k) const {
        if (type != Obj) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &arr[i];
        return nullptr;
    }
    std::string asString(const std::string& d = "") const {
        if (type == Str) return str;
        if (type == Num) { char buf[64]; std::snprintf(buf, sizeof buf, "%g", num); return buf; }
        return d;
    }
    double asNum(double d = 0) const {
        if (type == Num) return num;
        if (type == Str) { char* e = nullptr; double v = std::strtod(str.c_str(), &e); if (e != str.c_str()) return v; }
        return d;
    }
};

bool parseJson(const std::string& text, Json& out);

} // namespace wx
