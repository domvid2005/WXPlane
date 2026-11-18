#include "xpimgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMUtilities.h"
#include "imgui.h"
#include "version.h"

#if defined(_WIN32)
#include <windows.h>
#define WXGL_API __stdcall
#else
#include <dlfcn.h>
#define WXGL_API
#endif

namespace wx {
namespace xpui {
namespace {

// ---- OpenGL 1.x, looked up at run time (no GL headers or libraries needed to build) ------------------------------------------------
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
const GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
             GL_LINEAR = 0x2601, GL_SCISSOR_TEST = 0x0C11, GL_VERTEX_ARRAY = 0x8074, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078,
             GL_FLOAT = 0x1406, GL_TRIANGLES = 0x0004, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_MODELVIEW = 0x1700,
             GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2,
             GL_VIEWPORT = 0x0BA2, GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_CULL_FACE = 0x0B44, GL_SCISSOR_BOX = 0x0C10,
             GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F;
const unsigned int GL_CLIENT_VERTEX_ARRAY_BIT = 0x00000002;

struct GL {
    void(WXGL_API* Enable)(GLenum) = nullptr;
    void(WXGL_API* Disable)(GLenum) = nullptr;
    GLboolean(WXGL_API* IsEnabled)(GLenum) = nullptr;
    void(WXGL_API* Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void(WXGL_API* GetIntegerv)(GLenum, GLint*) = nullptr;
    void(WXGL_API* GetFloatv)(GLenum, float*) = nullptr;
    void(WXGL_API* BlendFunc)(GLenum, GLenum) = nullptr;
    void(WXGL_API* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void(WXGL_API* TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void(WXGL_API* PixelStorei)(GLenum, GLint) = nullptr;
    void(WXGL_API* DeleteTextures)(GLsizei, const unsigned int*) = nullptr;
    void(WXGL_API* EnableClientState)(GLenum) = nullptr;
    void(WXGL_API* DisableClientState)(GLenum) = nullptr;
    void(WXGL_API* VertexPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(WXGL_API* TexCoordPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(WXGL_API* ColorPointer)(GLint, GLenum, GLsizei, const void*) = nullptr;
    void(WXGL_API* DrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
    void(WXGL_API* DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void(WXGL_API* MatrixMode)(GLenum) = nullptr;
    void(WXGL_API* PushMatrix)(void) = nullptr;
    void(WXGL_API* PopMatrix)(void) = nullptr;
    void(WXGL_API* Translatef)(float, float, float) = nullptr;
    void(WXGL_API* Scalef)(float, float, float) = nullptr;
    void(WXGL_API* PushClientAttrib)(unsigned int) = nullptr;
    void(WXGL_API* PopClientAttrib)(void) = nullptr;
    void(WXGL_API* Color4f)(float, float, float, float) = nullptr;
    int state = 0;
} gl;

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

template <class F> bool get(F& f, const char* n) { f = (F)sym(n); return f != nullptr; }

bool loadGl() {
    if (gl.state == 0) {
        bool ok = true;
        ok &= get(gl.Enable, "glEnable"); ok &= get(gl.Disable, "glDisable"); ok &= get(gl.IsEnabled, "glIsEnabled");
        ok &= get(gl.Scissor, "glScissor"); ok &= get(gl.GetIntegerv, "glGetIntegerv"); ok &= get(gl.GetFloatv, "glGetFloatv");
        ok &= get(gl.BlendFunc, "glBlendFunc"); ok &= get(gl.TexImage2D, "glTexImage2D"); ok &= get(gl.TexParameteri, "glTexParameteri");
        ok &= get(gl.PixelStorei, "glPixelStorei"); ok &= get(gl.DeleteTextures, "glDeleteTextures");
        ok &= get(gl.EnableClientState, "glEnableClientState"); ok &= get(gl.DisableClientState, "glDisableClientState");
        ok &= get(gl.VertexPointer, "glVertexPointer"); ok &= get(gl.TexCoordPointer, "glTexCoordPointer"); ok &= get(gl.ColorPointer, "glColorPointer");
        ok &= get(gl.DrawElements, "glDrawElements"); ok &= get(gl.DrawArrays, "glDrawArrays"); ok &= get(gl.MatrixMode, "glMatrixMode"); ok &= get(gl.PushMatrix, "glPushMatrix");
        ok &= get(gl.PopMatrix, "glPopMatrix"); ok &= get(gl.Translatef, "glTranslatef"); ok &= get(gl.Scalef, "glScalef");
        ok &= get(gl.PushClientAttrib, "glPushClientAttrib"); ok &= get(gl.PopClientAttrib, "glPopClientAttrib"); ok &= get(gl.Color4f, "glColor4f");
        gl.state = ok ? 1 : -1;
        if (!ok) XPLMDebugString("[WXPlane] weather window: OpenGL functions not found, the window cannot be drawn\n");
    }
    return gl.state == 1;
}

// ---- state ---------------------------------------------------------------------------------------------------------------------
XPLMWindowID g_win = nullptr;
ImGuiContext* g_ctx = nullptr;
ImGuiContext* g_ctxEmbed = nullptr;   // the contents on another plugin's screen: its own context (own size and pointer), the same fonts
int g_embedFrame = -1;                // the sim frame the embedded picture was last built in
bool g_embedDown = false;
std::chrono::steady_clock::time_point g_embedAt{};   // when it was last asked for
BuildFn g_build = nullptr;
UiFonts g_fonts;
int g_fontTex = 0, g_logoTex = 0;
float g_logoAspect = 1;
float g_fontScale = 0;          // pixels per window unit the atlas was built for
bool g_fontsDirty = true;
std::chrono::steady_clock::time_point g_last{};
bool g_hasKeyFocus = false;   // the window holds X-Plane's keyboard focus (a text field is active)
int g_cursorFrames = 0;         // frames since the cursor callback last reported the mouse over the window
float g_mouseX = -1e9f, g_mouseY = -1e9f;
XPLMDataRef g_drMv = nullptr, g_drProj = nullptr, g_drVp = nullptr, g_drVr = nullptr, g_drSun = nullptr;

// window units (boxels) -> native pixels, for glScissor (X-Plane scales its UI, and VR/pop-out windows have their own transforms)
struct Xform { float mv[16], pr[16]; int vp[4]; bool ok = false; };

Xform readXform() {
    Xform x;
    if (!g_drMv) { g_drMv = XPLMFindDataRef("sim/graphics/view/modelview_matrix"); g_drProj = XPLMFindDataRef("sim/graphics/view/projection_matrix"); g_drVp = XPLMFindDataRef("sim/graphics/view/viewport"); }
    if (g_drMv && g_drProj && g_drVp && XPLMGetDatavf(g_drMv, x.mv, 0, 16) == 16 && XPLMGetDatavf(g_drProj, x.pr, 0, 16) == 16 && XPLMGetDatavi(g_drVp, x.vp, 0, 4) == 4) {
        x.ok = true;   // viewport: left, bottom, right, top
    } else if (gl.GetFloatv && gl.GetIntegerv) {
        gl.GetFloatv(GL_MODELVIEW_MATRIX, x.mv);
        gl.GetFloatv(GL_PROJECTION_MATRIX, x.pr);
        GLint v[4]; gl.GetIntegerv(GL_VIEWPORT, v);
        x.vp[0] = v[0]; x.vp[1] = v[1]; x.vp[2] = v[0] + v[2]; x.vp[3] = v[1] + v[3];
        x.ok = true;
    }
    return x;
}

void toNative(const Xform& x, float bx, float by, float& nx, float& ny) {
    const float e[4] = {x.mv[0] * bx + x.mv[4] * by + x.mv[12], x.mv[1] * bx + x.mv[5] * by + x.mv[13], x.mv[2] * bx + x.mv[6] * by + x.mv[14], x.mv[3] * bx + x.mv[7] * by + x.mv[15]};
    float c[4];
    for (int i = 0; i < 4; ++i) c[i] = x.pr[i] * e[0] + x.pr[4 + i] * e[1] + x.pr[8 + i] * e[2] + x.pr[12 + i] * e[3];
    const float w = std::fabs(c[3]) > 1e-9f ? c[3] : 1.0f;
    nx = x.vp[0] + (c[0] / w + 1.0f) * 0.5f * (float)(x.vp[2] - x.vp[0]);
    ny = x.vp[1] + (c[1] / w + 1.0f) * 0.5f * (float)(x.vp[3] - x.vp[1]);
}

void buildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    g_fonts = uiLoadFonts(scale);
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!g_fontTex) XPLMGenerateTextureNumbers(&g_fontTex, 1);
    XPLMBindTexture2d(g_fontTex, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_fontTex);
    io.Fonts->ClearTexData();   // the pixels are on the GPU now
    // the logo, once
    if (!g_logoTex) {
        std::vector<unsigned char> rgba;
        int lw = 0, lh = 0;
        if (uiLogoRgba(rgba, lw, lh)) {
            XPLMGenerateTextureNumbers(&g_logoTex, 1);
            XPLMBindTexture2d(g_logoTex, 0);
            const std::vector<UiImage> mips = uiLogoMips(rgba, lw, lh);
            // trilinear (GL_LINEAR_MIPMAP_LINEAR) over the smaller copies: clean edges at any size it is drawn at
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)(mips.empty() ? GL_LINEAR : 0x2703));
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, (GLint)mips.size());
            gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            for (size_t m = 0; m < mips.size(); ++m)
                gl.TexImage2D(GL_TEXTURE_2D, (GLint)(m + 1), (GLint)GL_RGBA, mips[m].w, mips[m].h, 0, GL_RGBA, GL_UNSIGNED_BYTE, mips[m].rgba.data());
            g_logoAspect = (float)lw / (float)lh;
        }
    }
    g_fonts.logo = g_logoTex ? (void*)(intptr_t)g_logoTex : nullptr;
    g_fonts.logoAspect = g_logoAspect;
    g_fontScale = scale;
    g_fontsDirty = false;
}

// Night dimming of the in-sim window. At night X-Plane's auto exposure counts the window (it is in the main view), and the window,
// dark as it is, is far brighter than a night cockpit: the whole scene went dark while it was open, and came back when it closed
// or was popped out (2026-11-13; not in daylight). So in the dark the window's colours are scaled down (alpha kept): from full at
// sun 0° to 45% below -8° (civil twilight on). A popped-out window is left as it is (not in the main view).
bool g_nightDimOn = true;
float nightDim(bool poppedOut) {
    if (poppedOut || !g_nightDimOn) return 1.0f;
    if (!g_drSun) g_drSun = XPLMFindDataRef("sim/graphics/scenery/sun_pitch_degrees");
    if (!g_drSun) return 1.0f;
    const float sun = XPLMGetDataf(g_drSun);
    const float t = std::max(0.0f, std::min(1.0f, -sun / 8.0f));
    return 1.0f - 0.55f * t;
}
void dimVertices(ImDrawData* dd, float k) {
    if (k >= 0.999f) return;
    const unsigned m = (unsigned)(k * 256.0f + 0.5f);
    for (int n = 0; n < dd->CmdListsCount; ++n)
        for (ImDrawVert& v : dd->CmdLists[n]->VtxBuffer) {
            const ImU32 c = v.col;
            const unsigned r = ((c >> IM_COL32_R_SHIFT) & 0xFF) * m >> 8, g = ((c >> IM_COL32_G_SHIFT) & 0xFF) * m >> 8, b = ((c >> IM_COL32_B_SHIFT) & 0xFF) * m >> 8;
            v.col = (c & IM_COL32_A_MASK) | (r << IM_COL32_R_SHIFT) | (g << IM_COL32_G_SHIFT) | (b << IM_COL32_B_SHIFT);
        }
}

void renderDrawData(ImDrawData* dd, int left, int top) {
    const Xform xf = readXform();
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const bool cull = gl.IsEnabled(GL_CULL_FACE) != 0, sc = gl.IsEnabled(GL_SCISSOR_TEST) != 0;
    GLint oldBox[4] = {0, 0, 0, 0};
    if (sc) gl.GetIntegerv(GL_SCISSOR_BOX, oldBox);
    gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_SCISSOR_TEST);
    gl.PushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    gl.EnableClientState(GL_VERTEX_ARRAY);
    gl.EnableClientState(GL_TEXTURE_COORD_ARRAY);
    gl.EnableClientState(GL_COLOR_ARRAY);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.Translatef((float)left, (float)top, 0.0f);   // ImGui's y runs down from the window's top-left corner
    gl.Scalef(1.0f, -1.0f, 1.0f);
    gl.Color4f(1, 1, 1, 1);
    int boundTex = -1;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const ImDrawVert* vb = cl->VtxBuffer.Data;
        const ImDrawIdx* ib = cl->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) { if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(cl, &cmd); continue; }
            if (cmd.ClipRect.z <= cmd.ClipRect.x || cmd.ClipRect.w <= cmd.ClipRect.y) continue;
            if (xf.ok) {
                float x0, y0, x1, y1;
                toNative(xf, (float)left + cmd.ClipRect.x, (float)top - cmd.ClipRect.w, x0, y0);
                toNative(xf, (float)left + cmd.ClipRect.z, (float)top - cmd.ClipRect.y, x1, y1);
                int sx = (int)std::floor(std::min(x0, x1)), sy = (int)std::floor(std::min(y0, y1));
                int ex = (int)std::ceil(std::max(x0, x1)), ey = (int)std::ceil(std::max(y0, y1));
                if (sc) {   // stay inside the scissor X-Plane had set (pop-out and VR windows)
                    sx = std::max(sx, oldBox[0]); sy = std::max(sy, oldBox[1]);
                    ex = std::min(ex, oldBox[0] + oldBox[2]); ey = std::min(ey, oldBox[1] + oldBox[3]);
                }
                if (ex <= sx || ey <= sy) continue;
                gl.Scissor(sx, sy, ex - sx, ey - sy);
            }
            const int tex = (int)(intptr_t)cmd.GetTexID();
            if (tex != boundTex) { XPLMBindTexture2d(tex, 0); boundTex = tex; }
            const ImDrawVert* v = vb + cmd.VtxOffset;
            gl.VertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, pos)));
            gl.TexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, uv)));
            gl.ColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, col)));
            gl.DrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, ib + cmd.IdxOffset);
        }
    }
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.Color4f(1, 1, 1, 1);   // leave X-Plane's drawing as we found it (white, no texture of ours bound)
    XPLMBindTexture2d(0, 0);
    if (sc) gl.Scissor(oldBox[0], oldBox[1], oldBox[2], oldBox[3]); else gl.Disable(GL_SCISSOR_TEST);
    if (cull) gl.Enable(GL_CULL_FACE);
}

