// Click2Screen — main window, recording panel, tray icon, global hotkeys and screenshot / recording flows.
#include "c2s.h"
#include "uiids.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>

// ---------------------------------------------------------------------------------------------------------------
namespace {

HINSTANCE g_inst;
HWND   g_main, g_panel;
HFONT  g_font, g_fontBold;
HICON  g_icon, g_iconSmall;
Settings g_cfg;
State  g_state = ST_IDLE;
Recorder* g_rec = nullptr;
RecParams g_target;
wchar_t g_outPath[MAX_PATH];
int    g_countdown;
bool   g_trayAdded, g_hideAsked, g_mainWasVisible = true, g_busyShot;
UINT   g_msgTaskbar;
NOTIFYICONDATAW g_nid;
const wchar_t* kMainClass = L"Click2ScreenMain";

UINT g_dpi = 96;                 // DPI of the monitor the main window is on (WM_DPICHANGED keeps it current)
int Sc(int v) { return MulDiv(v, (int)g_dpi, 96); }

void MakeFonts()
{
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_SEMIBOLD; lf.lfHeight = lf.lfHeight * 3 / 2;
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

void Beep(bool tick) { if (g_cfg.sounds) MessageBeep(tick ? MB_OK : MB_ICONASTERISK); }

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

void UpdatePanel()
{
    if (!g_panel) return;
    double sec = g_rec ? g_rec->ElapsedSec() : 0.0;
    int t = (int)sec;
    wchar_t buf[96];
    _snwprintf(buf, 96, L"%s  %02d:%02d:%02d", T(S_TIME), t / 3600, t / 60 % 60, t % 60);
    SetWindowTextW(GetDlgItem(g_panel, IDC_P_TIME), buf);
    double mb = g_rec && g_state != ST_ARMED && g_state != ST_COUNTDOWN ? g_rec->FileBytes() / 1048576.0 : 0.0;
    _snwprintf(buf, 96, L"%s  %.1f Mb", T(S_SIZE), mb);
    SetWindowTextW(GetDlgItem(g_panel, IDC_P_SIZE), buf);
    const wchar_t* b = T(S_START);
    if (g_state == ST_COUNTDOWN) { _snwprintf(buf, 96, T(S_STARTING_IN), g_countdown); b = buf; }
    else if (g_state == ST_RECORDING) b = T(S_PAUSE);
    else if (g_state == ST_PAUSED) b = T(S_RESUME);
    SetWindowTextW(GetDlgItem(g_panel, IDC_P_START), b);
    EnableWindow(GetDlgItem(g_panel, IDC_P_START), g_state != ST_COUNTDOWN);
    EnableWindow(GetDlgItem(g_panel, IDC_P_STOP), g_state == ST_RECORDING || g_state == ST_PAUSED || g_state == ST_ARMED || g_state == ST_COUNTDOWN);
    SetWindowTextW(GetDlgItem(g_panel, IDC_P_HINT), T(S_STOP_HINT));
    TrayTip();
}

void FinishRecording(bool ok, bool userStop);

void BeginRecord()
{
    Log(L"record: starting");
    wchar_t err[300] = L"";
    if (!BuildOutputPath(g_cfg.vidFolder, g_cfg.fileName, L"mp4", L"VidCounter", g_outPath, MAX_PATH, err, 300)) {
        Warn(err); g_state = ST_ARMED; UpdatePanel(); return;
    }
    g_target.cfg = g_cfg;
    wcscpy(g_target.path, g_outPath);
    if (!g_rec) g_rec = new Recorder;
    err[0] = 0;
    if (!g_rec->Start(g_target, err, 300)) {
        Warn(err[0] ? err : T(S_ERR_CAPTURE));
        delete g_rec; g_rec = nullptr;
        g_state = ST_ARMED; UpdatePanel(); return;
    }
    if (err[0]) Warn(err);       // the microphone was not available
    g_state = ST_RECORDING;
    Log(L"record: started, %dx%d -> %s", g_rec->Width(), g_rec->Height(), g_outPath);
    Beep(false);
    SetTimer(g_panel, TIMER_UI, 250, nullptr);
    UpdatePanel();
}

void ClosePanel()
{
    if (g_panel) {
        KillTimer(g_panel, TIMER_UI); KillTimer(g_panel, TIMER_COUNTDOWN);
        DestroyWindow(g_panel); g_panel = nullptr;
    }
    UnregisterHotKey(g_main, HK_STOP);
}

void FinishRecording(bool ok, bool userStop)
{
    (void)userStop;
    bool saved = false;
    if (g_rec) {
        if (g_rec->Active()) saved = g_rec->Stop();
        delete g_rec; g_rec = nullptr;
    }
    bool hadFile = ok && saved;
    Log(L"record: finished (saved=%d) %s", (int)hadFile, g_outPath);
    g_state = ST_IDLE;
    ClosePanel();
    TrayTip();
    if (hadFile) {
        Beep(false);
        wchar_t msg[MAX_PATH + 40];
        _snwprintf(msg, MAX_PATH + 40, T(S_SAVED), g_outPath);
        SetStatus(msg);
        Balloon(T(S_APP), msg);
        if (g_cfg.openFolder) ShowInExplorer(g_outPath);
    } else if (g_outPath[0] && ok) {
        wchar_t msg[MAX_PATH + 100];
        _snwprintf(msg, MAX_PATH + 100, T(S_ERR_SAVE), g_outPath);
        Warn(msg);
    }
    if (g_mainWasVisible) ShowMain(true);
}

void StopRecording(bool userStop)
{
    if (g_state == ST_IDLE) return;
    if (g_state == ST_ARMED || g_state == ST_COUNTDOWN) {       // nothing recorded yet
        if (g_rec) { delete g_rec; g_rec = nullptr; }
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
            UpdatePanel();
        } else BeginRecord();
        break;
    case ST_RECORDING: g_rec->SetPaused(true); g_state = ST_PAUSED; UpdatePanel(); break;
    case ST_PAUSED:    g_rec->SetPaused(false); g_state = ST_RECORDING; UpdatePanel(); break;
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

LRESULT CALLBACK PanelProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
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
            else { Beep(true); UpdatePanel(); }
        } else if (wp == TIMER_UI) {
            if (g_rec && (g_rec->LimitReached() || g_rec->Failed())) { FinishRecording(true, false); return 0; }
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
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = PanelProc; wc.hInstance = g_inst; wc.lpszClassName = L"Click2ScreenPanel";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = g_icon;
        RegisterClassW(&wc); reg = true;
    }
    int w = Sc(430), h = Sc(120);
    RECT r = { 0, 0, w, h };
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Click2ScreenPanel", T(S_PANEL_TITLE), WS_CAPTION | WS_SYSMENU,
                              (wa.left + wa.right - ww) / 2, wa.bottom - wh - Sc(20), ww, wh, nullptr, nullptr, g_inst, nullptr);
    // The panel must not appear in the recording (Windows 10 2004 and later).
    typedef BOOL(WINAPI* SetAff)(HWND, DWORD);
    SetAff aff = (SetAff)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowDisplayAffinity");
    if (aff) aff(g_panel, 0x11 /*WDA_EXCLUDEFROMCAPTURE*/);
    int m = Sc(12), y = Sc(10);
    MakeCtl(g_panel, L"STATIC", L"", 0, m, y, Sc(190), Sc(20), IDC_P_TIME);
    MakeCtl(g_panel, L"STATIC", L"", 0, m + Sc(190), y, Sc(130), Sc(20), IDC_P_SIZE);
    MakeCtl(g_panel, L"STATIC", L"", SS_RIGHT, w - m - Sc(80), y, Sc(80), Sc(20), IDC_P_HINT);
    y += Sc(34);
    int bw = (w - 2 * m - 2 * Sc(8)) / 3, bh = Sc(34);
    MakeCtl(g_panel, L"BUTTON", T(S_START), WS_TABSTOP | BS_PUSHBUTTON, m, y, bw, bh, IDC_P_START);
    MakeCtl(g_panel, L"BUTTON", T(S_STOP), WS_TABSTOP | BS_PUSHBUTTON, m + bw + Sc(8), y, bw, bh, IDC_P_STOP);
    MakeCtl(g_panel, L"BUTTON", T(S_HIDE), WS_TABSTOP | BS_PUSHBUTTON, m + 2 * (bw + Sc(8)), y, bw, bh, IDC_P_HIDE);
    UpdatePanel();
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
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    }
}

