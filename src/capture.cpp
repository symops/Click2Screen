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

bool Grabber::Grab(const RECT& src, bool mouse, bool highlight, bool fx)
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
    if (ok && mouse && fx) FxDraw(m_dc, src, m_w, m_h, highlight);     // the red lines, and the click waves with the highlight option: in recordings, with the pointer, under it and its halo
    const bool halo = highlight && fx;                                 // the yellow circle round the pointer: in recordings only, never in a screenshot
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
            if (halo) {
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

// ---- picture formats ---------------------------------------------------------------------------------------------
// WIC encoders: PNG, JPEG, BMP, GIF, TIFF and JPEG XR ship with Windows; WebP comes with Windows 10 1809+ (the WebP image
// extension on older builds).
static const GUID kContainerWebp = { 0xe094b0e2, 0x67f2, 0x45b3, { 0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xf3 } };
static const GUID kContainerWmp  = { 0x57a37caa, 0x367a, 0x4540, { 0x91, 0x6b, 0xf1, 0x83, 0xc5, 0x09, 0x3a, 0x4b } };

struct ImageFmt { const wchar_t* name; const wchar_t* ext; const GUID* container; };
static const ImageFmt kImageFmts[IMG_COUNT] = {
    { L"PNG",  L"png",  &GUID_ContainerFormatPng },
    { L"JPG",  L"jpg",  &GUID_ContainerFormatJpeg },
    { L"BMP",  L"bmp",  &GUID_ContainerFormatBmp },
    { L"GIF",  L"gif",  &GUID_ContainerFormatGif },
    { L"TIFF", L"tif",  &GUID_ContainerFormatTiff },
    { L"WebP", L"webp", &kContainerWebp },
    { L"JXR",  L"jxr",  &kContainerWmp },
};

const wchar_t* ImageFormatName(int fmt) { return fmt >= 0 && fmt < IMG_COUNT ? kImageFmts[fmt].name : L"PNG"; }
const wchar_t* ImageFormatExt(int fmt) { return fmt >= 0 && fmt < IMG_COUNT ? kImageFmts[fmt].ext : L"png"; }

int ImageFormatFromPath(const wchar_t* path)
{
    const wchar_t* dot = path ? wcsrchr(path, L'.') : nullptr;
    if (!dot) return -1;
    ++dot;
    if (!_wcsicmp(dot, L"jpeg") || !_wcsicmp(dot, L"jpe")) return IMG_JPG;
    if (!_wcsicmp(dot, L"tiff")) return IMG_TIFF;
    if (!_wcsicmp(dot, L"wdp") || !_wcsicmp(dot, L"hdp")) return IMG_JXR;
    for (int i = 0; i < IMG_COUNT; ++i) if (!_wcsicmp(dot, kImageFmts[i].ext)) return i;
    return -1;
}

bool ImageFormatAvailable(int fmt)
{
    // Asked every time the options window is built, and every format loads its own codec libraries: the answer is kept (and asked once
    // at start-up from a thread of its own, see WarmFormats in main.cpp).
    static int cache[IMG_COUNT];                                     // 0 unknown, 1 yes, 2 no
    static SRWLOCK lock = SRWLOCK_INIT;
    if (fmt < 0 || fmt >= IMG_COUNT) return false;
    AcquireSRWLockExclusive(&lock);
    struct Unlock { SRWLOCK* l; ~Unlock() { ReleaseSRWLockExclusive(l); } } unlock = { &lock };
    if (cache[fmt]) return cache[fmt] == 1;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) && f) {
        IWICBitmapEncoder* enc = nullptr;
        if (SUCCEEDED(f->CreateEncoder(*kImageFmts[fmt].container, nullptr, &enc)) && enc) { ok = true; enc->Release(); }
        f->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    cache[fmt] = ok ? 1 : 2;
    return ok;
}

