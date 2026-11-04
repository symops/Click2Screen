// Choosing what to capture: a rectangle dragged over the dimmed desktop, or a window pointed at. The desktop stays alive under the
// dimming (a video goes on playing): the overlay is a layered window with a picture of its own, a dark veil with a clear hole.
// Both run their own small message loop on a full-desktop top-most overlay and return when the user decides.
#include "c2s.h"
#include <dwmapi.h>
#include <wchar.h>
#include <stdio.h>

namespace {

struct Pick {
    bool region;                // true: drag a rectangle; false: point at a window
    HBITMAP dib; DWORD* bits;   // the picture of the overlay: premultiplied ARGB, one pixel per pixel of the virtual screen
    RECT vs;                    // virtual screen (screen coordinates of the overlay)
    bool dragging, done, ok;
    POINT a, b;                 // drag corners, overlay coordinates
    RECT result;                // screen coordinates
    HWND hwnd;                  // result window (window mode)
    HWND cur;                   // window under the pointer
    RECT curRc;                 // its rectangle, screen coordinates
    HWND self;
    HFONT font;
};

struct WinEntry { HWND hw; RECT rc; };
WinEntry g_wins[512];
int g_nwins = 0;
HWND g_skip = nullptr;
HWND g_desktop = nullptr;       // marks the desktop entries: the work area of a monitor (no taskbar / Start), below every window

BOOL CALLBACK DesktopProc(HMONITOR mon, HDC, LPRECT, LPARAM)
{
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (g_nwins < 512 && GetMonitorInfoW(mon, &mi)) { g_wins[g_nwins].hw = g_desktop; g_wins[g_nwins].rc = mi.rcWork; ++g_nwins; }
    return TRUE;
}

BOOL CALLBACK EnumProc(HWND hw, LPARAM)
{
    if (hw == g_skip || g_nwins >= 512) return TRUE;
    if (!IsWindowVisible(hw) || IsIconic(hw)) return TRUE;
    LONG ex = GetWindowLongW(hw, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) { if (!(ex & WS_EX_APPWINDOW)) return TRUE; }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hw, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t cls[64]; GetClassNameW(hw, cls, 64);
    // (the desktop windows and the taskbar are listed too: the taskbar can be recorded)
    RECT rc;
    if (!WindowVisibleRect(hw, &rc)) return TRUE;
    if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8) return TRUE;
    g_wins[g_nwins].hw = hw; g_wins[g_nwins].rc = rc; ++g_nwins;
    wchar_t title[60] = L""; GetWindowTextW(hw, title, 60);
    Log(L"picker: window %p [%s] %s %d,%d %dx%d", (void*)hw, cls, title, (int)rc.left, (int)rc.top, (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));
    return TRUE;
}

void Normalize(const POINT& a, const POINT& b, RECT* r)
{
    r->left = a.x < b.x ? a.x : b.x; r->right = a.x < b.x ? b.x : a.x;
    r->top = a.y < b.y ? a.y : b.y;  r->bottom = a.y < b.y ? b.y : a.y;
}

// A box of text (a size label, the hint) is drawn with GDI into a small bitmap and copied into the picture of the overlay, opaque.
void PutBox(Pick* p, int x, int y, int w, int h, COLORREF bg, int tx, int ty, const wchar_t* text)
{
    const int W = p->vs.right - p->vs.left, H = p->vs.bottom - p->vs.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    DWORD* px = nullptr;
    HDC scr = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(scr);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    ReleaseDC(nullptr, scr);
    if (bm && px) {
        HGDIOBJ ob = SelectObject(dc, bm);
        RECT r = { 0, 0, w, h };
        HBRUSH br = CreateSolidBrush(bg); FillRect(dc, &r, br); DeleteObject(br);
        HFONT of = (HFONT)SelectObject(dc, p->font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, tx, ty, text, (int)wcslen(text));
        SelectObject(dc, of);
        GdiFlush();
        for (int yy = 0; yy < h; ++yy) {
            const int dy = y + yy; if (dy < 0 || dy >= H) continue;
            for (int xx = 0; xx < w; ++xx) {
                const int dx = x + xx; if (dx < 0 || dx >= W) continue;
                p->bits[(size_t)dy * W + dx] = px[(size_t)yy * w + xx] | 0xFF000000u;
            }
        }
        SelectObject(dc, ob);
    }
    if (bm) DeleteObject(bm);
    DeleteDC(dc);
}

