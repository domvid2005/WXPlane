// WXPlane on another plugin's screen. A plugin that wants the weather window's contents on a surface of its own (XPTablet shows
// them on the cockpit tablet) sends WXPlane this message from inside its own drawing callback, once per surface per frame:
//     XPLMSendMessageToPlugin(XPLMFindPluginBySignature(WXPLANE_SIGNATURE), WXPLANE_MSG_EMBED, &request);
// Before sending, the caller sets OpenGL's modelview matrix so that the origin is the top-left corner of the area, x runs right,
// y runs DOWN, and one unit is one pixel of the surface being drawn. WXPlane draws its screen into widthPx x heightPx there (no
// scissor test is used: everything is clipped to the area before it reaches OpenGL) and sets `handled`. A WXPlane too old to know
// the message leaves `handled` at 0. The same struct is in XPTablet's source: change both together and raise `version`.
#pragma once

#define WXPLANE_SIGNATURE "com.wxplane.xp12.noaa"
#define WXPLANE_MSG_EMBED 0x01575850   /* plugin-defined messages must be above 0x00FFFFFF */

struct WxEmbedRequest {
    int structSize;        // sizeof(WxEmbedRequest)
    int version;           // 1
    float widthPx, heightPx;   // the area, in the units of the caller's matrix
    float pixelsPerUnit;   // real screen pixels per unit (1 on a panel texture, the UI scale in a window): for crisp text
    float mouseX, mouseY;  // the pointer in the area (same units, from its top-left corner, y down); far outside when not over it
    int mouseDown;         // the pointer is pressed
    float wheel;           // mouse wheel clicks since the last frame (up positive)
    float brightness;      // 0..1: the screen's brightness (colours are scaled by it)
    int frame;             // X-Plane's cycle number: a second request in the same frame repeats the picture on another surface
    int handled;           // out: WXPlane sets 1
};
