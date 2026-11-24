// Button icons by controller. Switch button faces are baked into a few UI atlases (RGBA8, straight alpha): the
// common button atlas (Interface/_US/common/icon/button_icon.gtx, black-ringed discs) and the prompt sprites of the
// HUD/menu atlases (plain white discs with grey letters: "(+) Stats", "(B) Close"...). With a PlayStation or Xbox pad
// in use (setting pad_icons: auto, switch, playstation, xbox) the face buttons, shoulder buttons and +/- are drawn
// again here with Direct2D over a copy of the atlas before it is uploaded (vk_texture.cpp). No art files: shapes and
// text only. Atlases are found by the hash of their level 0 (vk_texture.cpp tex_hash); cells were measured on the
// dumps (tools/padicons: find_discs.py, sheet.py, crop.py).
// Faces follow ui.cpp's positional mapping: Switch A = east (PS circle, Xbox B), B = south (cross, A), X = north
// (triangle, Y), Y = west (square, X).
#include "plat.h"
#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#else
#include "draw_linux.h"
#include <cmath>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "rt.h"

namespace rt {

std::atomic<int> g_pad_kind{0};         // ui.cpp: the open pad (0 Switch / other, 1 PlayStation, 2 Xbox)
static std::atomic<int> g_pad_icons{-1};  // setting: -1 not read, 0 auto, 1 switch, 2 playstation, 3 xbox
static const char* const k_icon_names[] = {"auto", "switch", "playstation", "xbox"};

int pad_icons_setting() {
    if (g_pad_icons < 0) {
        const char* v = getenv("NX_PAD_ICONS");
        int s = 0;
        for (int i = 0; i < 4; i++) if (v && !_stricmp(v, k_icon_names[i])) s = i;
        g_pad_icons = s;
    }
    return g_pad_icons;
}
void set_pad_icons(int s) {
    save_setting("pad_icons", k_icon_names[s]);
    g_pad_icons = s;
}
int pad_icon_style() {
    int s = pad_icons_setting();
    return s == 0 ? g_pad_kind.load() : s - 1;
}

namespace {
enum Kind { FACE_A, FACE_B, FACE_X, FACE_Y, PLUS, MINUS, SH_L, SH_R, SH_ZL, SH_ZR };
struct Cell { int x, y, w, h; Kind k; int look = 0; uint32_t fill = 0; };  // look: 0 the atlas's, 1 plain, 2 ringed, 3 bubble, 4 cut-out (white disc, glyph transparent); fill: disc colour (0 white; glyphs then black)
// common button atlas, hash 34c763427410e392 (256x256)
const Cell k_button_cells[] = {
    {219, 2, 26, 26, FACE_A}, {2, 33, 26, 26, FACE_B}, {157, 2, 26, 26, FACE_X}, {188, 2, 26, 26, FACE_Y},
    {126, 2, 26, 26, PLUS}, {95, 2, 26, 26, MINUS}, {157, 35, 26, 23, SH_L}, {2, 66, 26, 23, SH_R},
    {64, 64, 26, 26, SH_ZL}, {95, 64, 26, 26, SH_ZR}, {127, 64, 34, 25, SH_L}, {177, 64, 34, 25, SH_R},
    {176, 129, 22, 22, FACE_A}, {1, 154, 22, 22, FACE_B}, {126, 129, 22, 22, FACE_X}, {151, 129, 22, 22, FACE_Y},
    {101, 129, 22, 22, PLUS}, {76, 129, 22, 22, MINUS}, {126, 155, 22, 20, SH_L}, {1, 180, 22, 20, SH_R},
    {51, 179, 22, 22, SH_ZL}, {76, 179, 22, 22, SH_ZR}, {102, 180, 28, 21, SH_L}, {142, 180, 28, 21, SH_R},
};
// HUD/menu atlas, 2048 x 2048 (hash ea576a96a510fd6e): prompt sprites, and the large "(A)"
const Cell k_hud2048_cells[] = {
    {6, 1236, 32, 31, FACE_A}, {46, 1236, 32, 31, FACE_B}, {86, 1236, 32, 32, FACE_X}, {126, 1236, 32, 32, FACE_Y},
    {166, 1236, 32, 31, PLUS}, {206, 1236, 32, 32, MINUS}, {6, 1284, 36, 32, SH_L}, {50, 1284, 36, 32, SH_R},
    {1696, 188, 40, 39, FACE_A},
    {1890, 226, 32, 32, FACE_A, 4},  // "SELECT (A)" bubble (level-up skill inheritance; the game draws it dark over a green disc)
    {1254, 1866, 32, 32, FACE_A, 1},  // black "SELECT (A)" bubble (Yes/No dialogs, e.g. camp Load "Proceed?"; tinted yellow)
};
// the same atlas at 1024 x 1024 (hash 74557bfcf56aafe0)
const Cell k_hud1024_cells[] = {
    {3, 618, 16, 16, FACE_A}, {23, 618, 16, 16, FACE_B}, {43, 618, 16, 16, FACE_X}, {63, 618, 16, 16, FACE_Y},
    {83, 618, 16, 16, PLUS}, {103, 618, 16, 16, MINUS}, {3, 642, 18, 16, SH_L}, {25, 642, 18, 16, SH_R},
    {848, 94, 20, 20, FACE_A}, {945, 113, 16, 16, FACE_A, 4}, {627, 933, 16, 16, FACE_A, 1},
};
// menu atlas 1024 x 1024 (hash b18094e4c410f0aa): ringed + - A B X Y, and a large X / Y on a white panel
const Cell k_menu2_cells[] = {
    {838, 10, 36, 35, PLUS}, {891, 10, 35, 35, MINUS}, {518, 66, 36, 35, FACE_A}, {570, 66, 36, 35, FACE_B},
    {622, 66, 36, 35, FACE_X}, {674, 66, 36, 35, FACE_Y}, {37, 137, 50, 50, FACE_X}, {37, 249, 50, 50, FACE_Y},
};
// 2048 x 1024 (hash 16f810b5373bff80): ringed Y, and a large X with a thick ring
const Cell k_wide_cells[] = {{1924, 620, 36, 35, FACE_Y}, {1971, 615, 74, 73, FACE_X}, {1272, 902, 36, 35, FACE_X, 2, 0xFED128},
                             {1272, 970, 36, 35, FACE_X, 2, 0xFED128}};  // yellow: Stats "Other Info (X)"
// 512 x 512 (hash 171a1c16c51d09c9): ringed X / Y on a white panel
const Cell k_small_cells[] = {{18, 68, 26, 26, FACE_X}, {18, 124, 26, 26, FACE_Y}};
// speech bubbles (grey disc, white glyph inside the bubble's white; the game tints them green):
// 1024 x 1024 (hash a4c81da53e4659cc) camp menu "SELECT (A)"; 2048 x 2048 (hash df5661e8b0521c20) Artists "check it out! (A)"
const Cell k_select_cells[] = {{858, 544, 48, 48, FACE_A, 3, 0xA6A6A6}};
const Cell k_artists_cells[] = {{980, 1978, 32, 32, FACE_A, 3, 0xA6A6A6}};
// TOPIC / request menu (topic_window.gtx; the English mod's _US copy at 1024, hash 94e020b4e4e95ccd): ringed - and Y,
// black "Close (B)" and "SELECT (A)" discs with a small white button disc inside, (X) in the yellow "Details" and
// "Timeline" pills
const Cell k_topic_cells[] = {{626, 60, 36, 35, MINUS}, {626, 112, 36, 35, FACE_Y}, {52, 244, 36, 36, FACE_B, 1},
                              {200, 562, 32, 32, FACE_A, 1}, {840, 80, 36, 34, FACE_X, 2, 0xFBD130},
                              {838, 140, 36, 36, FACE_X, 2, 0xFBD130}};
// battle atlas 4096 x 4096 (battle_02/battle_003.gtx, hash 37357058a189ec59): "Quick Session (Y) OFF / ON" pills
// (white disc, black Y; user report 10-30). The game's own half-size atlas (no English mod) is not in the table yet.
const Cell k_battle4096_cells[] = {{462, 580, 32, 32, FACE_Y, 1}, {462, 672, 32, 32, FACE_Y, 1}};
// div 2: the same layout at half size. The English (uncensor) mod's _US Interface atlases are 2x the game's own:
// without the mod (or for other languages) the game loads the half-size originals, with other hashes.
struct Atlas { uint64_t hash; int w, h; bool plain; const Cell* cells; size_t n; int div; };
#define ATLAS(h, w, ht, plain, c) {h, w, ht, plain, c, sizeof(c) / sizeof(c[0]), 1}
#define ATLAS_HALF(h, w, ht, plain, c) {h, w, ht, plain, c, sizeof(c) / sizeof(c[0]), 2}
const Atlas k_atlases[] = {
    ATLAS(k_button_atlas_hash, 256, 256, false, k_button_cells),
    ATLAS(0xea576a96a510fd6eull, 2048, 2048, true, k_hud2048_cells),
    ATLAS(0x74557bfcf56aafe0ull, 1024, 1024, true, k_hud1024_cells),
    ATLAS(0x94e020b4e4e95ccdull, 1024, 1024, false, k_topic_cells),
    ATLAS(0x9fd0da0c8aec7663ull, 1024, 1024, true, k_hud1024_cells),  // the 1024 HUD atlas as found in the packs
    ATLAS(0xb18094e4c410f0aaull, 1024, 1024, false, k_menu2_cells),
    ATLAS(0x16f810b5373bff80ull, 2048, 1024, false, k_wide_cells),
    ATLAS(0x171a1c16c51d09c9ull, 512, 512, false, k_small_cells),
    ATLAS(0xa4c81da53e4659ccull, 1024, 1024, false, k_select_cells),
    ATLAS(0xdf5661e8b0521c20ull, 2048, 2048, false, k_artists_cells),
    ATLAS(0x37357058a189ec59ull, 4096, 4096, false, k_battle4096_cells),
    ATLAS_HALF(0x9a884b596879859bull, 2048, 2048, false, k_battle4096_cells),  // the game's own _US battle_003 (no mod)
    // the game's own (no English mod): _US + other languages at half size
    ATLAS_HALF(0x25e96f94da67ffbcull, 512, 512, false, k_topic_cells),  // _US topic_window
    ATLAS_HALF(0x2ebbd8bbb08ecbafull, 512, 512, false, k_topic_cells),  // _JP
    ATLAS_HALF(0x31bb1f049c915ab7ull, 512, 512, false, k_topic_cells),  // _FR
    ATLAS_HALF(0x71afab7fc1a1de23ull, 512, 512, false, k_topic_cells),  // _CN
    ATLAS_HALF(0xdc05f6d45e1b13d9ull, 512, 512, false, k_topic_cells),  // _TW
    ATLAS_HALF(0x90b79255bd022d33ull, 512, 512, false, k_menu2_cells),  // _US drc_tv
    ATLAS_HALF(0x57febd3b87911115ull, 1024, 512, false, k_wide_cells),  // _US status_drc001
    ATLAS_HALF(0xea81556a68ac884bull, 512, 512, false, k_select_cells),  // _US saveload001
    ATLAS_HALF(0x43e7b49de855cfd3ull, 1024, 1024, false, k_artists_cells),  // _US camp002
};

#ifdef _WIN32
template <class T> void release(T*& p) { if (p) p->Release(), p = nullptr; }

struct Painter {
    ID2D1Factory* f = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1StrokeStyle* round = nullptr;
    bool plain = false;  // prompt look: white disc without the black ring, grey glyphs
    bool bubble = false;  // look 3: grey disc, white glyph (the game tints both)
    bool cut = false;  // look 4: white disc, black glyph that the copy back turns into transparency
    uint32_t fill = 0xFFFFFF;  // disc colour; not white: every glyph in black
    uint32_t tint(uint32_t rgb) const { return cut ? 0x000000 : bubble ? 0xFFFFFF : fill == 0xFFFFFF ? rgb : 0x000000; }
    uint32_t ink() const { return cut ? 0x000000 : bubble ? 0xFFFFFF : plain ? 0x5A5A5A : 0x000000; }
    ~Painter() { release(round), release(rt), release(dw), release(f); }

