// TEST STUB - mirrors the subset of the real X-Plane SDK used by plugin.cpp. NOT shipped.
#pragma once
#define PLUGIN_API extern "C" __attribute__((visibility("default")))
typedef int XPLMPluginID;
typedef void* XPLMDataRef;
typedef int XPLMKeyFlags;
enum { xplm_ShiftFlag = 1, xplm_OptionAltFlag = 2, xplm_ControlFlag = 4, xplm_DownFlag = 8, xplm_UpFlag = 16 };
#define XPLM_VK_BACK 0x08
#define XPLM_VK_TAB 0x09
#define XPLM_VK_RETURN 0x0D
#define XPLM_VK_ESCAPE 0x1B
#define XPLM_VK_LEFT 0x25
#define XPLM_VK_RIGHT 0x27
#define XPLM_VK_DELETE 0x2E
#define XPLM_VK_HOME 0x24
#define XPLM_VK_END 0x23
#define XPLM_VK_ENTER 0xBB
