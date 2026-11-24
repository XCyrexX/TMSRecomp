#pragma once
// Linux 2D drawing for the menu (overlay.cpp), voice subtitles (subs.cpp) and button icons (pad_icons.cpp): cairo for
// shapes, pango for text, both standard on Linux desktops. Windows draws the same things with Direct2D / DirectWrite.
// Surfaces are CAIRO_FORMAT_ARGB32: premultiplied B, G, R, A bytes, top-down, like the Direct2D PBGRA targets.
#ifndef _WIN32
#include <cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace draw {

// wchar_t is UTF-32 on Linux
inline std::string utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) {
        uint32_t c = (uint32_t)*w;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3F)), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    }
    return s;
}
inline std::string utf8(const std::wstring& w) { return utf8(w.c_str()); }
inline std::wstring wide(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = (uint8_t)s[i];
        const int n = b < 0x80 ? 1 : b >> 5 == 6 ? 2 : b >> 4 == 14 ? 3 : b >> 3 == 30 ? 4 : 1;
        uint32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
        for (int k = 1; k < n && i + k < s.size(); k++) c = c << 6 | ((uint8_t)s[i + k] & 0x3F);
        w += (wchar_t)c;
        i += n;
    }
    return w;
}

struct Color { double r, g, b, a; };
inline Color rgb(uint32_t c, double a = 1) { return {(c >> 16 & 255) / 255.0, (c >> 8 & 255) / 255.0, (c & 255) / 255.0, a}; }
inline void set(cairo_t* cr, Color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

inline void round_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    r = std::min(r, std::min(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}
inline void ellipse(cairo_t* cr, double cx, double cy, double rx, double ry) {
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
    cairo_restore(cr);
}

enum Align { LEADING, TRAILING, CENTER };
// the DirectWrite families the Windows code asks for, as fontconfig fallbacks
constexpr const char* k_ui_font = "Segoe UI, Noto Sans, Cantarell, DejaVu Sans, Sans";
constexpr const char* k_icon_font = "Arial, Liberation Sans, Arimo, DejaVu Sans, Sans";

// a pango layout for utf-8 text, size in pixels (the em size, as DirectWrite's font size), weight 400 / 600 / 700
inline PangoLayout* layout(cairo_t* cr, const std::string& s, double size, int weight, const char* family = k_ui_font) {
    PangoLayout* l = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_from_string(family);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, s.c_str(), -1);
    return l;
}
// text in the box (x, y, w, h): wrapped at w, aligned, centred vertically (DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
inline void text(cairo_t* cr, const std::string& s, double x, double y, double w, double h, double size, Color c,
                 int weight = 600, Align align = LEADING, const char* family = k_ui_font) {
    if (s.empty() || w <= 0) return;
    PangoLayout* l = layout(cr, s, size, weight, family);
    pango_layout_set_width(l, (int)(w * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(l, align == CENTER ? PANGO_ALIGN_CENTER : align == TRAILING ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    int lw, lh;
    pango_layout_get_pixel_size(l, &lw, &lh);
    set(cr, c);
    cairo_move_to(cr, x, y + (h - lh) / 2);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

// A drawing surface of w x h; data() is premultiplied BGRA, w * 4 bytes a row (cairo_image_surface_get_stride may pad:
// stride() says it)
struct Canvas {
    cairo_surface_t* surf = nullptr;
    cairo_t* cr = nullptr;
    int w = 0, h = 0;
    Canvas(int w_, int h_) : w(w_), h(h_) {
        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cr = cairo_create(surf);
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_GOOD);
    }
    ~Canvas() { cairo_destroy(cr); cairo_surface_destroy(surf); }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    bool ok() const { return cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS; }
    void clear() {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    const uint8_t* data() { cairo_surface_flush(surf); return cairo_image_surface_get_data(surf); }
    int stride() const { return cairo_image_surface_get_stride(surf); }
};

}  // namespace draw
#endif