// ---- XPLM callbacks --------------------------------------------------------------------------------------------------------------
void draw(XPLMWindowID w, void*) {
    if (!g_ctx || !g_build || !loadGl()) return;
    ImGui::SetCurrentContext(g_ctx);
    int l, t, r, b;
    XPLMGetWindowGeometry(w, &l, &t, &r, &b);
    const float ww = (float)std::max(1, r - l), wh = (float)std::max(1, t - b);
    // pixels per window unit, for crisp text at X-Plane's UI scale and on high-DPI screens
    float scale = 1.0f;
    {
        const Xform xf = readXform();
        if (xf.ok) {
            float x0, y0, x1, y1;
            toNative(xf, (float)l, (float)b, x0, y0);
            toNative(xf, (float)r, (float)b, x1, y1);
            scale = std::max(1.0f, std::min(4.0f, std::fabs(x1 - x0) / ww));
        }
    }
    if (g_fontsDirty || std::fabs(scale - g_fontScale) > 0.05f * g_fontScale) buildFonts(scale);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(ww, wh);
    const auto now = std::chrono::steady_clock::now();
    float dt = g_last.time_since_epoch().count() ? std::chrono::duration<float>(now - g_last).count() : 1.0f / 60.0f;
    io.DeltaTime = std::max(1e-4f, std::min(0.5f, dt));
    g_last = now;
    // the mouse: the cursor callback says where it is while it is over the window
    if (++g_cursorFrames > 2) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    else io.AddMousePosEvent(g_mouseX - (float)l, (float)t - g_mouseY);
    ImGui::NewFrame();
    g_build(g_fonts, ww, wh, XPLMWindowIsPoppedOut(w) != 0);
    ImGui::Render();
    // keyboard focus only while text is being typed
    if (io.WantTextInput && !g_hasKeyFocus) { XPLMTakeKeyboardFocus(w); g_hasKeyFocus = true; }
    else if (!io.WantTextInput && g_hasKeyFocus) { if (XPLMHasKeyboardFocus(w)) XPLMTakeKeyboardFocus(nullptr); g_hasKeyFocus = false; }
    dimVertices(ImGui::GetDrawData(), nightDim(XPLMWindowIsPoppedOut(w) != 0));
    renderDrawData(ImGui::GetDrawData(), l, t);
}

