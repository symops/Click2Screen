// c2sautotest.exe [minutes] [--seed N] [--keep] [--installer] — randomized end-to-end test of Click2Screen.
//
// Every test runs with random parameters drawn from one seeded generator (the seed is printed first; the same seed
// repeats the same sequence of tests and parameters). A test window of four coloured quarters is shown on the screen and
// everything is checked as a user would see it: the screenshot / recording of a random part of it must have the right
// size, orientation and colours, saved files must decode again, the settings must survive the registry and the .ini
// export, the file names must follow the template, every language table must be complete, the region / window pickers
// must return what the "mouse" selected, the editor must save the rectangles it drew, and the recorder must produce an
// MP4 whose size, duration, frame count and picture match what was recorded (pauses, time limit, a moving window).
// With --installer the installer is also run through a random install / update / remove cycle (administrator).
//
// PASS / FAIL / WARN / SKIP go to the console and to %ProgramData%\Click2Screen\logs\autotest.log; exit code 1 if
// anything failed. Settings are kept in HKCU\Software\Click2Screen-Autotest (removed at the end), files in
// %TEMP%\c2sautotest-<pid> (removed unless --keep). WARN: something the environment may not offer (no microphone, a
// hotkey taken, no H.264 decoder); SKIP: not possible here (Media Foundation encoder under Wine).
#include "c2s.h"
#include "uiids.h"
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
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

static void RandomSettings(Settings& s)
{
    SettingsDefaults(s);
    wchar_t t[40];
    RandomText(t, 20); _snwprintf(s.vidFolder, MAX_PATH, L"%s\\vid %s", g_dir, t);
    RandomText(t, 20); _snwprintf(s.imgFolder, MAX_PATH, L"%s\\img %s", g_dir, t);
    wchar_t name[40]; RandomText(name, 12);
    switch (R(0, 3)) {
        case 0: _snwprintf(s.fileName, 128, L"%s $Y-$M-$D $H-$N-$S", name); break;
        case 1: _snwprintf(s.fileName, 128, L"Img%s", L"####"); break;
        case 2: _snwprintf(s.fileName, 128, L"%s_#_$D", name); break;
        default: _snwprintf(s.fileName, 128, L"%%USERNAME%% %s ##", name); break;
    }
    s.imageFormat = R(0, 2); s.capMouse = Chance(50); s.highlight = Chance(50); s.sounds = Chance(50); s.mic = Chance(50);
    s.openFolder = Chance(50); s.openEditor = Chance(50); s.runAtStartup = false;   // autostart is not touched by the test
    s.delaySec = R(0, 60); s.durH = R(0, 99); s.durM = R(0, 59); s.durS = R(0, 59);
    s.videoCodec = R(0, 1); s.audioCodec = R(0, 1); s.videoQuality = R(0, 2); s.audioQuality = R(0, 2);
    static const int f[] = { 5, 10, 15, 20, 25, 30, 45, 60 };
    s.fps = f[R(0, 7)];
    s.language = R(0, LangCount());
}

