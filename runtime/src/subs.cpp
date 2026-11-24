// Battle voice subtitles (setting voice_subs, default 1; user request 10-30). The game plays every battle voice line
// as its own file (Sound/Voice/Jp/BTL_*.adx, CRI ExPlayer_SetFile, seen in the game log hook in hle_sys.cpp), so the
// file name identifies the exact line. subtitles_en.tsv next to tms.exe (built from tools/subs: whisper transcripts,
// translated by hand) maps file stem -> seconds, speaker, English. A worker thread draws the active lines with
// DirectWrite into a premultiplied RGBA bitmap; vk.cpp blends it over the frame (overlay_spv.h), so screenshots and
// recordings include it.
#include "plat.h"
#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#else
#include "draw_linux.h"
#include <algorithm>
namespace D2D1 {
struct ColorF {
    float r, g, b, a;
    ColorF(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
};
}  // namespace D2D1
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "rt.h"

namespace rt {

namespace {
struct Entry { float sec; std::wstring speaker, text; };
struct Line { const Entry* e; int64_t until; };

std::unordered_map<std::string, Entry>& table() {
    static std::unordered_map<std::string, Entry> t = [] {
        std::unordered_map<std::string, Entry> m;
#ifdef _WIN32
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::ifstream f(std::filesystem::path(exe).parent_path() / "subtitles_en.tsv");
        auto wide = [](const std::string& s) {
            std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), 0);
            MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
            return w;
        };
#else
        std::ifstream f(plat::exe_path().parent_path() / "subtitles_en.tsv");
        auto wide = [](const std::string& s) { return draw::wide(s); };
#endif
        for (std::string l; std::getline(f, l);) {  // stem \t seconds \t speaker \t text
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            size_t a = l.find('\t'), b = a == l.npos ? a : l.find('\t', a + 1), c = b == l.npos ? b : l.find('\t', b + 1);
            if (c == l.npos || c + 1 >= l.size()) continue;
            // a two-speaker clip (*_mix files): "speaker / speaker2" and "text // text2" -> a second row under stem#2
            std::string stem = l.substr(0, a), who = l.substr(b + 1, c - b - 1), text = l.substr(c + 1);
            float sec = (float)atof(l.c_str() + a + 1);
            size_t t2 = text.find(" // "), w2 = who.find(" / ");
            if (t2 != text.npos) m[stem + "#2"] = {sec, w2 == who.npos ? L"" : wide(who.substr(w2 + 3)), wide(text.substr(t2 + 4))};
            m[stem] = {sec, wide(who.substr(0, w2)), wide(text.substr(0, t2))};
        }
        log("subtitles: %zu voice lines", m.size());
        return m;
    }();
    return t;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::mutex g_lock;
std::condition_variable g_cv;
std::deque<Line> g_lines;          // active, oldest first (g_lock)
uint32_t g_want_w = 0, g_want_h = 0;  // frame size to draw for (g_lock)
bool g_dirty = false;              // lines or size changed since the last bitmap (g_lock)
std::shared_ptr<const SubsBitmap> g_bitmap;  // latest finished bitmap (g_lock), null = nothing to show
std::atomic<bool> g_any{false};    // lines active (cheap check for the present path)

std::atomic<int> g_enabled{-1};

// Speaker name colours (lime for the party like the settings menu, light blue for Mirages, red for foes)
D2D1::ColorF speaker_color(const std::wstring& s) {
    static const wchar_t* const mirages[] = {L"Chrom", L"Caeda", L"Cain", L"Tharja", L"Virion", L"Draug", L"Navarre", L"Tiki"};
    static const wchar_t* const party[] = {L"Itsuki", L"Tsubasa", L"Touma", L"Kiria", L"Eleonora", L"Mamori", L"Yashiro", L"Maiko", L"Barry"};
    for (auto* m : mirages) if (s == m) return D2D1::ColorF(0.55f, 0.82f, 1.0f, 1);
    for (auto* p : party) if (s == p) return D2D1::ColorF(0.62f, 0.95f, 0.16f, 1);
    return D2D1::ColorF(1.0f, 0.45f, 0.40f, 1);
}

// Draws the lines into a new bitmap: one row per line, centred, speaker name coloured, black outline.
#ifdef _WIN32
std::shared_ptr<SubsBitmap> draw(const std::vector<Line>& lines, uint32_t fw, uint32_t fh) {
    static IWICImagingFactory* wic = nullptr;
    static ID2D1Factory* d2d = nullptr;
    static IDWriteFactory* dw = nullptr;
    if (!wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) {
            log("subtitles: Direct2D / WIC init failed");
            return nullptr;
        }
    }
    // Width 2/3: centred under the turn-order bar, clear of the party portraits on the right; long lines wrap.
    const float size = fh * 0.034f, row = size * 1.45f, pad = size * 0.25f;
    const uint32_t bw = fw * 2 / 3;
    IWICBitmap* bmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    IDWriteTextFormat* tf = nullptr;
    std::shared_ptr<SubsBitmap> out;
    std::vector<IDWriteTextLayout*> tls;
    std::vector<float> ys;
    std::vector<const Entry*> es;
    float bhf = 2 * pad;
    if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &tf))) {
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        tf->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.2f, size * 0.95f);
        for (const Line& l : lines) {
            const Entry& e = *l.e;
            std::wstring s = e.speaker.empty() ? e.text : e.speaker + L": " + e.text;
            IDWriteTextLayout* tl = nullptr;
            if (FAILED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, (float)bw, row, &tl))) continue;
            DWRITE_TEXT_METRICS m{};
            tl->GetMetrics(&m);
            ys.push_back(bhf - pad);
            tls.push_back(tl), es.push_back(&e);
            bhf += std::max(row, m.height + size * 0.25f);
        }
    }
    const uint32_t bh = (uint32_t)bhf;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (!tls.empty() && SUCCEEDED(wic->CreateBitmap(bw, bh, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
        SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bmp, rp, &rt))) {
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush *black = nullptr, *white = nullptr, *name = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.9f), &black);
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &name);
        for (size_t k = 0; k < tls.size(); k++) {
            const Entry& e = *es[k];
            IDWriteTextLayout* tl = tls[k];
            DWRITE_TEXT_RANGE nr{0, e.speaker.empty() ? 0u : (UINT32)e.speaker.size() + 1};
            float y = pad + ys[k], o = std::max(1.5f, size * 0.07f);
            static const float dirs[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
            for (auto& d : dirs) rt->DrawTextLayout(D2D1::Point2F(d[0] * o, y + d[1] * o), tl, black);
            if (nr.length) {
                name->SetColor(speaker_color(e.speaker));
                tl->SetDrawingEffect(name, nr);
            }
            rt->DrawTextLayout(D2D1::Point2F(0, y), tl, white);
        }
        if (black) black->Release();
        if (white) white->Release();
        if (name) name->Release();
        if (SUCCEEDED(rt->EndDraw())) {
            auto b = std::make_shared<SubsBitmap>();
            b->w = bw, b->h = bh, b->x = (fw - bw) / 2;
            b->y = (uint32_t)(fh * 0.145f);  // just below the battle turn-order bar (user 10-30: bottom collided with the wheel)
            b->rgba.resize((size_t)bw * bh);
            WICRect r{0, 0, (INT)bw, (INT)bh};
            if (SUCCEEDED(bmp->CopyPixels(&r, bw * 4, bw * bh * 4, (BYTE*)b->rgba.data()))) {
                for (uint32_t& p : b->rgba) p = (p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);  // BGRA -> RGBA
                out = b;
            }
        }
    }
    for (auto* tl : tls) tl->Release();
    if (tf) tf->Release();
    if (rt) rt->Release();
    if (bmp) bmp->Release();
    return out;
}
#else
// Linux: the same layout with pango: one row per line, centred, speaker name coloured, black outline
std::shared_ptr<SubsBitmap> draw(const std::vector<Line>& lines, uint32_t fw, uint32_t fh) {
    const float size = fh * 0.034f, row = size * 1.45f, pad = size * 0.25f;
    const uint32_t bw = fw * 2 / 3;
    if (!bw) return nullptr;
    // measure on a scratch surface first: the bitmap height follows the wrapped rows
    cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* mcr = cairo_create(scratch);
    struct Row { std::string text; size_t name_bytes; draw::Color name; float y; };
    std::vector<Row> rows;
    float bhf = 2 * pad;
    for (const Line& l : lines) {
        const Entry& e = *l.e;
        const std::string who = draw::utf8(e.speaker), what = draw::utf8(e.text);
        Row r{who.empty() ? what : who + ": " + what, who.empty() ? 0 : who.size() + 1, {}, bhf - pad};
        const D2D1::ColorF c = speaker_color(e.speaker);
        r.name = {c.r, c.g, c.b, c.a};
        PangoLayout* pl = draw::layout(mcr, r.text, size, 600);
        pango_layout_set_width(pl, (int)(bw * PANGO_SCALE));
        pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
        int lw, lh;
        pango_layout_get_pixel_size(pl, &lw, &lh);
        g_object_unref(pl);
        rows.push_back(r);
        bhf += std::max(row, lh + size * 0.25f);
    }
    cairo_destroy(mcr);
    cairo_surface_destroy(scratch);
    if (rows.empty()) return nullptr;
    const uint32_t bh = (uint32_t)bhf;
    draw::Canvas cv((int)bw, (int)bh);
    if (!cv.ok()) return nullptr;
    cv.clear();
    for (const Row& r : rows) {
        PangoLayout* pl = draw::layout(cv.cr, r.text, size, 600);
        pango_layout_set_width(pl, (int)(bw * PANGO_SCALE));
        pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
        const float y = pad + r.y, o = std::max(1.5f, size * 0.07f);
        static const float dirs[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
        cairo_set_source_rgba(cv.cr, 0, 0, 0, 0.9);
        for (auto& d : dirs) cairo_move_to(cv.cr, d[0] * o, y + d[1] * o), pango_cairo_show_layout(cv.cr, pl);
        if (r.name_bytes) {  // the speaker's colour on "Name:" (the outline above stays black)
            PangoAttrList* al = pango_attr_list_new();
            PangoAttribute* a = pango_attr_foreground_new((guint16)(r.name.r * 65535), (guint16)(r.name.g * 65535), (guint16)(r.name.b * 65535));
            a->start_index = 0, a->end_index = (guint)r.name_bytes;
            pango_attr_list_insert(al, a);
            pango_layout_set_attributes(pl, al);
            pango_attr_list_unref(al);
        }
        cairo_set_source_rgba(cv.cr, 1, 1, 1, 1);
        cairo_move_to(cv.cr, 0, y);
        pango_cairo_show_layout(cv.cr, pl);
        g_object_unref(pl);
    }
    auto b = std::make_shared<SubsBitmap>();
    b->w = bw, b->h = bh, b->x = (fw - bw) / 2;
    b->y = (uint32_t)(fh * 0.145f);  // just below the battle turn-order bar
    b->rgba.resize((size_t)bw * bh);
    const uint8_t* src = cv.data();
    for (uint32_t y = 0; y < bh; y++) {
        const uint32_t* s = (const uint32_t*)(src + (size_t)y * cv.stride());
        for (uint32_t x = 0; x < bw; x++) {
            const uint32_t p = s[x];
            b->rgba[(size_t)y * bw + x] = (p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);  // BGRA -> RGBA
        }
    }
    return b;
}
#endif

void worker() {
    for (;;) {
        std::vector<Line> lines;
        uint32_t w, h;
        {
            std::unique_lock<std::mutex> l(g_lock);
            g_cv.wait(l, [] { return g_dirty; });
            g_dirty = false;
            lines.assign(g_lines.begin(), g_lines.end());
            w = g_want_w, h = g_want_h;
        }
        std::shared_ptr<const SubsBitmap> b = lines.empty() || !w ? nullptr : draw(lines, w, h);
        std::lock_guard<std::mutex> l(g_lock);
        g_bitmap = b;
    }
}

void kick() {  // g_lock held
    static std::once_flag once;
    std::call_once(once, [] { std::thread(worker).detach(); });
    g_dirty = true;
    g_cv.notify_one();
}
}  // namespace