void track(int x, int y) { g_mouseX = (float)x; g_mouseY = (float)y; g_cursorFrames = 0; if (g_ctx) { ImGui::SetCurrentContext(g_ctx); int l, t, r, b; XPLMGetWindowGeometry(g_win, &l, &t, &r, &b); ImGui::GetIO().AddMousePosEvent((float)(x - l), (float)(t - y)); } }

int clickAny(int button, int x, int y, XPLMMouseStatus st) {
    if (!g_ctx) return 1;
    track(x, y);
    ImGui::SetCurrentContext(g_ctx);
    if (st == xplm_MouseDown) ImGui::GetIO().AddMouseButtonEvent(button, true);
    else if (st == xplm_MouseUp) ImGui::GetIO().AddMouseButtonEvent(button, false);
    return 1;
}
int click(XPLMWindowID, int x, int y, XPLMMouseStatus st, void*) { return clickAny(0, x, y, st); }
int rclick(XPLMWindowID, int x, int y, XPLMMouseStatus st, void*) { return clickAny(1, x, y, st); }
XPLMCursorStatus cursor(XPLMWindowID, int x, int y, void*) { track(x, y); return xplm_CursorDefault; }
int wheel(XPLMWindowID, int x, int y, int axis, int clicks, void*) {
    if (!g_ctx) return 1;
    track(x, y);
    ImGui::SetCurrentContext(g_ctx);
    if (axis == 0) ImGui::GetIO().AddMouseWheelEvent(0.0f, (float)clicks);
    else ImGui::GetIO().AddMouseWheelEvent(-(float)clicks, 0.0f);
    return 1;
}
// Keys reach the window only while it has X-Plane's keyboard focus, which it takes while a text field (the airport search) is being
// typed in and gives back as soon as it is not, so X-Plane keeps its own shortcuts the rest of the time.
void key(XPLMWindowID, char ch, XPLMKeyFlags flags, char vkey, void*, int losing) {
    if (!g_ctx || losing) { if (losing) g_hasKeyFocus = false; return; }
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO& io = ImGui::GetIO();
    const bool down = (flags & 8) != 0, up = (flags & 16) != 0;   // xplm_DownFlag, xplm_UpFlag
    ImGuiKey k = ImGuiKey_None;
    switch ((unsigned char)vkey) {   // XPLM_VK_* codes
        case 0x08: k = ImGuiKey_Backspace; break;
        case 0x09: k = ImGuiKey_Tab; break;
        case 0x0D: k = ImGuiKey_Enter; break;
        case 0x1B: k = ImGuiKey_Escape; break;
        case 0x23: k = ImGuiKey_End; break;
        case 0x24: k = ImGuiKey_Home; break;
        case 0x25: k = ImGuiKey_LeftArrow; break;
        case 0x27: k = ImGuiKey_RightArrow; break;
        case 0x2E: k = ImGuiKey_Delete; break;
        default: break;
    }
    if (k != ImGuiKey_None) { if (down) io.AddKeyEvent(k, true); if (up || down) io.AddKeyEvent(k, false); }
    else if (down && (unsigned char)ch >= 32 && (unsigned char)ch < 127) io.AddInputCharacter((unsigned int)(unsigned char)ch);
}

} // namespace

void createWindow();
void ensureWindowContext() {
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        ImGuiContext* prev = ImGui::GetCurrentContext();
        g_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendPlatformName = "wxplane_xplm";
        io.BackendRendererName = "wxplane_gl1";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.ConfigDebugHighlightIdConflicts = false;   // ImGui's red debug outlines (seen when clicking on the map)
        io.ConfigErrorRecoveryEnableTooltip = false;  // and its red error tooltips
        uiStyle();
        g_fontsDirty = true;
        if (prev) ImGui::SetCurrentContext(prev);
    }
}

