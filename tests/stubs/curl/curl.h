// TEST STUB for libcurl (syntax check only). NOT shipped.
#pragma once
#include <cstddef>
typedef void CURL;
typedef int CURLcode;
enum { CURLE_OK = 0 };
#define CURL_ERROR_SIZE 256
#define CURL_GLOBAL_DEFAULT 3L
enum { CURLOPT_RANGE = 999, CURLOPT_URL = 1, CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, CURLOPT_ERRORBUFFER, CURLOPT_USERAGENT, CURLOPT_FOLLOWLOCATION,
       CURLOPT_MAXREDIRS, CURLOPT_TIMEOUT, CURLOPT_CONNECTTIMEOUT, CURLOPT_NOSIGNAL, CURLOPT_ACCEPT_ENCODING, CURLINFO_RESPONSE_CODE };
extern "C" {
CURLcode curl_global_init(long);
void curl_global_cleanup(void);
CURL* curl_easy_init(void);
CURLcode curl_easy_setopt(CURL*, int, ...);
CURLcode curl_easy_perform(CURL*);
CURLcode curl_easy_getinfo(CURL*, int, ...);
void curl_easy_cleanup(CURL*);
const char* curl_easy_strerror(CURLcode);
}
