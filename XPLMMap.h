// Test-only stub of the SDK header (names and signatures as in X-Plane SDK 3.0+).
#pragma once
#include "XPLMDefs.h"
typedef void* XPLMMapLayerID;
typedef void* XPLMMapProjectionID;
typedef int XPLMMapStyle;
typedef int XPLMMapLayerType;
enum { xplm_MapLayer_Fill = 0, xplm_MapLayer_Markings = 1 };
typedef int XPLMMapOrientation;
enum { xplm_MapOrientation_Map = 0, xplm_MapOrientation_UI = 1 };
#define XPLM_MAP_USER_INTERFACE "XPLM_MAP_USER_INTERFACE"
typedef void (*XPLMMapCreatedCallback_f)(const char*, void*);
typedef void (*XPLMMapWillBeDeletedCallback_f)(XPLMMapLayerID, void*);
typedef void (*XPLMMapPrepareCacheCallback_f)(XPLMMapLayerID, const float*, XPLMMapProjectionID, void*);
typedef void (*XPLMMapDrawingCallback_f)(XPLMMapLayerID, const float*, float, float, XPLMMapStyle, XPLMMapProjectionID, void*);
typedef void (*XPLMMapIconDrawingCallback_f)(XPLMMapLayerID, const float*, float, float, XPLMMapStyle, XPLMMapProjectionID, void*);
typedef void (*XPLMMapLabelDrawingCallback_f)(XPLMMapLayerID, const float*, float, float, XPLMMapStyle, XPLMMapProjectionID, void*);
typedef struct {
    int structSize;
    const char* mapToCreateLayerIn;
    XPLMMapLayerType layerType;
    XPLMMapWillBeDeletedCallback_f willBeDeletedCallback;
    XPLMMapPrepareCacheCallback_f prepCacheCallback;
    XPLMMapDrawingCallback_f drawCallback;
    XPLMMapIconDrawingCallback_f iconCallback;
    XPLMMapLabelDrawingCallback_f labelCallback;
    int showUiToggle;
    const char* layerName;
    void* refcon;
} XPLMCreateMapLayer_t;
XPLMMapLayerID XPLMCreateMapLayer(XPLMCreateMapLayer_t*);
int XPLMDestroyMapLayer(XPLMMapLayerID);
void XPLMRegisterMapCreationHook(XPLMMapCreatedCallback_f, void*);
int XPLMMapExists(const char*);
void XPLMDrawMapLabel(XPLMMapLayerID, const char*, float, float, XPLMMapOrientation, float);
void XPLMMapProject(XPLMMapProjectionID, double, double, float*, float*);
