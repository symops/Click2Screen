// Click2Screen — main window, recording panel, tray icon, global hotkeys and screenshot / recording flows.
#include "c2s.h"
#include "uiids.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include <mmsystem.h>

// ---------------------------------------------------------------------------------------------------------------
namespace {

HINSTANCE g_inst;
HWND   g_main, g_panel, g_frame, g_tip;
bool   g_modeDesktop;             // the recording covers the whole desktop: no frame, the panel sits at the bottom
HFONT  g_font, g_fontBold;
HICON  g_icon, g_iconSmall;
Settings g_cfg;
State  g_state = ST_IDLE;
Recorder* g_recs[16];          // one recorder per recorded area: the whole desktop gives one file for each monitor
int    g_nrecs;
wchar_t g_outPaths[16][MAX_PATH];
RecParams g_target;
wchar_t g_outPath[MAX_PATH];
int    g_countdown;
bool   g_trayAdded, g_hideAsked, g_mainWasVisible = true, g_busyShot;
UINT   g_msgTaskbar;
NOTIFYICONDATAW g_nid;
const wchar_t* kMainClass = L"Click2ScreenMain";

void FrameShow(const RECT& rc);
void FrameHide();
void FramePlace(const RECT& rc);
void FrameBlink();
void CountdownShow(int n, const RECT* areas, int count);
void CountdownHide();
int RecordAreas(RECT* out);
int MonitorRects(RECT* out, int cap);
RECT TargetRect();
void RegisterOverlayClasses();

UINT g_dpi = 96;                 // DPI of the monitor the main window is on (WM_DPICHANGED keeps it current)
int Sc(int v) { return MulDiv(v, (int)g_dpi, 96); }

void MakeFonts()
{
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_BOLD; lf.lfQuality = ANTIALIASED_QUALITY;
    g_fontBold = CreateFontIndirectW(&lf);
}

HWND MakeCtl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    return c;
}

void SetStatus(const wchar_t* text)
{
    if (g_main) SetWindowTextW(GetDlgItem(g_main, IDC_STATUS), text);
}

