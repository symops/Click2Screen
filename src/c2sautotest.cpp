// c2sautotest.exe [minutes] [--seed N] [--keep] [--installer] — randomized end-to-end test of Click2Screen.
//
// Every test runs with random parameters drawn from one seeded generator (the seed is printed first; the same seed
// repeats the same sequence of tests and parameters). A test window of four coloured quarters is shown on the screen and
// everything is checked as a user would see it: the screenshot / recording of a random part of it must have the right
// size, orientation and colours, saved files must decode again, the settings must survive the registry and the .ini
// export, the file names must follow the template, every language table must be complete, the region / window pickers
// must return what the "mouse" selected, the editor must save the rectangles it drew, and the recorder must produce an
// MP4 whose size, duration, frame count and picture match what was recorded (pauses, a moving window).
// With --installer the installer is also run through a random install / update / remove cycle (administrator).
//
// PASS / FAIL / WARN / SKIP go to the console and to %ProgramData%\Click2Screen\logs\autotest.log; exit code 1 if
// anything failed. Settings are kept in HKCU\Software\Click2Screen-Autotest (removed at the end), files in
// %TEMP%\c2sautotest-<pid> (removed unless --keep). WARN: something the environment may not offer (no microphone, a
// hotkey taken, no H.264 decoder); SKIP: not possible here (Media Foundation encoder under Wine).
#include "c2s.h"
#include "uiids.h"
#include <mmsystem.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <mferror.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

void AppLog(const wchar_t* fmt, ...);          // applog.cpp

// ---- output ------------------------------------------------------------------------------------------------------
static int g_pass, g_fail, g_warn, g_skip;
static bool g_wine;
static int g_scale = 1;                // waits for the real program are multiplied by this (slow machines, low priority)
#define WMS(x) ((x) * g_scale)
static wchar_t g_dir[MAX_PATH];                // working folder

static void Emit(const wchar_t* line)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, w;
    wchar_t buf[900];
    _snwprintf(buf, 896, L"%s\r\n", line);
    buf[896] = 0;
    if (GetConsoleMode(out, &mode)) WriteConsoleW(out, buf, (DWORD)wcslen(buf), &w, nullptr);
    else {
        char u8[2800];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
        if (n > 1) WriteFile(out, u8, n - 1, &w, nullptr);
    }
    AppLog(L"%ls", line);
}

static void Rep(const wchar_t* level, const wchar_t* fmt, ...)
{
    wchar_t msg[700], line[760];
    va_list ap; va_start(ap, fmt); _vsnwprintf(msg, 690, fmt, ap); va_end(ap);
    msg[690] = 0;
    _snwprintf(line, 750, L"%-5s %s", level, msg);
    Emit(line);
}
#define PASS(...) do { ++g_pass; Rep(L"PASS", __VA_ARGS__); } while (0)
#define FAIL(...) do { ++g_fail; Rep(L"FAIL", __VA_ARGS__); } while (0)
#define WARN(...) do { ++g_warn; Rep(L"WARN", __VA_ARGS__); } while (0)
#define SKIP(...) do { ++g_skip; Rep(L"SKIP", __VA_ARGS__); } while (0)
#define INFO(...) Rep(L"INFO", __VA_ARGS__)
// The program's own log (its last lines) goes into the report when a check about the real program fails.
static void DumpProgramLog()
{
    static const GUID kProgramData = { 0x62ab5d82, 0xfdc1, 0x4dc3, { 0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97 } };
    PWSTR pd = nullptr;
    if (FAILED(SHGetKnownFolderPath(kProgramData, 0, nullptr, &pd)) || !pd) return;
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\Click2Screen\\logs\\panel.log", pd);
    CoTaskMemFree(pd);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    LONGLONG want = sz.QuadPart > 4000 ? 4000 : sz.QuadPart;
    LARGE_INTEGER pos; pos.QuadPart = sz.QuadPart - want;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    char buf[4100]; DWORD got = 0;
    ReadFile(h, buf, (DWORD)want, &got, nullptr);
    CloseHandle(h);
    buf[got] = 0;
    wchar_t* wide = new wchar_t[got + 1];
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, got, wide, got);
    wide[n > 0 ? n : 0] = 0;
    // the last 14 lines
    int lines = 0; wchar_t* start = wide + wcslen(wide);
    while (start > wide && lines < 14) { --start; if (*start == L'\n') ++lines; }
    if (*start == L'\n') ++start;
    Rep(L"INFO", L"  --- the program's log (last lines of %s) ---", path);
    for (wchar_t* line = start; *line;) {
        wchar_t* e = wcschr(line, L'\n');
        if (e) *e = 0;
        int l = (int)wcslen(line); while (l > 0 && (line[l - 1] == L'\r')) line[--l] = 0;
        if (*line) Rep(L"INFO", L"  | %s", line);
        if (!e) break;
        line = e + 1;
    }
    delete[] wide;
}

static void Check(bool ok, const wchar_t* name, const wchar_t* detail = L"")
{
    if (ok) { ++g_pass; Rep(L"PASS", L"%s %s", name, detail); return; }
    ++g_fail; Rep(L"FAIL", L"%s %s", name, detail);
    if (!wcsncmp(name, L"program", 7) || !wcsncmp(name, L"monkey", 6) || !wcsncmp(name, L"installer window", 16)) DumpProgramLog();
}

// ---- random numbers ----------------------------------------------------------------------------------------------
static uint64_t g_state;
static uint64_t NextU()
{
    g_state ^= g_state >> 12; g_state ^= g_state << 25; g_state ^= g_state >> 27;
    return g_state * 2685821657736338717ULL;
}
static int R(int lo, int hi) { return lo + (int)(NextU() % (uint64_t)(hi - lo + 1)); }
static bool Chance(int pct) { return R(1, 100) <= pct; }

// ---- helpers -----------------------------------------------------------------------------------------------------
static void Pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(5);
    } while ((int)(end - GetTickCount()) > 0);
}

static unsigned long long FileSz(const wchar_t* p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static bool Same(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b) == 0; }

static void RandomText(wchar_t* out, int maxLen)
{
    static const wchar_t* alphabets[] = { L"abcdefghijklmnopqrstuvwxyz0123456789 _-", L"\x0430\x0431\x0432\x0433\x0434\x0435\x0436\x0437 -_", L"\x4e2d\x6587\x6587\x4ef6\x540d\x79f0 -", L"\x00e4\x00f6\x00fc\x00df\x00e9\x00e8 -_" };
    const wchar_t* al = alphabets[R(0, 3)];
    int n = (int)wcslen(al), len = R(1, maxLen);
    for (int i = 0; i < len; ++i) out[i] = al[R(0, n - 1)];
    out[len] = 0;
    // no trailing blanks (folders / names do not keep them)
    while (len > 0 && (out[len - 1] == L' ' || out[len - 1] == L'.')) out[--len] = 0;
    if (!len) wcscpy(out, L"x");
}

// Picture / video formats: the ones this Windows can write; lossy ones are compared with a tolerance.
static bool LossyFmt(int f) { return f == IMG_JPG || f == IMG_WEBP || f == IMG_JXR; }
static int FmtTol(int f, int exact) { return LossyFmt(f) ? 28 : f == IMG_GIF ? 40 : exact; }
static int RandomImageFormat()
{
    int ok[IMG_COUNT], n = 0;
    for (int i = 0; i < IMG_COUNT; ++i) if (ImageFormatAvailable(i)) ok[n++] = i;
    return n ? ok[R(0, n - 1)] : IMG_PNG;
}
static int RandomVideoFormat()
{
    int ok[VC_COUNT], n = 0;
    for (int i = 0; i < VC_COUNT; ++i) if (VideoFormatAvailable(i)) ok[n++] = i;
    return n ? ok[R(0, n - 1)] : VC_H264_BEST;
}
static void SelData(HWND cb, int data)       // a format combo lists only the formats this Windows can write; the item data is the format
{
    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0), pick = 0;
    for (int i = 0; i < n; ++i) if ((int)SendMessageW(cb, CB_GETITEMDATA, i, 0) == data) pick = i;
    SendMessageW(cb, CB_SETCURSEL, pick, 0);
}

// Four different hotkeys, each usable or switched off. plain: function keys only (the test can press them without holding a modifier).
static void RandomHotkeys(int* hk, bool plain)
{
    for (int i = 0; i < HKI_COUNT; ++i) {
        for (;;) {
            int c;
            if (Chance(15)) c = 0;
            else if (plain || Chance(30)) { c = VK_F1 + R(0, 23); if (c == VK_F12) c = VK_F13; }
            else {
                static const UINT mods[] = { MOD_CONTROL, MOD_ALT, MOD_SHIFT, MOD_CONTROL | MOD_ALT, MOD_ALT | MOD_SHIFT, MOD_CONTROL | MOD_SHIFT | MOD_WIN, MOD_WIN };
                c = (int)((mods[R(0, 6)] << 16) | (Chance(50) ? 'A' + R(0, 25) : Chance(50) ? '0' + R(0, 9) : VK_SNAPSHOT));
            }
            bool dup = false;
            for (int j = 0; j < i; ++j) if (c && hk[j] == c) dup = true;
            if (dup) continue;
            hk[i] = c; break;
        }
    }
}

static void RandomSettings(Settings& s)
{
    SettingsDefaults(s);
    wchar_t t[40];
    RandomText(t, 20); _snwprintf(s.folder, MAX_PATH, L"%s\\out %s", g_dir, t);
    wchar_t name[40]; RandomText(name, 12);
    switch (R(0, 3)) {
        case 0: _snwprintf(s.fileName, 128, L"%s $Y-$M-$D $H-$N-$S", name); break;
        case 1: _snwprintf(s.fileName, 128, L"Img%s", L"####"); break;
        case 2: _snwprintf(s.fileName, 128, L"%s_#_$D", name); break;
        default: _snwprintf(s.fileName, 128, L"%%USERNAME%% %s ##", name); break;
    }
    s.imageFormat = RandomImageFormat(); s.capMouse = Chance(50); s.highlight = Chance(50); s.sounds = Chance(50); s.notify = Chance(50); s.loopback = Chance(50); s.mic = Chance(50);
    s.openFolder = Chance(50); s.runAtStartup = false;   // autostart is not touched by the test
    s.delaySec = R(0, 10);
    s.videoCodec = RandomVideoFormat(); s.audioCodec = R(0, 1); s.videoQuality = R(0, 2); s.audioQuality = R(0, 2);
    static const int f[] = { 5, 10, 15, 20, 25, 30, 45, 60 };
    s.fps = f[R(0, 7)];
    s.language = R(0, LangCount());
    RandomHotkeys(s.hotkey, false);
}

static bool SettingsEqual(const Settings& a, const Settings& b, bool withLanguage, wchar_t* why)
{
#define CMPS(f) if (!Same(a.f, b.f)) { _snwprintf(why, 150, L"%s differs: [%s] vs [%s]", L"" #f, a.f, b.f); return false; }
#define CMPI(f) if (a.f != b.f) { _snwprintf(why, 120, L"%s: %d vs %d", L"" #f, (int)a.f, (int)b.f); return false; }
    CMPS(folder) CMPS(fileName) CMPI(imageFormat) CMPI(capMouse) CMPI(highlight) CMPI(sounds) CMPI(notify) CMPI(loopback) CMPI(mic)
    CMPI(openFolder) CMPI(delaySec) CMPI(videoCodec) CMPI(audioCodec)
    CMPI(videoQuality) CMPI(audioQuality) CMPI(fps)
    for (int i = 0; i < HKI_COUNT; ++i) CMPI(hotkey[i])
    if (withLanguage) CMPI(language)
    return true;
}

// ---- the test window: four coloured quarters + a moving strip -------------------------------------------------------
struct Fixture { HWND hw; int w, h; };
static Fixture g_fx;
static int g_phase;
static const COLORREF kQuad[4] = { RGB(255, 0, 0), RGB(0, 200, 0), RGB(0, 0, 255), RGB(255, 255, 0) };   // TL TR BL BR

static LRESULT CALLBACK FxProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        RECT c; GetClientRect(hw, &c);
        int w = c.right, h = c.bottom, hw2 = w / 2, hh2 = h / 2;
        RECT q[4] = { {0, 0, hw2, hh2}, {hw2, 0, w, hh2}, {0, hh2, hw2, h}, {hw2, hh2, w, h} };
        for (int i = 0; i < 4; ++i) { HBRUSH b = CreateSolidBrush(kQuad[i]); FillRect(dc, &q[i], b); DeleteObject(b); }
        int g = 40 + (g_phase * 37) % 200;
        HBRUSH sb = CreateSolidBrush(RGB(g, g, g));
        RECT st = { 0, h - 10, w, h }; FillRect(dc, &st, sb); DeleteObject(sb);
        EndPaint(hw, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: ++g_phase; { RECT c; GetClientRect(hw, &c); RECT st = { 0, c.bottom - 10, c.right, c.bottom }; InvalidateRect(hw, &st, FALSE); } return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

static RECT FxScreenRect();
static COLORREF Expected(int lx, int ly, int fw, int fh);
// The Start menu (or the search flyout) may be open on Windows 11 when the tests begin, and it lies over the fixture: every colour is then the
// colour of the menu. A flyout of the shell in front is closed with Esc; it is told by the process that owns the foreground window.
static bool ShellFlyoutInFront()
{
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = L""; DWORD n = MAX_PATH;
    bool r = false;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* nm = wcsrchr(path, L'\\'); nm = nm ? nm + 1 : path;
        r = !_wcsicmp(nm, L"StartMenuExperienceHost.exe") || !_wcsicmp(nm, L"SearchHost.exe") || !_wcsicmp(nm, L"ShellExperienceHost.exe") || !_wcsicmp(nm, L"SearchApp.exe");
    }
    CloseHandle(h);
    return r;
}
static void CloseStartMenu()
{
    for (int i = 0; i < 3 && ShellFlyoutInFront(); ++i) {
        Rep(L"INFO", L"  a flyout of the shell (the Start menu?) is in front: closed with Esc");
        keybd_event(VK_ESCAPE, 0, 0, 0); keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        Sleep(500);
    }
}
static void MakeFixture(int w, int h)
{
    CloseStartMenu();
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = FxProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"C2SAutotestFixture";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc); reg = true;
    }
    if (g_fx.hw) DestroyWindow(g_fx.hw);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
    if (w > ww - 20) w = ww - 20;
    if (h > wh - 20) h = wh - 20;
    int x = wa.left + R(0, ww - w), y = wa.top + R(0, wh - h);
    g_fx.w = w; g_fx.h = h;
    g_fx.hw = CreateWindowExW(WS_EX_TOPMOST, L"C2SAutotestFixture", L"c2s fixture", WS_POPUP | WS_VISIBLE, x, y, w, h,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    { int dontRound = 1; DwmSetWindowAttribute(g_fx.hw, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &dontRound /*DWMWCP_DONOTROUND*/, sizeof(dontRound)); }   // Windows 11 rounds the corners of every window: the colour points near them would see the desktop
    SetTimer(g_fx.hw, 1, 40, nullptr);
    UpdateWindow(g_fx.hw);
    Pump(250);
    // The window is on the screen only when the desktop shows its colours: Windows 11 composes a new window a moment after it is shown,
    // and a capture taken before that sees what was behind it. Wait for a pixel of each of two quarters (up to a few seconds).
    {
        RECT fr = FxScreenRect();
        DWORD until = GetTickCount() + WMS(1500);
        for (;;) {
            HDC dc = GetDC(nullptr); bool ok = true;
            if (dc) {
                const int px[2][2] = { { w / 4, h / 4 }, { w * 3 / 4, h * 3 / 4 } };
                for (int i = 0; i < 2 && ok; ++i) {
                    COLORREF e = Expected(px[i][0], px[i][1], w, h), c = GetPixel(dc, fr.left + px[i][0], fr.top + px[i][1]);
                    if (c == CLR_INVALID) continue;
                    if (abs((int)GetRValue(c) - (int)GetRValue(e)) + abs((int)GetGValue(c) - (int)GetGValue(e)) + abs((int)GetBValue(c) - (int)GetBValue(e)) > 60) ok = false;
                }
                ReleaseDC(nullptr, dc);
            }
            if (ok || (int)(until - GetTickCount()) <= 0) break;
            Pump(100);
        }
    }
}

static RECT FxScreenRect() { RECT r; GetWindowRect(g_fx.hw, &r); return r; }

static COLORREF Expected(int lx, int ly, int fw, int fh)     // colour of the fixture at its local point
{
    return kQuad[(ly >= fh / 2 ? 2 : 0) + (lx >= fw / 2 ? 1 : 0)];
}
static bool SafePoint(int lx, int ly, int fw, int fh)        // away from the quarter borders and the strip
{
    if (abs(lx - fw / 2) < 8 || abs(ly - fh / 2) < 8) return false;
    if ((lx < 14 || lx >= fw - 14) && (ly < 14 || ly >= fh - 24)) return false;          // not in the corners (Windows 11 rounds them, the shadow of a window may reach in)
    return ly < fh - 16 && lx >= 2 && lx < fw - 2 && ly >= 2;
}
// A line of the gradient (any colour) is found by comparing with the same frame without it: a pixel of the line or of its dark outline
// within three rows of (x, y) that differs a lot. (Frames are bottom-up DIBs of width w and height h.)
static DWORD AffinityOf(HWND w)
{
    typedef BOOL(WINAPI* GetAff)(HWND, DWORD*);
    GetAff g = (GetAff)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDisplayAffinity");
    DWORD v = 0xFFFFFFFF;
    if (g && w) g(w, &v);
    return v;
}
static bool AffinityReported()                                    // does this Windows keep and report the exclusion from capture? (Wine does not)
{
    typedef BOOL(WINAPI* SetAff)(HWND, DWORD);
    SetAff sa = (SetAff)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
    HWND w = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool ok = false;
    if (sa && w) { sa(w, 0x11); ok = AffinityOf(w) == 0x11; }
    if (w) DestroyWindow(w);
    return ok;
}

static bool LineAt(const uint8_t* fr, const uint8_t* base, int w, int h, int x, int y)
{
    for (int dy = -3; dy <= 3; ++dy) {
        int yy = y + dy; if (yy < 0 || yy >= h) continue;
        size_t o = ((size_t)(h - 1 - yy) * w + x) * 4;
        if (abs(fr[o] - base[o]) + abs(fr[o + 1] - base[o + 1]) + abs(fr[o + 2] - base[o + 2]) > 80) return true;
    }
    return false;
}

static bool Near(COLORREF a, int r, int g, int b, int tol)
{
    return abs((int)GetRValue(a) - r) <= tol && abs((int)GetGValue(a) - g) <= tol && abs((int)GetBValue(a) - b) <= tol;
}

// Pixel (x, y from the top) of a 32-bit bottom-up DIB.
static bool BmpPixel(HBITMAP bmp, int x, int y, int* r, int* g, int* b, int* bw, int* bh)
{
    BITMAP bm;
    if (!GetObjectW(bmp, sizeof(bm), &bm) || !bm.bmBits || bm.bmBitsPixel != 32) return false;
    if (bw) *bw = bm.bmWidth;
    if (bh) *bh = bm.bmHeight;
    if (x < 0 || y < 0 || x >= bm.bmWidth || y >= bm.bmHeight) return false;
    const uint8_t* p = (const uint8_t*)bm.bmBits + (size_t)(bm.bmHeight - 1 - y) * bm.bmWidthBytes + (size_t)x * 4;
    *b = p[0]; *g = p[1]; *r = p[2];
    return true;
}

// Checks `samples` random safe points of the capture of fixture sub-rectangle (ox, oy, cw, ch), scaled by sx, sy.
// Which windows lie over the fixture (above it in the z-order, visible, overlapping): a colour that is wrong because something stands
// in front of the fixture is explained by this list.
static BOOL CALLBACK CoverProc(HWND hw, LPARAM lp)
{
    int* state = (int*)lp;                                            // 0 above the fixture, 1 reached it
    if (hw == g_fx.hw) { *state = 1; return FALSE; }
    if (!IsWindowVisible(hw)) return TRUE;
    RECT r, f, x; GetWindowRect(hw, &r); GetWindowRect(g_fx.hw, &f);
    if (!IntersectRect(&x, &r, &f)) return TRUE;
    wchar_t cls[80] = L"", title[100] = L""; GetClassNameW(hw, cls, 80); GetWindowTextW(hw, title, 100);
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    LONG ex = GetWindowLongW(hw, GWL_EXSTYLE);
    Rep(L"INFO", L"  in front of the fixture: [%s] \"%s\" %ld,%ld %ldx%ld pid %lu%s", cls, title, (long)r.left, (long)r.top, (long)(r.right - r.left), (long)(r.bottom - r.top), (unsigned long)pid, (ex & WS_EX_LAYERED) ? L" layered" : L"");
    return TRUE;
}
static void LogCoveringWindows()
{
    int st = 0; EnumWindows(CoverProc, (LPARAM)&st);
    // Where the fixture is, what GetPixel sees in it, and the whole screen as a picture beside the program's logs (the first two times in a run).
    RECT f; GetWindowRect(g_fx.hw, &f);
    HDC dc = GetDC(nullptr); COLORREF c = dc ? GetPixel(dc, f.left + g_fx.w / 4, f.top + g_fx.h / 4) : CLR_INVALID; if (dc) ReleaseDC(nullptr, dc);
    COLORREF e = Expected(g_fx.w / 4, g_fx.h / 4, g_fx.w, g_fx.h);
    Rep(L"INFO", L"  the fixture is at %ld,%ld %dx%d (visible %d, topmost %d); GetPixel of its first quarter: %d,%d,%d (expected %d,%d,%d)", (long)f.left, (long)f.top, g_fx.w, g_fx.h,
        (int)IsWindowVisible(g_fx.hw), (GetWindowLongW(g_fx.hw, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0, GetRValue(c), GetGValue(c), GetBValue(c), GetRValue(e), GetGValue(e), GetBValue(e));
    static int shots = 0;
    if (shots < 2) {
        wchar_t pd[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) > 0) {
            wchar_t path[MAX_PATH + 40]; _snwprintf(path, MAX_PATH + 40, L"%s\\Click2Screen\\logs\\colour-fail-%d.png", pd, ++shots);
            RECT vs = VirtualScreen();
            HBITMAP b = CaptureRect(vs, false, false);
            if (b) { bool ok = SaveBitmap(b, path, IMG_PNG); DeleteObject(b); Rep(L"INFO", L"  the screen was saved: %s (%d)", path, (int)ok); }
        }
    }
}

static int CheckColors(HBITMAP bmp, int ox, int oy, int cw, int ch, int tol, int samples, int* checked)
{
    int bad = 0; *checked = 0;
    int bw = 0, bh = 0;
    int r, g, b;
    BmpPixel(bmp, 0, 0, &r, &g, &b, &bw, &bh);
    for (int i = 0; i < samples * 4 && *checked < samples; ++i) {
        int x = R(0, bw - 1), y = R(0, bh - 1);
        int lx = ox + (int)((double)(x + 0.5) * cw / bw), ly = oy + (int)((double)(y + 0.5) * ch / bh);
        if (!SafePoint(lx, ly, g_fx.w, g_fx.h)) continue;
        // also keep a margin in the capture's own pixels when scaled
        if (!BmpPixel(bmp, x, y, &r, &g, &b, nullptr, nullptr)) continue;
        COLORREF e = Expected(lx, ly, g_fx.w, g_fx.h);
        ++*checked;
        if (!Near(e, r, g, b, tol)) { ++bad; Rep(L"INFO", L"  pixel (%d,%d) = %d,%d,%d, expected %d,%d,%d", x, y, r, g, b, GetRValue(e), GetGValue(e), GetBValue(e)); }
    }
    if (bad) LogCoveringWindows();
    return bad;
}

// ---- WIC decoding of saved images --------------------------------------------------------------------------------
static bool DecodePixelsOnce(const wchar_t* path, int* w, int* h, uint8_t** px)
{
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    *px = nullptr;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f))) break;
        if (FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        if (FAILED(f->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) break;
        UINT uw = 0, uh = 0; cv->GetSize(&uw, &uh);
        *px = new uint8_t[(size_t)uw * uh * 4];
        if (FAILED(cv->CopyPixels(nullptr, uw * 4, uw * uh * 4, *px))) { delete[] *px; *px = nullptr; break; }
        *w = (int)uw; *h = (int)uh; ok = true;
    } while (false);
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (f) f->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

// A picture that is still being written (a slow machine: a 3 MB BMP took seconds) is read again for a while before it counts as broken.
static bool DecodePixels(const wchar_t* path, int* w, int* h, uint8_t** px)
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (DecodePixelsOnce(path, w, h, px)) return true;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;       // there is no such file: nothing to wait for
        Sleep(150);
    }
    return false;
}

int CurrentDpi() { return 96; }

// ==== tests ==================================================================================================
static void T_Settings()
{
    SettingsUseKey(L"Software\\Click2Screen-Autotest");
    { Settings def; SettingsDefaults(def); Check(Same(def.fileName, L"File $D.$M.$Y $H-$N-$S"), L"settings: the default name template is File $D.$M.$Y $H-$N-$S"); }
    Settings a, b; RandomSettings(a);
    SettingsSave(a); SettingsLoad(b);
    wchar_t why[160] = L"";
    // a template that is not valid is replaced by the default on load; the random ones are valid
    Check(SettingsEqual(a, b, true, why), L"settings: registry round trip", why);
    // Values out of range in the registry are clamped on load.
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD bad = 999999; RegSetValueExW(k, L"Fps", 0, REG_DWORD, (BYTE*)&bad, 4); RegSetValueExW(k, L"Delay", 0, REG_DWORD, (BYTE*)&bad, 4);
        RegSetValueExW(k, L"ImageFormat", 0, REG_DWORD, (BYTE*)&bad, 4);
        RegCloseKey(k);
        Settings c; SettingsLoad(c);
        Check(c.fps >= 5 && c.fps <= 60 && c.delaySec <= 10 && c.imageFormat <= IMG_COUNT - 1, L"settings: out-of-range registry values are clamped");
    }
}

static void T_Ini()
{
    Settings a, b; RandomSettings(a);
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\export-%d.ini", g_dir, R(0, 99999));
    bool ex = SettingsExport(a, path);
    Check(ex, L"ini: export", path);
    if (!ex) return;
    SettingsDefaults(b); b.language = a.language;
    bool im = SettingsImport(b, path);
    wchar_t why[160] = L"";
    Check(im && SettingsEqual(a, b, true, why), L"ini: import gives the exported settings back", why);
    // the file is UTF-16 with a BOM
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    unsigned char bom[2] = {0, 0}; DWORD got = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, bom, 2, &got, nullptr); CloseHandle(h); }
    Check(bom[0] == 0xFF && bom[1] == 0xFE, L"ini: UTF-16 with a BOM");
    // out-of-range / invalid values are ignored, the others still applied
    WritePrivateProfileStringW(L"Click2Screen", L"Fps", L"500", path);
    WritePrivateProfileStringW(L"Click2Screen", L"Delay", L"abc", path);
    WritePrivateProfileStringW(L"Click2Screen", L"FileName", L"bad/name", path);
    WritePrivateProfileStringW(L"Click2Screen", L"VideoQuality", L"2", path);
    Settings c; SettingsDefaults(c);
    bool ok = SettingsImport(c, path);
    { Settings dd0; SettingsDefaults(dd0); Check(dd0.delaySec == 0, L"settings: the default delay before a capture is 0 seconds"); }
    Check(ok && c.fps == 25 && c.delaySec == 0 && Same(c.fileName, L"File $D.$M.$Y $H-$N-$S") && c.videoQuality == 2, L"ini: invalid values are ignored, valid ones applied");
    // missing file / other file
    wchar_t nope[MAX_PATH]; _snwprintf(nope, MAX_PATH, L"%s\\missing.ini", g_dir);
    Settings d; SettingsDefaults(d);
    Check(!SettingsImport(d, nope), L"ini: missing file is refused");
    wchar_t other[MAX_PATH]; _snwprintf(other, MAX_PATH, L"%s\\other.ini", g_dir);
    HANDLE oh = CreateFileW(other, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (oh != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(oh, "[Other]\r\nFps=30\r\n", 17, &w, nullptr); CloseHandle(oh); }
    Check(!SettingsImport(d, other), L"ini: a file without the Click2Screen section is refused");
}

// independent oracle for TemplateValid
static bool OracleValid(const wchar_t* t)
{
    int runs = 0, literal = 0; bool inRun = false;
    for (const wchar_t* p = t; *p; ++p) {
        if (*p == L'#') { if (!inRun) { ++runs; inRun = true; } continue; }
        inRun = false;
        if (*p == L'$' && p[1] && wcschr(L"YMDHNS", p[1])) { ++p; ++literal; continue; }
        if (_wcsnicmp(p, L"%USERNAME%", 10) == 0) { p += 9; ++literal; continue; }
        if (*p < 32 || wcschr(L"\\/:*?\"<>|", *p)) return false;
        ++literal;
    }
    return runs <= 1 && (literal > 0 || runs == 1);
}

static void T_Template()
{
    static const wchar_t* toks[] = { L"$Y", L"$M", L"$D", L"$H", L"$N", L"$S", L"%USERNAME%", L"#", L"##", L"###", L"a", L"Img", L"_", L"-", L" ", L"\x0444", L"/", L":", L"?", L"$Z", L"%", L"*", L"\"" };
    int bad = 0, total = 200;
    for (int i = 0; i < total; ++i) {
        wchar_t t[128] = L"";
        int n = R(0, 6);
        for (int j = 0; j < n; ++j) wcscat(t, toks[R(0, (int)(sizeof(toks) / sizeof(toks[0])) - 1)]);
        if (TemplateValid(t) != OracleValid(t)) { ++bad; Rep(L"INFO", L"  template \"%s\": program %d, oracle %d", t, (int)TemplateValid(t), (int)OracleValid(t)); }
    }
    Check(bad == 0, L"template: validity of 200 random templates matches the rules", bad ? L"(see INFO lines)" : L"");

    // the preview of the name (what the options window shows under the template field)
    {
        SettingsUseKey(L"Software\\Click2Screen-Autotest");
        wchar_t got[300], want[300], want2[300]; bool okTime = false;
        for (int tries = 0; tries < 3 && !okTime; ++tries) {                              // the clock may tick between the two readings
            SYSTEMTIME st; GetLocalTime(&st);
            _snwprintf(want, 300, L"File %02d.%02d.%04d %02d-%02d-%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
            TemplatePreview(L"File $D.$M.$Y $H-$N-$S", got, 300);
            GetLocalTime(&st);
            _snwprintf(want2, 300, L"File %02d.%02d.%04d %02d-%02d-%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
            okTime = Same(got, want) || Same(got, want2);
        }
        Check(okTime, L"template: the preview of \"File $D.$M.$Y $H-$N-$S\" is the name a file gets now, without the extension", got);
        wchar_t num[300], user[128]; DWORD un = 128; if (!GetUserNameW(user, &un)) wcscpy(user, L"user");
        TemplatePreview(L"Img####", num, 300);
        bool digits = wcsncmp(num, L"Img", 3) == 0 && wcslen(num) == 7; for (int i = 3; i < 7 && digits; ++i) if (num[i] < L'0' || num[i] > L'9') digits = false;
        Check(digits, L"template: the preview puts a number of the given width in the place of the #", num);
        wchar_t un2[300]; TemplatePreview(L"%USERNAME% x", un2, 300);
        wchar_t wantUser[300]; _snwprintf(wantUser, 300, L"%s x", user);
        Check(Same(un2, wantUser), L"template: the preview has the user name where %USERNAME% stands", un2);
        wchar_t bad1[300] = L"x", bad2[300] = L"x", dots[300];
        TemplatePreview(L"a/b", bad1, 300); TemplatePreview(L"a#b#", bad2, 300); TemplatePreview(L"abc. ", dots, 300);
        Check(bad1[0] == 0 && bad2[0] == 0, L"template: the preview of an invalid template is empty");
        Check(Same(dots, L"abc"), L"template: a name does not end with a dot or a blank in the preview either", dots);
    }

    // BuildOutputPath: never an existing file, numbering skips used numbers, characters of the name are legal
    wchar_t folder[MAX_PATH]; wchar_t part[24]; RandomText(part, 10);
    _snwprintf(folder, MAX_PATH, L"%s\\tpl %s", g_dir, part);
    int digits = R(1, 5);
    wchar_t tpl[64] = L"Shot", hashes[8] = L"";
    for (int i = 0; i < digits; ++i) wcscat(hashes, L"#");
    wcscat(tpl, hashes);
    if (Chance(50)) wcscat(tpl, L"_$Y");
    wchar_t key[40]; _snwprintf(key, 40, L"AutotestCounter%d", R(0, 99999));
    int made = R(2, 8), failed = 0;
    unsigned lastNum = 0;
    for (int i = 0; i < made; ++i) {
        wchar_t path[MAX_PATH], err[300] = L"";
        if (!BuildOutputPath(folder, tpl, L"png", key, path, MAX_PATH, err, 300)) { ++failed; Rep(L"INFO", L"  BuildOutputPath failed: %s", err); break; }
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) { ++failed; Rep(L"INFO", L"  returned an existing file %s", path); }
        // the number after "Shot"
        const wchar_t* s = wcsstr(path, L"\\Shot");
        unsigned num = s ? (unsigned)wcstoul(s + 5, nullptr, 10) : 0;
        if (num <= lastNum) { ++failed; Rep(L"INFO", L"  number %u after %u in %s", num, lastNum, path); }
        lastNum = num;
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (Chance(30)) {          // somebody took the next number meanwhile
            wchar_t nxt[MAX_PATH];
            _snwprintf(nxt, MAX_PATH, L"%s\\Shot%0*u.png", folder, digits, num + 1);
            HANDLE g = CreateFileW(nxt, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (g != INVALID_HANDLE_VALUE) CloseHandle(g);
        }
    }
    Check(failed == 0, L"template: numbered names are fresh and increasing", tpl);
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest", 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, key); RegCloseKey(k); }
    // a template without a number never overwrites
    wchar_t p1[MAX_PATH], p2[MAX_PATH], err[300];
    bool a = BuildOutputPath(folder, L"same name", L"mp4", nullptr, p1, MAX_PATH, err, 300);
    if (a) { HANDLE f = CreateFileW(p1, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr); if (f != INVALID_HANDLE_VALUE) CloseHandle(f); }
    bool b = BuildOutputPath(folder, L"same name", L"mp4", nullptr, p2, MAX_PATH, err, 300);
    Check(a && b && !Same(p1, p2), L"template: a name without # does not overwrite the previous file");
    // an unusable folder is reported
    wchar_t e2[300];
    Check(!EnsureFolder(L"", e2, 300) && e2[0], L"folder: empty path refused with a message");
}

static void T_Sizes()
{
    int bad = 0;
    for (int i = 0; i < 3000; ++i) {
        int w = R(1, 8192), h = R(1, 8192);
        if (Chance(30)) { w = R(1, 2200); h = R(1, 1300); }
        int ow, oh; FitSize(w, h, ow, oh);
        bool ok = ow >= 2 && oh >= 2 && ow <= 1920 && oh <= 1080 && (ow % 2 == 0) && (oh % 2 == 0);
        if (ok) {                                // each side within 2 px of the exact scale (even rounding, minimum 2)
            double k = 1.0;
            if (w > 1920) k = 1920.0 / w;
            if (h * k > 1080.0) k = 1080.0 / h;
            double ew = w * k, eh = h * k;
            if (ew < 2) ew = 2;
            if (eh < 2) eh = 2;
            if (fabs(ow - ew) > 2.01 || fabs(oh - eh) > 2.01) ok = false;
        }
        if (ok && w <= 1920 && h <= 1080 && w % 2 == 0 && h % 2 == 0 && (ow != w || oh != h)) ok = false;   // fits: unchanged
        if (!ok) { ++bad; if (bad < 6) Rep(L"INFO", L"  FitSize(%d,%d) = %d,%d", w, h, ow, oh); }
    }
    Check(bad == 0, L"sizes: FitSize gives even sides <= 1920x1080, scaled evenly within 2 px, fitting sizes unchanged (3000 random)");
    bad = 0;
    for (int i = 0; i < 3000; ++i) {
        int w1 = R(2, 1920), h1 = R(2, 1080), w2 = R(w1, 1920), h2 = R(h1, 1080);
        int a = AutoBitrate(w1, h1), b = AutoBitrate(w2, h2);
        if (a <= 0 || b > 4000000 || a > b) { ++bad; if (bad < 6) Rep(L"INFO", L"  AutoBitrate(%d,%d)=%d, (%d,%d)=%d", w1, h1, a, w2, h2, b); }
    }
    Check(bad == 0, L"sizes: AutoBitrate is positive, <= 4 Mbit/s and never falls when the frame grows (3000 random pairs)");
}

static void T_Languages()
{
    int bad = 0;
    LangSelect(1);
    wchar_t en[S__COUNT][700];
    for (int s = 0; s < S__COUNT; ++s) { wcsncpy(en[s], T((Str)s), 699); en[s][699] = 0; }
    for (int lng = 1; lng <= LangCount(); ++lng) {
        LangSelect(lng);
        for (int s = 0; s < S__COUNT; ++s) {
            const wchar_t* t = T((Str)s);
            if (!t || !*t) { ++bad; Rep(L"INFO", L"  language %d string %d is empty", lng, s); continue; }
            // same number of line breaks and of printf placeholders as English
            int nb1 = 0, nb2 = 0, p1 = 0, p2 = 0;
            for (const wchar_t* c = en[s]; *c; ++c) { if (*c == L'\n') ++nb1; if (*c == L'%' && c[1] != L'U' && c[1] != 0) ++p1; }
            for (const wchar_t* c = t; *c; ++c) { if (*c == L'\n') ++nb2; if (*c == L'%' && c[1] != L'U' && c[1] != 0) ++p2; }
            if (nb1 != nb2 || p1 != p2) { ++bad; Rep(L"INFO", L"  language %d string %d: line breaks %d/%d, placeholders %d/%d", lng, s, nb2, nb1, p2, p1); }
        }
        if (!*LangName(lng - 1)) { ++bad; }
    }
    LangSelect(0);
    Check(bad == 0, L"languages: every table is complete, with the English line breaks and placeholders", L"(17 languages)");
    // the formats really work with their arguments (no crash, no empty result)
    wchar_t buf[400];
    for (int lng = 1; lng <= LangCount(); ++lng) {
        LangSelect(lng);
        _snwprintf(buf, 400, T(S_STARTING_IN), 3); if (!buf[0]) ++bad;
        _snwprintf(buf, 400, T(S_SAVED), L"C:\\x.mp4"); if (!buf[0]) ++bad;
        _snwprintf(buf, 400, T(S_SU_LOG_COPY_FAIL), L"a.exe", 5UL); if (!buf[0]) ++bad;
        _snwprintf(buf, 400, T(S_SU_STATE_INST), L"1.0.291.1", L"C:\\p"); if (!buf[0]) ++bad;
    }
    LangSelect(0);
    Check(bad == 0, L"languages: formatted messages work in every language");
}

static void T_Capture()
{
    int fw = R(300, 900), fh = R(220, 600);
    MakeFixture(fw, fh);
    RECT fr = FxScreenRect();
    int cw = R(60, g_fx.w), ch = R(60, g_fx.h - 12);
    int ox = R(0, g_fx.w - cw), oy = R(0, g_fx.h - ch - 12);
    RECT rc = { fr.left + ox, fr.top + oy, fr.left + ox + cw, fr.top + oy + ch };
    bool mouse = Chance(50), hi = Chance(50);
    if (mouse) SetCursorPos(fr.right + 400 > GetSystemMetrics(SM_CXSCREEN) ? 2 : fr.right + 400, 2);   // away from the fixture
    HBITMAP bmp = CaptureRect(rc, false, false);
    wchar_t name[200];
    _snwprintf(name, 200, L"capture %dx%d at +%d+%d of %dx%d (mouse=%d highlight=%d)", cw, ch, ox, oy, g_fx.w, g_fx.h, mouse, hi);
    if (!bmp) { FAIL(L"%s: CaptureRect failed", name); return; }
    int bw = 0, bh = 0, r, g, b;
    BmpPixel(bmp, 0, 0, &r, &g, &b, &bw, &bh);
    int checked = 0, bad = CheckColors(bmp, ox, oy, cw, ch, 6, 14, &checked);
    Check(bw == cw && bh == ch && bad == 0 && checked > 0, name, bad ? L"colours or orientation wrong" : L"");
    // saving in every format decodes again, with the same size and colours
    for (int f = 0; f < IMG_COUNT; ++f) {
        if (!ImageFormatAvailable(f)) { SKIP(L"save %s: this Windows has no encoder for it", ImageFormatName(f)); continue; }
        wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\cap%d.%s", g_dir, R(0, 99999), ImageFormatExt(f));
        bool s = SaveBitmap(bmp, path, f);
        int dw = 0, dh = 0; uint8_t* px = nullptr;
        bool d = s && DecodePixels(path, &dw, &dh, &px);
        int tol = FmtTol(f, 3), badp = 0, n = 0;
        if (d && dw == cw && dh == ch) {
            for (int i = 0; i < 60 && n < 12; ++i) {
                int x = R(0, dw - 1), y = R(0, dh - 1), lx = ox + x, ly = oy + y;
                if (!SafePoint(lx, ly, g_fx.w, g_fx.h)) continue;
                if (LossyFmt(f) && (abs(lx - g_fx.w / 2) < 20 || abs(ly - g_fx.h / 2) < 20)) continue;   // ringing at edges
                const uint8_t* p = px + ((size_t)y * dw + x) * 4;
                COLORREF e = Expected(lx, ly, g_fx.w, g_fx.h);
                ++n; if (!Near(e, p[2], p[1], p[0], tol)) ++badp;
            }
        }
        wchar_t nm[120]; _snwprintf(nm, 120, L"save %s: file decodes, %dx%d, colours (%d points)", ImageFormatName(f), cw, ch, n);
        Check(s && d && dw == cw && dh == ch && badp == 0, nm);
        delete[] px;
    }
    DeleteObject(bmp);
    // the pointer: drawn when asked
    CURSORINFO ci; memset(&ci, 0, sizeof(ci)); ci.cbSize = sizeof(ci);
    SetCursorPos(rc.left + cw / 2, rc.top + ch / 2);
    Pump(60);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        HBITMAP a = CaptureRect(rc, false, false), b2 = CaptureRect(rc, true, hi);
        int diff = 0;
        if (a && b2) for (int y = 0; y < ch; y += 1) for (int x = 0; x < cw; x += 1) {
            int r1, g1, b1, r2, g2, b2c;
            BmpPixel(a, x, y, &r1, &g1, &b1, nullptr, nullptr); BmpPixel(b2, x, y, &r2, &g2, &b2c, nullptr, nullptr);
            if (abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2c) > 30) ++diff;
        }
        Check(diff > 20, L"capture: the mouse pointer is drawn into the picture when asked", hi ? L"(with the highlight)" : L"");
        if (a) DeleteObject(a);
        if (b2) DeleteObject(b2);
    } else SKIP(L"capture: the mouse pointer is not visible in this session");
}