void open(BuildFn build) {
    g_build = build;
    ensureWindowContext();
    if (!g_win) createWindow();
    if (!g_win) return;
    XPLMSetWindowIsVisible(g_win, 1);
    XPLMBringWindowToFront(g_win);
}

// the in-sim window, top left of the screen (made on open, and again on pop-in: see tick)
void createWindow() {
    {
        int sl, st, sr, sb;
        XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
        const int w = std::min(1240, std::max(600, sr - sl - 80)), h = std::min(820, std::max(420, st - sb - 120));
        XPLMCreateWindow_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.left = sl + 40; p.right = p.left + w;
        p.top = st - 70; p.bottom = p.top - h;
        p.visible = 1;
        p.drawWindowFunc = draw;
        p.handleMouseClickFunc = click;
        p.handleRightClickFunc = rclick;
        p.handleKeyFunc = key;
        p.handleCursorFunc = cursor;
        p.handleMouseWheelFunc = wheel;
        p.refcon = nullptr;
        p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
        p.layer = xplm_WindowLayerFloatingWindows;
        g_win = XPLMCreateWindowEx(&p);
        if (!g_win) return;
        XPLMSetWindowTitle(g_win, "WXPlane " WX_VERSION);
        XPLMSetWindowResizingLimits(g_win, 900, 560, 4000, 3000);
        if (!g_drVr) g_drVr = XPLMFindDataRef("sim/graphics/VR/enabled");
        if (g_drVr && XPLMGetDatai(g_drVr)) XPLMSetWindowPositioningMode(g_win, xplm_WindowVR, -1);
    }
}

bool isOpen() {
    if (g_win && XPLMGetWindowIsVisible(g_win)) return true;
    // shown on another plugin's screen within the last second: the window's data is wanted just the same
    return g_embedAt.time_since_epoch().count() && std::chrono::duration<double>(std::chrono::steady_clock::now() - g_embedAt).count() < 1.0;
}
void hide() {
    if (!g_win) return;
    if (g_hasKeyFocus && XPLMHasKeyboardFocus(g_win)) XPLMTakeKeyboardFocus(nullptr);
    g_hasKeyFocus = false;
    XPLMSetWindowIsVisible(g_win, 0);
}

// Popping out and back in is only asked for here (the button is pressed while the window is being drawn); the change itself is
// made by tick(), from the flight loop. Changing a window's positioning mode inside its own draw callback crashed X-Plane on
// pop-in (a customer's Log.txt, 2026-11-12: the crash came right after X-Plane rebuilt the popped-out window's swapchain).
bool g_popPending = false;
void setNightDim(bool on) { g_nightDimOn = on; }
void togglePopOut() { if (g_win) g_popPending = true; }

void tick() {
    if (!g_popPending || !g_win) return;
    g_popPending = false;
    if (g_ctx) {   // the click that pressed the button never gets its mouse-up in the old window: release it
        ImGui::SetCurrentContext(g_ctx);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (XPLMWindowIsPoppedOut(g_win)) {
        // Pop-in: the popped-out window is closed and a new in-sim window made, instead of switching the same window back to an
        // in-sim position. Switching it back left X-Plane's scene dark (its exposure, after it rebuilt the main view's offscreens
        // for the departing OS window, 2026-11-12), and every pop-out made another OS window that was never closed.
        if (g_hasKeyFocus && XPLMHasKeyboardFocus(g_win)) XPLMTakeKeyboardFocus(nullptr);
        g_hasKeyFocus = false;
        XPLMDestroyWindow(g_win);
        g_win = nullptr;
        createWindow();
        if (g_win) { XPLMSetWindowIsVisible(g_win, 1); XPLMBringWindowToFront(g_win); }
    } else {
        XPLMSetWindowPositioningMode(g_win, xplm_WindowPopOut, -1);
    }
    g_fontsDirty = true;   // a popped-out window has its own pixel scale
}

// ---- on another plugin's screen (embed.h) ----------------------------------------------------------------------------------------------
namespace {

ImDrawVert lerpVert(const ImDrawVert& a, const ImDrawVert& b, float t) {
    ImDrawVert o;
    o.pos = ImVec2(a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t);
    o.uv = ImVec2(a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t);
    ImU32 c = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const float ca = (float)((a.col >> sh) & 0xFF), cb = (float)((b.col >> sh) & 0xFF);
        c |= (ImU32)std::lround(ca + (cb - ca) * t) << sh;
    }
    o.col = c;
    return o;
}
// Sutherland-Hodgman against one edge. axis 0: x, 1: y. keepGreater: keep the side where the coordinate >= limit.
int clipEdge(const ImDrawVert* in, int n, ImDrawVert* out, int axis, float limit, bool keepGreater) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ImDrawVert& a = in[i];
        const ImDrawVert& b = in[(i + 1) % n];
        const float ca = axis ? a.pos.y : a.pos.x, cb = axis ? b.pos.y : b.pos.x;
        const bool ia = keepGreater ? ca >= limit : ca <= limit, ib = keepGreater ? cb >= limit : cb <= limit;
        if (ia) out[m++] = a;
        if (ia != ib) out[m++] = lerpVert(a, b, (limit - ca) / (cb - ca));
    }
    return m;
}

struct EmbedBatch { int tex; std::vector<ImDrawVert> v; };
std::vector<EmbedBatch> g_embedBatches;   // the embedded picture: plain triangles, each already cut to its clip rectangle

// The draw lists as triangles that need no scissor test. The surface may be an aircraft's panel texture, where a scissor box in
// screen pixels means nothing, so every triangle is cut to its command's clip rectangle here instead.
void clipDrawData(const ImDrawData* dd, std::vector<EmbedBatch>& out) {
    out.clear();
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const float x0 = cmd.ClipRect.x, y0 = cmd.ClipRect.y, x1 = cmd.ClipRect.z, y1 = cmd.ClipRect.w;
            if (x1 <= x0 || y1 <= y0) continue;
            const int tex = (int)(intptr_t)cmd.GetTexID();
            if (out.empty() || out.back().tex != tex) { out.emplace_back(); out.back().tex = tex; }
            std::vector<ImDrawVert>& dst = out.back().v;
            for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
                ImDrawVert tri[3];
                bool inside = true;
                for (int k = 0; k < 3; ++k) {
                    tri[k] = cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + e + k]];
                    inside = inside && tri[k].pos.x >= x0 && tri[k].pos.x <= x1 && tri[k].pos.y >= y0 && tri[k].pos.y <= y1;
                }
                if (inside) { dst.insert(dst.end(), tri, tri + 3); continue; }
                const float mnx = std::min({tri[0].pos.x, tri[1].pos.x, tri[2].pos.x}), mxx = std::max({tri[0].pos.x, tri[1].pos.x, tri[2].pos.x});
                const float mny = std::min({tri[0].pos.y, tri[1].pos.y, tri[2].pos.y}), mxy = std::max({tri[0].pos.y, tri[1].pos.y, tri[2].pos.y});
                if (mxx <= x0 || mnx >= x1 || mxy <= y0 || mny >= y1) continue;
                ImDrawVert a[8], b[8];   // a triangle cut by four edges has at most 7 corners
                int m = clipEdge(tri, 3, a, 0, x0, true);
                m = clipEdge(a, m, b, 0, x1, false);
                m = clipEdge(b, m, a, 1, y0, true);
                m = clipEdge(a, m, b, 1, y1, false);
                for (int k = 1; k + 1 < m; ++k) { dst.push_back(b[0]); dst.push_back(b[k]); dst.push_back(b[k + 1]); }
            }
        }
    }
}

} // namespace

