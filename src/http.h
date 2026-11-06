#pragma once
#include <cstddef>
#include <string>

namespace wx {

void httpGlobalInit();
void httpGlobalCleanup();

// Blocking GET (call from the worker thread only). Returns true on HTTP 2xx; `status` receives the code.
bool httpGet(const std::string& url, std::string& body, long& status, std::string& error, long timeoutSec = 12, size_t maxBytes = 8u * 1024u * 1024u);
// The same with an HTTP byte range ("first-last", e.g. "0-65535"): only that part of the file is downloaded.
bool httpGetRange(const std::string& url, const std::string& range, std::string& body, long& status, std::string& error, long timeoutSec = 20,
                  size_t maxBytes = 16u * 1024u * 1024u);

} // namespace wx