static void T_Scaled()
{
    MakeFixture(R(500, 1000), R(300, 640));
    RECT fr = FxScreenRect();
    int sw = R(150, g_fx.w), sh = R(120, g_fx.h - 12);
    int ow = (R(40, sw) + 1) & ~1, oh = (R(40, sh) + 1) & ~1;
    Grabber gr;
    wchar_t name[160]; _snwprintf(name, 160, L"scaled grab %dx%d -> %dx%d", sw, sh, ow, oh);
    if (!gr.Init(ow, oh)) { FAIL(L"%s: Init", name); return; }
    RECT rc = { fr.left, fr.top, fr.left + sw, fr.top + sh };
    // something short-lived (a toast, a window still being painted) may lie over the fixture for a moment: a wrong picture is taken again, twice at most
    bool ok = false; int checked = 0, bad = 1;
    for (int attempt = 0; attempt < 3 && !(ok && bad == 0 && checked > 0); ++attempt) {
        if (attempt) Pump(500);
        ok = gr.Grab(rc, false, false);
        HBITMAP bmp = gr.Release();
        checked = 0; bad = ok ? CheckColors(bmp, 0, 0, sw, sh, 24, 14, &checked) : 1;
        if (bmp) DeleteObject(bmp);
        if (!(ok && bad == 0 && checked > 0) && !gr.Init(ow, oh)) { FAIL(L"%s: Init", name); return; }
    }
    Check(ok && bad == 0 && checked > 0, name);
}

static void T_Edges()
{
    RECT vs = VirtualScreen();
    int bad = 0;
    for (int i = 0; i < 6; ++i) {
        int w = R(20, 400), h = R(20, 300);
        int x = vs.left + R(-w + 1, vs.right - vs.left - 1), y = vs.top + R(-h + 1, vs.bottom - vs.top - 1);
        RECT rc = { x, y, x + w, y + h };
        HBITMAP b = CaptureRect(rc, Chance(50), Chance(50));
        int bw = 0, bh = 0, r, g, bl;
        if (!b || !BmpPixel(b, 0, 0, &r, &g, &bl, &bw, &bh) || bw != w || bh != h) { ++bad; Rep(L"INFO", L"  rect %d,%d %dx%d (screen %d,%d..%d,%d) not captured at full size", x, y, w, h, vs.left, vs.top, vs.right, vs.bottom); }
        if (b) DeleteObject(b);
    }
    Check(bad == 0, L"capture: rectangles partly outside the screen keep their size (6 random)");
}

// ---- pickers, driven by messages -----------------------------------------------------------------
struct PickJob { int mode; POINT a, b; bool useWindow; bool clickFirst; };
static DWORD WINAPI PickDriver(LPVOID p)
{
    PickJob* j = (PickJob*)p;
    HWND ov = nullptr;
    for (int i = 0; i < 100 && !ov; ++i) { ov = FindWindowW(L"Click2ScreenPicker", nullptr); if (!ov) Sleep(50); }
    if (!ov) return 1;
    Sleep(200);
    RECT vs = VirtualScreen();
    if (j->mode == 0) {         // drag a rectangle (overlay coordinates)
        if (j->clickFirst) {    // a plain click is not a selection: the overlay waits for a real drag
            SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(j->a.x - vs.left, j->a.y - vs.top));
            SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(j->a.x - vs.left, j->a.y - vs.top));
            Sleep(150);
        }
        SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(j->a.x - vs.left, j->a.y - vs.top));
        int steps = R(2, 6);
        for (int s = 1; s <= steps; ++s)
            SendMessageW(ov, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(j->a.x + (j->b.x - j->a.x) * s / steps - vs.left, j->a.y + (j->b.y - j->a.y) * s / steps - vs.top));
        SetCursorPos(j->b.x, j->b.y);
        SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(j->b.x - vs.left, j->b.y - vs.top));
    } else if (j->mode == 1) {  // point at a window, click
        SetCursorPos(j->a.x, j->a.y);       // the real pointer on the target: a parked one sends its own WM_MOUSEMOVE and would win
        SendMessageW(ov, WM_MOUSEMOVE, 0, MAKELPARAM(j->a.x - vs.left, j->a.y - vs.top));
        Sleep(50);
        SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(j->a.x - vs.left, j->a.y - vs.top));
    } else {                    // cancel with Esc
        SendMessageW(ov, WM_KEYDOWN, VK_ESCAPE, 0);
    }
    // Safety net: a picker that did not finish (the test then fails instead of hanging) is cancelled.
    for (int i = 0; i < 40; ++i) { Sleep(100); if (!FindWindowW(L"Click2ScreenPicker", nullptr)) return 0; }
    ov = FindWindowW(L"Click2ScreenPicker", nullptr);
    if (ov) SendMessageW(ov, WM_KEYDOWN, VK_ESCAPE, 0);
    return 2;
}

static void T_Pickers()
{
    MakeFixture(R(400, 800), R(300, 500));
    RECT fr = FxScreenRect();
    RECT vs = VirtualScreen();
    int mode = R(0, 3);
    PickJob job; memset(&job, 0, sizeof(job)); job.mode = mode == 3 ? 1 : mode;
    RECT got = {0, 0, 0, 0}; HWND gotw = nullptr; bool ok;
    if (mode == 3) {
        // the desktop is a window too: pointing at a place no window covers returns the work area of that monitor (no taskbar)
        bool found = false; POINT pt = { 0, 0 }; RECT work = { 0, 0, 0, 0 };
        for (int i = 0; i < 60 && !found; ++i) {
            POINT q = { vs.left + R(0, vs.right - vs.left - 1), vs.top + R(0, vs.bottom - vs.top - 1) };
            if (PtInRect(&fr, q)) continue;
            MONITORINFO mi; mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(MonitorFromPoint(q, MONITOR_DEFAULTTONEAREST), &mi) || !PtInRect(&mi.rcWork, q)) continue;
            HWND under = WindowFromPoint(q), root = under ? GetAncestor(under, GA_ROOT) : nullptr;
            wchar_t cls[64] = L""; if (root) GetClassNameW(root, cls, 64);
            if (under == GetDesktopWindow() || root == GetDesktopWindow() || !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) { pt = q; work = mi.rcWork; found = true; }
        }
        if (!found) { SKIP(L"picker: no free desktop spot on this screen"); return; }
        job.a = pt;
        HANDLE th = CreateThread(nullptr, 0, PickDriver, &job, 0, nullptr);
        ok = PickWindow(nullptr, &gotw, &got);
        WaitForSingleObject(th, 5000); CloseHandle(th);
        Check(ok && gotw == nullptr && EqualRect(&got, &work), L"picker: pointing at the bare desktop returns its work area (the desktop is a window)");
        if (ok && !EqualRect(&got, &work)) Rep(L"INFO", L"  wanted %d,%d,%d,%d got %d,%d,%d,%d", work.left, work.top, work.right, work.bottom, got.left, got.top, got.right, got.bottom);
    } else if (mode == 0) {
        RECT want; want.left = vs.left + R(0, (vs.right - vs.left) - 60); want.top = vs.top + R(0, (vs.bottom - vs.top) - 60);
        want.right = want.left + R(20, vs.right - want.left > 500 ? 500 : vs.right - want.left); want.bottom = want.top + R(20, vs.bottom - want.top > 400 ? 400 : vs.bottom - want.top);
        bool flip = Chance(50);             // drag from the bottom-right corner back
        job.clickFirst = Chance(30);
        job.a.x = flip ? want.right : want.left; job.a.y = flip ? want.bottom : want.top;
        job.b.x = flip ? want.left : want.right; job.b.y = flip ? want.top : want.bottom;
        HANDLE th = CreateThread(nullptr, 0, PickDriver, &job, 0, nullptr);
        ok = PickRegion(nullptr, &got);
        WaitForSingleObject(th, 5000); CloseHandle(th);
        Check(ok && EqualRect(&got, &want), L"picker: the dragged region is returned exactly (any drag direction, a stray click first or not)");
        if (ok && !EqualRect(&got, &want)) Rep(L"INFO", L"  wanted %d,%d,%d,%d got %d,%d,%d,%d", want.left, want.top, want.right, want.bottom, got.left, got.top, got.right, got.bottom);
    } else if (mode == 1) {
        job.a.x = fr.left + R(10, g_fx.w - 10); job.a.y = fr.top + R(10, g_fx.h - 10);
        HANDLE th = CreateThread(nullptr, 0, PickDriver, &job, 0, nullptr);
        ok = PickWindow(nullptr, &gotw, &got);
        WaitForSingleObject(th, 5000); CloseHandle(th);
        RECT vis; WindowVisibleRect(g_fx.hw, &vis);
        Check(ok && gotw == g_fx.hw && EqualRect(&got, &vis), L"picker: pointing at the test window returns that window and its rectangle");
    } else {
        HANDLE th = CreateThread(nullptr, 0, PickDriver, &job, 0, nullptr);
        ok = Chance(50) ? PickRegion(nullptr, &got) : PickWindow(nullptr, &gotw, &got);
        WaitForSingleObject(th, 5000); CloseHandle(th);
        Check(!ok, L"picker: Esc cancels");
    }
}

// ---- recorder ----------------------------------------------------------------------------------------------------
static bool MfAvailable()
{
    if (g_wine) return false;
    return true;
}

struct VideoInfo { bool ok; int w, h; double dur; int frames; bool h264; bool hevc; bool wmv; bool audio; double adur; };
static const GUID kTestHevc = { 0x43564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID kTestWmv3 = { 0x33564d57, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

// Opens the MP4 with a source reader: stream types, duration, and a count of decoded video frames; with `check`, the
// colours of one decoded frame are tested against the fixture (static fixture only).
static VideoInfo Inspect(const wchar_t* path, bool checkPicture, bool* pictureOk, int* pictureChecked, bool* decoderMissing)
{
    VideoInfo v; memset(&v, 0, sizeof(v));
    *decoderMissing = false;
    HRESULT hrc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    IMFAttributes* at = nullptr; MFCreateAttributes(&at, 1);
    if (at) at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* rd = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(path, at, &rd)) && rd) {
        IMFMediaType* nt = nullptr;
        if (SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nt))) {
            GUID sub; UINT32 w = 0, h = 0;
            if (SUCCEEDED(nt->GetGUID(MF_MT_SUBTYPE, &sub))) { v.h264 = sub == MFVideoFormat_H264; v.hevc = sub == kTestHevc; v.wmv = sub == kTestWmv3; }
            MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &w, &h);
            v.w = (int)w; v.h = (int)h; v.ok = true;
            nt->Release();
        }
        IMFMediaType* an = nullptr;
        if (SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &an))) { v.audio = true; an->Release(); }
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8) v.dur = (double)pv.uhVal.QuadPart / 1e7;
        PropVariantClear(&pv);
        // decode to RGB32
        IMFMediaType* out = nullptr;
        if (v.ok && SUCCEEDED(MFCreateMediaType(&out))) {
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            HRESULT hr = rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
            out->Release();
            if (FAILED(hr)) *decoderMissing = true;
            else {
                IMFMediaType* cur = nullptr; INT32 stride = 0; bool strideTold = false;
                if (SUCCEEDED(rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
                    UINT32 s = 0, ow = 0, oh = 0;
                    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) { stride = (INT32)s; strideTold = true; } else stride = -(INT32)(v.w * 4);   // RGB32 without a stride attribute is bottom-up (the Media Foundation default)
                    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &ow, &oh);
                    Rep(L"INFO", L"  decoder output %ux%u, stride %d (native %dx%d)", ow, oh, (int)stride, v.w, v.h);
                    cur->Release();
                }
                int target = R(2, 6);
                for (int guard = 0; guard < 100000; ++guard) {
                    DWORD flags = 0; LONGLONG ts = 0; IMFSample* sm = nullptr;
                    HRESULT hr2 = rd->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sm);
                    if (FAILED(hr2) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) { if (sm) sm->Release(); break; }
                    if (sm) {
                        ++v.frames;
                        if (checkPicture && v.frames == target) {
                            Rep(L"INFO", L"  checked frame #%d is at %.2f s of the file", v.frames, (double)ts / 1e7);
                            IMFMediaBuffer* mb = nullptr; BYTE* data = nullptr; DWORD len = 0;
                            IMF2DBuffer* b2 = nullptr; BYTE* sl = nullptr; LONG pitch = 0;
                            bool use2d = false, locked = false;
                            if (SUCCEEDED(sm->ConvertToContiguousBuffer(&mb)) && mb) {
                                // The decoder pads the picture (to 16 pixels in both directions): the address of the first row and the
                                // pitch (negative: bottom-up) come from the 2-D buffer, not from the size of the memory.
                                if (SUCCEEDED(mb->QueryInterface(IID_IMF2DBuffer, (void**)&b2)) && b2 && SUCCEEDED(b2->Lock2D(&sl, &pitch))) use2d = true;
                                else if (SUCCEEDED(mb->Lock(&data, nullptr, &len))) locked = true;
                            }
                            if (use2d || locked) {
                                int absStride = stride < 0 ? -stride : stride;
                                int ah = (v.h + 15) & ~15;
                                if (locked && ah > 0 && len % ah == 0 && (int)(len / ah) >= v.w * 4) absStride = (int)(len / ah);       // the padded height
                                else if (locked && v.h > 0 && (int)(len / v.h) > absStride) absStride = (int)(len / v.h);
                                auto rowAt = [&](int y) -> const BYTE* {
                                    // The Source Reader's RGB32 picture is bottom-up in memory (a positive pitch says nothing else: seen on Windows 10,
                                    // and the files themselves are upright - checked with ffmpeg); a negative pitch makes `sl` the top row.
                                    // The Windows 10 decoder of WMV says what it gives: a positive stride is a top-down picture (the file is upright, ffmpeg agrees).
                                    if (use2d && v.wmv && strideTold && stride > 0 && pitch > 0) return sl + (ptrdiff_t)y * pitch;
                                    if (use2d) return pitch > 0 ? sl + (size_t)(v.h - 1 - y) * pitch : sl + (ptrdiff_t)y * pitch;
                                    return stride < 0 ? data + (size_t)(v.h - 1 - y) * absStride : data + (size_t)y * absStride;
                                };
                                int bad = 0, n = 0;
                                for (int i = 0; i < 200 && n < 16; ++i) {
                                    int x = R(0, v.w - 1), y = R(0, v.h - 1);
                                    // the recorded part is the whole fixture (region mode) scaled to w x h
                                    int lx = (int)((x + 0.5) * g_fx.w / v.w), ly = (int)((y + 0.5) * g_fx.h / v.h);
                                    if (!SafePoint(lx, ly, g_fx.w, g_fx.h) || abs(lx - g_fx.w / 2) < 20 || abs(ly - g_fx.h / 2) < 20) continue;
                                    const BYTE* p = rowAt(y) + x * 4;
                                    COLORREF e = Expected(lx, ly, g_fx.w, g_fx.h);
                                    ++n; if (!Near(e, p[2], p[1], p[0], 60)) { ++bad; if (bad < 5) Rep(L"INFO", L"  decoded frame (%d,%d) of %dx%d = %d,%d,%d, expected %d,%d,%d (%s, pitch %ld)", x, y, v.w, v.h, p[2], p[1], p[0], GetRValue(e), GetGValue(e), GetBValue(e), use2d ? L"2-D buffer" : L"flat buffer", (long)(use2d ? pitch : stride)); }
                                }
                                *pictureChecked = n; *pictureOk = n > 0 && bad == 0;
                                if (use2d) b2->Unlock2D(); else mb->Unlock();
                            }
                            if (b2) b2->Release();
                            if (mb) mb->Release();
                        }
                        sm->Release();
                    }
                }
            }
        }
        rd->Release();
    }
    if (at) at->Release();
    MFShutdown();
    if (SUCCEEDED(hrc)) CoUninitialize();
    return v;
}

// ---- AVI: a parser of our own (the file is checked as a player would read it) --------------------------------------
struct AviInfo { bool ok; int w, h; unsigned total, fps, vChunks, emptyV, aChunks, idxCount; unsigned long long aBytes; size_t pickOff; unsigned pickSize; bool riffOk; };

static unsigned Rd32(const uint8_t* p) { unsigned v; memcpy(&v, p, 4); return v; }

static AviInfo ReadAvi(const wchar_t* path, int pickN, uint8_t** keep)
{
    AviInfo a; memset(&a, 0, sizeof(a));
    *keep = nullptr;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return a;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 64 || sz.QuadPart > 400000000LL) { CloseHandle(h); return a; }
    size_t n = (size_t)sz.QuadPart;
    uint8_t* d = new uint8_t[n];
    DWORD got = 0; ReadFile(h, d, (DWORD)n, &got, nullptr); CloseHandle(h);
    if (got != n || memcmp(d, "RIFF", 4) || memcmp(d + 8, "AVI ", 4)) { delete[] d; return a; }
    a.riffOk = Rd32(d + 4) == n - 8;
    size_t pos = 12; int nonEmpty = 0;
    while (pos + 8 <= n) {
        unsigned id = Rd32(d + pos), size = Rd32(d + pos + 4);
        size_t end = pos + 8 + size;
        if (end > n) break;
        if (!memcmp(&id, "LIST", 4)) {
            if (!memcmp(d + pos + 8, "hdrl", 4)) {
                size_t q = pos + 12;
                while (q + 8 <= end) {
                    unsigned cs = Rd32(d + q + 4);
                    if (!memcmp(d + q, "avih", 4) && cs >= 56) { a.total = Rd32(d + q + 8 + 16); a.w = (int)Rd32(d + q + 8 + 32); a.h = (int)Rd32(d + q + 8 + 36); a.ok = true; }
                    if (!memcmp(d + q, "LIST", 4) && !memcmp(d + q + 8, "strl", 4)) {
                        size_t r = q + 12;
                        while (r + 8 <= q + 8 + cs) {
                            unsigned rs = Rd32(d + r + 4);
                            if (!memcmp(d + r, "strh", 4) && !memcmp(d + r + 8, "vids", 4)) { unsigned scale = Rd32(d + r + 8 + 20), rate = Rd32(d + r + 8 + 24); a.fps = scale ? rate / scale : 0; }
                            r += 8 + rs + (rs & 1);
                        }
                    }
                    q += 8 + cs + (cs & 1);
                }
            } else if (!memcmp(d + pos + 8, "movi", 4)) {
                size_t q = pos + 12;
                while (q + 8 <= end) {
                    unsigned cs = Rd32(d + q + 4);
                    if (!memcmp(d + q, "00dc", 4)) {
                        ++a.vChunks;
                        if (cs == 0) ++a.emptyV;
                        else { if (++nonEmpty == pickN) { a.pickOff = q + 8; a.pickSize = cs; } }
                    } else if (!memcmp(d + q, "01wb", 4)) { ++a.aChunks; a.aBytes += cs; }
                    q += 8 + cs + (cs & 1);
                }
            }
        } else if (!memcmp(&id, "idx1", 4)) a.idxCount = size / 16;
        pos = end + (size & 1);
    }
    *keep = d;
    return a;
}

// Compares a decoded picture (top-down BGRA) with the fixture shown on the screen (the whole fixture was recorded).
static bool PictureMatchesFixture(const uint8_t* px, int w, int h, int tol, int* checked)
{
    int bad = 0, n = 0;
    for (int i = 0; i < 300 && n < 16; ++i) {
        int x = R(0, w - 1), y = R(0, h - 1);
        int lx = (int)((x + 0.5) * g_fx.w / w), ly = (int)((y + 0.5) * g_fx.h / h);
        if (!SafePoint(lx, ly, g_fx.w, g_fx.h) || abs(lx - g_fx.w / 2) < 20 || abs(ly - g_fx.h / 2) < 20) continue;
        const uint8_t* q = px + ((size_t)y * w + x) * 4;
        COLORREF e = Expected(lx, ly, g_fx.w, g_fx.h);
        ++n; if (!Near(e, q[2], q[1], q[0], tol)) { ++bad; if (bad < 4) INFO(L"  picture (%d,%d) of %dx%d = %d,%d,%d, expected %d,%d,%d", x, y, w, h, q[2], q[1], q[0], GetRValue(e), GetGValue(e), GetBValue(e)); }
    }
    *checked = n;
    return n > 0 && bad == 0;
}

static void VerifyAvi(const wchar_t* path, int ew, int eh, double el, int fps, bool wantAudio, bool wantPicture, const wchar_t* desc)
{
    uint8_t* d = nullptr;
    AviInfo a = ReadAvi(path, 3, &d);
    wchar_t dd[260];
    _snwprintf(dd, 260, L"%dx%d, avih frames %u, %u video chunks (%u empty), %u audio chunks, idx1 %u, fps %u", a.w, a.h, a.total, a.vChunks, a.emptyV, a.aChunks, a.idxCount, a.fps);
    Check(a.ok && a.riffOk && a.w == ew && a.h == eh, L"recorder: AVI header is well formed and has the expected size", dd);
    Check(a.idxCount == a.vChunks + a.aChunks && a.total == a.vChunks, L"recorder: AVI index lists every chunk, the frame count is the video chunk count", dd);
    Check(a.fps == (unsigned)fps && fabs((double)a.total / (fps ? fps : 1) - el) < 1.2, L"recorder: AVI duration matches the recorded time", dd);
    // a picture that has not changed is an empty chunk (a repeat of the last frame): what counts is a real picture and a chunk for every frame of the rate
    Check(a.vChunks > a.emptyV && (double)a.vChunks > el * fps * 0.8, L"recorder: AVI holds real pictures at about the frame rate", dd);
    if (wantAudio) {
        if (a.aChunks > 0) Check(fabs((double)a.aBytes / 88200.0 - el) < 1.5, L"recorder: AVI sound is as long as the picture", dd);
        else WARN(L"recorder: no audio in the AVI (no microphone in this session?)");
    }
    if (wantPicture && a.pickSize > 0 && d) {
        wchar_t jp[MAX_PATH]; _snwprintf(jp, MAX_PATH, L"%s\\frame%d.jpg", g_dir, R(0, 999999));
        HANDLE h = CreateFileW(jp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, d + a.pickOff, a.pickSize, &wr, nullptr); CloseHandle(h); }
        int dw = 0, dh = 0; uint8_t* px = nullptr; int checked = 0;
        bool ok = DecodePixels(jp, &dw, &dh, &px) && dw == ew && dh == eh && PictureMatchesFixture(px, dw, dh, 40, &checked);
        Check(ok, L"recorder: a JPEG frame of the AVI has the colours of the screen in the right places", checked ? L"" : L"(no points)");
        delete[] px;
        DeleteFileW(jp);
    }
    (void)desc;
    delete[] d;
}

// ---- GIF: frame count, size, the delays and the colours of one frame -------------------------------------------------
static void VerifyGif(const wchar_t* path, int ew, int eh, double el, int fps, bool wantPicture)
{
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr; IWICBitmapDecoder* dec = nullptr;
    UINT frames = 0; double delaySum = 0; bool decoded = false, pictureOk = false; int checked = 0, fw = 0, fh = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f)) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrameCount(&frames)) && frames > 0) {
        decoded = true;
        for (UINT i = 0; i < frames; ++i) {
            IWICBitmapFrameDecode* fr = nullptr;
            if (FAILED(dec->GetFrame(i, &fr))) continue;
            IWICMetadataQueryReader* mq = nullptr;
            if (SUCCEEDED(fr->GetMetadataQueryReader(&mq)) && mq) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(mq->GetMetadataByName(L"/grctlext/Delay", &pv)) && pv.vt == VT_UI2) delaySum += pv.uiVal / 100.0;
                PropVariantClear(&pv);
                mq->Release();
            }
            if (i == (frames > 2 ? 2 : 0) && wantPicture) {
                IWICFormatConverter* cv = nullptr;
                if (SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                    UINT uw = 0, uh = 0; cv->GetSize(&uw, &uh); fw = (int)uw; fh = (int)uh;
                    uint8_t* px = new uint8_t[(size_t)uw * uh * 4];
                    if (SUCCEEDED(cv->CopyPixels(nullptr, uw * 4, uw * uh * 4, px))) pictureOk = PictureMatchesFixture(px, (int)uw, (int)uh, 60, &checked);
                    delete[] px;
                }
                if (cv) cv->Release();
            }
            fr->Release();
        }
    }
    if (dec) dec->Release(); if (f) f->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    wchar_t dd[200];
    _snwprintf(dd, 200, L"%u frames, delays %.2fs, recorded %.2fs, at most %d fps", frames, delaySum, el, fps);
    Check(decoded, L"recorder: the GIF decodes", dd);
    if (!decoded) return;
    Check(frames >= 2 && (double)frames > el * fps * 0.1 && (double)frames < el * fps * 1.5 + 4, L"recorder: the GIF has about as many frames as the frame rate gives", dd);
    if (delaySum > 0) Check(fabs(delaySum - el) < 1.5, L"recorder: the GIF frame delays add up to the recorded time", dd);
    if (wantPicture) {
        Check(fw == ew && fh == eh, L"recorder: the GIF has the expected size", dd);
        Check(pictureOk, L"recorder: a GIF frame has the colours of the screen in the right places", checked ? L"" : L"(no points)");
    }
}

static DWORD WINAPI PauseSpam(LPVOID p)
{
    Recorder* rec = (Recorder*)p;
    for (int i = 0; i < 40; ++i) { rec->SetPaused((i & 1) == 0); Sleep(R(1, 15)); }
    rec->SetPaused(false);
    return 0;
}

static void RecordOnce(bool useNull, bool sched = false)
{
    // The recorded area: the whole fixture (region), a random part of it, or the window itself.
    int mode = R(0, 2);             // 0 region = whole fixture, 1 region = part of it, 2 window
    MakeFixture(R(260, 900), R(200, 560));
    if (mode == 1 && g_fx.w < 300) mode = 0;
    RECT fr = FxScreenRect();
    RecParams p; memset(&p, 0, sizeof(p));
    p.rc = fr;
    int px = 0, py = 0, pw = g_fx.w, ph = g_fx.h;
    if (mode == 1) { pw = R(120, g_fx.w); ph = R(100, g_fx.h - 12); px = R(0, g_fx.w - pw); py = R(0, g_fx.h - ph - 12); p.rc.left = fr.left + px; p.rc.top = fr.top + py; p.rc.right = p.rc.left + pw; p.rc.bottom = p.rc.top + ph; }
    if (mode == 2) p.hwnd = g_fx.hw;
    RandomSettings(p.cfg);
    static const int fps[] = { 10, 15, 20, 25, 30, 45, 60 };
    p.cfg.fps = fps[R(0, 6)];
    const int fmt = p.cfg.videoCodec;
    const bool own = fmt == VC_AVI || fmt == VC_GIF;            // written by our own sinks: no Media Foundation, runs under Wine too
    if (own && !sched && VideoFormatAvailable(fmt)) useNull = false;
    const int effFps = fmt == VC_GIF && p.cfg.fps > 15 ? 15 : p.cfg.fps;
    p.cfg.capMouse = Chance(40); p.cfg.highlight = Chance(30);
    p.cfg.mic = !useNull && Chance(40);
    bool pause = Chance(40), spam = Chance(25), move = mode == 2 && Chance(40);
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\rec%d.%s", g_dir, R(0, 999999), VideoFormatExt(fmt));
    wcscpy(p.path, path);
    if (useNull) SetEnvironmentVariableW(L"C2S_NULLSINK", L"1"); else SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    wchar_t desc[300];
    _snwprintf(desc, 300, L"record (%s) %s %dx%d fps=%d codec=%d q=%d mic=%d pause=%d spam=%d move=%d", useNull ? L"no encoder" : own ? L"own sink" : L"MF",
               mode == 2 ? L"window" : mode == 1 ? L"part" : L"region", pw, ph, p.cfg.fps, p.cfg.videoCodec, p.cfg.videoQuality, p.cfg.mic, pause, spam, move);
    INFO(L"%s", desc);
    Recorder rec;
    wchar_t err[300] = L"";
    if (!rec.Start(p, err, 300)) { FAIL(L"%s: Start failed: %s", desc, err); SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr); return; }
    if (err[0]) WARN(L"%s: %s", desc, err);
    int ew, eh; int srcw = pw, srch = ph;
    if (mode == 2) { RECT vis; WindowVisibleRect(g_fx.hw, &vis); srcw = vis.right - vis.left; srch = vis.bottom - vis.top; }
    FitSizeForVideo(fmt, srcw, srch, ew, eh);
    Check(rec.Width() == ew && rec.Height() == eh, L"recorder: output size follows FitSize", desc);
    DWORD t0 = GetTickCount();
    int total = R(3, 5);
    double pausedFor = 0;
    HANDLE spamT = spam ? CreateThread(nullptr, 0, PauseSpam, &rec, 0, nullptr) : nullptr;
    int slice = 0;
    while ((int)(GetTickCount() - t0) < total * 1000 && !rec.LimitReached() && !rec.Failed()) {
        Pump(100);
        ++slice;
        if (move && slice % 5 == 0) {      // the recorded window moves and resizes while recording
            RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int nw = R(260, 700), nh = R(200, 480);
            SetWindowPos(g_fx.hw, HWND_TOPMOST, wa.left + R(0, wa.right - wa.left - nw), wa.top + R(0, wa.bottom - wa.top - nh), nw, nh, SWP_NOACTIVATE);
            g_fx.w = nw; g_fx.h = nh;
        }
        if (pause && slice == 8) { rec.SetPaused(true); DWORD pt = GetTickCount(); Pump(R(700, 1500)); pausedFor += (GetTickCount() - pt) / 1000.0; rec.SetPaused(false); }
    }
    if (spamT) { WaitForSingleObject(spamT, 5000); CloseHandle(spamT); }
    double wall = (GetTickCount() - t0) / 1000.0;
    double el = rec.ElapsedSec();
    unsigned long long frames = rec.FramesWritten();
    bool stopped = rec.Stop();
    bool failedSelf = rec.Failed();
    Check(stopped && !failedSelf, L"recorder: Stop finalizes the file", desc);
    if (!spam) {
        wchar_t d2[160]; _snwprintf(d2, 160, L"wall %.2fs, paused %.2fs, recorded %.2fs", wall, pausedFor, el);
        Check(fabs(el - (wall - pausedFor)) < 0.6, L"recorder: recorded time = wall time - pauses", d2);
    }
    if (useNull) {
        // frames at the requested rate while running (spam / pauses make the expected count a range)
        double lo = el * effFps * 0.4, hi = (el + 0.3) * effFps * 1.15 + 3;
        wchar_t d2[140]; _snwprintf(d2, 140, L"%llu frames in %.2fs at %d fps", frames, el, effFps);
        Check((double)frames <= hi && (double)frames >= (spam ? 1.0 : lo * 0.7), L"recorder: frame count follows the frame rate (no frames while paused)", d2);
        SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
        return;
    }
    // the file itself
    unsigned long long sz = FileSz(path);
    Check(sz > 1500, L"recorder: the video file exists and is not empty", desc);
    bool pictureOk = false, decoderMissing = false; int pictureChecked = 0;
    bool wantPicture = !move && p.cfg.capMouse == false;
    if (fmt == VC_AVI) { VerifyAvi(path, ew, eh, el, effFps, p.cfg.mic || p.cfg.loopback, wantPicture && mode != 1, desc); return; }
    if (fmt == VC_GIF) { VerifyGif(path, ew, eh, el, effFps, wantPicture && mode != 1); return; }
    VideoInfo v = Inspect(path, wantPicture, &pictureOk, &pictureChecked, &decoderMissing);
    wchar_t d3[200];
    _snwprintf(d3, 200, L"file %dx%d, %.2fs, %d frames, h264=%d hevc=%d wmv=%d audio=%d", v.w, v.h, v.dur, v.frames, v.h264, v.hevc, v.wmv, v.audio);
    bool codecOk = fmt == VC_H265 ? v.hevc : fmt == VC_WMV ? v.wmv : v.h264;
    Check(v.ok && codecOk && v.w == ew && v.h == eh, L"recorder: the video has the chosen codec at the expected size", d3);
    Check(fabs(v.dur - el) < (fmt == VC_WMV ? 1.6 : 0.9), L"recorder: the video duration matches the recorded time", d3);
    if (decoderMissing) WARN(L"recorder: no H.264 decoder here (Windows N edition?) - frames and picture not checked");
    else {
        double expect = el * effFps;
        Check(v.frames > expect * (g_scale > 1 ? 0.15 : 0.25) && v.frames < expect * 1.3 + 4, L"recorder: number of video frames is plausible for the frame rate", d3);
        if (wantPicture) {
            if (mode == 1) SKIP(L"recorder: picture check is for the whole fixture only");
            else Check(pictureOk, L"recorder: a decoded frame has the colours of the screen in the right places", pictureChecked ? L"" : L"(no points)");
        }
    }
    if (p.cfg.mic || p.cfg.loopback) {
        if (v.audio) Check(true, L"recorder: the video has the sound track (microphone and / or speakers)");
        else WARN(L"recorder: no audio track (no microphone in this session?)");
    }
}

static void T_Recorder()
{
    RecordOnce(!MfAvailable());
}

static void T_RecorderSched()
{
    RecordOnce(true, true);            // schedule / pauses, no encoder: works everywhere
}

static void T_RecorderLifecycle()
{
    // wrong things must be refused, not crash or hang
    RecParams p; memset(&p, 0, sizeof(p));
    SettingsDefaults(p.cfg); p.cfg.mic = false;
    wchar_t err[300];
    Recorder a;
    p.rc.left = 0; p.rc.top = 0; p.rc.right = 1; p.rc.bottom = 1;
    _snwprintf(p.path, MAX_PATH, L"%s\\tiny.mp4", g_dir);
    SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    Check(!a.Start(p, err, 300) && err[0], L"recorder: a 1x1 area is refused with a message");
    Recorder b;
    p.rc.right = 320; p.rc.bottom = 240;
    _snwprintf(p.path, MAX_PATH, L"%s\\no such folder\\x.mp4", g_dir);
    bool started = MfAvailable() ? b.Start(p, err, 300) : false;
    if (MfAvailable()) { Check(!started, L"recorder: an unwritable path is refused"); if (started) b.Stop(); }
    // start/stop cycles with the null sink: many quick runs, double Stop, Stop without Start
    SetEnvironmentVariableW(L"C2S_NULLSINK", L"1");
    int ok = 0, n = R(3, 8);
    for (int i = 0; i < n; ++i) {
        Recorder r;
        RecParams q = p; q.rc.right = R(64, 640); q.rc.bottom = R(48, 480); q.cfg.fps = R(10, 60);
        _snwprintf(q.path, MAX_PATH, L"%s\\cycle%d.mp4", g_dir, i);
        if (!r.Start(q, err, 300)) continue;
        Sleep(R(0, 300));
        if (Chance(50)) { r.SetPaused(true); Sleep(R(0, 100)); r.SetPaused(Chance(50)); }
        bool s1 = r.Stop(), s2 = r.Stop();
        if (s1 && !s2 && !r.Active()) ++ok;
    }
    Check(ok == n, L"recorder: quick start / stop cycles, a second Stop is harmless", L"");
    { Recorder idle; Check(!idle.Stop() && !idle.Active(), L"recorder: Stop without Start does nothing"); }
    SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
}

