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
const GLenum GL_BLEND = 0x0BE2, GL_DEPTH_TEST = 0x0B71, GL_ALPHA_TEST = 0x0BC0, GL_FOG = 0x0B60, GL_LIGHTING = 0x0B50,
             GL_BLEND_SRC = 0x0BE1, GL_BLEND_DST = 0x0BE0, GL_MATRIX_MODE = 0x0BA0, GL_TEXTURE_BINDING_2D = 0x8069,
             GL_DEPTH_WRITEMASK = 0x0B72, GL_CURRENT_COLOR = 0x0B00;

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
    void(WXGL_API* MatrixMode)(GLenum) = nullptr;
    void(WXGL_API* PushMatrix)(void) = nullptr;
    void(WXGL_API* PopMatrix)(void) = nullptr;
    void(WXGL_API* Translatef)(float, float, float) = nullptr;
    void(WXGL_API* Scalef)(float, float, float) = nullptr;
    void(WXGL_API* PushClientAttrib)(unsigned int) = nullptr;
    void(WXGL_API* PopClientAttrib)(void) = nullptr;
    void(WXGL_API* Color4f)(float, float, float, float) = nullptr;
    void(WXGL_API* DepthMask)(GLboolean) = nullptr;
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
        ok &= get(gl.DrawElements, "glDrawElements"); ok &= get(gl.MatrixMode, "glMatrixMode"); ok &= get(gl.PushMatrix, "glPushMatrix");
        ok &= get(gl.PopMatrix, "glPopMatrix"); ok &= get(gl.Translatef, "glTranslatef"); ok &= get(gl.Scalef, "glScalef");
        ok &= get(gl.PushClientAttrib, "glPushClientAttrib"); ok &= get(gl.PopClientAttrib, "glPopClientAttrib"); ok &= get(gl.Color4f, "glColor4f");
        ok &= get(gl.DepthMask, "glDepthMask");
        gl.state = ok ? 1 : -1;
        if (!ok) XPLMDebugString("[WXPlane] weather window: OpenGL functions not found, the window cannot be drawn\n");
    }
    return gl.state == 1;
}

// ---- state ---------------------------------------------------------------------------------------------------------------------
XPLMWindowID g_win = nullptr;
ImGuiContext* g_ctx = nullptr;
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
XPLMDataRef g_drMv = nullptr, g_drProj = nullptr, g_drVp = nullptr, g_drVr = nullptr;

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
    GLint oldRow = 0, oldAlign = 4, oldTex = 0;
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &oldRow); gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &oldAlign); gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
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
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, oldRow); gl.PixelStorei(GL_UNPACK_ALIGNMENT, oldAlign);   // as X-Plane had them
    XPLMBindTexture2d(oldTex, 0);
    g_fonts.logo = g_logoTex ? (void*)(intptr_t)g_logoTex : nullptr;
    g_fonts.logoAspect = g_logoAspect;
    g_fontScale = scale;
    g_fontsDirty = false;
}

void renderDrawData(ImDrawData* dd, int left, int top) {
    const Xform xf = readXform();
    // Every bit of OpenGL state the window's drawing changes is read first and put back exactly afterwards (blending, depth,
    // fog/lighting/alpha test, texturing, blend function, matrix mode, colour, bound texture). The drawing used to leave
    // XPLMSetGraphicsState's state behind (blending on, depth test and writes off), and plugins drawing after it (ToLiss Photon's
    // cockpit lighting, for one) could draw differently: the likely reason the cockpit's exposure changed while it was open (2026-11-12).
    // XPLMSetGraphicsState is not used: it caches, and putting the state back behind its back would leave its cache wrong.
    struct Saved { GLboolean blend, depth, alpha, fog, light, tex2d; GLint src, dst, mode, tex, depthMask; float col[4]; } sv{};
    sv.blend = gl.IsEnabled(GL_BLEND); sv.depth = gl.IsEnabled(GL_DEPTH_TEST); sv.alpha = gl.IsEnabled(GL_ALPHA_TEST);
    sv.fog = gl.IsEnabled(GL_FOG); sv.light = gl.IsEnabled(GL_LIGHTING); sv.tex2d = gl.IsEnabled(GL_TEXTURE_2D);
    gl.GetIntegerv(GL_BLEND_SRC, &sv.src); gl.GetIntegerv(GL_BLEND_DST, &sv.dst); gl.GetIntegerv(GL_MATRIX_MODE, &sv.mode);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &sv.tex); gl.GetIntegerv(GL_DEPTH_WRITEMASK, &sv.depthMask); gl.GetFloatv(GL_CURRENT_COLOR, sv.col);
    auto setOn = [](GLenum c, bool on) { if (on) gl.Enable(c); else gl.Disable(c); };
    setOn(GL_BLEND, true); setOn(GL_DEPTH_TEST, false); setOn(GL_ALPHA_TEST, false); setOn(GL_FOG, false); setOn(GL_LIGHTING, false); setOn(GL_TEXTURE_2D, true);
    gl.DepthMask(0);
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
    // and everything back as it was
    if (sc) gl.Scissor(oldBox[0], oldBox[1], oldBox[2], oldBox[3]); else gl.Disable(GL_SCISSOR_TEST);
    if (cull) gl.Enable(GL_CULL_FACE);
    XPLMBindTexture2d(sv.tex, 0);
    gl.Color4f(sv.col[0], sv.col[1], sv.col[2], sv.col[3]);
    gl.BlendFunc((GLenum)sv.src, (GLenum)sv.dst);
    gl.DepthMask((GLboolean)(sv.depthMask != 0));
    gl.MatrixMode((GLenum)sv.mode);
    setOn(GL_BLEND, sv.blend != 0); setOn(GL_DEPTH_TEST, sv.depth != 0); setOn(GL_ALPHA_TEST, sv.alpha != 0);
    setOn(GL_FOG, sv.fog != 0); setOn(GL_LIGHTING, sv.light != 0); setOn(GL_TEXTURE_2D, sv.tex2d != 0);
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
void open(BuildFn build) {
    g_build = build;
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

bool isOpen() { return g_win && XPLMGetWindowIsVisible(g_win); }
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

void destroy() {
    if (g_win) { XPLMDestroyWindow(g_win); g_win = nullptr; }
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

} // namespace xpui
} // namespace wx
