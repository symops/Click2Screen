// Recording effects drawn into the video: a wave at every mouse click (left button blue, right red, middle purple) and,
// the red lines drawn in the drawing window (left button draws, right button wipes) that fade away after ten seconds - point by
// point, in the order they were drawn. Clicks come from a low-level mouse hook (the thread that starts it pumps messages); the
// points of the lines are given by the drawing window (FxPenPoint).
#include "c2s.h"
#include <math.h>
#include <stdlib.h>

namespace {

struct Click { ULONGLONG t; int x, y, button; };       // button: 0 left, 1 right, 2 middle
const int kMaxClicks = 32;
const ULONGLONG kWaveMs = 650;
const ULONGLONG kTrailDefaultMs = 10ULL * 1000;          // a point of the trail lives 10 seconds
ULONGLONG g_trailMs = kTrailDefaultMs;                  // (the autotest shortens it: a fresh machine has not been up for five minutes)

struct Pt { int x, y; ULONGLONG t; bool brk; unsigned char shade; };   // brk: this point starts a new stroke; shade: 0..31 of the gradient

// The gradient of the lines: all the colours of the rainbow, a full turn in 3 seconds of drawing, so a stroke goes through the
// colours as it is drawn (the shade, 0..63 round the colour wheel, is kept with each point: nothing changes afterwards).
unsigned char ShadeAt(ULONGLONG t)
{
    return (unsigned char)((int)((double)(t % 3000) / 3000.0 * 64.0) & 63);
}

CRITICAL_SECTION g_cs;
bool g_csInit;
Click g_clicks[kMaxClicks];
int g_nClicks;
Pt* g_trail;
size_t g_nTrail, g_head, g_capTrail;
unsigned g_version;                                         // changes whenever the trail does (the on-screen copy redraws then)
bool g_btnWas[3];
HHOOK g_hook;
DWORD g_hookErr;
unsigned g_hookCalls;                                      // clicks seen by the hook (diagnostics)

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
    g_trail[g_nTrail].x = x; g_trail[g_nTrail].y = y; g_trail[g_nTrail].t = t; g_trail[g_nTrail].brk = brk; g_trail[g_nTrail].shade = ShadeAt(t);
    ++g_nTrail; ++g_version;
    Unlock();
}

LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        int b = wp == WM_LBUTTONDOWN ? 0 : wp == WM_RBUTTONDOWN ? 1 : wp == WM_MBUTTONDOWN ? 2 : -1;
        if (b >= 0) { const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp; ++g_hookCalls; AddClick(m->pt.x, m->pt.y, b, GetTickCount64()); }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// The points that have lived their time go, the oldest first (called with the lock held).
void Prune(ULONGLONG now)
{
    size_t before = g_head;
    while (g_head < g_nTrail && now - g_trail[g_head].t > g_trailMs) ++g_head;
    if (g_head != before) ++g_version;
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
    g_nClicks = 0; g_nTrail = 0; g_head = 0; ++g_version;
    Unlock();
    if (!g_hook) { g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0); g_hookErr = g_hook ? 0 : GetLastError(); }
    g_hookCalls = 0;
}

void FxStop()
{
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    Lock();
    g_nClicks = 0; g_nTrail = 0; g_head = 0; ++g_version;
    Unlock();
}

bool FxTestHook(unsigned* clicksSeen, unsigned long* err) { *clicksSeen = g_hookCalls; *err = g_hookErr; return g_hook != nullptr; }
// The mouse pen of the drawing window (it works over windows of any privilege: the input goes to our own window).
void FxPenPoint(int x, int y, bool newStroke)
{
    bool same = false;
    Lock();
    if (!newStroke && g_nTrail > g_head) same = g_trail[g_nTrail - 1].x == x && g_trail[g_nTrail - 1].y == y;
    Unlock();
    if (!same) AddPoint(x, y, GetTickCount64(), newStroke);
}

void FxWipe()
{
    Lock();
    if (g_nTrail > g_head) { g_head = g_nTrail = 0; ++g_version; }
    Unlock();
}

// The visible points of the trail (for the copy on the screen): newest last; `brk` marks the start of a stroke.
COLORREF FxLineColor(unsigned char shade)
{
    double h = (shade & 63) * 6.0 / 64.0;                       // 0..6 round the colour wheel, full colours
    int i = (int)h; double f = h - i;
    int up = (int)(255.0 * f + 0.5), down = 255 - up;
    switch (i) {
    case 0: return RGB(255, up, 0);
    case 1: return RGB(down, 255, 0);
    case 2: return RGB(0, 255, up);
    case 3: return RGB(0, down, 255);
    case 4: return RGB(up, 0, 255);
    default: return RGB(255, 0, down);
    }
}