// ---- the icon of the tray for a state of the recording -----------------------------------------------------------------------------------------------------
// The pixels of the icon of the program are read (with their alpha), a badge with a white rim is laid in the lower right corner (smooth: 4 x 4 samples a pixel).
static void Badge(uint32_t* px, int W, int H, double cx, double cy, double r, int cr, int cg, int cb)
{
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int in = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
            const double dx = x + (sx + 0.5) / 4.0 - cx, dy = y + (sy + 0.5) / 4.0 - cy;
            if (dx * dx + dy * dy <= r * r) ++in;
        }
        if (!in) continue;
        const double a = in / 16.0;
        uint32_t& q = px[(size_t)y * W + x];
        const double da = ((q >> 24) & 255) / 255.0, oa = a + da * (1.0 - a);
        const double dr = (double)((q >> 16) & 255), dg = (double)((q >> 8) & 255), db = (double)(q & 255);
        const int nr = (int)((cr * a + dr * da * (1.0 - a)) / oa + 0.5), ng = (int)((cg * a + dg * da * (1.0 - a)) / oa + 0.5), nb = (int)((cb * a + db * da * (1.0 - a)) / oa + 0.5);
        q = ((uint32_t)(oa * 255.0 + 0.5) << 24) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
    }
}

HICON MakeStateIcon(HICON base, int state)
{
    if (!base || state <= 0) return base;
    ICONINFO ii; memset(&ii, 0, sizeof(ii));
    if (!GetIconInfo(base, &ii)) return base;
    BITMAP bm; memset(&bm, 0, sizeof(bm));
    HBITMAP ref = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    HICON out = base;
    if (ref && GetObject(ref, sizeof(bm), &bm) && bm.bmWidth >= 8) {
        const int W = bm.bmWidth, H = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
        uint32_t* px = (uint32_t*)calloc((size_t)W * H, 4);
        uint32_t* mk = (uint32_t*)calloc((size_t)W * H, 4);
        HDC dc = GetDC(nullptr);
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        if (px && mk && ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, H, px, &bi, DIB_RGB_COLORS)) {
            bool anyAlpha = false;
            for (size_t i = 0; i < (size_t)W * H; ++i) if (px[i] >> 24) { anyAlpha = true; break; }
            if (!anyAlpha && ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, H, mk, &bi, DIB_RGB_COLORS))      // an icon without alpha: the mask says what is see-through
                for (size_t i = 0; i < (size_t)W * H; ++i) px[i] = (px[i] & 0x00FFFFFFu) | ((mk[i] & 0x00FFFFFFu) ? 0u : 0xFF000000u);
            const double r = W * 0.20, cx = W - r - 0.5, cy = H - r - 0.5;
            Badge(px, W, H, cx, cy, r + (W >= 24 ? 1.2 : 0.8), 255, 255, 255);                          // the white rim
            switch (state) {
            case 1: Badge(px, W, H, cx, cy, r, 255, 190, 0); break;                                    // ready: amber
            case 2: Badge(px, W, H, cx, cy, r, 255, 110, 0); break;                                    // countdown: orange
            case 3: Badge(px, W, H, cx, cy, r, 220, 30, 30); break;                                    // recording: red
            default: {                                                                                  // paused: a dark disc, two white bars
                Badge(px, W, H, cx, cy, r, 60, 70, 90);
                const int bw = (int)(r * 0.38 + 0.5) > 1 ? (int)(r * 0.38 + 0.5) : 1, bh = (int)(r * 1.1 + 0.5);
                for (int k = 0; k < 2; ++k) {
                    const int x0 = (int)(cx + (k ? 0.18 : -0.18 - 0.38) * r + 0.5), y0 = (int)(cy - bh / 2.0 + 0.5);
                    for (int y = y0; y < y0 + bh; ++y) for (int x = x0; x < x0 + bw; ++x) if (x >= 0 && y >= 0 && x < W && y < H) px[(size_t)y * W + x] = 0xFFFFFFFFu;
                }
                break; }
            }
            uint32_t* dib = nullptr;
            HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&dib, nullptr, 0);
            HBITMAP mask = CreateBitmap(W, H, 1, 1, nullptr);
            if (color && dib && mask) {
                memcpy(dib, px, (size_t)W * H * 4);
                ICONINFO n; memset(&n, 0, sizeof(n)); n.fIcon = TRUE; n.hbmColor = color; n.hbmMask = mask;
                HICON ic = CreateIconIndirect(&n);
                if (ic) out = ic;
            }
            if (color) DeleteObject(color);
            if (mask) DeleteObject(mask);
        }
        ReleaseDC(nullptr, dc);
        free(px); free(mk);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

