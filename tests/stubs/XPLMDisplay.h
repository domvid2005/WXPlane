// Test-only stub of the SDK header (signatures as in X-Plane SDK 4.x).
#pragma once
#include "XPLMDefs.h"
typedef void* XPLMWindowID;
enum { xplm_MouseDown = 1, xplm_MouseDrag = 2, xplm_MouseUp = 3 };
typedef int XPLMMouseStatus;
enum { xplm_CursorDefault = 1 };
typedef int XPLMCursorStatus;
enum { xplm_WindowDecorationNone = 0, xplm_WindowDecorationRoundRectangle = 1, xplm_WindowDecorationSelfDecorated = 2, xplm_WindowDecorationSelfDecoratedResizable = 3 };
typedef int XPLMWindowDecoration;
enum { xplm_WindowLayerFlightOverlay = 0, xplm_WindowLayerFloatingWindows = 1, xplm_WindowLayerModal = 2, xplm_WindowLayerGrowlNotifications = 3 };
typedef int XPLMWindowLayer;
typedef void (*XPLMDrawWindow_f)(XPLMWindowID, void*);
typedef void (*XPLMHandleKey_f)(XPLMWindowID, char, int, char, void*, int);
typedef int (*XPLMHandleMouseClick_f)(XPLMWindowID, int, int, XPLMMouseStatus, void*);
typedef XPLMCursorStatus (*XPLMHandleCursor_f)(XPLMWindowID, int, int, void*);
typedef int (*XPLMHandleMouseWheel_f)(XPLMWindowID, int, int, int, int, void*);
typedef struct {
    int structSize;
    int left, top, right, bottom;
    int visible;
    XPLMDrawWindow_f drawWindowFunc;
    XPLMHandleMouseClick_f handleMouseClickFunc;
    XPLMHandleKey_f handleKeyFunc;
    XPLMHandleCursor_f handleCursorFunc;
    XPLMHandleMouseWheel_f handleMouseWheelFunc;
    void* refcon;
    XPLMWindowDecoration decorateAsFloatingWindow;
    XPLMWindowLayer layer;
    XPLMHandleMouseClick_f handleRightClickFunc;
} XPLMCreateWindow_t;
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t*);
void XPLMDestroyWindow(XPLMWindowID);
void XPLMGetScreenBoundsGlobal(int*, int*, int*, int*);
void XPLMGetWindowGeometry(XPLMWindowID, int*, int*, int*, int*);
void XPLMSetWindowIsVisible(XPLMWindowID, int);
int XPLMGetWindowIsVisible(XPLMWindowID);
void XPLMSetWindowTitle(XPLMWindowID, const char*);
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int);
void XPLMBringWindowToFront(XPLMWindowID);