// ---- hotkeys -----------------------------------------------------------------------------------------------------
static volatile LONG g_hotkeyHits;
static LRESULT CALLBACK HkProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_HOTKEY) InterlockedIncrement(&g_hotkeyHits);
    return DefWindowProcW(hw, m, wp, lp);
}

// ---- hotkeys: which combinations are accepted, how they are named, how they are stored --------------------------------------
static void T_HotkeyKeys()
{
    auto code = [](UINT mod, UINT vk) { return (int)((mod << 16) | vk); };
    Check(!HotkeyUsable(code(0, VK_F12)) && HotkeyUsable(code(MOD_CONTROL, VK_F12)) && HotkeyUsable(code(MOD_CONTROL, VK_SNAPSHOT)) && HotkeyUsable(code(0, VK_F11)) && HotkeyUsable(code(MOD_ALT | MOD_SHIFT, '4')) &&
          HotkeyUsable(code(MOD_WIN, 'S')) && HotkeyUsable(code(0, VK_PAUSE)), L"hotkeys: usable combinations are accepted");
    Check(!HotkeyUsable(0) && !HotkeyUsable(code(0, 'A')) && !HotkeyUsable(code(0, '5')) && !HotkeyUsable(code(MOD_CONTROL, VK_CONTROL)) &&
          !HotkeyUsable(code(MOD_ALT, VK_SHIFT)) && !HotkeyUsable(code(0, VK_LBUTTON)) && !HotkeyUsable(code(0x100, 'A')) && !HotkeyUsable(-5),
          L"hotkeys: a plain letter, a modifier alone, a mouse button and unknown bits are refused");
    wchar_t n[80];
    HotkeyName(code(MOD_CONTROL, VK_SNAPSHOT), n, 80); bool a = Same(n, L"Ctrl+PrintScreen");
    HotkeyName(code(MOD_ALT | MOD_SHIFT, '4'), n, 80); bool b = Same(n, L"Alt+Shift+4");
    HotkeyName(code(0, VK_F11), n, 80); bool c = Same(n, L"F11");
    HotkeyName(code(MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, VK_F5), n, 80); bool d = Same(n, L"Ctrl+Alt+Shift+Win+F5");
    HotkeyName(0, n, 80); bool e = n[0] == 0;
    Check(a && b && c && d && e, L"hotkeys: the names read Ctrl+Alt+Shift+Win+Key and nothing for \"off\"");
    Settings def; SettingsDefaults(def);
    Check(def.hotkey[0] == code(MOD_CONTROL, VK_SNAPSHOT) && def.hotkey[1] == code(MOD_CONTROL, VK_F6) && def.hotkey[2] == code(MOD_CONTROL, VK_F7) &&
          def.hotkey[4] == code(MOD_CONTROL, VK_F8) && def.hotkey[5] == code(MOD_CONTROL, VK_F9) && def.hotkey[6] == code(MOD_CONTROL, VK_F10) &&
          def.hotkey[3] == code(MOD_CONTROL, VK_F11) && def.hotkey[7] == code(MOD_CONTROL, VK_F12),
          L"hotkeys: the defaults are Ctrl+PrintScreen for the desktop, then Ctrl+F6 ... Ctrl+F12 (region, window, record desktop / region / window, stop, drawing)");
    bool allUsable = true; for (int i = 0; i < HKI_COUNT; ++i) if (!HotkeyUsable(def.hotkey[i])) allUsable = false;
    Check(allUsable, L"hotkeys: every default is a usable combination");
    {
        HWND hp = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        wchar_t alone[200] = L"", withCtrl[200] = L"";
        for (int i = 0; i < 24 && hp; ++i) {
            wchar_t n[8]; _snwprintf(n, 8, L" F%d", i + 1);
            if (RegisterHotKey(hp, 9, MOD_NOREPEAT, VK_F1 + i)) UnregisterHotKey(hp, 9); else wcscat(alone, n);
            if (RegisterHotKey(hp, 9, MOD_CONTROL | MOD_NOREPEAT, VK_F1 + i)) UnregisterHotKey(hp, 9); else wcscat(withCtrl, n);
        }
        if (hp) DestroyWindow(hp);
        INFO(L"function keys this system refuses as hotkeys: alone [%s ], with Ctrl [%s ]", alone, withCtrl);
    }

    // stored values: off stays off, rubbish becomes the default
    SettingsUseKey(L"Software\\Click2Screen-Autotest");
    Settings a1 = def; a1.hotkey[1] = 0; a1.hotkey[3] = code(MOD_CONTROL | MOD_SHIFT, 'Q');
    SettingsSave(a1);
    Settings b1; SettingsLoad(b1);
    Check(b1.hotkey[1] == 0 && b1.hotkey[3] == a1.hotkey[3] && b1.hotkey[0] == def.hotkey[0], L"hotkeys: a switched-off hotkey and a changed one survive saving");
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD bad1 = 0x41, bad2 = 0xFFFFFFFF, bad3 = 0x00020000; // a plain letter, negative, a modifier without a key
        RegSetValueExW(k, L"HotkeyDesktop", 0, REG_DWORD, (BYTE*)&bad1, 4); RegSetValueExW(k, L"HotkeyRegion", 0, REG_DWORD, (BYTE*)&bad2, 4);
        RegSetValueExW(k, L"HotkeyWindow", 0, REG_DWORD, (BYTE*)&bad3, 4);
        RegCloseKey(k);
        Settings c1; SettingsLoad(c1);
        Check(c1.hotkey[0] == def.hotkey[0] && c1.hotkey[1] == def.hotkey[1] && c1.hotkey[2] == def.hotkey[2], L"hotkeys: unusable stored values fall back to the defaults");
    }
    // the ini file refuses them too
    Settings a2 = def; a2.hotkey[0] = code(MOD_ALT, 'Z');
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\hotkeys-%d.ini", g_dir, R(0, 99999));
    if (SettingsExport(a2, path)) {
        WritePrivateProfileStringW(L"Click2Screen", L"HotkeyRegion", L"65", path);             // a plain letter
        Settings b2 = def; b2.hotkey[0] = 0;
        bool im = SettingsImport(b2, path);
        Check(im && b2.hotkey[0] == a2.hotkey[0] && b2.hotkey[1] == def.hotkey[1], L"hotkeys: the ini file keeps a usable combination and ignores an unusable one");
    }
}

static void T_Hotkeys()
{
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = HkProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"C2SAutotestHotkeys";
        RegisterClassW(&wc); reg = true;
    }
    HWND hw = CreateWindowExW(0, L"C2SAutotestHotkeys", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    struct { int id; UINT mod; UINT vk; const wchar_t* name; } keys[] = {
        { 1, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT, L"Ctrl+PrintScreen" }, { 2, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '4', L"Alt+Shift+4" },
        { 3, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '5', L"Alt+Shift+5" }, { 4, MOD_NOREPEAT, VK_F11, L"F11" } };
    int k = R(0, 3);
    if (!RegisterHotKey(hw, keys[k].id, keys[k].mod, keys[k].vk)) {
        WARN(L"hotkey %s is taken by another program (Click2Screen itself running?)", keys[k].name);
        DestroyWindow(hw); return;
    }
    g_hotkeyHits = 0;
    INPUT in[8]; memset(in, 0, sizeof(in));
    int n = 0;
    auto key = [&](WORD vk, bool up) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; ++n; };
    if (keys[k].mod & MOD_CONTROL) key(VK_CONTROL, false);
    if (keys[k].mod & MOD_ALT) key(VK_MENU, false);
    if (keys[k].mod & MOD_SHIFT) key(VK_SHIFT, false);
    key((WORD)keys[k].vk, false); key((WORD)keys[k].vk, true);
    if (keys[k].mod & MOD_SHIFT) key(VK_SHIFT, true);
    if (keys[k].mod & MOD_ALT) key(VK_MENU, true);
    if (keys[k].mod & MOD_CONTROL) key(VK_CONTROL, true);
    UINT sent = SendInput(n, in, sizeof(INPUT));
    Pump(500);
    if (g_hotkeyHits > 0) PASS(L"hotkey %s registered and received (%ld)", keys[k].name, (long)g_hotkeyHits);
    else if (g_wine || sent == 0) SKIP(L"hotkey %s: key injection is not available here", keys[k].name);
    else WARN(L"hotkey %s: registered, but the injected keys were not delivered (locked desktop?)", keys[k].name);
    UnregisterHotKey(hw, keys[k].id);
    DestroyWindow(hw);
}

// ---- installer ---------------------------------------------------------------------------------------------------
static bool IsAdmin()
{
    HANDLE t = nullptr; TOKEN_ELEVATION el = {}; DWORD sz = 0; bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) { if (GetTokenInformation(t, TokenElevation, &el, sizeof(el), &sz)) r = el.TokenIsElevated != 0; CloseHandle(t); }
    return r;
}

static bool RunSetup(const wchar_t* args, DWORD* code)
{
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t* sl = wcsrchr(self, L'\\'); if (sl) *sl = 0;
    wchar_t cmd[MAX_PATH + 200];
    _snwprintf(cmd, MAX_PATH + 200, L"\"%s\\Click2Screen-Setup.exe\" %s", self, args);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

static bool g_installerAllowed;
static void T_Installer()
{
    if (!g_installerAllowed) { SKIP(L"installer: run with --installer (administrator) to test it"); return; }
    wchar_t pf[MAX_PATH] = L""; PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) { wcsncpy(pf, p, MAX_PATH - 1); CoTaskMemFree(p); }
    wchar_t exe[MAX_PATH]; _snwprintf(exe, MAX_PATH, L"%s\\Click2Screen\\Click2Screen.exe", pf);
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) { SKIP(L"installer: Click2Screen is already installed here; not touching it"); return; }
    wchar_t desk[MAX_PATH] = L"", menu[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_PublicDesktop, 0, nullptr, &p)) && p) { _snwprintf(desk, MAX_PATH, L"%s\\Click2Screen.lnk", p); CoTaskMemFree(p); }
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &p)) && p) { _snwprintf(menu, MAX_PATH, L"%s\\Click2Screen.lnk", p); CoTaskMemFree(p); }
    bool wantDesk = Chance(50), wantMenu = Chance(50), wantAuto = Chance(50);
    wchar_t args[200]; _snwprintf(args, 200, L"/install /silent%s%s%s", wantDesk ? L"" : L" /desktop-", wantMenu ? L"" : L" /startmenu-", wantAuto ? L" /autostart" : L"");
    INFO(L"installer: %s", args);
    DWORD code = 99;
    bool ran = RunSetup(args, &code);
    Check(ran && code == 0, L"installer: silent install returns 0");
    Check(GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES, L"installer: Click2Screen.exe is in Program Files");
    wchar_t sp[MAX_PATH]; _snwprintf(sp, MAX_PATH, L"%s\\Click2Screen\\Click2Screen-Setup.exe", pf);
    Check(GetFileAttributesW(sp) != INVALID_FILE_ATTRIBUTES, L"installer: the installer copy for removal is there");
    HKEY k; bool reg = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Click2Screen", 0, KEY_READ, &k) == ERROR_SUCCESS;
    if (reg) RegCloseKey(k);
    Check(reg, L"installer: registered in Apps & features");
    bool autoOn = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS && RegQueryValueExW(k, L"Click2Screen", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (k) RegCloseKey(k);
    Check(autoOn == wantAuto, L"installer: the autostart box is honoured");
    if (desk[0]) Check((GetFileAttributesW(desk) != INVALID_FILE_ATTRIBUTES) == wantDesk, L"installer: the desktop shortcut box is honoured");
    if (menu[0]) Check((GetFileAttributesW(menu) != INVALID_FILE_ATTRIBUTES) == wantMenu, L"installer: the Start menu shortcut box is honoured");
    // update in place, then remove
    code = 99; ran = RunSetup(L"/install /silent", &code);
    Check(ran && code == 0, L"installer: installing again (update) returns 0");
    code = 99; ran = RunSetup(L"/remove /silent", &code);
    Check(ran && code == 0, L"installer: silent removal returns 0");
    Sleep(4000);                      // the installer deletes its own copy after it exits
    Check(GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES, L"installer: the program is gone after the removal");
    reg = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Click2Screen", 0, KEY_READ, &k) == ERROR_SUCCESS;
    if (reg) RegCloseKey(k);
    Check(!reg, L"installer: the Apps & features entry is gone");
    if (desk[0]) Check(GetFileAttributesW(desk) == INVALID_FILE_ATTRIBUTES, L"installer: the desktop shortcut is gone");
    if (menu[0]) Check(GetFileAttributesW(menu) == INVALID_FILE_ATTRIBUTES, L"installer: the Start menu shortcut is gone");
}


// ==== more tests: the real program, leaks, threads, long paths, fuzzing ===========================================
static bool ValidSettings(const Settings& s, wchar_t* why)
{
#define NEED(c) if (!(c)) { _snwprintf(why, 100, L"%s", L"" #c); return false; }
    NEED(s.imageFormat >= 0 && s.imageFormat < IMG_COUNT) NEED(s.fps >= 5 && s.fps <= 60) NEED(s.delaySec >= 0 && s.delaySec <= 10)
    NEED(s.videoCodec >= 0 && s.videoCodec < VC_COUNT) NEED(s.audioCodec >= 0 && s.audioCodec <= 1)
    NEED(s.videoQuality >= 0 && s.videoQuality <= 2) NEED(s.audioQuality >= 0 && s.audioQuality <= 2)
    NEED(s.language >= 0 && s.language <= LangCount()) NEED(TemplateValid(s.fileName))
    NEED(wcslen(s.folder) < MAX_PATH)
    return true;
}

static void T_FuzzSettings()
{
    static const wchar_t* names[] = { L"OutputFolder", L"VidOutputFolder", L"FileName", L"ImageFormat", L"CapMouse", L"Highlight",
        L"EnableSound", L"EnableMicRec", L"OpenOutFolder", L"RunAtStartup", L"Delay", L"VideoCodec", L"AudioCodec", L"VideoQuality", L"AudioQuality", L"Fps", L"Language" };
    HKEY k;
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) { WARN(L"fuzz: cannot create the registry key"); return; }
    for (auto nm : names) {
        switch (R(0, 5)) {
            case 0: { DWORD v = (DWORD)NextU(); RegSetValueExW(k, nm, 0, REG_DWORD, (BYTE*)&v, 4); break; }
            case 1: { wchar_t t[700]; int n = R(0, 650); for (int i = 0; i < n; ++i) t[i] = (wchar_t)R(32, 0x2000); t[n] = 0; RegSetValueExW(k, nm, 0, REG_SZ, (BYTE*)t, (n + 1) * 2); break; }
            case 2: { BYTE b[40]; int n = R(0, 39); for (int i = 0; i < n; ++i) b[i] = (BYTE)NextU(); RegSetValueExW(k, nm, 0, REG_BINARY, b, n); break; }
            case 3: { wchar_t t[16]; RandomText(t, 10); RegSetValueExW(k, nm, 0, REG_SZ, (BYTE*)t, (DWORD)((wcslen(t) + 1) * 2)); break; }
            case 4: { DWORD v = R(0, 80); RegSetValueExW(k, nm, 0, REG_DWORD, (BYTE*)&v, 4); break; }
            default: break;                                     // missing
        }
    }
    RegCloseKey(k);
    Settings s; SettingsLoad(s);
    wchar_t why[120] = L"";
    Check(ValidSettings(s, why), L"fuzz: garbage in the registry still loads as valid settings", why);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest");
}

static void T_FuzzIni()
{
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\fuzz-%d.ini", g_dir, R(0, 99999));
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { WARN(L"fuzz: cannot create %s", path); return; }
    DWORD w;
    int kind = R(0, 3);
    if (kind == 0) {                       // random bytes
        BYTE b[600]; int n = R(0, 600); for (int i = 0; i < n; ++i) b[i] = (BYTE)NextU(); WriteFile(h, b, n, &w, nullptr);
    } else {                               // a Click2Screen section with random lines, ANSI or UTF-16
        wchar_t text[3000] = L"\xFEFF[Click2Screen]\r\n";
        static const wchar_t* keys[] = { L"Fps", L"Delay", L"FileName", L"OutputFolder", L"ImageFormat", L"CapMouse", L"VideoCodec", L"Junk", L"VidOutputFolder", L"Language" };
        int lines = R(0, 25);
        for (int i = 0; i < lines; ++i) {
            wchar_t line[400]; const wchar_t* key = keys[R(0, 9)];
            switch (R(0, 3)) {
                case 0: _snwprintf(line, 400, L"%s=%d\r\n", key, R(-100000, 100000)); break;
                case 1: { wchar_t t[40]; RandomText(t, 30); _snwprintf(line, 400, L"%s=%s\r\n", key, t); break; }
                case 2: _snwprintf(line, 400, L"%s=\r\n", key); break;
                default: _snwprintf(line, 400, L"%s=%s%s\r\n", key, L"99999999999999999999", L"x"); break;
            }
            if (wcslen(text) + wcslen(line) < 2990) wcscat(text, line);
        }
        if (kind == 2) { char a[3000]; int n = WideCharToMultiByte(CP_ACP, 0, text + 1, -1, a, 3000, nullptr, nullptr); WriteFile(h, a, n > 0 ? n - 1 : 0, &w, nullptr); }
        else WriteFile(h, text, (DWORD)(wcslen(text) * 2), &w, nullptr);
    }
    CloseHandle(h);
    Settings s; SettingsDefaults(s);
    bool r = SettingsImport(s, path);
    wchar_t why[120] = L"";
    wchar_t nm[100]; _snwprintf(nm, 100, L"fuzz: a random .ini (kind %d) is refused or applied with valid values only", kind);
    Check(ValidSettings(s, why), nm, r ? L"(imported)" : L"(refused)");
}

static void T_LongPaths()
{
    int bad = 0;
    for (int i = 0; i < 40; ++i) {
        int plen = R(1, MAX_PATH);
        wchar_t* buf = new wchar_t[plen + 32];
        for (int j = 0; j < plen + 32; ++j) buf[j] = 0x7777;
        wchar_t folder[400]; _snwprintf(folder, 400, L"%s", g_dir);
        int extra = R(0, 3) == 0 ? R(100, 300) : R(0, 30);
        for (int j = 0; j < extra && wcslen(folder) < 380; ++j) wcscat(folder, j % 17 == 16 ? L"\\" : L"d");
        wchar_t tpl[128] = L"";
        int tl = R(1, 100); for (int j = 0; j < tl; ++j) wcscat(tpl, j == 5 ? L"#" : (Chance(10) ? L"$Y" : L"n"));
        if (!TemplateValid(tpl)) wcscpy(tpl, L"x#");
        wchar_t err[300] = L"";
        bool ok = BuildOutputPath(folder, tpl, L"png", nullptr, buf, plen, err, 300);
        bool terminated = false;
        for (int j = 0; j < plen; ++j) if (buf[j] == 0) { terminated = true; break; }
        bool canary = true;
        for (int j = plen; j < plen + 32; ++j) if (buf[j] != 0x7777) canary = false;
        if (!canary || (ok && !terminated)) { ++bad; Rep(L"INFO", L"  path buffer %d: ok=%d terminated=%d canary=%d", plen, (int)ok, (int)terminated, (int)canary); }
        delete[] buf;
    }
    Check(bad == 0, L"paths: BuildOutputPath never writes beyond the caller's buffer and terminates the string (40 random)");
}

static DWORD g_threadOk, g_threadBad;
static DWORD WINAPI GrabThread(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    RECT fr = FxScreenRect();
    int cw = R(80, g_fx.w), ch = R(60, g_fx.h - 14);
    Grabber g;
    int ow = (cw + 1) & ~1, oh = (ch + 1) & ~1;
    if (!g.Init(ow, oh)) { InterlockedIncrement((LONG*)&g_threadBad); return 1; }
    RECT rc = { fr.left, fr.top, fr.left + cw, fr.top + ch };
    for (int i = 0; i < 25; ++i) {
        if (!g.Grab(rc, Chance(30), false)) { InterlockedIncrement((LONG*)&g_threadBad); continue; }
        HBITMAP b = nullptr;
        int r, gg, bl;
        // sample the middle of the top-left quarter: red (the pointer is far away)
        int x = cw / 5, y = ch / 5;
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi)); (void)bi;
        const uint8_t* p = g.Bits() + ((size_t)(oh - 1 - y) * ow + x) * 4;
        bl = p[0]; gg = p[1]; r = p[2]; (void)b;
        if (SafePoint(x, y, g_fx.w, g_fx.h) && !Near(Expected(x, y, g_fx.w, g_fx.h), r, gg, bl, 24)) InterlockedIncrement((LONG*)&g_threadBad);
        else InterlockedIncrement((LONG*)&g_threadOk);
    }
    CoUninitialize();
    return 0;
}

static void T_Threads()
{
    MakeFixture(R(400, 900), R(300, 560));
    SetCursorPos(2, 2);
    g_threadOk = g_threadBad = 0;
    int n = R(2, 6);
    HANDLE th[8];
    for (int i = 0; i < n; ++i) th[i] = CreateThread(nullptr, 0, GrabThread, nullptr, 0, nullptr);
    bool done = true;
    for (int i = 0; i < n; ++i) { if (WaitForSingleObject(th[i], 60000) != WAIT_OBJECT_0) done = false; CloseHandle(th[i]); }
    wchar_t d[80]; _snwprintf(d, 80, L"%d threads, %lu good frames, %lu bad", n, (unsigned long)g_threadOk, (unsigned long)g_threadBad);
    Check(done && g_threadBad == 0 && g_threadOk > 0, L"threads: parallel captures give correct pictures", d);
}

static void T_Leaks()
{
    MakeFixture(R(300, 700), R(220, 500));
    SetEnvironmentVariableW(L"C2S_NULLSINK", L"1");
    auto round = [&](int n) {
        for (int i = 0; i < n; ++i) {
            RECT fr = FxScreenRect();
            RECT rc = { fr.left, fr.top, fr.left + R(50, g_fx.w), fr.top + R(50, g_fx.h) };
            HBITMAP b = CaptureRect(rc, Chance(50), Chance(50));
            wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\leak.png", g_dir);
            if (b) { SaveBitmap(b, path, R(0, 2)); DeleteObject(b); }
            DeleteFileW(path);
            if (i % 3 == 0) {
                Recorder r; RecParams p; memset(&p, 0, sizeof(p)); SettingsDefaults(p.cfg); p.cfg.mic = false; p.cfg.fps = R(10, 60);
                p.rc = rc; wcscpy(p.path, path);
                wchar_t err[200];
                if (r.Start(p, err, 200)) { Sleep(R(20, 120)); r.Stop(); }
            }
        }
    };
    round(12);                                         // warm-up: caches, COM, WIC
    DWORD gdi0 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), usr0 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), h0 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h0);
    round(R(30, 60));
    DWORD gdi1 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), usr1 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), h1 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h1);
    SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    wchar_t d[160]; _snwprintf(d, 160, L"GDI %lu -> %lu, USER %lu -> %lu, handles %lu -> %lu", (unsigned long)gdi0, (unsigned long)gdi1, (unsigned long)usr0, (unsigned long)usr1, (unsigned long)h0, (unsigned long)h1);
    if (gdi0 == 0 && usr0 == 0 && g_wine) { SKIP(L"leaks: Wine does not count GDI / USER objects (%s)", d); return; }
    Check(gdi1 <= gdi0 + 6 && usr1 <= usr0 + 6 && h1 <= h0 + 30, L"leaks: GDI / USER objects and handles do not grow over many captures and recordings", d);
}

static void T_LongRecord()
{
    MakeFixture(R(300, 700), R(220, 500));
    SetEnvironmentVariableW(L"C2S_NULLSINK", L"1");
    RecParams p; memset(&p, 0, sizeof(p));
    SettingsDefaults(p.cfg); p.cfg.mic = false;
    static const int f[] = { 10, 15, 24, 25, 30, 60 };
    p.cfg.fps = f[R(0, 5)];
    p.rc = FxScreenRect();
    wcscpy(p.path, g_dir); wcscat(p.path, L"\\long.mp4");
    Recorder rec; wchar_t err[200];
    int secs = R(15, 35);
    INFO(L"long recording (no encoder): %d s at %d fps", secs, p.cfg.fps);
    if (!rec.Start(p, err, 200)) { FAIL(L"long recording: Start failed: %s", err); SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr); return; }
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < secs * 1000) Pump(200);
    double el = rec.ElapsedSec(); uint64_t fr = rec.FramesWritten();
    rec.Stop();
    SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    // The frame rate a machine can sustain is limited by the cost of one capture (slow under Wine / Xvfb): measure it.
    double cost = 0;
    {
        Grabber g; RECT rc = FxScreenRect();
        if (g.Init((rc.right - rc.left + 1) & ~1, (rc.bottom - rc.top + 1) & ~1)) {
            LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
            for (int i = 0; i < 20; ++i) g.Grab(rc, p.cfg.capMouse, p.cfg.highlight, true);        // the same work as the recorder does for a frame
            QueryPerformanceCounter(&b);
            cost = (double)(b.QuadPart - a.QuadPart) / f.QuadPart / 20.0;
        }
    }
    // The recorder takes a frame on a boundary of the frame interval, so a capture that is longer than one interval costs two (or more) of them.
    double interval = 1.0 / p.cfg.fps;
    double capacity = cost > 0 ? 1.0 / (ceil(cost * 1.5 / interval) * interval) : 1e9;           // frames per second the capture can deliver
    double rate = p.cfg.fps < capacity ? p.cfg.fps : capacity;
    double expect = el * rate;
    wchar_t d[200]; _snwprintf(d, 200, L"%llu frames in %.2f s at %d fps; one capture takes %.1f ms, so about %.0f frames/s possible (%.0f%% of that)", (unsigned long long)fr, el, p.cfg.fps, cost * 1000, capacity, 100.0 * fr / (el * rate));
    Check(fabs(el - secs) < 1.0 && fr >= expect * (p.cfg.fps > capacity * 0.9 ? 0.7 : 0.85) && fr <= el * p.cfg.fps * 1.05 + 2, L"long recording: no drift - the frame count follows the clock and the capture speed", d);
}

// ---- driving the real program ---------------------------------------------------------------------------------------
struct FindCtx { DWORD pid; const wchar_t* cls; HWND found; };
static BOOL CALLBACK FindProc(HWND hw, LPARAM lp)
{
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
    if (pid != c->pid) return TRUE;
    wchar_t cls[64]; GetClassNameW(hw, cls, 64);
    if (!wcscmp(cls, c->cls)) { c->found = hw; return FALSE; }
    return TRUE;
}
static HWND FindWin(DWORD pid, const wchar_t* cls, int waitMs)
{
    DWORD end = GetTickCount() + waitMs * g_scale;
    do {
        FindCtx c = { pid, cls, nullptr };
        EnumWindows(FindProc, (LPARAM)&c);
        if (c.found) return c.found;
        Pump(50);
    } while ((int)(end - GetTickCount()) > 0);
    return nullptr;
}
// A window found by class may still be under construction (its controls are created right after it): wait until it is shown.
static HWND FindReady(DWORD pid, const wchar_t* cls, int waitMs)
{
    HWND w = FindWin(pid, cls, waitMs);
    DWORD end = GetTickCount() + waitMs * g_scale;
    while (w && !IsWindowVisible(w) && (int)(end - GetTickCount()) > 0) Pump(30);
    if (w) Pump(30);
    return w && IsWindowVisible(w) ? w : nullptr;
}
// SetWindowText does not change the text of a control that belongs to another process (the documentation says so; Wine
// forwards it anyway): the message does.
static void SetText(HWND w, const wchar_t* t) { DWORD_PTR r = 0; SendMessageTimeoutW(w, WM_SETTEXT, 0, (LPARAM)t, SMTO_ABORTIFHUNG, 5000, &r); }
static void WinText(HWND w, wchar_t* out, int n) { out[0] = 0; SendMessageTimeoutW(w, WM_GETTEXT, n, (LPARAM)out, SMTO_ABORTIFHUNG, 3000, nullptr); }
static bool WaitText(HWND parent, int id, const wchar_t* want, int waitMs)
{
    DWORD end = GetTickCount() + waitMs * g_scale;
    wchar_t t[400];
    do { WinText(GetDlgItem(parent, id), t, 400); if (!wcscmp(t, want)) return true; Pump(50); } while ((int)(end - GetTickCount()) > 0);
    return false;
}
// A modal message box of the program (class #32770 of its process): confirm it.
static bool DismissBox(DWORD pid, int button, int waitMs)
{
    HWND dlg = FindWin(pid, L"#32770", WMS(waitMs));       // scaled under Wine (a recording in AVI / GIF keeps the CPU busy)
    if (!dlg) return false;
    {   // what the box says: a box nobody expected must be explained by the log
        wchar_t title[120] = L"", text[300] = L""; WinText(dlg, title, 120);
        HWND st = GetDlgItem(dlg, 0xFFFF); if (st) WinText(st, text, 300);
        Rep(L"INFO", L"  a box is closed by the test: [%s] %s", title, text);
    }
    PostMessageW(dlg, WM_COMMAND, button, 0);
    return true;
}

static int CountFiles(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') ++n; } while (FindNextFileW(h, &fd));   // (".c2s-probe-*": the program tries the folder for writing)
    FindClose(h);
    return n;
}

struct App { PROCESS_INFORMATION pi; HWND main; Settings cfg; wchar_t img[MAX_PATH], vid[MAX_PATH]; };

static bool StartApp(App& a, const Settings& s, const wchar_t* extraArgs = L"", bool realEncoder = false)
{
    memset(&a, 0, sizeof(a));
    a.cfg = s;
    SettingsSave(s);                                  // into the test key; the program reads it through C2S_REGKEY
    SetEnvironmentVariableW(L"C2S_REGKEY", L"Software\\Click2Screen-Autotest");
    wchar_t inst[40]; _snwprintf(inst, 40, L"-at%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"C2S_INSTANCE", inst);
    if (realEncoder) SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr); else SetEnvironmentVariableW(L"C2S_NULLSINK", L"1");
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t* sl = wcsrchr(self, L'\\'); if (sl) *sl = 0;
    wchar_t cmd[MAX_PATH + 100]; _snwprintf(cmd, MAX_PATH + 100, L"\"%s\\Click2Screen.exe\" %s", self, extraArgs);
    STARTUPINFOW si = { sizeof(si) };
    bool ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &a.pi) != FALSE;
    SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    if (!ok) return false;
    a.main = FindReady(a.pi.dwProcessId, C2S_CLASS_MAIN, 15000);          // shown = hotkeys, tray and controls are all set up
    return a.main != nullptr;
}
static void StopApp(App& a, bool graceful = true)
{
    if (!a.pi.hProcess) return;
    if (graceful && a.main && IsWindow(a.main)) PostMessageW(a.main, WM_COMMAND, ID_TRAY_EXIT, 0);
    if (WaitForSingleObject(a.pi.hProcess, graceful ? 10000 : 100) != WAIT_OBJECT_0) TerminateProcess(a.pi.hProcess, 99);
    CloseHandle(a.pi.hProcess); CloseHandle(a.pi.hThread);
    memset(&a.pi, 0, sizeof(a.pi));
}
static bool AppAlive(App& a) { DWORD c = 0; return GetExitCodeProcess(a.pi.hProcess, &c) && c == STILL_ACTIVE; }

static void AppSettings(Settings& s, int language)
{
    RandomSettings(s);
    s.language = language;
    s.sounds = false; s.openFolder = false; s.runAtStartup = false;
    s.delaySec = R(0, 2);
    s.mic = false;
}

static int HkIndexOf(int id) { return id == IDC_SHOT_DESKTOP ? 0 : id == IDC_SHOT_REGION ? 1 : id == IDC_SHOT_WINDOW ? 2 : id == IDC_REC_DESKTOP ? 4 : id == IDC_REC_REGION ? 5 : id == IDC_REC_WINDOW ? 6 : -1; }

// What the program writes on a button: the text of the language with the hotkey in use in the brackets of the second line.
static void ExpectedButtonText(Str s, int hkIndex, const int* hk, wchar_t* out, int cap)
{
    wcsncpy(out, T(s), cap - 1); out[cap - 1] = 0;
    if (hkIndex < 0) return;
    wchar_t* nl = wcschr(out, L'\n');
    if (nl) *nl = 0;
    wchar_t n[80]; HotkeyName(hk[hkIndex], n, 80);
    if (n[0]) { wcscat(out, L"\n("); wcscat(out, n); wcscat(out, L")"); }
}

static void AppTexts(App& a, int language, const wchar_t* tag, const int* hk)
{
    LangSelect(language);
    struct { int id; Str s; } t[] = { {IDC_REC_DESKTOP, S_REC_DESKTOP}, {IDC_REC_REGION, S_REC_REGION}, {IDC_REC_WINDOW, S_REC_WINDOW}, {IDC_SHOT_DESKTOP, S_SHOT_DESKTOP},
        {IDC_SHOT_REGION, S_SHOT_REGION}, {IDC_SHOT_WINDOW, S_SHOT_WINDOW}, {IDC_MENU, S_OPTIONS} };
    int bad = 0;
    DWORD end = GetTickCount() + WMS(8000);                   // the program rebuilds its texts a moment after the change
    for (;;) {
        bad = 0;
        for (auto& i : t) { wchar_t got[400], want[400]; WinText(GetDlgItem(a.main, i.id), got, 400); ExpectedButtonText(i.s, HkIndexOf(i.id), hk, want, 400); if (wcscmp(got, want)) ++bad; }
        wchar_t title[200]; WinText(a.main, title, 200);
        if (wcscmp(title, T(S_APP))) ++bad;
        if (!bad || (int)(end - GetTickCount()) <= 0) break;
        Sleep(100);
    }
    if (bad) for (auto& i : t) {
        wchar_t got[400], want[400]; WinText(GetDlgItem(a.main, i.id), got, 400); ExpectedButtonText(i.s, HkIndexOf(i.id), hk, want, 400);
        if (wcscmp(got, want)) Rep(L"INFO", L"  control %d: [%s] expected [%s]", i.id, got, want);
    }
    wchar_t nm[160]; _snwprintf(nm, 160, L"program: the main window speaks language %d (%s)", language, tag);
    Check(bad == 0, nm);
}

