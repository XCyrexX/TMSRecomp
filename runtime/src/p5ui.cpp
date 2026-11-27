// P5-style battle command panel (setting battle_buttons = 2; Discord request 11-25, prototype s113): while the command
// wheel waits for a command, the game's wheel (battle HUD part 2, draw 0x2ee160) is not drawn (hle_sys.cpp
// btl_ui_draw) and this panel is blended over the frame instead (overlay.cpp frame_overlay): each command on a slanted
// black label with a red edge and the pad button that picks it (ui.cpp poll_input; positions: bottom Attack, right
// Guard, top Skills, left Items; L Tactics, R Change, - Escape), laid out like the pad's face. Windows (Direct2D) only
// for now; Linux keeps the game's wheel.
#include "plat.h"  // Linux: GetTickCount64
#include "rt.h"
#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#endif
#include <algorithm>
#include <cmath>
#include <mutex>

namespace rt {
static std::atomic<int> g_mode{-1};
int battle_buttons_mode() {
    if (g_mode < 0) g_mode = getenv("NX_BATTLE_BUTTONS") ? std::clamp(atoi(getenv("NX_BATTLE_BUTTONS")), 0, 2) : 0;
    return g_mode;
}
void set_battle_buttons_mode(int m) { g_mode = m; log("p5: battle buttons %d", m); }
bool p5_wheel_up(int64_t now, bool input) {
    if (now - g_wheel_blocked_ms < 150) return false;  // tutorial / Stats over the wheel: theirs (user 11-26: soft lock, Stats haywire)
    return now - (input ? g_wheel_ms.load() : std::max(g_wheel_ms.load(), g_wheel_open_ms.load())) < 120;
}
bool p5_panel_active() {  // the wheel is up and the panel replaces it
#ifndef _WIN32
    return false;  // no panel drawing on Linux yet: the game's wheel stays
#endif
    return battle_buttons_mode() == 2 && p5_wheel_up((int64_t)GetTickCount64(), false);
}
#ifdef _WIN32
namespace {
struct Cmd { const wchar_t* name; const wchar_t* sub; float x, y, scale; int button; };  // x, y: panel units (-1..1)
enum { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_L, BTN_R, BTN_MINUS, BTN_ZR, BTN_PLUS };
const Cmd k_cmds[] = {
    {L"ATTACK", L"Weapon strike", 0.05f, 0.78f, 1.25f, BTN_SOUTH}, {L"GUARD", L"Defend, heal EP", 0.62f, 0.22f, 1.0f, BTN_EAST},
    {L"SKILLS", L"Mirage skills", 0.18f, -0.42f, 1.1f, BTN_NORTH}, {L"ITEMS", L"Use an item", -0.55f, 0.18f, 1.0f, BTN_WEST},
    {L"TACTICS", L"Orders", -0.62f, -0.62f, 0.8f, BTN_L}, {L"CHANGE", L"Swap members", 0.78f, -0.78f, 0.8f, BTN_R},
    {L"ESCAPE", L"Run away", -0.85f, 0.78f, 0.75f, BTN_MINUS},
    {L"AUTO", L"Auto-Battle", 0.9f, 0.5f, 0.7f, BTN_ZR},
    {L"STATS", L"Member info", 1.0f, 0.86f, 0.7f, BTN_PLUS}};  // the game's own + (hle_sys.cpp: its Stats / Auto-Battle pills,
                                                              // part 5, hide while the panel is up: user 11-26, the pill said X)  // not a wheel command: ui.cpp presses the game's X (user 11-26: the
                                                           // game's own "(X) Auto-Battle" hint is that face button, P5 has Items there)

// the glyph of a button for the pad style (pad_icon_style: 0 Switch, 1 PlayStation, 2 Xbox)
void button_glyph(ID2D1RenderTarget* rt, IDWriteFactory* dw, ID2D1Factory* f, int b, int style, D2D1_POINT_2F c, float r) {
    ID2D1SolidColorBrush *white = nullptr, *ink = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &ink);
    if (b <= BTN_WEST) {
        rt->FillEllipse({c, r, r}, white);
        rt->DrawEllipse({c, r, r}, ink, r * 0.12f);
        const float g = r * 0.45f, sw = r * 0.2f;
        if (style == 1) {  // PlayStation shapes in their colours
            ID2D1SolidColorBrush* col = nullptr;
            static const uint32_t cols[4] = {0x4A7FD8, 0xE0464B, 0x2FA67E, 0xD064C8};
            rt->CreateSolidColorBrush(D2D1::ColorF(cols[b]), &col);
            if (b == BTN_SOUTH) rt->DrawLine({c.x - g, c.y - g}, {c.x + g, c.y + g}, col, sw), rt->DrawLine({c.x - g, c.y + g}, {c.x + g, c.y - g}, col, sw);
            else if (b == BTN_EAST) rt->DrawEllipse({c, g, g}, col, sw);
            else if (b == BTN_WEST) rt->DrawRectangle({c.x - g * 0.85f, c.y - g * 0.85f, c.x + g * 0.85f, c.y + g * 0.85f}, col, sw);
            else {
                ID2D1PathGeometry* p = nullptr;
                ID2D1GeometrySink* s = nullptr;
                f->CreatePathGeometry(&p);
                p->Open(&s);
                s->BeginFigure({c.x, c.y - g}, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine({c.x + g, c.y + g * 0.75f});
                s->AddLine({c.x - g, c.y + g * 0.75f});
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                rt->DrawGeometry(p, col, sw);
                s->Release(), p->Release();
            }
            col->Release();
        } else {  // letters: Xbox A B Y X by position, Switch B A X Y
            static const wchar_t* xb[4] = {L"A", L"B", L"Y", L"X"};
            static const wchar_t* sw_[4] = {L"B", L"A", L"X", L"Y"};
            static const uint32_t xcol[4] = {0x3E9B2F, 0xD8262B, 0xE0A800, 0x2B6CD6};
            IDWriteTextFormat* tf = nullptr;
            dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, r * 1.25f, L"en-us", &tf);
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ID2D1SolidColorBrush* col = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(style == 2 ? xcol[b] : 0x000000), &col);
            const wchar_t* t = style == 2 ? xb[b] : sw_[b];
            rt->DrawText(t, 1, tf, {c.x - r, c.y - r, c.x + r, c.y + r}, col);
            col->Release(), tf->Release();
        }
    } else {  // shoulders / minus: a white rounded tag
        static const wchar_t* names[3][5] = {{L"L", L"R", L"−", L"ZR", L"+"}, {L"L1", L"R1", L"SHARE", L"R2", L"OPTIONS"}, {L"LB", L"RB", L"VIEW", L"RT", L"MENU"}};
        const wchar_t* t = names[std::clamp(style, 0, 2)][b - BTN_L];
        const float w = r * (wcslen(t) > 5 ? 3.5f : wcslen(t) > 2 ? 2.6f : 1.6f);
        rt->FillRoundedRectangle({{c.x - w, c.y - r * 0.75f, c.x + w, c.y + r * 0.75f}, r * 0.5f, r * 0.5f}, white);
        IDWriteTextFormat* tf = nullptr;
        dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, r * 0.95f, L"en-us", &tf);
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER), tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt->DrawText(t, (UINT32)wcslen(t), tf, {c.x - w, c.y - r, c.x + w, c.y + r}, ink);
        tf->Release();
    }
    white->Release(), ink->Release();
}

