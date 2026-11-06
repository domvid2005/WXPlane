// The few OpenGL 1.x calls used to draw on X-Plane's map. They are looked up at run time from the OpenGL library
// X-Plane has already loaded, so building needs no GL headers or libraries on any platform.
#pragma once

namespace wxgl {
constexpr unsigned int LINE_LOOP = 0x0002, LINE_STRIP = 0x0003, TRIANGLE_FAN = 0x0006;
bool load();   // true when all functions were found
void begin(unsigned int mode);
void end();
void vertex(float x, float y);
void color(float r, float g, float b, float a);
void lineWidth(float w);
} // namespace wxgl