    ID2D1SolidColorBrush* brush(uint32_t rgb) {
        ID2D1SolidColorBrush* b = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(rgb), &b);
        return b;
    }
    void text(const wchar_t* s, D2D1_RECT_F r, float size, uint32_t rgb) {
        IDWriteTextFormat* tf = nullptr;
        if (FAILED(dw->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &tf)))
            return;
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* b = brush(rgb);
        rt->DrawText(s, (UINT32)wcslen(s), tf, r, b);
        release(b), release(tf);
    }
    void line(float x0, float y0, float x1, float y1, float w, uint32_t rgb) {
        ID2D1SolidColorBrush* b = brush(rgb);
        rt->DrawLine({x0, y0}, {x1, y1}, b, w, round);
        release(b);
    }
    // white disc with the game's black ring (2 px at 26 px)
    void disc(float cx, float cy, float r) {
        ID2D1SolidColorBrush *w = brush(fill), *k = brush(0x000000);
        float sw = r * 0.16f;
        D2D1_ELLIPSE e{{cx, cy}, r - sw / 2, r - sw / 2};
        if (plain) e = {{cx, cy}, r, r};
        rt->FillEllipse(e, w);
        if (!plain) rt->DrawEllipse(e, k, sw);
        release(w), release(k);
    }
    void face(const Cell& c, int style) {
        float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f, r = c.w / 2.0f;
        disc(cx, cy, r);
        if (style == 2) {  // Xbox letters in their colours
            static const wchar_t* const letter[] = {L"B", L"A", L"Y", L"X"};
            static const uint32_t col[] = {0xD8262B, 0x3E9B2F, 0xE0A800, 0x2B6CD6};
            text(letter[c.k], D2D1::RectF(c.x, c.y + r * 0.04f, c.x + c.w, c.y + c.h), r * 1.25f, tint(col[c.k]));
            return;
        }
        float sw = r * 0.2f, g = r * 0.42f;  // PlayStation symbols, stroked
        ID2D1SolidColorBrush* b = nullptr;
        switch (c.k) {
            case FACE_A:  // circle
                b = brush(tint(0xE0464B));
                rt->DrawEllipse({{cx, cy}, g, g}, b, sw);
                break;
            case FACE_B:  // cross
                line(cx - g, cy - g, cx + g, cy + g, sw, tint(0x4A7FD8));
                line(cx - g, cy + g, cx + g, cy - g, sw, tint(0x4A7FD8));
                break;
            case FACE_X: {  // triangle
                b = brush(tint(0x2FA67E));
                ID2D1PathGeometry* p = nullptr;
                ID2D1GeometrySink* s = nullptr;
                f->CreatePathGeometry(&p);
                p->Open(&s);
                float t = g * 1.15f, oy = cy + g * 0.15f;
                s->BeginFigure({cx, oy - t}, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine({cx + t * 0.866f, oy + t * 0.5f});
                s->AddLine({cx - t * 0.866f, oy + t * 0.5f});
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                rt->DrawGeometry(p, b, sw, round);
                release(s), release(p);
                break;
            }
            default:  // square
                b = brush(tint(0xD266AE));
                rt->DrawRectangle(D2D1::RectF(cx - g * 0.85f, cy - g * 0.85f, cx + g * 0.85f, cy + g * 0.85f), b, sw);
                break;
        }
        release(b);
    }
    void plus_minus(const Cell& c, int style) {
        float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f, r = c.w / 2.0f, sw = r * 0.16f;
        disc(cx, cy, r);
        if (c.k == PLUS) {  // Options / Menu: three lines
            for (int i = -1; i <= 1; i++) line(cx - r * 0.38f, cy + i * r * 0.3f, cx + r * 0.38f, cy + i * r * 0.3f, sw, ink());
        } else if (style == 2) {  // View: two overlapping squares
            ID2D1SolidColorBrush *k = brush(ink()), *w = brush(0xFFFFFF);
            float s = r * 0.42f, o = r * 0.16f;
            D2D1_RECT_F back = D2D1::RectF(cx - s + o, cy - s - o + o * 0.2f, cx + s * 0.6f + o, cy + s * 0.6f - o);
            D2D1_RECT_F front = D2D1::RectF(cx - s * 0.6f - o, cy - s * 0.6f + o, cx + s - o, cy + s + o - o * 0.2f);
            rt->DrawRectangle(back, k, sw * 0.8f);
            rt->FillRectangle(front, w);
            rt->DrawRectangle(front, k, sw * 0.8f);
            release(k), release(w);
        } else {  // Share / Create: arrow up out of a tray
            line(cx, cy + r * 0.1f, cx, cy - r * 0.45f, sw, ink());
            line(cx - r * 0.22f, cy - r * 0.23f, cx, cy - r * 0.45f, sw, ink());
            line(cx + r * 0.22f, cy - r * 0.23f, cx, cy - r * 0.45f, sw, ink());
            line(cx - r * 0.4f, cy + r * 0.05f, cx - r * 0.4f, cy + r * 0.4f, sw, ink());
            line(cx - r * 0.4f, cy + r * 0.4f, cx + r * 0.4f, cy + r * 0.4f, sw, ink());
            line(cx + r * 0.4f, cy + r * 0.4f, cx + r * 0.4f, cy + r * 0.05f, sw, ink());
        }
    }
    void shoulder(const Cell& c, int style) {
        static const wchar_t* const ps[] = {L"L1", L"R1", L"L2", L"R2"};
        static const wchar_t* const xb[] = {L"LB", L"RB", L"LT", L"RT"};
        float sw = c.h * 0.085f;
        D2D1_ROUNDED_RECT rr{D2D1::RectF(c.x + sw / 2, c.y + sw / 2, c.x + c.w - sw / 2, c.y + c.h - sw / 2), c.h * 0.28f, c.h * 0.28f};
        if (plain) rr.rect = D2D1::RectF(c.x, c.y, c.x + c.w, c.y + c.h);
        ID2D1SolidColorBrush *w = brush(0xFFFFFF), *k = brush(0);
        rt->FillRoundedRectangle(rr, w);
        if (!plain) rt->DrawRoundedRectangle(rr, k, sw);
        release(w), release(k);
        int i = c.k - SH_L;
        text(style == 2 ? xb[i] : ps[i], D2D1::RectF(c.x, c.y + c.h * 0.03f, c.x + c.w, c.y + c.h), c.h * 0.58f, ink());
    }
};
#else
// Linux: the same shapes with cairo / pango (draw_linux.h)
struct RectF { float left, top, right, bottom; };
struct Painter {
    cairo_t* cr = nullptr;
    bool plain = false, bubble = false, cut = false;
    uint32_t fill = 0xFFFFFF;
    uint32_t tint(uint32_t rgb) const { return cut ? 0x000000 : bubble ? 0xFFFFFF : fill == 0xFFFFFF ? rgb : 0x000000; }
    uint32_t ink() const { return cut ? 0x000000 : bubble ? 0xFFFFFF : plain ? 0x5A5A5A : 0x000000; }

