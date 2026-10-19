// "Click2Screen.exe /selftest <folder>": checks the pieces that need no window - size and bitrate rules, the file name
// template, screenshots in the three formats, and a short recording with and without a pause. The result is written to
// <folder>\selftest.log; the exit code is the number of failed checks.
#include "c2s.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

static FILE* g_log;
static int g_fail;

static void Out(const wchar_t* fmt, ...)
{
    wchar_t line[700];
    va_list ap; va_start(ap, fmt); _vsnwprintf(line, 690, fmt, ap); va_end(ap);
    line[690] = 0;
    if (g_log) { fputws(line, g_log); fputws(L"\n", g_log); fflush(g_log); }
}
static void Check(bool ok, const wchar_t* name, const wchar_t* detail = L"")
{
    Out(L"%s  %s  %s", ok ? L"PASS" : L"FAIL", name, detail);
    if (!ok) ++g_fail;
}
static bool NearBps(int got, double want) { return got > want * 0.995 && got < want * 1.005; }

static bool HasMagic(const wchar_t* path, const void* magic, int n, int off)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[16]; DWORD got = 0;
    SetFilePointer(h, off, nullptr, FILE_BEGIN);
    bool ok = ReadFile(h, buf, n, &got, nullptr) && (int)got == n && memcmp(buf, magic, n) == 0;
    CloseHandle(h);
    return ok;
}
static unsigned long long FileSize(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    return ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

int RunSelfTest(const wchar_t* outDir)
{
    wchar_t logPath[MAX_PATH];
    CreateDirectoryW(outDir, nullptr);
    _snwprintf(logPath, MAX_PATH, L"%s\\selftest.log", outDir);
    g_log = _wfopen(logPath, L"w, ccs=UTF-8");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LangSelect(1);
    Out(L"Click2Screen %s self-test", C2S_VERSION_STR);

    // sizes and bitrates
    int w, h;
    FitSize(3840, 2160, w, h); Check(w == 1920 && h == 1080, L"FitSize 3840x2160 -> 1920x1080");
    FitSize(101, 51, w, h);    Check(w == 102 && h == 52, L"FitSize 101x51 -> even 102x52");
    FitSize(1, 1, w, h);       Check(w == 2 && h == 2, L"FitSize 1x1 -> 2x2");
    FitSize(3440, 1440, w, h); Check(w == 1920 && h == 804, L"FitSize 3440x1440 keeps the aspect");
    Check(AutoBitrate(1920, 1080) == 4000000 && AutoBitrate(3000, 2000) == 4000000, L"AutoBitrate: 1920x1080 and above = 4 Mbit/s");
    Check(AutoBitrate(1280, 720) == 2000000 && AutoBitrate(854, 480) == 1500000 && AutoBitrate(640, 360) == 768000 && AutoBitrate(426, 240) == 384000, L"AutoBitrate: the table values at the anchors");
    Check(NearBps(AutoBitrate(400, 200), 384000.0 * 400 * 200 / (426.0 * 240)), L"AutoBitrate: proportional to the area below 426x240");
    Check(AutoBitrate(1000, 600) > 1500000 && AutoBitrate(1000, 600) < 2000000, L"AutoBitrate: between the anchors it is interpolated");

    // file name template
    Check(TemplateValid(L"File $D.$M.$Y $H-$N-$S"), L"template: date and time");
    Check(TemplateValid(L"Img####"), L"template: Img####");
    Check(TemplateValid(L"%USERNAME%_#"), L"template: user name and number");
    Check(!TemplateValid(L"a#b#"), L"template: two # runs rejected");
    Check(!TemplateValid(L"a/b"), L"template: path separator rejected");
    Check(!TemplateValid(L""), L"template: empty rejected");

    wchar_t dir[MAX_PATH], path[MAX_PATH], err[300];
    _snwprintf(dir, MAX_PATH, L"%s\\files", outDir);
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Click2Screen", 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"SelfTestCounter"); RegCloseKey(k); }
    }
    bool ok = BuildOutputPath(dir, L"Img####", L"png", L"SelfTestCounter", path, MAX_PATH, err, 300);
    Check(ok && wcsstr(path, L"Img0001.png"), L"BuildOutputPath: first number", path);
    if (ok) { HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr); if (f != INVALID_HANDLE_VALUE) CloseHandle(f); }
    ok = BuildOutputPath(dir, L"Img####", L"png", L"SelfTestCounter", path, MAX_PATH, err, 300);
    Check(ok && wcsstr(path, L"Img0002.png"), L"BuildOutputPath: next number", path);
    ok = BuildOutputPath(dir, L"File $D.$M.$Y $H-$N-$S", L"mp4", nullptr, path, MAX_PATH, err, 300);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t want[32]; _snwprintf(want, 32, L"%04d-%02d-%02d at ", st.wYear, st.wMonth, st.wDay);
    Check(ok && wcsstr(path, want), L"BuildOutputPath: date template", path);

    // screenshots
    RECT rc = { 0, 0, 320, 240 };
    HBITMAP bmp = CaptureRect(rc, true, true);
    Check(bmp != nullptr, L"CaptureRect 320x240");
    if (bmp) {
        const wchar_t* ext[] = { L"png", L"jpg", L"bmp" };
        static const unsigned char magic[3][4] = { { 0x89, 'P', 'N', 'G' }, { 0xFF, 0xD8, 0xFF, 0 }, { 'B', 'M', 0, 0 } };
        const int mlen[3] = { 4, 3, 2 };
        for (int i = 0; i < 3; ++i) {
            _snwprintf(path, MAX_PATH, L"%s\\shot.%s", outDir, ext[i]);
            bool s = SaveBitmap(bmp, path, i);
            Check(s && FileSize(path) > 100 && HasMagic(path, magic[i], mlen[i], 0), L"SaveBitmap", ext[i]);
        }
        DeleteObject(bmp);
    }

    // recording
    Settings cfg; SettingsDefaults(cfg);
    cfg.mic = false; cfg.fps = 20;
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        if (wine) { Out(L"SKIP  recording with the Media Foundation encoder: not available under Wine"); continue; }
        RecParams p; memset(&p, 0, sizeof(p));
        p.rc.left = 0; p.rc.top = 0; p.rc.right = 640; p.rc.bottom = 360;
        p.cfg = cfg;
        _snwprintf(p.path, MAX_PATH, L"%s\\rec%d.mp4", outDir, pass);
        DeleteFileW(p.path);
        Recorder rec;
        err[0] = 0;
        bool started = rec.Start(p, err, 300);
        Check(started, pass ? L"Recorder.Start (with pause)" : L"Recorder.Start", err);
        if (!started) continue;
        Sleep(1200);
        if (pass) { rec.SetPaused(true); Sleep(1500); rec.SetPaused(false); Sleep(800); }
        else Sleep(1300);
        double el = rec.ElapsedSec();
        bool done = rec.Stop();
        wchar_t d[100]; _snwprintf(d, 100, L"elapsed %.2fs, file %llu bytes", el, FileSize(p.path));
        Check(done && !rec.Failed(), L"Recorder.Stop finalizes the file", d);
        Check(FileSize(p.path) > 2000 && HasMagic(p.path, "ftyp", 4, 4), L"MP4 container (ftyp box)", d);
        if (pass) Check(el > 1.6 && el < 2.6, L"pause is excluded from the recording time", d);
        else Check(el > 2.0 && el < 3.2, L"recording time", d);
    }

    // Frame scheduling and pause handling without an encoder (the encoder itself needs real Windows Media Foundation).
    {
        SetEnvironmentVariableW(L"C2S_NULLSINK", L"1");
        RecParams p; memset(&p, 0, sizeof(p));
        p.rc.right = 640; p.rc.bottom = 360; p.cfg = cfg; p.cfg.fps = 20;
        _snwprintf(p.path, MAX_PATH, L"%s\null.mp4", outDir);
        Recorder rec;
        bool started = rec.Start(p, err, 300);
        Check(started, L"scheduler: start without encoder");
        if (started) {
            Sleep(1000); rec.SetPaused(true); Check(rec.Paused(), L"scheduler: paused"); Sleep(1000); rec.SetPaused(false); Sleep(1000);
            double el = rec.ElapsedSec();
            rec.Stop();
            unsigned long long fr = rec.FramesWritten();
            wchar_t d[100]; _snwprintf(d, 100, L"elapsed %.2fs, frames %llu (expected about 40)", el, fr);
            Check(el > 1.8 && el < 2.3, L"scheduler: media time excludes the pause", d);
            Check(fr >= 30 && fr <= 48, L"scheduler: frame count follows the frame rate, none during the pause", d);
        }
        SetEnvironmentVariableW(L"C2S_NULLSINK", nullptr);
    }

    Out(L"%d check(s) failed", g_fail);
    if (g_log) fclose(g_log);
    return g_fail;
}