bool subs_enabled() {
    if (g_enabled < 0) {
        const char* v = getenv("NX_VOICE_SUBS");
        g_enabled = v ? atoi(v) != 0 : 1;
    }
    return g_enabled != 0;
}

void set_subs_enabled(bool on) {
    g_enabled = on;
    save_setting("voice_subs", on ? "1" : "0");
    std::lock_guard<std::mutex> l(g_lock);
    g_lines.clear();
    g_any = false;
    kick();
}

void subs_voice(const char* path) {
    const char* f = strrchr(path, '/');
    f = f ? f + 1 : path;
    if (strncmp(f, "BTL_", 4) || !subs_enabled()) return;
    std::string stem(f, strcspn(f, "."));
    auto it = table().find(stem), it2 = table().find(stem + "#2");
    if (it == table().end()) return;
    const int64_t t = now_ms(), until = t + 600 + (int64_t)(std::max(it->second.sec, 1.2f) * 1000);
    std::lock_guard<std::mutex> l(g_lock);
    for (auto i = g_lines.begin(); i != g_lines.end();)  // the same line again: only the newest copy
        i = i->e == &it->second ? g_lines.erase(i) : i + 1;
    g_lines.push_back({&it->second, until});
    if (it2 != table().end()) g_lines.push_back({&it2->second, until});
    while (g_lines.size() > 2) g_lines.pop_front();  // a third row would reach the action banner
    g_any = true;
    kick();
}

