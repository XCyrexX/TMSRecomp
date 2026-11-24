#pragma once
// Linux layer for launcher.cpp: the few Win32 / Direct2D / DirectWrite names the launcher's pages use, implemented
// with SDL3 (window, input, message boxes, file dialogs, clipboard) and cairo / pango (drawing, text). The page code
// stays the same on both systems; launcher.cpp has the Linux versions of the window loop, setup runner and updates.
#ifndef _WIN32
#include <SDL3/SDL.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include <cwchar>
#include <string>
#include <vector>

#include "../src/draw_linux.h"
#include "../src/plat.h"

// ------------------------------------------------------------------------------------------------ basic types
using HWND = SDL_Window*;
using UINT = unsigned;
using DWORD = uint32_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using HANDLE = void*;  // unused on Linux (pipes are file descriptors)
struct D2D1_COLOR_F { float r, g, b, a; };
struct D2D1_RECT_F { float left, top, right, bottom; };
struct D2D1_POINT_2F { float x, y; };
struct D2D1_SIZE_F { float width, height; };
struct D2D1_ELLIPSE { D2D1_POINT_2F point; float radiusX, radiusY; };
struct D2D1_ROUNDED_RECT { D2D1_RECT_F rect; float radiusX, radiusY; };
enum DWRITE_TEXT_ALIGNMENT { DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_TEXT_ALIGNMENT_CENTER };
enum DWRITE_PARAGRAPH_ALIGNMENT { DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_PARAGRAPH_ALIGNMENT_FAR, DWRITE_PARAGRAPH_ALIGNMENT_CENTER };
using DWRITE_FONT_WEIGHT = int;
constexpr int DWRITE_FONT_WEIGHT_NORMAL = 400, DWRITE_FONT_WEIGHT_SEMI_BOLD = 600, DWRITE_FONT_WEIGHT_BOLD = 700;
enum DWRITE_FONT_STRETCH { DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STRETCH_CONDENSED };
enum D2D1_ANTIALIAS_MODE { D2D1_ANTIALIAS_MODE_ALIASED };
constexpr int D2DERR_RECREATE_TARGET = 1;

// window messages the worker threads post (as SDL user events: SDL_PushEvent is thread-safe)
constexpr UINT WM_APP = 0x8000;
inline Uint32 g_user_event = 0;
inline void PostMessageW(HWND, UINT msg, WPARAM wp, LPARAM lp) {
    SDL_Event e{};
    e.type = g_user_event;
    e.user.code = (Sint32)msg;
    e.user.data1 = (void*)wp;
    e.user.data2 = (void*)lp;
    SDL_PushEvent(&e);
}
inline bool g_need_redraw = true;
inline void InvalidateRect(HWND, const void*, int) { g_need_redraw = true; }
inline void SetCapture(HWND) { SDL_CaptureMouse(true); }
inline void ReleaseCapture() { SDL_CaptureMouse(false); }

// ------------------------------------------------------------------------------------------------ message boxes
constexpr UINT MB_OK = 0, MB_YESNOCANCEL = 3, MB_YESNO = 4, MB_ICONERROR = 0x10, MB_ICONQUESTION = 0x20,
               MB_ICONWARNING = 0x30, MB_ICONINFORMATION = 0x40;
constexpr int IDOK = 1, IDCANCEL = 2, IDYES = 6, IDNO = 7;
inline int MessageBoxW(HWND w, const wchar_t* text, const wchar_t* title, UINT type) {
    const std::string t = draw::utf8(text), c = draw::utf8(title);
    SDL_MessageBoxButtonData yes{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, IDYES, "Yes"}, no{0, IDNO, "No"},
        cancel{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDCANCEL, "Cancel"},
        ok{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDOK, "OK"};
    std::vector<SDL_MessageBoxButtonData> b;
    if ((type & 0xF) == MB_YESNOCANCEL) b = {yes, no, cancel};
    else if ((type & 0xF) == MB_YESNO) no.flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, b = {yes, no};
    else b = {ok};
    const UINT icon = type & 0xF0;
    SDL_MessageBoxData d{icon == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR : icon == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING
                                                                                               : SDL_MESSAGEBOX_INFORMATION,
                         w, c.c_str(), t.c_str(), (int)b.size(), b.data(), nullptr};
    int r = 0;
    if (!SDL_ShowMessageBox(&d, &r)) return b.size() == 1 ? IDOK : IDCANCEL;
    g_need_redraw = true;
    return r ? r : IDCANCEL;
}