void drawEmbedded(BuildFn build, WxEmbedRequest& rq) {
    if (rq.structSize < (int)sizeof(WxEmbedRequest) || rq.version < 1 || rq.widthPx < 50 || rq.heightPx < 50 || !build || !loadGl()) return;
    rq.handled = 1;
    g_embedAt = std::chrono::steady_clock::now();
    ensureWindowContext();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    // The screen is laid out at the window's smallest size or larger (900 x 560 units): on a small tablet it is scaled down to fit
    // rather than squeezed. s: the caller's pixels per layout unit.
    const float s = std::max(0.25f, std::min(1.6f, std::min(rq.widthPx / 900.0f, rq.heightPx / 560.0f)));
    const float W = rq.widthPx / s, H = rq.heightPx / s;
    if (rq.frame != g_embedFrame || g_embedBatches.empty()) {
        g_embedFrame = rq.frame;
        ImGui::SetCurrentContext(g_ctx);
        if (g_fontsDirty || g_fontScale <= 0) buildFonts(std::max(1.0f, std::min(4.0f, s * std::max(0.5f, rq.pixelsPerUnit))));   // else: the window's fonts, as they are
        if (!g_ctxEmbed) {
            g_ctxEmbed = ImGui::CreateContext(ImGui::GetIO().Fonts);   // the same font atlas and texture
            ImGui::SetCurrentContext(g_ctxEmbed);
            ImGuiIO& eio = ImGui::GetIO();
            eio.IniFilename = nullptr; eio.LogFilename = nullptr;
            eio.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            eio.ConfigDebugHighlightIdConflicts = false;
            eio.ConfigErrorRecoveryEnableTooltip = false;
            uiStyle();
        }
        ImGui::SetCurrentContext(g_ctxEmbed);
        ImGuiIO& io = ImGui::GetIO();
        io.FontDefault = g_fonts.body;                              // the atlas may have been rebuilt by the window since
        io.FontGlobalScale = 1.0f / std::max(1.0f, g_fontScale);
        io.DisplaySize = ImVec2(W, H);
        io.DeltaTime = 1.0f / 60.0f;
        const bool over = rq.mouseX >= 0 && rq.mouseY >= 0 && rq.mouseX <= rq.widthPx && rq.mouseY <= rq.heightPx;
        if (over || g_embedDown) io.AddMousePosEvent(rq.mouseX / s, rq.mouseY / s); else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        const bool down = rq.mouseDown != 0 && (over || g_embedDown);
        if (down != g_embedDown) { io.AddMouseButtonEvent(0, down); g_embedDown = down; }
        if (rq.wheel != 0 && over) io.AddMouseWheelEvent(0.0f, rq.wheel);
        ImGui::NewFrame();
        uiSetEmbedded(true);
        build(g_fonts, W, H, false);
        uiSetEmbedded(false);
        ImGui::Render();
        clipDrawData(ImGui::GetDrawData(), g_embedBatches);
        const float k = std::max(0.0f, std::min(1.0f, rq.brightness));
        if (k < 0.999f) {
            const unsigned m = (unsigned)(k * 256.0f + 0.5f);
            for (EmbedBatch& b : g_embedBatches)
                for (ImDrawVert& v : b.v) {
                    const ImU32 c = v.col;
                    const unsigned r = ((c >> IM_COL32_R_SHIFT) & 0xFF) * m >> 8, g = ((c >> IM_COL32_G_SHIFT) & 0xFF) * m >> 8, bl = ((c >> IM_COL32_B_SHIFT) & 0xFF) * m >> 8;
                    v.col = (c & IM_COL32_A_MASK) | (r << IM_COL32_R_SHIFT) | (g << IM_COL32_G_SHIFT) | (bl << IM_COL32_B_SHIFT);
                }
        }
    }
    // draw: the caller's matrix has the area's top-left corner at the origin, y down, one unit per pixel
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const bool cull = gl.IsEnabled(GL_CULL_FACE) != 0;
    gl.Disable(GL_CULL_FACE);
    gl.PushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    gl.EnableClientState(GL_VERTEX_ARRAY);
    gl.EnableClientState(GL_TEXTURE_COORD_ARRAY);
    gl.EnableClientState(GL_COLOR_ARRAY);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.Scalef(s, s, 1.0f);
    gl.Color4f(1, 1, 1, 1);
    for (const EmbedBatch& b : g_embedBatches) {
        if (b.v.empty()) continue;
        XPLMBindTexture2d(b.tex, 0);
        const ImDrawVert* v = b.v.data();
        gl.VertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, pos)));
        gl.TexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, uv)));
        gl.ColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, col)));
        gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)b.v.size());
    }
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.Color4f(1, 1, 1, 1);
    XPLMBindTexture2d(0, 0);
    if (cull) gl.Enable(GL_CULL_FACE);
    if (prev) ImGui::SetCurrentContext(prev);
}

void destroy() {
    if (g_win) { XPLMDestroyWindow(g_win); g_win = nullptr; }
    if (g_ctxEmbed) { ImGui::DestroyContext(g_ctxEmbed); g_ctxEmbed = nullptr; }   // before the context whose fonts it shares
    if (g_ctx) {
        ImGui::SetCurrentContext(g_ctx);
        ImGui::DestroyContext(g_ctx);
        g_ctx = nullptr;
    }
    if (g_fontTex && gl.DeleteTextures) { const unsigned int t = (unsigned int)g_fontTex; gl.DeleteTextures(1, &t); }
    if (g_logoTex && gl.DeleteTextures) { const unsigned int t = (unsigned int)g_logoTex; gl.DeleteTextures(1, &t); }
    g_fontTex = 0; g_logoTex = 0;
    g_fontsDirty = true;
}


