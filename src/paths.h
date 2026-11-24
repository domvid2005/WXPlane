#pragma once
#include <string>

namespace wx {

// An old-style Mac (HFS) path, as X-Plane gives it on a Mac before XPLM_USE_NATIVE_PATHS is on: "Macintosh HD:X-Plane 12:Output:...".
// A Windows path ("D:\X-Plane 12\Output\...", or with '/' as X-Plane sometimes mixes them) has a ':' too but is not one, and neither is
// a POSIX path. (5.1.6 and 5.1.7 took a Windows path for an HFS one; see moveHfsLeftovers.)
inline bool isHfsPath(const std::string& p) {
    return !p.empty() && p.find(':') != std::string::npos && p.find('/') == std::string::npos && p.find('\\') == std::string::npos;
}

}  // namespace wx
