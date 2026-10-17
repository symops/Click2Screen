// Screenshot editor: view with zoom and scroll bars, red rectangles, undo, Save (over the file) and Save as.
#include "c2s.h"
#include <commdlg.h>
#include <wchar.h>

namespace {

enum { ID_RECT = 1, ID_ZIN, ID_ZOUT, ID_UNDO, ID_SAVE, ID_SAVEAS, MAX_RECTS = 256, TOOLBAR_H = 40 };

struct Ed {
    HBITMAP bmp;
    int w, h;
    wchar_t path[MAX_PATH];
    int format;
    RECT rects[MAX_RECTS];
    int nrects;
    double zoom;
    int sx, sy;                 // scroll offsets, window pixels
    bool drawing; POINT p0, p1; // drag, image coordinates
    HWND hwnd, bRect, bZin, bZout, bUndo, bSave, bSaveAs;
    HFONT font;
    bool dirty;
};

int Sc(int v) { return MulDiv(v, CurrentDpi(), 96); }

void CanvasRect(Ed* e, RECT* r)
{
    GetClientRect(e->hwnd, r);
    r->top = Sc(TOOLBAR_H);
}

void UpdateScroll(Ed* e)
{
    RECT c; CanvasRect(e, &c);
    int cw = c.right - c.left, ch = c.bottom - c.top;
    int iw = (int)(e->w * e->zoom + 0.5), ih = (int)(e->h * e->zoom + 0.5);
    int maxx = iw > cw ? iw - cw : 0, maxy = ih > ch ? ih - ch : 0;
    if (e->sx > maxx) e->sx = maxx;
    if (e->sy > maxy) e->sy = maxy;
    if (e->sx < 0) e->sx = 0;
    if (e->sy < 0) e->sy = 0;
    SCROLLINFO si; memset(&si, 0, sizeof(si)); si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = iw > cw ? iw - 1 : 0; si.nPage = cw; si.nPos = e->sx;
    SetScrollInfo(e->hwnd, SB_HORZ, &si, TRUE);
    si.nMax = ih > ch ? ih - 1 : 0; si.nPage = ch; si.nPos = e->sy;
    SetScrollInfo(e->hwnd, SB_VERT, &si, TRUE);
}

// image point -> window point
POINT ToWin(Ed* e, int ix, int iy)
{
    RECT c; CanvasRect(e, &c);
    int iw = (int)(e->w * e->zoom + 0.5), ih = (int)(e->h * e->zoom + 0.5);
    int ox = (c.right - c.left > iw) ? (c.right - c.left - iw) / 2 : -e->sx;
    int oy = (c.bottom - c.top > ih) ? (c.bottom - c.top - ih) / 2 : -e->sy;
    POINT p = { c.left + ox + (int)(ix * e->zoom), c.top + oy + (int)(iy * e->zoom) };
    return p;
}
POINT ToImg(Ed* e, int wx, int wy)
{
    POINT o = ToWin(e, 0, 0);
    POINT p = { (int)((wx - o.x) / e->zoom), (int)((wy - o.y) / e->zoom) };
    if (p.x < 0) p.x = 0; if (p.y < 0) p.y = 0;
    if (p.x > e->w) p.x = e->w; if (p.y > e->h) p.y = e->h;
    return p;
}

void NormRect(POINT a, POINT b, RECT* r)
{
    r->left = a.x < b.x ? a.x : b.x; r->right = a.x < b.x ? b.x : a.x;
    r->top = a.y < b.y ? a.y : b.y;  r->bottom = a.y < b.y ? b.y : a.y;
}

void DrawRects(HDC dc, Ed* e, bool atImage, double k, int ox, int oy)
{
    int pw = atImage ? (e->w > 1600 ? 5 : 3) : Sc(3);
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(230, 40, 40));
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (int i = 0; i < e->nrects; ++i)
        Rectangle(dc, ox + (int)(e->rects[i].left * k), oy + (int)(e->rects[i].top * k),
                      ox + (int)(e->rects[i].right * k), oy + (int)(e->rects[i].bottom * k));
    if (e->drawing) {
        RECT r; NormRect(e->p0, e->p1, &r);
        Rectangle(dc, ox + (int)(r.left * k), oy + (int)(r.top * k), ox + (int)(r.right * k), oy + (int)(r.bottom * k));
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
}

// The picture with the rectangles drawn into it.
HBITMAP Render(Ed* e)
{
    HDC scr = GetDC(nullptr);
    HDC src = CreateCompatibleDC(scr), dst = CreateCompatibleDC(scr);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = e->w; bi.bmiHeader.biHeight = e->h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP out = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP os = (HBITMAP)SelectObject(src, e->bmp), od = (HBITMAP)SelectObject(dst, out);
    BitBlt(dst, 0, 0, e->w, e->h, src, 0, 0, SRCCOPY);
    bool d = e->drawing; e->drawing = false;
    DrawRects(dst, e, true, 1.0, 0, 0);
    e->drawing = d;
    SelectObject(src, os); SelectObject(dst, od);
    DeleteDC(src); DeleteDC(dst); ReleaseDC(nullptr, scr);
    return out;
}

void Layout(Ed* e)
{
    int h = Sc(TOOLBAR_H), pad = Sc(6), bh = h - 2 * pad, x = pad;
    struct { HWND hw; int w; } b[] = { {e->bRect, 120}, {e->bZin, 90}, {e->bZout, 90}, {e->bUndo, 90}, {e->bSave, 90}, {e->bSaveAs, 120} };
    for (auto& i : b) { MoveWindow(i.hw, x, pad, Sc(i.w), bh, TRUE); x += Sc(i.w) + pad; }
}

HWND MakeBtn(Ed* e, const wchar_t* text, int id, DWORD style = 0)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, e->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)e->font, TRUE);
    return b;
}

