#include "http.h"

#include <curl/curl.h>

namespace wx {

namespace {
size_t sink(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    if (s->size() + size * nmemb > 8u * 1024u * 1024u) return 0; // refuse absurdly large replies
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

void httpGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void httpGlobalCleanup() { curl_global_cleanup(); }

bool httpGet(const std::string& url, std::string& body, long& status, std::string& error, long timeoutSec) {
    body.clear();
    status = 0;
    CURL* c = curl_easy_init();
    if (!c) { error = "curl init failed"; return false; }
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "XPlane12-WXPlane/1.0 (+x-plane plugin)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // required for multithreaded use
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        curl_easy_cleanup(c);
        return false;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

} // namespace wx