// The only sound of the original is a short click (about 8.3 kHz, dying away in some 20 ms) on every second of the delay.
// It is made here, as 44.1 kHz stereo PCM in memory (no sound files).
const int kTickFrames = 6208;
unsigned char g_tickWav[44 + kTickFrames * 4];
bool g_tickBuilt;
void BuildTick()
{
    unsigned char* w = g_tickWav;
    unsigned dataBytes = kTickFrames * 4;
    memcpy(w, "RIFF", 4); unsigned v = 36 + dataBytes; memcpy(w + 4, &v, 4); memcpy(w + 8, "WAVEfmt ", 8);
    v = 16; memcpy(w + 16, &v, 4);
    unsigned short us = 1; memcpy(w + 20, &us, 2); us = 2; memcpy(w + 22, &us, 2);
    v = 44100; memcpy(w + 24, &v, 4); v = 44100 * 4; memcpy(w + 28, &v, 4);
    us = 4; memcpy(w + 32, &us, 2); us = 16; memcpy(w + 34, &us, 2);
    memcpy(w + 36, "data", 4); memcpy(w + 40, &dataBytes, 4);
    short* s = (short*)(w + 44);
    for (int i = 0; i < kTickFrames; ++i) {
        double t = i / 44100.0 - 0.0100;                         // the click starts 10 ms in
        double a = t < 0 ? 0 : 30000.0 * exp(-t / 0.0055) * sin(6.283185307179586 * 8318.0 * t);
        s[2 * i] = s[2 * i + 1] = (short)a;
    }
    g_tickBuilt = true;
}
void Beep(bool tick)
{
    if (!g_cfg.sounds || !tick) return;                          // only the countdown ticks (as the original)
    if (!g_tickBuilt) BuildTick();
    PlaySoundW((LPCWSTR)g_tickWav, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void Balloon(const wchar_t* title, const wchar_t* text)
{
    if (!g_trayAdded) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy(n.szInfoTitle, title, 63); n.szInfoTitle[63] = 0;
    wcsncpy(n.szInfo, text, 255); n.szInfo[255] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void Warn(const wchar_t* text)
{
    MessageBoxW(g_main && IsWindowVisible(g_main) ? g_main : nullptr, text, T(S_APP), MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

// Shell_NotifyIcon talks to the shell and can block when the shell is busy: on exit it must not hold the program.
static DWORD WINAPI TrayRemoveThread(LPVOID p) { Shell_NotifyIconW(NIM_DELETE, (NOTIFYICONDATAW*)p); return 0; }
static void TrayRemove(NOTIFYICONDATAW* n)
{
    HANDLE t = CreateThread(nullptr, 0, TrayRemoveThread, n, 0, nullptr);
    if (t) { if (WaitForSingleObject(t, 3000) != WAIT_OBJECT_0) Log(L"exit: the shell did not answer, the tray icon is left to it"); CloseHandle(t); }
}

// ---- tray ----------------------------------------------------------------------------------------------------------
void TrayTip()
{
    if (!g_trayAdded) return;
    const wchar_t* s = T(S_APP);
    if (g_state == ST_RECORDING) s = T(S_RECORDING);
    else if (g_state == ST_PAUSED) s = T(S_PAUSED);
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_TIP;
    wcsncpy(n.szTip, s, 127); n.szTip[127] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayAdd()
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_main; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_C2S_TRAY; g_nid.hIcon = g_iconSmall;
    wcsncpy(g_nid.szTip, T(S_APP), 127);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

// ---- panel -----------------------------------------------------------------------------------------------------
void ShowMain(bool show)
{
    if (!g_main) return;
    if (show) { ShowWindow(g_main, SW_SHOW); ShowWindow(g_main, SW_RESTORE); SetForegroundWindow(g_main); }
    else ShowWindow(g_main, SW_HIDE);
}

// ---- the look: the original's minimal one (a dark caption, one row of flat icons, a dark strip while recording) ----
const COLORREF kCapTop = RGB(70, 80, 101), kCapBot = RGB(39, 45, 56), kBody = RGB(238, 250, 255), kBlue = RGB(8, 115, 197),
               kGlyph = RGB(54, 62, 78), kStripTop = RGB(40, 45, 57), kStripBot = RGB(71, 80, 101), kStripSep = RGB(82, 91, 108),
               kStripInk = RGB(177, 180, 187), kRecRed = RGB(203, 62, 58);
const int kMainW = 206, kMainH = 62, kCapH = 28, kStripW = 230, kStripH = 29, kFrameW = 3;

void Box(HDC dc, int x, int y, int w, int h, COLORREF c)       // a rectangle in 96-dpi units at the pixel origin (ox, oy) set by the caller
{
    RECT r = { x, y, x + w, y + h };
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
}

struct Glyph { HDC dc; int ox, oy; };                           // logical (96 dpi) coordinates relative to (ox, oy), scaled to the DPI
void GBox(const Glyph& g, int x, int y, int w, int h, COLORREF c)
{
    Box(g.dc, g.ox + Sc(x), g.oy + Sc(y), Sc(x + w) - Sc(x), Sc(y + h) - Sc(y), c);
}

// The three icons are 16 x 16 grids of 1-unit strokes (a dashed square for the region, a window inside the dashes for a
// window, a window with a title bar for the desktop); a '#' is a set unit, scaled to the DPI.
const char* const kIconRegion[16] = {
    "##### #### #####", "#              #", "#              #", "#              #", "                ", "#              #", "#              #", "#              #",
    "#              #", "#              #", "#              #", "                ", "#              #", "#              #", "#              #", "##### #### #####" };
const char* const kIconWindow[16] = {
    "##### #### #####", "#              #", "#              #", "#              #", "   ##########   ", "#  ##########  #", "#  #        #  #", "#  #        #  #",
    "#  #        #  #", "#  #        #  #", "#  #        #  #", "   ##########   ", "#              #", "#              #", "#              #", "##### #### #####" };
const char* const kIconDesktop[16] = {
    " ############## ", "######### # # ##", "################", "#              #", "#              #", "#  ##  ##  ##  #", "#  #        #  #", "#              #",
    "#  #        #  #", "#  #        #  #", "#              #", "#  #        #  #", "#  ##  ##  ##  #", "#              #", "##            ##", " ############## " };

void DrawGrid(const Glyph& g, const char* const* rows, COLORREF c)
{
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            if (rows[y][x] == '#') GBox(g, x, y, 1, 1, c);
}

void DrawShotIcon(const Glyph& g, int id, COLORREF c)
{
    if (id == IDC_SHOT_REGION || id == IDC_REC_REGION) DrawGrid(g, kIconRegion, c);
    else if (id == IDC_SHOT_WINDOW || id == IDC_REC_WINDOW) DrawGrid(g, kIconWindow, c);
    else DrawGrid(g, kIconDesktop, c);
}

void DrawMainItem(const DRAWITEMSTRUCT* d)
{
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    bool down = (d->itemState & ODS_SELECTED) != 0;
    int id = (int)d->CtlID;
    if (id == IDC_MIN || id == IDC_CLOSE) {
        int cap = (int)(rc.top + 0);
        (void)cap;
        for (int y = 0; y < h; ++y) {                                        // the caption gradient continues behind the buttons
            int t = y * 255 / (h > 1 ? h - 1 : 1);
            COLORREF c = RGB(GetRValue(kCapTop) + (GetRValue(kCapBot) - GetRValue(kCapTop)) * t / 255,
                             GetGValue(kCapTop) + (GetGValue(kCapBot) - GetGValue(kCapTop)) * t / 255,
                             GetBValue(kCapTop) + (GetBValue(kCapBot) - GetBValue(kCapTop)) * t / 255);
            if (down) c = RGB(GetRValue(c) / 2, GetGValue(c) / 2, GetBValue(c) / 2);
            Box(dc, rc.left, rc.top + y, w, 1, c);
        }
        HPEN pen = CreatePen(PS_SOLID, Sc(1) > 1 ? Sc(1) : 1, RGB(235, 238, 243));
        HPEN old = (HPEN)SelectObject(dc, pen);
        int cx = rc.left + w / 2, cy = rc.top + h / 2;
        if (id == IDC_MIN) { MoveToEx(dc, cx - Sc(5), rc.top + Sc(19), nullptr); LineTo(dc, cx + Sc(5), rc.top + Sc(19)); }
        else {
            MoveToEx(dc, cx - Sc(5), cy - Sc(5), nullptr); LineTo(dc, cx + Sc(5) + 1, cy + Sc(5) + 1);
            MoveToEx(dc, cx + Sc(5), cy - Sc(5), nullptr); LineTo(dc, cx - Sc(5) - 1, cy + Sc(5) + 1);
        }
        SelectObject(dc, old); DeleteObject(pen);
    } else {
        Box(dc, rc.left, rc.top, w, h, down ? RGB(205, 228, 245) : kBody);
        if (id == IDC_MENU) {
            Glyph g = { dc, rc.left + (w - Sc(12)) / 2, rc.top + (h - Sc(10)) / 2 };
            for (int y = 0; y < 3; ++y) GBox(g, 0, y * 4, 12, 2, kGlyph);
        } else {
            bool rec = id == IDC_REC_DESKTOP || id == IDC_REC_REGION || id == IDC_REC_WINDOW;
            Glyph g = { dc, rc.left + (w - Sc(16)) / 2, rc.top + (h - Sc(16)) / 2 };
            DrawShotIcon(g, id, rec || id == IDC_SHOT_REGION ? kBlue : RGB(0, 0, 0));   // (as the original: the region screenshot icon is blue too)
        }
    }
    if (d->itemState & ODS_FOCUS) { RECT f = rc; InflateRect(&f, -1, -1); DrawFocusRect(dc, &f); }
}

void FillStrip(HDC dc, int x, int w)                            // the vertical gradient of the recording strip
{
    int h = Sc(kStripH);
    for (int y = 0; y < h; ++y) {
        int t = y * 255 / (h > 1 ? h - 1 : 1);
        Box(dc, x, y, w, 1, RGB(GetRValue(kStripTop) + (GetRValue(kStripBot) - GetRValue(kStripTop)) * t / 255,
                                GetGValue(kStripTop) + (GetGValue(kStripBot) - GetGValue(kStripTop)) * t / 255,
                                GetBValue(kStripTop) + (GetBValue(kStripBot) - GetBValue(kStripTop)) * t / 255));
    }
}

void DrawPanelItem(const DRAWITEMSTRUCT* d)
{
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    FillStrip(dc, rc.left, rc.right - rc.left);
    int id = (int)d->CtlID;
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    if (id == IDC_P_START || id == IDC_P_STOP) {
        bool enabled = (d->itemState & ODS_DISABLED) == 0;
        COLORREF ink = enabled ? kStripInk : RGB(110, 115, 125);
        int r = Sc(5);
        if (id == IDC_P_STOP) Box(dc, cx - r, cy - r, 2 * r, 2 * r, ink);
        else if (g_state == ST_RECORDING) { Box(dc, cx - r, cy - r, Sc(3), 2 * r, ink); Box(dc, cx + r - Sc(3), cy - r, Sc(3), 2 * r, ink); }
        else {
            HBRUSH b = CreateSolidBrush(enabled ? kRecRed : RGB(110, 115, 125)); HPEN pn = CreatePen(PS_SOLID, 1, enabled ? kRecRed : RGB(110, 115, 125));
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pn);
            Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pn);
        }
        return;
    }
    wchar_t text[100]; GetWindowTextW(d->hwndItem, text, 100);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(253, 253, 253));
    HGDIOBJ of = SelectObject(dc, g_fontBold);
    RECT t = rc;
    DrawTextW(dc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void TimeText(int t, bool minus, wchar_t* out)
{
    if (t >= 3600) _snwprintf(out, 32, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else _snwprintf(out, 32, L"%s%02d:%02d", minus ? L"-" : L"", t / 60, t % 60);
}

void SetCell(HWND parent, int id, const wchar_t* text)
{
    HWND c = GetDlgItem(parent, id);
    wchar_t old[100] = L"";
    GetWindowTextW(c, old, 100);
    if (wcscmp(old, text) != 0) { SetWindowTextW(c, text); InvalidateRect(c, nullptr, FALSE); }
}

void UpdatePanel()
{
    if (!g_panel) return;
    double sec = g_nrecs ? g_recs[0]->ElapsedSec() : 0.0;
    wchar_t buf[96];
    if (g_state == ST_COUNTDOWN) _snwprintf(buf, 96, L"%d", g_countdown);          // 3 - 2 - 1, a tick every second
    else TimeText((int)sec, false, buf);
    SetCell(g_panel, IDC_P_TIME, buf);
    double mb = 0.0;
    if (g_nrecs && g_state != ST_ARMED && g_state != ST_COUNTDOWN) { uint64_t bytes = 0; for (int i = 0; i < g_nrecs; ++i) bytes += g_recs[i]->FileBytes(); mb = bytes / 1048576.0; }
    _snwprintf(buf, 96, L"%d Mb", (int)mb);
    SetCell(g_panel, IDC_P_SIZE, buf);
    const wchar_t* b = T(S_START);
    if (g_state == ST_COUNTDOWN) { _snwprintf(buf, 96, T(S_STARTING_IN), g_countdown); b = buf; }
    else if (g_state == ST_RECORDING) b = T(S_PAUSE);
    else if (g_state == ST_PAUSED) b = T(S_RESUME);
    SetCell(g_panel, IDC_P_START, b);
    EnableWindow(GetDlgItem(g_panel, IDC_P_START), g_state != ST_COUNTDOWN);
    EnableWindow(GetDlgItem(g_panel, IDC_P_STOP), g_state == ST_RECORDING || g_state == ST_PAUSED || g_state == ST_ARMED || g_state == ST_COUNTDOWN);
    _snwprintf(buf, 96, L"F11: %s", T(S_STOP));
    SetCell(g_panel, IDC_P_HINT, buf);
    TrayTip();
}

void FinishRecording(bool ok, bool userStop);

void DeleteRecs()
{
    for (int i = 0; i < g_nrecs; ++i) { delete g_recs[i]; g_recs[i] = nullptr; }
    g_nrecs = 0;
}

void BeginRecord()
{
    Log(L"record: starting");
    wchar_t err[300] = L"";
    g_target.cfg = g_cfg;
    if (!VideoFormatAvailable(g_cfg.videoCodec)) g_target.cfg.videoCodec = VC_H264_BEST;   // e.g. HEVC chosen on another computer
    // The whole desktop is one file for each monitor (like the screenshots); a region or a window is one file.
    RECT areas[16]; int n = 1;
    if (g_modeDesktop) { n = MonitorRects(areas, 16); if (n < 1) { areas[0] = g_target.rc; n = 1; } }
    else areas[0] = g_target.rc;
    DeleteRecs();
    for (int i = 0; i < n; ++i) {
        if (!BuildOutputPath(g_cfg.vidFolder, g_cfg.fileName, VideoFormatExt(g_target.cfg.videoCodec), L"VidCounter", g_outPaths[i], MAX_PATH, err, 300)) {
            Warn(err); break;
        }
        RecParams rp = g_target;
        rp.rc = areas[i];
        if (i > 0) rp.cfg.mic = false;                           // one microphone: its sound goes into the first file
        wcscpy(rp.path, g_outPaths[i]);
        Recorder* r = new Recorder;
        wchar_t e2[300] = L"";
        if (!r->Start(rp, e2, 300)) {
            Warn(e2[0] ? e2 : T(S_ERR_CAPTURE));
            delete r;
            break;
        }
        if (e2[0]) Warn(e2);                                    // the microphone was not available
        g_recs[g_nrecs++] = r;
        Log(L"record: started, %dx%d -> %s", r->Width(), r->Height(), g_outPaths[i]);
    }
    if (g_nrecs != n) {                                          // one of them did not start: none is recorded
        for (int i = 0; i < g_nrecs; ++i) { g_recs[i]->Stop(); DeleteFileW(g_outPaths[i]); }
        DeleteRecs();
        g_state = ST_ARMED; UpdatePanel(); return;
    }
    wcscpy(g_outPath, g_outPaths[n - 1]);
    g_state = ST_RECORDING;
    Beep(false);
    SetTimer(g_panel, TIMER_UI, 250, nullptr);
    CountdownHide();
    FxStart();                                                   // click waves and the Shift trail
    UpdatePanel();
}

void ClosePanel()
{
    if (g_panel) {
        KillTimer(g_panel, TIMER_UI); KillTimer(g_panel, TIMER_COUNTDOWN);
        DestroyWindow(g_panel); g_panel = nullptr;
    }
    FrameHide();
    CountdownHide();
    FxStop();
    UnregisterHotKey(g_main, HK_STOP);
}

void FinishRecording(bool ok, bool userStop)
{
    (void)userStop;
    int n = g_nrecs, savedCount = 0;
    bool saved[16] = {};
    for (int i = 0; i < n; ++i) {
        if (g_recs[i]->Active()) saved[i] = g_recs[i]->Stop();
        if (ok && saved[i]) ++savedCount;
    }
    DeleteRecs();
    Log(L"record: finished (saved %d of %d) %s", savedCount, n, g_outPath);
    g_state = ST_IDLE;
    ClosePanel();
    TrayTip();
    const wchar_t* last = nullptr;
    for (int i = 0; i < n; ++i) {
        if (ok && saved[i]) last = g_outPaths[i];
        else if (ok && g_outPaths[i][0]) {
            wchar_t msg[MAX_PATH + 100];
            _snwprintf(msg, MAX_PATH + 100, T(S_ERR_SAVE), g_outPaths[i]);
            Warn(msg);
        }
    }
    if (last) {
        Beep(false);
        wchar_t msg[MAX_PATH + 40];
        _snwprintf(msg, MAX_PATH + 40, T(S_SAVED), last);
        SetStatus(msg);
        Balloon(T(S_APP), msg);
        if (g_cfg.openFolder) ShowInExplorer(last);
    }
    if (g_mainWasVisible) ShowMain(true);
}

void StopRecording(bool userStop)
{
    if (g_state == ST_IDLE) return;
    if (g_state == ST_ARMED || g_state == ST_COUNTDOWN) {       // nothing recorded yet
        DeleteRecs();
        g_state = ST_IDLE; ClosePanel(); TrayTip();
        if (g_mainWasVisible) ShowMain(true);
        return;
    }
    FinishRecording(true, userStop);
}

void StartPressed()
{
    switch (g_state) {
    case ST_ARMED:
        if (g_cfg.delaySec > 0) {
            g_state = ST_COUNTDOWN; g_countdown = g_cfg.delaySec;
            SetTimer(g_panel, TIMER_COUNTDOWN, 1000, nullptr);
            Beep(true);
            { RECT ar[16]; int an = RecordAreas(ar); CountdownShow(g_countdown, ar, an); }
            UpdatePanel();
        } else BeginRecord();
        break;
    case ST_RECORDING: for (int i = 0; i < g_nrecs; ++i) g_recs[i]->SetPaused(true); g_state = ST_PAUSED; UpdatePanel(); break;
    case ST_PAUSED:    for (int i = 0; i < g_nrecs; ++i) g_recs[i]->SetPaused(false); g_state = ST_RECORDING; UpdatePanel(); break;
    default: break;
    }
}

void HidePanel()
{
    if (!g_panel) return;
    if (!g_hideAsked) {
        if (MessageBoxW(g_panel, T(S_HIDE_ASK), T(S_APP), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        g_hideAsked = true;
    }
    ShowWindow(g_panel, SW_HIDE);
    Balloon(T(S_APP), T(S_TRAY_HIDDEN));
}

void ExcludeFromCapture(HWND w)
{
    // The panel and the frame must not appear in the recording (Windows 10 2004 and later; the frame lies outside the
    // recorded rectangle anyway).
    typedef BOOL(WINAPI* SetAff)(HWND, DWORD);
    SetAff aff = (SetAff)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
    if (aff) aff(w, 0x11 /*WDA_EXCLUDEFROMCAPTURE*/);
}

// The recorded rectangle now (a followed window moves).
RECT TargetRect()
{
    RECT rc = g_target.rc, r;
    if (g_target.hwnd && IsWindow(g_target.hwnd) && WindowVisibleRect(g_target.hwnd, &r)) rc = r;
    return rc;
}

// The frame lies just outside the recorded rectangle; the strip sits below it, centred (above it or inside at the
// bottom of the screen). For the whole desktop there is no frame and the strip sits at the bottom of the main screen.
void PlaceOverlay()
{
    if (!g_panel) return;
    RECT rc = TargetRect();
    int fw = Sc(kFrameW);
    if (g_frame) FramePlace(rc);
    int w = Sc(kStripW), h = Sc(kStripH);
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi); GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int x, y;
    if (g_modeDesktop) {
        POINT origin = { 0, 0 };
        MONITORINFO pm; pm.cbSize = sizeof(pm); GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm);
        wa = pm.rcWork;
        x = (wa.left + wa.right - w) / 2; y = wa.bottom - h - Sc(20);
    } else {
        x = (rc.left + rc.right - w) / 2 - 1; y = rc.bottom + fw;
        if (y + h > wa.bottom) y = rc.top - fw - h;                 // no room below: above the frame
        if (y < wa.top) y = rc.bottom - h - Sc(10);                 // nor above: inside, at the bottom
    }
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void RoundWindow(HWND w, int wpx, int hpx, int radius)         // the lower corners are round, the upper ones nearly square (as the original)
{
    HRGN low = CreateRoundRectRgn(0, 0, wpx + 1, hpx + 1, radius, radius), top = CreateRectRgn(0, 0, wpx, hpx / 2);
    CombineRgn(low, low, top, RGN_OR);
    DeleteObject(top);
    SetWindowRgn(w, low, TRUE);
}

LRESULT CALLBACK PanelProc(HWND hw, UINT m, WPARAM wp, LPARAM lp);
LRESULT CALLBACK CountProc(HWND hw, UINT m, WPARAM wp, LPARAM lp);
void RegisterOverlayClasses();
void FrameShow(const RECT& rc);
void FrameHide();
void FramePlace(const RECT& rc);
void CountdownHide();
int MonitorRects(RECT* out, int cap);

LRESULT CALLBACK FrameProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: { RECT r; GetClientRect(hw, &r); Box((HDC)wp, 0, 0, r.right, r.bottom, RGB(255, 0, 0)); return 1; }
    case WM_NCCREATE: return DefWindowProcW(hw, m, wp, lp);
    }
    return DefWindowProcW(hw, m, wp, lp);
}

LRESULT CALLBACK PanelProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NCCREATE: return DefWindowProcW(hw, m, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        RECT r; GetClientRect(hw, &r);
        FillStrip(dc, 0, r.right);
        static const int seps[4] = { 47, 103, 168, 199 };
        for (int x : seps) { Box(dc, Sc(x), Sc(6), 1, Sc(kStripH) - Sc(12), kStripSep); }
        EndPaint(hw, &ps);
        return 0; }
    case WM_DRAWITEM: DrawPanelItem((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CONTEXTMENU: {
        HMENU mn = CreatePopupMenu();
        AppendMenuW(mn, MF_STRING, IDC_P_HIDE, T(S_HIDE));
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (lp == (LPARAM)-1) GetCursorPos(&pt);
        int cmd = TrackPopupMenu(mn, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hw, nullptr);
        DestroyMenu(mn);
        if (cmd == IDC_P_HIDE) PostMessageW(hw, WM_COMMAND, MAKEWPARAM(IDC_P_HIDE, 0), 0);
        return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_P_START: StartPressed(); break;
        case IDC_P_STOP: StopRecording(true); break;
        case IDC_P_HIDE: HidePanel(); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_COUNTDOWN) {
            if (--g_countdown <= 0) { KillTimer(hw, TIMER_COUNTDOWN); BeginRecord(); }
            else { Beep(true); { RECT ar[16]; int an = RecordAreas(ar); CountdownShow(g_countdown, ar, an); } UpdatePanel(); }
        } else if (wp == TIMER_UI) {
            for (int i = 0; i < g_nrecs; ++i) if (g_recs[i]->LimitReached() || g_recs[i]->Failed()) { FinishRecording(true, false); return 0; }
            if (g_target.hwnd) PlaceOverlay();                       // a followed window moves
            UpdatePanel();
        }
        return 0;
    case WM_CLOSE: HidePanel(); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

void ShowPanel()
{
    if (g_state == ST_IDLE) return;                 // no panel without a recording to control
    if (g_panel) { ShowWindow(g_panel, SW_SHOWNA); return; }
    RegisterOverlayClasses();
    int w = Sc(kStripW), h = Sc(kStripH);
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Click2ScreenPanel", T(S_PANEL_TITLE), WS_POPUP | WS_CLIPCHILDREN,
                              0, 0, w, h, nullptr, nullptr, g_inst, nullptr);
    ExcludeFromCapture(g_panel);
    RoundWindow(g_panel, w, h, Sc(8));
    // time | size | F11: Stop | start / pause | stop  (cells split at 47, 103, 168, 199 of 230 units)
    static const int cell[6] = { 0, 47, 103, 168, 199, 230 };
    static const int ids[5] = { IDC_P_TIME, IDC_P_SIZE, IDC_P_HINT, IDC_P_START, IDC_P_STOP };
    for (int i = 0; i < 5; ++i) {
        bool button = i >= 3;
        HWND c = CreateWindowExW(0, button ? L"BUTTON" : L"STATIC", L"", WS_CHILD | WS_VISIBLE | (button ? (BS_OWNERDRAW | WS_TABSTOP) : SS_OWNERDRAW),
                                 Sc(cell[i]) + 1, 0, Sc(cell[i + 1]) - Sc(cell[i]) - 1, h, g_panel, (HMENU)(INT_PTR)ids[i], g_inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    }
    if (!g_modeDesktop) FrameShow(TargetRect());
    UpdatePanel();
    PlaceOverlay();
    ShowWindow(g_panel, SW_SHOWNA);
}

// ---- recording ----------------------------------------------------------------------------------------------
enum Mode { M_DESKTOP, M_REGION, M_WINDOW };

bool ChooseTarget(Mode mode, RECT* rc, HWND* hw)
{
    *hw = nullptr;
    if (mode == M_DESKTOP) { *rc = VirtualScreen(); return true; }
    if (mode == M_REGION) return PickRegion(g_main, rc);
    return PickWindow(g_main, hw, rc);
}

void Arm(Mode mode)
{
    Log(L"arm: mode %d, state %d", (int)mode, (int)g_state);
    if (g_state != ST_IDLE) { Warn(T(S_ERR_ALREADY)); return; }
    g_mainWasVisible = g_main && IsWindowVisible(g_main);
    RECT rc; HWND hw;
    if (!ChooseTarget(mode, &rc, &hw)) { if (g_mainWasVisible) ShowMain(true); return; }
    if (rc.right - rc.left < 16 || rc.bottom - rc.top < 16) { Warn(T(S_ERR_REGION_SMALL)); if (g_mainWasVisible) ShowMain(true); return; }
    wchar_t err[300];
    if (!EnsureFolder(g_cfg.vidFolder, err, 300)) { Warn(err); if (g_mainWasVisible) ShowMain(true); return; }
    memset(&g_target, 0, sizeof(g_target));
    g_target.rc = rc; g_target.hwnd = hw;
    g_modeDesktop = mode == M_DESKTOP;
    g_outPath[0] = 0;
    ShowMain(false);
    g_state = ST_ARMED;
    if (!RegisterHotKey(g_main, HK_STOP, MOD_NOREPEAT, VK_F11)) { /* F11 taken: the panel button and the tray still work */ }
    ShowPanel();
    TrayTip();
}

// ---- screenshots ---------------------------------------------------------------------------------------------
void PumpFor(int ms)
{
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return; }   // not ours to swallow: the main loop must see it
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

// The countdown: big red digits (3, 2, 1) in the centre of the chosen area (of every monitor for the whole desktop).
// The windows are see-through and click-through, excluded from the picture, and gone before it is taken.
const COLORREF kDigitKey = RGB(1, 2, 3), kDigitRed = RGB(230, 30, 30);
HWND g_digit[16];
int g_nDigit;
LRESULT CALLBACK CountProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_NCCREATE) return DefWindowProcW(hw, m, wp, lp);
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        RECT r; GetClientRect(hw, &r);
        Box(dc, 0, 0, r.right, r.bottom, kDigitKey);
        wchar_t t[16]; GetWindowTextW(hw, t, 16);
        HFONT f = CreateFontW(-(int)(r.bottom * 0.82), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, kDigitRed);
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of); DeleteObject(f);
        EndPaint(hw, &ps);
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

void CountdownShow(int n, const RECT* areas, int count)
{
    RegisterOverlayClasses();
    wchar_t t[16]; _snwprintf(t, 16, L"%d", n);
    if (count > 16) count = 16;
    for (int i = 0; i < count; ++i) {
        int aw = areas[i].right - areas[i].left, ah = areas[i].bottom - areas[i].top;
        int sz = (aw < ah ? aw : ah) / 3;
        if (sz < Sc(70)) sz = Sc(70);
        if (sz > Sc(220)) sz = Sc(220);
        if (i >= g_nDigit) {
            g_digit[i] = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Click2ScreenCount", t, WS_POPUP,
                                         0, 0, sz, sz, nullptr, nullptr, g_inst, nullptr);
            if (!g_digit[i]) continue;
            SetLayeredWindowAttributes(g_digit[i], kDigitKey, 0, LWA_COLORKEY);
            ExcludeFromCapture(g_digit[i]);
            g_nDigit = i + 1;
        }
        SetWindowTextW(g_digit[i], t);
        SetWindowPos(g_digit[i], HWND_TOPMOST, (areas[i].left + areas[i].right - sz) / 2, (areas[i].top + areas[i].bottom - sz) / 2, sz, sz, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_digit[i], nullptr, FALSE);
        UpdateWindow(g_digit[i]);
    }
}

void CountdownHide()
{
    for (int i = 0; i < g_nDigit; ++i) if (g_digit[i]) { DestroyWindow(g_digit[i]); g_digit[i] = nullptr; }
    g_nDigit = 0;
}

// The area a recording / countdown refers to: the recorded rectangle, or every monitor for the whole desktop.
int RecordAreas(RECT* out)
{
    if (g_modeDesktop) { int n = MonitorRects(out, 16); if (n > 0) return n; }
    out[0] = TargetRect();
    return 1;
}

void RegisterOverlayClasses()
{
    static bool reg = false;
    if (reg) return;
    reg = true;
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PanelProc; wc.hInstance = g_inst; wc.lpszClassName = L"Click2ScreenPanel";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = g_icon;
    RegisterClassW(&wc);
    wc.lpfnWndProc = FrameProc; wc.lpszClassName = C2S_CLASS_FRAME; wc.hIcon = nullptr;
    RegisterClassW(&wc);
    wc.lpfnWndProc = CountProc; wc.lpszClassName = L"Click2ScreenCount";
    RegisterClassW(&wc);
}

// The red frame round the chosen area (recording and screenshots), blinking twice a second as in the original.
bool g_frameOn;
void FramePlace(const RECT& rc)
{
    if (!g_frame) return;
    int fw = Sc(kFrameW);
    int w = rc.right - rc.left + 2 * fw, h = rc.bottom - rc.top + 2 * fw;
    HRGN outer = CreateRectRgn(0, 0, w, h), inner = CreateRectRgn(fw, fw, w - fw, h - fw);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    DeleteObject(inner);
    SetWindowRgn(g_frame, outer, TRUE);
    SetWindowPos(g_frame, HWND_TOPMOST, rc.left - fw, rc.top - fw, w, h, SWP_NOACTIVATE | (g_frameOn ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void FrameShow(const RECT& rc)
{
    RegisterOverlayClasses();
    if (!g_frame) {
        g_frame = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, C2S_CLASS_FRAME, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
        if (!g_frame) return;
        ExcludeFromCapture(g_frame);
        SetTimer(g_main, TIMER_BLINK, 500, nullptr);
    }
    g_frameOn = true;
    FramePlace(rc);
}

void FrameHide()
{
    if (g_frame) { KillTimer(g_main, TIMER_BLINK); DestroyWindow(g_frame); g_frame = nullptr; }
    g_frameOn = false;
}

void FrameBlink()
{
    if (!g_frame) return;
    g_frameOn = !g_frameOn;
    ShowWindow(g_frame, g_frameOn ? SW_SHOWNA : SW_HIDE);
}

struct MonList { RECT r[16]; int n; };
BOOL CALLBACK MonEnum(HMONITOR, HDC, LPRECT r, LPARAM p)
{
    MonList* m = (MonList*)p;
    if (m->n < 16) m->r[m->n++] = *r;
    return TRUE;
}

// The monitors' rectangles, left to right, then top to bottom.
int MonitorRects(RECT* out, int cap)
{
    MonList m; m.n = 0;
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, (LPARAM)&m);
    for (int i = 1; i < m.n; ++i)
        for (int j = i; j > 0 && (m.r[j].left < m.r[j - 1].left || (m.r[j].left == m.r[j - 1].left && m.r[j].top < m.r[j - 1].top)); --j) {
            RECT t = m.r[j]; m.r[j] = m.r[j - 1]; m.r[j - 1] = t;
        }
    int n = m.n < cap ? m.n : cap;
    for (int i = 0; i < n; ++i) out[i] = m.r[i];
    return n;
}

void Shot(Mode mode)
{
    Log(L"shot: mode %d, busy %d, state %d", (int)mode, (int)g_busyShot, (int)g_state);
    if (g_busyShot) return;
    if (g_state == ST_RECORDING || g_state == ST_PAUSED || g_state == ST_COUNTDOWN) return;   // hotkeys during a recording are ignored
    g_busyShot = true;
    bool wasVisible = g_main && IsWindowVisible(g_main);
    RECT rects[16]; int count = 1; HWND hw = nullptr;
    wchar_t err[300];
    if (!EnsureFolder(g_cfg.imgFolder, err, 300)) { Warn(err); g_busyShot = false; return; }
    if (mode == M_DESKTOP) {
        // One picture per monitor (not one merged picture of the whole desktop).
        count = MonitorRects(rects, 16);
        if (count < 1) { rects[0] = VirtualScreen(); count = 1; }
        if (wasVisible) { ShowMain(false); PumpFor(250); }
    }
    else if (!ChooseTarget(mode, &rects[0], &hw)) { if (wasVisible) ShowMain(true); g_busyShot = false; return; }
    else if (wasVisible) ShowMain(false);
    if (mode != M_DESKTOP) FrameShow(rects[0]);              // the red frame round the chosen area (as for a recording)
    for (int i = g_cfg.delaySec; i > 0; --i) { Beep(true); CountdownShow(i, rects, count); PumpFor(1000); }
    CountdownHide();
    if (hw) { if (!IsWindow(hw) || !WindowVisibleRect(hw, &rects[0])) { if (wasVisible) ShowMain(true); g_busyShot = false; return; } }
    if (mode != M_DESKTOP) PumpFor(300);          // let the screen show the desktop again, not the dimmed picker
    HBITMAP bmps[16];                             // all the monitors are captured at the same moment, then saved one by one
    int got = 0;
    Log(L"shot: capturing %d area(s)", count);
    for (int i = 0; i < count; ++i) { bmps[i] = CaptureRect(rects[i], g_cfg.capMouse, g_cfg.highlight); if (bmps[i]) ++got; }
    Log(L"shot: captured %d of %d", got, count);
    FrameHide();
    if (wasVisible) ShowMain(true);
    if (got == 0) { Warn(T(S_ERR_CAPTURE)); g_busyShot = false; return; }
    wchar_t path[MAX_PATH] = L"", last[MAX_PATH] = L"";
    bool saved = false;
    for (int i = 0; i < count; ++i) {
        if (!bmps[i]) continue;
        if (!BuildOutputPath(g_cfg.imgFolder, g_cfg.fileName, ImageFormatExt(g_cfg.imageFormat), L"ImgCounter", path, MAX_PATH, err, 300)) {
            Warn(err); DeleteObject(bmps[i]); bmps[i] = nullptr; continue;
        }
        Log(L"shot: saving picture %d as %s", i + 1, ImageFormatName(g_cfg.imageFormat));
        DWORD t0 = GetTickCount();
        bool wrote = SaveBitmap(bmps[i], path, g_cfg.imageFormat);
        Log(L"shot: saved %s as %s in %lu ms (%d)", path, ImageFormatName(g_cfg.imageFormat), (unsigned long)(GetTickCount() - t0), (int)wrote);
        if (!wrote) {
            wchar_t msg[MAX_PATH + 100]; _snwprintf(msg, MAX_PATH + 100, T(S_ERR_SAVE), path);
            Warn(msg); DeleteObject(bmps[i]); bmps[i] = nullptr; continue;
        }
        saved = true;
        wcscpy(last, path);
        wchar_t msg[MAX_PATH + 40]; _snwprintf(msg, MAX_PATH + 40, T(S_SAVED), path);
        SetStatus(msg);
        if (g_cfg.openEditor) OpenEditor(g_main, bmps[i], path, g_cfg.imageFormat);   // takes the bitmap
        else DeleteObject(bmps[i]);
        bmps[i] = nullptr;
    }
    if (saved) Beep(false);
    if (saved && g_cfg.openFolder) ShowInExplorer(last);
    g_busyShot = false;
}

// ---- options -----------------------------------------------------------------------------------------------------
const int kPages = 5;
struct OptState { Settings s; bool ok, done; HWND dlg; HWND pg[kPages][32]; int np[kPages]; int sel; RECT tabRc[kPages]; };

HWND OAdd(OptState* o, int page, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = MakeCtl(parent, cls, text, style, x, y, w, h, id);
    if (o->np[page] < 32) o->pg[page][o->np[page]++] = c;
    return c;
}

int ComboAdd(HWND cb, const wchar_t* t, LPARAM data = -1)
{
    int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
    if (i >= 0 && data >= 0) SendMessageW(cb, CB_SETITEMDATA, i, data);
    return i;
}

// A format combo lists the formats this Windows can write; the item data is the format number.
void ComboSelectData(HWND d, int id, int data)
{
    HWND cb = GetDlgItem(d, id);
    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0), pick = 0;
    for (int i = 0; i < n; ++i) if ((int)SendMessageW(cb, CB_GETITEMDATA, i, 0) == data) pick = i;
    SendMessageW(cb, CB_SETCURSEL, pick, 0);
}
int ComboGetData(HWND d, int id, int fallback)
{
    HWND cb = GetDlgItem(d, id);
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0) return fallback;
    int v = (int)SendMessageW(cb, CB_GETITEMDATA, i, 0);
    return v < 0 ? fallback : v;
}

void OptLoad(OptState* o)
{
    HWND d = o->dlg; Settings& s = o->s;
    auto chk = [&](int id, bool v) { CheckDlgButton(d, id, v ? BST_CHECKED : BST_UNCHECKED); };
    chk(O_AUTOSTART, s.runAtStartup); chk(O_MOUSE, s.capMouse); chk(O_HILITE, s.highlight); chk(O_OPENFOLDER, s.openFolder);
    chk(O_OPENEDITOR, s.openEditor); chk(O_SOUNDS, s.sounds); chk(O_MIC, s.mic);
    SendDlgItemMessageW(d, O_DELAY, CB_SETCURSEL, s.delaySec < 0 ? 0 : s.delaySec > 10 ? 10 : s.delaySec, 0);
    ComboSelectData(d, O_VCODEC, s.videoCodec);
    SendDlgItemMessageW(d, O_ACODEC, CB_SETCURSEL, s.audioCodec, 0);
    SendDlgItemMessageW(d, O_VQ, CB_SETCURSEL, s.videoQuality, 0);
    SendDlgItemMessageW(d, O_AQ, CB_SETCURSEL, s.audioQuality, 0);
    static const int fpsv[] = { 10, 15, 20, 25, 30, 45, 60 };
    int fi = 3; for (int i = 0; i < 7; ++i) if (fpsv[i] == s.fps) fi = i;
    SendDlgItemMessageW(d, O_FPS, CB_SETCURSEL, fi, 0);
    SetDlgItemInt(d, O_DH, s.durH, FALSE); SetDlgItemInt(d, O_DM, s.durM, FALSE); SetDlgItemInt(d, O_DS, s.durS, FALSE);
    SetDlgItemTextW(d, O_VIDFOLDER, s.vidFolder); SetDlgItemTextW(d, O_IMGFOLDER, s.imgFolder);
    ComboSelectData(d, O_IMGFMT, s.imageFormat);
    SetDlgItemTextW(d, O_TPL, s.fileName);
    SendDlgItemMessageW(d, O_LANG, CB_SETCURSEL, s.language, 0);
}

// The dialog's controls -> s (no checks).
void OptCollect(OptState* o, Settings& s)
{
    HWND d = o->dlg;
    auto chk = [&](int id) { return IsDlgButtonChecked(d, id) == BST_CHECKED; };
    s.runAtStartup = chk(O_AUTOSTART); s.capMouse = chk(O_MOUSE); s.highlight = chk(O_HILITE); s.openFolder = chk(O_OPENFOLDER);
    s.openEditor = chk(O_OPENEDITOR); s.sounds = chk(O_SOUNDS); s.mic = chk(O_MIC);
    auto num = [&](int id, int lo, int hi) { int v = (int)GetDlgItemInt(d, id, nullptr, FALSE); return v < lo ? lo : v > hi ? hi : v; };
    { int i = (int)SendDlgItemMessageW(d, O_DELAY, CB_GETCURSEL, 0, 0); s.delaySec = i < 0 ? 0 : i > 10 ? 10 : i; }
    s.videoCodec = ComboGetData(d, O_VCODEC, s.videoCodec);
    s.audioCodec = (int)SendDlgItemMessageW(d, O_ACODEC, CB_GETCURSEL, 0, 0);
    s.videoQuality = (int)SendDlgItemMessageW(d, O_VQ, CB_GETCURSEL, 0, 0);
    s.audioQuality = (int)SendDlgItemMessageW(d, O_AQ, CB_GETCURSEL, 0, 0);
    static const int fpsv[] = { 10, 15, 20, 25, 30, 45, 60 };
    int fi = (int)SendDlgItemMessageW(d, O_FPS, CB_GETCURSEL, 0, 0); if (fi < 0 || fi > 6) fi = 3;
    s.fps = fpsv[fi];
    s.durH = num(O_DH, 0, 99); s.durM = num(O_DM, 0, 59); s.durS = num(O_DS, 0, 59);
    GetDlgItemTextW(d, O_VIDFOLDER, s.vidFolder, MAX_PATH); GetDlgItemTextW(d, O_IMGFOLDER, s.imgFolder, MAX_PATH);
    s.imageFormat = ComboGetData(d, O_IMGFMT, s.imageFormat);
    GetDlgItemTextW(d, O_TPL, s.fileName, 128);
    s.language = (int)SendDlgItemMessageW(d, O_LANG, CB_GETCURSEL, 0, 0); if (s.language < 0) s.language = 0;
}

bool OptStore(OptState* o)
{
    HWND d = o->dlg; Settings s = o->s;
    OptCollect(o, s);
    wchar_t err[300];
    if (!TemplateValid(s.fileName)) { MessageBoxW(d, T(S_ERR_TEMPLATE), T(S_OPT_TITLE), MB_OK | MB_ICONWARNING); return false; }
    // A folder that does not exist yet is created (the original only warned).
    if (!EnsureFolder(s.vidFolder, err, 300) || !EnsureFolder(s.imgFolder, err, 300)) {
        MessageBoxW(d, err, T(S_OPT_TITLE), MB_OK | MB_ICONWARNING); return false;
    }
    o->s = s;
    return true;
}

// .ini export / import of the dialog's current values.
void IniFileFilter(wchar_t* filter)
{
    const wchar_t* name = T(S_INI_FILTER);
    size_t l = wcslen(name) + 1;
    memcpy(filter, name, l * sizeof(wchar_t));
    wcscpy(filter + l, L"*.ini");
    filter[l + 6] = 0;
}

void OptExport(OptState* o)
{
    Settings s = o->s; OptCollect(o, s);
    wchar_t file[MAX_PATH] = L"Click2Screen.ini", filter[200];
    IniFileFilter(filter);
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = o->dlg; ofn.lpstrFilter = filter; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR; ofn.lpstrDefExt = L"ini"; ofn.lpstrTitle = T(S_OPT_EXPORT);
    if (!GetSaveFileNameW(&ofn)) return;
    if (!SettingsExport(s, file)) {
        wchar_t msg[MAX_PATH + 100]; _snwprintf(msg, MAX_PATH + 100, T(S_ERR_EXPORT), file);
        MessageBoxW(o->dlg, msg, T(S_OPT_TITLE), MB_OK | MB_ICONWARNING);
    }
}

void OptImport(OptState* o)
{
    wchar_t file[MAX_PATH] = L"", filter[200];
    IniFileFilter(filter);
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = o->dlg; ofn.lpstrFilter = filter; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR; ofn.lpstrTitle = T(S_OPT_IMPORT);
    if (!GetOpenFileNameW(&ofn)) return;
    Settings s = o->s; OptCollect(o, s);          // keeps what the file does not say (and the language)
    if (!SettingsImport(s, file)) {
        wchar_t msg[MAX_PATH + 100]; _snwprintf(msg, MAX_PATH + 100, T(S_ERR_IMPORT), file);
        MessageBoxW(o->dlg, msg, T(S_OPT_TITLE), MB_OK | MB_ICONWARNING);
        return;
    }
    o->s = s; OptLoad(o);
}

void BrowseFolder(HWND d, int editId)
{
    wchar_t cur[MAX_PATH]; GetDlgItemTextW(d, editId, cur, MAX_PATH);
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = d; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    bi.lpszTitle = T(S_BROWSE);
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, p)) SetDlgItemTextW(d, editId, p);
        CoTaskMemFree(pidl);
    }
}

void ShowPage(OptState* o, int page)
{
    o->sel = page;
    for (int p = 0; p < kPages; ++p)
        for (int i = 0; i < o->np[p]; ++i) ShowWindow(o->pg[p][i], p == page ? SW_SHOW : SW_HIDE);
    InvalidateRect(o->dlg, nullptr, FALSE);
}

// Tabs and the page frame are drawn by hand (the original's flat look): 1-unit grey lines, the selected tab open to the page.
void PaintOptions(OptState* o, HDC dc)
{
    const COLORREF face = GetSysColor(COLOR_BTNFACE), line = RGB(160, 160, 160);
    RECT cr; GetClientRect(o->dlg, &cr);
    Box(dc, 0, 0, cr.right, cr.bottom, face);
    RECT pg = { Sc(16), Sc(37), Sc(513), Sc(287) };
    Box(dc, pg.left, pg.top, pg.right - pg.left, 1, line); Box(dc, pg.left, pg.bottom - 1, pg.right - pg.left, 1, line);
    Box(dc, pg.left, pg.top, 1, pg.bottom - pg.top, line); Box(dc, pg.right - 1, pg.top, 1, pg.bottom - pg.top, line);
    const Str names[kPages] = { S_OPT_GENERAL, S_OPT_VIDEO_AUDIO, S_OPT_IMAGES, S_OPT_FILENAME, S_OPT_LANG };
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0, 0, 0));
    HGDIOBJ of = SelectObject(dc, g_font);
    for (int i = 0; i < kPages; ++i) {
        RECT t = o->tabRc[i];
        bool on = i == o->sel;
        if (on) { t.top -= Sc(2); }
        Box(dc, t.left, t.top, t.right - t.left, t.bottom - t.top, on ? RGB(255, 255, 255) : RGB(232, 232, 232));
        Box(dc, t.left, t.top, t.right - t.left, 1, line); Box(dc, t.left, t.top, 1, t.bottom - t.top, line); Box(dc, t.right - 1, t.top, 1, t.bottom - t.top, line);
        if (on) Box(dc, t.left + 1, pg.top, t.right - t.left - 2, 1, RGB(255, 255, 255));        // opens onto the page
        RECT tx = t;
        DrawTextW(dc, T(names[i]), -1, &tx, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
}

LRESULT CALLBACK OptProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    OptState* o = (OptState*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return DefWindowProcW(hw, m, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps); if (o) PaintOptions(o, dc); EndPaint(hw, &ps); return 0; }
    case WM_LBUTTONDOWN:
        if (o) {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            for (int i = 0; i < kPages; ++i) if (PtInRect(&o->tabRc[i], pt)) { ShowPage(o, i); break; }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: if (OptStore(o)) { o->ok = true; o->done = true; } break;
        case IDCANCEL: o->done = true; break;
        case O_BRVID: BrowseFolder(hw, O_VIDFOLDER); break;
        case O_BRIMG: BrowseFolder(hw, O_IMGFOLDER); break;
        case O_EXPORT: OptExport(o); break;
        case O_IMPORT: OptImport(o); break;
        case O_DEFAULTS: { Settings cur = o->s; OptCollect(o, cur); Settings d; SettingsDefaults(d); d.language = cur.language; o->s = d; OptLoad(o); } break;
        }
        return 0;
    case WM_CLOSE: o->done = true; return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

bool OptionsDialog()
{
    static bool reg = false;
    if (!reg) {
        INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_TAB_CLASSES | ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&ic);
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = OptProc; wc.hInstance = g_inst; wc.lpszClassName = L"Click2ScreenOptions";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = g_icon;
        RegisterClassW(&wc); reg = true;
    }
    OptState o; memset(&o, 0, sizeof(o));
    o.s = g_cfg;
    // 526 x 340 units as the original: tabs at y 16, the page frame 16..513 x 37..287, buttons 75 x 22 at y 303.
    int W = Sc(526), H = Sc(340);
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND owner = g_main && IsWindowVisible(g_main) ? g_main : nullptr;
    o.dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"Click2ScreenOptions", T(S_OPT_TITLE), WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr, g_inst, &o);
    HWND d = o.dlg;
    {   // the tab rectangles: the text width + 16 units, 21 units high
        HDC dc = GetDC(d); HGDIOBJ of = SelectObject(dc, g_font);
        const Str names[kPages] = { S_OPT_GENERAL, S_OPT_VIDEO_AUDIO, S_OPT_IMAGES, S_OPT_FILENAME, S_OPT_LANG };
        int x = Sc(16);
        for (int i = 0; i < kPages; ++i) {
            SIZE sz; GetTextExtentPoint32W(dc, T(names[i]), (int)wcslen(T(names[i])), &sz);
            o.tabRc[i].left = x; o.tabRc[i].right = x + sz.cx + Sc(16); o.tabRc[i].top = Sc(16); o.tabRc[i].bottom = Sc(37);
            x = o.tabRc[i].right;
        }
        SelectObject(dc, of); ReleaseDC(d, dc);
    }
    // All positions are in 96-dpi units inside the dialog; Sc() scales them.
    auto add = [&](int page, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        return OAdd(&o, page, d, cls, text, style, Sc(x), Sc(y), Sc(w), Sc(h), id);
    };
    auto label = [&](int page, Str s, int x, int y, int w) { add(page, L"STATIC", T(s), SS_LEFT | SS_NOPREFIX, x, y, w, 16, 0); };
    auto combo = [&](int page, int id, int x, int y, int w) { return add(page, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, 200, id); };
    auto edit = [&](int page, int id, int x, int y, int w, DWORD extra = 0) { return add(page, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | extra, x, y, w, 20, id); };

    // page 0: general (checkboxes at 28.5-unit steps from y 91; the delay on top)
    label(0, S_OPT_DELAY, 33, 61, 200);
    HWND dl = combo(0, O_DELAY, 244, 58, 40);
    for (int i = 0; i <= 10; ++i) { wchar_t n[8]; _snwprintf(n, 8, L"%d", i); ComboAdd(dl, n); }
    label(0, S_OPT_SECONDS, 294, 61, 150);
    static const struct { Str s; int id; } chk[] = { {S_OPT_AUTOSTART, O_AUTOSTART}, {S_OPT_MOUSE, O_MOUSE}, {S_OPT_HIGHLIGHT, O_HILITE},
        {S_OPT_OPEN_FOLDER, O_OPENFOLDER}, {S_OPT_OPEN_EDITOR, O_OPENEDITOR}, {S_OPT_SOUNDS, O_SOUNDS}, {S_OPT_MIC, O_MIC} };
    for (int i = 0; i < 7; ++i) add(0, L"BUTTON", T(chk[i].s), WS_TABSTOP | BS_AUTOCHECKBOX, 33, 91 + i * 57 / 2, 460, 18, chk[i].id);

    // page 1: video (left column) and audio (right column)
    label(1, S_OPT_VIDEO_OPTIONS, 35, 52, 220); label(1, S_OPT_AUDIO_OPTIONS, 270, 52, 220);
    label(1, S_OPT_VCODEC, 35, 72, 220);        label(1, S_OPT_ACODEC, 270, 72, 220);
    HWND vc = combo(1, O_VCODEC, 34, 90, 223);  HWND ac = combo(1, O_ACODEC, 270, 90, 223);
    for (int i = 0; i < VC_COUNT; ++i) if (VideoFormatAvailable(i)) ComboAdd(vc, VideoFormatName(i), i);
    ComboAdd(ac, T(S_AC_AAC)); ComboAdd(ac, T(S_AC_MP3));
    label(1, S_OPT_VQUALITY, 35, 118, 220);     label(1, S_OPT_AQUALITY, 270, 118, 220);
    HWND vq = combo(1, O_VQ, 34, 136, 223);     HWND aq = combo(1, O_AQ, 270, 136, 223);
    HWND qs[2] = { vq, aq };
    for (HWND q : qs) { ComboAdd(q, T(S_Q_HIGH)); ComboAdd(q, T(S_Q_MEDIUM)); ComboAdd(q, T(S_Q_LOW)); }
    label(1, S_OPT_FPS, 35, 166, 140);
    HWND fp = combo(1, O_FPS, 180, 162, 77);
    static const wchar_t* const fpsText[7] = { L"10", L"15", L"20", L"25", L"30", L"45", L"60" };
    for (const wchar_t* f : fpsText) ComboAdd(fp, f);
    label(1, S_OPT_VIDFOLDER, 35, 193, 400);
    edit(1, O_VIDFOLDER, 34, 211, 417);
    add(1, L"BUTTON", L"...", WS_TABSTOP | BS_PUSHBUTTON, 459, 210, 33, 22, O_BRVID);
    label(1, S_OPT_DURATION, 35, 240, 450);
    edit(1, O_DH, 35, 258, 38, ES_NUMBER);  label(1, S_OPT_HOURS, 78, 260, 70);
    edit(1, O_DM, 160, 258, 38, ES_NUMBER); label(1, S_OPT_MINUTES, 203, 260, 70);
    edit(1, O_DS, 285, 258, 38, ES_NUMBER); label(1, S_OPT_SECONDS, 328, 260, 70);

    // page 2: images
    label(2, S_OPT_IMGFORMAT, 35, 63, 160);
    HWND fm = combo(2, O_IMGFMT, 203, 61, 188);
    for (int i = 0; i < IMG_COUNT; ++i) if (ImageFormatAvailable(i)) ComboAdd(fm, ImageFormatName(i), i);
    label(2, S_OPT_IMGFOLDER, 35, 113, 400);
    edit(2, O_IMGFOLDER, 134, 111, 316);
    add(2, L"BUTTON", L"...", WS_TABSTOP | BS_PUSHBUTTON, 459, 110, 33, 22, O_BRIMG);

    // page 3: file name
    label(3, S_OPT_TEMPLATE, 34, 61, 120);
    edit(3, O_TPL, 160, 59, 333);
    add(3, L"STATIC", T(S_OPT_TEMPLATE_TIPS), SS_LEFT | SS_NOPREFIX, 34, 92, 460, 150, 0);

    // page 4: language (and the settings file: export / import)
    label(4, S_OPT_LANG, 33, 57, 205);
    HWND lg = combo(4, O_LANG, 33, 76, 205);
    ComboAdd(lg, T(S_LANG_AUTO));
    for (int i = 0; i < LangCount(); ++i) ComboAdd(lg, LangName(i));
    add(4, L"BUTTON", T(S_OPT_EXPORT), WS_TABSTOP | BS_PUSHBUTTON, 33, 118, 205, 22, O_EXPORT);
    add(4, L"BUTTON", T(S_OPT_IMPORT), WS_TABSTOP | BS_PUSHBUTTON, 33, 148, 205, 22, O_IMPORT);

    // the buttons below the page: Restore default 170 wide on the left, Ok and Cancel 75 wide on the right
    auto btn = [&](const wchar_t* text, DWORD style, int x, int w, int id) { MakeCtl(d, L"BUTTON", text, WS_TABSTOP | style, Sc(x), Sc(303), Sc(w), Sc(22), id); };
    btn(T(S_RESTORE_DEFAULT), BS_PUSHBUTTON, 16, 170, O_DEFAULTS);
    btn(T(S_BTN_OK), BS_DEFPUSHBUTTON, 354, 75, IDOK);
    btn(T(S_BTN_CANCEL), BS_PUSHBUTTON, 435, 75, IDCANCEL);
    // all the children use the dialog font
    {   HWND c = nullptr;
        while ((c = FindWindowExW(d, c, nullptr, nullptr)) != nullptr) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    OptLoad(&o);
    ShowPage(&o, 0);

    // centre over the owner / screen
    RECT wr; GetWindowRect(d, &wr);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(d, HWND_TOPMOST, (wa.left + wa.right - (wr.right - wr.left)) / 2, (wa.top + wa.bottom - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE);
    ShowWindow(d, SW_SHOW);
    if (owner) EnableWindow(owner, FALSE);
    MSG msg;
    bool quit = false; int quitCode = 0;
    while (!o.done) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { quit = true; quitCode = (int)msg.wParam; o.ok = false; break; }    // the program is being closed: nothing is saved
        if (r < 0) break;
        if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(d);
    if (quit) { PostQuitMessage(quitCode); return false; }                                // the main loop has to see WM_QUIT too
    if (owner) SetForegroundWindow(owner);
    if (o.ok) {
        g_cfg = o.s;
        SettingsSave(g_cfg);
        SettingsSetAutostart(g_cfg.runAtStartup);
    }
    return o.ok;
}

// ---- main window ---------------------------------------------------------------------------------------------
void LayoutMainText();

void ApplyLanguage()
{
    LangSelect(g_cfg.language);
    LayoutMainText();
    TrayTip();
}

void LayoutMainText()
{
    if (!g_main) return;
    SetWindowTextW(g_main, T(S_APP));
    // The icons carry no text; the window text of each button is its tooltip (and what a screen reader says).
    struct { int id; Str s; } t[] = {
        {IDC_REC_DESKTOP, S_REC_DESKTOP}, {IDC_REC_REGION, S_REC_REGION}, {IDC_REC_WINDOW, S_REC_WINDOW},
        {IDC_SHOT_DESKTOP, S_SHOT_DESKTOP}, {IDC_SHOT_REGION, S_SHOT_REGION}, {IDC_SHOT_WINDOW, S_SHOT_WINDOW},
        {IDC_MENU, S_OPTIONS} };
    for (auto& i : t) SetWindowTextW(GetDlgItem(g_main, i.id), T(i.s));
    InvalidateRect(g_main, nullptr, TRUE);
}

void CreateMainControls(HWND hw)
{
    // 206 x 62 units: a 28-unit caption (icon, name, minimize, close) and a row of 26-unit buttons with 16-unit icons:
    // three screenshots (black) at 1, 27, 53; three recordings (blue) at 91, 117, 143; the menu at 180.
    struct { int id; int x; } b[] = {
        { IDC_SHOT_DESKTOP, 1 }, { IDC_SHOT_WINDOW, 27 }, { IDC_SHOT_REGION, 53 },
        { IDC_REC_DESKTOP, 91 }, { IDC_REC_WINDOW, 117 }, { IDC_REC_REGION, 143 }, { IDC_MENU, 180 } };
    for (auto& i : b)
        MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, Sc(i.x), Sc(32), Sc(26), Sc(26), i.id);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, Sc(150), 0, Sc(28), Sc(kCapH), IDC_MIN);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, Sc(178), 0, Sc(28), Sc(kCapH), IDC_CLOSE);
    // The last saved file / error (read by the tests and the screen readers); the window itself shows no status line.
    CreateWindowExW(0, L"STATIC", T(S_STATUS_READY), WS_CHILD | SS_LEFT, 0, 0, Sc(10), Sc(10), hw, (HMENU)(INT_PTR)IDC_STATUS, g_inst, nullptr);
    // Tooltips of the icon buttons.
    if (g_tip) DestroyWindow(g_tip);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, hw, nullptr, g_inst, nullptr);
    for (auto& i : b) {
        if (i.id == IDC_MENU) continue;
        TOOLINFOW ti; memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti); ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; ti.hwnd = hw; ti.uId = (UINT_PTR)GetDlgItem(hw, i.id);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, Sc(320));
}