static void T_App()
{
    MakeFixture(R(300, 600), R(200, 400));
    Settings s; int language = R(0, LangCount());
    AppSettings(s, language);
    // the program's folders: fresh ones
    wchar_t t[24]; _snwprintf(t, 24, L"app%d", R(0, 99999));
    _snwprintf(s.folder, MAX_PATH, L"%s\\%s\\out", g_dir, t);
    App a;
    if (!StartApp(a, s)) { FAIL(L"program: Click2Screen.exe did not show its window"); StopApp(a, false); return; }
    INFO(L"program flow: language %d, delay %d s", language, s.delaySec);
    LangSelect(language == 0 ? 0 : language);
    AppTexts(a, language, L"at start", s.hotkey);

    // a second copy hands over to the first and ends
    {
        App b; memset(&b, 0, sizeof(b));
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t* sl = wcsrchr(self, L'\\'); if (sl) *sl = 0;
        wchar_t cmd[MAX_PATH + 40]; _snwprintf(cmd, MAX_PATH + 40, L"\"%s\\Click2Screen.exe\"", self);
        STARTUPINFOW si = { sizeof(si) };
        SetEnvironmentVariableW(L"C2S_REGKEY", L"Software\\Click2Screen-Autotest");
        wchar_t inst[40]; _snwprintf(inst, 40, L"-at%lu", GetCurrentProcessId()); SetEnvironmentVariableW(L"C2S_INSTANCE", inst);
        bool ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &b.pi) != FALSE;
        DWORD code = 99;
        bool ended = ok && WaitForSingleObject(b.pi.hProcess, 10000) == WAIT_OBJECT_0 && GetExitCodeProcess(b.pi.hProcess, &code) && code == 0;
        if (ok) { CloseHandle(b.pi.hProcess); CloseHandle(b.pi.hThread); }
        Check(ended && AppAlive(a), L"program: a second copy hands over to the first one and exits at once");
    }

    // a screenshot with the hotkey message
    {
        int before = CountFiles(s.folder);
        PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0);
        DWORD end = GetTickCount() + WMS(20000);
        while (CountFiles(s.folder) <= before && (int)(end - GetTickCount()) > 0) Sleep(100);
        wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*.*", s.folder);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        wchar_t file[MAX_PATH] = L"";
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') _snwprintf(file, MAX_PATH, L"%s\\%s", s.folder, fd.cFileName); } while (FindNextFileW(h, &fd)); FindClose(h); }
        // the file shows up while the program is still writing it: wait until its size stops changing
        unsigned long long last = 0; int stable = 0;
        for (int i = 0; i < 100 && stable < 4 && file[0]; ++i) { unsigned long long z = FileSz(file); if (z == last && z > 0) ++stable; else stable = 0; last = z; Sleep(150); }
        int dw = 0, dh = 0; uint8_t* px = nullptr;
        bool d = file[0] && DecodePixels(file, &dw, &dh, &px);
        RECT vs = VirtualScreen();
        wchar_t dd[200]; _snwprintf(dd, 200, L"%s %dx%d, screen %dx%d", file, dw, dh, (int)(vs.right - vs.left), (int)(vs.bottom - vs.top));
        Check(d && dw == vs.right - vs.left && dh == vs.bottom - vs.top && AppAlive(a), L"program: the desktop hotkey saves a picture of the whole screen", dd);
        delete[] px;
        // the sequence number / template of the name is the program's own: the extension follows the format
        wchar_t want[16]; _snwprintf(want, 16, L".%s", ImageFormatExt(s.imageFormat));
        size_t fl = wcslen(file), el = wcslen(want);
        Check(fl > el && !_wcsicmp(file + fl - el, want), L"program: the file has the extension of the chosen format");
    }

    // a recording through the panel
    {
        LangSelect(language);
        if (Chance(50)) PostMessageW(a.main, WM_HOTKEY, HK_REC_DESKTOP, 0); else PostMessageW(a.main, WM_COMMAND, IDC_REC_DESKTOP, 0);       // the hotkey or the button
        HWND panel = FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000);
        Check(panel != nullptr, L"program: Record desktop opens the recording panel");
        if (panel) {
            Check(!IsWindowVisible(a.main), L"program: the main window steps aside while the panel is shown");
            Check(WaitText(panel, IDC_P_START, T(S_START), 2000), L"program: the panel offers Start");
            PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0);
            bool rec = WaitText(panel, IDC_P_START, T(S_PAUSE), (s.delaySec + 6) * 1000);
            Check(rec, L"program: after the countdown the recording runs (the button offers Pause)");
            if (rec) {
                Sleep(1300);
                wchar_t t1[100], t2[100];
                WinText(GetDlgItem(panel, IDC_P_TIME), t1, 100); Sleep(1200); WinText(GetDlgItem(panel, IDC_P_TIME), t2, 100);
                Check(wcscmp(t1, t2) != 0, L"program: the time label runs while recording", t2);
                {
                    PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0);
                    Check(WaitText(panel, IDC_P_START, T(S_RESUME), 3000), L"program: Pause -> the button offers Resume");
                    WinText(GetDlgItem(panel, IDC_P_TIME), t1, 100); Sleep(1200); WinText(GetDlgItem(panel, IDC_P_TIME), t2, 100);
                    Check(!wcscmp(t1, t2), L"program: the time label stands still while paused", t2);
                    PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0);
                    Check(WaitText(panel, IDC_P_START, T(S_PAUSE), 3000), L"program: Resume -> recording again");
                    if (Chance(40)) {                          // hide the panel, bring it back from the tray menu command
                        PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_HIDE, BN_CLICKED), 0);
                        bool asked = DismissBox(a.pi.dwProcessId, IDYES, 5000);
                        DWORD hideEnd = GetTickCount() + WMS(3000);
                        while (IsWindowVisible(panel) && (int)(hideEnd - GetTickCount()) > 0) Sleep(50);
                        Check(asked && !IsWindowVisible(panel), L"program: Hide asks once and hides the panel");
                        PostMessageW(a.main, WM_COMMAND, ID_TRAY_PANEL, 0);
                        Sleep(300);
                        Check(IsWindowVisible(panel), L"program: the tray command shows the panel again");
                    }
                    if (Chance(50)) PostMessageW(a.main, WM_HOTKEY, HK_STOP, 0);                     // F11
                    else PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0);    // the Stop button
                }
                DWORD end = GetTickCount() + WMS(12000);
                while (FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) && (int)(end - GetTickCount()) > 0) Sleep(100);
                Check(FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) == nullptr, L"program: Stop closes the panel");
                Sleep(300);
                wchar_t st[500], prefix[100]; WinText(GetDlgItem(a.main, IDC_STATUS), st, 500);
                wcsncpy(prefix, T(S_SAVED), 99); prefix[99] = 0; wchar_t* pc = wcsstr(prefix, L"%s"); if (pc) *pc = 0;
                Check(IsWindowVisible(a.main) && wcsncmp(st, prefix, wcslen(prefix)) == 0, L"program: the main window is back and reports the saved file", st);
            }
        }
    }

    // the drawing hotkey switches the drawing on and off; the program stays alive and answers
    {
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0); Pump(300);
        bool onAlive = AppAlive(a);
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0); Pump(300);
        Check(onAlive && AppAlive(a), L"program: the drawing hotkey can be switched on and off");
        // the drawing window: it covers the screen, the left button draws, the right one wipes, Esc leaves
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0);
        HWND ink = FindWin(a.pi.dwProcessId, C2S_CLASS_INK, 4000);
        { DWORD vis = GetTickCount() + WMS(4000); while (ink && !IsWindowVisible(ink) && (int)(vis - GetTickCount()) > 0) Sleep(50); }     // found while it is still being created: wait until it is shown
        RECT vs = VirtualScreen(), ir = {};
        if (ink) GetWindowRect(ink, &ir);
        LONG iex = ink ? GetWindowLongW(ink, GWL_EXSTYLE) : 0;
        wchar_t id[160]; _snwprintf(id, 160, L"window %d,%d %dx%d, the screen %d,%d %dx%d, topmost %d", (int)ir.left, (int)ir.top, (int)(ir.right - ir.left), (int)(ir.bottom - ir.top), (int)vs.left, (int)vs.top, (int)(vs.right - vs.left), (int)(vs.bottom - vs.top), (iex & WS_EX_TOPMOST) != 0);
        Check(ink && IsWindowVisible(ink) && ir.left == vs.left && ir.top == vs.top && ir.right == vs.right && ir.bottom == vs.bottom && (iex & WS_EX_TOPMOST), L"program: the drawing on puts a topmost window over the whole screen that takes the mouse", id);
        if (ink) {
            int x0 = 60, y0 = 60;
            PostMessageW(ink, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
            for (int i = 1; i <= 12; ++i) PostMessageW(ink, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x0 + i * 9, y0 + i * 5));
            PostMessageW(ink, WM_LBUTTONUP, 0, MAKELPARAM(x0 + 108, y0 + 60));
            HWND tr = nullptr; DWORD te = GetTickCount() + WMS(g_scale > 1 ? 8000 : 3000);
            RECT trc = {}; bool placed = false;
            while ((int)(te - GetTickCount()) > 0) {
                tr = FindWin(a.pi.dwProcessId, L"Click2ScreenTrail", 0);
                if (tr && IsWindowVisible(tr)) { GetWindowRect(tr, &trc); placed = trc.left <= vs.left + x0 + 2 && trc.right >= vs.left + x0 + 100; if (placed) break; }
                Sleep(50);
            }
            Check(placed, L"program: a stroke of the left button on the drawing window shows as a red line on the screen");
            if (AffinityReported()) Check(tr && AffinityOf(tr) == 0, L"program: without a recording the line is part of the screen (not excluded from capture), so a screen sharing sees it");
            else SKIP(L"program: this Windows does not report the exclusion from capture");
            PostMessageW(ink, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(200, 200));
            DWORD we = GetTickCount() + WMS(3000);
            while (tr && IsWindowVisible(tr) && (int)(we - GetTickCount()) > 0) Sleep(50);
            Check(!tr || !IsWindowVisible(tr), L"program: the right button on the drawing window wipes the lines");
            PostMessageW(ink, WM_KEYDOWN, VK_ESCAPE, 0);
            DWORD ee = GetTickCount() + WMS(3000);
            while (IsWindow(ink) && (int)(ee - GetTickCount()) > 0) Sleep(50);
            Check(!IsWindow(ink) && AppAlive(a), L"program: Esc closes the drawing window");
        }
        // once more, the other way round
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0);
        HWND ink2 = FindWin(a.pi.dwProcessId, C2S_CLASS_INK, 4000);
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0);
        DWORD me2 = GetTickCount() + WMS(3000);
        while (ink2 && IsWindow(ink2) && (int)(me2 - GetTickCount()) > 0) Sleep(50);
        Check(ink2 && !IsWindow(ink2), L"program: the hotkey switches the drawing window on and off (and off again)");
    }
    // "Restore default" sets back the page that is open only
    {
        PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
        HWND o = FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 10000);
        Check(o != nullptr, L"program: Options opens (restore default test)");
        if (o) {
            {   // the outline / waves box depends on "show the pointer": greyed while that one is off
                bool pointerOn = SendMessageW(GetDlgItem(o, O_MOUSE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool en0 = IsWindowEnabled(GetDlgItem(o, O_HILITE)) != 0;
                SendMessageW(GetDlgItem(o, O_MOUSE), BM_CLICK, 0, 0); Sleep(150);
                bool en1 = IsWindowEnabled(GetDlgItem(o, O_HILITE)) != 0;
                SendMessageW(GetDlgItem(o, O_MOUSE), BM_CLICK, 0, 0); Sleep(150);
                bool en2 = IsWindowEnabled(GetDlgItem(o, O_HILITE)) != 0;
                wchar_t dd[120]; _snwprintf(dd, 120, L"pointer on at start %d: highlight box enabled %d, after one click %d, after two %d", (int)pointerOn, (int)en0, (int)en1, (int)en2);
                Check(en0 == pointerOn && en1 == !pointerOn && en2 == pointerOn, L"program: the outline-and-waves box is enabled only while the pointer box is checked", dd);
            }
            Settings d; SettingsDefaults(d);
            SendMessageW(GetDlgItem(o, O_SOUNDS), BM_SETCHECK, d.sounds ? BST_UNCHECKED : BST_CHECKED, 0);       // a general setting, changed
            SendMessageW(GetDlgItem(o, O_FPS), CB_SETCURSEL, d.fps == 10 ? 6 : 0, 0);                              // a video setting, changed
            SendMessageW(GetDlgItem(o, O_HK0), WM_KEYDOWN, VK_BACK, 0);                                           // a hotkey, switched off
            PostMessageW(o, WM_COMMAND, O_DEFAULTS, 0); Sleep(300); Pump(100);                                    // the general page is open
            bool generalBack = (SendMessageW(GetDlgItem(o, O_SOUNDS), BM_GETCHECK, 0, 0) == BST_CHECKED) == d.sounds;
            int fpsIdx = (int)SendMessageW(GetDlgItem(o, O_FPS), CB_GETCURSEL, 0, 0);
            wchar_t hk0[80]; WinText(GetDlgItem(o, O_HK0), hk0, 80);
            wchar_t dd[200]; _snwprintf(dd, 200, L"general back to default: %d, the video setting is item %d, the hotkey field says [%s]", generalBack, fpsIdx, hk0);
            Check(generalBack, L"program: Restore default sets back the settings of the open page", dd);
            Check(fpsIdx == (d.fps == 10 ? 6 : 0), L"program: Restore default leaves the other pages as they are", dd);
            Check(wcscmp(hk0, T(S_HK_NONE)) == 0, L"program: Restore default leaves the hotkeys page as it is", dd);
            PostMessageW(o, WM_COMMAND, IDCANCEL, 0);
            DWORD cend = GetTickCount() + WMS(8000);
            while (IsWindow(o) && (int)(cend - GetTickCount()) > 0) Sleep(50);
        }
    }
    // the options window changes the settings the program keeps
    {
        PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
        HWND o = FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 10000);
        Check(o != nullptr, L"program: Options opens its window");
        if (o) {
            Settings w; RandomSettings(w);
            w.runAtStartup = false; w.sounds = false; w.openFolder = false;
            wcscpy(w.folder, s.folder);
            static const int fpsv[] = { 10, 15, 20, 25, 30, 45, 60 };
            w.fps = fpsv[R(0, 6)];
            w.language = R(0, LangCount());
            RandomHotkeys(w.hotkey, true);                          // function keys: pressed without a modifier held
            bool dupKeys = Chance(25);
            if (dupKeys) { int k; do k = VK_F1 + R(0, 23); while (!HotkeyUsable(k)); w.hotkey[1] = w.hotkey[0] = k; }      // a key the box accepts (not a plain F12)
            for (int i = 0; i < HKI_COUNT; ++i) {
                HWND e = GetDlgItem(o, O_HK0 + i);
                if (!e) continue;
                SendMessageW(e, WM_KEYDOWN, VK_BACK, 0);              // off first, then the key
                if (w.hotkey[i]) SendMessageW(e, WM_KEYDOWN, (WPARAM)w.hotkey[i], 0);
            }
            auto chk = [&](int id, bool v) { SendMessageW(GetDlgItem(o, id), BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); };
            chk(O_AUTOSTART, false); chk(O_MOUSE, w.capMouse); chk(O_HILITE, w.highlight); chk(O_OPENFOLDER, false); chk(O_SOUNDS, false); chk(O_NOTIFY, w.notify); chk(O_LOOPBACK, w.loopback); chk(O_MIC, w.mic);
            wchar_t n[16];
            SendMessageW(GetDlgItem(o, O_DELAY), CB_SETCURSEL, w.delaySec, 0);
            SelData(GetDlgItem(o, O_VCODEC), w.videoCodec); SendMessageW(GetDlgItem(o, O_ACODEC), CB_SETCURSEL, w.audioCodec, 0);
            SendMessageW(GetDlgItem(o, O_VQ), CB_SETCURSEL, w.videoQuality, 0); SendMessageW(GetDlgItem(o, O_AQ), CB_SETCURSEL, w.audioQuality, 0);
            int fi = 0; for (int i = 0; i < 7; ++i) if (fpsv[i] == w.fps) fi = i;
            SendMessageW(GetDlgItem(o, O_FPS), CB_SETCURSEL, fi, 0);
            SelData(GetDlgItem(o, O_IMGFMT), w.imageFormat);
            SendMessageW(GetDlgItem(o, O_LANG), CB_SETCURSEL, w.language, 0);
            bool bad = Chance(30) || dupKeys;
            SetText(GetDlgItem(o, O_TPL), bad && !dupKeys ? L"a#b#" : w.fileName);
            {   // under the template field the window shows the name a file would get now
                Sleep(250);
                wchar_t pv[600]; WinText(GetDlgItem(o, O_TPLPREVIEW), pv, 600);
                wchar_t e1[460], e2[460]; const wchar_t* tpl = bad && !dupKeys ? L"a#b#" : w.fileName;
                TemplatePreview(tpl, e1, 460); Sleep(30); TemplatePreview(tpl, e2, 460);
                size_t pl = wcslen(pv);
                for (wchar_t* c = e1; *c; ++c) if (*c >= L'0' && *c <= L'9') *c = L'0';             // the time (and the number) moves on: compare the shape,
                for (wchar_t* c = e2; *c; ++c) if (*c >= L'0' && *c <= L'9') *c = L'0';             // every digit as a 0
                wchar_t pvn[600]; wcsncpy(pvn, pv, 599); pvn[599] = 0; for (wchar_t* c = pvn; *c; ++c) if (*c >= L'0' && *c <= L'9') *c = L'0';
                size_t pn = wcslen(pvn);
                bool endsWith1 = e1[0] && pn >= wcslen(e1) && !wcscmp(pvn + pn - wcslen(e1), e1), endsWith2 = e2[0] && pn >= wcslen(e2) && !wcscmp(pvn + pn - wcslen(e2), e2);
                (void)pl;
                bool ok = e1[0] ? (endsWith1 || endsWith2) : pv[0] == 0;
                wchar_t dpv[700]; _snwprintf(dpv, 700, L"shown [%s], expected the name [%s]", pv, e1);
                Check(ok, L"program: the Save tab shows under the template how a file will be named (without the extension)", dpv);
            }
            PostMessageW(o, WM_COMMAND, IDOK, 0);
            if (bad) {
                bool warned = DismissBox(a.pi.dwProcessId, IDOK, 5000);
                Sleep(300);
                Check(warned && IsWindow(o), dupKeys ? L"program: the same hotkey on two actions is refused with a message and the window stays open" : L"program: an invalid template is refused with a message and the window stays open");
                PostMessageW(o, WM_COMMAND, IDCANCEL, 0);
                DWORD cend = GetTickCount() + WMS(8000);
                while (IsWindow(o) && (int)(cend - GetTickCount()) > 0) Sleep(50);
                Sleep(500);
                Settings now; SettingsLoad(now);
                wchar_t why[160] = L"";
                Check(SettingsEqual(s, now, true, why), L"program: Cancel leaves the settings as they were", why);
            } else {
                DWORD end = GetTickCount() + WMS(10000);
                while (IsWindow(o) && (int)(end - GetTickCount()) > 0) Sleep(50);
                Settings now; wchar_t why[160] = L"";
                bool same = false;
                end = GetTickCount() + WMS(10000);                  // the program stores the settings just after the window closes
                do { SettingsLoad(now); same = SettingsEqual(w, now, true, why); if (!same) Sleep(100); } while (!same && (int)(end - GetTickCount()) > 0);
                Check(!IsWindow(o) && same, L"program: OK stores exactly what the options window showed", why);
                // the program registered exactly the new hotkeys: another window cannot take them, but can take the freed ones
                {
                    static bool hreg = false;
                    if (!hreg) {
                        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
                        wc.lpfnWndProc = HkProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"C2SAutotestHotkeys";
                        RegisterClassW(&wc); hreg = true;
                    }
                    HWND hw2 = CreateWindowExW(0, L"C2SAutotestHotkeys", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
                    Sleep(300);
                    int taken = 0, wrongFree = 0, freedOld = 0, wrongNew = 0, wrongOld = 0;
                    Settings def; SettingsDefaults(def);
                    for (int i = 0; i < HKI_COUNT; ++i) {                 // the stop key is registered only while a recording is on
                        if (i == 3) continue;
                        if (w.hotkey[i] && !(g_wine && (w.hotkey[i] & 0xFFFF) == VK_SNAPSHOT)) {
                            if (RegisterHotKey(hw2, 50 + i, (UINT)w.hotkey[i] >> 16, (UINT)w.hotkey[i] & 0xFFFF)) { UnregisterHotKey(hw2, 50 + i); ++wrongFree; ++wrongNew; wchar_t hn[60]; HotkeyName(w.hotkey[i], hn, 60); Rep(L"INFO", L"  new hotkey %s (index %d) was not registered by the program", hn, i); } else ++taken;
                        }
                        if (g_wine && (def.hotkey[i] & 0xFFFF) == VK_SNAPSHOT) continue;      // Wine refuses Ctrl+PrintScreen to everybody
                        bool oldStillUsed = false; for (int j = 0; j < HKI_COUNT; ++j) if (w.hotkey[j] == def.hotkey[i]) oldStillUsed = true;
                        if (!oldStillUsed) {
                            if (RegisterHotKey(hw2, 60 + i, (UINT)def.hotkey[i] >> 16, (UINT)def.hotkey[i] & 0xFFFF)) { UnregisterHotKey(hw2, 60 + i); ++freedOld; } else { ++wrongFree; ++wrongOld; wchar_t hn[60]; HotkeyName(def.hotkey[i], hn, 60); Rep(L"INFO", L"  old hotkey %s still taken", hn); }
                        }
                    }
                    DestroyWindow(hw2);
                    wchar_t d2[120]; _snwprintf(d2, 120, L"%d new ones taken by the program, %d new ones still free, %d old ones released, %d old ones still taken", taken, wrongNew, freedOld, wrongOld);
                    if (g_wine && wrongNew && !wrongOld) SKIP(L"program: Wine does not report a hotkey taken by another process (%s)", d2);
                    else Check(wrongFree == 0, L"program: after OK the new hotkeys are registered by the program and the old ones are released", d2);
                }
                AppTexts(a, w.language, L"after changing the language in Options", w.hotkey);
            }
        }
    }
    Check(AppAlive(a), L"program: still running after all of it");
    // quit from the menu command
    PostMessageW(a.main, WM_COMMAND, ID_TRAY_EXIT, 0);
    DWORD code = 99;
    bool ended = WaitForSingleObject(a.pi.hProcess, 10000) == WAIT_OBJECT_0 && GetExitCodeProcess(a.pi.hProcess, &code) && code == 0;
    Check(ended, L"program: Exit ends the program with code 0");
    StopApp(a, false);
    LangSelect(0);
}


// ---- more flows of the real program ---------------------------------------------------------------------------------
// Drives the program's picker overlay (its own process) the way a mouse would. mode 0: drag from a to b (screen
// coordinates); 1: point at `a` and click (window picking); 2: Esc.
static bool DrivePicker(DWORD pid, int mode, POINT a, POINT b, bool stray = false)
{
    HWND ov = FindWin(pid, C2S_CLASS_PICKER, 10000);
    if (!ov) return false;
    Pump(300);
    RECT vs = VirtualScreen();
    auto L = [&](POINT p) { return MAKELPARAM(p.x - vs.left, p.y - vs.top); };
    if (mode == 0) {
        if (stray) { SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, L(a)); SendMessageW(ov, WM_LBUTTONUP, 0, L(a)); Sleep(150); }
        SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, L(a));
        for (int i = 1; i <= 4; ++i) { POINT m = { a.x + (b.x - a.x) * i / 4, a.y + (b.y - a.y) * i / 4 }; SendMessageW(ov, WM_MOUSEMOVE, MK_LBUTTON, L(m)); }
        SetCursorPos(b.x, b.y);
        SendMessageW(ov, WM_LBUTTONUP, 0, L(b));
    } else if (mode == 1) {
        SetCursorPos(a.x, a.y);                       // the real pointer on the target too: a parked one would send its own WM_MOUSEMOVE in between
        SendMessageW(ov, WM_MOUSEMOVE, 0, L(a)); Pump(80);
        SendMessageW(ov, WM_LBUTTONUP, 0, L(a));
    } else SendMessageW(ov, WM_KEYDOWN, VK_ESCAPE, 0);
    return true;
}

static bool LatestFile(const wchar_t* dir, wchar_t* out)
{
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*.*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    FILETIME best = {0, 0}; bool any = false;
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (!any || CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) { best = fd.ftLastWriteTime; _snwprintf(out, MAX_PATH, L"%s\\%s", dir, fd.cFileName); any = true; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

static bool WaitNewFile(const wchar_t* dir, int before, int waitMs, wchar_t* file)
{
    DWORD end = GetTickCount() + waitMs * g_scale;
    while (CountFiles(dir) <= before && (int)(end - GetTickCount()) > 0) Pump(100);
    if (CountFiles(dir) <= before || !LatestFile(dir, file)) return false;
    unsigned long long last = 0; int stable = 0;
    for (int i = 0; i < 100 && stable < 4; ++i) { unsigned long long z = FileSz(file); if (z == last && z > 0) ++stable; else stable = 0; last = z; Pump(150); }
    return true;
}

static void PrepareApp(Settings& s)
{
    AppSettings(s, R(0, LangCount()));
    wchar_t t[24]; _snwprintf(t, 24, L"app%d", R(0, 99999));
    _snwprintf(s.folder, MAX_PATH, L"%s\\%s\\out", g_dir, t);
    s.delaySec = 0;
}

// Record from the program's panel: Start, wait, Stop. Returns false if something did not happen.
static bool PanelRecord(App& a, const Settings& s, bool& panelSeen)
{
    HWND panel = FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000);
    panelSeen = panel != nullptr;
    if (!panel) return false;
    LangSelect(s.language);
    PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0);
    if (!WaitText(panel, IDC_P_START, T(S_PAUSE), (s.delaySec + 6) * 1000)) return false;
    Sleep(R(500, 1500));
    PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0);
    DWORD end = GetTickCount() + WMS(12000);
    while (FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) && (int)(end - GetTickCount()) > 0) Sleep(100);
    return FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) == nullptr;
}

static void T_AppPicks()
{
    MakeFixture(R(380, 700), R(260, 460));
    Settings s; PrepareApp(s);
    s.notify = false;                                              // the system toast of the step before ("saved") would lie over the fixture and be captured into the next picture (seen on Windows 10)
    App a;
    if (!StartApp(a, s)) { FAIL(L"program picks: Click2Screen.exe did not show its window"); StopApp(a, false); return; }
    RECT fr = FxScreenRect(), vs = VirtualScreen();
    INFO(L"picks: fixture %dx%d at %d,%d (window %p valid=%d), screen %d,%d..%d,%d, app pid %lu main %p", g_fx.w, g_fx.h, (int)fr.left, (int)fr.top, (void*)g_fx.hw, (int)IsWindow(g_fx.hw), (int)vs.left, (int)vs.top, (int)vs.right, (int)vs.bottom, a.pi.dwProcessId, (void*)a.main);
    SetCursorPos(2, 2);
    int order[4] = { 0, 1, 2, 3 };
    for (int i = 3; i > 0; --i) { int j = R(0, i); int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < 4; ++k) {
        int what = order[k];
        wchar_t nm[160];
        // a box left by the step before (e.g. "the microphone could not be opened" on a machine without one) would lie over the fixture and be picked instead of it
        {   // a box may come a moment after the step before (a slow machine shows the "area too small" box late): wait for a quiet spell
            DWORD stop = GetTickCount() + WMS(10000); int quiet = 0; HWND last = nullptr;
            while (quiet < 3 && (int)(stop - GetTickCount()) > 0) {
                HWND bx = FindWin(a.pi.dwProcessId, L"#32770", WMS(300));
                if (bx) {
                    if (bx == last) PostMessageW(bx, WM_CLOSE, 0, 0);          // the same box is still there after OK: close it the other way
                    else DismissBox(a.pi.dwProcessId, IDOK, 300);
                    last = bx; quiet = 0; Sleep(300);
                } else { last = nullptr; ++quiet; Pump(300); }
            }
        }
        if (what == 0) {                                               // region screenshot (Alt+Shift+4)
            POINT p1 = { vs.left + R(10, (vs.right - vs.left) - 330), vs.top + R(10, (vs.bottom - vs.top) - 260) };
            POINT p2 = { p1.x + R(40, 320), p1.y + R(40, 250) };
            bool flip = Chance(50); if (flip) { POINT t = p1; p1 = p2; p2 = t; }
            int before = CountFiles(s.folder);
            PostMessageW(a.main, WM_HOTKEY, HK_REGION, 0);
            bool driven = DrivePicker(a.pi.dwProcessId, 0, p1, p2, Chance(30));
            wchar_t file[MAX_PATH] = L""; int dw = 0, dh = 0; uint8_t* px = nullptr;
            bool got = driven && WaitNewFile(s.folder, before, 20000, file) && DecodePixels(file, &dw, &dh, &px);
            _snwprintf(nm, 160, L"program picks: region screenshot %dx%d is saved at exactly that size", abs(p2.x - p1.x), abs(p2.y - p1.y));
            wchar_t dd[200]; _snwprintf(dd, 200, L"driven=%d got=%d file %dx%d, dragged (%d,%d)-(%d,%d)", (int)driven, (int)got, dw, dh, (int)p1.x, (int)p1.y, (int)p2.x, (int)p2.y);
            Check(got && dw == abs(p2.x - p1.x) && dh == abs(p2.y - p1.y), nm, dd);
            delete[] px;
        } else if (what == 1) {                                        // window screenshot (Alt+Shift+5)
            POINT p = { fr.left + R(20, g_fx.w - 20), fr.top + R(20, g_fx.h - 20) };
            int before = CountFiles(s.folder);
            PostMessageW(a.main, WM_HOTKEY, HK_WINDOW, 0);
            bool driven = DrivePicker(a.pi.dwProcessId, 1, p, p);
            wchar_t file[MAX_PATH] = L""; int dw = 0, dh = 0; uint8_t* px = nullptr;
            bool got = driven && WaitNewFile(s.folder, before, 20000, file) && DecodePixels(file, &dw, &dh, &px);
            RECT vis; WindowVisibleRect(g_fx.hw, &vis);
            wchar_t dd[200]; _snwprintf(dd, 200, L"driven=%d got=%d file %dx%d, window %dx%d", (int)driven, (int)got, dw, dh, (int)(vis.right - vis.left), (int)(vis.bottom - vis.top));
            Check(got && dw == vis.right - vis.left && dh == vis.bottom - vis.top, L"program picks: window screenshot has the size of the pointed window", dd);
            int bad = 0, n = 0;
            if (got) for (int i = 0; i < 400 && n < 10; ++i) {
                int x = R(0, dw - 1), y = R(0, dh - 1);
                if (!SafePoint(x, y, g_fx.w, g_fx.h)) continue;
                if (LossyFmt(s.imageFormat) && (abs(x - g_fx.w / 2) < 20 || abs(y - g_fx.h / 2) < 20)) continue;     // JPEG rings at edges
                if (s.capMouse && abs(x - (p.x - vis.left)) < 64 && abs(y - (p.y - vis.top)) < 64) continue;           // the pointer (and its halo) is in the picture where the test clicked
                const uint8_t* q = px + ((size_t)y * dw + x) * 4;
                COLORREF ex = Expected(x, y, g_fx.w, g_fx.h);
                // Under Wine / Xvfb a part of the window may not be repainted yet when the dimmed picker has just gone: a
                // pure black pixel is then not counted (on Windows the desktop is composed and every pixel counts).
                if (g_wine && ((q[0] < (LossyFmt(s.imageFormat) ? 70 : 30) && q[1] < (LossyFmt(s.imageFormat) ? 70 : 30) && q[2] < (LossyFmt(s.imageFormat) ? 70 : 30)) || (q[0] > 235 && q[1] > 235 && q[2] > 235))) continue;   // black / white: not repainted yet
                ++n; if (!Near(ex, q[2], q[1], q[0], LossyFmt(s.imageFormat) ? 90 : s.imageFormat == IMG_GIF ? 60 : 6)) { ++bad; if (bad < 4) Rep(L"INFO", L"  window picture (%d,%d) = %d,%d,%d expected %d,%d,%d (format %d, picture %dx%d)", x, y, q[2], q[1], q[0], GetRValue(ex), GetGValue(ex), GetBValue(ex), s.imageFormat, dw, dh); }
            }
            if (got && bad) {                                          // keep the picture that failed beside the logs, and a shot of the whole screen at that moment
                wchar_t pd[MAX_PATH] = L"", keep[MAX_PATH + 60], scr[MAX_PATH + 60];
                GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
                const wchar_t* ext = wcsrchr(file, L'.');
                static int fails = 0; ++fails;
                _snwprintf(keep, MAX_PATH + 60, L"%s\\Click2Screen\\logs\\picks-window-fail-%d%s", pd, fails, ext ? ext : L".img");
                CopyFileW(file, keep, FALSE);
                _snwprintf(scr, MAX_PATH + 60, L"%s\\Click2Screen\\logs\\picks-screen-fail-%d.png", pd, fails);
                { HBITMAP sb = CaptureRect(VirtualScreen(), false, false); if (sb) { SaveBitmap(sb, scr, IMG_PNG); DeleteObject(sb); } }
            }
            Check(got && n >= 5 && bad == 0, L"program picks: the window screenshot shows that window's colours in the right places");
            delete[] px;
        } else if (what == 2) {                                        // region recording
            bool tiny = Chance(30);
            POINT p1 = { fr.left + R(10, 60), fr.top + R(10, 60) };
            POINT p2 = tiny ? POINT{ p1.x + R(5, 12), p1.y + R(5, 12) } : POINT{ p1.x + R(80, g_fx.w - 80), p1.y + R(60, g_fx.h - 80) };
            PostMessageW(a.main, WM_COMMAND, IDC_REC_REGION, 0);
            bool driven = DrivePicker(a.pi.dwProcessId, 0, p1, p2);
            if (tiny) {
                bool box = DismissBox(a.pi.dwProcessId, IDOK, 8000);
                { DWORD gone = GetTickCount() + WMS(4000); while (FindWin(a.pi.dwProcessId, L"#32770", 0) && (int)(gone - GetTickCount()) > 0) Sleep(50); }     // a slow machine keeps the box on the screen a while: it would cover the next pick
                Sleep(300);
                Check(driven && box && FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) == nullptr && AppAlive(a), L"program picks: a region smaller than 16 px is refused with a message, no panel");
                ShowWindow(a.main, SW_SHOW);
            } else {
                bool seen = false;
                bool ok = driven && PanelRecord(a, s, seen);
                Check(ok, L"program picks: a dragged region records through the panel and the panel closes");
            }
        } else {                                                       // window recording
            POINT p = { fr.left + R(20, g_fx.w - 20), fr.top + R(20, g_fx.h - 20) };
            PostMessageW(a.main, WM_COMMAND, IDC_REC_WINDOW, 0);
            bool driven = DrivePicker(a.pi.dwProcessId, 1, p, p);
            bool seen = false;
            bool ok = driven && PanelRecord(a, s, seen);
            wchar_t dd[100]; _snwprintf(dd, 100, L"picker driven=%d, panel seen=%d, picker still open=%d", (int)driven, (int)seen, FindWin(a.pi.dwProcessId, C2S_CLASS_PICKER, 0) != nullptr);
            Check(ok, L"program picks: a pointed window records through the panel and the panel closes", dd);
        }
        if (!AppAlive(a)) { FAIL(L"program picks: the program died"); break; }
        if (!IsWindowVisible(a.main)) ShowWindow(a.main, SW_SHOW);
        Pump(100);
    }
    // Esc in the picker cancels and brings the main window back
    PostMessageW(a.main, WM_COMMAND, IDC_REC_REGION, 0);
    POINT z = { 0, 0 };
    bool driven = DrivePicker(a.pi.dwProcessId, 2, z, z);
    DWORD end = GetTickCount() + WMS(8000);
    while (FindWin(a.pi.dwProcessId, C2S_CLASS_PICKER, 0) && (int)(end - GetTickCount()) > 0) Sleep(100);
    Sleep(300);
    Check(driven && FindWin(a.pi.dwProcessId, C2S_CLASS_PICKER, 0) == nullptr && FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) == nullptr && IsWindowVisible(a.main), L"program picks: Esc in the picker cancels and the main window returns");
    StopApp(a);
}

static void T_AppOddities()
{
    MakeFixture(R(300, 500), R(220, 360));
    int which = R(0, 6);
    Settings s; PrepareApp(s);
    if (which == 0) {                                  // the hotkeys are taken by somebody else: the program still starts and works
        static bool reg = false;
        if (!reg) { WNDCLASSW wc; memset(&wc, 0, sizeof(wc)); wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"C2SAutotestHoldKeys"; RegisterClassW(&wc); reg = true; }
        HWND hold = CreateWindowExW(0, L"C2SAutotestHoldKeys", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        int held = 0;
        if (RegisterHotKey(hold, 1, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT)) ++held;
        if (RegisterHotKey(hold, 2, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '4')) ++held;
        if (RegisterHotKey(hold, 3, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '5')) ++held;
        App a;
        bool ok = StartApp(a, s);
        Check(ok && AppAlive(a), L"program oddities: the program starts although its hotkeys are held by another program", L"");
        if (ok) {                                      // and the buttons still work
            int before = CountFiles(s.folder);
            PostMessageW(a.main, WM_COMMAND, IDC_SHOT_DESKTOP, 0);
            wchar_t f[MAX_PATH];
            Check(WaitNewFile(s.folder, before, 20000, f), L"program oddities: the screenshot button works without the hotkeys");
        }
        StopApp(a);
        for (int i = 1; i <= 3; ++i) UnregisterHotKey(hold, i);
        DestroyWindow(hold);
        (void)held;
    } else if (which == 1) {                           // an output folder that cannot exist: a message, no crash, the program goes on
        wcscpy(s.folder, L"C:\\nul\\x\\no such drive\\img");
        App a;
        if (!StartApp(a, s)) { FAIL(L"program oddities: no window"); StopApp(a, false); return; }
        PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0);
        bool box = DismissBox(a.pi.dwProcessId, IDOK, 15000);
        Sleep(300);
        Check(box && AppAlive(a), L"program oddities: an unusable pictures folder gives a message and the program stays up");
        DWORD_PTR res = 0;
        Check(SendMessageTimeoutW(a.main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 5000, &res) != 0, L"program oddities: the window still answers after that");
        StopApp(a);
    } else if (which >= 4) {                           // the installer closes the program (WM_C2S_QUIT) in the middle of something
        App a;
        if (!StartApp(a, s)) { FAIL(L"program oddities: no window"); StopApp(a, false); return; }
        const wchar_t* what = L"";
        if (which == 4) {                              // while the region picker is on the screen
            what = L"while the region picker is open";
            PostMessageW(a.main, WM_COMMAND, IDC_REC_REGION, 0);
            if (!FindReady(a.pi.dwProcessId, C2S_CLASS_PICKER, 10000)) FAIL(L"program oddities: the picker did not open");
        } else if (which == 5) {                       // while the options window is open
            what = L"while the options window is open";
            PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
            if (!FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 10000)) FAIL(L"program oddities: the options window did not open");
        } else {                                       // while recording
            what = L"while a recording runs";
            PostMessageW(a.main, WM_COMMAND, IDC_REC_DESKTOP, 0);
            HWND panel = FindReady(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000);
            if (panel) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0); LangSelect(s.language); WaitText(panel, IDC_P_START, T(S_PAUSE), (s.delaySec + 6) * 1000); Pump(500); }
            else FAIL(L"program oddities: the panel did not open");
        }
        PostMessageW(a.main, WM_C2S_QUIT, 0, 0);
        DWORD code = 99;
        bool exited = WaitForSingleObject(a.pi.hProcess, WMS(10000)) == WAIT_OBJECT_0;
        bool ok = exited && GetExitCodeProcess(a.pi.hProcess, &code) && code == 0;
        wchar_t nm[160]; _snwprintf(nm, 160, L"program oddities: the installer's close message ends the program %s", what);
        Check(ok, nm, exited ? L"" : L"(it kept running)");
        StopApp(a, false);
    } else {                                           // the autostart box: written for the current user and taken away again
        HKEY k; wchar_t old[MAX_PATH] = L""; DWORD n = sizeof(old), t = 0; bool had = false;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            had = RegQueryValueExW(k, L"Click2Screen", nullptr, &t, (BYTE*)old, &n) == ERROR_SUCCESS; RegCloseKey(k);
        }
        App a;
        if (!StartApp(a, s)) { FAIL(L"program oddities: no window"); StopApp(a, false); return; }
        for (int round = 0; round < 2; ++round) {
            bool want = round == 0;
            PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
            HWND o = FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 10000);
            if (!o) { FAIL(L"program oddities: no options window"); break; }
            SendMessageW(GetDlgItem(o, O_AUTOSTART), BM_SETCHECK, want ? BST_CHECKED : BST_UNCHECKED, 0);
            PostMessageW(o, WM_COMMAND, IDOK, 0);
            DWORD end = GetTickCount() + WMS(10000);
            while (IsWindow(o) && (int)(end - GetTickCount()) > 0) Sleep(50);
            wchar_t val[MAX_PATH + 40] = L""; bool present = false;
            end = GetTickCount() + WMS(8000);
            do {
                present = false;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS) {
                    DWORD sz = sizeof(val), ty = 0; present = RegQueryValueExW(k, L"Click2Screen", nullptr, &ty, (BYTE*)val, &sz) == ERROR_SUCCESS; RegCloseKey(k);
                }
                if (present == want) break; Sleep(100);
            } while ((int)(end - GetTickCount()) > 0);
            bool good = present == want && (!want || (val[0] == L'"' && wcsstr(val, L"Click2Screen.exe\" /tray")));
            Check(good, want ? L"program oddities: the autostart box writes the Run entry with /tray" : L"program oddities: clearing the box removes the Run entry", val);
        }
        StopApp(a);
        // put back what was there
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            if (had) RegSetValueExW(k, L"Click2Screen", 0, REG_SZ, (BYTE*)old, (DWORD)((wcslen(old) + 1) * 2)); else RegDeleteValueW(k, L"Click2Screen");
            RegCloseKey(k);
        }
    }
}