// A short line of our own in the subtitle spot (battle speed changes): shown even with subtitles off.
void subs_notice(const std::wstring& text) {
    static std::deque<Entry> keep;  // lines point at their entry
    std::lock_guard<std::mutex> l(g_lock);
    if (keep.size() > 8) keep.pop_front();
    keep.push_back({1.0f, L"", text});
    for (auto i = g_lines.begin(); i != g_lines.end();)  // only the newest notice
        i = i->e->speaker.empty() ? g_lines.erase(i) : i + 1;
    g_lines.push_back({&keep.back(), now_ms() + 1500});
    while (g_lines.size() > 2) g_lines.pop_front();
    g_any = true;
    kick();
}

std::shared_ptr<const SubsBitmap> subs_frame(uint32_t fw, uint32_t fh) {
    if (!g_any) return nullptr;
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> l(g_lock);
    bool changed = fw != g_want_w || fh != g_want_h;
    g_want_w = fw, g_want_h = fh;
    while (!g_lines.empty() && g_lines.front().until <= t) g_lines.pop_front(), changed = true;
    for (auto i = g_lines.begin(); i != g_lines.end();)
        if (i->until <= t) i = g_lines.erase(i), changed = true;
        else ++i;
    if (g_lines.empty()) g_any = false, g_bitmap = nullptr;
    if (changed) kick();
    return g_bitmap && g_bitmap->x + g_bitmap->w <= fw && g_bitmap->y + g_bitmap->h <= fh ? g_bitmap : nullptr;
}

}  // namespace rt