    void text(const wchar_t* s, RectF r, float size, uint32_t rgb) {
        draw::text(cr, draw::utf8(s), r.left, r.top, r.right - r.left, r.bottom - r.top, size, draw::rgb(rgb), 700,
                   draw::CENTER, draw::k_icon_font);
    }
    void stroke(float w, uint32_t rgb) {
        draw::set(cr, draw::rgb(rgb));
        cairo_set_line_width(cr, w);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }
    void line(float x0, float y0, float x1, float y1, float w, uint32_t rgb) {
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
        stroke(w, rgb);
    }
    void disc(float cx, float cy, float r) {
        float sw = r * 0.16f, er = plain ? r : r - sw / 2;
        draw::ellipse(cr, cx, cy, er, er);
        draw::set(cr, draw::rgb(fill));
        if (plain) { cairo_fill(cr); return; }
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, sw);
        draw::set(cr, draw::rgb(0));
        cairo_stroke(cr);
    }
    void rect(float x0, float y0, float x1, float y1) { cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0); }
    void face(const Cell& c, int style) {
        float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f, r = c.w / 2.0f;
        disc(cx, cy, r);
        if (style == 2) {  // Xbox letters in their colours
            static const wchar_t* const letter[] = {L"B", L"A", L"Y", L"X"};
            static const uint32_t col[] = {0xD8262B, 0x3E9B2F, 0xE0A800, 0x2B6CD6};
            text(letter[c.k], RectF{(float)c.x, c.y + r * 0.04f, (float)(c.x + c.w), (float)(c.y + c.h)}, r * 1.25f, tint(col[c.k]));
            return;
        }
        float sw = r * 0.2f, g = r * 0.42f;  // PlayStation symbols, stroked
        switch (c.k) {
            case FACE_A:  // circle
                draw::ellipse(cr, cx, cy, g, g);
                stroke(sw, tint(0xE0464B));
                break;
            case FACE_B:  // cross
                line(cx - g, cy - g, cx + g, cy + g, sw, tint(0x4A7FD8));
                line(cx - g, cy + g, cx + g, cy - g, sw, tint(0x4A7FD8));
                break;
            case FACE_X: {  // triangle
                float t = g * 1.15f, oy = cy + g * 0.15f;
                cairo_move_to(cr, cx, oy - t);
                cairo_line_to(cr, cx + t * 0.866f, oy + t * 0.5f);
                cairo_line_to(cr, cx - t * 0.866f, oy + t * 0.5f);
                cairo_close_path(cr);
                stroke(sw, tint(0x2FA67E));
                break;
            }
            default:  // square
                rect(cx - g * 0.85f, cy - g * 0.85f, cx + g * 0.85f, cy + g * 0.85f);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
                draw::set(cr, draw::rgb(tint(0xD266AE)));
                cairo_set_line_width(cr, sw);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
                cairo_stroke(cr);
                break;
        }
    }
    void plus_minus(const Cell& c, int style) {
        float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f, r = c.w / 2.0f, sw = r * 0.16f;
        disc(cx, cy, r);
        if (c.k == PLUS) {  // Options / Menu: three lines
            for (int i = -1; i <= 1; i++) line(cx - r * 0.38f, cy + i * r * 0.3f, cx + r * 0.38f, cy + i * r * 0.3f, sw, ink());
        } else if (style == 2) {  // View: two overlapping squares
            float s = r * 0.42f, o = r * 0.16f;
            auto box = [&](float x0, float y0, float x1, float y1, bool filled) {
                if (filled) { rect(x0, y0, x1, y1); draw::set(cr, draw::rgb(0xFFFFFF)); cairo_fill(cr); }
                rect(x0, y0, x1, y1);
                draw::set(cr, draw::rgb(ink()));
                cairo_set_line_width(cr, sw * 0.8f);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
                cairo_stroke(cr);
            };
            box(cx - s + o, cy - s - o + o * 0.2f, cx + s * 0.6f + o, cy + s * 0.6f - o, false);
            box(cx - s * 0.6f - o, cy - s * 0.6f + o, cx + s - o, cy + s + o - o * 0.2f, true);
        } else {  // Share / Create: arrow up out of a tray
            line(cx, cy + r * 0.1f, cx, cy - r * 0.45f, sw, ink());
            line(cx - r * 0.22f, cy - r * 0.23f, cx, cy - r * 0.45f, sw, ink());
            line(cx + r * 0.22f, cy - r * 0.23f, cx, cy - r * 0.45f, sw, ink());
            line(cx - r * 0.4f, cy + r * 0.05f, cx - r * 0.4f, cy + r * 0.4f, sw, ink());
            line(cx - r * 0.4f, cy + r * 0.4f, cx + r * 0.4f, cy + r * 0.4f, sw, ink());
            line(cx + r * 0.4f, cy + r * 0.4f, cx + r * 0.4f, cy + r * 0.05f, sw, ink());
        }
    }
    void shoulder(const Cell& c, int style) {
        static const wchar_t* const ps[] = {L"L1", L"R1", L"L2", L"R2"};
        static const wchar_t* const xb[] = {L"LB", L"RB", L"LT", L"RT"};
        float sw = c.h * 0.085f;
        RectF rr{c.x + sw / 2, c.y + sw / 2, c.x + c.w - sw / 2, c.y + c.h - sw / 2};
        if (plain) rr = RectF{(float)c.x, (float)c.y, (float)(c.x + c.w), (float)(c.y + c.h)};
        draw::round_rect(cr, rr.left, rr.top, rr.right - rr.left, rr.bottom - rr.top, c.h * 0.28f);
        draw::set(cr, draw::rgb(0xFFFFFF));
        if (plain) cairo_fill(cr);
        else {
            cairo_fill_preserve(cr);
            draw::set(cr, draw::rgb(0));
            cairo_set_line_width(cr, sw);
            cairo_stroke(cr);
        }
        int i = c.k - SH_L;
        text(style == 2 ? xb[i] : ps[i], RectF{(float)c.x, c.y + c.h * 0.03f, (float)(c.x + c.w), (float)(c.y + c.h)}, c.h * 0.58f, ink());
    }
};
#endif
}  // namespace

