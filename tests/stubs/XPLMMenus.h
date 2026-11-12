#pragma once
#include "XPLMDefs.h"
typedef void* XPLMMenuID;
typedef void (*XPLMMenuHandler_f)(void*, void*);
enum { xplm_Menu_NoCheck = 0, xplm_Menu_Unchecked = 1, xplm_Menu_Checked = 2 };
typedef int XPLMMenuCheck;
XPLMMenuID XPLMFindPluginsMenu(void);
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f, void*);
void XPLMDestroyMenu(XPLMMenuID);
int XPLMAppendMenuItem(XPLMMenuID, const char*, void*, int);
void XPLMAppendMenuSeparator(XPLMMenuID);
void XPLMSetMenuItemName(XPLMMenuID, int, const char*, int);
void XPLMCheckMenuItem(XPLMMenuID, int, XPLMMenuCheck);
void XPLMEnableMenuItem(XPLMMenuID, int, int);
#include "XPLMUtilities.h"
int XPLMAppendMenuItemWithCommand(XPLMMenuID, const char*, XPLMCommandRef);
void XPLMRemoveMenuItem(XPLMMenuID, int);
