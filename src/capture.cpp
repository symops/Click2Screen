// Screen capture: the GDI path (BitBlt from the screen DC with CAPTUREBLT, pointer drawn on top), frame size / bitrate
// rules, saving a bitmap as PNG / JPEG / BMP through WIC.
#include "c2s.h"
#include <wincodec.h>
#include <dwmapi.h>
#include <wchar.h>
#include <math.h>

RECT VirtualScreen()
{
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

void FitSize(int srcW, int srcH, int& outW, int& outH)
{
    double w = srcW < 2 ? 2 : srcW, h = srcH < 2 ? 2 : srcH;
    double k = 1.0;
    if (w > 1920.0) k = 1920.0 / w;
    if (h * k > 1080.0) k = 1080.0 / h;
    int ow = (int)floor(w * k + 0.5), oh = (int)floor(h * k + 0.5);
    ow = (ow + 1) & ~1; oh = (oh + 1) & ~1;
    if (ow > 1920) ow = 1920;
    if (oh > 1080) oh = 1080;
    if (ow < 2) ow = 2;
    if (oh < 2) oh = 2;
    outW = ow; outH = oh;
}

int AutoBitrate(int w, int h)
{
    // The five anchors of the table found in the original recorder (analysis/FINDINGS.md section 4): bit rate for
    // 426x240, 640x360, 854x480, 1280x720, 1920x1080. The original took the first entry the frame is strictly smaller
    // than in both sides and scaled it by the area ratio, which makes the rate jump at every anchor (and leaves exactly
    // 1920x1080 on a 2 Mbit/s fallback). Here the rate follows the pixel count linearly between the anchors: it is the
    // table value at each anchor, proportional to the area below the first one, and capped by the last one.
    static const struct { double area; int bps; } A[5] = {
        { 426.0 * 240, 384000 }, { 640.0 * 360, 768000 }, { 854.0 * 480, 1500000 }, { 1280.0 * 720, 2000000 }, { 1920.0 * 1080, 4000000 }
    };
    double area = (double)w * (double)h;
    if (area <= 0) return A[0].bps;
    if (area <= A[0].area) return (int)(A[0].bps * area / A[0].area);
    for (int i = 1; i < 5; ++i)
        if (area <= A[i].area) {
            double t = (area - A[i - 1].area) / (A[i].area - A[i - 1].area);
            return (int)(A[i - 1].bps + t * (A[i].bps - A[i - 1].bps));
        }
    return A[4].bps;
}

// ---- Grabber ---------------------------------------------------------------------------------------------------
Grabber::Grabber() : m_dc(nullptr), m_bmp(nullptr), m_old(nullptr), m_bits(nullptr), m_w(0), m_h(0),
                     m_halo(nullptr), m_haloBmp(nullptr), m_haloOld(nullptr) {}
Grabber::~Grabber() { Close(); }

void Grabber::Close()
{
    if (m_dc) {
        if (m_old) SelectObject(m_dc, m_old);
        DeleteDC(m_dc);
    }
    if (m_bmp) DeleteObject(m_bmp);
    if (m_halo) {
        if (m_haloOld) SelectObject(m_halo, m_haloOld);
        DeleteDC(m_halo);
    }
    if (m_haloBmp) DeleteObject(m_haloBmp);
    m_dc = nullptr; m_bmp = nullptr; m_old = nullptr; m_bits = nullptr; m_w = m_h = 0;
    m_halo = nullptr; m_haloBmp = nullptr; m_haloOld = nullptr;
}

bool Grabber::Init(int outW, int outH)
{
    Close();
    if (outW < 1 || outH < 1) return false;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = outW;
    bi.bmiHeader.biHeight = outH;        // bottom-up, the default layout of Media Foundation RGB32
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    m_bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, (void**)&m_bits, nullptr, 0);
    m_dc = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    if (!m_bmp || !m_dc || !m_bits) { Close(); return false; }
    m_old = (HBITMAP)SelectObject(m_dc, m_bmp);
    m_w = outW; m_h = outH;
    memset(m_bits, 0, (size_t)outW * outH * 4);
    return true;
}

HBITMAP Grabber::Release()
{
    HBITMAP b = m_bmp;
    if (m_dc) {
        if (m_old) SelectObject(m_dc, m_old);
        DeleteDC(m_dc);
    }
    m_dc = nullptr; m_old = nullptr; m_bmp = nullptr; m_bits = nullptr;
    return b;
}

