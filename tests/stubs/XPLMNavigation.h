#pragma once
#include "XPLMDefs.h"
typedef int XPLMNavRef;
typedef int XPLMNavType;
#define XPLM_NAV_NOT_FOUND -1
enum { xplm_Nav_Airport = 1 };
XPLMNavRef XPLMFindNavAid(const char*, const char*, float*, float*, int*, XPLMNavType);
void XPLMGetNavAidInfo(XPLMNavRef, XPLMNavType*, float*, float*, float*, int*, float*, char*, char*, char*);