void Shot(Mode mode)
{
    Log(L"shot: mode %d, busy %d, state %d", (int)mode, (int)g_busyShot, (int)g_state);
    if (g_busyShot) return;
    if (g_state == ST_RECORDING || g_state == ST_PAUSED || g_state == ST_COUNTDOWN) return;   // hotkeys during a recording are ignored
    g_busyShot = true;
    bool wasVisible = g_main && IsWindowVisible(g_main);
    RECT rc; HWND hw = nullptr;
    wchar_t err[300];
    if (!EnsureFolder(g_cfg.imgFolder, err, 300)) { Warn(err); g_busyShot = false; return; }
    if (mode == M_DESKTOP) { rc = VirtualScreen(); if (wasVisible) { ShowMain(false); PumpFor(250); } }
    else if (!ChooseTarget(mode, &rc, &hw)) { if (wasVisible) ShowMain(true); g_busyShot = false; return; }
    else if (wasVisible) ShowMain(false);
    for (int i = g_cfg.delaySec; i > 0; --i) { Beep(true); PumpFor(1000); }
    if (hw) { if (!IsWindow(hw) || !WindowVisibleRect(hw, &rc)) { if (wasVisible) ShowMain(true); g_busyShot = false; return; } }
    if (mode != M_DESKTOP) PumpFor(300);          // let the screen show the desktop again, not the dimmed picker
    HBITMAP bmp = CaptureRect(rc, g_cfg.capMouse, g_cfg.highlight);
    if (wasVisible) ShowMain(true);
    if (!bmp) { Warn(T(S_ERR_CAPTURE)); g_busyShot = false; return; }
    static const wchar_t* ext[] = { L"png", L"jpg", L"bmp" };
    wchar_t path[MAX_PATH];
    if (!BuildOutputPath(g_cfg.imgFolder, g_cfg.fileName, ext[g_cfg.imageFormat], L"ImgCounter", path, MAX_PATH, err, 300)) {
        Warn(err); DeleteObject(bmp); g_busyShot = false; return;
    }
    if (!SaveBitmap(bmp, path, g_cfg.imageFormat)) {
        wchar_t msg[MAX_PATH + 100]; _snwprintf(msg, MAX_PATH + 100, T(S_ERR_SAVE), path);
        Warn(msg); DeleteObject(bmp); g_busyShot = false; return;
    }
    Beep(false);
    wchar_t msg[MAX_PATH + 40]; _snwprintf(msg, MAX_PATH + 40, T(S_SAVED), path);
    SetStatus(msg);
    if (g_cfg.openEditor) OpenEditor(g_main, bmp, path, g_cfg.imageFormat);   // takes the bitmap
    else DeleteObject(bmp);
    if (g_cfg.openFolder) ShowInExplorer(path);
    g_busyShot = false;
}

