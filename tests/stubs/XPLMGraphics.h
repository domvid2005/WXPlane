// Test-only stub of the SDK header.
#pragma once
#include "XPLMDefs.h"
enum { xplmFont_Proportional = 18 };
typedef int XPLMFontID;
void XPLMDrawString(float* rgb, int x, int y, char* str, int* wordWrapWidth, XPLMFontID font);
float XPLMMeasureString(XPLMFontID font, const char* str, int n);
void XPLMGetFontDimensions(XPLMFontID font, int* w, int* h, int* digitsOnly);
void XPLMDrawTranslucentDarkBox(int l, int t, int r, int b);
void XPLMSetGraphicsState(int fog, int texUnits, int lighting, int alphaTest, int alphaBlend, int depthTest, int depthWrite);
