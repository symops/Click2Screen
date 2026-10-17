// Choosing what to capture: a rectangle dragged over a dimmed snapshot of the desktop, or a window pointed at.
// Both run their own small message loop on a full-desktop top-most overlay and return when the user decides.
#include "c2s.h"
#include <dwmapi.h>
#include <wchar.h>
#include <stdio.h>

namespace {

struct Pick {
    bool region;                // true: drag a rectangle; false: point at a window
    HBITMAP shot, dim;          // snapshot of the desktop and its darkened copy
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

BOOL CALLBACK EnumProc(HWND hw, LPARAM)
{
    if (hw == g_skip || g_nwins >= 512) return TRUE;
    if (!IsWindowVisible(hw) || IsIconic(hw)) return TRUE;
    LONG ex = GetWindowLongW(hw, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) { if (!(ex & WS_EX_APPWINDOW)) return TRUE; }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hw, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t cls[64]; GetClassNameW(hw, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ) { /* allowed: the taskbar can be recorded */ }
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

HBITMAP MakeDim(HBITMAP src, int w, int h)
{
    HDC scr = GetDC(nullptr);
    HDC d1 = CreateCompatibleDC(scr), d2 = CreateCompatibleDC(scr);
    HBITMAP out = CreateCompatibleBitmap(scr, w, h);
    HBITMAP o1 = (HBITMAP)SelectObject(d1, src), o2 = (HBITMAP)SelectObject(d2, out);
    BitBlt(d2, 0, 0, w, h, d1, 0, 0, SRCCOPY);
    // 55 % black over the copy
    HDC dk = CreateCompatibleDC(scr);
    HBITMAP kb = CreateCompatibleBitmap(scr, 1, 1);
    HBITMAP ko = (HBITMAP)SelectObject(dk, kb);
    SetPixel(dk, 0, 0, RGB(0, 0, 0));
    BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 140; bf.AlphaFormat = 0;
    AlphaBlend(d2, 0, 0, w, h, dk, 0, 0, 1, 1, bf);
    SelectObject(dk, ko); DeleteObject(kb); DeleteDC(dk);
    SelectObject(d1, o1); SelectObject(d2, o2);
    DeleteDC(d1); DeleteDC(d2);
    ReleaseDC(nullptr, scr);
    return out;
}

void Paint(Pick* p, HDC dc)
{
    int w = p->vs.right - p->vs.left, h = p->vs.bottom - p->vs.top;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP buf = CreateCompatibleBitmap(dc, w, h);
    HBITMAP ob = (HBITMAP)SelectObject(mem, buf);
    HDC src = CreateCompatibleDC(dc);
    HBITMAP os = (HBITMAP)SelectObject(src, p->dim);
    BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    RECT sel = {0, 0, 0, 0};
    bool have = false;
    if (p->region) {
        if (p->dragging || p->done) { Normalize(p->a, p->b, &sel); have = true; }
    } else if (p->cur) {
        sel = p->curRc; OffsetRect(&sel, -p->vs.left, -p->vs.top); have = true;
    }
    if (have && sel.right > sel.left && sel.bottom > sel.top) {
        SelectObject(src, p->shot);
        BitBlt(mem, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top, src, sel.left, sel.top, SRCCOPY);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 150, 255));
        HPEN op = (HPEN)SelectObject(mem, pen);
        HBRUSH ob2 = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, sel.left, sel.top, sel.right + 1, sel.bottom + 1);
        SelectObject(mem, ob2); SelectObject(mem, op); DeleteObject(pen);
        wchar_t t[64];
        _snwprintf(t, 64, L"%d x %d", (int)(sel.right - sel.left), (int)(sel.bottom - sel.top));
        HFONT of = (HFONT)SelectObject(mem, p->font);
        SetBkMode(mem, OPAQUE); SetBkColor(mem, RGB(0, 150, 255)); SetTextColor(mem, RGB(255, 255, 255));
        int ty = sel.top - 24; if (ty < 0) ty = sel.top + 4;
        TextOutW(mem, sel.left + 2, ty, t, (int)wcslen(t));
        SelectObject(mem, of);
    }
    // hint at the top centre of the primary monitor
    {
        const wchar_t* hint = T(p->region ? S_PICK_REGION_HINT : S_PICK_WINDOW_HINT);
        HFONT of = (HFONT)SelectObject(mem, p->font);
        SIZE sz; GetTextExtentPoint32W(mem, hint, (int)wcslen(hint), &sz);
        int cx = GetSystemMetrics(SM_CXSCREEN) / 2 - p->vs.left;
        RECT hr = { cx - sz.cx / 2 - 12, 12, cx + sz.cx / 2 + 12, 12 + sz.cy + 12 };
        HBRUSH hb = CreateSolidBrush(RGB(30, 34, 44));
        FillRect(mem, &hr, hb); DeleteObject(hb);
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(255, 255, 255));
        TextOutW(mem, hr.left + 12, hr.top + 6, hint, (int)wcslen(hint));
        SelectObject(mem, of);
    }
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(src, os); DeleteDC(src);
    SelectObject(mem, ob); DeleteObject(buf); DeleteDC(mem);
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
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        if (p) Paint(p, dc);
        EndPaint(hw, &ps);
        return 0; }
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
            if (p->dragging) { p->b.x = (short)LOWORD(lp); p->b.y = (short)HIWORD(lp); InvalidateRect(hw, nullptr, FALSE); }
        } else {
            POINT pt = { (short)LOWORD(lp) + p->vs.left, (short)HIWORD(lp) + p->vs.top };
            HWND found = nullptr; RECT fr = {0, 0, 0, 0};
            for (int i = 0; i < g_nwins; ++i)
                if (PtInRect(&g_wins[i].rc, pt)) { found = g_wins[i].hw; fr = g_wins[i].rc; break; }
            if (found != p->cur || (found && !EqualRect(&fr, &p->curRc))) {
                p->cur = found; p->curRc = fr; InvalidateRect(hw, nullptr, FALSE);
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
            if (r.right - r.left < 4 || r.bottom - r.top < 4) { InvalidateRect(hw, nullptr, FALSE); return 0; }   // a click: try again
            p->result = r; OffsetRect(&p->result, p->vs.left, p->vs.top);
            Finish(p, true);
        } else if (!p->region && p->cur) {
            p->hwnd = p->cur; p->result = p->curRc;
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
    // The owner window stays out of the snapshot.
    bool wasVisible = owner && IsWindowVisible(owner);
    if (wasVisible) { ShowWindow(owner, SW_HIDE); Sleep(200); }
    p.shot = CaptureRect(p.vs, false, false);
    if (!p.shot) { if (wasVisible) ShowWindow(owner, SW_SHOW); return false; }
    p.dim = MakeDim(p.shot, w, h);
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 4;
    p.font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!region) { g_nwins = 0; g_skip = nullptr; EnumWindows(EnumProc, 0); }
    HWND ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Click2ScreenPicker", L"", WS_POPUP,
                              p.vs.left, p.vs.top, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), &p);
    p.self = ov;
    if (!ov) { DeleteObject(p.shot); DeleteObject(p.dim); DeleteObject(p.font); if (wasVisible) ShowWindow(owner, SW_SHOW); return false; }
    ShowWindow(ov, SW_SHOW);
    SetForegroundWindow(ov); SetFocus(ov);
    MSG msg;
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    DestroyWindow(ov);
    DeleteObject(p.shot); DeleteObject(p.dim); DeleteObject(p.font);
    if (wasVisible) ShowWindow(owner, SW_SHOW);
    if (p.ok) { *out = p.result; if (hw) *hw = p.hwnd; }
    return p.ok;
}

} // namespace

bool PickRegion(HWND owner, RECT* out) { return Run(owner, true, out, nullptr); }
bool PickWindow(HWND owner, HWND* hw, RECT* out) { return Run(owner, false, out, hw); }
