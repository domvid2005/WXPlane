#pragma once
#include "XPLMDefs.h"
// the plugin list (the turbulence-conflict check); the harness defines these
XPLMPluginID XPLMGetMyID();
int XPLMCountPlugins();
XPLMPluginID XPLMGetNthPlugin(int index);
void XPLMGetPluginInfo(XPLMPluginID id, char* outName, char* outFilePath, char* outSignature, char* outDescription);
int XPLMIsPluginEnabled(XPLMPluginID id);
