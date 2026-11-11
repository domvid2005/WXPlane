#include "http.h"

#include <curl/curl.h>

#include <sys/stat.h>

namespace wx {

namespace {
struct Sink { std::string* s; size_t max; };
size_t sink(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* k = static_cast<Sink*>(ud);
    if (k->s->size() + size * nmemb > k->max) return 0; // refuse absurdly large replies
    k->s->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

// The Linux plugin carries its own libcurl and OpenSSL (2026-11-11: under Steam's runtime the system libcurl.so.4 is replaced by
// one without the CURL_OPENSSL_4 symbol versions, and the plugin did not load). That OpenSSL looks for certificates where the build
// machine (Ubuntu) keeps them, so the distribution's bundle is found here instead. Windows and macOS use the system's own store.
static std::string g_caFile, g_caPath;
static bool fileExists(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static bool dirExists(const char* p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
void httpGlobalInit() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
#if defined(__linux__)
    static const char* kBundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                  // Debian, Ubuntu, Arch, Gentoo, Steam runtime
        "/etc/pki/tls/certs/ca-bundle.crt",                    // Fedora, RHEL
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",   // RHEL, CentOS
        "/etc/ssl/ca-bundle.pem",                              // openSUSE
        "/var/lib/ca-certificates/ca-bundle.pem",              // openSUSE
        "/etc/ssl/cert.pem",                                   // Alpine, Void, Arch
        "/etc/pki/tls/cacert.pem",
    };
    for (const char* b : kBundles) if (fileExists(b)) { g_caFile = b; break; }
    if (dirExists("/etc/ssl/certs")) g_caPath = "/etc/ssl/certs";
#endif
}
std::string httpTlsInfo() {
    const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    std::string s = std::string("libcurl ") + (v && v->version ? v->version : "?") + ", " + (v && v->ssl_version ? v->ssl_version : "no TLS");
#if defined(__linux__)
    s += g_caFile.empty() ? ", no certificate bundle found" : ", certificates " + g_caFile;
#endif
    return s;
}
void httpGlobalCleanup() { curl_global_cleanup(); }

static bool get(const std::string& url, const char* range, std::string& body, long& status, std::string& error, long timeoutSec, size_t maxBytes,
                const std::atomic<bool>* cancel = nullptr);
static int progress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* c = static_cast<const std::atomic<bool>*>(ud);
    return (c && c->load()) ? 1 : 0;   // non-zero aborts the transfer
}
bool httpGetCancellable(const std::string& url, const std::string& range, std::string& body, long& status, std::string& error, long timeoutSec,
                        size_t maxBytes, const std::atomic<bool>* cancel) {
    return get(url, range.empty() ? nullptr : range.c_str(), body, status, error, timeoutSec, maxBytes, cancel);
}
bool httpGet(const std::string& url, std::string& body, long& status, std::string& error, long timeoutSec, size_t maxBytes) {
    return get(url, nullptr, body, status, error, timeoutSec, maxBytes);
}
bool httpGetRange(const std::string& url, const std::string& range, std::string& body, long& status, std::string& error, long timeoutSec, size_t maxBytes) {
    return get(url, range.c_str(), body, status, error, timeoutSec, maxBytes);
}
static bool get(const std::string& url, const char* range, std::string& body, long& status, std::string& error, long timeoutSec, size_t maxBytes,
                const std::atomic<bool>* cancel) {
    body.clear();
    status = 0;
    Sink k{&body, maxBytes};
    CURL* c = curl_easy_init();
    if (!c) { error = "curl init failed"; return false; }
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &k);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "XPlane12-WXPlane/1.0 (+x-plane plugin)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // required for multithreaded use
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (!g_caFile.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, g_caFile.c_str());
    if (!g_caPath.empty()) curl_easy_setopt(c, CURLOPT_CAPATH, g_caPath.c_str());
    if (range) curl_easy_setopt(c, CURLOPT_RANGE, range);
    if (cancel) {
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*)cancel);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
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
