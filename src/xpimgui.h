// The WXPlane weather window inside X-Plane: an XPLM window that hosts a Dear ImGui context, feeds it X-Plane's mouse events and
// draws its output with the few OpenGL 1.x calls X-Plane's plugin drawing allows (looked up at run time, like wxgl.cpp). Can pop out
// into its own OS window, and opens in VR when VR is on.
#pragma once

#include "wxui.h"

namespace wx {
namespace xpui {

// Called once per frame while the window is visible, inside an ImGui frame: draw the UI into width x height (window units).
using BuildFn = void (*)(const UiFonts& fonts, float width, float height, bool poppedOut);

void open(BuildFn build);   // creates the window on first use, then shows it and brings it to the front
bool isOpen();
void hide();
void setNightDim(bool on);   // the in-sim window dims with the daylight (on by default)
void togglePopOut();        // its own OS window, or back into the sim (asked for here, done by tick())
void tick();                // from the flight loop: makes a pending pop-out / pop-in (never inside the window's own drawing)
void destroy();             // plugin stop: the window, the ImGui context and the font texture

} // namespace xpui
} // namespace wx