// ---- the AviTab tab ---------------------------------------------------------------------------------------------------------------
// AviTab has no add-on interface; like AviTab Browser, WXPlane draws on AviTab's own screen in the 3D cockpit: an icon on its home
// screen, and, once that is tapped (AviTab is sent to its About page underneath), the WXPlane map over the screen below AviTab's
// header, so AviTab's own Home button still takes the tablet back. Taps come from X-Plane's 3D click position (click_3d_*_pixels,
// panel pixels) through an invisible full-screen window that passes on every click it does not use. AviTab publishes where its
// screen is on the panel (avitab/panel_left/bottom/width/height); its 800 x 480 picture is centred in that rectangle.
namespace tab {
struct Refs {
    XPLMDataRef l = nullptr, b = nullptr, w = nullptr, h = nullptr, pow = nullptr, en = nullptr, menu = nullptr, bright = nullptr;
    XPLMDataRef cx = nullptr, cy = nullptr, vr = nullptr, browser = nullptr;
    XPLMCommandRef about = nullptr;
    bool ok() const { return l && b && w && h && pow && en && menu && cx && cy; }
} R;
TabletBuildFn g_tbuild = nullptr;
ImGuiContext* g_tctx = nullptr;
UiFonts g_tfonts;
int g_tfontTex = 0;
float g_tfontScale = 0;
XPLMWindowID g_cap = nullptr;
bool g_registered = false, g_shown = false, g_wantShow = false, g_down = false;
float g_iconX = 0.357f, g_iconY = 0.568f;   // the icon's centre on AviTab's home screen (0..1 from the left / the bottom)
std::chrono::steady_clock::time_point g_tlast{};
float g_nx = -1, g_ny = -1;   // the last tap position on the tablet (0..1)
constexpr float kLogicalW = 1000.0f;   // the tablet's picture is laid out 1000 units wide
constexpr float kHeaderFrac = 0.068f;  // AviTab's header (clock, Home) at the top of its screen: left to AviTab

// what the tab sees, written to Log.txt whenever it changes (so a missing icon can be traced)
void note(const std::string& what) {
    static std::string last;
    if (what == last) return;
    last = what;
    XPLMDebugString(("[WXPlane] AviTab tab: " + what + "\n").c_str());
}
void findRefs() {
    if (R.ok()) return;
    R.l = XPLMFindDataRef("avitab/panel_left"); R.b = XPLMFindDataRef("avitab/panel_bottom");
    R.w = XPLMFindDataRef("avitab/panel_width"); R.h = XPLMFindDataRef("avitab/panel_height");
    R.pow = XPLMFindDataRef("avitab/panel_powered"); R.en = XPLMFindDataRef("avitab/panel_enabled");
    R.menu = XPLMFindDataRef("avitab/is_in_menu"); R.bright = XPLMFindDataRef("avitab/brightness");
    R.cx = XPLMFindDataRef("sim/graphics/view/click_3d_x_pixels"); R.cy = XPLMFindDataRef("sim/graphics/view/click_3d_y_pixels");
    R.vr = XPLMFindDataRef("sim/graphics/VR/enabled");
    R.browser = XPLMFindDataRef("avitab_browser/visible");
    R.about = XPLMFindCommand("AviTab/app_about");
}
// AviTab's 800 x 480 screen on the panel (pixels), centred in its rectangle
bool screen(float& x, float& y, float& w, float& h) {
    if (!R.ok()) return false;
    x = (float)XPLMGetDatai(R.l); y = (float)XPLMGetDatai(R.b); w = (float)XPLMGetDatai(R.w); h = (float)XPLMGetDatai(R.h);
    if (w < 50 || h < 30) return false;
    const float ah = w * 0.6f;
    if (h > ah) { y += (h - ah) * 0.5f; h = ah; }
    return true;
}
// XPTablet publishes AviTab's datarefs itself when it stands in for AviTab (and says so in xptablet/owns_avitab). WXPlane is not
// drawn over its screen: XPTablet shows the whole WXPlane window on its Weather page instead (embed.h, drawEmbedded).
bool xpTablet() {
    static XPLMDataRef own = nullptr;
    if (!own) own = XPLMFindDataRef("xptablet/owns_avitab");
    return own && XPLMGetDatai(own) != 0;
}
bool powered() { return R.ok() && !xpTablet() && XPLMGetDatai(R.pow) && XPLMGetDatai(R.en); }
bool inMenu() { return R.ok() && XPLMGetDatai(R.menu) != 0; }
bool browserShown() { return R.browser && XPLMGetDatai(R.browser) != 0; }
// the tap, as 0..1 across AviTab's screen (from the left, from the bottom)
bool tapPos(float& nx, float& ny) {
    float x, y, w, h;
    if (!screen(x, y, w, h)) return false;
    const float px = XPLMGetDataf(R.cx), py = XPLMGetDataf(R.cy);
    if (px < 0 || py < 0) return false;
    nx = (px - x) / w; ny = (py - y) / h;
    return nx >= 0 && nx <= 1 && ny >= 0 && ny <= 1;
}
void iconRect(float w, float h, float& x0, float& y0, float& x1, float& y1) {   // in screen pixels from its bottom-left
    const float iw = w * 0.125f, ih = iw * 1.12f;
    x0 = g_iconX * w - iw * 0.5f; x1 = x0 + iw; y0 = g_iconY * h - ih * 0.5f; y1 = y0 + ih;
}
bool onIcon(float nx, float ny) {
    float x, y, w, h, x0, y0, x1, y1;
    if (!screen(x, y, w, h)) return false;
    iconRect(w, h, x0, y0, x1, y1);
    return nx * w >= x0 && nx * w <= x1 && ny * h >= y0 && ny * h <= y1;
}

void buildTabletFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_tfonts = uiLoadFonts(scale);
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!g_tfontTex) XPLMGenerateTextureNumbers(&g_tfontTex, 1);
    XPLMBindTexture2d(g_tfontTex, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint)GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_tfontTex);
    io.Fonts->ClearTexData();
    g_tfonts.logo = nullptr;
    g_tfontScale = scale;
}