// ---- options -----------------------------------------------------------------------------------------------------
struct OptState { Settings s; bool ok, done; HWND dlg; HWND tab; HWND pg[4][32]; int np[4]; };

HWND OAdd(OptState* o, int page, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = MakeCtl(parent, cls, text, style, x, y, w, h, id);
    if (o->np[page] < 32) o->pg[page][o->np[page]++] = c;
    return c;
}

int ComboAdd(HWND cb, const wchar_t* t) { return (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t); }

void OptLoad(OptState* o)
{
    HWND d = o->dlg; Settings& s = o->s;
    auto chk = [&](int id, bool v) { CheckDlgButton(d, id, v ? BST_CHECKED : BST_UNCHECKED); };
    chk(O_AUTOSTART, s.runAtStartup); chk(O_MOUSE, s.capMouse); chk(O_HILITE, s.highlight); chk(O_OPENFOLDER, s.openFolder);
    chk(O_OPENEDITOR, s.openEditor); chk(O_SOUNDS, s.sounds); chk(O_MIC, s.mic);
    SetDlgItemInt(d, O_DELAY, s.delaySec, FALSE);
    SendDlgItemMessageW(d, O_VCODEC, CB_SETCURSEL, s.videoCodec, 0);
    SendDlgItemMessageW(d, O_ACODEC, CB_SETCURSEL, s.audioCodec, 0);
    SendDlgItemMessageW(d, O_VQ, CB_SETCURSEL, s.videoQuality, 0);
    SendDlgItemMessageW(d, O_AQ, CB_SETCURSEL, s.audioQuality, 0);
    static const int fpsv[] = { 10, 15, 20, 25, 30, 45, 60 };
    int fi = 3; for (int i = 0; i < 7; ++i) if (fpsv[i] == s.fps) fi = i;
    SendDlgItemMessageW(d, O_FPS, CB_SETCURSEL, fi, 0);
    SetDlgItemInt(d, O_DH, s.durH, FALSE); SetDlgItemInt(d, O_DM, s.durM, FALSE); SetDlgItemInt(d, O_DS, s.durS, FALSE);
    SetDlgItemTextW(d, O_VIDFOLDER, s.vidFolder); SetDlgItemTextW(d, O_IMGFOLDER, s.imgFolder);
    SendDlgItemMessageW(d, O_IMGFMT, CB_SETCURSEL, s.imageFormat, 0);
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
    s.delaySec = num(O_DELAY, 0, 60);
    s.videoCodec = (int)SendDlgItemMessageW(d, O_VCODEC, CB_GETCURSEL, 0, 0);
    s.audioCodec = (int)SendDlgItemMessageW(d, O_ACODEC, CB_GETCURSEL, 0, 0);
    s.videoQuality = (int)SendDlgItemMessageW(d, O_VQ, CB_GETCURSEL, 0, 0);
    s.audioQuality = (int)SendDlgItemMessageW(d, O_AQ, CB_GETCURSEL, 0, 0);
    static const int fpsv[] = { 10, 15, 20, 25, 30, 45, 60 };
    int fi = (int)SendDlgItemMessageW(d, O_FPS, CB_GETCURSEL, 0, 0); if (fi < 0 || fi > 6) fi = 3;
    s.fps = fpsv[fi];
    s.durH = num(O_DH, 0, 99); s.durM = num(O_DM, 0, 59); s.durS = num(O_DS, 0, 59);
    GetDlgItemTextW(d, O_VIDFOLDER, s.vidFolder, MAX_PATH); GetDlgItemTextW(d, O_IMGFOLDER, s.imgFolder, MAX_PATH);
    s.imageFormat = (int)SendDlgItemMessageW(d, O_IMGFMT, CB_GETCURSEL, 0, 0); if (s.imageFormat < 0) s.imageFormat = 0;
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
    for (int p = 0; p < 4; ++p)
        for (int i = 0; i < o->np[p]; ++i) ShowWindow(o->pg[p][i], p == page ? SW_SHOW : SW_HIDE);
}