// Draws the overlay again: the dark veil, a clear hole where the choice is, a blue frame round it, its size, the hint. The hole keeps a
// pixel of alpha 1 (not 0): a layered window passes the mouse through the pixels that have none, and the choice needs the mouse.
void Render(Pick* p)
{
    const int W = p->vs.right - p->vs.left, H = p->vs.bottom - p->vs.top;
    if (!p->bits || W <= 0 || H <= 0) return;
    DWORD* b = p->bits;
    const size_t n = (size_t)W * H;
    const DWORD veil = 140u << 24;                                  // black, 55 % (premultiplied: the colour is 0)
    for (size_t i = 0; i < n; ++i) b[i] = veil;
    RECT sel = {0, 0, 0, 0};
    bool have = false;
    if (p->region) {
        if (p->dragging || p->done) { Normalize(p->a, p->b, &sel); have = true; }
    } else if (p->cur) {
        sel = p->curRc; OffsetRect(&sel, -p->vs.left, -p->vs.top); have = true;
    }
    if (have && sel.right > sel.left && sel.bottom > sel.top) {
        RECT c = sel;                                                // the part inside the overlay
        if (c.left < 0) c.left = 0;
        if (c.top < 0) c.top = 0;
        if (c.right > W) c.right = W;
        if (c.bottom > H) c.bottom = H;
        for (int y = c.top; y < c.bottom; ++y) for (int x = c.left; x < c.right; ++x) b[(size_t)y * W + x] = 0x01000000u;
        const DWORD blue = 0xFF0096FFu;                              // the frame, 2 pixels, on the edge of the choice
        auto dot = [&](int x, int y) { if (x >= 0 && y >= 0 && x < W && y < H) b[(size_t)y * W + x] = blue; };
        for (int x = sel.left - 1; x <= sel.right + 1; ++x) { dot(x, sel.top - 1); dot(x, sel.top); dot(x, sel.bottom); dot(x, sel.bottom + 1); }
        for (int y = sel.top - 1; y <= sel.bottom + 1; ++y) { dot(sel.left - 1, y); dot(sel.left, y); dot(sel.right, y); dot(sel.right + 1, y); }
        wchar_t t[64];
        _snwprintf(t, 64, L"%d x %d", (int)(sel.right - sel.left), (int)(sel.bottom - sel.top));
        HDC scr = GetDC(nullptr); HDC m = CreateCompatibleDC(scr); ReleaseDC(nullptr, scr);
        HFONT of = (HFONT)SelectObject(m, p->font);
        SIZE sz; GetTextExtentPoint32W(m, t, (int)wcslen(t), &sz);
        SelectObject(m, of); DeleteDC(m);
        int ty = sel.top - 24; if (ty < 0) ty = sel.top + 4;
        PutBox(p, sel.left + 2, ty, sz.cx + 4, sz.cy, RGB(0, 150, 255), 2, 0, t);
    }
    // hint at the top centre of the primary monitor
    {
        const wchar_t* hint = T(p->region ? S_PICK_REGION_HINT : S_PICK_WINDOW_HINT);
        HDC scr = GetDC(nullptr); HDC m = CreateCompatibleDC(scr); ReleaseDC(nullptr, scr);
        HFONT of = (HFONT)SelectObject(m, p->font);
        SIZE sz; GetTextExtentPoint32W(m, hint, (int)wcslen(hint), &sz);
        SelectObject(m, of); DeleteDC(m);
        int cx = GetSystemMetrics(SM_CXSCREEN) / 2 - p->vs.left;
        PutBox(p, cx - sz.cx / 2 - 12, 12, sz.cx + 24, sz.cy + 12, RGB(30, 34, 44), 12, 6, hint);
    }
    HDC scr = GetDC(nullptr);
    HDC src = CreateCompatibleDC(scr);
    HGDIOBJ os = SelectObject(src, p->dib);
    POINT pos = { p->vs.left, p->vs.top }, from = { 0, 0 };
    SIZE size = { W, H };
    BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(p->self, scr, &pos, &size, src, &from, 0, &bf, ULW_ALPHA);
    SelectObject(src, os); DeleteDC(src); ReleaseDC(nullptr, scr);
}

void Finish(Pick* p, bool ok)
{
    p->ok = ok; p->done = true;
    // The message loop is blocked in GetMessage: without one more message the overlay would stay on the screen until
    // the next mouse move or key.
    if (p->self) PostMessageW(p->self, WM_NULL, 0, 0);
}