// ImGui output onto the panel: `ox, oy` the top-left corner of the picture in panel pixels, `k` panel pixels per unit (y up on the
// panel, down in ImGui). Clipping with the GL matrices of the panel pass (the panel texture's own pixels).
void renderOnPanel(ImDrawData* dd, float ox, float oy, float k) {
    float mv[16], pr[16];
    GLint vp[4];
    gl.GetFloatv(GL_MODELVIEW_MATRIX, mv);
    gl.GetFloatv(GL_PROJECTION_MATRIX, pr);
    gl.GetIntegerv(GL_VIEWPORT, vp);
    Xform xf;
    std::memcpy(xf.mv, mv, sizeof mv); std::memcpy(xf.pr, pr, sizeof pr);
    xf.vp[0] = vp[0]; xf.vp[1] = vp[1]; xf.vp[2] = vp[0] + vp[2]; xf.vp[3] = vp[1] + vp[3]; xf.ok = true;
    {   // the picture's corners must land in the viewport, or the clipping would hide it: then no clipping at all
        float ax, ay, bx, by;
        toNative(xf, ox, oy - dd->DisplaySize.y * k, ax, ay);
        toNative(xf, ox + dd->DisplaySize.x * k, oy, bx, by);
        const bool sane = std::isfinite(ax) && std::isfinite(bx) && std::fabs(bx - ax) > 4 && std::fabs(by - ay) > 4 &&
                          std::max(ax, bx) > xf.vp[0] && std::min(ax, bx) < xf.vp[2] && std::max(ay, by) > xf.vp[1] && std::min(ay, by) < xf.vp[3];
        if (!sane) xf.ok = false;
        static bool told = false;
        if (!told) {
            told = true;
            char b[220];
            std::snprintf(b, sizeof b, "[WXPlane] AviTab tab: panel pass viewport %d,%d %dx%d, picture at %.0f,%.0f-%.0f,%.0f there (%s)\n", vp[0], vp[1], vp[2], vp[3], ax, ay, bx, by, sane ? "clipped" : "not clipped");
            XPLMDebugString(b);
        }
    }
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const bool cull = gl.IsEnabled(GL_CULL_FACE) != 0, sc = gl.IsEnabled(GL_SCISSOR_TEST) != 0;
    GLint oldBox[4] = {0, 0, 0, 0};
    if (sc) gl.GetIntegerv(GL_SCISSOR_BOX, oldBox);
    gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_SCISSOR_TEST);
    gl.PushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    gl.EnableClientState(GL_VERTEX_ARRAY);
    gl.EnableClientState(GL_TEXTURE_COORD_ARRAY);
    gl.EnableClientState(GL_COLOR_ARRAY);
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.Translatef(ox, oy, 0.0f);
    gl.Scalef(k, -k, 1.0f);
    gl.Color4f(1, 1, 1, 1);
    int boundTex = -1;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const ImDrawVert* vb = cl->VtxBuffer.Data;
        const ImDrawIdx* ib = cl->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            if (cmd.ClipRect.z <= cmd.ClipRect.x || cmd.ClipRect.w <= cmd.ClipRect.y) continue;
            if (!xf.ok) gl.Disable(GL_SCISSOR_TEST);
            float x0, y0, x1, y1;
            if (xf.ok) {
            toNative(xf, ox + cmd.ClipRect.x * k, oy - cmd.ClipRect.w * k, x0, y0);
            toNative(xf, ox + cmd.ClipRect.z * k, oy - cmd.ClipRect.y * k, x1, y1);
            int sx = (int)std::floor(std::min(x0, x1)), sy = (int)std::floor(std::min(y0, y1));
            int ex = (int)std::ceil(std::max(x0, x1)), ey = (int)std::ceil(std::max(y0, y1));
            if (sc) { sx = std::max(sx, oldBox[0]); sy = std::max(sy, oldBox[1]); ex = std::min(ex, oldBox[0] + oldBox[2]); ey = std::min(ey, oldBox[1] + oldBox[3]); }
            if (ex <= sx || ey <= sy) continue;
            gl.Scissor(sx, sy, ex - sx, ey - sy);
            }
            const int tex = (int)(intptr_t)cmd.GetTexID();
            if (tex != boundTex) { XPLMBindTexture2d(tex, 0); boundTex = tex; }
            const ImDrawVert* v = vb + cmd.VtxOffset;
            gl.VertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, pos)));
            gl.TexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, uv)));
            gl.ColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const void*)((const char*)v + offsetof(ImDrawVert, col)));
            gl.DrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, ib + cmd.IdxOffset);
        }
    }
    gl.PopMatrix();
    gl.PopClientAttrib();
    gl.Color4f(1, 1, 1, 1);
    XPLMBindTexture2d(0, 0);
    if (sc) gl.Scissor(oldBox[0], oldBox[1], oldBox[2], oldBox[3]); else gl.Disable(GL_SCISSOR_TEST);
    if (cull) gl.Enable(GL_CULL_FACE);
}

// the tablet's own ImGui context (current on return) with its fonts for `sw` panel pixels across
void ensureContext(float sw) {
    if (!g_tctx) {
        g_tctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_tctx);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        uiStyle();
    }
    ImGui::SetCurrentContext(g_tctx);
    const float scale = std::max(1.0f, std::min(3.0f, sw / kLogicalW * 1.5f));
    if (std::fabs(scale - g_tfontScale) > 0.05f) buildTabletFonts(scale);
}

// one ImGui frame on the tablet: the map over AviTab's About page
void frame(float sx, float sy, float sw, float sh) {
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ensureContext(sw);
    const float areaH = sh * (1.0f - kHeaderFrac);   // below AviTab's header
    const float k = sw / kLogicalW;                   // panel pixels per unit
    ImGuiIO& io = ImGui::GetIO();
    const float W = kLogicalW, H = areaH / k;
    io.DisplaySize = ImVec2(W, H);
    const auto now = std::chrono::steady_clock::now();
    float dt = g_tlast.time_since_epoch().count() ? std::chrono::duration<float>(now - g_tlast).count() : 1.0f / 30.0f;
    io.DeltaTime = std::max(1e-4f, std::min(0.5f, dt));
    g_tlast = now;
    if (g_nx >= 0) io.AddMousePosEvent(g_nx * W, (1.0f - g_ny / (1.0f - kHeaderFrac)) * H);
    else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ImGui::NewFrame();
    if (g_tbuild) g_tbuild(g_tfonts, W, H);
    ImGui::Render();
    const float bright = R.bright ? std::max(0.15f, std::min(1.0f, XPLMGetDataf(R.bright))) : 1.0f;
    dimVertices(ImGui::GetDrawData(), bright);
    renderOnPanel(ImGui::GetDrawData(), sx, sy + areaH, k);
    if (prev) ImGui::SetCurrentContext(prev);
}

void drawIcon(float sx, float sy, float sw, float sh) {
    float x0, y0, x1, y1;
    iconRect(sw, sh, x0, y0, x1, y1);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ensureContext(sw);
    // drawn with ImGui too: a rounded square with "WX" and a label, the size of AviTab's own icons
    const float k = sw / kLogicalW;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(kLogicalW, sh / k);
    io.DeltaTime = 1.0f / 30.0f;
    ImGui::NewFrame();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 a(x0 / k, (sh - y1) / k), b(x1 / k, (sh - y0) / k);
    const float r = (b.x - a.x) * 0.16f;
    const float iconH = (b.x - a.x);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + iconH), IM_COL32(20, 34, 52, 255), r);
    dl->AddRect(a, ImVec2(b.x, a.y + iconH), IM_COL32(110, 168, 255, 255), r, 0, 2.0f);
    const float fs = iconH * 0.42f;
    const ImVec2 ts = g_tfonts.title->CalcTextSizeA(fs, FLT_MAX, 0, "WX");
    dl->AddText(g_tfonts.title, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + (iconH - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), "WX");
    const float ls = iconH * 0.16f;
    const ImVec2 lsz = g_tfonts.body->CalcTextSizeA(ls, FLT_MAX, 0, "WXPlane");
    dl->AddText(g_tfonts.body, ls, ImVec2((a.x + b.x - lsz.x) * 0.5f, a.y + iconH + 2), IM_COL32(255, 255, 255, 255), "WXPlane");
    ImGui::Render();
    renderOnPanel(ImGui::GetDrawData(), sx, sy + sh, k);
    if (prev) ImGui::SetCurrentContext(prev);
}

