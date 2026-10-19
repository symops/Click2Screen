// Recording effects drawn into the video: a wave at every mouse click (left button blue, right red, middle purple) and,
// while Shift is held, a thin red trail behind the pointer that fades away after five minutes - point by point, in the
// order it was drawn. Clicks come from a low-level mouse hook (the thread that starts it pumps messages); the trail is
// sampled once per video frame.
#include "c2s.h"
#include <math.h>
#include <stdlib.h>

namespace {

struct Click { ULONGLONG t; int x, y, button; };       // button: 0 left, 1 right, 2 middle
const int kMaxClicks = 32;
const ULONGLONG kWaveMs = 650;
const ULONGLONG kTrailMs = 5ULL * 60 * 1000;           // the trail of a point lives five minutes

struct Pt { int x, y; ULONGLONG t; bool brk; };         // brk: this point starts a new stroke

CRITICAL_SECTION g_cs;
bool g_csInit;
Click g_clicks[kMaxClicks];
int g_nClicks;
Pt* g_trail;
size_t g_nTrail, g_head, g_capTrail;
bool g_shiftWas;
HHOOK g_hook;

void Lock() { if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; } EnterCriticalSection(&g_cs); }
void Unlock() { LeaveCriticalSection(&g_cs); }

void AddClick(int x, int y, int button, ULONGLONG t)
{
    Lock();
    if (g_nClicks == kMaxClicks) { memmove(g_clicks, g_clicks + 1, (kMaxClicks - 1) * sizeof(Click)); --g_nClicks; }
    g_clicks[g_nClicks].t = t; g_clicks[g_nClicks].x = x; g_clicks[g_nClicks].y = y; g_clicks[g_nClicks].button = button;
    ++g_nClicks;
    Unlock();
}

void AddPoint(int x, int y, ULONGLONG t, bool brk)
{
    Lock();
    if (g_nTrail == g_capTrail) {
        if (g_head > g_nTrail / 2) {                                    // drop what has faded out
            memmove(g_trail, g_trail + g_head, (g_nTrail - g_head) * sizeof(Pt));
            g_nTrail -= g_head; g_head = 0;
        } else {
            size_t cap = g_capTrail ? g_capTrail * 2 : 4096;
            Pt* p = (Pt*)realloc(g_trail, cap * sizeof(Pt));
            if (!p) { Unlock(); return; }
            g_trail = p; g_capTrail = cap;
        }
    }
    g_trail[g_nTrail].x = x; g_trail[g_nTrail].y = y; g_trail[g_nTrail].t = t; g_trail[g_nTrail].brk = brk;
    ++g_nTrail;
    Unlock();
}

LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        int b = wp == WM_LBUTTONDOWN ? 0 : wp == WM_RBUTTONDOWN ? 1 : wp == WM_MBUTTONDOWN ? 2 : -1;
        if (b >= 0) { const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp; AddClick(m->pt.x, m->pt.y, b, GetTickCount64()); }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

COLORREF WaveColor(int button)
{
    return button == 0 ? RGB(50, 110, 255) : button == 1 ? RGB(255, 60, 60) : RGB(175, 75, 230);
}

// One expanding ring, soft edged, fading out; premultiplied alpha, blended onto the frame.
void DrawWave(HDC dc, double cx, double cy, double radius, double thick, double alpha, COLORREF color)
{
    int R = (int)(radius + thick + 3.0);
    int N = 2 * R + 2;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = N; bi.bmiHeader.biHeight = -N; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HDC mem = CreateCompatibleDC(dc);
    if (!bmp || !mem || !px) { if (bmp) DeleteObject(bmp); if (mem) DeleteDC(mem); return; }
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    const unsigned cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            double dx = x + 0.5 - N / 2.0, dy = y + 0.5 - N / 2.0;
            double d = fabs(sqrt(dx * dx + dy * dy) - radius);
            double cov = thick / 2.0 + 0.5 - d;
            if (cov <= 0) { px[y * N + x] = 0; continue; }
            if (cov > 1) cov = 1;
            unsigned A = (unsigned)(cov * alpha * 255.0);
            px[y * N + x] = (A << 24) | ((cr * A / 255) << 16) | ((cg * A / 255) << 8) | (cb * A / 255);
        }
    BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(dc, (int)(cx - N / 2.0), (int)(cy - N / 2.0), N, N, mem, 0, 0, N, N, bf);
    SelectObject(mem, old); DeleteDC(mem); DeleteObject(bmp);
}

} // namespace

void FxStart()
{
    Lock();
    g_nClicks = 0; g_nTrail = 0; g_head = 0; g_shiftWas = false;
    Unlock();
    if (!g_hook) g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
}

void FxStop()
{
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    Lock();
    g_nClicks = 0; g_nTrail = 0; g_head = 0; g_shiftWas = false;
    Unlock();
}

void FxTestClick(int x, int y, int button) { AddClick(x, y, button, GetTickCount64()); }
void FxTestTrail(int x, int y, bool newStroke, unsigned long long ageMs)
{
    ULONGLONG now = GetTickCount64();
    AddPoint(x, y, now > ageMs ? now - ageMs : 0, newStroke);
}

// Called for every video frame (the frame is already in `dc`): samples Shift / the pointer, draws the trail and the waves.
void FxDraw(HDC dc, const RECT& src, int outW, int outH)
{
    ULONGLONG now = GetTickCount64();
    int sw = src.right - src.left, sh = src.bottom - src.top;
    if (sw <= 0 || sh <= 0) return;
    double sx = (double)outW / sw, sy = (double)outH / sh, sc = sx < sy ? sx : sy;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shift) {
        POINT p;
        if (GetCursorPos(&p)) {
            bool brk = !g_shiftWas;
            bool same = false;
            Lock();
            if (!brk && g_nTrail > g_head) same = g_trail[g_nTrail - 1].x == p.x && g_trail[g_nTrail - 1].y == p.y;
            Unlock();
            if (!same) AddPoint(p.x, p.y, now, brk);
        }
    }
    g_shiftWas = shift;

    Lock();
    while (g_head < g_nTrail && now - g_trail[g_head].t > kTrailMs) ++g_head;       // the oldest points fade out first
    if (g_nTrail > g_head) {
        int w = (int)(2.0 * sc + 0.5); if (w < 1) w = 1;
        HPEN pen = CreatePen(PS_SOLID, w, RGB(255, 40, 40));
        HGDIOBJ op = SelectObject(dc, pen);
        bool pen_down = false;
        for (size_t i = g_head; i < g_nTrail; ++i) {
            int x = (int)((g_trail[i].x - src.left) * sx), y = (int)((g_trail[i].y - src.top) * sy);
            if (g_trail[i].brk || !pen_down) { MoveToEx(dc, x, y, nullptr); pen_down = true; if (g_trail[i].brk) { LineTo(dc, x, y); continue; } }
            LineTo(dc, x, y);
        }
        SelectObject(dc, op); DeleteObject(pen);
    }
    Click cl[kMaxClicks]; int nc = 0;
    for (int i = 0; i < g_nClicks; ++i) if (now - g_clicks[i].t < kWaveMs) cl[nc++] = g_clicks[i];
    Unlock();

    for (int i = 0; i < nc; ++i) {
        double p = (double)(now - cl[i].t) / kWaveMs;                                 // 0 .. 1
        double radius = (8.0 + 34.0 * p) * sc;
        double thick = 3.0 * sc; if (thick < 2.0) thick = 2.0;
        DrawWave(dc, (cl[i].x - src.left) * sx, (cl[i].y - src.top) * sy, radius, thick, 0.95 * (1.0 - p), WaveColor(cl[i].button));
    }
}