LRESULT CALLBACK OptProc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    OptState* o = (OptState*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return DefWindowProcW(hw, m, wp, lp);
    case WM_NOTIFY:
        if (((NMHDR*)lp)->idFrom == O_TABS && ((NMHDR*)lp)->code == TCN_SELCHANGE)
            ShowPage(o, TabCtrl_GetCurSel(o->tab));
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
    int W = Sc(660), H = Sc(430);
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND owner = g_main && IsWindowVisible(g_main) ? g_main : nullptr;
    o.dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"Click2ScreenOptions", T(S_OPT_TITLE), WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr, g_inst, &o);
    HWND d = o.dlg;
    int mg = Sc(10), tabH = Sc(32);
    // Flat tab buttons without a page body: the controls stay on the plain window background (no mismatched colours).
    o.tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_BUTTONS | TCS_FLATBUTTONS | TCS_FOCUSNEVER,
                            mg, mg, W - 2 * mg, tabH, d, (HMENU)(INT_PTR)O_TABS, g_inst, nullptr);
    SendMessageW(o.tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    const Str tabs[] = { S_OPT_GENERAL, S_OPT_VIDEO_AUDIO, S_OPT_FILES, S_OPT_LANG };
    for (int i = 0; i < 4; ++i) { TCITEMW ti; memset(&ti, 0, sizeof(ti)); ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)T(tabs[i]); TabCtrl_InsertItem(o.tab, i, &ti); }
    RECT ar = { mg, mg + tabH + Sc(8), W - mg, H - Sc(56) };
    int x0 = ar.left + Sc(8), y = ar.top + Sc(8), cw = ar.right - ar.left - Sc(16), lh = Sc(22), gap = Sc(30);
    auto chkbox = [&](int page, Str s, int id) { OAdd(&o, page, d, L"BUTTON", T(s), WS_TABSTOP | BS_AUTOCHECKBOX, x0, y, cw, lh, id); y += gap; };

    // page 0: general
    chkbox(0, S_OPT_AUTOSTART, O_AUTOSTART); chkbox(0, S_OPT_MOUSE, O_MOUSE); chkbox(0, S_OPT_HIGHLIGHT, O_HILITE);
    chkbox(0, S_OPT_OPEN_FOLDER, O_OPENFOLDER); chkbox(0, S_OPT_OPEN_EDITOR, O_OPENEDITOR); chkbox(0, S_OPT_SOUNDS, O_SOUNDS);
    chkbox(0, S_OPT_MIC, O_MIC);
    OAdd(&o, 0, d, L"STATIC", T(S_OPT_DELAY), 0, x0, y + Sc(3), Sc(200), lh, 0);
    OAdd(&o, 0, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER, x0 + Sc(210), y, Sc(60), lh, O_DELAY);
    OAdd(&o, 0, d, L"STATIC", T(S_OPT_SECONDS), 0, x0 + Sc(280), y + Sc(3), Sc(100), lh, 0);

    // page 1: video and audio
    y = ar.top + Sc(8);
    auto combo = [&](Str lab, int id, const wchar_t* i0, const wchar_t* i1, const wchar_t* i2 = nullptr, const wchar_t* i3 = nullptr,
                     const wchar_t* i4 = nullptr, const wchar_t* i5 = nullptr, const wchar_t* i6 = nullptr) {
        OAdd(&o, 1, d, L"STATIC", T(lab), 0, x0, y + Sc(3), Sc(170), lh, 0);
        HWND cb = OAdd(&o, 1, d, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x0 + Sc(180), y, cw - Sc(190), Sc(200), id);
        const wchar_t* items[] = { i0, i1, i2, i3, i4, i5, i6 };
        for (auto t : items) if (t) ComboAdd(cb, t);
        y += gap;
    };
    combo(S_OPT_VCODEC, O_VCODEC, T(S_VC_BEST), T(S_VC_FAST));
    combo(S_OPT_VQUALITY, O_VQ, T(S_Q_HIGH), T(S_Q_MEDIUM), T(S_Q_LOW));
    combo(S_OPT_ACODEC, O_ACODEC, T(S_AC_AAC), T(S_AC_MP3));
    combo(S_OPT_AQUALITY, O_AQ, T(S_Q_HIGH), T(S_Q_MEDIUM), T(S_Q_LOW));
    combo(S_OPT_FPS, O_FPS, L"10", L"15", L"20", L"25", L"30", L"45", L"60");
    OAdd(&o, 1, d, L"STATIC", T(S_OPT_DURATION), 0, x0, y + Sc(3), cw, lh, 0); y += gap;
    OAdd(&o, 1, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER, x0, y, Sc(50), lh, O_DH);
    OAdd(&o, 1, d, L"STATIC", T(S_OPT_HOURS), 0, x0 + Sc(56), y + Sc(3), Sc(60), lh, 0);
    OAdd(&o, 1, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER, x0 + Sc(130), y, Sc(50), lh, O_DM);
    OAdd(&o, 1, d, L"STATIC", T(S_OPT_MINUTES), 0, x0 + Sc(186), y + Sc(3), Sc(70), lh, 0);
    OAdd(&o, 1, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER, x0 + Sc(270), y, Sc(50), lh, O_DS);
    OAdd(&o, 1, d, L"STATIC", T(S_OPT_SECONDS), 0, x0 + Sc(326), y + Sc(3), Sc(70), lh, 0);

    // page 2: files
    y = ar.top + Sc(8);
    OAdd(&o, 2, d, L"STATIC", T(S_OPT_VIDFOLDER), 0, x0, y, cw, lh, 0); y += Sc(22);
    OAdd(&o, 2, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x0, y, cw - Sc(100), lh, O_VIDFOLDER);
    OAdd(&o, 2, d, L"BUTTON", T(S_BROWSE), WS_TABSTOP | BS_PUSHBUTTON, x0 + cw - Sc(92), y - Sc(1), Sc(92), lh + Sc(2), O_BRVID); y += Sc(34);
    OAdd(&o, 2, d, L"STATIC", T(S_OPT_IMGFOLDER), 0, x0, y, cw, lh, 0); y += Sc(22);
    OAdd(&o, 2, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x0, y, cw - Sc(100), lh, O_IMGFOLDER);
    OAdd(&o, 2, d, L"BUTTON", T(S_BROWSE), WS_TABSTOP | BS_PUSHBUTTON, x0 + cw - Sc(92), y - Sc(1), Sc(92), lh + Sc(2), O_BRIMG); y += Sc(34);
    OAdd(&o, 2, d, L"STATIC", T(S_OPT_IMGFORMAT), 0, x0, y + Sc(3), Sc(170), lh, 0);
    HWND fm = OAdd(&o, 2, d, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, x0 + Sc(180), y, Sc(120), Sc(120), O_IMGFMT);
    ComboAdd(fm, L"PNG"); ComboAdd(fm, L"JPG"); ComboAdd(fm, L"BMP"); y += gap;
    OAdd(&o, 2, d, L"STATIC", T(S_OPT_TEMPLATE), 0, x0, y, cw, lh, 0); y += Sc(22);
    OAdd(&o, 2, d, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x0, y, cw, lh, O_TPL); y += Sc(30);
    OAdd(&o, 2, d, L"STATIC", T(S_OPT_TEMPLATE_TIPS), 0, x0, y, cw, Sc(80), 0);

    // page 3: language
    y = ar.top + Sc(8);
    OAdd(&o, 3, d, L"STATIC", T(S_OPT_LANG), 0, x0, y + Sc(3), Sc(170), lh, 0);
    HWND lg = OAdd(&o, 3, d, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x0 + Sc(180), y, Sc(260), Sc(320), O_LANG);
    ComboAdd(lg, T(S_LANG_AUTO));
    for (int i = 0; i < LangCount(); ++i) ComboAdd(lg, LangName(i));

    int by = H - Sc(40);
    MakeCtl(d, L"BUTTON", T(S_RESTORE_DEFAULT), WS_TABSTOP | BS_PUSHBUTTON, mg, by, Sc(130), Sc(28), O_DEFAULTS);
    MakeCtl(d, L"BUTTON", T(S_OPT_EXPORT), WS_TABSTOP | BS_PUSHBUTTON, mg + Sc(136), by, Sc(150), Sc(28), O_EXPORT);
    MakeCtl(d, L"BUTTON", T(S_OPT_IMPORT), WS_TABSTOP | BS_PUSHBUTTON, mg + Sc(292), by, Sc(150), Sc(28), O_IMPORT);
    MakeCtl(d, L"BUTTON", T(S_BTN_OK), WS_TABSTOP | BS_DEFPUSHBUTTON, W - mg - Sc(180), by, Sc(84), Sc(28), IDOK);
    MakeCtl(d, L"BUTTON", T(S_BTN_CANCEL), WS_TABSTOP | BS_PUSHBUTTON, W - mg - Sc(90), by, Sc(84), Sc(28), IDCANCEL);
    OptLoad(&o);
    ShowPage(&o, 0);

    // centre over the owner / screen
    RECT wr; GetWindowRect(d, &wr);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(d, HWND_TOPMOST, (wa.left + wa.right - (wr.right - wr.left)) / 2, (wa.top + wa.bottom - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE);
    ShowWindow(d, SW_SHOW);
    if (owner) EnableWindow(owner, FALSE);
    MSG msg;
    while (!o.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(d);
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
    struct { int id; Str s; } t[] = {
        {IDC_REC_DESKTOP, S_REC_DESKTOP}, {IDC_REC_REGION, S_REC_REGION}, {IDC_REC_WINDOW, S_REC_WINDOW},
        {IDC_SHOT_DESKTOP, S_SHOT_DESKTOP}, {IDC_SHOT_REGION, S_SHOT_REGION}, {IDC_SHOT_WINDOW, S_SHOT_WINDOW},
        {IDC_OPTIONS, S_OPTIONS}, {IDC_HINT, S_HOTKEYS_HINT}, {IDC_GRP_VIDEO, S_GRP_VIDEO}, {IDC_GRP_SHOT, S_GRP_SHOT} };
    for (auto& i : t) SetWindowTextW(GetDlgItem(g_main, i.id), T(i.s));
}

void CreateMainControls(HWND hw)
{
    int m = Sc(14), bw = Sc(140), bh = Sc(52), gap = Sc(10), gh = Sc(98), y = Sc(14);
    MakeCtl(hw, L"BUTTON", L"", BS_GROUPBOX, m, y, 3 * bw + 2 * gap + 2 * Sc(10), gh, IDC_GRP_VIDEO);
    int bx = m + Sc(10), by = y + Sc(26);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, bx, by, bw, bh, IDC_REC_DESKTOP);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, bx + bw + gap, by, bw, bh, IDC_REC_REGION);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, bx + 2 * (bw + gap), by, bw, bh, IDC_REC_WINDOW);
    y += gh + Sc(10);
    MakeCtl(hw, L"BUTTON", L"", BS_GROUPBOX, m, y, 3 * bw + 2 * gap + 2 * Sc(10), gh, IDC_GRP_SHOT);
    by = y + Sc(26);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, bx, by, bw, bh, IDC_SHOT_DESKTOP);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, bx + bw + gap, by, bw, bh, IDC_SHOT_REGION);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, bx + 2 * (bw + gap), by, bw, bh, IDC_SHOT_WINDOW);
    y += gh + Sc(12);
    MakeCtl(hw, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, m, y, Sc(110), Sc(30), IDC_OPTIONS);
    MakeCtl(hw, L"STATIC", L"", SS_LEFT, m + Sc(124), y + Sc(7), Sc(300), Sc(20), IDC_HINT);
    y += Sc(42);
    MakeCtl(hw, L"STATIC", T(S_STATUS_READY), SS_LEFT | SS_PATHELLIPSIS, m, y, 3 * bw + 2 * gap + 2 * Sc(10), Sc(20), IDC_STATUS);
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
        SetWindowPos(g_main, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REC_DESKTOP: Arm(M_DESKTOP); break;
        case IDC_REC_REGION:  Arm(M_REGION); break;
        case IDC_REC_WINDOW:  Arm(M_WINDOW); break;
        case IDC_SHOT_DESKTOP: PostMessageW(hw, WM_HOTKEY, HK_DESKTOP, 0); break;
        case IDC_SHOT_REGION:  PostMessageW(hw, WM_HOTKEY, HK_REGION, 0); break;
        case IDC_SHOT_WINDOW:  PostMessageW(hw, WM_HOTKEY, HK_WINDOW, 0); break;
        case IDC_OPTIONS: case ID_TRAY_OPTIONS: if (OptionsDialog()) ApplyLanguage(); break;
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
        if (wp && g_rec && g_rec->Active()) g_rec->Stop();
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
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&ic);
    MakeFonts();
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = kMainClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = g_icon;
    RegisterClassW(&wc);
    int cw = 3 * Sc(140) + 2 * Sc(10) + 2 * Sc(10) + 2 * Sc(14), ch = Sc(14) + 2 * Sc(98) + Sc(10) + Sc(12) + Sc(42) + Sc(34);
    RECT r = { 0, 0, cw, ch };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    CreateWindowExW(0, kMainClass, T(S_APP), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_main) return 1;
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