int drawCb(XPLMDrawingPhase, int, void*) {
    if (!g_tbuild || !loadGl()) return 1;
    findRefs();
    if (!R.ok()) { note("AviTab's datarefs not found (is AviTab installed and loaded?)"); return 1; }
    if (xpTablet()) { note("the cockpit tablet is XPTablet: WXPlane shows on its Weather page, nothing is drawn over it"); return 1; }
    float x, y, w, h;
    if (!powered()) { note("AviTab is off or not shown in this cockpit (panel_powered " + std::to_string(XPLMGetDatai(R.pow)) + ", panel_enabled " + std::to_string(XPLMGetDatai(R.en)) + ")"); return 1; }
    if (!screen(x, y, w, h)) { note("AviTab's screen has no size on the panel (" + std::to_string(XPLMGetDatai(R.w)) + " x " + std::to_string(XPLMGetDatai(R.h)) + ")"); return 1; }
    char b[200];
    std::snprintf(b, sizeof b, "screen at %.0f,%.0f size %.0fx%.0f on the panel; %s", x, y, w, h,
                  g_shown ? "the map is shown" : !inMenu() ? "AviTab is in one of its apps (the icon shows on its home screen)" : browserShown() ? "AviTab Browser is showing" : "icon drawn on the home screen");
    note(b);
    if (g_shown) frame(x, y, w, h);
    else if (inMenu() && !browserShown()) drawIcon(x, y, w, h);
    return 1;
}

void imguiMouse(bool down) {
    if (!g_tctx) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_tctx);
    ImGui::GetIO().AddMouseButtonEvent(0, down);
    if (prev) ImGui::SetCurrentContext(prev);
}

int capClick(XPLMWindowID, int, int, XPLMMouseStatus st, void*) {
    findRefs();
    if (!powered()) return 0;
    float nx, ny;
    const bool on = tapPos(nx, ny);
    if (st == xplm_MouseDown) {
        if (!on) return 0;
        if (!g_shown) {
            if (inMenu() && !browserShown() && onIcon(nx, ny)) {   // the icon: AviTab to its About page, the map over it
                if (R.about) XPLMCommandOnce(R.about);
                g_wantShow = true;
                return 1;
            }
            return 0;
        }
        if (ny > 1.0f - kHeaderFrac) return 0;   // AviTab's header (its Home button): AviTab's
        g_nx = nx; g_ny = ny; g_down = true;
        imguiMouse(true);
        return 1;
    }
    if (!g_down) return 0;
    if (on) { g_nx = nx; g_ny = std::min(ny, 1.0f - kHeaderFrac); }
    if (st == xplm_MouseUp) { g_down = false; imguiMouse(false); }
    return 1;
}
int capWheel(XPLMWindowID, int, int, int axis, int clicks, void*) {
    if (!g_shown || !g_tctx) return 0;
    float nx, ny;
    if (!tapPos(nx, ny) || ny > 1.0f - kHeaderFrac) return 0;
    g_nx = nx; g_ny = ny;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_tctx);
    ImGui::GetIO().AddMouseWheelEvent(axis == 1 ? (float)clicks : 0.0f, axis == 0 ? (float)clicks : 0.0f);
    if (prev) ImGui::SetCurrentContext(prev);
    return 1;
}
XPLMCursorStatus capCursor(XPLMWindowID, int, int, void*) {
    if (g_shown && !g_down) { float nx, ny; if (tapPos(nx, ny)) { g_nx = nx; g_ny = std::min(ny, 1.0f - kHeaderFrac); } }
    return xplm_CursorDefault;
}

void makeCaptureWindow() {
    if (g_cap) return;
    if (R.vr && XPLMGetDatai(R.vr)) return;
    int l, t, r, b;
    XPLMGetScreenBoundsGlobal(&l, &t, &r, &b);
    XPLMCreateWindow_t p{};
    p.structSize = sizeof p;
    p.left = l; p.top = t; p.right = r; p.bottom = b;
    p.visible = 1;
    p.drawWindowFunc = [](XPLMWindowID, void*) {};
    p.handleMouseClickFunc = capClick;
    p.handleRightClickFunc = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 0; };
    p.handleMouseWheelFunc = capWheel;
    p.handleKeyFunc = [](XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {};
    p.handleCursorFunc = capCursor;
    p.layer = xplm_WindowLayerFlightOverlay;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    g_cap = XPLMCreateWindowEx(&p);
    XPLMSetWindowPositioningMode(g_cap, xplm_WindowFullScreenOnMonitor, -1);
}
} // namespace tab

void tabletStart(TabletBuildFn build, float iconX, float iconY) {
    tab::g_tbuild = build;
    tab::g_iconX = iconX; tab::g_iconY = iconY;
    if (!tab::g_registered) { XPLMRegisterDrawCallback(tab::drawCb, xplm_Phase_Gauges, 0, nullptr); tab::g_registered = true; }
}

bool tabletTick() {
    using namespace tab;
    if (!g_tbuild) return false;
    findRefs();
    if (!R.ok()) { g_shown = false; return false; }   // AviTab is not installed (or not loaded yet)
    if (xpTablet()) {   // XPTablet's screen: no icon, no map over it and no click window of ours
        g_shown = g_wantShow = false;
        if (g_cap) { XPLMDestroyWindow(g_cap); g_cap = nullptr; }
        return false;
    }
    makeCaptureWindow();
    // The click window covers the whole screen: it is only there while the mouse points at the tablet (or a press on it is held).
    // Up all the time, it sat over X-Plane's own pop-up messages, whose buttons then could not be pressed (2026-11-17).
    if (g_cap) { float nx, ny; XPLMSetWindowIsVisible(g_cap, (g_down || (powered() && tapPos(nx, ny))) ? 1 : 0); }
    if (g_wantShow && !inMenu()) { g_shown = true; g_wantShow = false; }
    if (g_shown && (inMenu() || !powered() || browserShown())) { g_shown = false; g_down = false; g_nx = -1; }
    return g_shown;
}

void tabletStop() {
    using namespace tab;
    if (g_registered) { XPLMUnregisterDrawCallback(drawCb, xplm_Phase_Gauges, 0, nullptr); g_registered = false; }
    if (g_cap) { XPLMDestroyWindow(g_cap); g_cap = nullptr; }
    if (g_tctx) { ImGuiContext* prev = ImGui::GetCurrentContext(); ImGui::DestroyContext(g_tctx); g_tctx = nullptr; if (prev && prev != g_tctx) ImGui::SetCurrentContext(prev); }
    if (g_tfontTex && gl.DeleteTextures) { const unsigned int t = (unsigned int)g_tfontTex; gl.DeleteTextures(1, &t); }
    g_tfontTex = 0; g_tfontScale = 0;
    g_shown = g_wantShow = false;
    g_tbuild = nullptr;
}

} // namespace xpui
} // namespace wx