// A soft yellow disc, premultiplied alpha, drawn lazily and reused.
static void BuildHalo(HDC ref, HDC& dc, HBITMAP& bmp, HBITMAP& old)
{
    const int N = 64;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = N; bi.bmiHeader.biHeight = -N; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    uint32_t* px = nullptr;
    bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    dc = CreateCompatibleDC(ref);
    if (!bmp || !dc || !px) return;
    old = (HBITMAP)SelectObject(dc, bmp);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            double dx = x + 0.5 - N / 2.0, dy = y + 0.5 - N / 2.0, d = sqrt(dx * dx + dy * dy) / (N / 2.0);
            double a = d >= 1.0 ? 0.0 : (d > 0.8 ? (1.0 - d) / 0.2 : 1.0) * 0.38;
            unsigned A = (unsigned)(a * 255.0), R = A * 255 / 255, G = A * 220 / 255, B = 0;
            px[y * N + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
}

bool Grabber::Grab(const RECT& src, bool mouse, bool highlight)
{
    int sw = src.right - src.left, sh = src.bottom - src.top;
    if (!m_dc || sw <= 0 || sh <= 0) return false;
    HDC scr = GetDC(nullptr);
    if (!scr) return false;
    BOOL ok;
    if (sw == m_w && sh == m_h) {
        ok = BitBlt(m_dc, 0, 0, m_w, m_h, scr, src.left, src.top, SRCCOPY | CAPTUREBLT);
    } else {
        SetStretchBltMode(m_dc, HALFTONE);
        SetBrushOrgEx(m_dc, 0, 0, nullptr);
        ok = StretchBlt(m_dc, 0, 0, m_w, m_h, scr, src.left, src.top, sw, sh, SRCCOPY | CAPTUREBLT);
    }
    if (ok && mouse) {
        CURSORINFO ci; memset(&ci, 0, sizeof(ci)); ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
            double sx = (double)m_w / sw, sy = (double)m_h / sh;
            ICONINFO ii; memset(&ii, 0, sizeof(ii));
            int hx = 0, hy = 0;
            if (GetIconInfo(ci.hCursor, &ii)) {
                hx = (int)ii.xHotspot; hy = (int)ii.yHotspot;
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
            }
            double px = (ci.ptScreenPos.x - src.left) * sx, py = (ci.ptScreenPos.y - src.top) * sy;
            if (highlight) {
                if (!m_halo) BuildHalo(scr, m_halo, m_haloBmp, m_haloOld);
                if (m_halo) {
                    int r = (int)(26 * (sx < sy ? sx : sy) + 0.5); if (r < 8) r = 8;
                    BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
                    AlphaBlend(m_dc, (int)px - r, (int)py - r, 2 * r, 2 * r, m_halo, 0, 0, 64, 64, bf);
                }
            }
            int cw = GetSystemMetrics(SM_CXCURSOR), ch = GetSystemMetrics(SM_CYCURSOR);
            cw = (int)(cw * sx + 0.5); ch = (int)(ch * sy + 0.5);
            DrawIconEx(m_dc, (int)(px - hx * sx), (int)(py - hy * sy), ci.hCursor, cw, ch, 0, nullptr, DI_NORMAL);
        }
    }
    ReleaseDC(nullptr, scr);
    return ok != FALSE;
}

HBITMAP CaptureRect(const RECT& rc, bool mouse, bool highlight)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 1 || h < 1) return nullptr;
    Grabber g;
    if (!g.Init(w, h) || !g.Grab(rc, mouse, highlight)) return nullptr;
    return g.Release();
}

bool WindowVisibleRect(HWND hw, RECT* rc)
{
    RECT r;
    if (SUCCEEDED(DwmGetWindowAttribute(hw, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &r, sizeof(r))) && r.right > r.left) {
        *rc = r; return true;
    }
    if (GetWindowRect(hw, &r)) { *rc = r; return true; }
    return false;
}

// ---- saving ----------------------------------------------------------------------------------------------------
// One attempt; `step` names where it failed and `hr` says why.
static bool SaveBitmapOnce(HBITMAP bmp, const wchar_t* path, int format, const wchar_t** step, HRESULT* hr)
{
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICBitmap* wb = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    HRESULT h = S_OK;
#define STEP(name, expr) do { *step = L"" name; h = (expr); if (FAILED(h)) { *hr = h; goto done; } } while (0)
    STEP("factory", CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
    STEP("bitmap from HBITMAP", f->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wb));
    STEP("stream", f->CreateStream(&st));
    STEP("open the file", st->InitializeFromFilename(path, GENERIC_WRITE));
    {
        const GUID& cf = format == IMG_JPG ? GUID_ContainerFormatJpeg : format == IMG_BMP ? GUID_ContainerFormatBmp : GUID_ContainerFormatPng;
        STEP("encoder", f->CreateEncoder(cf, nullptr, &enc));
    }
    STEP("encoder init", enc->Initialize(st, WICBitmapEncoderNoCache));
    STEP("frame", enc->CreateNewFrame(&fr, &props));
    if (format == IMG_JPG && props) {
        PROPBAG2 opt; memset(&opt, 0, sizeof(opt));
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.92f;
        props->Write(1, &opt, &v);
    }
    STEP("frame init", fr->Initialize(props));
    {
        UINT w = 0, hh = 0; wb->GetSize(&w, &hh);
        STEP("frame size", fr->SetSize(w, hh));
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        STEP("pixel format", fr->SetPixelFormat(&pf));
    }
    STEP("write the picture", fr->WriteSource(wb, nullptr));
    STEP("frame commit", fr->Commit());
    STEP("file commit", enc->Commit());
    ok = true;
done:
#undef STEP
    if (props) props->Release();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (wb) wb->Release();
    if (f) f->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

bool SaveBitmap(HBITMAP bmp, const wchar_t* path, int format)
{
    if (!bmp) return false;
    // A freshly created file can be held for a moment by a virus scanner or an indexer: try a few times.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const wchar_t* step = L"";
        HRESULT hr = S_OK;
        if (SaveBitmapOnce(bmp, path, format, &step, &hr)) return true;
        Log(L"save %s: attempt %d failed at '%s' (0x%08lX)", path, attempt, step, (unsigned long)hr);
        DeleteFileW(path);
        if (attempt < 3) Sleep(150 * attempt);
    }
    return false;
}