void RoundMain()
{
    RECT r; GetWindowRect(g_main, &r);
    RoundWindow(g_main, r.right - r.left, r.bottom - r.top, Sc(8));
}

void PaintMain(HWND hw, HDC dc)
{
    int w = Sc(kMainW), capH = Sc(kCapH);
    for (int y = 0; y < capH; ++y) {                                          // caption: a vertical gradient
        int t = y * 255 / (capH - 1);
        Box(dc, 0, y, w, 1, RGB(GetRValue(kCapTop) + (GetRValue(kCapBot) - GetRValue(kCapTop)) * t / 255,
                                GetGValue(kCapTop) + (GetGValue(kCapBot) - GetGValue(kCapTop)) * t / 255,
                                GetBValue(kCapTop) + (GetBValue(kCapBot) - GetBValue(kCapTop)) * t / 255));
    }
    RECT rc; GetClientRect(hw, &rc);
    Box(dc, 0, capH, w, rc.bottom - capH, kBody);
    if (g_iconSmall) DrawIconEx(dc, Sc(6), Sc(6), g_iconSmall, Sc(16), Sc(16), 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ of = SelectObject(dc, g_fontBold);
    RECT t = { Sc(28), 0, Sc(148), capH };
    DrawTextW(dc, T(S_APP), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
}

void ShowMainMenu()
{
    HMENU mn = CreatePopupMenu();
    AppendMenuW(mn, MF_STRING, ID_TRAY_OPTIONS, T(S_OPTIONS));
    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mn, MF_STRING, ID_MENU_PICTURES, T(S_OPEN_PICTURES));
    AppendMenuW(mn, MF_STRING, ID_MENU_VIDEOS, T(S_OPEN_VIDEOS));
    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mn, MF_STRING | (g_cfg.capMouse ? MF_CHECKED : 0), ID_MENU_MOUSE, T(S_OPT_MOUSE));
    RECT r; GetWindowRect(GetDlgItem(g_main, IDC_MENU), &r);
    int cmd = TrackPopupMenu(mn, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON, r.right, r.bottom, 0, g_main, nullptr);
    DestroyMenu(mn);
    if (cmd) PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

void OpenFolder(const wchar_t* folder)
{
    wchar_t err[300];
    if (!EnsureFolder(folder, err, 300)) { Warn(err); return; }
    ShellExecuteW(nullptr, L"open", folder, nullptr, nullptr, SW_SHOWNORMAL);
}

void RebuildMain(UINT dpi, const RECT* suggested)
{
    wchar_t status[MAX_PATH + 60] = L"";
    GetWindowTextW(GetDlgItem(g_main, IDC_STATUS), status, MAX_PATH + 60);
    g_dpi = dpi;
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    MakeFonts();
    HWND c;
    while ((c = GetWindow(g_main, GW_CHILD)) != nullptr) DestroyWindow(c);
    CreateMainControls(g_main);
    LayoutMainText();
    SetStatus(status);
    if (suggested)
        SetWindowPos(g_main, nullptr, suggested->left, suggested->top, Sc(kMainW), Sc(kMainH), SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(g_main, nullptr, 0, 0, Sc(kMainW), Sc(kMainH), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    RoundMain();
}

void RegisterHotkeys()
{
    wchar_t bad[160] = L"";
    struct { int id; UINT mod; UINT vk; const wchar_t* name; } hk[] = {
        { HK_DESKTOP, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT, L"Ctrl+PrintScreen" },
        { HK_REGION,  MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '4', L"Alt+Shift+4" },
        { HK_WINDOW,  MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '5', L"Alt+Shift+5" } };
    for (auto& h : hk)
        if (!RegisterHotKey(g_main, h.id, h.mod, h.vk)) { if (bad[0]) wcscat(bad, L", "); wcscat(bad, h.name); }
    if (bad[0]) { wchar_t msg[260]; _snwprintf(msg, 260, T(S_ERR_HOTKEY), bad); Balloon(T(S_APP), msg); }
}

LRESULT CALLBACK MainProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == g_msgTaskbar && g_msgTaskbar) { TrayAdd(); return 0; }
    switch (m) {
    case WM_CREATE:
        g_main = hw;
        CreateMainControls(hw);
        return 0;
    case WM_NCCREATE: return DefWindowProcW(hw, m, wp, lp);
    case WM_TIMER: if (wp == TIMER_BLINK) FrameBlink(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps); PaintMain(hw, dc); EndPaint(hw, &ps); return 0; }
    case WM_DRAWITEM: DrawMainItem((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_NCHITTEST: {                         // the caption is drawn by the program: dragging it moves the window
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hw, &pt);
        return pt.y >= 0 && pt.y < Sc(kCapH) ? HTCAPTION : HTCLIENT; }
    case WM_NOTIFY: {
        NMTTDISPINFOW* nm = (NMTTDISPINFOW*)lp;
        if (nm->hdr.code == TTN_GETDISPINFOW) {
            static wchar_t tip[120];
            int id = GetDlgCtrlID((HWND)nm->hdr.idFrom);
            static const struct { int id; Str s; } t[] = { {IDC_REC_DESKTOP, S_REC_DESKTOP}, {IDC_REC_REGION, S_REC_REGION}, {IDC_REC_WINDOW, S_REC_WINDOW},
                {IDC_SHOT_DESKTOP, S_SHOT_DESKTOP}, {IDC_SHOT_REGION, S_SHOT_REGION}, {IDC_SHOT_WINDOW, S_SHOT_WINDOW} };
            tip[0] = 0;
            for (auto& i : t) if (i.id == id) { wcsncpy(tip, T(i.s), 119); tip[119] = 0; }
            for (wchar_t* c = tip; *c; ++c) if (*c == L'\n') *c = L' ';       // "Capture desktop (Ctrl+PrintScreen)"
            nm->lpszText = tip;
        }
        return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MENU: ShowMainMenu(); break;
        case IDC_MIN: ShowWindow(hw, SW_HIDE); break;           // no taskbar button: the program lives in the tray
        case IDC_CLOSE: PostMessageW(hw, WM_CLOSE, 0, 0); break;
        case ID_MENU_PICTURES: OpenFolder(g_cfg.imgFolder); break;
        case ID_MENU_VIDEOS: OpenFolder(g_cfg.vidFolder); break;
        case ID_MENU_MOUSE: g_cfg.capMouse = !g_cfg.capMouse; SettingsSave(g_cfg); break;
        case IDC_OPTIONS:
            if (OptionsDialog()) ApplyLanguage();
            break;
        case IDC_REC_DESKTOP: Arm(M_DESKTOP); break;
        case IDC_REC_REGION:  Arm(M_REGION); break;
        case IDC_REC_WINDOW:  Arm(M_WINDOW); break;
        case IDC_SHOT_DESKTOP: PostMessageW(hw, WM_HOTKEY, HK_DESKTOP, 0); break;
        case IDC_SHOT_REGION:  PostMessageW(hw, WM_HOTKEY, HK_REGION, 0); break;
        case IDC_SHOT_WINDOW:  PostMessageW(hw, WM_HOTKEY, HK_WINDOW, 0); break;
        case ID_TRAY_OPTIONS: if (OptionsDialog()) ApplyLanguage(); break;
        case ID_TRAY_SHOW: ShowMain(true); break;
        case ID_TRAY_PANEL: if (g_state != ST_IDLE) ShowPanel(); break;
        case ID_TRAY_PAUSE: if (g_state != ST_IDLE) StartPressed(); break;
        case ID_TRAY_STOP: if (g_state != ST_IDLE) StopRecording(true); break;
        case ID_TRAY_EXIT: DestroyWindow(hw); break;
        }
        return 0;
    case WM_HOTKEY:
        switch (wp) {
        case HK_DESKTOP: Shot(M_DESKTOP); break;
        case HK_REGION: Shot(M_REGION); break;
        case HK_WINDOW: Shot(M_WINDOW); break;
        case HK_STOP: StopRecording(true); break;
        }
        return 0;
    case WM_C2S_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (g_state == ST_IDLE) ShowMain(!IsWindowVisible(g_main) || IsIconic(g_main));
            else if (g_state == ST_ARMED || g_state == ST_RECORDING || g_state == ST_PAUSED) StartPressed();
        } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            HMENU mn = CreatePopupMenu();
            if (g_state == ST_IDLE) AppendMenuW(mn, MF_STRING, ID_TRAY_SHOW, T(S_APP));
            else {
                AppendMenuW(mn, MF_STRING, ID_TRAY_PANEL, T(S_SHOW_PANEL));
                AppendMenuW(mn, MF_STRING, ID_TRAY_PAUSE, g_state == ST_RECORDING ? T(S_PAUSE) : g_state == ST_PAUSED ? T(S_RESUME) : T(S_START));
                AppendMenuW(mn, MF_STRING, ID_TRAY_STOP, T(S_STOP));
            }
            AppendMenuW(mn, MF_STRING, ID_TRAY_OPTIONS, T(S_OPTIONS));
            AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(mn, MF_STRING, ID_TRAY_EXIT, T(S_EXIT));
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hw);
            TrackPopupMenu(mn, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hw, nullptr);
            PostMessageW(hw, WM_NULL, 0, 0);
            DestroyMenu(mn);
        }
        return 0;
    case WM_DPICHANGED: RebuildMain(HIWORD(wp), (const RECT*)lp); return 0;
    case WM_C2S_SHOW: ShowMain(true); return 0;
    case WM_C2S_QUIT: DestroyWindow(hw); return 0;
    case WM_CLOSE:
        if (g_state != ST_IDLE) { ShowWindow(hw, SW_HIDE); return 0; }
        ShowWindow(hw, SW_HIDE);          // the hotkeys keep working from the tray; "Exit" in its menu quits
        return 0;
    case WM_DESTROY:
        Log(L"exit: main window destroyed, state %d", (int)g_state);
        if (g_state != ST_IDLE) { g_mainWasVisible = false; g_state = ST_RECORDING; FinishRecording(true, true); }
        ClosePanel();
        if (g_trayAdded) { TrayRemove(&g_nid); g_trayAdded = false; }
        Log(L"exit: tray icon removed");
        UnregisterHotKey(hw, HK_DESKTOP); UnregisterHotKey(hw, HK_REGION); UnregisterHotKey(hw, HK_WINDOW); UnregisterHotKey(hw, HK_STOP);
        Log(L"exit: leaving the message loop");
        PostQuitMessage(0);
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) for (int i = 0; i < g_nrecs; ++i) if (g_recs[i]->Active()) g_recs[i]->Stop();
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
int CurrentDpi() { return (int)g_dpi; }