// A "monkey": a random sequence of things a user can do; after every step the program must be alive and answering.
static void T_Monkey()
{
    MakeFixture(R(380, 640), R(260, 420));
    Settings s; PrepareApp(s);
    s.delaySec = R(0, 1);
    App a;
    if (!StartApp(a, s)) { FAIL(L"monkey: Click2Screen.exe did not show its window"); StopApp(a, false); return; }
    RECT fr = FxScreenRect();
    int steps = R(25, 50), bad = 0;
    INFO(L"monkey: %d random steps, language %d", steps, s.language);
    for (int i = 0; i < steps && bad == 0; ++i) {
        int act = R(0, 9);
        HWND panel = FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0);
        switch (act) {
            case 0: PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0); Pump(R(100, 900)); break;
            case 1: if (!panel) { PostMessageW(a.main, WM_COMMAND, IDC_REC_DESKTOP, 0); Pump(400); } break;
            case 2: if (panel) PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0); Pump(R(100, 1200)); break;
            case 3: if (panel) PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0); Pump(R(100, 800)); break;
            case 4: PostMessageW(a.main, WM_HOTKEY, HK_STOP, 0); Pump(R(100, 600)); break;
            case 5: PostMessageW(a.main, WM_COMMAND, ID_TRAY_PAUSE, 0); Pump(R(50, 500)); break;
            case 6: PostMessageW(a.main, WM_COMMAND, ID_TRAY_PANEL, 0); Pump(200); break;
            case 7: {   // options: open, then OK or Cancel
                PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
                HWND o = FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 3000);
                if (o) { PostMessageW(o, WM_COMMAND, Chance(50) ? IDOK : IDCANCEL, 0); DWORD end = GetTickCount() + WMS(6000); while (IsWindow(o) && (int)(end - GetTickCount()) > 0) Sleep(50); }
                break; }
            case 8: if (!panel) { PostMessageW(a.main, WM_COMMAND, IDC_REC_REGION, 0); POINT p1 = { fr.left + R(5, 50), fr.top + R(5, 50) }, p2 = { p1.x + R(60, 200), p1.y + R(60, 150) };
                      if (Chance(80)) DrivePicker(a.pi.dwProcessId, 0, p1, p2); else { POINT z = {0, 0}; DrivePicker(a.pi.dwProcessId, 2, z, z); } Pump(300); } break;
            default: if (panel && Chance(50)) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_HIDE, BN_CLICKED), 0); Pump(200); } else PostMessageW(a.main, WM_COMMAND, IDC_SHOT_REGION, 0); break;
        }
        // modal boxes (the question about hiding the panel, warnings) are answered like a user would
        for (int guard = 0; guard < 3; ++guard) {
            HWND box = FindWin(a.pi.dwProcessId, L"#32770", 0);
            if (!box) break;
            PostMessageW(box, WM_COMMAND, Chance(70) ? IDYES : IDOK, 0); Pump(150);
        }
        HWND pk = FindWin(a.pi.dwProcessId, C2S_CLASS_PICKER, 0);
        if (pk) { POINT z = {0, 0}; DrivePicker(a.pi.dwProcessId, 2, z, z); Pump(200); }
        DWORD_PTR res = 0;
        if (!AppAlive(a)) { FAIL(L"monkey: the program died at step %d (action %d)", i, act); ++bad; break; }
        if (!SendMessageTimeoutW(a.main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 8000, &res)) { FAIL(L"monkey: the program stopped answering at step %d (action %d)", i, act); ++bad; break; }
    }
    if (!bad) {
        // finish whatever is going on, then leave
        HWND panel = FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0);
        if (panel) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0); Pump(1500); }
        Pump(500);
        PASS(L"monkey: %d random steps, the program stayed alive and answering", steps);
        PostMessageW(a.main, WM_COMMAND, ID_TRAY_EXIT, 0);
        DWORD code = 99;
        bool exited = WaitForSingleObject(a.pi.hProcess, WMS(15000)) == WAIT_OBJECT_0;
        bool ended = exited && GetExitCodeProcess(a.pi.hProcess, &code) && code == 0;
        wchar_t dd[120]; _snwprintf(dd, 120, L"exited=%d code=%lu, box open=%d, panel open=%d", (int)exited, (unsigned long)code, FindWin(a.pi.dwProcessId, L"#32770", 0) != nullptr, FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) != nullptr);
        Check(ended, L"monkey: Exit after all of it ends the program with code 0", dd);
    }
    StopApp(a, false);
}


// ---- the program must not talk to the network ---------------------------------------------------------------------
// No update check, no telemetry, no downloads: the binaries must not import any networking library, and no URL may
// appear in them except the XML namespace identifiers of the manifest.
static bool ReadAll(const wchar_t* path, uint8_t** data, DWORD* size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFileSize(h, nullptr);
    *data = new uint8_t[n ? n : 1];
    DWORD got = 0;
    bool ok = ReadFile(h, *data, n, &got, nullptr) && got == n;
    CloseHandle(h);
    if (!ok) { delete[] *data; *data = nullptr; return false; }
    *size = n;
    return true;
}

static DWORD RvaToOffset(const IMAGE_SECTION_HEADER* sec, int n, DWORD rva)
{
    for (int i = 0; i < n; ++i)
        if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + (sec[i].Misc.VirtualSize > sec[i].SizeOfRawData ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData))
            return rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
    return 0;
}

// Imported DLL names of a PE file, lower case, separated by '|' (also the delay-load table).
static bool ImportedDlls(const uint8_t* d, DWORD size, char* out, int cap)
{
    out[0] = 0;
    if (size < sizeof(IMAGE_DOS_HEADER)) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)d;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > size) return false;
    const IMAGE_NT_HEADERS32* nt32 = (const IMAGE_NT_HEADERS32*)(d + dos->e_lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE) return false;
    bool is64 = nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    const IMAGE_DATA_DIRECTORY* dir = is64 ? ((const IMAGE_NT_HEADERS64*)nt32)->OptionalHeader.DataDirectory : nt32->OptionalHeader.DataDirectory;
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)((const uint8_t*)&nt32->OptionalHeader + nt32->FileHeader.SizeOfOptionalHeader);
    int ns = nt32->FileHeader.NumberOfSections;
    DWORD rva = dir[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for (DWORD off = rva ? RvaToOffset(sec, ns, rva) : 0; off && off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= size; off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(d + off);
        if (!imp->Name) break;
        DWORD no = RvaToOffset(sec, ns, imp->Name);
        if (!no || no >= size) break;
        char nm[80]; int i = 0;
        for (; i < 79 && d[no + i] && no + i < size; ++i) nm[i] = (char)tolower(d[no + i]);
        nm[i] = 0;
        if (strlen(out) + strlen(nm) + 2 < (size_t)cap) { strcat(out, nm); strcat(out, "|"); }
    }
    DWORD drva = dir[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress;     // delay-load imports would be listed too
    if (drva) strcat(out, "(delay-load table present)|");
    return true;
}

static void T_NoNetwork()
{
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t* sl = wcsrchr(self, L'\\'); if (sl) *sl = 0;
    static const char* const names[] = { "Click2Screen.exe", "Click2Screen-Setup.exe", "c2slauncher.exe" };
    static const char* const bad[] = { "ws2_32", "wsock32", "wininet", "winhttp", "urlmon", "mswsock", "dnsapi", "httpapi", "iphlpapi", "netapi32", "mpr.dll", "wlanapi", "webservices", "rpcrt4", "wldap32", "secur32", "sspicli", "schannel", "crypt32", "bcrypt" };
    int checked = 0, problems = 0;
    for (const char* nm : names) {
        wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\%S", self, nm);
        uint8_t* d = nullptr; DWORD n = 0;
        if (!ReadAll(path, &d, &n)) {                       // the launcher exists only in the x86 build folder
            wchar_t alt[MAX_PATH]; _snwprintf(alt, MAX_PATH, L"%s\\x86\\%S", self, nm);
            if (!ReadAll(alt, &d, &n)) continue;
        }
        ++checked;
        char dlls[2000];
        if (!ImportedDlls(d, n, dlls, sizeof(dlls))) { ++problems; Rep(L"INFO", L"  %S: cannot read the import table", nm); delete[] d; continue; }
        for (const char* b : bad) if (strstr(dlls, b)) { ++problems; Rep(L"INFO", L"  %S imports %S (%S)", nm, b, dlls); }
        if (strstr(dlls, "delay-load")) { ++problems; Rep(L"INFO", L"  %S has a delay-load table: its imports are not listed above", nm); }
        // URLs: only the namespaces of the manifest (an identifier, not a request)
        for (DWORD i = 0; i + 8 < n; ++i) {
            if (!(d[i] == 'h' && d[i + 1] == 't' && d[i + 2] == 't' && d[i + 3] == 'p')) continue;
            const char* t = (const char*)d + i;
            if (!strncmp(t, "http://schemas.microsoft.com/", 29)) continue;
            if (!strcmp(nm, "Click2Screen-Setup.exe") && !strncmp(t, "https://github.com/symops/Click2Screen", 38) && (unsigned char)t[38] < 33) continue;     // the link of the installer to the project page: opened only by a click, in the browser
            if (!strncmp(t, "http://", 7) || !strncmp(t, "https://", 8)) { ++problems; char u[80]; strncpy(u, t, 79); u[79] = 0; for (char* c = u; *c; ++c) if ((unsigned char)*c < 32) *c = 0; Rep(L"INFO", L"  %S contains the address %S", nm, u); }
        }
        // wide strings too
        for (DWORD i = 0; i + 20 < n; i += 2) {
            const wchar_t* w = (const wchar_t*)(d + i);
            if (!strcmp(nm, "Click2Screen-Setup.exe") && !wcsncmp(w, L"https://github.com/symops/Click2Screen", 38) && w[38] < 33) continue;      // the same link, wide
            if ((!wcsncmp(w, L"http://", 7) || !wcsncmp(w, L"https://", 8)) && wcsncmp(w, L"http://schemas.microsoft.com/", 29)) { ++problems; Rep(L"INFO", L"  %S contains a wide address %.60s", nm, w); }
        }
        delete[] d;
    }
    if (!checked) { SKIP(L"no network: the program binaries are not next to the autotest"); return; }
    wchar_t dd[60]; _snwprintf(dd, 60, L"(%d binaries checked)", checked);
    Check(problems == 0, L"no network: the binaries import no networking library and contain no address", dd);
    // and the source of the program in this tree, when it is there (a development checkout)
    wchar_t src[MAX_PATH]; _snwprintf(src, MAX_PATH, L"%s\\..\\..\\src\\main.cpp", self);
    uint8_t* sd = nullptr; DWORD sn = 0;
    if (ReadAll(src, &sd, &sn)) {
        static const char* const api[] = { "winhttp", "wininet", "WSAStartup", "InternetOpen", "URLDownloadTo", "getaddrinfo", "socket(", "curl_" };
        int hits = 0;
        char* text = new char[sn + 1]; memcpy(text, sd, sn); text[sn] = 0;
        for (const char* a : api) if (strstr(text, a)) { ++hits; Rep(L"INFO", L"  main.cpp mentions %S", a); }
        delete[] text; delete[] sd;
        Check(hits == 0, L"no network: the program source names no networking API");
    }
}

// ---- the installer window --------------------------------------------------------------------------------------------
enum { SU_TITLE = 100, SU_SUB, SU_STATE, SU_DESKTOP, SU_START, SU_AUTO, SU_LOG, SU_INSTALL, SU_REMOVE, SU_LAUNCH, SU_CLOSE, SU_LANGLBL, SU_LANG, SU_PATHLBL, SU_PATH, SU_BROWSE, SU_VERSION, SU_GROUP, SU_RUNAFTER };

static bool LaunchSetupGui(PROCESS_INFORMATION& pi, HWND& wnd)
{
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t* sl = wcsrchr(self, L'\\'); if (sl) *sl = 0;
    wchar_t cmd[MAX_PATH + 40]; _snwprintf(cmd, MAX_PATH + 40, L"\"%s\\Click2Screen-Setup.exe\"", self);
    STARTUPINFOW si = { sizeof(si) };
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    wnd = FindWin(pi.dwProcessId, L"Click2ScreenSetup", 20000);
    // the window can be found while it is still being created: wait until it is shown and its controls exist
    DWORD end = GetTickCount() + WMS(20000);
    while (wnd && (!IsWindowVisible(wnd) || !GetDlgItem(wnd, SU_LANG) || !GetDlgItem(wnd, SU_CLOSE)) && (int)(end - GetTickCount()) > 0) Pump(50);
    return wnd != nullptr && GetDlgItem(wnd, SU_LANG) != nullptr;
}

static void T_SetupGui()
{
    PROCESS_INFORMATION pi; HWND w = nullptr;
    if (!LaunchSetupGui(pi, w)) {
        if (!g_wine && !g_installerAllowed) { SKIP(L"installer window: needs administrator rights (--installer)"); return; }
        FAIL(L"installer window: it did not appear"); if (pi.hProcess) { TerminateProcess(pi.hProcess, 99); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); } return;
    }
    // every language: switch the combo like a user and read the texts back
    int order[32], n = LangCount();
    for (int i = 0; i < n; ++i) order[i] = i;
    for (int i = n - 1; i > 0; --i) { int j = R(0, i); int t = order[i]; order[i] = order[j]; order[j] = t; }
    int bad = 0;
    HWND combo = GetDlgItem(w, SU_LANG);
    wchar_t ver[64]; _snwprintf(ver, 64, L"%s", C2S_VERSION_STR);
    for (int k = 0; k < n && k < 8; ++k) {                     // eight random languages per run
        int lng = order[k];
        SendMessageW(combo, CB_SETCURSEL, lng, 0);
        SendMessageW(w, WM_COMMAND, MAKEWPARAM(SU_LANG, CBN_SELCHANGE), (LPARAM)combo);
        LangSelect(lng + 1);
        wchar_t got[500], want[500];
        struct { int id; Str s; } t[] = { {SU_DESKTOP, S_SU_DESKTOP}, {SU_START, S_SU_STARTMENU}, {SU_AUTO, S_SU_AUTOSTART}, {SU_REMOVE, S_SU_REMOVE}, {SU_LAUNCH, S_SU_LAUNCH}, {SU_CLOSE, S_SU_CLOSE}, {SU_LANGLBL, S_SU_LANG}, {SU_PATHLBL, S_SU_PATH}, {SU_RUNAFTER, S_SU_RUNAFTER}, {SU_GROUP, S_SU_STATUS} };
        for (auto& i : t) { WinText(GetDlgItem(w, i.id), got, 500); if (wcscmp(got, T(i.s))) { ++bad; Rep(L"INFO", L"  language %d control %d: [%s] expected [%s]", lng, i.id, got, T(i.s)); } }
        WinText(GetDlgItem(w, SU_TITLE), got, 500); if (wcscmp(got, T(S_APP))) ++bad;
        WinText(GetDlgItem(w, SU_SUB), got, 500); if (wcscmp(got, T(S_SU_SUB))) { ++bad; Rep(L"INFO", L"  subtitle [%s] expected [%s]", got, T(S_SU_SUB)); }
        wchar_t vn[100]; _snwprintf(vn, 100, T(S_SU_VERSION), ver);
        _snwprintf(want, 500, L"%s   \x00B7   %s, %s   \x00B7", vn, L"" C2S_AUTHOR_STR, L"" C2S_EMAIL_STR);       // version · author, e-mail · (the link to the project follows, as its own control)
        WinText(GetDlgItem(w, SU_VERSION), got, 500); if (wcscmp(got, want)) { ++bad; Rep(L"INFO", L"  version line [%s] expected [%s]", got, want); }
        WinText(GetDlgItem(w, SU_INSTALL), got, 500);
        if (wcscmp(got, T(S_SU_INSTALL)) && wcscmp(got, T(S_SU_UPDATE))) { ++bad; Rep(L"INFO", L"  install button [%s]", got); }
    }
    Check(bad == 0, L"installer window: every language switched in the combo shows that language everywhere (8 random)");
    LangSelect(0);
    // not installed -> Remove and Launch are disabled; the state line says so
    wchar_t st[300]; WinText(GetDlgItem(w, SU_STATE), st, 300);
    bool installed = wcsstr(st, L"1.0") != nullptr;
    LangSelect(R(1, LangCount())); SendMessageW(combo, CB_SETCURSEL, 0, 0); SendMessageW(w, WM_COMMAND, MAKEWPARAM(SU_LANG, CBN_SELCHANGE), (LPARAM)combo); LangSelect(1);
    if (!installed) Check(!IsWindowEnabled(GetDlgItem(w, SU_REMOVE)) && !IsWindowEnabled(GetDlgItem(w, SU_LAUNCH)) && IsWindowEnabled(GetDlgItem(w, SU_BROWSE)), L"installer window: with nothing installed Remove and Launch are disabled and the folder can be chosen");
    else Check(!IsWindowEnabled(GetDlgItem(w, SU_BROWSE)), L"installer window: an installed copy stays where it is (Browse is off)");
    // a DPI change moves the controls; a window message storm must not break it
    for (int i = 0; i < 20; ++i) { SendMessageW(w, WM_NULL, 0, 0); Pump(5); }
    PostMessageW(w, WM_COMMAND, SU_CLOSE, 0);
    DWORD code = 99;
    bool ended = WaitForSingleObject(pi.hProcess, WMS(10000)) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 0;
    if (!ended) TerminateProcess(pi.hProcess, 99);
    Check(ended, L"installer window: Close ends the installer with code 0");
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

static void T_SetupGuiInstall()
{
    if (!g_installerAllowed) { SKIP(L"installer window: install through the window needs --installer"); return; }
    wchar_t pf[MAX_PATH] = L""; PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) { wcsncpy(pf, p, MAX_PATH - 1); CoTaskMemFree(p); }
    wchar_t exe[MAX_PATH]; _snwprintf(exe, MAX_PATH, L"%s\\Click2Screen\\Click2Screen.exe", pf);
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) { SKIP(L"installer window: Click2Screen is already installed here; not touching it"); return; }
    PROCESS_INFORMATION pi; HWND w = nullptr;
    if (!LaunchSetupGui(pi, w)) { FAIL(L"installer window: it did not appear"); return; }
    bool wantDesk = Chance(50), wantMenu = Chance(50), wantAuto = Chance(50);
    SendMessageW(GetDlgItem(w, SU_DESKTOP), BM_SETCHECK, wantDesk ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(w, SU_START), BM_SETCHECK, wantMenu ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(w, SU_AUTO), BM_SETCHECK, wantAuto ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(w, SU_RUNAFTER), BM_SETCHECK, BST_UNCHECKED, 0);          // do not start the program from the test
    PostMessageW(w, WM_COMMAND, SU_INSTALL, 0);
    DWORD end = GetTickCount() + WMS(60000);
    while (!IsWindowEnabled(GetDlgItem(w, SU_REMOVE)) && (int)(end - GetTickCount()) > 0) Pump(100);
    Pump(500);
    Check(IsWindowEnabled(GetDlgItem(w, SU_REMOVE)) && IsWindowEnabled(GetDlgItem(w, SU_LAUNCH)) && !IsWindowEnabled(GetDlgItem(w, SU_BROWSE)), L"installer window: Install finishes, enables Remove / Launch and fixes the folder");
    Check(GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES, L"installer window: the program is in Program Files");
    wchar_t log[3000]; WinText(GetDlgItem(w, SU_LOG), log, 3000);
    Check(wcslen(log) > 20, L"installer window: the log tells what was done", log);
    HKEY k; bool autoOn = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS && RegQueryValueExW(k, L"Click2Screen", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (k) RegCloseKey(k);
    Check(autoOn == wantAuto, L"installer window: the autostart box is honoured");
    // Remove: asks first
    PostMessageW(w, WM_COMMAND, SU_REMOVE, 0);
    bool asked = DismissBox(pi.dwProcessId, IDYES, 10000);
    end = GetTickCount() + WMS(60000);
    do { Pump(200); } while (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES && (int)(end - GetTickCount()) > 0);
    Pump(1500);
    Check(asked && GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES, L"installer window: Remove asks first and then takes the program away");
    PostMessageW(w, WM_COMMAND, SU_CLOSE, 0);
    if (WaitForSingleObject(pi.hProcess, WMS(10000)) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}


// ---- recording effects: click waves and the lines of the drawing window are drawn into the frame --------------------------------------
static void T_Effects()
{
    MakeFixture(R(320, 520), R(260, 380));
    RECT fr = FxScreenRect();
    int w = g_fx.w, h = g_fx.h - 14;
    SetCursorPos(2, 2); Pump(80);
    FxStop();                                                   // forgets everything (no hook is installed by the test)
    RECT rc = { fr.left, fr.top, fr.left + w, fr.top + h };
    Grabber g0, g1;
    if (!g0.Init(w, h) || !g1.Init(w, h)) { FAIL(L"effects: no frame to draw into"); return; }
    // a wave on the green quarter (top right): its colour is told from what it changes there
    int button = R(0, 2);
    int cx = w * 3 / 4 + R(-20, 20), cy = h / 4 + R(-15, 15);
    g0.Grab(rc, true, false, false);
    FxTestClick(fr.left + cx, fr.top + cy, button);
    g1.Grab(rc, true, true, true);                                // the highlight option on: waves (and the halo, which is far from here)
    int ringN = 0, farN = 0; double dR = 0, dG = 0, dB = 0;
    const uint8_t* a = g0.Bits(); const uint8_t* b = g1.Bits();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t o = ((size_t)(h - 1 - y) * w + x) * 4;            // the DIB is bottom-up; both frames alike
            int r0 = a[o + 2], g_0 = a[o + 1], b0 = a[o], r1 = b[o + 2], g_1 = b[o + 1], b1 = b[o];
            int diff = abs(r1 - r0) + abs(g_1 - g_0) + abs(b1 - b0);
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (diff > 30 && d2 < 30 * 30) { ++ringN; dR += r1 - r0; dG += g_1 - g_0; dB += b1 - b0; }
            if (diff > 6 && d2 > 70 * 70) ++farN;
        }
    if (ringN > 0) { dR /= ringN; dG /= ringN; dB /= ringN; }
    bool colour = button == 0 ? (dB > dR && dB > 20) : button == 1 ? (dR > dB && dR > 20) : (dB > 20 && dR > 20);
    wchar_t dd[200]; _snwprintf(dd, 200, L"button %d, %d ring pixels, %d changed farN away, mean change %.0f,%.0f,%.0f", button, ringN, farN, dR, dG, dB);
    Check(ringN >= 25 && farN == 0, L"effects: a click draws a wave round the pointer and nothing else", dd);
    Check(ringN >= 25 && colour, L"effects: the wave has the colour of the button (left blue, right red, middle purple)", dd);
    // the waves belong to the highlight option: without it a click leaves no wave (the pointer itself is still shown)
    {
        FxStop();
        Grabber gc, gd; gc.Init(w, h); gd.Init(w, h);
        gc.Grab(rc, true, false, false);
        FxTestClick(fr.left + cx, fr.top + cy, 0);
        gd.Grab(rc, true, false, true);
        int ch3 = 0;
        for (size_t o = 0; o + 3 < (size_t)w * h * 4; o += 4)
            if (abs(gd.Bits()[o + 2] - gc.Bits()[o + 2]) + abs(gd.Bits()[o + 1] - gc.Bits()[o + 1]) + abs(gd.Bits()[o] - gc.Bits()[o]) > 30) ++ch3;
        Check(ch3 == 0, L"effects: with the highlight option off a click draws no wave", ch3 ? L"(something was drawn)" : L"");
    }
    // the waves belong to "include the pointer": with it off nothing is drawn
    {
        FxStop();
        Grabber ga, gb; ga.Init(w, h); gb.Init(w, h);
        ga.Grab(rc, false, false, false);
        FxTestClick(fr.left + cx, fr.top + cy, 0);
        gb.Grab(rc, false, true, true);
        int ch2 = 0;
        for (size_t o = 0; o + 3 < (size_t)w * h * 4; o += 4)
            if (abs(gb.Bits()[o + 2] - ga.Bits()[o + 2]) + abs(gb.Bits()[o + 1] - ga.Bits()[o + 1]) + abs(gb.Bits()[o] - ga.Bits()[o]) > 30) ++ch2;
        Check(ch2 == 0, L"effects: with \"include the pointer\" off a recording has no waves either", ch2 ? L"(something was drawn)" : L"");
    }
    // The drawn lines (a gradient of all the colours, smooth, anti-aliased). The colours come with the moment of drawing, so the test
    // gives the points the shades it wants (red - orange: they differ from both the blue and the yellow of the lower half).
    FxStop();
    int ty = h * 3 / 4, oy = ty - 30;                    // both in the lower half
    Grabber gbase; gbase.Init(w, h);
    gbase.Grab(rc, true, false, false);                          // the frame without any line
    for (int i = 0; i < 10; ++i) FxTestTrail(fr.left + 30 + i * 20, fr.top + oy, i == 0, 12000);                  // a stroke older than the lifetime (10 s): the oldest goes first
    for (int i = 0; i < 10; ++i) FxTestTrailShade(fr.left + 30 + i * 20, fr.top + ty, i == 0, 0);
    Grabber g2; g2.Init(w, h);
    g2.Grab(rc, true, false, true);
    int red = 0, tested = 0, oldRed = 0, oldTested = 0;
    for (int x = 40; x < 30 + 9 * 20 - 5; x += 3) {
        ++tested; if (LineAt(g2.Bits(), gbase.Bits(), w, h, x, ty)) ++red;
        ++oldTested; if (LineAt(g2.Bits(), gbase.Bits(), w, h, x, oy)) ++oldRed;
    }
    wchar_t d2[160]; _snwprintf(d2, 160, L"%d of %d sampled points drawn on the fresh line, %d of %d on the old one", red, tested, oldRed, oldTested);
    Check(tested > 5 && red * 10 >= tested * 9, L"effects: a drawn line is in the frame", d2);
    Check(oldTested > 5 && oldRed == 0, L"effects: a line point that has lived its time has faded out (the lifetime is 10 seconds in the program)", d2);
    FxStop();
    // the lines of the drawing window (the left button draws, the right one wipes) are in the frame, and the wipe takes them all
    {
        Grabber gk; gk.Init(w, h);
        FxStart();
        for (int i = 0; i < 12; ++i) FxTestTrailShade(fr.left + 40 + i * 14, fr.top + ty, i == 0, 0);
        gk.Grab(rc, true, false, true);
        int redL = 0, tL = 0;
        for (int x = 46; x < 40 + 11 * 14 - 4; x += 3) { ++tL; if (LineAt(gk.Bits(), gbase.Bits(), w, h, x, ty)) ++redL; }
        FxWipe();
        gk.Grab(rc, true, false, true);
        int redR = 0;
        for (int x = 46; x < 40 + 11 * 14 - 4; x += 3) if (LineAt(gk.Bits(), gbase.Bits(), w, h, x, ty)) ++redR;
        // a line is a gradient: points drawn at different moments have different colours (here 30 shades from red to orange in 25 points)
        for (int i = 0; i < 25; ++i) FxTestTrailShade(fr.left + 40 + i * 10, fr.top + ty, i == 0, 250 + i * 5 / 4);
        gk.Grab(rc, true, false, true);
        int cr[25], cg[25], cb[25], good = 0;
        for (int i = 0; i < 25; ++i) {
            int x = 44 + i * 10; if (i == 24) x -= 6;
            size_t o = ((size_t)(h - 1 - ty) * w + x) * 4; cr[i] = gk.Bits()[o + 2]; cg[i] = gk.Bits()[o + 1]; cb[i] = gk.Bits()[o];
            if (LineAt(gk.Bits(), gbase.Bits(), w, h, x, ty)) ++good;
        }
        int spread = 0;
        for (int i = 0; i < 25; ++i) for (int j = i + 1; j < 25; ++j) { int d = abs(cr[i] - cr[j]) + abs(cg[i] - cg[j]) + abs(cb[i] - cb[j]); if (d > spread) spread = d; }
        wchar_t gd[200]; _snwprintf(gd, 200, L"%d of 25 pixels on the line, the colours of the stroke differ by up to %d (start %d,%d,%d end %d,%d,%d)", good, spread, cr[0], cg[0], cb[0], cr[24], cg[24], cb[24]);
        Check(good >= 23 && spread > 60, L"effects: a line is a gradient of colours, not one colour", gd);
        // the colours change smoothly along a long segment between two far points (not one colour and then a jump)
        FxWipe();
        int span = w - 90 < 300 ? w - 90 : 300;
        FxTestTrailShade(fr.left + 40, fr.top + ty, true, 250);
        FxTestTrailShade(fr.left + 40 + span, fr.top + ty, false, 40);
        gk.Grab(rc, true, false, true);
        int prevR = -1, prevG = 0, prevB = 0, maxStep = 0, firstR = 0, firstG = 0, firstB = 0, lastR = 0, lastG = 0, lastB = 0, samples = 0;
        for (int x = 46; x < 40 + span - 6; x += 3) {
            size_t o = ((size_t)(h - 1 - ty) * w + x) * 4; int r = gk.Bits()[o + 2], g = gk.Bits()[o + 1], bl = gk.Bits()[o];
            if (prevR >= 0) { int d = abs(r - prevR) + abs(g - prevG) + abs(bl - prevB); if (d > maxStep) maxStep = d; }
            else { firstR = r; firstG = g; firstB = bl; }
            lastR = r; lastG = g; lastB = bl; ++samples;
            prevR = r; prevG = g; prevB = bl;
        }
        int total = abs(firstR - lastR) + abs(firstG - lastG) + abs(firstB - lastB);
        wchar_t sd[200]; _snwprintf(sd, 200, L"%d samples along %d px: the biggest step between neighbours %d, start %d,%d,%d end %d,%d,%d", samples, span, maxStep, firstR, firstG, firstB, lastR, lastG, lastB);
        Check(samples > 20 && maxStep < 40 && total > 100, L"effects: the colours of a long segment change smoothly from one end to the other", sd);
        // smooth edges: a slanted line has pixels of in-between colour (an aliased one would have none), and there is no black outline
        FxWipe();
        FxTestTrailShade(fr.left + 40, fr.top + ty - 30, true, 0);
        FxTestTrailShade(fr.left + 40 + 110, fr.top + ty + 30, false, 0);
        gk.Grab(rc, true, false, true);
        int mixed = 0, dark = 0;
        for (int y = ty - 36; y <= ty + 36; ++y)
            for (int x = 34; x < 40 + 120; ++x) {
                size_t o = ((size_t)(h - 1 - y) * w + x) * 4; int r = gk.Bits()[o + 2], g = gk.Bits()[o + 1], bl = gk.Bits()[o];
                if (r > 40 && r < 215 && g < 40) ++mixed;                       // between the blue of the background and the red of the line
                if (r < 60 && g < 60 && bl < 60) ++dark;                        // a black outline would be here
            }
        wchar_t ad[160]; _snwprintf(ad, 160, L"%d pixels of in-between colour on the edges, %d dark pixels", mixed, dark);
        Check(mixed >= 40, L"effects: the lines have smooth (anti-aliased) edges", ad);
        Check(dark == 0, L"effects: the lines have no black outline", ad);
        FxStop(); SetCursorPos(2, 2);
        wchar_t kd[160]; _snwprintf(kd, 160, L"%d of %d points drawn; after the wipe: %d", redL, tL, redR);
        Check(tL > 5 && redL * 10 >= tL * 8, L"effects: the lines of the drawing window are in the frame", kd);
        Check(redR == 0, L"effects: the wipe removes all the lines from the frame at once", kd);
    }
    // the mouse pen of the drawing window: points given by the program itself (works over windows of any privilege)
    {
        POINT pt[4096]; unsigned char br[4096]; unsigned ver = 0;
        FxStart();
        FxPenPoint(100, 100, true); FxPenPoint(120, 110, false); FxPenPoint(120, 110, false); FxPenPoint(150, 130, false);   // the same point twice counts once
        FxPenPoint(300, 300, true); FxPenPoint(310, 320, false);
        int n = FxTrailSnapshot(pt, br, 4096, &ver);
        bool shape = n == 5 && br[0] == 1 && br[1] == 0 && br[2] == 0 && br[3] == 1 && pt[3].x == 300 && pt[4].y == 320;
        FxWipe();
        int after = FxTrailSnapshot(pt, br, 4096, &ver);
        FxStop();
        wchar_t dd[120]; _snwprintf(dd, 120, L"%d points in two strokes (shape ok %d), after the wipe %d", n, (int)shape, after);
        Check(shape, L"drawing: the mouse pen adds points in strokes (a repeated point is skipped)", dd);
        Check(after == 0, L"drawing: wiping removes every line at once", dd);
    }
    // the yellow circle round the pointer is for recordings only: a screenshot never has it
    {
        Grabber s1, s2; s1.Init(w, h); s2.Init(w, h);
        SetCursorPos(fr.left + w / 2, fr.top + h / 2); Pump(80);
        s1.Grab(rc, true, true, false);                                 // a screenshot with the highlight option on
        s2.Grab(rc, true, false, false);                                // ... and off
        SetCursorPos(2, 2);
        Check(memcmp(s1.Bits(), s2.Bits(), (size_t)w * h * 4) == 0, L"effects: a screenshot has no yellow circle round the pointer whatever the option says");
    }
    // a real click, seen by the low-level mouse hook
    FxStart();
    SetCursorPos(fr.left + cx, fr.top + cy); Pump(120);
    Grabber g3; g3.Init(w, h);
    g3.Grab(rc, true, true, true);                                         // before the click: the pointer and its halo, no wave yet
    unsigned seen = 0; unsigned long herr = 0; bool hooked = false;
    for (int attempt = 0; attempt < 2 && !seen; ++attempt) {             // a slow machine delivers the click late (or drops it once): wait for the hook, then try once more
        if (attempt) g3.Grab(rc, true, true, true);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        DWORD until = GetTickCount() + WMS(3000);
        do { Pump(50); hooked = FxTestHook(&seen, &herr); } while (!seen && (int)(until - GetTickCount()) > 0);
    }
    Pump(120);
    Grabber g4; g4.Init(w, h);
    g4.Grab(rc, true, true, true);                                         // after it: what has changed is the wave
    int changed = 0;
    const uint8_t* e0 = g3.Bits(); const uint8_t* e1 = g4.Bits();
    for (size_t o = 0; o + 3 < (size_t)w * h * 4; o += 4)
        if (abs(e1[o + 2] - e0[o + 2]) + abs(e1[o + 1] - e0[o + 1]) + abs(e1[o] - e0[o]) > 30) ++changed;
    FxStop();
    SetCursorPos(2, 2);
    if (changed < 25 && g_wine) SKIP(L"effects: Wine does not pass a synthetic click to the low-level hook");
    else if (!seen && hooked) Rep(L"WARN", L"effects: this Windows did not deliver the synthetic click to the low-level hook in %d s (the hook is installed, error %lu): nothing to check", 2 * WMS(3000) / 1000, herr);
    else { wchar_t hd[160]; _snwprintf(hd, 160, L"%d pixels changed, hook installed %d (error %lu), clicks seen by the hook %u", changed, (int)hooked, herr, seen); Check(changed >= 25, L"effects: a real click (low-level mouse hook) makes a wave in the frame", hd); }
}

// ---- sound: the conversion, the mix, the queue, and the speakers themselves ---------------------------------------------
// The AVI sink: a still picture is written once and then repeated (empty chunks), and a changed one is written again. 60 frames of one picture
// and one different at the end must make exactly two real JPEG chunks.
static void T_AviStill()
{
    if (!ImageFormatAvailable(IMG_JPG)) { SKIP(L"avi still: no JPEG encoder here"); return; }
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\still.avi", g_dir);
    const int W = 320, H = 200, FPS = 25;
    wchar_t why[160] = L"";
    FrameSink* sink = CreateAviSink(path, W, H, FPS, 1, false, why, 160);
    if (!sink) { FAIL(L"avi still: the sink could not be made: %s", why); return; }
    uint8_t* dib = new uint8_t[(size_t)W * H * 4];
    for (size_t i = 0; i < (size_t)W * H; ++i) ((uint32_t*)dib)[i] = (i / W) % 2 ? 0x00FF8000u : 0x000080FFu;      // stripes: not a flat picture
    bool ok = true;
    for (int i = 0; i < 60 && ok; ++i) ok = sink->Video(dib, (LONGLONG)i * 10000000LL / FPS);
    ((uint32_t*)dib)[1000] ^= 0x00FFFFFF;                                                                 // one pixel changes
    ok = ok && sink->Video(dib, (LONGLONG)60 * 10000000LL / FPS);
    ok = sink->Close(ok) && ok;
    delete sink; delete[] dib;
    uint8_t* keep = nullptr; AviInfo a = ReadAvi(path, 0, &keep); delete[] keep;
    LARGE_INTEGER sz; sz.QuadPart = 0;
    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf != INVALID_HANDLE_VALUE) { GetFileSizeEx(hf, &sz); CloseHandle(hf); }
    wchar_t d[160]; _snwprintf(d, 160, L"%u video chunks, %u empty, the file is %lld bytes", a.vChunks, a.emptyV, (long long)sz.QuadPart);
    Check(ok && a.ok && a.vChunks == 61 && a.emptyV == 59, L"avi still: a picture that does not change is written once, then repeated", d);
    DeleteFileW(path);
}

