// Builds the text shown by the "Show Local METAR/ATIS Data" window. Pure logic (no X-Plane calls) so it can be tested.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "config.h"
#include "fetcher.h"

namespace wx {

enum class LineKind { Title, Header, Metar, Atis, Applied, Note, Blank };

struct ViewLine {
    LineKind kind = LineKind::Note;
    std::string text;
};

// One block per station (nearest first): raw METAR, ATIS, and what is applied to the sim; then winds-aloft stations.
std::vector<ViewLine> buildViewLines(const FetchResult& res, bool haveData, const Config& cfg, std::time_t now);

// Greedy word wrap on a character budget (the window uses real font metrics instead; this is the fallback/test form).
std::vector<std::string> wrapText(const std::string& s, size_t maxChars);

} // namespace wx