bool is_pad_icon_atlas(uint64_t hash) {
    for (const Atlas& a : k_atlases) if (a.hash == hash) return true;
    return false;
}
bool pad_icon_atlas_size(uint32_t w, uint32_t h) {
    for (const Atlas& a : k_atlases) if ((uint32_t)a.w == w && (uint32_t)a.h == h) return true;
    return false;
}

// Draws the cells' buttons over rgba (RGBA8, straight alpha, n x nh) in place. style 1 PlayStation, 2 Xbox.
#ifdef _WIN32
static bool paint_cells(int style, const std::vector<Cell>& cells, bool atlas_plain, int n, int nh, uint8_t* rgba) {
    Painter p;
    p.plain = atlas_plain;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &p.f)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&p.dw)))
        return false;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(p.f->CreateDCRenderTarget(&rp, &p.rt))) return false;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                  D2D1_LINE_JOIN_ROUND);
    p.f->CreateStrokeStyle(sp, nullptr, 0, &p.round);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), n, -nh, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return false; }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r{0, 0, n, nh};
    p.rt->BindDC(dc, &r);
    p.rt->BeginDraw();
    p.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (const Cell& c : cells) {
        p.bubble = c.look == 3, p.cut = c.look == 4;
        p.plain = c.look ? c.look == 1 || c.look >= 3 : atlas_plain;
        p.fill = c.fill ? c.fill : 0xFFFFFF;
        if (c.k <= FACE_Y) p.face(c, style);
        else if (c.k <= MINUS) p.plus_minus(c, style);
        else p.shoulder(c, style);
    }
    bool ok = SUCCEEDED(p.rt->EndDraw());
    if (ok) {
        const uint8_t* src = (const uint8_t*)bits;  // BGRA premultiplied, top-down
        for (const Cell& c : cells)
            for (int y = std::max(0, c.y - 1); y < std::min(nh, c.y + c.h + 1); y++)
                for (int x = std::max(0, c.x - 1); x < std::min(n, c.x + c.w + 1); x++) {
                    const uint8_t* s = src + (y * n + x) * 4;
                    uint8_t* d = rgba + (y * n + x) * 4;
                    if (c.look == 4) {  // replace: white where the disc is, alpha falls with the glyph's darkness
                        d[0] = d[1] = d[2] = 255;
                        d[3] = (uint8_t)std::min(255, (s[0] + s[1] + s[2]) / 3);  // premultiplied grey = alpha x white
                        continue;
                    }
                    // over the original: our disc covers its glyph, panels and shadows around it stay
                    float sa = s[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
                    if (oa <= 0) continue;
                    for (int k = 0; k < 3; k++)
                        d[k] = (uint8_t)std::min(255.0f, (s[2 - k] + d[k] * da * (1 - sa)) / oa + 0.5f);
                    d[3] = (uint8_t)(oa * 255 + 0.5f);
                }
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}
#else
static bool paint_cells(int style, const std::vector<Cell>& cells, bool atlas_plain, int n, int nh, uint8_t* rgba) {
    draw::Canvas cv(n, nh);
    if (!cv.ok()) return false;
    cv.clear();
    Painter p;
    p.cr = cv.cr;
    p.plain = atlas_plain;
    for (const Cell& c : cells) {
        p.bubble = c.look == 3, p.cut = c.look == 4;
        p.plain = c.look ? c.look == 1 || c.look >= 3 : atlas_plain;
        p.fill = c.fill ? c.fill : 0xFFFFFF;
        if (c.k <= FACE_Y) p.face(c, style);
        else if (c.k <= MINUS) p.plus_minus(c, style);
        else p.shoulder(c, style);
    }
    const uint8_t* base = cv.data();  // BGRA premultiplied, top-down
    const int stride = cv.stride();
    for (const Cell& c : cells)
        for (int y = std::max(0, c.y - 1); y < std::min(nh, c.y + c.h + 1); y++)
            for (int x = std::max(0, c.x - 1); x < std::min(n, c.x + c.w + 1); x++) {
                const uint8_t* s = base + (size_t)y * stride + x * 4;
                uint8_t* d = rgba + (y * n + x) * 4;
                if (c.look == 4) {
                    d[0] = d[1] = d[2] = 255;
                    d[3] = (uint8_t)std::min(255, (s[0] + s[1] + s[2]) / 3);
                    continue;
                }
                float sa = s[3] / 255.0f, da = d[3] / 255.0f, oa = sa + da * (1 - sa);
                if (oa <= 0) continue;
                for (int k = 0; k < 3; k++)
                    d[k] = (uint8_t)std::min(255.0f, (s[2 - k] + d[k] * da * (1 - sa)) / oa + 0.5f);
                d[3] = (uint8_t)(oa * 255 + 0.5f);
            }
    return true;
}
#endif

// rgba: an atlas of k_atlases (RGBA8, straight alpha, w x h), changed in place. style 1 PlayStation, 2 Xbox.
bool pad_icon_atlas(int style, uint64_t hash, uint32_t w, uint32_t h, uint8_t* rgba) {
    const Atlas* at = nullptr;
    for (const Atlas& a : k_atlases) if (a.hash == hash && (uint32_t)a.w == w && (uint32_t)a.h == h) at = &a;
    if (!at) return false;
    // Only the cells' bounding box is painted (a full 4096 x 4096 software target + its copies cost ~86 ms per load of
    // the battle atlas), and the result is cached per atlas + style: the same hash = the same original pixels.
    std::vector<Cell> cells(at->cells, at->cells + at->n);
    int x0 = at->w, y0 = at->h, x1 = 0, y1 = 0;
    for (Cell& c : cells) {
        c.x /= at->div, c.y /= at->div, c.w /= at->div, c.h /= at->div;
        x0 = std::min(x0, c.x - 1), y0 = std::min(y0, c.y - 1), x1 = std::max(x1, c.x + c.w + 1), y1 = std::max(y1, c.y + c.h + 1);
    }
    x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(at->w, x1), y1 = std::min(at->h, y1);
    if (x1 <= x0 || y1 <= y0) return false;
    const int bw = x1 - x0, bh = y1 - y0;
    const size_t row = (size_t)bw * 4, stride = (size_t)at->w * 4;
    auto put = [&](const uint8_t* sub) {
        for (int y = 0; y < bh; y++) memcpy(rgba + (y0 + y) * stride + x0 * 4, sub + y * row, row);
    };
    static std::mutex m;
    static std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
    const uint64_t key = hash ^ (uint64_t)style << 62;
    std::lock_guard<std::mutex> lk(m);
    if (auto it = cache.find(key); it != cache.end()) return put(it->second.data()), true;
    std::vector<uint8_t> sub(row * bh);
    for (int y = 0; y < bh; y++) memcpy(sub.data() + y * row, rgba + (y0 + y) * stride + x0 * 4, row);
    for (Cell& c : cells) c.x -= x0, c.y -= y0;
    if (!paint_cells(style, cells, at->plain, bw, bh, sub.data())) return false;
    put(sub.data());
    cache[key] = std::move(sub);
    return true;
}

// Field interaction prompts ("(A) Talk", "(A) Exit", "(A) Check"... in 5 languages): whole pills pre-rendered in the
// effects Effect/map_0086..0095.ptcl, 512 x 128 BC3 (vk_texture.cpp decodes them). The green (A) disc sits at a
// different x per text, so it is found here: the leftmost round blob of opaque green (the pill's green border is wide).
static const uint64_t k_prompt_hashes[] = {
    0x0a1121d1123819dcull, 0x0d958b8b7e1852f5ull, 0x1388f62b0404aca6ull, 0x17cad5c99d823874ull, 0x1b23f7e7e2810cadull,
    0x1e4fb959bbc86e2dull, 0x1ef1081941e223d9ull, 0x232a2715e2737e93ull, 0x2d829d75a162f5c5ull, 0x2e019500c97f345eull,
    0x31b87b13d644650bull, 0x378d6a5e12791370ull, 0x3904e9419c035457ull, 0x3953990c54b24bf4ull, 0x3a3043403fefd112ull,
    0x443f62a2c9967a86ull, 0x49fe07ea64ab2751ull, 0x4da182f9c7e5a03full, 0x5048c18c8da31e5dull, 0x550a3aa178794266ull,
    0x55f721afdb19ec0full, 0x5761dab8dcd4fcf4ull, 0x665a0ce7165c86baull, 0x73571ed2b4f20705ull, 0x7bdcee88ce3ea21eull,
    0x820852f5d9647dc4ull, 0x829bf5dfba334167ull, 0x8aafac20d2f018d0ull, 0x8c5330694cae3b43ull, 0xa18fbfb6fe45a949ull,
    0xa5e3338d7887f62dull, 0xacda13f1dcbaa9c1ull, 0xae9ea682858c618aull, 0xaf22f738c6983ef7ull, 0xb5a3880cc5a5e69dull,
    0xbb200a4fd25dfe12ull, 0xbd585b68a49976c2ull, 0xbe2b9f8f51f27490ull, 0xbf9431f4c3de73e1ull, 0xc2e29b32cb58bac8ull,
    0xc8042cb35a6097b2ull, 0xccbf9b1d6119733eull, 0xcff32a454dd4b2d6ull, 0xdb722b95180a5ae9ull, 0xe0467adce73f48bbull,
    0xe2b912af1668225eull, 0xe66a7b3cb27bdb92ull, 0xe885537fa3e477a5ull, 0xee1ae98ed77c8a58ull, 0xf6ebe594efecb62eull,
};
bool is_pad_prompt(uint64_t hash) {
    for (uint64_t h : k_prompt_hashes) if (h == hash) return true;
    return false;
}
bool pad_icon_prompt(int style, uint32_t w, uint32_t h, uint8_t* rgba) {
    auto green = [&](uint32_t i) {
        const uint8_t* q = rgba + i * 4;
        return q[3] > 200 && q[1] > 140 && q[1] > q[0] + 40 && q[1] > q[2] + 80;
    };
    std::vector<uint8_t> seen(w * h);
    std::vector<uint32_t> stack;
    int best_x = 1 << 30;
    Cell best{};
    for (uint32_t i0 = 0; i0 < w * h; i0++) {
        if (seen[i0] || !green(i0)) continue;
        uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0, n = 0;
        uint64_t sum[3] = {};
        stack.assign(1, i0), seen[i0] = 1;
        while (!stack.empty()) {
            const uint32_t i = stack.back(), x = i % w, y = i / w;
            stack.pop_back(), n++;
            for (int k = 0; k < 3; k++) sum[k] += rgba[i * 4 + k];
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            const uint32_t nb[4] = {x > 0 ? i - 1 : i, x + 1 < w ? i + 1 : i, y > 0 ? i - w : i, y + 1 < h ? i + w : i};
            for (uint32_t j : nb) if (!seen[j] && green(j)) seen[j] = 1, stack.push_back(j);
        }
        const int bw = (int)(x1 - x0 + 1), bh = (int)(y1 - y0 + 1);
        const float fill = (float)n / (bw * bh);  // a disc with the letter cut out: ~0.5..0.8
        if (bw < 14 || bw > 90 || abs(bw - bh) > std::max(2, bw / 8) || fill < 0.4f || fill > 0.85f || (int)x0 >= best_x) continue;
        const float r = bw / 2.0f * 1.16f;  // out to the middle of the disc's black ring
        const float cx = x0 + bw / 2.0f, cy = y0 + bh / 2.0f;
        best = {(int)(cx - r + 0.5f), (int)(cy - r + 0.5f), (int)(2 * r + 0.5f), (int)(2 * r + 0.5f), FACE_A, 2,
                (uint32_t)(sum[0] / n << 16 | sum[1] / n << 8 | sum[2] / n)};
        best_x = (int)x0;
    }
    if (best_x == 1 << 30) return false;
    return paint_cells(style, {best}, false, (int)w, (int)h, rgba);
}

// Cell table check without playing: every k_atlases texture found in <dir> as a dump (<w>x<h>_<fmt>_<hash>.dds,
// RGBA8, from NX_TEX_DUMP or tools/texextract) gets both styles drawn into <dir>/padicons_<style>_<hash>.dds.
int pad_icons_selftest(const char* dir) {
    int done = 0;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        size_t u = n.rfind('_');
        if (n.rfind("padicons", 0) == 0 || u == std::string::npos || e.path().extension() != ".dds") continue;
        uint64_t hash = strtoull(n.c_str() + u + 1, nullptr, 16);
        if (!is_pad_icon_atlas(hash)) continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
        if (b.size() < 148) continue;
        uint32_t h, w;
        memcpy(&h, &b[12], 4), memcpy(&w, &b[16], 4);
        if (b.size() < 148 + (size_t)w * h * 4) continue;
        for (int style = 1; style <= 2; style++) {
            std::vector<uint8_t> px(b.begin(), b.end());
            if (!pad_icon_atlas(style, hash, w, h, px.data() + 148)) continue;
            char out[64];
            snprintf(out, sizeof out, "padicons_%d_%016llx.dds", style, (unsigned long long)hash);
            std::ofstream(std::filesystem::path(dir) / out, std::ios::binary).write((const char*)px.data(), (std::streamsize)px.size());
            done++;
        }
    }
    printf("pad icons self-test: %d atlases drawn into %s\n", done, dir);
    return done ? 0 : 1;
}

