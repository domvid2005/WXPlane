#pragma once
#include "XPLMDefs.h"
// the plugin list (the turbulence-conflict check); the harness defines these
XPLMPluginID XPLMGetMyID();
int XPLMCountPlugins();
XPLMPluginID XPLMGetNthPlugin(int index);
void XPLMGetPluginInfo(XPLMPluginID id, char* outName, char* outFilePath, char* outSignature, char* outDescription);
int XPLMIsPluginEnabled(XPLMPluginID id);
XPLMPluginID XPLMFindPluginBySignature(const char* inSignature);
#ifndef XPLM_NO_PLUGIN_ID
#define XPLM_NO_PLUGIN_ID (-1)
#endif
// features (XPLM200 and later): XPLM_USE_NATIVE_PATHS
void XPLMEnableFeature(const char* inFeature, int inEnable);
int XPLMIsFeatureEnabled(const char* inFeature);
