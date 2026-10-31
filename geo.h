#pragma once
#include <algorithm>
#include <cmath>

namespace wx {

// Great-circle distance in nautical miles.
inline double distNm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 3440.065;
    const double d2r = 3.14159265358979323846 / 180.0;
    double dLat = (lat2 - lat1) * d2r, dLon = (lon2 - lon1) * d2r;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * d2r) * std::cos(lat2 * d2r) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
}

} // namespace wx