LRESULT CALLBACK PickProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    Pick* p = (Pick*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return DefWindowProcW(hw, m, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hw, &ps); EndPaint(hw, &ps); return 0; }          // the picture of a layered window is set by Render
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, p && p->region ? IDC_CROSS : IDC_HAND));
        return TRUE;
    case WM_LBUTTONDOWN:
        if (p && p->region) {
            p->dragging = true;
            p->a.x = p->b.x = (short)LOWORD(lp); p->a.y = p->b.y = (short)HIWORD(lp);
            SetCapture(hw);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!p) return 0;
        if (p->region) {
            if (p->dragging) { p->b.x = (short)LOWORD(lp); p->b.y = (short)HIWORD(lp); Render(p); }
        } else {
            POINT pt = { (short)LOWORD(lp) + p->vs.left, (short)HIWORD(lp) + p->vs.top };
            HWND found = nullptr; RECT fr = {0, 0, 0, 0};
            for (int i = 0; i < g_nwins; ++i)
                if (PtInRect(&g_wins[i].rc, pt)) { found = g_wins[i].hw; fr = g_wins[i].rc; break; }
            if (found != p->cur || (found && !EqualRect(&fr, &p->curRc))) {
                p->cur = found; p->curRc = fr; Render(p);
                Log(L"picker: pointer %d,%d -> window %p (of %d)", (int)pt.x, (int)pt.y, (void*)found, g_nwins);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (!p) return 0;
        if (p->region && p->dragging) {
            ReleaseCapture();
            p->dragging = false;
            p->b.x = (short)LOWORD(lp); p->b.y = (short)HIWORD(lp);
            RECT r; Normalize(p->a, p->b, &r);
            if (r.right - r.left < 4 || r.bottom - r.top < 4) { Render(p); return 0; }   // a click: try again
            p->result = r; OffsetRect(&p->result, p->vs.left, p->vs.top);
            Finish(p, true);
        } else if (!p->region && p->cur) {
            p->hwnd = p->cur == g_desktop ? nullptr : p->cur;      // the desktop is a fixed rectangle, not a window to follow
            p->result = p->curRc;
            Finish(p, true);
        }
        return 0;
    case WM_RBUTTONUP:
        if (p) Finish(p, false);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && p) Finish(p, false);
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp && p && !p->done) Finish(p, false);
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

bool Run(HWND owner, bool region, RECT* out, HWND* hw)
{
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = PickProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Click2ScreenPicker";
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassW(&wc); reg = true;
    }
    Pick p; memset(&p, 0, sizeof(p));
    p.region = region; p.vs = VirtualScreen();
    int w = p.vs.right - p.vs.left, h = p.vs.bottom - p.vs.top;
    // The owner window steps aside (the overlay would dim it too).
    bool wasVisible = owner && IsWindowVisible(owner);
    DWORD t0 = GetTickCount();
    if (wasVisible) ShowWindow(owner, SW_HIDE);
    {   // the picture of the overlay
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        HDC scr = GetDC(nullptr);
        p.dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, (void**)&p.bits, nullptr, 0);
        ReleaseDC(nullptr, scr);
        if (!p.dib || !p.bits) { if (p.dib) DeleteObject(p.dib); if (wasVisible) ShowWindow(owner, SW_SHOW); return false; }
    }
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 4;
    p.font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!region) {
        g_nwins = 0; g_skip = nullptr; g_desktop = GetDesktopWindow();
        EnumWindows(EnumProc, 0);
        EnumDisplayMonitors(nullptr, nullptr, DesktopProc, 0);       // the desktop is a window too (what is left when no window is under the pointer)
    }
    HWND ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Click2ScreenPicker", L"", WS_POPUP,
                              p.vs.left, p.vs.top, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), &p);
    p.self = ov;
    if (!ov) { DeleteObject(p.dib); DeleteObject(p.font); if (wasVisible) ShowWindow(owner, SW_SHOW); return false; }
    { BOOL off = TRUE; DwmSetWindowAttribute(ov, 3 /*DWMWA_TRANSITIONS_FORCEDISABLED*/, &off, sizeof(off)); }      // no fade-in either
    Render(&p);
    ShowWindow(ov, SW_SHOW);
    SetForegroundWindow(ov); SetFocus(ov);
    Log(L"picker: ready %lu ms after the button", (unsigned long)(GetTickCount() - t0));
    MSG msg;
    bool quit = false; int quitCode = 0;
    while (!p.done) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { quit = true; quitCode = (int)msg.wParam; p.ok = false; break; }
        if (r < 0) break;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    DestroyWindow(ov);
    DeleteObject(p.dib); DeleteObject(p.font);
    // WM_QUIT (the program is being closed) belongs to the main loop: hand it on, or the process would live on without windows
    if (quit) { PostQuitMessage(quitCode); return false; }
    if (wasVisible) ShowWindow(owner, SW_SHOW);
    if (p.ok) { *out = p.result; if (hw) *hw = p.hwnd; }
    return p.ok;
}

} // namespace

bool PickRegion(HWND owner, RECT* out) { return Run(owner, true, out, nullptr); }
bool PickWindow(HWND owner, HWND* hw, RECT* out) { return Run(owner, false, out, hw); }