static void T_Audio()
{
    // 48 kHz stereo -> mono 44.1 kHz: the length, the pitch and the loudness stay, the stream is continuous over the blocks
    {
        const int rate = R(0, 1) ? 48000 : 32000, blocks = 100, bf = rate / 100;          // 1 s in 10 ms blocks
        const double freq = 1000.0 + R(0, 4) * 250.0;
        static int16_t out[200000]; int total = 0;
        double phase = 0; float prev = 0; double t = 0;
        float* blk = new float[(size_t)bf * 2];
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < bf; ++i, t += 1.0 / rate) { float v = (float)(0.5 * sin(6.283185307179586 * freq * t)); blk[2 * i] = v; blk[2 * i + 1] = v; }
            total += AudioToMono44k(blk, bf, 2, rate, &phase, &prev, out + total, 200000 - total);
        }
        delete[] blk;
        int crossings = 0, maxJump = 0, peak = 0;
        for (int i = 1; i < total; ++i) {
            if (out[i - 1] < 0 && out[i] >= 0) ++crossings;
            int j = abs(out[i] - out[i - 1]); if (j > maxJump) maxJump = j;
            if (abs(out[i]) > peak) peak = abs(out[i]);
        }
        double expectJump = 6.283185307179586 * freq / 44100.0 * 16383.0 * 1.2;
        wchar_t d[200]; _snwprintf(d, 200, L"%d Hz in, %d samples (44100 expected), %d periods, loudest %d, biggest step %d (limit %.0f)", rate, total, crossings, peak, maxJump, expectJump);
        Check(abs(total - 44100) <= 3, L"audio: one second of any rate becomes one second at 44.1 kHz", d);
        Check(abs(crossings - (int)freq) <= 2, L"audio: the pitch is kept by the conversion", d);
        Check(peak > 15500 && peak < 17000 && maxJump < expectJump, L"audio: the loudness is kept and the stream is continuous over the blocks (no clicks)", d);
        // the two channels are averaged: opposite signals cancel
        float opp[8] = { 1, -1, 1, -1, 1, -1, 1, -1 }; int16_t o2[16]; double ph = 0; float pv = 0;
        int n2 = AudioToMono44k(opp, 4, 2, 44100, &ph, &pv, o2, 16);
        bool silent = true; for (int i = 0; i < n2; ++i) if (abs(o2[i]) > 2) silent = false;
        Check(n2 > 0 && silent, L"audio: the channels are averaged to mono");
    }
    // the Windows resampler: a 96 kHz stream with a 1 kHz tone and a 40 kHz one (above the new Nyquist limit): the 40 kHz tone must not
    // fold into the band as a 4.1 kHz whistle, which linear interpolation lets through
    {
        const int rate = 96000, blocks = 100, bf = rate / 100;
        AudioResampler rs;
        if (!rs.Init(rate)) SKIP(L"audio: this Windows has no audio resampler to try");
        else {
            static int16_t out[200000]; static int16_t lin[200000]; int total = 0, tl = 0;
            double phase = 0; float prev = 0; double t = 0;
            float* blk = new float[bf]; float* st = new float[(size_t)bf * 2];
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < bf; ++i, t += 1.0 / rate) {
                    blk[i] = (float)(0.3 * sin(6.283185307179586 * 1000.0 * t) + 0.3 * sin(6.283185307179586 * 40000.0 * t));
                    st[2 * i] = st[2 * i + 1] = blk[i];
                }
                total += rs.Process(blk, bf, out + total, 200000 - total);
                tl += AudioToMono44k(st, bf, 2, rate, &phase, &prev, lin + tl, 200000 - tl);
            }
            delete[] blk; delete[] st;
            auto level = [](const int16_t* x, int n, double f) {                          // the Goertzel level of one frequency, in units of the sample
                double w = 6.283185307179586 * f / 44100.0, c = 2.0 * cos(w), s1 = 0, s2 = 0;
                for (int i = 0; i < n; ++i) { double v = x[i] / 32768.0 + c * s1 - s2; s2 = s1; s1 = v; }
                return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / n;
            };
            const int skip = 2000, nn = total - skip;                                       // the filter's start-up is left out
            double tone = level(out + skip, nn, 1000.0), alias = level(out + skip, nn, 4100.0), aliasLin = level(lin + skip, tl - skip, 4100.0);
            wchar_t d[240]; _snwprintf(d, 240, L"%d samples (44100 expected), tone %.3f, alias at 4.1 kHz %.4f (linear interpolation: %.4f)", total, tone, alias, aliasLin);
            Check(abs(total - 44100) <= 600, L"audio: the Windows resampler turns one second at 96 kHz into one second at 44.1 kHz", d);
            Check(tone > 0.2 && tone < 0.4, L"audio: the Windows resampler keeps the tone and its loudness", d);
            Check(alias < tone * 0.03 && alias < aliasLin * 0.5, L"audio: the Windows resampler does not fold ultrasound into the band (the linear conversion does)", d);
        }
    }
    // the mix is a sum that is clipped, the queue pads with silence and drops the oldest
    {
        int16_t a[4] = { 30000, -30000, 100, 0 }, b[4] = { 30000, -30000, 200, 0 }, o[4];
        AudioMix(a, b, 4, o);
        Check(o[0] == 32767 && o[1] == -32768 && o[2] == 300 && o[3] == 0, L"audio: the mix adds the sources and clips at the limits");
        Fifo16 q; int16_t in[100]; for (int i = 0; i < 100; ++i) in[i] = (int16_t)(i + 1);
        q.Push(in, 100);
        int16_t got[150]; q.Pop(got, 150);
        bool pad = got[0] == 1 && got[99] == 100 && got[100] == 0 && got[149] == 0 && q.Size() == 0;
        q.Push(in, 100); q.TrimTo(10); int16_t g2[10]; q.Pop(g2, 10);
        Check(pad && g2[0] == 91 && g2[9] == 100, L"audio: a queue pads what is missing with silence and keeps only the newest samples when trimmed");
    }
    // the speakers: when this machine has a playback device, a tone played on it is captured
    {
        SpeakerCapture sc;
        if (!sc.Open()) { SKIP(L"audio: no playback device to capture here (loopback)"); return; }
        const int n = 44100;
        static unsigned char wav[44 + 44100 * 4];
        memcpy(wav, "RIFF", 4); unsigned v = 36 + n * 4; memcpy(wav + 4, &v, 4); memcpy(wav + 8, "WAVEfmt ", 8); v = 16; memcpy(wav + 16, &v, 4);
        unsigned short us = 1; memcpy(wav + 20, &us, 2); us = 2; memcpy(wav + 22, &us, 2); v = 44100; memcpy(wav + 24, &v, 4); v = 44100 * 4; memcpy(wav + 28, &v, 4);
        us = 4; memcpy(wav + 32, &us, 2); us = 16; memcpy(wav + 34, &us, 2); memcpy(wav + 36, "data", 4); v = n * 4; memcpy(wav + 40, &v, 4);
        short* sp = (short*)(wav + 44);
        for (int i = 0; i < n; ++i) { short x = (short)(12000 * sin(6.283185307179586 * 1000.0 * i / 44100.0)); sp[2 * i] = sp[2 * i + 1] = x; }
        int16_t buf[8192]; long long total = 0; double energy = 0;
        PlaySoundW((LPCWSTR)wav, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        DWORD end = GetTickCount() + 1500;
        while ((int)(end - GetTickCount()) > 0) {
            int got = sc.Pull(buf, 8192);
            for (int i = 0; i < got; ++i) energy += (double)buf[i] * buf[i];
            total += got;
            Sleep(20);
        }
        PlaySoundW(nullptr, nullptr, 0);
        double rms = total > 0 ? sqrt(energy / total) : 0;
        wchar_t d[160]; _snwprintf(d, 160, L"%lld samples captured, loudness %.0f", total, rms);
        Check(total > 20000 && rms > 1500, L"audio: a tone played on the speakers is captured by the loopback", d);
    }
}

// ---- the red frame (blinking) and the red countdown digits of the real program -----------------------------------------
static void T_AppFrame()
{
    MakeFixture(R(380, 700), R(260, 460));
    Settings s; PrepareApp(s);
    s.delaySec = 3; s.capMouse = false;
    App a;
    if (!StartApp(a, s)) { FAIL(L"frame: the program did not start"); StopApp(a, false); return; }
    RECT fr = FxScreenRect();
    LONG ex = GetWindowLongW(a.main, GWL_EXSTYLE);
    Check(!(ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_APPWINDOW), L"frame: the main window has a taskbar button while it is shown");
    {   // Minimize waits on the taskbar and a screenshot leaves it there; Close sends the window to the tray
        PostMessageW(a.main, WM_COMMAND, IDC_MIN, 0);
        DWORD me = GetTickCount() + WMS(3000);
        while (!IsIconic(a.main) && (int)(me - GetTickCount()) > 0) Sleep(50);
        bool minimized = IsIconic(a.main) != 0 && IsWindowVisible(a.main);
        Check(minimized, L"frame: Minimize minimizes the main window to the taskbar (it stays shown, not hidden to the tray)");
        int shots = CountFiles(s.folder);
        PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0);
        DWORD se = GetTickCount() + WMS(10000);
        while (CountFiles(s.folder) <= shots && (int)(se - GetTickCount()) > 0) Sleep(100);
        Sleep(500);
        Check(CountFiles(s.folder) > shots && IsIconic(a.main), L"frame: a screenshot taken while the window is minimized does not bring it back");
        ShowWindow(a.main, SW_RESTORE); Sleep(300);
        PostMessageW(a.main, WM_COMMAND, IDC_CLOSE, 0);
        DWORD ce = GetTickCount() + WMS(3000);
        while (IsWindowVisible(a.main) && (int)(ce - GetTickCount()) > 0) Sleep(50);
        Check(!IsWindowVisible(a.main) && AppAlive(a), L"frame: Close hides the main window to the tray and the program keeps running");
        PostMessageW(a.main, WM_C2S_SHOW, 0, 0);
        DWORD re = GetTickCount() + WMS(3000);
        while (!IsWindowVisible(a.main) && (int)(re - GetTickCount()) > 0) Sleep(50);
        Check(IsWindowVisible(a.main) && !IsIconic(a.main), L"frame: the tray brings the window back");
    }
    POINT p1 = { fr.left + 30, fr.top + 30 }, p2 = { fr.left + 30 + R(160, 260), fr.top + 30 + R(110, 170) };
    int before = CountFiles(s.folder);
    PostMessageW(a.main, WM_HOTKEY, HK_REGION, 0);
    bool driven = DrivePicker(a.pi.dwProcessId, 0, p1, p2);
    bool seenOn = false, seenOff = false, digit = false, centred = false, framed = false, digitBig = false;
    DWORD end = GetTickCount() + WMS(3600);
    while (driven && (int)(end - GetTickCount()) > 0) {
        HWND f = FindWin(a.pi.dwProcessId, L"Click2ScreenFrame", 0);
        HWND d = FindWin(a.pi.dwProcessId, L"Click2ScreenCount", 0);
        if (f) {
            if (IsWindowVisible(f)) { seenOn = true; RECT r; GetWindowRect(f, &r); RECT want = { p1.x - 3, p1.y - 3, p2.x + 3, p2.y + 3 }; framed = framed || EqualRect(&r, &want) || (abs(r.left - want.left) <= 1 && abs(r.top - want.top) <= 1 && abs(r.right - want.right) <= 1 && abs(r.bottom - want.bottom) <= 1); }
            else seenOff = true;
        }
        if (d && IsWindowVisible(d)) {
            digit = true; RECT r; GetWindowRect(d, &r);
            if (r.right - r.left > 112 || r.right - r.left < 30) digitBig = true;                  // 36..110 units: small, not as big as before
            int mx = (r.left + r.right) / 2, my = (r.top + r.bottom) / 2, cx = (p1.x + p2.x) / 2, cy = (p1.y + p2.y) / 2;
            centred = centred || (abs(mx - cx) <= 2 && abs(my - cy) <= 2);
        }
        if (seenOn && seenOff && digit && framed && centred) break;
        if (!f && !d && CountFiles(s.folder) > before) break;
        Pump(60);
    }
    wchar_t file[MAX_PATH] = L"";
    bool got = driven && WaitNewFile(s.folder, before, 20000, file);
    Pump(300);
    HWND f2 = FindWin(a.pi.dwProcessId, L"Click2ScreenFrame", 0), d2 = FindWin(a.pi.dwProcessId, L"Click2ScreenCount", 0);
    wchar_t dd[200]; _snwprintf(dd, 200, L"driven %d, on %d off %d, frame at the area %d, digit %d centred %d", (int)driven, (int)seenOn, (int)seenOff, (int)framed, (int)digit, (int)centred);
    Check(framed, L"frame: after the region is chosen a red frame stands round it (as for a recording)", dd);
    Check(seenOn && seenOff, L"frame: the red frame blinks", dd);
    Check(digit && centred && !digitBig, L"frame: the countdown digits are small and stand in the middle of the chosen area", dd);
    Check(got && !f2 && !d2, L"frame: the picture is saved and the frame and the digits are gone", dd);
    // a region recording: the frame is steady while the area is only chosen and during the countdown, and blinks while it records
    PostMessageW(a.main, WM_COMMAND, IDC_REC_REGION, 0);
    bool drv2 = DrivePicker(a.pi.dwProcessId, 0, p1, p2);
    HWND panel = drv2 ? FindReady(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000) : nullptr;
    bool armOn = false, armOff = false, recOn = false, recOff = false;
    DWORD t1 = GetTickCount();
    while ((int)(GetTickCount() - t1) < 1500) {
        HWND f = FindWin(a.pi.dwProcessId, L"Click2ScreenFrame", 0);
        if (f) { if (IsWindowVisible(f)) armOn = true; else armOff = true; }
        Pump(60);
    }
    if (panel) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0); LangSelect(s.language); }
    bool running = panel && WaitText(panel, IDC_P_START, T(S_PAUSE), (s.delaySec + 8) * 1000);
    DWORD t2 = GetTickCount();
    while (running && (int)(GetTickCount() - t2) < 2000) {
        HWND f = FindWin(a.pi.dwProcessId, L"Click2ScreenFrame", 0);
        if (f) { if (IsWindowVisible(f)) recOn = true; else recOff = true; }
        Pump(60);
    }
    if (panel) PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0);
    wchar_t d3[160]; _snwprintf(d3, 160, L"armed: on %d off %d; recording (running %d): on %d off %d", (int)armOn, (int)armOff, (int)running, (int)recOn, (int)recOff);
    Check(armOn && !armOff, L"frame: the red frame does not blink while the area is only chosen", d3);
    Check(running && recOn && recOff, L"frame: the red frame blinks while the recording runs", d3);
    StopApp(a);
}

// ---- the whole path: the real program records an AVI while real clicks and a line of the drawing window happen; the frames are checked ---
static void T_AppEffects()
{
    if (!VideoFormatAvailable(VC_AVI)) { SKIP(L"program effects: no JPEG encoder for the AVI here"); return; }
    MakeFixture(R(520, 700), R(380, 480));
    Settings s; PrepareApp(s);
    s.videoCodec = VC_AVI; s.capMouse = true; s.highlight = Chance(50); s.delaySec = 0; s.mic = false; s.videoQuality = 0; s.fps = 25;
    App a;
    if (!StartApp(a, s, L"", true)) { FAIL(L"program effects: the program did not start"); StopApp(a, false); return; }
    RECT fr = FxScreenRect();
    int w = g_fx.w, h = g_fx.h - 14;
    POINT p1 = { fr.left, fr.top }, p2 = { fr.left + w, fr.top + h };
    SetCursorPos(2, 2);
    PostMessageW(a.main, WM_COMMAND, IDC_REC_REGION, 0);
    bool driven = DrivePicker(a.pi.dwProcessId, 0, p1, p2);
    HWND panel = driven ? FindReady(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000) : nullptr;
    LangSelect(s.language);
    bool running = false;
    if (panel) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0); running = WaitText(panel, IDC_P_START, T(S_PAUSE), 15000); }
    int cx = w * 3 / 4, cy = h / 4, ty = h * 3 / 4;                      // the click in the green quarter, the trail in the lower half
    bool overlaySeen = false; DWORD trailAffinity = 0xFFFFFFFF;
    if (running) {
        Pump(500);
        SetCursorPos(fr.left + cx, fr.top + cy); Pump(150);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); Sleep(60); mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        Pump(900);
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0);                    // the drawing on: its window takes the mouse; the left button draws across the lower half
        HWND ink = FindWin(a.pi.dwProcessId, C2S_CLASS_INK, 4000);
        { DWORD vis = GetTickCount() + WMS(4000); while (ink && !IsWindowVisible(ink) && (int)(vis - GetTickCount()) > 0) Sleep(50); }     // found while it is still being created: wait until it is shown
        RECT vs = VirtualScreen();
        if (ink) {
            PostMessageW(ink, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(fr.left + 40 - vs.left, fr.top + ty - vs.top));
            for (int i = 0; i <= 40; ++i) {
                PostMessageW(ink, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(fr.left + 40 + i * 6 - vs.left, fr.top + ty - vs.top)); Pump(40);
                if (i > 20) { HWND tw = FindWin(a.pi.dwProcessId, L"Click2ScreenTrail", 0); if (tw && IsWindowVisible(tw)) { overlaySeen = true; trailAffinity = AffinityOf(tw); } }
            }
            PostMessageW(ink, WM_LBUTTONUP, 0, MAKELPARAM(fr.left + 280 - vs.left, fr.top + ty - vs.top));
        }
        Pump(500);
        PostMessageW(a.main, WM_HOTKEY, HK_DRAW, 0);                    // the drawing off again before the recording is stopped
        Pump(200);
        SetCursorPos(2, 2); Pump(300);
        PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0);
    }
    wchar_t file[MAX_PATH] = L"";
    int before = 0;
    bool got = running && WaitNewFile(s.folder, before, 20000, file);
    Pump(500);
    int waveFrames = 0, trailFrames = 0, frames = 0, tested = 0;
    uint8_t* ref = nullptr; int refW = 0;                                   // the first decoded picture, before anything is drawn
    if (got) {
        for (int n = 2; n <= 400; n += 2) {                               // every second picture of the video
            uint8_t* keep = nullptr;
            AviInfo ai = ReadAvi(file, n, &keep);
            if (!ai.ok || ai.pickSize == 0 || !keep) { delete[] keep; break; }
            wchar_t jp[MAX_PATH]; _snwprintf(jp, MAX_PATH, L"%s\\fxframe%d.jpg", g_dir, n);
            HANDLE hf = CreateFileW(jp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (hf != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(hf, keep + ai.pickOff, ai.pickSize, &wr, nullptr); CloseHandle(hf); }
            delete[] keep;
            int dw = 0, dh = 0; uint8_t* px = nullptr;
            ++frames;
            if (DecodePixelsOnce(jp, &dw, &dh, &px) && dw >= w - 4 && dh >= h - 4) {
                ++tested;
                if (!ref) { ref = new uint8_t[(size_t)dw * dh * 4]; memcpy(ref, px, (size_t)dw * dh * 4); refW = dw; }
                int wave = 0, trail = 0;
                auto blueish = [](const uint8_t* q) { return q[0] > 150 && q[2] < 130 && q[1] < 200 && q[1] > 60; };
                for (int y = 0; y < dh; ++y)
                    for (int x = 0; x < dw; ++x) {
                        const uint8_t* q = px + ((size_t)y * dw + x) * 4;
                        int dx = x - cx, dy = y - cy;
                        if (dx * dx + dy * dy < 80 * 80 && dx * dx + dy * dy >= 22 * 22 && blueish(q) && (!ref || ref == px || !blueish(ref + ((size_t)y * dw + x) * 4))) ++wave;        // blue-ish over the green quarter, outside the 22 px the pointer itself covers (the blue "busy" ring of Windows 10/11 is no wave)
                        if (y > h / 2 + 8 && abs(y - ty) < 8 && ref) { const uint8_t* rq = ref + ((size_t)y * dw + x) * 4; if (abs(q[0] - rq[0]) + abs(q[1] - rq[1]) + abs(q[2] - rq[2]) > 120) ++trail; }   // differs from the first picture: the line or its outline
                    }
                if (wave >= 24) ++waveFrames;
                if (trail >= 12) ++trailFrames;
            }
            delete[] px;
            DeleteFileW(jp);
        }
    }
    delete[] ref; (void)refW;
    wchar_t dd[220]; _snwprintf(dd, 220, L"the video has %d frames, %d decoded: %d show the wave, %d show the trail (running %d, file %d)", frames, tested, waveFrames, trailFrames, (int)running, (int)got);
    Check(got && tested > 5, L"program effects: the real program records the AVI", dd);
    if (s.highlight) Check(waveFrames >= (g_scale > 1 ? 1 : 2), L"program effects: a real click is a blue wave in the recorded video (a slow machine records few frames in the 0.65 s a wave lasts)", dd);
    else Check(waveFrames == 0, L"program effects: with the highlight option off a real click leaves no wave in the video", dd);
    Check(trailFrames >= 3, L"program effects: a line drawn with the left button in the drawing window is in the recorded video", dd);
    Check(overlaySeen, L"program effects: the line is shown on the screen too, while recording", dd);
    if (AffinityReported()) { wchar_t ad2[300]; _snwprintf(ad2, 300, L"display affinity of the line window %lx (0x11 = out of the capture); %s", (unsigned long)trailAffinity, dd); if (trailAffinity == 0x11) Check(true, L"program effects: while our own recording runs the line window is kept out of the capture (the line is in the picture already)", ad2);
        else WARN(L"program effects: this Windows refuses the exclusion from capture for the layered line window (error 8 in the log): it stays in the capture and the line is drawn twice in one place; %s", ad2); }
    else SKIP(L"program effects: this Windows does not report the exclusion from capture");
    StopApp(a);
}

// ---- more about the real program ------------------------------------------------------------------------------------
// "System language": with the automatic setting the program speaks the language of Windows (under Wine: of the locale the
// whole test runs in - tools/wine-soak.sh picks LC_ALL at random for every run). Both processes see the same language.
static void T_AppLocale()
{
    Settings s; PrepareApp(s); s.language = 0;
    App a;
    if (!StartApp(a, s)) { FAIL(L"locale: the program did not start"); StopApp(a, false); return; }
    wchar_t tag[80]; _snwprintf(tag, 80, L"automatic language, the system's is %s", LangName(LangAutoIndex()));
    AppTexts(a, 0, tag, s.hotkey);
    StopApp(a);
}

// The program is killed in the middle of a recording (a crash, the task manager): the next start must be clean.
static void T_AppCrashRestart()
{
    MakeFixture(R(300, 500), R(220, 360));
    Settings s; PrepareApp(s);
    App a;
    if (!StartApp(a, s)) { FAIL(L"crash: the program did not start"); StopApp(a, false); return; }
    PostMessageW(a.main, WM_COMMAND, IDC_REC_DESKTOP, 0);
    HWND panel = FindReady(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000);
    if (panel) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0); LangSelect(s.language); WaitText(panel, IDC_P_START, T(S_PAUSE), (s.delaySec + 6) * 1000); Pump(R(200, 1200)); }
    TerminateProcess(a.pi.hProcess, 66);                              // no chance to clean up
    WaitForSingleObject(a.pi.hProcess, 10000);
    CloseHandle(a.pi.hProcess); CloseHandle(a.pi.hThread); memset(&a.pi, 0, sizeof(a.pi));
    Settings now; SettingsLoad(now);
    wchar_t why[160] = L"";
    Check(SettingsEqual(s, now, true, why), L"crash: the settings are intact after the program was killed", why);
    App b;
    bool ok = StartApp(b, s);                                         // the same single-instance name: the mutex is gone with the process
    Check(ok && AppAlive(b), L"crash: the program starts again after being killed in a recording");
    if (ok) {
        int before = CountFiles(s.folder);
        PostMessageW(b.main, WM_HOTKEY, HK_DESKTOP, 0);
        wchar_t f[MAX_PATH];
        DWORD t0 = GetTickCount();
        bool got = WaitNewFile(s.folder, before, 20000, f);
        wchar_t d[400]; _snwprintf(d, 400, L"files before %d, after %d, alive %d, waited %lu ms, image format %d, delay %d, folder %s (exists %d)", before, CountFiles(s.folder), (int)AppAlive(b),
                                   (unsigned long)(GetTickCount() - t0), s.imageFormat, s.delaySec, s.folder, (int)(GetFileAttributesW(s.folder) != INVALID_FILE_ATTRIBUTES));
        Check(got, L"crash: and works (a screenshot after the restart)", d);
    }
    StopApp(b);
}

// Hotkeys that move between actions: every new key is the old key of another action (F2/F3/F4 rotated). The program must free all
// the old keys before it takes the new ones, or it loses the key it still holds itself (error 1409).
static void T_AppHotkeyMove()
{
    Settings s; PrepareApp(s);
    for (int i = 0; i < HKI_COUNT; ++i) s.hotkey[i] = 0;
    s.hotkey[0] = VK_F2; s.hotkey[1] = VK_F3; s.hotkey[2] = VK_F4;
    App a;
    if (!StartApp(a, s)) { FAIL(L"hotkey move: the program did not start"); StopApp(a, false); return; }
    DWORD tOpen = GetTickCount();
    PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
    HWND o = FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 10000);
    DWORD openMs = GetTickCount() - tOpen;
    { wchar_t d[100]; _snwprintf(d, 100, L"Options took %lu ms to appear", (unsigned long)openMs); Check(o != nullptr && (int)openMs < WMS(6000), L"hotkey move: Options opens its window without a long freeze", d); }
    {   // the probe the program makes at start-up in the background keeps its answer in the registry (beside the settings), per Windows build
        wchar_t key[200]; _snwprintf(key, 200, L"%s\\Probe", SettingsKeyName());
        DWORD stopAt = GetTickCount() + WMS(25000); bool stored = false; DWORD build = 0, hevc = 99;
        while (!stored && (int)(stopAt - GetTickCount()) > 0) {
            HKEY k;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                DWORD n = 4, t = 0;
                stored = RegQueryValueExW(k, L"Build", nullptr, &t, (BYTE*)&build, &n) == ERROR_SUCCESS; n = 4;
                RegQueryValueExW(k, L"Hevc", nullptr, &t, (BYTE*)&hevc, &n);
                RegCloseKey(k);
            }
            if (!stored) Sleep(200);
        }
        wchar_t d[120]; _snwprintf(d, 120, L"Windows build %lu, HEVC encoder %s", (unsigned long)build, hevc == 1 ? L"works" : hevc == 0 ? L"does not work" : L"unknown");
        Check(stored && hevc <= 1, L"hotkey move: the start-up probe of the video formats stores its answer in the registry", d);
    }
    if (o) {
        const int nk[3] = { VK_F3, VK_F4, VK_F2 };
        for (int i = 0; i < 3; ++i) {
            HWND e = GetDlgItem(o, O_HK0 + i);
            if (!e) continue;
            SendMessageW(e, WM_KEYDOWN, VK_BACK, 0);
            SendMessageW(e, WM_KEYDOWN, nk[i], 0);
        }
        PostMessageW(o, WM_COMMAND, IDOK, 0);
        DWORD end = GetTickCount() + WMS(10000);
        while (IsWindow(o) && (int)(end - GetTickCount()) > 0) Sleep(50);
        Sleep(WMS(500));
        HWND hw2 = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        int taken = 0;
        const int keys[3] = { VK_F2, VK_F3, VK_F4 };
        for (int k = 0; k < 3; ++k) {
            if (!RegisterHotKey(hw2, 90 + k, 0, (UINT)keys[k])) ++taken; else UnregisterHotKey(hw2, 90 + k);
        }
        if (hw2) DestroyWindow(hw2);
        wchar_t d[100]; _snwprintf(d, 100, L"%d of 3 moved keys are held by the program", taken);
        Check(!IsWindow(o) && taken == 3, L"hotkey move: keys that move between actions are all registered by the program", d);
    }
    {   // a program that does nothing does not use the processor (a busy loop would show here): 6 s of its own CPU time while it is idle
        Pump(WMS(3000));                                                 // the start-up work (probes, the options window just closed) settles first
        auto cpuOf = [&]() -> double {
            FILETIME c, e, k, u;
            if (!GetProcessTimes(a.pi.hProcess, &c, &e, &k, &u)) return -1;
            return (double)(((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e7;
        };
        double c0 = cpuOf(); DWORD t0 = GetTickCount();
        Pump(6000);
        double used = cpuOf() - c0, wall = (GetTickCount() - t0) / 1000.0;
        wchar_t d[120]; _snwprintf(d, 120, L"%.2f s of CPU in %.1f s idle: %.1f %% of a core", used, wall, 100.0 * used / wall);
        Check(c0 >= 0 && used / wall < 0.15, L"hotkey move: an idle program does not load the processor", d);
    }
    PostMessageW(a.main, WM_COMMAND, ID_TRAY_EXIT, 0);
    WaitForSingleObject(a.pi.hProcess, WMS(15000));
    StopApp(a, false);
}

// The key frames of a real H.264 recording are rare (the original has one every 20-24 s): a still screen is mostly key frames
// otherwise. The sync-sample table ('stss') of the finished file tells how many there are.
static int Mp4KeyFrames(const wchar_t* path, int* total)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 200000000LL) { CloseHandle(h); return -1; }
    unsigned char* b = (unsigned char*)malloc((size_t)sz.QuadPart);
    DWORD rd = 0; bool ok = b && ReadFile(h, b, (DWORD)sz.QuadPart, &rd, nullptr) && rd == (DWORD)sz.QuadPart;
    CloseHandle(h);
    int keys = -1; *total = -1;
    for (DWORD i = 4; ok && i + 16 < rd; ++i) {
        if (!memcmp(b + i, "stss", 4) && keys < 0) {
            keys = (int)((b[i + 8] << 24) | (b[i + 9] << 16) | (b[i + 10] << 8) | b[i + 11]);
            wchar_t list[300] = L""; int n = 0;                                    // which samples are the key ones (counted from 1)
            for (int k = 0; k < keys && k < 14 && i + 12 + 4 * (k + 1) <= rd; ++k) {
                DWORD o = i + 12 + 4 * k; int smp = (int)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]);
                n += _snwprintf(list + n, 300 - n, L"%d ", smp);
            }
            Rep(L"INFO", L"  key samples: %s", list);
        }
        if (!memcmp(b + i, "stsz", 4) && *total < 0) *total = (int)((b[i + 12] << 24) | (b[i + 13] << 16) | (b[i + 14] << 8) | b[i + 15]);   // sample_count after version/flags and sample_size
    }
    free(b);
    return keys;
}

// The audio codec of the first sound track of an MP4: 0x40 AAC, 0x6B / 0x69 MP3 (the object type of the esds box), 0 none found.
static int Mp4AudioObjectType(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(f, nullptr), got = 0;
    uint8_t* b = (uint8_t*)malloc(sz ? sz : 1);
    int type = 0;
    if (b && ReadFile(f, b, sz, &got, nullptr)) {
        for (DWORD i = 0; i + 60 < got && !type; ++i) {
            if (memcmp(b + i, "esds", 4)) continue;
            DWORD k = i + 8;                                           // after the name, the version and flags
            for (DWORD e = k + 40; k < e && k + 4 < got; ++k) if (b[k] == 0x04) {          // DecoderConfigDescriptor
                DWORD q = k + 1; while (q < got && (b[q] & 0x80)) ++q; ++q;                 // its length
                if (q < got) type = b[q];
                break;
            }
        }
    }
    free(b); CloseHandle(f);
    return type;
}

// The codec chosen in the settings is the codec in the file, or the recording is refused: never the other one in silence.
static void T_AudioCodecs()
{
    if (g_wine || !VideoFormatAvailable(VC_H264_BEST)) { SKIP(L"audio codecs: there is no real encoder here"); return; }
    MakeFixture(R(300, 500), R(220, 360));
    for (int codec = 0; codec <= 1; ++codec) {
        RecParams p; memset(&p, 0, sizeof(p));
        SettingsDefaults(p.cfg); p.cfg.mic = false; p.cfg.loopback = true; p.cfg.videoCodec = VC_H264_BEST; p.cfg.audioCodec = codec; p.cfg.fps = 10;
        p.rc = FxScreenRect();
        wcscpy(p.path, g_dir); wcscat(p.path, L"\\acodec.mp4");
        DeleteFileW(p.path);
        Recorder rec; wchar_t err[200] = L"";
        const wchar_t* nm = codec ? L"MP3" : L"AAC";
        if (!rec.Start(p, err, 200)) {
            if (codec) Rep(L"WARN", L"audio codecs: %s is refused with a message, not replaced (this Windows has no %s encoder?): %s", nm, nm, err);
            else FAIL(L"audio codecs: %s: Start failed: %s", nm, err);
            continue;
        }
        DWORD t0 = GetTickCount();
        while ((int)(GetTickCount() - t0) < 3000) Pump(100);
        bool stopped = rec.Stop();
        int type = Mp4AudioObjectType(p.path);
        wchar_t d[120]; _snwprintf(d, 120, L"chosen %s, object type 0x%02X in the file", nm, type);
        bool right = codec ? (type == 0x6B || type == 0x69) : type == 0x40;
        Check(stopped && right, L"audio codecs: the codec chosen is the codec recorded", d);
    }
}

// ---- a random video codec and a random audio codec at every quality: they work, the picture is upright, a worse quality is a smaller file ----------
static void QsTone(bool on)
{
    static unsigned char wav[44 + 44100 * 4];
    static bool made = false;
    if (on) {
        if (!made) {
            const int n = 44100;
            memcpy(wav, "RIFF", 4); unsigned v = 36 + n * 4; memcpy(wav + 4, &v, 4); memcpy(wav + 8, "WAVEfmt ", 8); v = 16; memcpy(wav + 16, &v, 4);
            unsigned short us = 1; memcpy(wav + 20, &us, 2); us = 2; memcpy(wav + 22, &us, 2); v = 44100; memcpy(wav + 24, &v, 4); v = 44100 * 4; memcpy(wav + 28, &v, 4);
            us = 4; memcpy(wav + 32, &us, 2); us = 16; memcpy(wav + 34, &us, 2); memcpy(wav + 36, "data", 4); v = n * 4; memcpy(wav + 40, &v, 4);
            short* sp = (short*)(wav + 44);
            unsigned seed = 12345;                                       // a tone with a noisy bed: the encoder has something to spend its bits on
            for (int i = 0; i < n; ++i) { seed = seed * 1664525u + 1013904223u; short x = (short)(9000 * sin(6.283185307179586 * 1000.0 * i / 44100.0) + (int)((seed >> 16) % 6000) - 3000); sp[2 * i] = sp[2 * i + 1] = x; }
            made = true;
        }
        PlaySoundW((LPCWSTR)wav, nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    } else PlaySoundW(nullptr, nullptr, 0);
}

// One recording of the fixture: `noise` scrolls a noisy picture over it after the first 1.5 s (the first seconds are the known colours, for the orientation).
// Returns the size of the file, 0 when it was not made; the checks of this one recording are made here.
static unsigned long long QsRecord(int vc, int vq, int ac, int aq, bool audio, bool noise, int fps, double secs, const wchar_t* tag, int* audioType, bool small = false)
{
    RecParams p; memset(&p, 0, sizeof(p));
    SettingsDefaults(p.cfg);
    p.cfg.videoCodec = vc; p.cfg.videoQuality = vq; p.cfg.audioCodec = ac; p.cfg.audioQuality = aq; p.cfg.fps = fps;
    p.cfg.mic = false; p.cfg.loopback = audio; p.cfg.capMouse = false; p.cfg.highlight = false;
    p.rc = FxScreenRect();
    int regW = g_fx.w, regH = g_fx.h;
    if (small) { regW = 128; regH = 96; p.rc.right = p.rc.left + regW; p.rc.bottom = p.rc.top + regH; }          // the sound part: a tiny picture, the sound is almost all of the file
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\qs-%d-%d-%d-%d.%s", g_dir, vc, vq, ac, aq, VideoFormatExt(vc));
    DeleteFileW(path); wcscpy(p.path, path);
    wchar_t desc[260]; _snwprintf(desc, 260, L"%s: video codec %d quality %d, audio codec %d quality %d, audio %d, %d fps", tag, vc, vq, ac, aq, (int)audio, fps);
    Recorder rec; wchar_t err[300] = L"";
    if (!rec.Start(p, err, 300)) { FAIL(L"quality sizes: Start failed (%s): %s", desc, err); return 0; }
    int ew, eh; FitSizeForVideo(vc, regW, regH, ew, eh);
    // the noise: a bitmap twice as wide as the fixture, scrolled
    HDC wdc = GetDC(g_fx.hw), mdc = CreateCompatibleDC(wdc);
    HBITMAP nb = CreateCompatibleBitmap(wdc, g_fx.w * 2, g_fx.h);
    HGDIOBJ old = SelectObject(mdc, nb);
    { const size_t cnt = (size_t)g_fx.w * 2 * g_fx.h; DWORD* px = (DWORD*)malloc(cnt * 4); unsigned sd = 777;
      int amp = 32; { wchar_t e[16]; if (GetEnvironmentVariableW(L"C2S_QS_AMP", e, 16)) amp = _wtoi(e); if (amp < 0) amp = 0; if (amp > 127) amp = 127; }      // the noise on a smooth picture: how hard the content is
      const int W2 = g_fx.w * 2;
      for (size_t i = 0; px && i < cnt; ++i) {
          int x = (int)(i % W2), y = (int)(i / W2);
          int ch[3] = { x * 255 / W2, y * 255 / g_fx.h, 128 };
          DWORD c = 0;
          for (int k = 0; k < 3; ++k) { sd = sd * 1664525u + 1013904223u; int v = ch[k] + (int)((sd >> 16) % (2 * amp + 1)) - amp; v = v < 0 ? 0 : v > 255 ? 255 : v; c = (c << 8) | (DWORD)v; }
          px[i] = c;
      }
      BITMAPINFO bi; memset(&bi, 0, sizeof(bi)); bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = g_fx.w * 2; bi.bmiHeader.biHeight = -g_fx.h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
      SetDIBitsToDevice(mdc, 0, 0, g_fx.w * 2, g_fx.h, 0, 0, 0, g_fx.h, px, &bi, DIB_RGB_COLORS); free(px); }
    DWORD t0 = GetTickCount(); int step = 0;
    while ((int)(GetTickCount() - t0) < (int)(secs * 1000) && !rec.Failed()) {
        Pump(40);
        if (noise && (int)(GetTickCount() - t0) > 1500) { int off = (step++ * 13) % g_fx.w; BitBlt(wdc, 0, 0, g_fx.w, g_fx.h, mdc, off, 0, SRCCOPY); }
    }
    SelectObject(mdc, old); DeleteObject(nb); DeleteDC(mdc); ReleaseDC(g_fx.hw, wdc);
    double el = rec.ElapsedSec();
    bool stopped = rec.Stop();
    InvalidateRect(g_fx.hw, nullptr, TRUE); Pump(100);                       // the known colours again
    Check(stopped && !rec.Failed(), L"quality sizes: Stop finalizes the file", desc);
    unsigned long long sz = FileSz(path);
    Check(sz > 1500, L"quality sizes: the file is not empty", desc);
    if (vc == VC_AVI) {
        VerifyAvi(path, ew, eh, el, fps, audio, !small, desc);
        return sz;
    }
    bool pictureOk = false, decoderMissing = false; int pictureChecked = 0;
    VideoInfo v = Inspect(path, !small, &pictureOk, &pictureChecked, &decoderMissing);
    wchar_t d3[320]; _snwprintf(d3, 320, L"%s: file %dx%d, %.2f s, %d frames, %llu bytes", desc, v.w, v.h, v.dur, v.frames, sz);
    bool codecOk = vc == VC_H265 ? v.hevc : vc == VC_WMV ? v.wmv : v.h264;
    Check(v.ok && codecOk && v.w == ew && v.h == eh, L"quality sizes: the video has the chosen codec at the expected size", d3);
    Check(fabs(v.dur - el) < (vc == VC_WMV ? 1.6 : 0.9), L"quality sizes: the duration matches the recorded time", d3);
    if (decoderMissing) WARN(L"quality sizes: no decoder here - frames and picture not checked");
    else {
        Check(v.frames > el * fps * 0.25 && v.frames < el * fps * 1.3 + 4, L"quality sizes: the number of frames is plausible", d3);
        if (!small) Check(pictureOk, L"quality sizes: the picture is upright and has the colours of the screen", d3);
    }
    if (audio) {
        Check(v.audio, L"quality sizes: the file has the sound track", d3);
        if (vc != VC_WMV && audioType) {
            int t = Mp4AudioObjectType(path); *audioType = t;
            bool right = ac ? (t == 0x6B || t == 0x69) : t == 0x40;
            wchar_t d4[320]; _snwprintf(d4, 320, L"%s: audio object type 0x%02X", desc, t);
            Check(right, L"quality sizes: the audio codec in the file is the audio codec chosen", d4);
        }
    }
    return sz;
}

static void T_QualitySizes()
{
    if (g_wine && !VideoFormatAvailable(VC_AVI)) { SKIP(L"quality sizes: nothing to record with here"); return; }
    int avail[VC_COUNT], na = 0;
    for (int i = 0; i < VC_COUNT; ++i) if (i != VC_GIF && VideoFormatAvailable(i) && (!g_wine || i == VC_AVI)) avail[na++] = i;
    if (!na) { SKIP(L"quality sizes: no video format here"); return; }
    int vc = avail[R(0, na - 1)], ac = R(0, 1);
    const int fps = R(0, 1) ? 15 : 25;
    { wchar_t e[16];                                                   // C2S_QS_VC / C2S_QS_AC: one codec of the matrix instead of a random one
      if (GetEnvironmentVariableW(L"C2S_QS_VC", e, 16)) { int w = _wtoi(e); for (int i = 0; i < na; ++i) if (avail[i] == w) vc = w; }
      if (GetEnvironmentVariableW(L"C2S_QS_AC", e, 16)) ac = _wtoi(e) ? 1 : 0; }
    MakeFixture(R(420, 640), R(260, 380));
    wchar_t head[200]; _snwprintf(head, 200, L"video codec %d, audio codec %d (%s), %d fps", vc, ac, ac ? L"MP3" : L"AAC", fps);
    INFO(L"quality sizes: %s", head);
    bool dev = false;
    if (!g_wine) { SpeakerCapture sc; dev = sc.Open(); }
    const bool snd = !g_wine;                                           // the speakers are asked for always; with no playback device the track is silence (the encoder still has its bit rate)
    if (!dev) INFO(L"quality sizes: no playback device here - the sound track is silence");
    // The sound first, on a still picture: what the sound adds to the file is the file with it less the same file without it. The three audio
    // qualities must be 20 % apart in that; the sound of the medium quality is then taken off the files of the video part, so that a constant
    // sound track (about 100 KB for 7 s) does not hide the steps of the video quality.
    unsigned long long audioAdd[3] = {};
    if (snd) {
        const unsigned long long noSnd = QsRecord(vc, 1, ac, 1, false, false, fps, 7.0, L"no sound", nullptr, true);
        if (dev) QsTone(true);
        for (int q = 0; q < 3; ++q) {
            if (vc == VC_AVI && q != 1) continue;                          // the AVI holds the sound uncompressed: no audio quality there
            unsigned long long t = QsRecord(vc, 1, ac, q, snd, false, fps, 7.0, L"audio quality", nullptr, true);
            audioAdd[q] = t > noSnd ? t - noSnd : 0;
        }
        if (dev) QsTone(false);
        wchar_t da[300]; _snwprintf(da, 300, L"%s: the sound adds %llu (best), %llu (medium), %llu (worst) bytes to %llu", head, audioAdd[0], audioAdd[1], audioAdd[2], noSnd);
        if (vc == VC_AVI) SKIP(L"quality sizes: the AVI holds the sound uncompressed - no audio quality there");
        else if (audioAdd[0] && audioAdd[1] && audioAdd[2]) {
            Check(audioAdd[0] > audioAdd[1] * 1.20, L"quality sizes: the best audio quality adds at least 20 % more than the medium one", da);
            Check(audioAdd[1] > audioAdd[2] * 1.20, L"quality sizes: the medium audio quality adds at least 20 % more than the worst one", da);
        } else FAIL(L"quality sizes: the sound added nothing to the file: %s", da);
    }
    // the video quality: a moving noisy picture, the audio quality fixed; the sound of the medium quality is not counted
    unsigned long long vs[3] = {};
    if (dev) QsTone(true);
    for (int q = 0; q < 3; ++q) { unsigned long long t = QsRecord(vc, q, ac, 1, snd, true, fps, 7.0, L"video quality", nullptr); vs[q] = t > audioAdd[1] ? t - audioAdd[1] : t; }
    if (dev) QsTone(false);
    wchar_t d[300]; _snwprintf(d, 300, L"%s: best %llu, medium %llu, worst %llu bytes (the sound taken off)", head, vs[0], vs[1], vs[2]);
    if (vs[0] && vs[1] && vs[2] && g_wine) {
        if (!(vs[0] > vs[1] * 1.20 && vs[1] > vs[2] * 1.20)) WARN(L"quality sizes: Wine's JPEG encoder does not follow the quality (the check is made on Windows): %s", d);
    } else if (vs[0] && vs[1] && vs[2]) {
        Check(vs[0] > vs[1] * 1.20, L"quality sizes: the best video quality is a file at least 20 % bigger than the medium one", d);
        Check(vs[1] > vs[2] * 1.20, L"quality sizes: the medium video quality is a file at least 20 % bigger than the worst one", d);
    }
}

static void T_KeyInterval()
{
    if (g_wine || !VideoFormatAvailable(VC_H264_BEST)) { SKIP(L"key frames: there is no real H.264 encoder here"); return; }
    MakeFixture(R(300, 600), R(220, 400));
    RecParams p; memset(&p, 0, sizeof(p));
    SettingsDefaults(p.cfg); p.cfg.mic = false; p.cfg.loopback = false;
    p.cfg.videoCodec = VC_H264_BEST;
    { static const int f[] = { 10, 15, 25 }; p.cfg.fps = f[R(0, 2)]; }          // the interval is a time (20 s), whatever the frame rate
    p.rc = FxScreenRect();
    wcscpy(p.path, g_dir); wcscat(p.path, L"\\keys.mp4");
    Recorder rec; wchar_t err[200];
    auto cpuNow = []() -> double {                                     // CPU time of this process (kernel + user), seconds
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
        return (double)(((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e7;
    };
    if (!rec.Start(p, err, 200)) { FAIL(L"key frames: Start failed: %s", err); return; }
    DWORD t0 = GetTickCount(); double cpu0 = cpuNow();
    while ((int)(GetTickCount() - t0) < 24000) Pump(200);
    double cpuUsed = cpuNow() - cpu0, wall = (GetTickCount() - t0) / 1000.0;
    double el = rec.ElapsedSec();
    bool stopped = rec.Stop();
    int total = 0, keys = Mp4KeyFrames(p.path, &total);
    wchar_t d[200]; _snwprintf(d, 200, L"%d key frames of %d samples in %.1f s", keys, total, el);
    Check(stopped && keys >= 1, L"key frames: the file has a sync-sample table", d);
    if (keys > 3 && keys <= (int)(el / 1.5)) Rep(L"WARN", L"key frames: this Windows' H.264 encoder puts its own key frames in about every %.1f s although 20 s were asked for: %s", el / keys, d);
    else Check(keys >= 1 && keys <= 3, L"key frames: one every 20 s, not one every few seconds", d);
    // a still screen at 10 frames a second is cheap: the recording must not eat a big part of a core (the complaint was CPU bursts)
    wchar_t dc[160]; _snwprintf(dc, 160, L"%.1f s of CPU in %.1f s: %.0f %% of one core (window %dx%d at %d fps)", cpuUsed, wall, 100.0 * cpuUsed / wall, (int)(p.rc.right - p.rc.left), (int)(p.rc.bottom - p.rc.top), p.cfg.fps);
    Check(wall > 5 && cpuUsed / wall < 0.8, L"key frames: recording a still screen does not load the CPU heavily", dc);
}

// A flood of hotkeys and commands: the program takes what it can, ignores the rest and stays up.
static void T_AppFlood()
{
    MakeFixture(R(300, 500), R(220, 360));
    Settings s; PrepareApp(s);
    App a;
    if (!StartApp(a, s)) { FAIL(L"flood: the program did not start"); StopApp(a, false); return; }
    int n = R(15, 40);
    for (int i = 0; i < n; ++i) {
        switch (R(0, 3)) {
            case 0: PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0); break;
            case 1: PostMessageW(a.main, WM_COMMAND, IDC_SHOT_DESKTOP, 0); break;
            case 2: PostMessageW(a.main, WM_HOTKEY, HK_STOP, 0); break;
            default: PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0); break;
        }
        if (Chance(30)) Pump(R(1, 30));
    }
    // answer whatever opened (options windows, boxes) until things are quiet
    for (int round = 0; round < 40 * (g_scale > 1 ? 3 : 1); ++round) {       // a slow machine builds every options window in seconds, and the burst holds a dozen of them
        Pump(300);
        HWND o = FindWin(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 0);
        if (o) PostMessageW(o, WM_COMMAND, IDCANCEL, 0);
        HWND box = FindWin(a.pi.dwProcessId, L"#32770", 0);
        if (box) PostMessageW(box, WM_COMMAND, IDOK, 0);
    }
    DWORD_PTR res = 0;
    bool answers = SendMessageTimeoutW(a.main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, WMS(g_scale > 1 ? 30000 : 10000), &res) != 0;
    int files = CountFiles(s.folder);
    wchar_t d[100]; _snwprintf(d, 100, L"%d messages sent, %d pictures saved", n, files);
    Check(AppAlive(a) && answers, L"flood: the program is alive and answering after a burst of hotkeys and commands", d);
    PostMessageW(a.main, WM_COMMAND, ID_TRAY_EXIT, 0);
    DWORD code = 99;
    bool ended = WaitForSingleObject(a.pi.hProcess, WMS(15000)) == WAIT_OBJECT_0 && GetExitCodeProcess(a.pi.hProcess, &code) && code == 0;
    Check(ended, L"flood: and it still exits with code 0");
    StopApp(a, false);
}

// The installer finds a running program, closes it and installs over it.
static void T_InstallWhileRunning()
{
    if (!g_installerAllowed) { SKIP(L"install while running: needs --installer"); return; }
    wchar_t pf[MAX_PATH] = L""; PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) { wcsncpy(pf, p, MAX_PATH - 1); CoTaskMemFree(p); }
    wchar_t exe[MAX_PATH]; _snwprintf(exe, MAX_PATH, L"%s\\Click2Screen\\Click2Screen.exe", pf);
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) { SKIP(L"install while running: Click2Screen is already installed here; not touching it"); return; }
    Settings s; PrepareApp(s);
    App a;
    if (!StartApp(a, s)) { FAIL(L"install while running: the program did not start"); StopApp(a, false); return; }
    if (Chance(50)) {                                                 // also in the middle of a recording
        PostMessageW(a.main, WM_COMMAND, IDC_REC_DESKTOP, 0);
        HWND panel = FindReady(a.pi.dwProcessId, C2S_CLASS_PANEL, 10000);
        if (panel) { PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0); LangSelect(s.language); WaitText(panel, IDC_P_START, T(S_PAUSE), (s.delaySec + 6) * 1000); }
    }
    DWORD code = 99;
    bool ran = RunSetup(L"/install /silent", &code);
    Check(ran && code == 0, L"install while running: the installer returns 0 with the program running");
    bool gone = WaitForSingleObject(a.pi.hProcess, WMS(10000)) == WAIT_OBJECT_0;
    Check(gone, L"install while running: the running program was closed by the installer");
    Check(GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES, L"install while running: the program is installed");
    StopApp(a, false);
    code = 99; RunSetup(L"/remove /silent", &code);
    Sleep(3000);
}


