#pragma once
#include "XPLMDefs.h"
void XPLMDebugString(const char*);
void XPLMGetPrefsPath(char*);
const char* XPLMGetDirectorySeparator(void);
char* XPLMExtractFileAndPath(char*);
typedef void* XPLMCommandRef;
XPLMCommandRef XPLMFindCommand(const char*);
void XPLMCommandOnce(XPLMCommandRef);