// Resistance badges as icons (setting resist_icons, default 1; user request 11-02, like SMT V). The badges are white
// letters ("Wk", "Rs", "Nu", "Dr", "Rp") that the game draws over a grey disc (a separate sprite): the status screen
// atlases (camp/status001.gtx, status_drc/status_drc001.gtx = the Stats screen) and the battle atlas (battle_02/battle_002.gtx: analysis badges and the larger Rp / Nu /
// Dr). Each letter cell is cleared and gets an icon: <data>\resist_icons\{weak,resist,null,absorb,repel}.png when
// present, else resist_icons\ next to tms.exe (the user's own art, shipped from data/), else a plain disc in the type's colour. Cells = a square around the letters'
// centre, measured on the dumps (2048 scale; the game's own _US atlases without the English mod are the same layout at
// half size). Both atlas sizes are in k_atlases too, so pad_icon_atlas_size keeps their level 0 for hashing.
namespace {
enum Res { R_WK, R_RS, R_NU, R_DR, R_RP, R_COUNT };
const char* const k_res_files[R_COUNT] = {"weak", "resist", "null", "absorb", "repel"};
const uint32_t k_res_colour[R_COUNT] = {0xE02020, 0x9A9A9A, 0xB060E0, 0x50C050, 0x30A0F0};
struct RCell { int cx, cy, size; Res r; };
// 48 = the whole sprite (Skill Inheritance drawlog 11-02: 24 px quads at 720p, Wk uv from (452,34), Rs from (402,184))
const RCell k_status_res[] = {{428, 58, 48, R_DR}, {476, 58, 48, R_WK}, {428, 108, 48, R_RP}, {427, 158, 48, R_NU},
                              {426, 208, 48, R_RS}};
// battle badges: 48 = the whole sprite (target panel drawlog 11-02: 24 px quads at 720p, uv from (2,174) for Wk)
const RCell k_battle_res[] = {{26, 150, 48, R_DR}, {74, 150, 48, R_RP}, {122, 150, 48, R_NU}, {170, 150, 48, R_RS},
                              {26, 198, 48, R_WK}, {128, 982, 60, R_RP}, {213, 982, 60, R_NU}, {301, 982, 60, R_DR}};
struct RAtlas { uint64_t hash; uint32_t w, h; const RCell* cells; size_t n; int div; };
#define RATLAS(h, w, ht, c, d) {h, w, ht, c, sizeof(c) / sizeof(c[0]), d}
const RAtlas k_res_atlases[] = {
    RATLAS(0x266e32dcca881b44ull, 2048, 1024, k_status_res, 1),  // _US camp/status001 (English mod)
    RATLAS(0x6e6254f0ac17ee63ull, 1024, 512, k_status_res, 2),   // the game's own
    RATLAS(0x16f810b5373bff80ull, 2048, 1024, k_status_res, 1),  // _US status_drc/status_drc001 (Stats screen; English mod)
    RATLAS(0x57febd3b87911115ull, 1024, 512, k_status_res, 2),   // the game's own
    // same cells (pixel-matched to status001, 11-02): result/result_001 = Skill Inheritance screen, fusion/fusion001 = Carnage fusion
    RATLAS(0x3c5e643864ee95dfull, 2048, 2048, k_status_res, 1),  // _US result/result_001 (English mod)
    RATLAS(0x7fd8beb4e7f14b1aull, 1024, 1024, k_status_res, 2),  // the game's own
    RATLAS(0x688fc06ddecf3eb3ull, 2048, 2048, k_status_res, 1),  // _US fusion/fusion001 (English mod)
    RATLAS(0x133f164b20932f02ull, 1024, 1024, k_status_res, 2),  // the game's own
    RATLAS(0x7ada3bc97c0dd991ull, 2048, 2048, k_battle_res, 1),  // _US battle_02/battle_002 (English mod)
    RATLAS(0xf7914dfb3c6f343dull, 1024, 1024, k_battle_res, 2),  // the game's own
};
std::atomic<int> g_resist_icons{-1};

// icon k at size x size, BGRA premultiplied; empty when there is no file
#ifndef _WIN32
// Linux: PNG through cairo, cropped to a square around its alpha bounds, scaled with a good filter
std::vector<uint8_t> load_icon(int k, int size) {
    std::vector<uint8_t> out;
    const std::string name = std::string(k_res_files[k]) + ".png";
    std::filesystem::path path = std::filesystem::path(g_data_dir) / "resist_icons" / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        path = plat::exe_path().parent_path() / "resist_icons" / name;
        if (!std::filesystem::exists(path, ec)) return out;
    }
    cairo_surface_t* img = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        log("resist icons: could not read %s", path.string().c_str());
        return out;
    }
    const int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img), st = cairo_image_surface_get_stride(img);
    const uint8_t* px = cairo_image_surface_get_data(img);
    int x0 = iw, y0 = ih, x1 = -1, y1 = -1;
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++)
            if (px[(size_t)y * st + x * 4 + 3] > 24) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    int cx = 0, cy = 0, side = std::min(iw, ih);
    if (x1 >= x0) {
        side = std::min({std::max(x1 - x0, y1 - y0) + 1, iw, ih});
        cx = std::clamp((x0 + x1 + 1 - side) / 2, 0, iw - side), cy = std::clamp((y0 + y1 + 1 - side) / 2, 0, ih - side);
    }
    draw::Canvas cv(size, size);
    cv.clear();
    cairo_scale(cv.cr, (double)size / side, (double)size / side);
    cairo_set_source_surface(cv.cr, img, -cx, -cy);
    cairo_pattern_set_filter(cairo_get_source(cv.cr), CAIRO_FILTER_BEST);
    cairo_paint(cv.cr);
    cairo_surface_destroy(img);
    out.resize((size_t)size * size * 4);
    const uint8_t* src = cv.data();
    for (int y = 0; y < size; y++) memcpy(out.data() + (size_t)y * size * 4, src + (size_t)y * cv.stride(), (size_t)size * 4);
    return out;
}
#else
std::vector<uint8_t> load_icon(int k, int size) {
    std::vector<uint8_t> out;
    // <data>\resist_icons wins (player override), else the set shipped next to tms.exe (user's own art, repo data/)
    const std::string name = std::string(k_res_files[k]) + ".png";
    std::filesystem::path path = std::filesystem::path(g_data_dir) / "resist_icons" / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        path = std::filesystem::path(exe).parent_path() / "resist_icons" / name;
        if (!std::filesystem::exists(path, ec)) return out;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    IWICBitmapClipper* cl = nullptr;
    IWICBitmapScaler* sc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        // the art sits in a larger transparent canvas (192 px, top-left): crop to a square around its alpha bounds so it
        // fills the cell (= the badge's white disc)
        UINT iw = 0, ih = 0;
        cv->GetSize(&iw, &ih);
        std::vector<uint8_t> full((size_t)iw * ih * 4);
        WICRect rc{0, 0, (INT)iw, (INT)ih};
        if (iw && ih && SUCCEEDED(cv->CopyPixels(nullptr, iw * 4, (UINT)full.size(), full.data()))) {
            int x0 = (int)iw, y0 = (int)ih, x1 = -1, y1 = -1;
            for (int y = 0; y < (int)ih; y++)
                for (int x = 0; x < (int)iw; x++)
                    if (full[((size_t)y * iw + x) * 4 + 3] > 24) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            if (x1 >= x0) {
                const int side = std::min({std::max(x1 - x0, y1 - y0) + 1, (int)iw, (int)ih});
                rc.X = std::clamp((x0 + x1 + 1 - side) / 2, 0, (int)iw - side), rc.Y = std::clamp((y0 + y1 + 1 - side) / 2, 0, (int)ih - side);
                rc.Width = rc.Height = side;
            }
        }
        if (SUCCEEDED(f->CreateBitmapClipper(&cl)) && SUCCEEDED(cl->Initialize(cv, &rc)) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(cl, size, size, WICBitmapInterpolationModeFant))) {
            out.resize((size_t)size * size * 4);
            if (FAILED(sc->CopyPixels(nullptr, size * 4, (UINT)out.size(), out.data()))) out.clear();
        }
    }
    release(sc), release(cl), release(cv), release(fr), release(d), release(f);
    if (out.empty()) log("resist icons: could not read %s", path.string().c_str());
    return out;
}
#endif
}  // namespace