// ---- saving a picture to a file somebody else holds ---------------------------------------------------------------
struct LockJob { wchar_t path[MAX_PATH]; int holdMs; HANDLE ready; };
static DWORD WINAPI HoldFile(LPVOID p)
{
    LockJob* j = (LockJob*)p;
    HANDLE h = CreateFileW(j->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);      // exclusive: nobody else may open it
    SetEvent(j->ready);
    Sleep(j->holdMs);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}

static void T_SaveLocked()
{
    MakeFixture(R(200, 400), R(160, 300));
    RECT fr = FxScreenRect();
    HBITMAP bmp = CaptureRect(fr, false, false);
    if (!bmp) { FAIL(L"save to a held file: no picture"); return; }
    for (int pass = 0; pass < 2; ++pass) {
        bool brief = pass == 0;
        LockJob job; memset(&job, 0, sizeof(job));
        _snwprintf(job.path, MAX_PATH, L"%s\\held%d.png", g_dir, R(0, 99999));
        job.holdMs = brief ? R(50, 300) : 3000;                  // released within the retries / not at all
        job.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE th = CreateThread(nullptr, 0, HoldFile, &job, 0, nullptr);
        WaitForSingleObject(job.ready, 5000);
        DWORD t0 = GetTickCount();
        bool ok = SaveBitmap(bmp, job.path, IMG_PNG);
        DWORD took = GetTickCount() - t0;
        WaitForSingleObject(th, 10000); CloseHandle(th); CloseHandle(job.ready);
        wchar_t d[140]; _snwprintf(d, 140, L"held %d ms, SaveBitmap returned %d after %lu ms, file %llu bytes", job.holdMs, (int)ok, (unsigned long)took, FileSz(job.path));
        if (brief) {
            int dw = 0, dh = 0; uint8_t* px = nullptr;
            bool good = ok && DecodePixels(job.path, &dw, &dh, &px);
            Check(good, L"save to a held file: a file held for a moment is saved after the retries", d);
            delete[] px;
        } else {
            Check(!ok && took >= 300 && FileSz(job.path) == 0, L"save to a held file: a file that stays held makes the save fail (after the retries), without a partial picture", d);
        }
    }
    DeleteObject(bmp);
}

// ==== main =====================================================================================================
struct TestDef { const wchar_t* name; void (*fn)(); int weight; };


// ---- the encoder laboratory (c2sautotest.exe 25 --keylab) ---------------------------------------------------------------------------
// Round 1 (Windows 11 VM): the H.264 encoder took GOP sizes of 30 / 200 / 1000 and put a key frame every second anyway; the WMV encoder gave
// an upside-down picture for a bottom-up RGB32 input (the stride must be told) and ignored every quality setting of the encoder config.
// Round 2: more settings (the media type attribute for the key frame spacing, content, bit rate, size), the processor time of the encoding alone,
// and a real recording of the recorder to see the fix of the WMV orientation.
namespace {
const GUID kLabGop        = { 0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };
const GUID kLabRate       = { 0x1c0608e9, 0x370c, 0x4710, { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 } };
const GUID kLabQvS        = { 0x98332df8, 0x03cd, 0x476b, { 0x89, 0xfa, 0x3f, 0x9e, 0x44, 0x2d, 0xec, 0x9f } };
const GUID kLabQuality    = { 0xfcbf57a3, 0x7ea5, 0x4b0c, { 0x96, 0x44, 0x69, 0xb4, 0x0c, 0x39, 0xc3, 0x91 } };
const GUID kLabMaxSpacing = { 0xc16eb52b, 0x73a1, 0x476f, { 0x8d, 0x62, 0x83, 0x9d, 0x6a, 0x02, 0x06, 0x52 } };   // MF_MT_MAX_KEYFRAME_SPACING
const GUID kLabContentType = { 0x66117aca, 0xeb77, 0x459d, { 0x93, 0x0c, 0xa4, 0x8d, 0x9d, 0x06, 0x83, 0xfc } };
const GUID kLabRealTime    = { 0x143a0ff6, 0xa131, 0x43da, { 0xb8, 0x1e, 0x98, 0xfb, 0xb8, 0xec, 0x37, 0x8e } };
const GUID kLabCommonLow   = { 0x9d3ecd55, 0x89e8, 0x490a, { 0x97, 0x0a, 0x0c, 0x95, 0x48, 0xd5, 0xa5, 0x6e } };
const GUID kLabTemporal    = { 0x19caebff, 0xb74d, 0x4cfd, { 0x8c, 0x27, 0xc2, 0xf9, 0xd9, 0x7d, 0x5f, 0x52 } };
const GUID kLabMaxRef      = { 0x964829ed, 0x94f9, 0x43b4, { 0xb7, 0x4d, 0xef, 0x40, 0x94, 0x4b, 0x69, 0xa0 } };
const GUID kLabIidApi     = { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
const GUID kLabIidSwEx    = { 0x588d72ab, 0x5bc1, 0x496a, { 0x87, 0x14, 0xb7, 0x06, 0x17, 0x14, 0x1b, 0x25 } };

static bool LabKeys(const wchar_t* path, int* keys, int cap, int* nKeys, int* total)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 400000000LL) { CloseHandle(h); return false; }
    unsigned char* b = (unsigned char*)malloc((size_t)sz.QuadPart);
    DWORD rd = 0; bool ok = b && ReadFile(h, b, (DWORD)sz.QuadPart, &rd, nullptr) && rd == (DWORD)sz.QuadPart;
    CloseHandle(h);
    *nKeys = -1; *total = -1;
    auto be = [&](DWORD o) { return (int)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]); };
    for (DWORD i = 4; ok && i + 16 < rd; ++i) {
        if (!memcmp(b + i, "stss", 4) && *nKeys < 0) {
            *nKeys = be(i + 8);
            for (int k = 0; k < *nKeys && k < cap && i + 12 + 4 * (k + 1) <= rd; ++k) keys[k] = be(i + 12 + 4 * k);
        }
        if (!memcmp(b + i, "stsz", 4) && *total < 0) *total = be(i + 12);
    }
    free(b);
    return *nKeys >= 0;
}

static double LabCpuSeconds()
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return (double)((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
}

// One sink writer: H.264 or WMV, 10 fps unless told, a picture that is red above and blue below with a white square.
struct LabEx { const GUID* g; int vt; int val; const wchar_t* name; };          // vt: 1 unsigned, 2 boolean
struct LabCfg {
    const wchar_t* name;
    bool wmv;
    int w, h, fps;
    int bitrate;        // bits per second
    int jump;           // the square jumps every `jump` frames (0: the picture never changes after the first frame)
    int gopAttr;        // AVEncMPVGOPSize through the encoder config attributes (0: not set)
    int gopApi;         // ... through ICodecAPI
    int spacing;        // MF_MT_MAX_KEYFRAME_SPACING on the output type (0: not set)
    int qvsAttr;        // AVEncCommonQualityVsSpeed through the encoder config (-1: not set)
    int qvsApi;         // ... through ICodecAPI (-1: not set)
    int rcApi;          // rate control through ICodecAPI: 0 not set, 1 unconstrained VBR, 3 CBR
    int stride;         // MF_MT_DEFAULT_STRIDE of the input: 0 not set, -1 bottom-up
    bool topDown;       // the pixels are given top-down
    int frames;
    const struct LabEx* ex;   // more properties through ICodecAPI (round 3)
    int nex;
    bool ranges;        // write the range the encoder gives for the key frame interval and the quality
    int declFps;        // the frame rate told to the encoder in the media types (0: fps)
    int ptsFps;         // the rate of the time stamps of the samples (0: fps)
    int renego;         // after the values: 1 the same input type again through the sink writer, 2 the types set again on the encoder itself (IMFSinkWriterEx), 3 a changed input type, 4 flush + types on the encoder
};

static void LabOne(const LabCfg& c, int idx, bool keep)
{
    wchar_t pd[MAX_PATH] = L"", path[MAX_PATH + 40];
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    _snwprintf(path, MAX_PATH + 40, L"%s\\Click2Screen\\logs\\lab-%d.%s", pd, idx, c.wmv ? L"wmv" : L"mp4");
    DeleteFileW(path);
    const int W = c.w, H = c.h;
    IMFAttributes* attrs = nullptr; IMFAttributes* enc = nullptr; IMFSinkWriter* w = nullptr;
    MFCreateAttributes(&attrs, 4); MFCreateAttributes(&enc, 4);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    bool anyEnc = false;
    if (c.qvsAttr >= 0) { enc->SetUINT32(kLabQvS, c.qvsAttr); anyEnc = true; }
    if (c.gopAttr) { enc->SetUINT32(kLabGop, (UINT32)c.gopAttr); anyEnc = true; }
    if (anyEnc) attrs->SetUnknown(MF_SINK_WRITER_ENCODER_CONFIG, enc);
    HRESULT hr = MFCreateSinkWriterFromURL(path, nullptr, attrs, &w);
    if (FAILED(hr) || !w) { Rep(L"INFO", L"lab %d [%s]: no sink writer 0x%08lX", idx, c.name, (unsigned long)hr); enc->Release(); attrs->Release(); return; }
    IMFMediaType* vo = nullptr; IMFMediaType* vi = nullptr;
    MFCreateMediaType(&vo); MFCreateMediaType(&vi);
    vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); vo->SetGUID(MF_MT_SUBTYPE, c.wmv ? MFVideoFormat_WMV3 : MFVideoFormat_H264);
    vo->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)c.bitrate); vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vo, MF_MT_FRAME_SIZE, W, H); MFSetAttributeRatio(vo, MF_MT_FRAME_RATE, c.declFps ? c.declFps : c.fps, 1); MFSetAttributeRatio(vo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (!c.wmv) vo->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    if (c.spacing) vo->SetUINT32(kLabMaxSpacing, (UINT32)c.spacing);
    vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); vi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vi, MF_MT_FRAME_SIZE, W, H); MFSetAttributeRatio(vi, MF_MT_FRAME_RATE, c.declFps ? c.declFps : c.fps, 1); MFSetAttributeRatio(vi, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (c.stride) vi->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(LONG)(c.stride * W * 4));
    DWORD st = 0;
    hr = w->AddStream(vo, &st);
    if (SUCCEEDED(hr)) hr = w->SetInputMediaType(st, vi, nullptr);
    wchar_t info[600] = L"";
    auto cat = [&](const wchar_t* fmt, ...) { va_list ap; va_start(ap, fmt); size_t n = wcslen(info); _vsnwprintf(info + n, 600 - n - 1, fmt, ap); va_end(ap); };
    if (SUCCEEDED(hr)) {
        ICodecAPI* api = nullptr;
        if (SUCCEEDED(w->GetServiceForStream(st, GUID_NULL, kLabIidApi, (void**)&api)) && api) {
            VARIANT v; VariantInit(&v);
            auto setU = [&](const GUID& g, unsigned val, const wchar_t* nm) { v.vt = VT_UI4; v.ulVal = val; HRESULT r = api->SetValue(&g, &v); if (FAILED(r)) cat(L" %s:0x%08lX", nm, (unsigned long)r); };
            if (c.gopApi) setU(kLabGop, (unsigned)c.gopApi, L"GOP");
            if (c.qvsApi >= 0) setU(kLabQvS, (unsigned)c.qvsApi, L"QvS");
            if (c.rcApi) setU(kLabRate, c.rcApi == 1 ? eAVEncCommonRateControlMode_UnconstrainedVBR : eAVEncCommonRateControlMode_CBR, L"rate");
            for (int e = 0; e < c.nex; ++e) {
                const LabEx& x = c.ex[e];
                HRESULT sup = api->IsSupported(x.g);
                v.vt = x.vt == 2 ? VT_BOOL : VT_UI4; if (x.vt == 2) v.boolVal = x.val ? VARIANT_TRUE : VARIANT_FALSE; else v.ulVal = (ULONG)x.val;
                HRESULT r = api->SetValue(x.g, &v);
                cat(L" %s=%d(%s)", x.name, x.val, r == S_OK ? L"ok" : sup == S_OK ? L"refused" : L"unsupported");
            }
            if (c.ranges) {
                struct { const GUID* g; const wchar_t* n; } rg[] = { { &kLabGop, L"GOP" }, { &kLabQvS, L"QvS" } };
                for (auto& q : rg) {
                    VARIANT lo, hi, stp; VariantInit(&lo); VariantInit(&hi); VariantInit(&stp);
                    HRESULT r = api->GetParameterRange(q.g, &lo, &hi, &stp);
                    if (SUCCEEDED(r)) cat(L" range[%s]=%lu..%lu step %lu", q.n, (unsigned long)lo.ulVal, (unsigned long)hi.ulVal, (unsigned long)stp.ulVal); else cat(L" range[%s]:0x%08lX", q.n, (unsigned long)r);
                }
            }
            if (c.renego == 1) cat(L" renego1:0x%08lX", (unsigned long)w->SetInputMediaType(st, vi, nullptr));
            if (c.renego == 3) { vi->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255); cat(L" renego3:0x%08lX", (unsigned long)w->SetInputMediaType(st, vi, nullptr)); }
            if (c.renego == 2 || c.renego == 4) {
                IMFSinkWriterEx* wx = nullptr;
                HRESULT rx = w->QueryInterface(__uuidof(IMFSinkWriterEx), (void**)&wx);
                cat(L" ex:0x%08lX", (unsigned long)rx);
                for (DWORD ti2 = 0; SUCCEEDED(rx) && ti2 < 6; ++ti2) {
                    IMFTransform* t = nullptr; GUID cg = GUID_NULL;
                    if (FAILED(wx->GetTransformForStream(st, ti2, &cg, &t)) || !t) break;
                    const bool isEnc = IsEqualGUID(cg, MFT_CATEGORY_VIDEO_ENCODER) != 0;
                    cat(L" [t%lu %s", (unsigned long)ti2, isEnc ? L"encoder" : cg.Data1 == MFT_CATEGORY_VIDEO_PROCESSOR.Data1 ? L"processor" : L"other");
                    if (isEnc) {
                        IMFMediaType* to = nullptr; IMFMediaType* ti = nullptr;
                        HRESULT r1 = t->GetOutputCurrentType(0, &to), r2 = t->GetInputCurrentType(0, &ti);
                        if (c.renego == 4) t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                        HRESULT r3 = to ? t->SetOutputType(0, to, 0) : r1, r4 = ti ? t->SetInputType(0, ti, 0) : r2;
                        cat(L" cur:0x%08lX/0x%08lX set:0x%08lX/0x%08lX", (unsigned long)r1, (unsigned long)r2, (unsigned long)r3, (unsigned long)r4);
                        if (to) to->Release(); if (ti) ti->Release();
                    }
                    cat(L"]");
                    t->Release();
                }
                if (wx) wx->Release();
            }
            VARIANT g; VariantInit(&g);
            if (api->IsSupported(&kLabQvS) == S_OK && SUCCEEDED(api->GetValue(&kLabQvS, &g))) cat(L" QvS now %lu", (unsigned long)g.ulVal);
            api->Release();
        }
    }
    if (SUCCEEDED(hr)) hr = w->BeginWriting();
    // two pictures are made once: the encoding alone is what is timed
    const size_t bytes = (size_t)W * H * 4;
    BYTE* pic[2] = { (BYTE*)malloc(bytes), (BYTE*)malloc(bytes) };
    for (int k = 0; k < 2; ++k)
        for (int y = 0; y < H; ++y) {
            int row = c.topDown ? y : H - 1 - y;
            DWORD col = y < H / 2 ? 0x00FF0000 : 0x000000FF;
            DWORD* line = (DWORD*)(pic[k] + (size_t)row * W * 4);
            for (int x = 0; x < W; ++x) line[x] = col;
            if (y >= H / 2 - 50 && y < H / 2 + 50) { int bx = k ? W / 2 : W / 4; for (int x = bx; x < bx + 100; ++x) line[x] = 0x00FFFFFF; }
        }
    double cpu0 = LabCpuSeconds(); DWORD t0 = GetTickCount();
    int done = 0;
    if (SUCCEEDED(hr)) {
        for (int i = 0; i < c.frames; ++i) {
            const BYTE* src = pic[c.jump ? (i / c.jump) % 2 : 0];
            IMFMediaBuffer* b = nullptr; IMFSample* sm = nullptr; BYTE* d = nullptr;
            if (FAILED(MFCreateMemoryBuffer((DWORD)bytes, &b)) || FAILED(b->Lock(&d, nullptr, nullptr))) { hr = E_FAIL; if (b) b->Release(); break; }
            memcpy(d, src, bytes); b->Unlock(); b->SetCurrentLength((DWORD)bytes);
            MFCreateSample(&sm); sm->AddBuffer(b);
            const int pf = c.ptsFps ? c.ptsFps : c.fps;
            sm->SetSampleTime((LONGLONG)i * 10000000LL / pf); sm->SetSampleDuration(10000000LL / pf);
            hr = w->WriteSample(st, sm); sm->Release(); b->Release();
            if (FAILED(hr)) break;
            ++done;
        }
    }
    HRESULT hf = w->Finalize();
    DWORD wall = GetTickCount() - t0; double cpu = LabCpuSeconds() - cpu0;
    free(pic[0]); free(pic[1]);
    w->Release(); vo->Release(); vi->Release(); enc->Release(); attrs->Release();
    LARGE_INTEGER fsz; fsz.QuadPart = 0;
    HANDLE hf2 = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf2 != INVALID_HANDLE_VALUE) { GetFileSizeEx(hf2, &fsz); CloseHandle(hf2); }
    wchar_t keysText[300] = L"";
    if (!c.wmv) {
        int keys[64] = {}, nKeys = 0, total = 0;
        if (LabKeys(path, keys, 64, &nKeys, &total)) {
            size_t ln = _snwprintf(keysText, 300, L", %d key frames of %d: ", nKeys, total);
            for (int k = 0; k < nKeys && k < 10; ++k) ln += _snwprintf(keysText + ln, 300 - ln - 1, L"%d ", keys[k]);
        }
    }
    Rep(L"INFO", L"lab %d [%s] %dx%d %d fps %d kbit/s: %d frames (0x%08lX, 0x%08lX)%s; %.2f s of CPU for %lu ms of wall = %.0f %% of a core, %.1f ms of CPU per frame; %lld bytes%s",
        idx, c.name, W, H, c.fps, c.bitrate / 1000, done, (unsigned long)hr, (unsigned long)hf, info, cpu, (unsigned long)wall, wall ? 100.0 * cpu / (wall / 1000.0) : 0.0, done ? 1000.0 * cpu / done : 0.0, (long long)fsz.QuadPart, keysText);
    if (!keep || c.wmv == false) { if (!keep) DeleteFileW(path); }
}

// The recorder itself, WMV: a recording of the fixture (the four quarters), kept for the other decoder.
static void LabRecorderWmv()
{
    wchar_t pd[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    RecParams p; memset(&p, 0, sizeof(p));
    SettingsDefaults(p.cfg); p.cfg.mic = false; p.cfg.loopback = false; p.cfg.videoCodec = VC_WMV; p.cfg.fps = 15; p.cfg.capMouse = false;
    MakeFixture(640, 360);
    p.rc = FxScreenRect();
    _snwprintf(p.path, MAX_PATH, L"%s\\Click2Screen\\logs\\lab-recorder.wmv", pd);
    DeleteFileW(p.path);
    Recorder rec; wchar_t err[200];
    if (!rec.Start(p, err, 200)) { Rep(L"INFO", L"lab recorder WMV: Start failed: %s", err); return; }
    double cpu0 = LabCpuSeconds(); DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < 6000) Pump(100);
    double cpu = LabCpuSeconds() - cpu0; double wall = (GetTickCount() - t0) / 1000.0;
    bool ok = rec.Stop();
    Rep(L"INFO", L"lab recorder WMV (the real Recorder, 640x360, 15 fps, 6 s): stopped %d, %.2f s of CPU in %.1f s = %.0f %% of a core; the file is lab-recorder.wmv (the fixture: red top left, blue top right? see kQuad)", (int)ok, cpu, wall, 100.0 * cpu / wall);
    if (g_fx.hw) { DestroyWindow(g_fx.hw); g_fx.hw = nullptr; }
}
} // namespace


// ---- round 5: the H.264 encoder MFT used directly (no sink writer, no MP4): where are its IDR frames? ------------------------------------
namespace {
static const GUID kClsidH264Enc = { 0x6ca50344, 0x051a, 0x4ded, { 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35 } };    // CLSID_CMSH264EncoderMFT
static const GUID kIidTransformLab = { 0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d } };

struct DirectCfg { const wchar_t* name; int gop; int when; /* 0 never set, 1 before the types, 2 after the types, 3 after BEGIN_STREAMING */ int w, h, fps; bool mp4; const wchar_t* file; };

static void DirectOne(const DirectCfg& c)
{
    IMFTransform* mft = nullptr;
    HRESULT hr = CoCreateInstance(kClsidH264Enc, nullptr, CLSCTX_INPROC_SERVER, kIidTransformLab, (void**)&mft);
    if (FAILED(hr) || !mft) { Rep(L"INFO", L"direct [%s]: the encoder MFT cannot be made 0x%08lX", c.name, (unsigned long)hr); return; }
    ICodecAPI* api = nullptr; mft->QueryInterface(kLabIidApi, (void**)&api);
    auto setGop = [&]() -> HRESULT { if (!api || !c.gop) return S_OK; VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = (ULONG)c.gop; return api->SetValue(&kLabGop, &v); };
    HRESULT hrGop = S_OK;
    if (c.when == 1) hrGop = setGop();
    IMFMediaType* out = nullptr; IMFMediaType* in = nullptr;
    MFCreateMediaType(&out); MFCreateMediaType(&in);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, 2400000); out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, c.w, c.h); MFSetAttributeRatio(out, MF_MT_FRAME_RATE, c.fps, 1); MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12); in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, c.w, c.h); MFSetAttributeRatio(in, MF_MT_FRAME_RATE, c.fps, 1); MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hrO = mft->SetOutputType(0, out, 0), hrI = mft->SetInputType(0, in, 0);
    if (c.when == 2) hrGop = setGop();
    if (c.when == 4) { hrGop = setGop(); hrO = mft->SetOutputType(0, out, 0); hrI = mft->SetInputType(0, in, 0); }           // set, then negotiate the types again
    if (c.when == 5) { hrGop = setGop(); mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); hrO = mft->SetOutputType(0, out, 0); hrI = mft->SetInputType(0, in, 0); }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (c.when == 3) hrGop = setGop();
    VARIANT gv; VariantInit(&gv); unsigned gopNow = 0;
    if (api && api->IsSupported(&kLabGop) == S_OK && SUCCEEDED(api->GetValue(&kLabGop, &gv))) gopNow = gv.ulVal;
    // an NV12 picture: red above, blue below
    const size_t ySize = (size_t)c.w * c.h, frameBytes = ySize * 3 / 2;
    BYTE* pic = (BYTE*)malloc(frameBytes);
    for (int y = 0; y < c.h; ++y) memset(pic + (size_t)y * c.w, y < c.h / 2 ? 81 : 41, c.w);
    for (int y = 0; y < c.h / 2; ++y) { BYTE* uv = pic + ySize + (size_t)y * c.w; for (int x = 0; x < c.w; x += 2) { bool top = y < c.h / 4; uv[x] = top ? 90 : 240; uv[x + 1] = top ? 240 : 110; } }
    IMFSinkWriter* sw = nullptr; DWORD st = 0; HRESULT hrSw = S_OK, hrWr = S_OK;
    if (c.mp4) {
        IMFAttributes* at = nullptr; MFCreateAttributes(&at, 2); at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        IMFMediaType* cur = nullptr; hrSw = mft->GetOutputCurrentType(0, &cur);
        if (SUCCEEDED(hrSw)) hrSw = MFCreateSinkWriterFromURL(c.file, nullptr, at, &sw);
        if (SUCCEEDED(hrSw)) hrSw = sw->AddStream(cur, &st);
        if (SUCCEEDED(hrSw)) hrSw = sw->SetInputMediaType(st, cur, nullptr);
        if (SUCCEEDED(hrSw)) hrSw = sw->BeginWriting();
        if (cur) cur->Release(); at->Release();
    }
    MFT_OUTPUT_STREAM_INFO info; memset(&info, 0, sizeof(info)); mft->GetOutputStreamInfo(0, &info);
    int frames = c.fps * 30, outSamples = 0, idrs = 0; int idrAt[40] = {};
    for (int i = 0; i < frames && SUCCEEDED(hrI) && SUCCEEDED(hrO); ++i) {
        IMFMediaBuffer* b = nullptr; IMFSample* sm = nullptr; BYTE* d = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)frameBytes, &b)) || FAILED(b->Lock(&d, nullptr, nullptr))) break;
        memcpy(d, pic, frameBytes); b->Unlock(); b->SetCurrentLength((DWORD)frameBytes);
        MFCreateSample(&sm); sm->AddBuffer(b);
        sm->SetSampleTime((LONGLONG)i * 10000000LL / c.fps); sm->SetSampleDuration(10000000LL / c.fps);
        HRESULT r = mft->ProcessInput(0, sm, 0); sm->Release(); b->Release();
        if (FAILED(r)) break;
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER ob; memset(&ob, 0, sizeof(ob));
            IMFSample* os = nullptr; IMFMediaBuffer* obuf = nullptr;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                MFCreateSample(&os); MFCreateMemoryBuffer(info.cbSize ? info.cbSize : (DWORD)frameBytes, &obuf); os->AddBuffer(obuf); ob.pSample = os;
            }
            DWORD status = 0; HRESULT ro = mft->ProcessOutput(0, 1, &ob, &status);
            if (ro == S_OK && ob.pSample) {
                IMFMediaBuffer* mb = nullptr; ob.pSample->ConvertToContiguousBuffer(&mb);
                BYTE* bp = nullptr; DWORD len = 0;
                if (mb && SUCCEEDED(mb->Lock(&bp, nullptr, &len))) {
                    bool idr = false;
                    for (DWORD k = 0; k + 4 < len; ++k) if (bp[k] == 0 && bp[k + 1] == 0 && bp[k + 2] == 1 && (bp[k + 3] & 0x1F) == 5) { idr = true; break; }
                    if (idr) { if (idrs < 40) idrAt[idrs] = outSamples + 1; ++idrs; }
                    mb->Unlock();
                }
                if (mb) mb->Release();
                if (sw && SUCCEEDED(hrSw)) { HRESULT w1 = sw->WriteSample(st, ob.pSample); if (FAILED(w1)) hrWr = w1; }
                ++outSamples;
            }
            if (ob.pSample && ob.pSample != os) ob.pSample->Release();
            if (os) os->Release();
            if (obuf) obuf->Release();
            if (ob.pEvents) ob.pEvents->Release();
            if (ro != S_OK) break;
        }
    }
    wchar_t list[300] = L""; size_t ln = 0;
    for (int k = 0; k < idrs && k < 12; ++k) ln += _snwprintf(list + ln, 300 - ln - 1, L"%d ", idrAt[k]);
    Rep(L"INFO", L"direct [%s] %dx%d %d fps: GOP asked %d (when %d, 0x%08lX), read back %u; types out 0x%08lX in 0x%08lX; %d frames in, %d out, %d IDR: %s", c.name, c.w, c.h, c.fps, c.gop, c.when, (unsigned long)hrGop, gopNow, (unsigned long)hrO, (unsigned long)hrI, frames, outSamples, idrs, list);
    HRESULT hrFin = S_OK; if (sw) { hrFin = sw->Finalize(); if (SUCCEEDED(hrSw) && FAILED(hrFin)) {} sw->Release(); }
    if (c.mp4) Rep(L"INFO", L"  sink writer on the encoded stream: setup 0x%08lX, WriteSample 0x%08lX, finalize 0x%08lX, file %s", (unsigned long)hrSw, (unsigned long)hrWr, (unsigned long)hrFin, c.file);
    free(pic);
    if (api) api->Release();
    out->Release(); in->Release(); mft->Release();
}
} // namespace

// ---- audio lab: which MP3 / AAC output types does this Windows accept in an MP4 sink writer? ------------------------------------------
static bool AudioLabTry(const wchar_t* what, IMFMediaType* out, IMFMediaType* in)
{
    wchar_t pd[MAX_PATH] = L"", path[MAX_PATH + 40];
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    _snwprintf(path, MAX_PATH + 40, L"%s\\Click2Screen\\logs\\audiolab.mp4", pd);
    DeleteFileW(path);
    IMFSinkWriter* w = nullptr; DWORD idx = 0;
    HRESULT h1 = MFCreateSinkWriterFromURL(path, nullptr, nullptr, &w), h2 = E_FAIL, h3 = E_FAIL;
    if (SUCCEEDED(h1) && w) { h2 = w->AddStream(out, &idx); if (SUCCEEDED(h2)) h3 = w->SetInputMediaType(idx, in, nullptr); }
    if (w) w->Release();
    DeleteFileW(path);
    Rep(L"INFO", L"audiolab [%s]: writer 0x%08lX, AddStream 0x%08lX, SetInputMediaType 0x%08lX", what, (unsigned long)h1, (unsigned long)h2, (unsigned long)h3);
    return SUCCEEDED(h3);
}

