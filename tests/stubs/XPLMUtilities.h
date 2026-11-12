#pragma once
#include "XPLMDefs.h"
void XPLMDebugString(const char*);
void XPLMGetPrefsPath(char*);
const char* XPLMGetDirectorySeparator(void);
char* XPLMExtractFileAndPath(char*);
typedef void* XPLMCommandRef;
XPLMCommandRef XPLMFindCommand(const char*);
void XPLMCommandOnce(XPLMCommandRef);
enum { xplm_CommandBegin = 0, xplm_CommandContinue = 1, xplm_CommandEnd = 2 };
typedef int XPLMCommandPhase;
typedef int (*XPLMCommandCallback_f)(XPLMCommandRef, XPLMCommandPhase, void*);
XPLMCommandRef XPLMCreateCommand(const char* name, const char* description);
void XPLMRegisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f, int before, void* refcon);
void XPLMUnregisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f, int before, void* refcon);
