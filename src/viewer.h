// Builds the text shown by the "Show Local METAR/ATIS Data" window. Pure logic (no X-Plane calls) so it can be tested.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "config.h"
#include "fetcher.h"

namespace wx {

enum class LineKind { Title, Header, Metar, Atis, Applied, Note, Blank, SvcOk, SvcWarn, SvcDown, SvcIdle };

struct ViewLine {
    LineKind kind = LineKind::Note;
    std::string text;
};

// One block per station (nearest first): raw METAR, ATIS, and what is applied to the sim; then winds-aloft stations; then the state of
// every data service (the fetch result's, then `extra`: the turbulence worker's).
std::vector<ViewLine> buildViewLines(const FetchResult& res, bool haveData, const Config& cfg, std::time_t now,
                                     const std::vector<ServiceStatus>* extra = nullptr);

// The radar region at a position: "US" (NOAA MRMS), "Europe" (EUMETNET OPERA), or "" where no radar composite covers.
std::string radarRegion(double lat, double lon);
// "NOAA MRMS" / "EUMETNET OPERA" for a region, "" for none.
std::string radarProvider(const std::string& region);

// Greedy word wrap on a character budget (the window uses real font metrics instead; this is the fallback/test form).
std::vector<std::string> wrapText(const std::string& s, size_t maxChars);

} // namespace wx