static bool SettingsEqual(const Settings& a, const Settings& b, bool withLanguage, wchar_t* why)
{
#define CMPS(f) if (!Same(a.f, b.f)) { _snwprintf(why, 150, L"%s differs: [%s] vs [%s]", L"" #f, a.f, b.f); return false; }
#define CMPI(f) if (a.f != b.f) { _snwprintf(why, 120, L"%s: %d vs %d", L"" #f, (int)a.f, (int)b.f); return false; }
    CMPS(vidFolder) CMPS(imgFolder) CMPS(fileName) CMPI(imageFormat) CMPI(capMouse) CMPI(highlight) CMPI(sounds) CMPI(mic)
    CMPI(openFolder) CMPI(openEditor) CMPI(delaySec) CMPI(durH) CMPI(durM) CMPI(durS) CMPI(videoCodec) CMPI(audioCodec)
    CMPI(videoQuality) CMPI(audioQuality) CMPI(fps)
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

static void MakeFixture(int w, int h)
{
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
    SetTimer(g_fx.hw, 1, 40, nullptr);
    UpdateWindow(g_fx.hw);
    Pump(250);
}

static RECT FxScreenRect() { RECT r; GetWindowRect(g_fx.hw, &r); return r; }

static COLORREF Expected(int lx, int ly, int fw, int fh)     // colour of the fixture at its local point
{
    return kQuad[(ly >= fh / 2 ? 2 : 0) + (lx >= fw / 2 ? 1 : 0)];
}
static bool SafePoint(int lx, int ly, int fw, int fh)        // away from the quarter borders and the strip
{
    if (abs(lx - fw / 2) < 8 || abs(ly - fh / 2) < 8) return false;
    return ly < fh - 16 && lx >= 2 && lx < fw - 2 && ly >= 2;
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
    return bad;
}

// ---- WIC decoding of saved images --------------------------------------------------------------------------------
static bool DecodePixels(const wchar_t* path, int* w, int* h, uint8_t** px)
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

int CurrentDpi() { return 96; }          // the editor asks the main program for it

// ==== tests ==================================================================================================
static void T_Settings()
{
    SettingsUseKey(L"Software\\Click2Screen-Autotest");
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
        Check(c.fps >= 5 && c.fps <= 60 && c.delaySec <= 60 && c.imageFormat <= 2, L"settings: out-of-range registry values are clamped");
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
    Check(ok && c.fps == 25 && c.delaySec == 3 && Same(c.fileName, L"$Y-$M-$D at $H-$N-$S") && c.videoQuality == 2, L"ini: invalid values are ignored, valid ones applied");
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
        _snwprintf(buf, 400, T(S_SU_STATE_INST), L"1.0.0", L"C:\\p"); if (!buf[0]) ++bad;
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
    static const wchar_t* ext[] = { L"png", L"jpg", L"bmp" };
    for (int f = 0; f < 3; ++f) {
        wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\cap%d.%s", g_dir, R(0, 99999), ext[f]);
        bool s = SaveBitmap(bmp, path, f);
        int dw = 0, dh = 0; uint8_t* px = nullptr;
        bool d = s && DecodePixels(path, &dw, &dh, &px);
        int tol = f == IMG_JPG ? 28 : 3, badp = 0, n = 0;
        if (d && dw == cw && dh == ch) {
            for (int i = 0; i < 60 && n < 12; ++i) {
                int x = R(0, dw - 1), y = R(0, dh - 1), lx = ox + x, ly = oy + y;
                if (!SafePoint(lx, ly, g_fx.w, g_fx.h)) continue;
                if (f == IMG_JPG && (abs(lx - g_fx.w / 2) < 20 || abs(ly - g_fx.h / 2) < 20)) continue;   // ringing at edges
                const uint8_t* p = px + ((size_t)y * dw + x) * 4;
                COLORREF e = Expected(lx, ly, g_fx.w, g_fx.h);
                ++n; if (!Near(e, p[2], p[1], p[0], tol)) ++badp;
            }
        }
        wchar_t nm[120]; _snwprintf(nm, 120, L"save %s: file decodes, %dx%d, colours (%d points)", ext[f], cw, ch, n);
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
    bool ok = gr.Grab(rc, false, false);
    HBITMAP bmp = gr.Release();
    int checked = 0, bad = ok ? CheckColors(bmp, 0, 0, sw, sh, 24, 14, &checked) : 1;
    Check(ok && bad == 0 && checked > 0, name);
    if (bmp) DeleteObject(bmp);
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

// ---- pickers and the editor, driven by messages -----------------------------------------------------------------
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
        SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(j->b.x - vs.left, j->b.y - vs.top));
    } else if (j->mode == 1) {  // point at a window, click
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
    int mode = R(0, 2);
    PickJob job; memset(&job, 0, sizeof(job)); job.mode = mode;
    RECT got = {0, 0, 0, 0}; HWND gotw = nullptr; bool ok;
    if (mode == 0) {
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

static void T_Editor()
{
    MakeFixture(R(300, 500), R(240, 400));
    RECT fr = FxScreenRect();
    HBITMAP bmp = CaptureRect(fr, false, false);
    if (!bmp) { FAIL(L"editor: no picture to edit"); return; }
    int f = R(0, 2);
    static const wchar_t* ext[] = { L"png", L"jpg", L"bmp" };
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\edit%d.%s", g_dir, R(0, 99999), ext[f]);
    SaveBitmap(bmp, path, f);
    HBITMAP copy = (HBITMAP)CopyImage(bmp, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    DeleteObject(bmp);
    OpenEditor(nullptr, copy, path, f);
    Pump(200);
    HWND ed = FindWindowW(L"Click2ScreenEditor", nullptr);
    if (!ed) { FAIL(L"editor: the window did not open"); return; }
    // a storm of zoom / scroll / resize messages must neither crash nor hang the editor, nor change the picture
    {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        for (int i = 0; i < 40; ++i) {
            switch (R(0, 6)) {
                case 0: SendMessageW(ed, WM_COMMAND, 2, 0); break;                              // zoom in
                case 1: SendMessageW(ed, WM_COMMAND, 3, 0); break;                              // zoom out
                case 2: SendMessageW(ed, WM_MOUSEWHEEL, MAKEWPARAM(Chance(50) ? MK_CONTROL : 0, (short)(Chance(50) ? 120 : -120)), MAKELPARAM(50, 50)); break;
                case 3: SendMessageW(ed, WM_HSCROLL, MAKEWPARAM(R(0, 1) ? SB_LINEDOWN : SB_PAGEUP, 0), 0); break;
                case 4: SendMessageW(ed, WM_VSCROLL, MAKEWPARAM(R(0, 1) ? SB_PAGEDOWN : SB_LINEUP, 0), 0); break;
                case 5: SetWindowPos(ed, nullptr, wa.left, wa.top, R(300, 1000), R(250, 700), SWP_NOZORDER | SWP_NOACTIVATE); break;
                default: SendMessageW(ed, WM_KEYDOWN, Chance(50) ? VK_ADD : VK_SUBTRACT, 0); break;
            }
            Pump(5);
        }
        DWORD_PTR res = 0;
        bool alive = SendMessageTimeoutW(ed, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 5000, &res) != 0;
        Check(alive && IsWindow(ed), L"editor: 40 random zoom / scroll / resize messages leave it alive");
    }
    int n = R(1, 4), added = 0;
    RECT rects[4];
    for (int i = 0; i < n; ++i) {
        int w = R(30, g_fx.w / 2), h = R(30, g_fx.h / 2);
        int l = R(10, g_fx.w - w - 10), t = R(10, g_fx.h - h - 10);
        RECT r = { l, t, l + w, t + h };
        if (EditorTestAddRect(ed, r.left, r.top, r.right, r.bottom)) rects[added++] = r;
    }
    bool saved = EditorTestSave(ed);
    DestroyWindow(ed);
    Pump(50);
    int dw = 0, dh = 0; uint8_t* px = nullptr;
    bool d = saved && DecodePixels(path, &dw, &dh, &px);
    int tol = f == IMG_JPG ? 60 : 12, red = 0, tested = 0;
    if (d) for (int i = 0; i < added; ++i) {
        // the left edge of each rectangle (the pen is 3 px wide, centred on the edge)
        for (int k = 0; k < 4; ++k) {
            int y = rects[i].top + 4 + (rects[i].bottom - rects[i].top - 8) * k / 3, x = rects[i].left;
            if (x < 1 || x >= dw - 1 || y < 0 || y >= dh) continue;
            bool isRed;
            if (f == IMG_JPG) {          // a 3 px line is blurred by JPEG: look for the reddest pixel around the edge
                int best = -999;
                for (int dx = -2; dx <= 2; ++dx) {
                    if (x + dx < 0 || x + dx >= dw) continue;
                    const uint8_t* q = px + ((size_t)y * dw + x + dx) * 4;
                    int d = (int)q[2] - (q[1] > q[0] ? q[1] : q[0]);
                    if (d > best) best = d;
                }
                isRed = best >= 70;
            } else {
                const uint8_t* p = px + ((size_t)y * dw + x) * 4;
                isRed = Near(RGB(230, 40, 40), p[2], p[1], p[0], tol);
            }
            ++tested; if (isRed) ++red;
        }
    }
    wchar_t nm[140]; _snwprintf(nm, 140, L"editor: %d rectangle(s) are in the saved .%s file (%d/%d edge points red)", added, ext[f], red, tested);
    Check(saved && d && added > 0 && tested > 0 && red * 10 >= tested * 8, nm);
    delete[] px;
}

// ---- recorder ----------------------------------------------------------------------------------------------------
static bool MfAvailable()
{
    if (g_wine) return false;
    return true;
}

struct VideoInfo { bool ok; int w, h; double dur; int frames; bool h264; bool audio; double adur; };

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
            if (SUCCEEDED(nt->GetGUID(MF_MT_SUBTYPE, &sub))) v.h264 = sub == MFVideoFormat_H264;
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
                IMFMediaType* cur = nullptr; INT32 stride = 0;
                if (SUCCEEDED(rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
                    UINT32 s = 0;
                    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = (INT32)s; else stride = v.w * 4;
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
                            IMFMediaBuffer* mb = nullptr; BYTE* data = nullptr; DWORD len = 0;
                            if (SUCCEEDED(sm->ConvertToContiguousBuffer(&mb)) && SUCCEEDED(mb->Lock(&data, nullptr, &len))) {
                                int absStride = stride < 0 ? -stride : stride;
                                int bad = 0, n = 0;
                                for (int i = 0; i < 200 && n < 16; ++i) {
                                    int x = R(0, v.w - 1), y = R(0, v.h - 1);
                                    // the recorded part is the whole fixture (region mode) scaled to w x h
                                    int lx = (int)((x + 0.5) * g_fx.w / v.w), ly = (int)((y + 0.5) * g_fx.h / v.h);
                                    if (!SafePoint(lx, ly, g_fx.w, g_fx.h) || abs(lx - g_fx.w / 2) < 20 || abs(ly - g_fx.h / 2) < 20) continue;
                                    const BYTE* p = stride < 0 ? data + (size_t)(v.h - 1 - y) * absStride + x * 4 : data + (size_t)y * absStride + x * 4;
                                    COLORREF e = Expected(lx, ly, g_fx.w, g_fx.h);
                                    ++n; if (!Near(e, p[2], p[1], p[0], 60)) ++bad;
                                }
                                *pictureChecked = n; *pictureOk = n > 0 && bad == 0;
                                mb->Unlock();
                            }
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

static DWORD WINAPI PauseSpam(LPVOID p)
{
    Recorder* rec = (Recorder*)p;
    for (int i = 0; i < 40; ++i) { rec->SetPaused((i & 1) == 0); Sleep(R(1, 15)); }
    rec->SetPaused(false);
    return 0;
}

static void RecordOnce(bool useNull)
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
    p.cfg.capMouse = Chance(40); p.cfg.highlight = Chance(30);
    p.cfg.mic = !useNull && Chance(40);
    int limit = 0;
    p.cfg.durH = p.cfg.durM = p.cfg.durS = 0;
    if (Chance(30)) { limit = R(2, 3); p.cfg.durS = limit; }
    bool pause = Chance(40), spam = Chance(25), move = mode == 2 && Chance(40);
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%s\\rec%d.mp4", g_dir, R(0, 999999));
    wcscpy(p.path, path);
    if (useNull) SetEnvironmentVariableW(L"C2S_NULLSINK", L"1"); else SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    wchar_t desc[300];
    _snwprintf(desc, 300, L"record (%s) %s %dx%d fps=%d codec=%d q=%d mic=%d limit=%d pause=%d spam=%d move=%d", useNull ? L"no encoder" : L"MF",
               mode == 2 ? L"window" : mode == 1 ? L"part" : L"region", pw, ph, p.cfg.fps, p.cfg.videoCodec, p.cfg.videoQuality, p.cfg.mic, limit, pause, spam, move);
    INFO(L"%s", desc);
    Recorder rec;
    wchar_t err[300] = L"";
    if (!rec.Start(p, err, 300)) { FAIL(L"%s: Start failed: %s", desc, err); SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr); return; }
    if (err[0]) WARN(L"%s: %s", desc, err);
    int ew, eh; int srcw = pw, srch = ph;
    if (mode == 2) { RECT vis; WindowVisibleRect(g_fx.hw, &vis); srcw = vis.right - vis.left; srch = vis.bottom - vis.top; }
    FitSize(srcw, srch, ew, eh);
    Check(rec.Width() == ew && rec.Height() == eh, L"recorder: output size follows FitSize", desc);
    DWORD t0 = GetTickCount();
    int total = limit ? limit + 2 : R(3, 5);
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
    bool limited = rec.LimitReached();
    unsigned long long frames = rec.FramesWritten();
    bool stopped = rec.Stop();
    bool failedSelf = rec.Failed();
    Check(stopped && !failedSelf, L"recorder: Stop finalizes the file", desc);
    if (limit) {
        wchar_t d2[120]; _snwprintf(d2, 120, L"limit %ds, recorded %.2fs", limit, el);
        Check(limited && fabs(el - limit) < 0.7, L"recorder: the time limit stops the recording by itself", d2);
    } else if (!spam) {
        wchar_t d2[160]; _snwprintf(d2, 160, L"wall %.2fs, paused %.2fs, recorded %.2fs", wall, pausedFor, el);
        Check(fabs(el - (wall - pausedFor)) < 0.6, L"recorder: recorded time = wall time - pauses", d2);
    }
    if (useNull) {
        // frames at the requested rate while running (spam / pauses make the expected count a range)
        double lo = (limit ? limit : el) * p.cfg.fps * 0.4, hi = (el + 0.3) * p.cfg.fps * 1.15 + 3;
        wchar_t d2[140]; _snwprintf(d2, 140, L"%llu frames in %.2fs at %d fps", frames, el, p.cfg.fps);
        Check((double)frames <= hi && (double)frames >= (spam ? 1.0 : lo * 0.7), L"recorder: frame count follows the frame rate (no frames while paused)", d2);
        SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
        return;
    }
    // the MP4 itself
    unsigned long long sz = FileSz(path);
    Check(sz > 1500, L"recorder: the MP4 file exists and is not empty", desc);
    bool pictureOk = false, decoderMissing = false; int pictureChecked = 0;
    bool wantPicture = !move && p.cfg.capMouse == false;
    VideoInfo v = Inspect(path, wantPicture, &pictureOk, &pictureChecked, &decoderMissing);
    wchar_t d3[200];
    _snwprintf(d3, 200, L"file %dx%d, %.2fs, %d frames, h264=%d audio=%d", v.w, v.h, v.dur, v.frames, v.h264, v.audio);
    Check(v.ok && v.h264 && v.w == ew && v.h == eh, L"recorder: MP4 is H.264 at the expected size", d3);
    Check(fabs(v.dur - el) < 0.9, L"recorder: MP4 duration matches the recorded time", d3);
    if (decoderMissing) WARN(L"recorder: no H.264 decoder here (Windows N edition?) - frames and picture not checked");
    else {
        double expect = el * p.cfg.fps;
        Check(v.frames > expect * 0.25 && v.frames < expect * 1.3 + 4, L"recorder: number of video frames is plausible for the frame rate", d3);
        if (wantPicture) {
            if (mode == 1) SKIP(L"recorder: picture check is for the whole fixture only");
            else Check(pictureOk, L"recorder: a decoded frame has the colours of the screen in the right places", pictureChecked ? L"" : L"(no points)");
        }
    }
    if (p.cfg.mic) {
        if (v.audio) Check(true, L"recorder: the MP4 has the microphone track");
        else WARN(L"recorder: no audio track (no microphone in this session?)");
    }
}

static void T_Recorder()
{
    RecordOnce(!MfAvailable());
}

static void T_RecorderSched()
{
    RecordOnce(true);                  // schedule / pauses / limit, no encoder: works everywhere
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
    NEED(s.imageFormat >= 0 && s.imageFormat <= 2) NEED(s.fps >= 5 && s.fps <= 60) NEED(s.delaySec >= 0 && s.delaySec <= 60)
    NEED(s.durH >= 0 && s.durH <= 99) NEED(s.durM >= 0 && s.durM <= 59) NEED(s.durS >= 0 && s.durS <= 59)
    NEED(s.videoCodec >= 0 && s.videoCodec <= 1) NEED(s.audioCodec >= 0 && s.audioCodec <= 1)
    NEED(s.videoQuality >= 0 && s.videoQuality <= 2) NEED(s.audioQuality >= 0 && s.audioQuality <= 2)
    NEED(s.language >= 0 && s.language <= LangCount()) NEED(TemplateValid(s.fileName))
    NEED(wcslen(s.vidFolder) < MAX_PATH && wcslen(s.imgFolder) < MAX_PATH)
    return true;
}

static void T_FuzzSettings()
{
    static const wchar_t* names[] = { L"VidOutputFolder", L"ImgOutputFolder", L"FileName", L"ImageFormat", L"CapMouse", L"Highlight",
        L"EnableSound", L"EnableMicRec", L"OpenOutFolder", L"OpenEditor", L"RunAtStartup", L"Delay", L"RecDurationHour", L"RecDurationMin",
        L"RecDurationSec", L"VideoCodec", L"AudioCodec", L"VideoQuality", L"AudioQuality", L"Fps", L"Language" };
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
        static const wchar_t* keys[] = { L"Fps", L"Delay", L"FileName", L"VidOutputFolder", L"ImageFormat", L"CapMouse", L"VideoCodec", L"Junk", L"RecDurationSec", L"Language" };
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
            for (int i = 0; i < 20; ++i) g.Grab(rc, p.cfg.capMouse, false);
            QueryPerformanceCounter(&b);
            cost = (double)(b.QuadPart - a.QuadPart) / f.QuadPart / 20.0;
        }
    }
    double capacity = cost > 0 ? 1.0 / (cost * 1.25) : 1e9;           // frames per second the capture can deliver
    double rate = p.cfg.fps < capacity ? p.cfg.fps : capacity;
    double expect = el * rate;
    wchar_t d[200]; _snwprintf(d, 200, L"%llu frames in %.2f s at %d fps; one capture takes %.1f ms, so about %.0f frames/s possible (%.0f%% of that)", (unsigned long long)fr, el, p.cfg.fps, cost * 1000, capacity, 100.0 * fr / (el * rate));
    Check(fabs(el - secs) < 1.0 && fr >= expect * 0.85 && fr <= el * p.cfg.fps * 1.05 + 2, L"long recording: no drift - the frame count follows the clock and the capture speed", d);
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
    HWND dlg = FindWin(pid, L"#32770", waitMs);
    if (!dlg) return false;
    PostMessageW(dlg, WM_COMMAND, button, 0);
    return true;
}

static int CountFiles(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

struct App { PROCESS_INFORMATION pi; HWND main; Settings cfg; wchar_t img[MAX_PATH], vid[MAX_PATH]; };

static bool StartApp(App& a, const Settings& s, const wchar_t* extraArgs = L"")
{
    memset(&a, 0, sizeof(a));
    a.cfg = s;
    SettingsSave(s);                                  // into the test key; the program reads it through C2S_REGKEY
    SetEnvironmentVariableW(L"C2S_REGKEY", L"Software\\Click2Screen-Autotest");
    wchar_t inst[40]; _snwprintf(inst, 40, L"-at%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"C2S_INSTANCE", inst);
    SetEnvironmentVariableW(L"C2S_NULLSINK", L"1");
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t* sl = wcsrchr(self, L'\\'); if (sl) *sl = 0;
    wchar_t cmd[MAX_PATH + 100]; _snwprintf(cmd, MAX_PATH + 100, L"\"%s\\Click2Screen.exe\" %s", self, extraArgs);
    STARTUPINFOW si = { sizeof(si) };
    bool ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &a.pi) != FALSE;
    SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    if (!ok) return false;
    a.main = FindWin(a.pi.dwProcessId, C2S_CLASS_MAIN, 15000);
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
    s.sounds = false; s.openFolder = false; s.openEditor = false; s.runAtStartup = false;
    s.delaySec = R(0, 2); s.durH = s.durM = s.durS = 0;
    s.mic = false;
}

static void AppTexts(App& a, int language, const wchar_t* tag)
{
    LangSelect(language);
    struct { int id; Str s; } t[] = { {IDC_REC_DESKTOP, S_REC_DESKTOP}, {IDC_REC_REGION, S_REC_REGION}, {IDC_REC_WINDOW, S_REC_WINDOW}, {IDC_SHOT_DESKTOP, S_SHOT_DESKTOP},
        {IDC_SHOT_REGION, S_SHOT_REGION}, {IDC_SHOT_WINDOW, S_SHOT_WINDOW}, {IDC_OPTIONS, S_OPTIONS}, {IDC_HINT, S_HOTKEYS_HINT} };
    int bad = 0;
    DWORD end = GetTickCount() + WMS(8000);                   // the program rebuilds its texts a moment after the change
    for (;;) {
        bad = 0;
        for (auto& i : t) { wchar_t got[400]; WinText(GetDlgItem(a.main, i.id), got, 400); if (wcscmp(got, T(i.s))) ++bad; }
        wchar_t title[200]; WinText(a.main, title, 200);
        if (wcscmp(title, T(S_APP))) ++bad;
        if (!bad || (int)(end - GetTickCount()) <= 0) break;
        Sleep(100);
    }
    if (bad) for (auto& i : t) {
        wchar_t got[400]; WinText(GetDlgItem(a.main, i.id), got, 400);
        if (wcscmp(got, T(i.s))) Rep(L"INFO", L"  control %d: [%s] expected [%s]", i.id, got, T(i.s));
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
    _snwprintf(s.imgFolder, MAX_PATH, L"%s\\%s\\img", g_dir, t); _snwprintf(s.vidFolder, MAX_PATH, L"%s\\%s\\vid", g_dir, t);
    bool limitFlow = Chance(25);
    if (limitFlow) s.durS = 3;
    App a;
    if (!StartApp(a, s)) { FAIL(L"program: Click2Screen.exe did not show its window"); StopApp(a, false); return; }
    INFO(L"program flow: language %d, delay %d s, limit %d s", language, s.delaySec, s.durS);
    LangSelect(language == 0 ? 0 : language);
    AppTexts(a, language, L"at start");

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
        int before = CountFiles(s.imgFolder);
        PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0);
        DWORD end = GetTickCount() + WMS(20000);
        while (CountFiles(s.imgFolder) <= before && (int)(end - GetTickCount()) > 0) Sleep(100);
        wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%s\\*.*", s.imgFolder);
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        wchar_t file[MAX_PATH] = L"";
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) _snwprintf(file, MAX_PATH, L"%s\\%s", s.imgFolder, fd.cFileName); } while (FindNextFileW(h, &fd)); FindClose(h); }
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
        static const wchar_t* ext[] = { L".png", L".jpg", L".bmp" };
        size_t fl = wcslen(file), el = wcslen(ext[s.imageFormat]);
        Check(fl > el && !_wcsicmp(file + fl - el, ext[s.imageFormat]), L"program: the file has the extension of the chosen format");
    }

    // a recording through the panel
    {
        LangSelect(language);
        PostMessageW(a.main, WM_COMMAND, IDC_REC_DESKTOP, 0);
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
                if (!limitFlow) {
                    PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0);
                    Check(WaitText(panel, IDC_P_START, T(S_RESUME), 3000), L"program: Pause -> the button offers Resume");
                    WinText(GetDlgItem(panel, IDC_P_TIME), t1, 100); Sleep(1200); WinText(GetDlgItem(panel, IDC_P_TIME), t2, 100);
                    Check(!wcscmp(t1, t2), L"program: the time label stands still while paused", t2);
                    PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_START, BN_CLICKED), 0);
                    Check(WaitText(panel, IDC_P_START, T(S_PAUSE), 3000), L"program: Resume -> recording again");
                    if (Chance(40)) {                          // hide the panel, bring it back from the tray menu command
                        PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_HIDE, BN_CLICKED), 0);
                        bool asked = DismissBox(a.pi.dwProcessId, IDYES, 5000);
                        Sleep(300);
                        Check(asked && !IsWindowVisible(panel), L"program: Hide asks once and hides the panel");
                        PostMessageW(a.main, WM_COMMAND, ID_TRAY_PANEL, 0);
                        Sleep(300);
                        Check(IsWindowVisible(panel), L"program: the tray command shows the panel again");
                    }
                    if (Chance(50)) PostMessageW(a.main, WM_HOTKEY, HK_STOP, 0);                     // F11
                    else PostMessageW(panel, WM_COMMAND, MAKEWPARAM(IDC_P_STOP, BN_CLICKED), 0);    // the Stop button
                } else {
                    Sleep(1500);                                // the time limit stops it by itself
                }
                DWORD end = GetTickCount() + WMS(12000);
                while (FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) && (int)(end - GetTickCount()) > 0) Sleep(100);
                Check(FindWin(a.pi.dwProcessId, C2S_CLASS_PANEL, 0) == nullptr, limitFlow ? L"program: the time limit closes the recording by itself" : L"program: Stop closes the panel");
                Sleep(300);
                wchar_t st[500], prefix[100]; WinText(GetDlgItem(a.main, IDC_STATUS), st, 500);
                wcsncpy(prefix, T(S_SAVED), 99); prefix[99] = 0; wchar_t* pc = wcsstr(prefix, L"%s"); if (pc) *pc = 0;
                Check(IsWindowVisible(a.main) && wcsncmp(st, prefix, wcslen(prefix)) == 0, L"program: the main window is back and reports the saved file", st);
            }
        }
    }

    // the options window changes the settings the program keeps
    {
        PostMessageW(a.main, WM_COMMAND, IDC_OPTIONS, 0);
        HWND o = FindReady(a.pi.dwProcessId, C2S_CLASS_OPTIONS, 10000);
        Check(o != nullptr, L"program: Options opens its window");
        if (o) {
            Settings w; RandomSettings(w);
            w.runAtStartup = false; w.sounds = false; w.openFolder = false; w.openEditor = false;
            wcscpy(w.vidFolder, s.vidFolder); wcscpy(w.imgFolder, s.imgFolder);
            static const int fpsv[] = { 10, 15, 20, 25, 30, 45, 60 };
            w.fps = fpsv[R(0, 6)];
            w.language = R(0, LangCount());
            auto chk = [&](int id, bool v) { SendMessageW(GetDlgItem(o, id), BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); };
            chk(O_AUTOSTART, false); chk(O_MOUSE, w.capMouse); chk(O_HILITE, w.highlight); chk(O_OPENFOLDER, false); chk(O_OPENEDITOR, false); chk(O_SOUNDS, false); chk(O_MIC, w.mic);
            SetWindowTextW(GetDlgItem(o, O_DELAY), L""); wchar_t n[16];
            _snwprintf(n, 16, L"%d", w.delaySec); SetWindowTextW(GetDlgItem(o, O_DELAY), n);
            SendMessageW(GetDlgItem(o, O_VCODEC), CB_SETCURSEL, w.videoCodec, 0); SendMessageW(GetDlgItem(o, O_ACODEC), CB_SETCURSEL, w.audioCodec, 0);
            SendMessageW(GetDlgItem(o, O_VQ), CB_SETCURSEL, w.videoQuality, 0); SendMessageW(GetDlgItem(o, O_AQ), CB_SETCURSEL, w.audioQuality, 0);
            int fi = 0; for (int i = 0; i < 7; ++i) if (fpsv[i] == w.fps) fi = i;
            SendMessageW(GetDlgItem(o, O_FPS), CB_SETCURSEL, fi, 0);
            _snwprintf(n, 16, L"%d", w.durH); SetWindowTextW(GetDlgItem(o, O_DH), n);
            _snwprintf(n, 16, L"%d", w.durM); SetWindowTextW(GetDlgItem(o, O_DM), n);
            _snwprintf(n, 16, L"%d", w.durS); SetWindowTextW(GetDlgItem(o, O_DS), n);
            SendMessageW(GetDlgItem(o, O_IMGFMT), CB_SETCURSEL, w.imageFormat, 0);
            SendMessageW(GetDlgItem(o, O_LANG), CB_SETCURSEL, w.language, 0);
            bool bad = Chance(30);
            SetWindowTextW(GetDlgItem(o, O_TPL), bad ? L"a#b#" : w.fileName);
            PostMessageW(o, WM_COMMAND, IDOK, 0);
            if (bad) {
                bool warned = DismissBox(a.pi.dwProcessId, IDOK, 5000);
                Sleep(300);
                Check(warned && IsWindow(o), L"program: an invalid template is refused with a message and the window stays open");
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
                AppTexts(a, w.language, L"after changing the language in Options");
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
        SendMessageW(ov, WM_LBUTTONUP, 0, L(b));
    } else if (mode == 1) {
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
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
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
    _snwprintf(s.imgFolder, MAX_PATH, L"%s\\%s\\img", g_dir, t); _snwprintf(s.vidFolder, MAX_PATH, L"%s\\%s\\vid", g_dir, t);
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
        if (what == 0) {                                               // region screenshot (Alt+Shift+4)
            POINT p1 = { vs.left + R(10, (vs.right - vs.left) - 330), vs.top + R(10, (vs.bottom - vs.top) - 260) };
            POINT p2 = { p1.x + R(40, 320), p1.y + R(40, 250) };
            bool flip = Chance(50); if (flip) { POINT t = p1; p1 = p2; p2 = t; }
            int before = CountFiles(s.imgFolder);
            PostMessageW(a.main, WM_HOTKEY, HK_REGION, 0);
            bool driven = DrivePicker(a.pi.dwProcessId, 0, p1, p2, Chance(30));
            wchar_t file[MAX_PATH] = L""; int dw = 0, dh = 0; uint8_t* px = nullptr;
            bool got = driven && WaitNewFile(s.imgFolder, before, 20000, file) && DecodePixels(file, &dw, &dh, &px);
            _snwprintf(nm, 160, L"program picks: region screenshot %dx%d is saved at exactly that size", abs(p2.x - p1.x), abs(p2.y - p1.y));
            Check(got && dw == abs(p2.x - p1.x) && dh == abs(p2.y - p1.y), nm);
            delete[] px;
        } else if (what == 1) {                                        // window screenshot (Alt+Shift+5)
            POINT p = { fr.left + R(20, g_fx.w - 20), fr.top + R(20, g_fx.h - 20) };
            int before = CountFiles(s.imgFolder);
            PostMessageW(a.main, WM_HOTKEY, HK_WINDOW, 0);
            bool driven = DrivePicker(a.pi.dwProcessId, 1, p, p);
            wchar_t file[MAX_PATH] = L""; int dw = 0, dh = 0; uint8_t* px = nullptr;
            bool got = driven && WaitNewFile(s.imgFolder, before, 20000, file) && DecodePixels(file, &dw, &dh, &px);
            RECT vis; WindowVisibleRect(g_fx.hw, &vis);
            wchar_t dd[200]; _snwprintf(dd, 200, L"driven=%d got=%d file %dx%d, window %dx%d", (int)driven, (int)got, dw, dh, (int)(vis.right - vis.left), (int)(vis.bottom - vis.top));
            Check(got && dw == vis.right - vis.left && dh == vis.bottom - vis.top, L"program picks: window screenshot has the size of the pointed window", dd);
            int bad = 0, n = 0;
            if (got) for (int i = 0; i < 400 && n < 10; ++i) {
                int x = R(0, dw - 1), y = R(0, dh - 1);
                if (!SafePoint(x, y, g_fx.w, g_fx.h)) continue;
                if (s.imageFormat == IMG_JPG && (abs(x - g_fx.w / 2) < 20 || abs(y - g_fx.h / 2) < 20)) continue;     // JPEG rings at edges
                const uint8_t* q = px + ((size_t)y * dw + x) * 4;
                COLORREF ex = Expected(x, y, g_fx.w, g_fx.h);
                // Under Wine / Xvfb a part of the window may not be repainted yet when the dimmed picker has just gone: a
                // pure black pixel is then not counted (on Windows the desktop is composed and every pixel counts).
                if (g_wine && q[0] == 0 && q[1] == 0 && q[2] == 0) continue;
                ++n; if (!Near(ex, q[2], q[1], q[0], s.imageFormat == IMG_JPG ? 40 : 6)) { ++bad; if (bad < 4) Rep(L"INFO", L"  window picture (%d,%d) = %d,%d,%d expected %d,%d,%d (format %d, picture %dx%d)", x, y, q[2], q[1], q[0], GetRValue(ex), GetGValue(ex), GetBValue(ex), s.imageFormat, dw, dh); }
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
            int before = CountFiles(s.imgFolder);
            PostMessageW(a.main, WM_COMMAND, IDC_SHOT_DESKTOP, 0);
            wchar_t f[MAX_PATH];
            Check(WaitNewFile(s.imgFolder, before, 20000, f), L"program oddities: the screenshot button works without the hotkeys");
        }
        StopApp(a);
        for (int i = 1; i <= 3; ++i) UnregisterHotKey(hold, i);
        DestroyWindow(hold);
        (void)held;
    } else if (which == 1) {                           // an output folder that cannot exist: a message, no crash, the program goes on
        wcscpy(s.imgFolder, L"C:\\nul\\x\\no such drive\\img");
        s.openEditor = false;
        App a;
        if (!StartApp(a, s)) { FAIL(L"program oddities: no window"); StopApp(a, false); return; }
        PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0);
        bool box = DismissBox(a.pi.dwProcessId, IDOK, 15000);
        Sleep(300);
        Check(box && AppAlive(a), L"program oddities: an unusable pictures folder gives a message and the program stays up");
        DWORD_PTR res = 0;
        Check(SendMessageTimeoutW(a.main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 5000, &res) != 0, L"program oddities: the window still answers after that");
        StopApp(a);
    } else if (which == 2) {                           // the editor opens from the program, with its own title, and closes
        s.openEditor = true;
        App a;
        if (!StartApp(a, s)) { FAIL(L"program oddities: no window"); StopApp(a, false); return; }
        PostMessageW(a.main, WM_HOTKEY, HK_DESKTOP, 0);
        HWND ed = FindWin(a.pi.dwProcessId, C2S_CLASS_EDITOR, 20000);
        Check(ed != nullptr, L"program oddities: the editor opens after a screenshot when asked");
        if (ed) {
            LangSelect(s.language);
            wchar_t title[200] = L"";
            DWORD tend = GetTickCount() + WMS(10000);          // the editor first draws the big picture
            do { WinText(ed, title, 200); if (!wcscmp(title, T(S_ED_TITLE))) break; Sleep(100); } while ((int)(tend - GetTickCount()) > 0);
            if (wcscmp(title, T(S_ED_TITLE))) Rep(L"INFO", L"  editor title [%s] expected [%s], program language %d", title, T(S_ED_TITLE), s.language);
            Check(!wcscmp(title, T(S_ED_TITLE)), L"program oddities: the editor title is in the program's language", title);
            DWORD_PTR res = 0;
            for (int i = 0; i < 10; ++i) SendMessageTimeoutW(ed, WM_COMMAND, 2, 0, SMTO_ABORTIFHUNG, 3000, &res);        // zoom in x10
            PostMessageW(ed, WM_CLOSE, 0, 0);                                                                            // nothing drawn: closes without a question
            DWORD end = GetTickCount() + WMS(8000);
            while (IsWindow(ed) && (int)(end - GetTickCount()) > 0) Sleep(100);
            Check(!IsWindow(ed) && AppAlive(a), L"program oddities: the editor closes at once when nothing was changed");
        }
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
            if (!strncmp(t, "http://", 7) || !strncmp(t, "https://", 8)) { ++problems; char u[80]; strncpy(u, t, 79); u[79] = 0; for (char* c = u; *c; ++c) if ((unsigned char)*c < 32) *c = 0; Rep(L"INFO", L"  %S contains the address %S", nm, u); }
        }
        // wide strings too
        for (DWORD i = 0; i + 20 < n; i += 2) {
            const wchar_t* w = (const wchar_t*)(d + i);
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
enum { SU_TITLE = 100, SU_SUB, SU_STATE, SU_DESKTOP, SU_START, SU_AUTO, SU_LOG, SU_INSTALL, SU_REMOVE, SU_LAUNCH, SU_CLOSE, SU_LANGLBL, SU_LANG };

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
        struct { int id; Str s; } t[] = { {SU_DESKTOP, S_SU_DESKTOP}, {SU_START, S_SU_STARTMENU}, {SU_AUTO, S_SU_AUTOSTART}, {SU_REMOVE, S_SU_REMOVE}, {SU_LAUNCH, S_SU_LAUNCH}, {SU_CLOSE, S_SU_CLOSE}, {SU_LANGLBL, S_SU_LANG} };
        for (auto& i : t) { WinText(GetDlgItem(w, i.id), got, 500); if (wcscmp(got, T(i.s))) { ++bad; Rep(L"INFO", L"  language %d control %d: [%s] expected [%s]", lng, i.id, got, T(i.s)); } }
        WinText(GetDlgItem(w, SU_TITLE), got, 500); if (wcscmp(got, T(S_APP))) ++bad;
        _snwprintf(want, 500, L"%s \x2014 %s", ver, T(S_SU_SUB));
        WinText(GetDlgItem(w, SU_SUB), got, 500); if (wcscmp(got, want)) { ++bad; Rep(L"INFO", L"  subtitle [%s] expected [%s]", got, want); }
        WinText(GetDlgItem(w, SU_INSTALL), got, 500);
        if (wcscmp(got, T(S_SU_INSTALL)) && wcscmp(got, T(S_SU_UPDATE))) { ++bad; Rep(L"INFO", L"  install button [%s]", got); }
    }
    Check(bad == 0, L"installer window: every language switched in the combo shows that language everywhere (8 random)");
    LangSelect(0);
    // not installed -> Remove and Launch are disabled; the state line says so
    wchar_t st[300]; WinText(GetDlgItem(w, SU_STATE), st, 300);
    bool installed = wcsstr(st, L"1.0") != nullptr;
    LangSelect(R(1, LangCount())); SendMessageW(combo, CB_SETCURSEL, 0, 0); SendMessageW(w, WM_COMMAND, MAKEWPARAM(SU_LANG, CBN_SELCHANGE), (LPARAM)combo); LangSelect(1);
    if (!installed) Check(!IsWindowEnabled(GetDlgItem(w, SU_REMOVE)) && !IsWindowEnabled(GetDlgItem(w, SU_LAUNCH)), L"installer window: with nothing installed Remove and Launch are disabled");
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
    PostMessageW(w, WM_COMMAND, SU_INSTALL, 0);
    DWORD end = GetTickCount() + WMS(60000);
    while (!IsWindowEnabled(GetDlgItem(w, SU_REMOVE)) && (int)(end - GetTickCount()) > 0) Pump(100);
    Pump(500);
    Check(IsWindowEnabled(GetDlgItem(w, SU_REMOVE)) && IsWindowEnabled(GetDlgItem(w, SU_LAUNCH)), L"installer window: Install finishes and enables Remove / Launch");
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

// ==== main =====================================================================================================
struct TestDef { const wchar_t* name; void (*fn)(); int weight; };

int wmain(int argc, wchar_t** argv)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppLogOpen(L"autotest");
    int minutes = 3;
    uint64_t seed = 0;
    bool keep = false;
    const wchar_t* only = nullptr;
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
        { L"pickers", T_Pickers, 3 }, { L"editor", T_Editor, 2 }, { L"recorder", T_Recorder, 3 }, { L"scheduler", T_RecorderSched, 3 },
        { L"lifecycle", T_RecorderLifecycle, 2 }, { L"hotkeys", T_Hotkeys, 1 }, { L"installer", T_Installer, 1 },
        { L"fuzz-registry", T_FuzzSettings, 2 }, { L"fuzz-ini", T_FuzzIni, 2 }, { L"long-paths", T_LongPaths, 2 }, { L"threads", T_Threads, 2 },
        { L"leaks", T_Leaks, 2 }, { L"long-recording", T_LongRecord, 1 }, { L"program", T_App, 5 },
        { L"no-network", T_NoNetwork, 1 }, { L"setup-window", T_SetupGui, 2 }, { L"setup-install-window", T_SetupGuiInstall, 1 }, { L"program-picks", T_AppPicks, 3 }, { L"program-oddities", T_AppOddities, 3 }, { L"monkey", T_Monkey, 4 },
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
        if ((!wcscmp(tests[idx].name, L"installer") || !wcscmp(tests[idx].name, L"setup-install-window")) && round > n) continue;       // the installer cycle runs once
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
