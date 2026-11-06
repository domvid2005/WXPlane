#include "wxgl.h"

#if defined(_WIN32)
#include <windows.h>
#define WXGL_API __stdcall
#else
#include <dlfcn.h>
#define WXGL_API
#endif

namespace wxgl {
namespace {
typedef void(WXGL_API* PBegin)(unsigned int);
typedef void(WXGL_API* PEnd)(void);
typedef void(WXGL_API* PVertex2f)(float, float);
typedef void(WXGL_API* PColor4f)(float, float, float, float);
typedef void(WXGL_API* PLineWidth)(float);
PBegin pBegin = nullptr; PEnd pEnd = nullptr; PVertex2f pVertex = nullptr; PColor4f pColor = nullptr; PLineWidth pLineWidth = nullptr;
int state = 0;   // 0 not tried, 1 ok, -1 failed

void* sym(const char* n) {
#if defined(_WIN32)
    HMODULE m = GetModuleHandleA("opengl32.dll");
    if (!m) m = LoadLibraryA("opengl32.dll");
    return m ? (void*)GetProcAddress(m, n) : nullptr;
#elif defined(__APPLE__)
    static void* h = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    void* p = dlsym(RTLD_DEFAULT, n);
    return p ? p : (h ? dlsym(h, n) : nullptr);
#else
    void* p = dlsym(RTLD_DEFAULT, n);
    if (p) return p;
    static void* h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    return h ? dlsym(h, n) : nullptr;
#endif
}
} // namespace

bool load() {
    if (state == 0) {
        pBegin = (PBegin)sym("glBegin"); pEnd = (PEnd)sym("glEnd"); pVertex = (PVertex2f)sym("glVertex2f");
        pColor = (PColor4f)sym("glColor4f"); pLineWidth = (PLineWidth)sym("glLineWidth");
        state = (pBegin && pEnd && pVertex && pColor && pLineWidth) ? 1 : -1;
    }
    return state == 1;
}
void begin(unsigned int m) { if (pBegin) pBegin(m); }
void end() { if (pEnd) pEnd(); }
void vertex(float x, float y) { if (pVertex) pVertex(x, y); }
void color(float r, float g, float b, float a) { if (pColor) pColor(r, g, b, a); }
void lineWidth(float w) { if (pLineWidth) pLineWidth(w); }
} // namespace wxgl