// ---- the watermark -------------------------------------------------------------------------------------------------------------------------------------------
// The mark is kept as premultiplied BGRA (top-down); laying it over a picture is one multiply-add per pixel of the mark.
// The mark is up to three parts, one under another: the text, the date and time (a template; the video shows the running clock), the picture.
struct Watermark { int w, h, x, y; uint32_t* px; Settings cfg; int picW, picH; uint32_t *txt, *dat, *img; int tw, th, dw, dh, iw, ih; int sec; };

static uint32_t* MarkFromText(const Settings& s, const wchar_t* text, int* mw, int* mh)
{
    HDC scr = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    LOGFONTW lf; memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -s.wmSize; lf.lfWeight = FW_BOLD; lf.lfQuality = ANTIALIASED_QUALITY; lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    const int W = sz.cx + 6, H = sz.cy + 6;
    uint32_t* out = nullptr;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    uint32_t* bits = nullptr;
    HBITMAP bm = sz.cx > 0 && sz.cy > 0 ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0) : nullptr;
    if (bm && bits) {
        HGDIOBJ ob = SelectObject(dc, bm);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 3, 3, text, (int)wcslen(text));
        GdiFlush();
        out = (uint32_t*)malloc((size_t)W * H * 4);
        if (out) {
            const int o = s.wmOpacity;                                            // 10..100
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const int cov = (int)(bits[(size_t)y * W + x] & 0xFF);            // the coverage of the text (white on black, smoothed)
                const int sh = x > 0 && y > 0 ? (int)(bits[(size_t)(y - 1) * W + (x - 1)] & 0xFF) : 0;       // the shadow: the same, a pixel to the lower right
                const int at = cov * o / 100;                                     // the text: dark grey (at any opacity)
                const int as = sh * o * 6 / 1000;                                 // the halo: white, a little weaker, so the text can be read on a dark picture too
                const int under = as * (255 - at) / 255;                          // the part of the halo that is not covered by the text
                const int a = at + under;
                const int c = at * 64 / 255 + under;                              // premultiplied: the grey 64 of the text + the white of the halo
                out[(size_t)y * W + x] = ((uint32_t)a << 24) | ((uint32_t)c << 16) | ((uint32_t)c << 8) | (uint32_t)c;
            }
            *mw = W; *mh = H;
        }
        SelectObject(dc, ob); DeleteObject(bm);
    }
    SelectObject(dc, of); DeleteObject(f); DeleteDC(dc);
    return out;
}

static uint32_t* MarkFromFile(const Settings& s, int picW, int picH, int* mw, int* mh)
{
    if (!s.wmImage[0]) return nullptr;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    uint32_t* out = nullptr;
    IWICImagingFactory* f = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr; IWICBitmapScaler* sc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) &&
        SUCCEEDED(f->CreateDecoderFromFilename(s.wmImage, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w0 = 0, h0 = 0; cv->GetSize(&w0, &h0);
        long long W = (long long)w0 * s.wmScale / 100, H = (long long)h0 * s.wmScale / 100;
        if (W > picW) { H = H * picW / W; W = picW; }                          // not bigger than the picture it goes on
        if (H > picH) { W = W * picH / H; H = picH; }
        if (W < 1) W = 1;
        if (H < 1) H = 1;
        IWICBitmapSource* src = cv;
        if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(cv, (UINT)W, (UINT)H, WICBitmapInterpolationModeFant))) src = sc;
        else { W = w0; H = h0; }
        out = (uint32_t*)malloc((size_t)W * H * 4);
        if (out && SUCCEEDED(src->CopyPixels(nullptr, (UINT)W * 4, (UINT)((size_t)W * H * 4), (BYTE*)out))) {
            const int o = s.wmOpacity;
            for (size_t i = 0; i < (size_t)W * H; ++i) {                       // the opacity: every channel of a premultiplied pixel
                uint32_t p = out[i];
                out[i] = ((((p >> 24) & 255) * o / 100) << 24) | ((((p >> 16) & 255) * o / 100) << 16) | ((((p >> 8) & 255) * o / 100) << 8) | ((p & 255) * o / 100);
            }
            *mw = (int)W; *mh = (int)H;
        } else { free(out); out = nullptr; }
    }
    if (sc) sc->Release();
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return out;
}

