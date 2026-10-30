#pragma once
#include "XPLMDefs.h"
XPLMDataRef XPLMFindDataRef(const char*);
double XPLMGetDatad(XPLMDataRef);
int XPLMGetDatavf(XPLMDataRef, float*, int, int);
float XPLMGetDataf(XPLMDataRef);
int XPLMGetDatai(XPLMDataRef);