// ---- round 9: what makes the size of the H.264 file follow the quality? Realistic content (a smooth picture with noise), sizes at three quality choices ------
namespace {
static const GUID kLabMean   = { 0xf7222374, 0x2144, 0x4815, { 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52 } };    // AVEncCommonMeanBitRate
static const GUID kLabMaxBr  = { 0x9651eae4, 0x39b9, 0x4ebf, { 0x85, 0xef, 0xd7, 0xf4, 0x44, 0xec, 0x74, 0x65 } };    // AVEncCommonMaxBitRate
static const GUID kLabMaxQp  = { 0x3daf6f66, 0xa6a7, 0x45e0, { 0xa8, 0xe5, 0xf2, 0x74, 0x3f, 0x46, 0xa3, 0xa2 } };    // AVEncVideoMaxQP
static const GUID kLabQual   = { 0xfcbf57a3, 0x7ea5, 0x4b0c, { 0x96, 0x44, 0x69, 0xb4, 0x0c, 0x39, 0xc3, 0x91 } };    // AVEncCommonQuality
static const GUID kLabRc     = { 0x1c0608e9, 0x370c, 0x4710, { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 } };    // AVEncCommonRateControlMode
static const GUID kLabGopSz  = { 0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };    // AVEncMPVGOPSize

struct SizeVariant { const wchar_t* name; int rc; /* -1 not set */ bool mean; double maxMul; /* 0 not set */ int quality; /* 0 not set, else per choice below */ int maxQp; /* 0 not set */ };

static int g_qual3[3] = { 85, 65, 40 };
static double g_capMul3[3] = { 2.0, 1.2, 0.7 };
static int g_capBase = 0;           // bit/s the cap multipliers apply to (maxMul < 0 asks for it)

static unsigned long long SizeOne(const SizeVariant& v, int choice, const DWORD* noise, int W, int H, int fps, int secs, int idx, int* keysOut)
{
    wchar_t pd[MAX_PATH] = L"", path[MAX_PATH + 40];
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    _snwprintf(path, MAX_PATH + 40, L"%s\\Click2Screen\\logs\\size-%d.mp4", pd, idx);
    DeleteFileW(path);
    static const int bitrates[3] = { 900000, 600000, 360000 };
    const int* qualities = g_qual3;
    static const int maxQps[3] = { 34, 40, 46 };
    const int bitrate = bitrates[choice];
    IMFAttributes* attrs = nullptr; IMFSinkWriter* w = nullptr;
    MFCreateAttributes(&attrs, 2); attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (FAILED(MFCreateSinkWriterFromURL(path, nullptr, attrs, &w)) || !w) { attrs->Release(); return 0; }
    IMFMediaType* vo = nullptr; IMFMediaType* vi = nullptr; MFCreateMediaType(&vo); MFCreateMediaType(&vi);
    vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); vo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vo->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate); vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive); vo->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(vo, MF_MT_FRAME_SIZE, W, H); MFSetAttributeRatio(vo, MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(vo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); vi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32); vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vi, MF_MT_FRAME_SIZE, W, H); MFSetAttributeRatio(vi, MF_MT_FRAME_RATE, fps, 1); MFSetAttributeRatio(vi, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD st = 0;
    HRESULT hr = w->AddStream(vo, &st);
    if (SUCCEEDED(hr)) hr = w->SetInputMediaType(st, vi, nullptr);
    wchar_t note[200] = L"";
    if (SUCCEEDED(hr)) {
        ICodecAPI* api = nullptr;
        if (SUCCEEDED(w->GetServiceForStream(st, GUID_NULL, kLabIidApi, (void**)&api)) && api) {
            VARIANT x; VariantInit(&x);
            auto setU = [&](const GUID& g, unsigned val, const wchar_t* nm) { x.vt = VT_UI4; x.ulVal = val; HRESULT r = api->SetValue(&g, &x); if (r != S_OK) _snwprintf(note + wcslen(note), 190 - wcslen(note), L" %s:0x%08lX", nm, (unsigned long)r); };
            setU(kLabGopSz, (unsigned)(fps * 20), L"gop");
            if (v.rc >= 0) setU(kLabRc, (unsigned)v.rc, L"rc");
            if (v.mean) setU(kLabMean, (unsigned)bitrate, L"mean");
            if (v.maxMul > 0) setU(kLabMaxBr, (unsigned)(bitrate * v.maxMul), L"max");
            if (v.maxMul < 0) setU(kLabMaxBr, (unsigned)(g_capBase * g_capMul3[choice]), L"cap");
            if (v.quality) setU(kLabQual, (unsigned)qualities[choice], L"quality");
            if (v.maxQp) setU(kLabMaxQp, (unsigned)maxQps[choice], L"maxqp");
            api->Release();
        }
        IMFSinkWriterEx* wx = nullptr;
        if (SUCCEEDED(w->QueryInterface(__uuidof(IMFSinkWriterEx), (void**)&wx)) && wx) {
            for (DWORD ti = 0; ti < 6; ++ti) {
                IMFTransform* t = nullptr; GUID cg = GUID_NULL;
                if (FAILED(wx->GetTransformForStream(st, ti, &cg, &t)) || !t) break;
                if (IsEqualGUID(cg, MFT_CATEGORY_VIDEO_ENCODER)) {
                    IMFMediaType* to = nullptr; IMFMediaType* ti2 = nullptr; t->GetOutputCurrentType(0, &to); t->GetInputCurrentType(0, &ti2);
                    if (to) t->SetOutputType(0, to, 0);
                    if (ti2) t->SetInputType(0, ti2, 0);
                    if (to) to->Release();
                    if (ti2) ti2->Release();
                }
                t->Release();
            }
            wx->Release();
        }
        hr = w->BeginWriting();
    }
    const size_t bytes = (size_t)W * H * 4;
    BYTE* pic = (BYTE*)malloc(bytes);
    const int W2 = W * 2;
    int done = 0;
    for (int i = 0; i < fps * secs && SUCCEEDED(hr); ++i) {
        int off = (i * 13) % W;
        for (int y = 0; y < H; ++y) memcpy(pic + (size_t)(H - 1 - y) * W * 4, noise + (size_t)y * W2 + off, (size_t)W * 4);      // bottom-up
        IMFMediaBuffer* b = nullptr; IMFSample* sm = nullptr; BYTE* d = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)bytes, &b)) || FAILED(b->Lock(&d, nullptr, nullptr))) break;
        memcpy(d, pic, bytes); b->Unlock(); b->SetCurrentLength((DWORD)bytes);
        MFCreateSample(&sm); sm->AddBuffer(b);
        sm->SetSampleTime((LONGLONG)i * 10000000LL / fps); sm->SetSampleDuration(10000000LL / fps);
        hr = w->WriteSample(st, sm); sm->Release(); b->Release();
        if (SUCCEEDED(hr)) ++done;
    }
    HRESULT hf = w->Finalize();
    w->Release(); free(pic); vo->Release(); vi->Release(); attrs->Release();
    int total = 0; *keysOut = Mp4KeyFrames(path, &total);
    unsigned long long sz = FileSz(path);
    Rep(L"INFO", L"  size [%s] choice %d (%d kbit/s asked): %llu bytes, %d frames written, finalize 0x%08lX, %d key frames%s", v.name, choice, bitrate / 1000, sz, done, (unsigned long)hf, *keysOut, note);
    return sz;
}
} // namespace

static int RunKeyLab9()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab9: MFStartup failed"); return 1; }
    const int W = 470, H = 302, fps = 25, secs = 7, W2 = W * 2;
    int amp = 32; { wchar_t e[16]; if (GetEnvironmentVariableW(L"C2S_QS_AMP", e, 16)) amp = _wtoi(e); }
    DWORD* noise = (DWORD*)malloc((size_t)W2 * H * 4); unsigned sd = 777;
    for (size_t i = 0; i < (size_t)W2 * H; ++i) {
        int x = (int)(i % W2), y = (int)(i / W2); int ch[3] = { x * 255 / W2, y * 255 / H, 128 }; DWORD c = 0;
        for (int k = 0; k < 3; ++k) { sd = sd * 1664525u + 1013904223u; int v = ch[k] + (int)((sd >> 16) % (2 * amp + 1)) - amp; v = v < 0 ? 0 : v > 255 ? 255 : v; c = (c << 8) | (DWORD)v; }
        noise[i] = c;
    }
    Rep(L"INFO", L"keylab9: H.264, %dx%d at %d fps for %d s, a smooth picture with noise +-%d scrolling; asked 900 / 600 / 360 kbit/s", W, H, fps, secs, amp);
    //                          name                                          rc  mean maxMul quality maxQp
    static const SizeVariant vars[] = {
        { L"as the recorder: bit rate in the type only",                      -1, false, 0,   0, 0  },
        { L"CBR + mean bit rate",                                              0, true,  0,   0, 0  },
        { L"peak constrained VBR + mean + max = mean",                         2, true,  1.0, 0, 0  },
        { L"unconstrained VBR + mean",                                         1, true,  0,   0, 0  },
        { L"quality mode + quality 85 / 65 / 40",                              3, false, 0,   1, 0  },
        { L"as the recorder + max QP 34 / 40 / 46",                           -1, false, 0,   0, 1  },
        { L"CBR + mean + max QP 34 / 40 / 46",                                 0, true,  0,   0, 1  },
    };
    int idx = 500;
    for (const SizeVariant& v : vars) {
        unsigned long long s3[3]; int keys;
        Rep(L"INFO", L"keylab9 variant: %s", v.name);
        for (int c = 0; c < 3; ++c) s3[c] = SizeOne(v, c, noise, W, H, fps, secs, ++idx, &keys);
        Rep(L"INFO", L"RESULT [%s]: %llu / %llu / %llu bytes, best/medium %.2f, medium/worst %.2f", v.name, s3[0], s3[1], s3[2], s3[1] ? (double)s3[0] / s3[1] : 0.0, s3[2] ? (double)s3[1] / s3[2] : 0.0);
    }
    free(noise);
    MFShutdown();
    Rep(L"INFO", L"keylab9: done");
    return 0;
}

static int RunKeyLab11()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab11: MFStartup failed"); return 1; }
    const int W = 470, H = 302, fps = 25, secs = 5, W2 = W * 2;
    static const int quals[] = { 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90 };
    static const int amps[] = { 2, 4, 8, 16 };
    int idx = 700;
    for (int amp : amps) {
        DWORD* noise = (DWORD*)malloc((size_t)W2 * H * 4); unsigned sd = 777;
        for (size_t i = 0; i < (size_t)W2 * H; ++i) {
            int x = (int)(i % W2), y = (int)(i / W2); int ch[3] = { x * 255 / W2, y * 255 / H, 128 }; DWORD c = 0;
            for (int k = 0; k < 3; ++k) { sd = sd * 1664525u + 1013904223u; int v = ch[k] + (int)((sd >> 16) % (2 * amp + 1)) - amp; v = v < 0 ? 0 : v > 255 ? 255 : v; c = (c << 8) | (DWORD)v; }
            noise[i] = c;
        }
        wchar_t line[600] = L""; size_t ln = 0;
        for (int q : quals) {
            g_qual3[0] = g_qual3[1] = g_qual3[2] = q;
            SizeVariant v = { L"curve", 3, false, 0.0, 1, 0 };
            int keys; unsigned long long sz = SizeOne(v, 0, noise, W, H, fps, secs, ++idx, &keys);
            ln += _snwprintf(line + ln, 600 - ln - 1, L" %d:%llu", q, sz / 1000);
        }
        Rep(L"INFO", L"CURVE noise %d (quality:KB for 5 s):%s", amp, line);
        free(noise);
    }
    MFShutdown();
    Rep(L"INFO", L"keylab11: done");
    return 0;
}

static int RunKeyLab10()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab10: MFStartup failed"); return 1; }
    const int W = 470, H = 302, fps = 25, secs = 7, W2 = W * 2;
    g_capBase = 500000;
    struct QSet { const wchar_t* name; int q[3]; bool cap; };
    static const QSet sets[] = {
        { L"quality 85/65/40",           { 85, 65, 40 }, false },
        { L"quality 70/50/30",           { 70, 50, 30 }, false },
        { L"quality 85/65/40 + cap 1000/600/350 kbit/s", { 85, 65, 40 }, true },
        { L"quality 70/50/30 + cap 1000/600/350 kbit/s", { 70, 50, 30 }, true },
        { L"quality 60/45/30 + cap 1000/600/350 kbit/s", { 60, 45, 30 }, true },
    };
    static const int amps[] = { 0, 4, 32 };
    int idx = 600;
    for (int amp : amps) {
        DWORD* noise = (DWORD*)malloc((size_t)W2 * H * 4); unsigned sd = 777;
        for (size_t i = 0; i < (size_t)W2 * H; ++i) {
            int x = (int)(i % W2), y = (int)(i / W2); int ch[3] = { x * 255 / W2, y * 255 / H, 128 }; DWORD c = 0;
            for (int k = 0; k < 3; ++k) { sd = sd * 1664525u + 1013904223u; int v = amp ? ch[k] + (int)((sd >> 16) % (2 * amp + 1)) - amp : ch[k]; v = v < 0 ? 0 : v > 255 ? 255 : v; c = (c << 8) | (DWORD)v; }
            noise[i] = c;
        }
        Rep(L"INFO", L"keylab10: content = a scrolling gradient with noise +-%d", amp);
        for (const QSet& qs : sets) {
            memcpy(g_qual3, qs.q, sizeof(g_qual3));
            SizeVariant v = { qs.name, 3, false, qs.cap ? -1.0 : 0.0, 1, 0 };
            unsigned long long s3[3]; int keys;
            for (int c = 0; c < 3; ++c) s3[c] = SizeOne(v, c, noise, W, H, fps, secs, ++idx, &keys);
            Rep(L"INFO", L"RESULT noise %d [%s]: %llu / %llu / %llu bytes, best/medium %.2f, medium/worst %.2f", amp, qs.name, s3[0], s3[1], s3[2], s3[1] ? (double)s3[0] / s3[1] : 0.0, s3[2] ? (double)s3[1] / s3[2] : 0.0);
        }
        free(noise);
    }
    MFShutdown();
    Rep(L"INFO", L"keylab10: done");
    return 0;
}

static const GUID kAudioWma8 = { 0x00000161, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static int RunAudioLab()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"audiolab: MFStartup failed"); return 1; }
    auto pcm = [](int rate, int ch) {
        IMFMediaType* t = nullptr; MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate); t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 2 * ch); t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * ch);
        return t;
    };
    auto ours = [](const GUID& sub, int bps, int rate, int ch) {
        IMFMediaType* t = nullptr; MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); t->SetGUID(MF_MT_SUBTYPE, sub);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate); t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
        t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bps); t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        if (sub == MFAudioFormat_AAC) { t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); }
        return t;
    };
    Rep(L"INFO", L"audiolab: the types of the program (44.1 kHz mono, bytes a second 16000 / 12000 / 8000 for MP3, 3000 / 2000 / 1500 for AAC)");
    IMFMediaType* in = pcm(44100, 1);
    const int mp3Bps[3] = { 16000, 12000, 8000 }, aacBps[3] = { 24000, 16000, 12000 };
    for (int i = 0; i < 3; ++i) { IMFMediaType* o = ours(MFAudioFormat_MP3, mp3Bps[i], 44100, 1); wchar_t n[64]; _snwprintf(n, 64, L"program MP3 %d", mp3Bps[i]); AudioLabTry(n, o, in); o->Release(); }
    for (int i = 0; i < 3; ++i) { IMFMediaType* o = ours(MFAudioFormat_AAC, aacBps[i], 44100, 1); wchar_t n[64]; _snwprintf(n, 64, L"program AAC %d", aacBps[i]); AudioLabTry(n, o, in); o->Release(); }
    in->Release();
    // the types Windows itself lists for the MP3 encoder
    Rep(L"INFO", L"audiolab: MFTranscodeGetAudioOutputAvailableTypes(MP3)");
    IMFCollection* col = nullptr;
    HRESULT hr = MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_MP3, MFT_ENUM_FLAG_ALL, nullptr, &col);
    DWORD n = 0; if (SUCCEEDED(hr) && col) col->GetElementCount(&n);
    Rep(L"INFO", L"  0x%08lX, %lu types", (unsigned long)hr, (unsigned long)n);
    int tried = 0;
    for (DWORD i = 0; SUCCEEDED(hr) && col && i < n && tried < 40; ++i) {
        IUnknown* u = nullptr; IMFMediaType* t = nullptr;
        if (FAILED(col->GetElement(i, &u)) || !u) continue;
        if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            UINT32 rate = 0, ch = 0, bps = 0, ba = 0, ud = 0; t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps); t->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &ba); t->GetBlobSize(MF_MT_USER_DATA, &ud);
            if (rate == 44100 && ch == 1) {
                wchar_t nm[100]; _snwprintf(nm, 100, L"system MP3 %lu Hz %lu ch %lu B/s align %lu userdata %lu", (unsigned long)rate, (unsigned long)ch, (unsigned long)bps, (unsigned long)ba, (unsigned long)ud);
                IMFMediaType* pin = pcm(44100, 1); AudioLabTry(nm, t, pin); pin->Release(); ++tried;
            }
            t->Release();
        }
        u->Release();
    }
    if (col) col->Release();
    MFShutdown();
    Rep(L"INFO", L"audiolab: MFTranscodeGetAudioOutputAvailableTypes(WMA 8) at 44.1 kHz mono");
    {
        IMFCollection* wc = nullptr;
        HRESULT hw = MFTranscodeGetAudioOutputAvailableTypes(kAudioWma8, MFT_ENUM_FLAG_ALL, nullptr, &wc);
        DWORD wn = 0; if (SUCCEEDED(hw) && wc) wc->GetElementCount(&wn);
        Rep(L"INFO", L"  0x%08lX, %lu types", (unsigned long)hw, (unsigned long)wn);
        for (DWORD i = 0; SUCCEEDED(hw) && wc && i < wn; ++i) {
            IUnknown* u = nullptr; IMFMediaType* t = nullptr;
            if (FAILED(wc->GetElement(i, &u)) || !u) continue;
            if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&t))) && t) {
                UINT32 rate = 0, ch = 0, bps = 0, bits = 16; t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps); t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                if (ch == 1) Rep(L"INFO", L"  WMA %lu Hz mono %lu bit, %lu bytes a second (%lu kbit/s)", (unsigned long)rate, (unsigned long)bits, (unsigned long)bps, (unsigned long)(bps * 8 / 1000));
                t->Release();
            }
            u->Release();
        }
        if (wc) wc->Release();
    }
    Rep(L"INFO", L"audiolab: done");
    return 0;
}

static int RunKeyLab7()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab7: MFStartup failed"); return 1; }
    Rep(L"INFO", L"keylab7: GOP 200 through ICodecAPI, then the types negotiated again in four ways (the sink writer, still picture, 1080p)");
    //             name                                            wmv    w     h    fps bitrate  jump gopA gopApi spac qvsA qvsApi rc stride topDown frames ex nex ranges declFps ptsFps renego
    static const LabCfg cfgs[] = {
        { L"no renegotiation (as in 1.0.300)",                     false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0, 0, 0 },
        { L"same input type again through the sink writer",        false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0, 0, 1 },
        { L"types set again on the encoder (IMFSinkWriterEx)",     false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0, 0, 2 },
        { L"changed input type through the sink writer",           false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0, 0, 3 },
        { L"flush and types set again on the encoder",             false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0, 0, 4 },
    };
    int idx = 300;
    for (const LabCfg& c : cfgs) LabOne(c, ++idx, false);
    MFShutdown();
    Rep(L"INFO", L"keylab7: done");
    return 0;
}

static int RunKeyLab6()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab6: MFStartup failed"); return 1; }
    Rep(L"INFO", L"keylab6: can the GOP be applied after the types, and an MP4 written from the encoder's own H.264");
    static const DirectCfg cfgs[] = {
        { L"GOP 200 set after the types, then the types again",    200, 4, 1920, 1080, 10, false, nullptr },
        { L"GOP 200 set after the types, flush, types again",      200, 5, 1920, 1080, 10, false, nullptr },
        { L"GOP 200 before the types, to MP4 (pass-through)",      200, 1, 1920, 1080, 10, true,  L"C:\\ProgramData\\Click2Screen\\logs\\lab6-pass-200.mp4" },
        { L"GOP 200 before the types, 640x360, to MP4",            200, 1, 640,  360,  10, true,  L"C:\\ProgramData\\Click2Screen\\logs\\lab6-pass-360.mp4" },
        { L"no GOP, to MP4 (for comparison)",                      0,   0, 1920, 1080, 10, true,  L"C:\\ProgramData\\Click2Screen\\logs\\lab6-pass-none.mp4" },
    };
    for (const DirectCfg& c : cfgs) DirectOne(c);
    MFShutdown();
    Rep(L"INFO", L"keylab6: done");
    return 0;
}

static int RunKeyLab5()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab5: MFStartup failed"); return 1; }
    Rep(L"INFO", L"keylab5: every H.264 encoder that Windows lists");
    {
        MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_H264 };
        const UINT32 flagSets[] = { MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_TRANSCODE_ONLY | MFT_ENUM_FLAG_FIELDOFUSE, 0x3F };
        for (UINT32 fl : flagSets) {
            IMFActivate** act = nullptr; UINT32 cnt = 0;
            HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, fl, nullptr, &out, &act, &cnt);
            Rep(L"INFO", L"  MFTEnumEx flags 0x%X: 0x%08lX, %u found", fl, (unsigned long)hr, cnt);
            for (UINT32 i = 0; SUCCEEDED(hr) && act && i < cnt; ++i) {
                wchar_t nm[160] = L""; UINT32 len = 0; act[i]->GetString(MFT_FRIENDLY_NAME_Attribute, nm, 160, &len);
                GUID clsid = GUID_NULL; act[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
                Rep(L"INFO", L"    \"%s\" {%08lX-%04X-%04X-...}", nm, (unsigned long)clsid.Data1, clsid.Data2, clsid.Data3);
                act[i]->Release();
            }
            if (act) CoTaskMemFree(act);
        }
    }
    Rep(L"INFO", L"keylab5: the Microsoft H.264 encoder used directly, 30 s of a still picture, where are the IDR frames");
    //                      name                                  gop when  w     h    fps
    static const DirectCfg cfgs[] = {
        { L"no GOP set",                                           0,   0, 1920, 1080, 10 },
        { L"GOP 200 set before the types",                         200, 1, 1920, 1080, 10 },
        { L"GOP 200 set after the types",                          200, 2, 1920, 1080, 10 },
        { L"GOP 200 set after BEGIN_STREAMING",                    200, 3, 1920, 1080, 10 },
        { L"GOP 30 set before the types",                          30,  1, 1920, 1080, 10 },
        { L"GOP 200 before the types, 25 fps",                     500, 1, 1920, 1080, 25 },
        { L"GOP 200 before the types, 640x360",                    200, 1, 640,  360,  10 },
    };
    for (const DirectCfg& c : cfgs) DirectOne(c);
    MFShutdown();
    Rep(L"INFO", L"keylab5: done");
    return 0;
}

static int RunKeyLab4()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab4: MFStartup failed"); return 1; }
    //  Is the period of the encoder's own key frames counted in frames of the frame rate it was told, or in time? And what does a told rate do to the size?
    //             name                                            wmv    w     h    fps bitrate  jump gopA gopApi spac qvsA qvsApi rc stride topDown frames ex nex ranges declFps ptsFps
    static const LabCfg cfgs[] = {
        { L"1080p told 10 fps, stamps 10 fps (as now), still",     false, 1920, 1080, 10, 2400000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0,   0 },
        { L"1080p told 30 fps, stamps 10 fps, still",              false, 1920, 1080, 10, 2400000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 30,  10 },
        { L"1080p told 60 fps, stamps 10 fps, still",              false, 1920, 1080, 10, 2400000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 60,  10 },
        { L"1080p told 120 fps, stamps 10 fps, still",             false, 1920, 1080, 10, 2400000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 120, 10 },
        { L"1080p told 10 fps, stamps 25 fps, still",              false, 1920, 1080, 10, 2400000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 10,  25 },
        { L"1080p told 25 fps, stamps 10 fps, still",              false, 1920, 1080, 10, 2400000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 25,  10 },
        { L"720p told 60 fps, stamps 10 fps, still",               false, 1280, 720,  10, 2000000, 0, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 60,  10 },
        { L"1080p told 10 fps, stamps 10 fps, the picture changes every frame (size to compare)", false, 1920, 1080, 10, 2400000, 1, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 0, 0 },
        { L"1080p told 60 fps, stamps 10 fps, the picture changes every frame", false, 1920, 1080, 10, 2400000, 1, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 60, 10 },
        { L"1080p told 30 fps, stamps 10 fps, the picture changes every frame", false, 1920, 1080, 10, 2400000, 1, 0, 0, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false, 30, 10 },
    };
    int idx = 200;
    Rep(L"INFO", L"keylab4: H.264, the frame rate told to the encoder against the time stamps");
    for (const LabCfg& c : cfgs) LabOne(c, ++idx, false);
    MFShutdown();
    Rep(L"INFO", L"keylab4: done");
    return 0;
}

static int RunKeyLab3()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab3: MFStartup failed"); return 1; }
    static const LabEx exContent[] = { { &kLabContentType, 1, 1, L"ContentType(FixedCameraAngle)" } };
    static const LabEx exRtOff[] = { { &kLabRealTime, 2, 0, L"RealTime" } };
    static const LabEx exRtOn[] = { { &kLabRealTime, 2, 1, L"RealTime" } };
    static const LabEx exLowOn[] = { { &kLabCommonLow, 2, 1, L"CommonLowLatency" } };
    static const LabEx exLowOff[] = { { &kLabCommonLow, 2, 0, L"CommonLowLatency" } };
    static const LabEx exLayer[] = { { &kLabTemporal, 1, 1, L"TemporalLayers" } };
    static const LabEx exRef1[] = { { &kLabMaxRef, 1, 1, L"MaxNumRefFrame" } };
    static const LabEx exCombo[] = { { &kLabRealTime, 2, 0, L"RealTime" }, { &kLabCommonLow, 2, 0, L"CommonLowLatency" }, { &kLabContentType, 1, 1, L"ContentType" } };
    //             name                                           wmv    w     h    fps bitrate  jump gopA gopApi spac qvsA qvsApi rc stride topDown frames ex nex ranges
    static const LabCfg cfgs[] = {
        { L"H.264 1080p, GOP 200, the ranges",                    false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, nullptr, 0, true },
        { L"H.264 1080p, QvS 0",                                  false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, 0,  0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, QvS 20",                                 false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, 20, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, QvS 50",                                 false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, 50, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, QvS 85",                                 false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, 85, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, QvS 100",                                false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, 100, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, content type: fixed camera",             false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exContent, 1, false },
        { L"H.264 1080p, real time off",                          false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exRtOff, 1, false },
        { L"H.264 1080p, real time on",                           false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exRtOn, 1, false },
        { L"H.264 1080p, common low latency on",                  false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exLowOn, 1, false },
        { L"H.264 1080p, common low latency off",                 false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exLowOff, 1, false },
        { L"H.264 1080p, one temporal layer",                     false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exLayer, 1, false },
        { L"H.264 1080p, one reference frame",                    false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exRef1, 1, false },
        { L"H.264 1080p, real time off + low latency off + fixed camera", false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 0, 0, false, 300, exCombo, 3, false },
        { L"H.264 1080p, CBR + GOP 200",                          false, 1920, 1080, 10, 2400000, 0, 0, 200, 0, -1, -1, 3, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, GOP 100 (10 s) to see whether any value counts", false, 1920, 1080, 10, 2400000, 0, 0, 100, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, GOP 5 (half a second)",                  false, 1920, 1080, 10, 2400000, 0, 0, 5, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, GOP 15 (1.5 s)",                         false, 1920, 1080, 10, 2400000, 0, 0, 15, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false },
        { L"H.264 1080p, GOP 25 (2.5 s)",                         false, 1920, 1080, 10, 2400000, 0, 0, 25, 0, -1, -1, 0, 0, false, 300, nullptr, 0, false },
    };
    int idx = 100;
    Rep(L"INFO", L"keylab3: H.264 1080p, 10 fps, 300 frames of a picture that never changes");
    for (const LabCfg& c : cfgs) LabOne(c, ++idx, false);
    MFShutdown();
    Rep(L"INFO", L"keylab3: done");
    return 0;
}

static int RunKeyLab()
{
    HRESULT hrc = CoInitializeEx(nullptr, COINIT_MULTITHREADED); (void)hrc;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Rep(L"INFO", L"keylab: MFStartup failed"); return 1; }
    //             name                                                  wmv    w     h    fps bitrate  jump gopA gopApi spac qvsA qvsApi rc stride topDown frames
    static const LabCfg h264[] = {
        { L"H.264 as the recorder (GOP 200 both ways), jump every 10", false, 1280, 720, 10, 2000000, 10, 200, 200, 0,   85, -1, 0, 0, false, 600 },
        { L"H.264 the picture never changes",                         false, 1280, 720, 10, 2000000, 0,  200, 200, 0,   85, -1, 0, 0, false, 600 },
        { L"H.264 jump every 100 frames (10 s)",                      false, 1280, 720, 10, 2000000, 100, 200, 200, 0,  85, -1, 0, 0, false, 600 },
        { L"H.264 + MF_MT_MAX_KEYFRAME_SPACING 200, never changes",   false, 1280, 720, 10, 2000000, 0,  200, 200, 200, 85, -1, 0, 0, false, 600 },
        { L"H.264 + spacing 200 only (no GOP), never changes",        false, 1280, 720, 10, 2000000, 0,  0,   0,   200, 85, -1, 0, 0, false, 600 },
        { L"H.264 + spacing 200, jump every 10",                      false, 1280, 720, 10, 2000000, 10, 200, 200, 200, 85, -1, 0, 0, false, 600 },
        { L"H.264 bit rate 500 kbit/s, never changes",                false, 1280, 720, 10, 500000,  0,  200, 200, 0,   85, -1, 0, 0, false, 600 },
        { L"H.264 bit rate 8 Mbit/s, never changes",                  false, 1280, 720, 10, 8000000, 0,  200, 200, 0,   85, -1, 0, 0, false, 600 },
        { L"H.264 640x360, never changes",                            false, 640,  360, 10, 800000,  0,  200, 200, 0,   85, -1, 0, 0, false, 600 },
        { L"H.264 1920x1080 as a recording, never changes",           false, 1920, 1080, 10, 2400000, 0, 200, 200, 0,   85, -1, 0, 0, false, 300 },
        { L"H.264 1920x1080 25 fps GOP 500, never changes",           false, 1920, 1080, 25, 2400000, 0, 500, 500, 0,   85, -1, 0, 0, false, 300 },
        { L"H.264 1920x1080 QvS 25 (the fast one), never changes",    false, 1920, 1080, 10, 2400000, 0, 200, 200, 0,   25, -1, 0, 0, false, 300 },
    };
    //             name                                                  wmv   w     h     fps bitrate jump gopA gopApi spac qvsA qvsApi rc stride topDown frames
    static const LabCfg wmv[] = {
        { L"WMV 1920x1080 as the recorder now (no stride)",           true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, 85, -1, 0, 0,  false, 200 },
        { L"WMV 1920x1080 stride -W*4",                               true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, 85, -1, 0, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, QvS 0 through ICodecAPI",      true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, -1, 0,  0, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, QvS 25 through ICodecAPI",     true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, -1, 25, 0, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, QvS 50 through ICodecAPI",     true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, -1, 50, 0, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, QvS 100 through ICodecAPI",    true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, -1, 100, 0, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, CBR through ICodecAPI",        true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, -1, -1, 3, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, VBR through ICodecAPI",        true, 1920, 1080, 10, 2400000, 10, 0, 0, 0, -1, -1, 1, -1, false, 200 },
        { L"WMV 1920x1080 stride -W*4, picture never changes",        true, 1920, 1080, 10, 2400000, 0,  0, 0, 0, 85, -1, 0, -1, false, 200 },
        { L"WMV 1280x720 stride -W*4",                                true, 1280, 720,  10, 2000000, 10, 0, 0, 0, 85, -1, 0, -1, false, 200 },
        { L"WMV 640x360 stride -W*4",                                 true, 640,  360,  10, 800000,  10, 0, 0, 0, 85, -1, 0, -1, false, 200 },
    };
    int idx = 0;
    Rep(L"INFO", L"keylab: H.264 (the numbers of the key frames of the file, the processor time of the encoding alone)");
    for (const LabCfg& c : h264) LabOne(c, ++idx, false);
    Rep(L"INFO", L"keylab: WMV (the files are kept beside the logs; the host decodes them: red above, blue below)");
    for (const LabCfg& c : wmv) LabOne(c, ++idx, true);
    LabRecorderWmv();
    MFShutdown();
    Rep(L"INFO", L"keylab: done");
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppLogOpen(L"autotest");
    // The console of whoever started the tests (a black window with the job's output) may lie where the fixture is and get into the pictures.
    { HWND con = GetConsoleWindow(); if (con && IsWindowVisible(con)) ShowWindow(con, SW_MINIMIZE); }
    int minutes = 3;
    uint64_t seed = 0;
    bool keep = false;
    const wchar_t* only = nullptr;
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab")) return RunKeyLab();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab3")) return RunKeyLab3();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab4")) return RunKeyLab4();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab5")) return RunKeyLab5();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab6")) return RunKeyLab6();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab7")) return RunKeyLab7();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--audiolab")) return RunAudioLab();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab9")) return RunKeyLab9();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab10")) return RunKeyLab10();
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--keylab11")) return RunKeyLab11();
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--seed") && i + 1 < argc) seed = _wcstoui64(argv[++i], nullptr, 10);
        else if (!wcscmp(argv[i], L"--keep")) keep = true;
        else if (!wcscmp(argv[i], L"--only") && i + 1 < argc) only = argv[++i];      // names separated by commas, each run once
        else if (!wcscmp(argv[i], L"--installer")) g_installerAllowed = true;
        else if (!wcscmp(argv[i], L"--slow") && i + 1 < argc) g_scale = _wtoi(argv[++i]);
        else if (argv[i][0] != L'-') minutes = _wtoi(argv[i]);
    }
    if (minutes < 1) minutes = 1;
    if (!seed) seed = ((uint64_t)time(nullptr) << 16) ^ GetTickCount64() ^ 0x9E3779B97F4A7C15ULL;
    g_state = seed ? seed : 1;
    for (int i = 0; i < 8; ++i) NextU();
    g_wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (g_wine && g_scale == 1) g_scale = 4;          // Wine at a low priority on a busy machine is slow
    if (g_installerAllowed && !g_wine && !IsAdmin()) { WARN(L"--installer needs administrator rights: the installer test will be skipped"); g_installerAllowed = false; }
    SettingsUseKey(L"Software\\Click2Screen-Autotest");
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    _snwprintf(g_dir, MAX_PATH, L"%sc2sautotest-%lu", tmp, GetCurrentProcessId());
    SHCreateDirectoryExW(nullptr, g_dir, nullptr);
    INFO(L"Click2Screen autotest %s: seed %llu, %d minute(s)%s, files in %s", C2S_VERSION_STR, (unsigned long long)seed, minutes, g_wine ? L", running under Wine (Media Foundation encoder tests skipped)" : L"", g_dir);

    TestDef tests[] = {
        { L"settings", T_Settings, 2 }, { L"ini", T_Ini, 2 }, { L"template", T_Template, 3 }, { L"sizes", T_Sizes, 1 },
        { L"languages", T_Languages, 1 }, { L"capture", T_Capture, 4 }, { L"scaled", T_Scaled, 2 }, { L"edges", T_Edges, 1 },
        { L"pickers", T_Pickers, 3 }, { L"recorder", T_Recorder, 3 }, { L"scheduler", T_RecorderSched, 3 },
        { L"lifecycle", T_RecorderLifecycle, 2 }, { L"hotkeys", T_Hotkeys, 1 }, { L"installer", T_Installer, 1 },
        { L"fuzz-registry", T_FuzzSettings, 2 }, { L"fuzz-ini", T_FuzzIni, 2 }, { L"long-paths", T_LongPaths, 2 }, { L"threads", T_Threads, 2 },
        { L"leaks", T_Leaks, 2 }, { L"effects", T_Effects, 2 }, { L"program-frame", T_AppFrame, 3 }, { L"audio", T_Audio, 2 }, { L"hotkey-keys", T_HotkeyKeys, 2 }, { L"program-effects", T_AppEffects, 3 }, { L"long-recording", T_LongRecord, 1 }, { L"program", T_App, 5 },
        { L"no-network", T_NoNetwork, 1 }, { L"save-locked", T_SaveLocked, 1 }, { L"program-locale", T_AppLocale, 2 }, { L"program-crash", T_AppCrashRestart, 2 }, { L"program-flood", T_AppFlood, 2 }, { L"program-hotkey-move", T_AppHotkeyMove, 2 }, { L"keyframes", T_KeyInterval, 1 }, { L"audio-codecs", T_AudioCodecs, 1 }, { L"quality-sizes", T_QualitySizes, 1 }, { L"avi-still", T_AviStill, 1 }, { L"install-while-running", T_InstallWhileRunning, 1 }, { L"setup-window", T_SetupGui, 2 }, { L"setup-install-window", T_SetupGuiInstall, 1 }, { L"program-picks", T_AppPicks, 3 }, { L"program-oddities", T_AppOddities, 3 }, { L"monkey", T_Monkey, 4 },
    };
    const int nt = (int)(sizeof(tests) / sizeof(tests[0]));
    DWORD t0 = GetTickCount();
    if (only) {                                    // just the named tests, once each (a repeated name runs again)
        int runs2 = 0;
        for (const wchar_t* p = only; *p;) {
            wchar_t nm[40]; int l = 0;
            while (*p && *p != L',' && l < 39) nm[l++] = *p++;
            nm[l] = 0; if (*p == L',') ++p;
            bool found = false;
            for (int i = 0; i < nt; ++i) if (!wcscmp(tests[i].name, nm)) { INFO(L"--- test: %s", nm); tests[i].fn(); ++runs2; found = true; }
            if (!found) FAIL(L"no test named %s", nm);
        }
        if (g_fx.hw) DestroyWindow(g_fx.hw);
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest");
        INFO(L"%d test(s) run: %d PASS, %d FAIL, %d WARN, %d SKIP (seed %llu)", runs2, g_pass, g_fail, g_warn, g_skip, (unsigned long long)seed);
        return g_fail ? 1 : 0;
    }
    int order[64], n = nt;
    for (int i = 0; i < nt; ++i) order[i] = i;
    for (int i = nt - 1; i > 0; --i) { int j = R(0, i); int t = order[i]; order[i] = order[j]; order[j] = t; }
    int round = 0, runs = 0;
    for (;;) {
        int idx;
        if (round < n) idx = order[round];
        else {                                      // after the first pass: random picks, heavier tests more often
            int tot = 0; for (int i = 0; i < nt; ++i) tot += tests[i].weight;
            int x = R(1, tot); idx = 0;
            for (int i = 0; i < nt; ++i) { x -= tests[i].weight; if (x <= 0) { idx = i; break; } }
        }
        ++round;
        if ((!wcscmp(tests[idx].name, L"installer") || !wcscmp(tests[idx].name, L"setup-install-window") || !wcscmp(tests[idx].name, L"install-while-running")) && round > n) continue;       // the installer cycle runs once
        INFO(L"--- test %d: %s", runs + 1, tests[idx].name);
        tests[idx].fn();
        ++runs;
        if ((int)(GetTickCount() - t0) > minutes * 60 * 1000 && round >= n) break;
    }
    if (g_fx.hw) DestroyWindow(g_fx.hw);
    // clean up
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Click2Screen-Autotest");
    if (!keep) {
        wchar_t from[MAX_PATH + 2] = {}; wcsncpy(from, g_dir, MAX_PATH);
        SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
    INFO(L"%d test(s) run in %.1f s: %d PASS, %d FAIL, %d WARN, %d SKIP (seed %llu)", runs, (GetTickCount() - t0) / 1000.0, g_pass, g_fail, g_warn, g_skip, (unsigned long long)seed);
    return g_fail ? 1 : 0;
}