void SetZoom(Ed* e, double z)
{
    if (z < 0.1) z = 0.1; if (z > 8.0) z = 8.0;
    e->zoom = z; UpdateScroll(e); InvalidateRect(e->hwnd, nullptr, FALSE);
}

bool SaveTo(Ed* e, const wchar_t* path, int format)
{
    HBITMAP r = Render(e);
    bool ok = SaveBitmap(r, path, format);
    DeleteObject(r);
    if (!ok) {
        wchar_t msg[MAX_PATH + 100];
        _snwprintf(msg, MAX_PATH + 100, T(S_ERR_SAVE), path);
        MessageBoxW(e->hwnd, msg, T(S_ED_TITLE), MB_OK | MB_ICONERROR);
    } else e->dirty = false;
    return ok;
}

void SaveAs(Ed* e)
{
    wchar_t file[MAX_PATH];
    wcsncpy(file, e->path, MAX_PATH - 1); file[MAX_PATH - 1] = 0;
    wchar_t filter[200];
    // "PNG\0*.png\0JPEG\0*.jpg\0BMP\0*.bmp\0\0"
    const wchar_t* parts[] = { L"PNG (*.png)", L"*.png", L"JPEG (*.jpg)", L"*.jpg", L"BMP (*.bmp)", L"*.bmp" };
    int n = 0;
    for (auto p : parts) { size_t l = wcslen(p) + 1; memcpy(filter + n, p, l * sizeof(wchar_t)); n += (int)l; }
    filter[n] = 0;
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = e->hwnd; ofn.lpstrFilter = filter; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.nFilterIndex = e->format == IMG_JPG ? 2 : e->format == IMG_BMP ? 3 : 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = T(S_ED_SAVE_AS);
    if (!GetSaveFileNameW(&ofn)) return;
    int fmt = ofn.nFilterIndex == 2 ? IMG_JPG : ofn.nFilterIndex == 3 ? IMG_BMP : IMG_PNG;
    const wchar_t* dot = wcsrchr(file, L'.');
    if (dot) {   // the extension typed wins over the filter
        if (!_wcsicmp(dot, L".jpg") || !_wcsicmp(dot, L".jpeg")) fmt = IMG_JPG;
        else if (!_wcsicmp(dot, L".bmp")) fmt = IMG_BMP;
        else if (!_wcsicmp(dot, L".png")) fmt = IMG_PNG;
    }
    if (SaveTo(e, file, fmt)) { wcscpy(e->path, file); e->format = fmt; }
}