bool resist_icons_on() {
    if (g_resist_icons < 0) g_resist_icons = !getenv("NX_RESIST_ICONS") || atoi(getenv("NX_RESIST_ICONS")) != 0;
    return g_resist_icons != 0;
}
void set_resist_icons(bool on) {
    save_setting("resist_icons", on ? "1" : "0");
    g_resist_icons = on;  // the atlases upload again (vk_texture.cpp check_pool_texture)
}
bool is_resist_atlas(uint64_t hash) {
    for (const RAtlas& a : k_res_atlases) if (a.hash == hash) return true;
    return false;
}
bool resist_icon_cell(uint64_t hash, float u, float v) {
    for (const RAtlas& a : k_res_atlases) {
        if (a.hash != hash) continue;
        const float x = u * a.w * a.div, y = v * a.h * a.div;  // in the cells' 2048-scale units
        for (size_t i = 0; i < a.n; i++)
            if (fabsf(x - a.cells[i].cx) < a.cells[i].size / 2.0f && fabsf(y - a.cells[i].cy) < a.cells[i].size / 2.0f) return true;
    }
    return false;
}
// rgba: RGBA8, straight alpha, changed in place
bool resist_icon_atlas(uint64_t hash, uint32_t w, uint32_t h, uint8_t* rgba) {
    for (const RAtlas& a : k_res_atlases) {
        if (a.hash != hash || a.w != w || a.h != h) continue;
        for (size_t i = 0; i < a.n; i++) {
            const RCell& c = a.cells[i];
            const int size = c.size / a.div, x0 = c.cx / a.div - size / 2, y0 = c.cy / a.div - size / 2;
            if (x0 < 0 || y0 < 0 || x0 + size > (int)w || y0 + size > (int)h) continue;
            const std::vector<uint8_t> icon = load_icon(c.r, size);
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++) {
                    uint8_t* d = rgba + ((size_t)(y0 + y) * w + x0 + x) * 4;
                    if (!icon.empty()) {  // BGRA premultiplied -> RGBA straight
                        const uint8_t* s = &icon[((size_t)y * size + x) * 4];
                        const float al = s[3] / 255.0f;
                        for (int k = 0; k < 3; k++) d[k] = al > 0 ? (uint8_t)std::min(255.0f, s[2 - k] / al + 0.5f) : 0;
                        d[3] = s[3];
                        continue;
                    }
                    // no file: a disc of the type's colour with a dark ring, soft 1 px edge
                    const float dx = x + 0.5f - size / 2.0f, dy = y + 0.5f - size / 2.0f, r = sqrtf(dx * dx + dy * dy), R = size / 2.0f - 1;
                    const uint32_t col = r > R * 0.82f ? 0x202020 : k_res_colour[c.r];
                    d[0] = (uint8_t)(col >> 16), d[1] = (uint8_t)(col >> 8), d[2] = (uint8_t)col;
                    d[3] = (uint8_t)(255 * std::clamp(R - r + 0.5f, 0.0f, 1.0f));
                }
        }
        return true;
    }
    return false;
}

}  // namespace rt