// "open" a folder, file or web page with the desktop's default program
void open_with_desktop(const std::string& what);  // launcher.cpp
constexpr int SW_SHOWNORMAL = 1;
inline void ShellExecuteW(HWND, const wchar_t*, const wchar_t* what, const void*, const void*, int) {
    open_with_desktop(draw::utf8(what));
}
inline void ShellExecuteW(HWND, const wchar_t*, const char* what, const void*, const void*, int) {  // fs::path::c_str()
    open_with_desktop(what);
}

// ------------------------------------------------------------------------------------------------ drawing
struct ID2D1SolidColorBrush {
    D2D1_COLOR_F c{1, 1, 1, 1};
    void SetColor(D2D1_COLOR_F v) { c = v; }
};
// a font + paragraph settings (DirectWrite's text format)
struct IDWriteTextFormat {
    std::string family;
    float size = 15;
    int weight = 400;
    bool condensed = false;
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    void SetTextAlignment(DWRITE_TEXT_ALIGNMENT a) { align = a; }
    void SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT p) { para = p; }
};
struct ID2D1Bitmap {
    cairo_surface_t* s = nullptr;
    D2D1_SIZE_F GetSize() const {
        return {(float)cairo_image_surface_get_width(s), (float)cairo_image_surface_get_height(s)};
    }
    void Release() { cairo_surface_destroy(s); delete this; }
};
namespace D2D1 {
struct Matrix3x2F {
    float dx = 0, dy = 0;
    static Matrix3x2F Translation(float x, float y) { return {x, y}; }
    static Matrix3x2F Identity() { return {}; }
};
}  // namespace D2D1

// The render target: a cairo context on the window's pixels (logical units: the launcher's 1180 x 720 layout)
struct ID2D1HwndRenderTarget {
    cairo_t* cr = nullptr;
    void set(const ID2D1SolidColorBrush* b) { cairo_set_source_rgba(cr, b->c.r, b->c.g, b->c.b, b->c.a); }
    void FillRectangle(D2D1_RECT_F r, ID2D1SolidColorBrush* b) {
        set(b);
        cairo_rectangle(cr, r.left, r.top, r.right - r.left, r.bottom - r.top);
        cairo_fill(cr);
    }
    void FillRoundedRectangle(const D2D1_ROUNDED_RECT& rr, ID2D1SolidColorBrush* b) {
        set(b);
        draw::round_rect(cr, rr.rect.left, rr.rect.top, rr.rect.right - rr.rect.left, rr.rect.bottom - rr.rect.top, rr.radiusX);
        cairo_fill(cr);
    }
    void DrawRoundedRectangle(const D2D1_ROUNDED_RECT& rr, ID2D1SolidColorBrush* b, float w) {
        set(b);
        draw::round_rect(cr, rr.rect.left, rr.rect.top, rr.rect.right - rr.rect.left, rr.rect.bottom - rr.rect.top, rr.radiusX);
        cairo_set_line_width(cr, w);
        cairo_stroke(cr);
    }
    void FillEllipse(const D2D1_ELLIPSE& e, ID2D1SolidColorBrush* b) {
        set(b);
        draw::ellipse(cr, e.point.x, e.point.y, e.radiusX, e.radiusY);
        cairo_fill(cr);
    }
    void DrawLine(D2D1_POINT_2F a, D2D1_POINT_2F z, ID2D1SolidColorBrush* b, float w) {
        set(b);
        cairo_set_line_width(cr, w);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_move_to(cr, a.x, a.y);
        cairo_line_to(cr, z.x, z.y);
        cairo_stroke(cr);
    }
    void PushAxisAlignedClip(D2D1_RECT_F r, D2D1_ANTIALIAS_MODE) {
        cairo_save(cr);
        cairo_rectangle(cr, r.left, r.top, r.right - r.left, r.bottom - r.top);
        cairo_clip(cr);
    }
    void PopAxisAlignedClip() { cairo_restore(cr); }
    float tx = 0, ty = 0;  // SetTransform: translations only (scrolling pages)
    void SetTransform(const D2D1::Matrix3x2F& m) {
        cairo_translate(cr, m.dx - tx, m.dy - ty);
        tx = m.dx, ty = m.dy;
    }
};
#endif