LRESULT CALLBACK EdProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    Ed* e = (Ed*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        ((Ed*)((CREATESTRUCTW*)lp)->lpCreateParams)->hwnd = hw;
        return DefWindowProcW(hw, m, wp, lp);              // the default handling sets the title and the frame up
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        e->font = CreateFontIndirectW(&ncm.lfMessageFont);
        e->bRect  = MakeBtn(e, T(S_ED_RECT), ID_RECT, BS_AUTOCHECKBOX | BS_PUSHLIKE);
        e->bZin   = MakeBtn(e, T(S_ED_ZOOM_IN), ID_ZIN);
        e->bZout  = MakeBtn(e, T(S_ED_ZOOM_OUT), ID_ZOUT);
        e->bUndo  = MakeBtn(e, T(S_ED_UNDO), ID_UNDO);
        e->bSave  = MakeBtn(e, T(S_ED_SAVE), ID_SAVE);
        e->bSaveAs = MakeBtn(e, T(S_ED_SAVE_AS), ID_SAVEAS);
        SendMessageW(e->bRect, BM_SETCHECK, BST_CHECKED, 0);
        Layout(e);
        return 0; }
    case WM_SIZE: if (e) { Layout(e); UpdateScroll(e); InvalidateRect(hw, nullptr, FALSE); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        RECT c; CanvasRect(e, &c);
        int cw = c.right - c.left, ch = c.bottom - c.top;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP buf = CreateCompatibleBitmap(dc, cw > 0 ? cw : 1, ch > 0 ? ch : 1);
        HBITMAP ob = (HBITMAP)SelectObject(mem, buf);
        HBRUSH bg = CreateSolidBrush(RGB(58, 62, 72));
        RECT all = { 0, 0, cw, ch }; FillRect(mem, &all, bg); DeleteObject(bg);
        POINT o = ToWin(e, 0, 0); o.y -= c.top;
        int iw = (int)(e->w * e->zoom + 0.5), ih = (int)(e->h * e->zoom + 0.5);
        HDC src = CreateCompatibleDC(dc);
        HBITMAP os = (HBITMAP)SelectObject(src, e->bmp);
        SetStretchBltMode(mem, e->zoom < 1.0 ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        StretchBlt(mem, o.x, o.y, iw, ih, src, 0, 0, e->w, e->h, SRCCOPY);
        SelectObject(src, os); DeleteDC(src);
        DrawRects(mem, e, false, e->zoom, o.x, o.y);
        BitBlt(dc, 0, c.top, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(buf); DeleteDC(mem);
        // toolbar background
        RECT tb = { 0, 0, c.right, c.top };
        FillRect(dc, &tb, GetSysColorBrush(COLOR_BTNFACE));
        EndPaint(hw, &ps);
        return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ZIN: SetZoom(e, e->zoom * 1.25); break;
        case ID_ZOUT: SetZoom(e, e->zoom / 1.25); break;
        case ID_UNDO: if (e->nrects) { --e->nrects; e->dirty = true; InvalidateRect(hw, nullptr, FALSE); } break;
        case ID_SAVE: SaveTo(e, e->path, e->format); break;
        case ID_SAVEAS: SaveAs(e); break;
        }
        SetFocus(hw);
        return 0;
    case WM_LBUTTONDOWN: {
        RECT c; CanvasRect(e, &c);
        if (SendMessageW(e->bRect, BM_GETCHECK, 0, 0) == BST_CHECKED && (short)HIWORD(lp) >= c.top) {
            e->drawing = true; e->p0 = e->p1 = ToImg(e, (short)LOWORD(lp), (short)HIWORD(lp));
            SetCapture(hw);
        }
        return 0; }
    case WM_MOUSEMOVE:
        if (e->drawing) { e->p1 = ToImg(e, (short)LOWORD(lp), (short)HIWORD(lp)); InvalidateRect(hw, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (e->drawing) {
            ReleaseCapture();
            e->p1 = ToImg(e, (short)LOWORD(lp), (short)HIWORD(lp));
            RECT r; NormRect(e->p0, e->p1, &r);
            e->drawing = false;
            if (r.right - r.left >= 3 && r.bottom - r.top >= 3 && e->nrects < MAX_RECTS) { e->rects[e->nrects++] = r; e->dirty = true; }
            InvalidateRect(hw, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            SetZoom(e, e->zoom * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.25 : 0.8));
        } else { e->sy -= GET_WHEEL_DELTA_WPARAM(wp) / 2; UpdateScroll(e); InvalidateRect(hw, nullptr, FALSE); }
        return 0;
    case WM_HSCROLL: case WM_VSCROLL: {
        int bar = m == WM_HSCROLL ? SB_HORZ : SB_VERT;
        SCROLLINFO si; memset(&si, 0, sizeof(si)); si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        GetScrollInfo(hw, bar, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
            case SB_LINEUP: pos -= 30; break;
            case SB_LINEDOWN: pos += 30; break;
            case SB_PAGEUP: pos -= (int)si.nPage; break;
            case SB_PAGEDOWN: pos += (int)si.nPage; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        if (bar == SB_HORZ) e->sx = pos; else e->sy = pos;
        UpdateScroll(e); InvalidateRect(hw, nullptr, FALSE);
        return 0; }
    case WM_KEYDOWN:
        if (wp == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && e->nrects) { --e->nrects; InvalidateRect(hw, nullptr, FALSE); }
        else if (wp == VK_ESCAPE) DestroyWindow(hw);
        else if (wp == VK_ADD || wp == VK_OEM_PLUS) SetZoom(e, e->zoom * 1.25);
        else if (wp == VK_SUBTRACT || wp == VK_OEM_MINUS) SetZoom(e, e->zoom / 1.25);
        return 0;
    case WM_CLOSE:
        if (e->dirty) {
            int r = MessageBoxW(hw, T(S_ED_SAVE), T(S_ED_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !SaveTo(e, e->path, e->format)) return 0;
        }
        DestroyWindow(hw);
        return 0;
    case WM_NCDESTROY:
        if (e) {
            if (e->bmp) DeleteObject(e->bmp);
            if (e->font) DeleteObject(e->font);
            delete e;
            SetWindowLongPtrW(hw, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

} // namespace

void OpenEditor(HWND owner, HBITMAP bmp, const wchar_t* path, int format)
{
    static bool reg = false;
    if (!bmp) return;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = EdProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Click2ScreenEditor";
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc); reg = true;
    }
    Ed* e = new Ed; memset(e, 0, sizeof(Ed));
    BITMAP b; GetObjectW(bmp, sizeof(b), &b);
    e->bmp = bmp; e->w = b.bmWidth; e->h = b.bmHeight; e->format = format; e->zoom = 1.0;
    wcsncpy(e->path, path, MAX_PATH - 1);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int maxw = (wa.right - wa.left) * 8 / 10, maxh = (wa.bottom - wa.top) * 8 / 10 - Sc(TOOLBAR_H);
    if (e->w > maxw || e->h > maxh) {
        double kw = (double)maxw / e->w, kh = (double)maxh / e->h;
        e->zoom = kw < kh ? kw : kh;
    }
    int cw = (int)(e->w * e->zoom) + 2, ch = (int)(e->h * e->zoom) + 2 + Sc(TOOLBAR_H);
    if (cw < Sc(700)) cw = Sc(700);
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL, FALSE);
    HWND hw = CreateWindowExW(0, L"Click2ScreenEditor", T(S_ED_TITLE), WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                              GetModuleHandleW(nullptr), e);
    if (!hw) { DeleteObject(bmp); delete e; return; }
    UpdateScroll(e);
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
    (void)owner;
}

// ---- test hooks (c2sautotest.exe) ------------------------------------------------------------------------------
bool EditorTestAddRect(HWND hw, int l, int t, int r, int b)
{
    Ed* e = (Ed*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (!e || e->nrects >= MAX_RECTS) return false;
    RECT rc = { l, t, r, b };
    e->rects[e->nrects++] = rc;
    e->dirty = true;
    return true;
}

bool EditorTestSave(HWND hw)
{
    Ed* e = (Ed*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    return e && SaveTo(e, e->path, e->format);
}

int EditorTestRectCount(HWND hw)
{
    Ed* e = (Ed*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    return e ? e->nrects : -1;
}
