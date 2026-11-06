#pragma once
#include "XPLMDefs.h"
XPLMDataRef XPLMFindDataRef(const char*);
double XPLMGetDatad(XPLMDataRef);
void XPLMSetDatavf(XPLMDataRef, float*, int, int);
int XPLMGetDatavf(XPLMDataRef, float*, int, int);
float XPLMGetDataf(XPLMDataRef);
int XPLMGetDatai(XPLMDataRef);
typedef int XPLMDataTypeID;
enum { xplmType_Unknown = 0, xplmType_Int = 1, xplmType_Float = 2, xplmType_Double = 4, xplmType_FloatArray = 8, xplmType_IntArray = 16, xplmType_Data = 32 };
XPLMDataTypeID XPLMGetDataRefTypes(XPLMDataRef);
int XPLMCanWriteDataRef(XPLMDataRef);
void XPLMSetDatai(XPLMDataRef, int);
int XPLMGetDatavi(XPLMDataRef, int*, int, int);
typedef struct { int structSize; const char* name; XPLMDataTypeID type; int writable; int owner; } XPLMDataRefInfo_t;
int XPLMCountDataRefs(void);
void XPLMGetDataRefsByIndex(int offset, int count, XPLMDataRef* outDataRefs);
void XPLMGetDataRefInfo(XPLMDataRef inDataRef, XPLMDataRefInfo_t* outInfo);