bool has_cmd(uint32_t ids, int id) {
    for (int i = 0; i < 8 && (ids >> 4 * i & 15); i++)
        if ((int)(ids >> 4 * i & 15) == id + 1) return true;
    return false;
}
std::shared_ptr<SubsBitmap> draw_panel(uint32_t fw, uint32_t fh, int style, uint32_t ids) {
    static IWICImagingFactory* wic = nullptr;
    static ID2D1Factory* d2d = nullptr;
    static IDWriteFactory* dw = nullptr;
    if (!wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw)))
            return nullptr;
    }
    // panel: right of the screen centre (the acting member stands in the middle), lower half, in the 16:9 area
    const float u = fh / 720.0f;  // 720p units
    const uint32_t bw = (uint32_t)(560 * u), bh = (uint32_t)(380 * u);
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    std::shared_ptr<SubsBitmap> out;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(wic->CreateBitmap(bw, bh, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(bmp, rp, &rt))) {
        if (bmp) bmp->Release();
        return nullptr;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    ID2D1SolidColorBrush *black = nullptr, *red = nullptr, *white = nullptr, *grey = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.04f, 0.04f, 0.93f), &black);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x8BE01E), &red);  // TMS lime green (user 11-26; was P5 red 0xE8102A)
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.78f, 0.78f, 1), &grey);
    static const int k_id[] = {0, 3, 1, 2, 5, 4, 6, -1, -1};  // k_cmds -> wheel command ids (hle_sys.cpp btl_wheel_input), -1 always
    for (const Cmd& c : k_cmds) {
        if (k_id[&c - k_cmds] >= 0 && !has_cmd(ids, k_id[&c - k_cmds])) continue;  // e.g. Tactics: only some members' wheels have it
        const float s = c.scale * u;
        const float cx = bw * 0.5f + c.x * bw * 0.38f, cy = bh * 0.5f + c.y * bh * 0.36f;
        // the button's half width: a disc 15, a shoulder / minus tag wider (its text must not run into the name)
        static const wchar_t* tags[3][5] = {{L"L", L"R", L"−", L"ZR", L"+"}, {L"L1", L"R1", L"SHARE", L"R2", L"OPTIONS"}, {L"LB", L"RB", L"VIEW", L"RT", L"MENU"}};
        const float gw = (c.button <= BTN_WEST ? 15.0f : 15.0f * [&] { const size_t n = wcslen(tags[std::clamp(style, 0, 2)][c.button - BTN_L]); return n > 5 ? 3.5f : n > 2 ? 2.6f : 1.6f; }()) * s;
        const float tx = 2 * gw + 4 * s;  // name start from the label's left edge
        const float lw = 150 * s + std::max(0.0f, tx - 34 * s), lh = 40 * s;
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(-8, {cx, cy}));
        // red slab behind, black slanted label in front (the P5 look), name in heavy italic, a small line under it
        auto slab = [&](float dx, float dy, float grow, ID2D1Brush* br) {
            ID2D1PathGeometry* p = nullptr;
            ID2D1GeometrySink* sk = nullptr;
            d2d->CreatePathGeometry(&p);
            p->Open(&sk);
            const float x0 = cx - lw / 2 - grow + dx, x1 = cx + lw / 2 + grow + dx, y0 = cy - lh / 2 - grow + dy, y1 = cy + lh / 2 + grow + dy;
            sk->BeginFigure({x0 + lh * 0.25f, y0}, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine({x1 + lh * 0.25f, y0}), sk->AddLine({x1 - lh * 0.25f, y1}), sk->AddLine({x0 - lh * 0.25f, y1});
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();
            rt->FillGeometry(p, br);
            sk->Release(), p->Release();
        };
        slab(6 * s, 5 * s, 2 * s, red);
        slab(0, 0, 0, black);
        IDWriteTextFormat* tf = nullptr;
        dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL, 26 * s, L"en-us", &tf);
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING), tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt->DrawText(c.name, (UINT32)wcslen(c.name), tf, {cx - lw / 2 + tx, cy - lh / 2 - 4 * s, cx + lw / 2 + 20 * s, cy + lh / 2 - 6 * s}, white);
        tf->Release();
        dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL, 10.5f * s, L"en-us", &tf);
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING), tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt->DrawText(c.sub, (UINT32)wcslen(c.sub), tf, {cx - lw / 2 + tx + 2 * s, cy + lh / 2 - 16 * s, cx + lw / 2 + 20 * s, cy + lh / 2 - 1 * s}, grey);
        tf->Release();
        button_glyph(rt, dw, d2d, c.button, style, {cx - lw / 2 + gw - 3 * s, cy}, 15 * s);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    black->Release(), red->Release(), white->Release(), grey->Release();
    if (SUCCEEDED(rt->EndDraw())) {
        auto b = std::make_shared<SubsBitmap>();
        b->w = bw, b->h = bh;
        b->x = std::min(fw - bw, (uint32_t)(fw * 0.5f - bw * 0.15f));  // just right of the middle
        b->y = fh - bh - (uint32_t)(48 * u);                             // above the HELP bar
        b->rgba.resize((size_t)bw * bh);
        WICRect r{0, 0, (INT)bw, (INT)bh};
        if (SUCCEEDED(bmp->CopyPixels(&r, bw * 4, bw * bh * 4, (BYTE*)b->rgba.data()))) {
            for (uint32_t& p : b->rgba) p = (p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);  // BGRA -> RGBA
            out = b;
        }
    }
    rt->Release(), bmp->Release();
    return out;
}
}  // namespace

std::shared_ptr<const SubsBitmap> p5_panel(uint32_t fw, uint32_t fh) {
    if (!p5_panel_active() || fw < 64 || fh < 64) return nullptr;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    static std::shared_ptr<const SubsBitmap> cached;
    static uint32_t cw = 0, ch = 0;
    static int cs = -1;
    static uint32_t cids = 0;
    const int style = pad_icon_style();
    const uint32_t ids = g_wheel_ids;
    if (!cached || cw != fw || ch != fh || cs != style || cids != ids) {
        cached = draw_panel(fw, fh, style, ids), cw = fw, ch = fh, cs = style, cids = ids;
        if (cached) log("p5: command panel %ux%u at %u,%u (style %d)", cached->w, cached->h, cached->x, cached->y, style);
    }
    return cached;
}
#else
std::shared_ptr<const SubsBitmap> p5_panel(uint32_t, uint32_t) { return nullptr; }
#endif
}  // namespace rt