void ShowInExplorer(const wchar_t* path)
{
    wchar_t args[MAX_PATH + 40];
    _snwprintf(args, MAX_PATH + 40, L"/n, /e, /select, \"%s\"", path);
    args[MAX_PATH + 39] = 0;
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args, nullptr, SW_SHOWNORMAL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    g_inst = inst;
    if (cmd && wcsstr(cmd, L"/selftest")) {
        const wchar_t* dir = wcsstr(cmd, L"/selftest") + 9;
        while (*dir == L' ') ++dir;
        return RunSelfTest(*dir ? dir : L".");
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AppLogOpen(L"panel");
    // c2sautotest.exe runs the real program with its own settings key and its own single-instance name.
    wchar_t envKey[160], mutexName[96] = L"Click2Screen-SingleInstance", suffix[40];
    if (GetEnvironmentVariableW(L"C2S_REGKEY", envKey, 160) > 0) SettingsUseKey(envKey);
    if (GetEnvironmentVariableW(L"C2S_INSTANCE", suffix, 40) > 0) wcscat(mutexName, suffix);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kMainClass, nullptr);
        if (other) PostMessageW(other, WM_C2S_SHOW, 0, 0);
        return 0;
    }
    {
        typedef UINT(WINAPI* GetDpiFn)();
        GetDpiFn sys = (GetDpiFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
        if (sys) g_dpi = sys();
        else { HDC dc = GetDC(nullptr); g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    }
    SettingsLoad(g_cfg);
    LangSelect(g_cfg.language);
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&ic);
    MakeFonts();
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = kMainClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = g_icon;
    RegisterClassW(&wc);
    // A window of its own look: no system caption (the program draws the 28-unit one), 206 x 62 units, rounded corners.
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = Sc(kMainW), wh = Sc(kMainH);
    CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, T(S_APP), WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                    (wa.left + wa.right - ww) / 2, (wa.top + wa.bottom - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;
    RoundMain();
    LayoutMainText();
    TrayAdd();
    RegisterHotkeys();
    bool tray = cmd && (wcsstr(cmd, L"/tray") || wcsstr(cmd, L"--tray") || wcsstr(cmd, L"/t ") || wcsstr(cmd, L"-t ") || (wcslen(cmd) >= 2 && (!wcscmp(cmd + wcslen(cmd) - 2, L"/t") || !wcscmp(cmd + wcslen(cmd) - 2, L"-t"))));
    if (!tray) ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    // Development helpers (see README): open the options / the recording panel right away.
    if (cmd && wcsstr(cmd, L"/dev-options")) PostMessageW(g_main, WM_COMMAND, IDC_OPTIONS, 0);
    if (cmd && wcsstr(cmd, L"/dev-panel")) PostMessageW(g_main, WM_COMMAND, IDC_REC_DESKTOP, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_main && IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