static int MarkSecond() { SYSTEMTIME t; GetLocalTime(&t); return (int)t.wSecond + 60 * (int)t.wMinute + 3600 * (int)t.wHour + 86400 * (int)t.wDay; }

// The parts one under another (4 pixels between), each at the side the place says; the mark goes to its place in the picture.
static void MarkCompose(Watermark* wm)
{
    free(wm->px); wm->px = nullptr; wm->w = wm->h = 0;
    struct P { uint32_t* px; int w, h; } parts[3] = { { wm->txt, wm->tw, wm->th }, { wm->dat, wm->dw, wm->dh }, { wm->img, wm->iw, wm->ih } };
    int W = 0, H = 0, n = 0;
    for (const P& q : parts) if (q.px) { if (q.w > W) W = q.w; H += q.h + (n ? 4 : 0); ++n; }
    if (!n) return;
    uint32_t* out = (uint32_t*)calloc((size_t)W * H, 4);
    if (!out) return;
    int y = 0;
    for (const P& q : parts) {
        if (!q.px) continue;
        const int x0 = wm->cfg.wmH == 0 ? 0 : wm->cfg.wmH == 2 ? W - q.w : (W - q.w) / 2;
        for (int r = 0; r < q.h; ++r) memcpy(out + (size_t)(y + r) * W + x0, q.px + (size_t)r * q.w, (size_t)q.w * 4);
        y += q.h + 4;
    }
    wm->px = out; wm->w = W; wm->h = H;
    const int picW = wm->picW, picH = wm->picH;
    const int margin = (picW < picH ? picW : picH) / 60 > 8 ? (picW < picH ? picW : picH) / 60 : 8;
    wm->x = wm->cfg.wmH == 0 ? margin : wm->cfg.wmH == 2 ? picW - W - margin : (picW - W) / 2;
    wm->y = wm->cfg.wmV == 0 ? margin : wm->cfg.wmV == 2 ? picH - H - margin : (picH - H) / 2;
    if (wm->x < 0) wm->x = 0;
    if (wm->y < 0) wm->y = 0;
}

static void MarkDate(Watermark* wm)
{
    free(wm->dat); wm->dat = nullptr; wm->dw = wm->dh = 0;
    wchar_t t[160]; WatermarkDateString(wm->cfg.wmDate, t, 160);
    if (t[0]) wm->dat = MarkFromText(wm->cfg, t, &wm->dw, &wm->dh);
    wm->sec = MarkSecond();
}

Watermark* WatermarkCreate(const Settings& s, int picW, int picH)
{
    if (picW < 8 || picH < 8) return nullptr;
    Watermark* wm = new Watermark;
    memset(wm, 0, sizeof(*wm));
    wm->cfg = s; wm->picW = picW; wm->picH = picH;
    if ((s.wmParts & WMP_TEXT) && s.wmText[0]) wm->txt = MarkFromText(s, s.wmText, &wm->tw, &wm->th);
    if (s.wmParts & WMP_DATE) MarkDate(wm);
    if (s.wmParts & WMP_IMAGE) wm->img = MarkFromFile(s, picW, picH, &wm->iw, &wm->ih);
    MarkCompose(wm);
    if (!wm->px) { WatermarkFree(wm); return nullptr; }
    return wm;
}

void WatermarkApply(Watermark* wm, uint8_t* bits, int w, int h)
{
    if (!wm || !bits) return;
    if (wm->dat && MarkSecond() != wm->sec) { MarkDate(wm); MarkCompose(wm); if (!wm->px) return; }      // the clock ticks
    for (int r = 0; r < wm->h; ++r) {
        const int dy = wm->y + r;
        if (dy >= h) break;
        uint8_t* row = bits + (size_t)(h - 1 - dy) * w * 4;                    // the picture is bottom-up
        const uint32_t* mp = wm->px + (size_t)r * wm->w;
        for (int c = 0; c < wm->w; ++c) {
            const int dx = wm->x + c;
            if (dx >= w) break;
            const uint32_t p = mp[c];
            const unsigned a = p >> 24;
            if (!a) continue;
            uint8_t* d = row + (size_t)dx * 4;
            const unsigned ia = 255 - a;
            d[0] = (uint8_t)((p & 255) + d[0] * ia / 255);
            d[1] = (uint8_t)(((p >> 8) & 255) + d[1] * ia / 255);
            d[2] = (uint8_t)(((p >> 16) & 255) + d[2] * ia / 255);
        }
    }
}

