#pragma once
#include "XPLMDefs.h"
XPLMDataRef XPLMFindDataRef(const char*);
double XPLMGetDatad(XPLMDataRef);
int XPLMGetDatavf(XPLMDataRef, float*, int, int);
float XPLMGetDataf(XPLMDataRef);
int XPLMGetDatai(XPLMDataRef);
typedef int XPLMDataTypeID;
enum { xplmType_Unknown = 0, xplmType_Int = 1, xplmType_Float = 2, xplmType_Double = 4 };
XPLMDataTypeID XPLMGetDataRefTypes(XPLMDataRef);
int XPLMCanWriteDataRef(XPLMDataRef);
void XPLMSetDatai(XPLMDataRef, int);
