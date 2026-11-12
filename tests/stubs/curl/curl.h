// TEST STUB for libcurl (syntax check only). NOT shipped.
#pragma once
#include <cstddef>
typedef void CURL;
typedef int CURLcode;
typedef long long curl_off_t;
enum { CURLE_OK = 0, CURLE_COULDNT_CONNECT = 7 };
#define CURL_ERROR_SIZE 256
#define CURL_GLOBAL_DEFAULT 3L
enum { CURLOPT_RANGE = 999, CURLOPT_URL = 1, CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, CURLOPT_ERRORBUFFER, CURLOPT_USERAGENT, CURLOPT_FOLLOWLOCATION,
       CURLOPT_MAXREDIRS, CURLOPT_TIMEOUT, CURLOPT_CONNECTTIMEOUT, CURLOPT_NOSIGNAL, CURLOPT_ACCEPT_ENCODING, CURLINFO_RESPONSE_CODE,
       CURLOPT_XFERINFOFUNCTION, CURLOPT_XFERINFODATA, CURLOPT_NOPROGRESS, CURLOPT_CAINFO, CURLOPT_CAPATH };
enum CURLversion { CURLVERSION_NOW = 11 };
struct curl_version_info_data { int age; const char* version; unsigned int version_num; const char* host; int features; const char* ssl_version; };
extern "C" {
CURLcode curl_global_init(long);
void curl_global_cleanup(void);
CURL* curl_easy_init(void);
CURLcode curl_easy_setopt(CURL*, int, ...);
CURLcode curl_easy_perform(CURL*);
CURLcode curl_easy_getinfo(CURL*, int, ...);
void curl_easy_cleanup(CURL*);
const char* curl_easy_strerror(CURLcode);
const curl_version_info_data* curl_version_info(CURLversion);
}