void WatermarkFree(Watermark* wm)
{
    if (!wm) return;
    free(wm->px); free(wm->txt); free(wm->dat); free(wm->img);
    delete wm;
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
    IWICPalette* pal = nullptr;
    IWICFormatConverter* conv = nullptr;
    HRESULT h = S_OK;
    if (format < 0 || format >= IMG_COUNT) format = IMG_PNG;
#define STEP(name, expr) do { *step = L"" name; h = (expr); if (FAILED(h)) { *hr = h; goto done; } } while (0)
    STEP("factory", CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f));
    STEP("bitmap from HBITMAP", f->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wb));
    STEP("stream", f->CreateStream(&st));
    STEP("open the file", st->InitializeFromFilename(path, GENERIC_WRITE));
    STEP("encoder", f->CreateEncoder(*kImageFmts[format].container, nullptr, &enc));
    STEP("encoder init", enc->Initialize(st, WICBitmapEncoderNoCache));
    STEP("frame", enc->CreateNewFrame(&fr, &props));
    if ((format == IMG_JPG || format == IMG_WEBP) && props) {
        PROPBAG2 opt; memset(&opt, 0, sizeof(opt));
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.92f;
        props->Write(1, &opt, &v);                       // (an encoder without this property just ignores it)
    }
    STEP("frame init", fr->Initialize(props));
    {
        UINT w = 0, hh = 0; wb->GetSize(&w, &hh);
        STEP("frame size", fr->SetSize(w, hh));
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (format == IMG_GIF) pf = GUID_WICPixelFormat8bppIndexed;
        STEP("pixel format", fr->SetPixelFormat(&pf));
        if (format == IMG_GIF) {                                      // GIF has 256 colours: a palette made from the picture
            STEP("palette", f->CreatePalette(&pal));
            {   // counted on a copy a third of the size on each side (nine times fewer pixels, the same colours)
                IWICBitmapScaler* sc = nullptr;
                bool done = false;
                if (w >= 90 && hh >= 90 && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
                    SUCCEEDED(sc->Initialize(wb, w / 3, hh / 3, WICBitmapInterpolationModeNearestNeighbor)))
                    done = SUCCEEDED(pal->InitializeFromBitmap(sc, 256, FALSE));
                if (sc) sc->Release();
                if (!done) STEP("palette from the picture", pal->InitializeFromBitmap(wb, 256, FALSE));
            }
            STEP("set the palette", fr->SetPalette(pal));
            STEP("converter", f->CreateFormatConverter(&conv));
            STEP("convert to the palette", conv->Initialize(wb, GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone, pal, 0.0, WICBitmapPaletteTypeCustom));
        }
    }
    STEP("write the picture", fr->WriteSource(conv ? (IWICBitmapSource*)conv : (IWICBitmapSource*)wb, nullptr));
    STEP("frame commit", fr->Commit());
    STEP("file commit", enc->Commit());
    ok = true;
done:
#undef STEP
    if (conv) conv->Release();
    if (pal) pal->Release();
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

void FitSizeForVideo(int vc, int srcW, int srcH, int& w, int& h, int quality)
{
    FitSize(srcW, srcH, w, h);
    if (vc == VC_GIF) {                              // a GIF has no inter-frame compression: smaller pictures, the lower the quality the smaller
        const int mw = quality <= 0 ? 960 : quality == 1 ? 800 : 640, mh = mw * 9 / 16;
        if (w > mw) { h = h * mw / w; w = mw; }
        if (h > mh) { w = w * mh / h; h = mh; }
        w &= ~1; h &= ~1;
        if (w < 2) w = 2;
        if (h < 2) h = 2;
    }
}