int FxTrailSnapshot(POINT* pts, unsigned char* brk, int cap, unsigned* version, unsigned char* shade)
{
    Lock();
    Prune(GetTickCount64());
    int n = (int)(g_nTrail - g_head);
    size_t from = g_head;
    if (n > cap) { from = g_nTrail - cap; n = cap; }
    for (int i = 0; i < n; ++i) { pts[i].x = g_trail[from + i].x; pts[i].y = g_trail[from + i].y; brk[i] = g_trail[from + i].brk ? 1 : 0; if (shade) shade[i] = g_trail[from + i].shade; }
    if (n > 0 && n == cap) brk[0] = 1;
    *version = g_version;
    Unlock();
    return n;
}

void FxTestTrailLifetime(unsigned ms) { g_trailMs = ms ? ms : kTrailDefaultMs; }
void FxTestClick(int x, int y, int button) { AddClick(x, y, button, GetTickCount64()); }
void FxTestTrail(int x, int y, bool newStroke, unsigned long long ageMs)
{
    ULONGLONG now = GetTickCount64();
    AddPoint(x, y, now > ageMs ? now - ageMs : 0, newStroke);
}

// Called for every video frame (the frame is already in `dc`): draws the lines and the waves.
void FxDraw(HDC dc, const RECT& src, int outW, int outH, bool waves)
{
    ULONGLONG now = GetTickCount64();
    int sw = src.right - src.left, sh = src.bottom - src.top;
    if (sw <= 0 || sh <= 0) return;
    double sx = (double)outW / sw, sy = (double)outH / sh, sc = sx < sy ? sx : sy;

    // The hook is the exact source of clicks; the state of the buttons is read here as well, once per frame, so a click is
    // not lost when the hook is not called (a hook can be refused or removed by Windows): a press seen by neither is added,
    // one the hook has already reported (same button, a moment ago) is not counted twice.
    static const int vks[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    for (int b = 0; b < 3; ++b) {
        SHORT st = GetAsyncKeyState(vks[b]);
        bool down = (st & 0x8000) != 0, pressedSince = (st & 1) != 0;
        if ((down && !g_btnWas[b]) || pressedSince) {
            bool seen = false;
            Lock();
            for (int i = 0; i < g_nClicks; ++i) if (g_clicks[i].button == b && now - g_clicks[i].t < 300) seen = true;
            Unlock();
            POINT cp;
            if (!seen && GetCursorPos(&cp)) AddClick(cp.x, cp.y, b, now);
        }
        g_btnWas[b] = down;
    }

    Lock();
    Prune(now);                                                                      // the oldest points fade out first
    if (g_nTrail > g_head) {
        int w = (int)(3.0 * sc + 0.5); if (w < 2) w = 2;                         // thin, but it survives video compression
        // Two passes: a thin dark outline under the whole line (it shows on a background of any colour), then the gradient on top.
        HPEN pens[64]; for (int i = 0; i < 64; ++i) pens[i] = nullptr;           // one pen for each shade, made when it is needed
        HPEN outline = CreatePen(PS_SOLID, w + 2, RGB(30, 30, 30));
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        for (int pass = 0; pass < 2; ++pass) {
            bool pen_down = false; int px = 0, py = 0;
            for (size_t i = g_head; i < g_nTrail; ++i) {
                int x = (int)((g_trail[i].x - src.left) * sx), y = (int)((g_trail[i].y - src.top) * sy);
                int sh = g_trail[i].shade & 63;
                if (pass == 0) SelectObject(dc, outline);
                else { if (!pens[sh]) pens[sh] = CreatePen(PS_SOLID, w, FxLineColor((unsigned char)sh)); SelectObject(dc, pens[sh]); }
                if (g_trail[i].brk || !pen_down) { MoveToEx(dc, x, y, nullptr); pen_down = true; px = x; py = y; if (g_trail[i].brk) { LineTo(dc, x, y); continue; } }
                MoveToEx(dc, px, py, nullptr); LineTo(dc, x, y); px = x; py = y;
            }
        }
        SelectObject(dc, op);
        DeleteObject(outline);
        for (int i = 0; i < 64; ++i) if (pens[i]) DeleteObject(pens[i]);
    }
    Click cl[kMaxClicks]; int nc = 0;
    for (int i = 0; i < g_nClicks; ++i) if (now - g_clicks[i].t < kWaveMs) cl[nc++] = g_clicks[i];
    Unlock();

    for (int i = 0; i < nc && waves; ++i) {
        double p = (double)(now - cl[i].t) / kWaveMs;                                 // 0 .. 1
        double radius = (10.0 + 50.0 * p) * sc;                                       // a ring that grows from the pointer, easy to see in a video
        double thick = 6.0 * sc * (1.0 - 0.5 * p); if (thick < 3.0) thick = 3.0;
        DrawWave(dc, (cl[i].x - src.left) * sx, (cl[i].y - src.top) * sy, radius, thick, 0.95 * (1.0 - 0.8 * p), WaveColor(cl[i].button));
    }
}
