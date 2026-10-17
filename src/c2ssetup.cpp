// Click2Screen-Setup.exe — graphical installer.
//
// Installs Click2Screen.exe (and itself, for removal) to %ProgramFiles%\Click2Screen, creates the shortcuts, registers the
// program in "Apps & features" and starts it afterwards. The same window removes it again. Runs elevated (manifest).
// Expects Click2Screen.exe next to it. Command line (for scripts): /install or /remove, with /silent for no window;
// the exit code is 0 on success. The settings and the captures the user made are never touched by the removal.
#include "c2s.h"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

namespace {

// ---- strings: the shared 17-language table (lang.cpp)
int g_lang = 0;          // language chosen in the window: index in the table

// ---- state -----------------------------------------------------------------------------------------------------
enum { IDC_TITLE = 100, IDC_SUB, IDC_STATE, IDC_DESKTOP, IDC_START, IDC_AUTO, IDC_LOG, IDC_INSTALL, IDC_REMOVE, IDC_LAUNCH, IDC_CLOSE, IDC_LANGLBL, IDC_LANG };
enum { ACT_INSTALL = 1, ACT_REMOVE };
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_C2S_QUIT (WM_APP + 3)

const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Click2Screen";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font, g_fontTitle;
HICON g_icon;
HANDLE g_worker;
int g_action;
bool g_ok, g_silent, g_selfDelete;
bool g_wantDesktop = true, g_wantStart = true, g_wantAuto = false;

HWND Ctl(int id) { return GetDlgItem(g_wnd, id); }
UINT g_dpi = 96;
int Sc(int v) { return MulDiv(v, (int)g_dpi, 96); }

void Say(const wchar_t* fmt, ...)
{
    wchar_t line[600];
    va_list ap; va_start(ap, fmt); _vsnwprintf(line, 590, fmt, ap); va_end(ap);
    line[590] = 0;
    Log(L"%s", line);
    if (!g_silent && g_wnd) PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM)_wcsdup(line));
}

bool KnownFolder(const KNOWNFOLDERID& id, wchar_t* out)
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) return false;
    wcsncpy(out, p, MAX_PATH - 1); out[MAX_PATH - 1] = 0;
    CoTaskMemFree(p);
    return true;
}

bool ProgramDir(wchar_t* dir)
{
    if (!KnownFolder(FOLDERID_ProgramFiles, dir)) return false;
    wcsncat(dir, L"\\Click2Screen", MAX_PATH - wcslen(dir) - 1);
    return true;
}

int Dpi96() { return 96; }

void SiblingPath(const wchar_t* name, wchar_t* out)
{
    GetModuleFileNameW(nullptr, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\');
    wcscpy(slash ? slash + 1 : out, name);
}

bool GetRegStr(HKEY root, const wchar_t* key, const wchar_t* name, wchar_t* out, DWORD cap)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD n = cap * sizeof(wchar_t), t = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &t, (BYTE*)out, &n) == ERROR_SUCCESS && t == REG_SZ;
    RegCloseKey(k);
    if (ok) out[cap - 1] = 0;
    return ok;
}

void SetRegStr(HKEY k, const wchar_t* name, const wchar_t* v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

// ---- steps -----------------------------------------------------------------------------------------------------
void CloseRunningApp()
{
    for (int tries = 0; tries < 3; ++tries) {
        HWND app = FindWindowW(L"Click2ScreenMain", nullptr);
        if (!app) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(app, &pid);
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(app, WM_C2S_QUIT, 0, 0);             // finishes a recording in progress and exits
        DWORD w = proc ? WaitForSingleObject(proc, 8000) : WAIT_FAILED;
        if (proc) CloseHandle(proc);
        Say(L"Click2Screen (pid %lu) closed: %s", pid, w == WAIT_OBJECT_0 ? L"yes" : L"not in time");
        if (!proc) Sleep(500);
    }
}

bool CopyOne(const wchar_t* from, const wchar_t* to)
{
    for (int tries = 0; tries < 20; ++tries) {
        if (CopyFileW(from, to, FALSE)) return true;
        Sleep(250);
    }
    return false;
}

bool MakeLink(const wchar_t* linkPath, const wchar_t* target, const wchar_t* workDir, const wchar_t* desc)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) return false;
    link->SetPath(target);
    link->SetWorkingDirectory(workDir);
    link->SetDescription(desc);
    link->SetIconLocation(target, 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(linkPath, TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

void LinkPaths(wchar_t* desktop, wchar_t* menu)
{
    desktop[0] = menu[0] = 0;
    wchar_t d[MAX_PATH], m[MAX_PATH];
    if (KnownFolder(FOLDERID_PublicDesktop, d)) _snwprintf(desktop, MAX_PATH, L"%s\\Click2Screen.lnk", d);
    if (KnownFolder(FOLDERID_CommonPrograms, m)) _snwprintf(menu, MAX_PATH, L"%s\\Click2Screen.lnk", m);
}

bool DoInstall()
{
    Say(T(S_SU_LOG_INSTALL));
    wchar_t dir[MAX_PATH];
    if (!ProgramDir(dir)) { Say(T(S_SU_LOG_NODIR)); return false; }
    wchar_t srcExe[MAX_PATH], srcSetup[MAX_PATH], srcUn[MAX_PATH];
    SiblingPath(L"Click2Screen.exe", srcExe);
    GetModuleFileNameW(nullptr, srcSetup, MAX_PATH);
    SiblingPath(L"uninstall.cmd", srcUn);
    if (GetFileAttributesW(srcExe) == INVALID_FILE_ATTRIBUTES) { Say(T(S_SU_LOG_NOSRC)); return false; }
    Say(T(S_SU_LOG_CLOSEAPP));
    CloseRunningApp();
    Say(T(S_SU_LOG_COPY), dir);
    SHCreateDirectoryExW(nullptr, dir, nullptr);
    wchar_t dstExe[MAX_PATH], dstSetup[MAX_PATH], dstUn[MAX_PATH];
    _snwprintf(dstExe, MAX_PATH, L"%s\\Click2Screen.exe", dir);
    _snwprintf(dstSetup, MAX_PATH, L"%s\\Click2Screen-Setup.exe", dir);
    _snwprintf(dstUn, MAX_PATH, L"%s\\uninstall.cmd", dir);
    if (!CopyOne(srcExe, dstExe)) { Say(T(S_SU_LOG_COPY_FAIL), L"Click2Screen.exe", GetLastError()); return false; }
    wchar_t a[MAX_PATH], b[MAX_PATH];
    wcscpy(a, srcSetup); wcscpy(b, dstSetup);
    if (_wcsicmp(a, b) != 0 && !CopyOne(srcSetup, dstSetup)) { Say(T(S_SU_LOG_COPY_FAIL), L"Click2Screen-Setup.exe", GetLastError()); return false; }
    if (GetFileAttributesW(srcUn) != INVALID_FILE_ATTRIBUTES) CopyOne(srcUn, dstUn);

    Say(T(S_SU_LOG_REG));
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        wchar_t un[MAX_PATH + 24], icon[MAX_PATH + 4];
        _snwprintf(un, MAX_PATH + 24, L"\"%s\" /remove", dstSetup);
        _snwprintf(icon, MAX_PATH + 4, L"%s,0", dstExe);
        SetRegStr(k, L"DisplayName", L"Click2Screen");
        SetRegStr(k, L"DisplayVersion", L"" C2S_VER_STR);
        SetRegStr(k, L"Publisher", L"" C2S_AUTHOR_STR);
        SetRegStr(k, L"InstallLocation", dir);
        SetRegStr(k, L"DisplayIcon", icon);
        SetRegStr(k, L"UninstallString", un);
        DWORD one = 1;
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        WIN32_FILE_ATTRIBUTE_DATA fd;
        if (GetFileAttributesExW(dstExe, GetFileExInfoStandard, &fd)) {
            DWORD kb = (fd.nFileSizeLow + 1023) / 1024;
            RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&kb, sizeof(kb));
        }
        RegCloseKey(k);
    }

    wchar_t desk[MAX_PATH], menu[MAX_PATH];
    LinkPaths(desk, menu);
    Say(T(S_SU_LOG_LINKS));
    if (g_wantDesktop && desk[0]) MakeLink(desk, dstExe, dir, L"Click2Screen"); else if (desk[0]) DeleteFileW(desk);
    if (g_wantStart && menu[0]) MakeLink(menu, dstExe, dir, L"Click2Screen"); else if (menu[0]) DeleteFileW(menu);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        if (g_wantAuto) {
            wchar_t v[MAX_PATH + 16];
            _snwprintf(v, MAX_PATH + 16, L"\"%s\" /tray", dstExe);
            SetRegStr(k, L"Click2Screen", v);
            Say(T(S_SU_LOG_AUTO_ON));
        } else {
            RegDeleteValueW(k, L"Click2Screen");
            Say(T(S_SU_LOG_AUTO_OFF));
        }
        RegCloseKey(k);
    }
    Say(T(S_SU_LOG_DONE));
    return true;
}

void ScheduleSelfDelete(const wchar_t* dir)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[2 * MAX_PATH + 160];
    _snwprintf(cmd, 2 * MAX_PATH + 160, L"/c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"", self, dir);
    ShellExecuteW(nullptr, L"open", L"cmd.exe", cmd, nullptr, SW_HIDE);
}

bool DoRemove()
{
    Say(T(S_SU_LOG_REMOVE));
    CloseRunningApp();
    wchar_t desk[MAX_PATH], menu[MAX_PATH], dir[MAX_PATH];
    LinkPaths(desk, menu);
    Say(T(S_SU_LOG_LINKS_DEL));
    if (desk[0]) DeleteFileW(desk);
    if (menu[0]) DeleteFileW(menu);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"Click2Screen"); RegCloseKey(k); }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"Click2Screen"); RegCloseKey(k); }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    if (ProgramDir(dir)) {
        wchar_t f[MAX_PATH];
        _snwprintf(f, MAX_PATH, L"%s\\Click2Screen.exe", dir); for (int i = 0; i < 20 && !DeleteFileW(f) && GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES; ++i) Sleep(250);
        _snwprintf(f, MAX_PATH, L"%s\\uninstall.cmd", dir); DeleteFileW(f);
        wchar_t self[MAX_PATH], want[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        _snwprintf(want, MAX_PATH, L"%s\\Click2Screen-Setup.exe", dir);
        if (_wcsicmp(self, want) == 0) { g_selfDelete = true; ScheduleSelfDelete(dir); }
        else { _snwprintf(f, MAX_PATH, L"%s\\Click2Screen-Setup.exe", dir); DeleteFileW(f); RemoveDirectoryW(dir); }
    }
    Say(T(S_SU_LOG_KEPT));
    Say(T(S_SU_LOG_DONE));
    return true;
}

DWORD WINAPI Worker(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_ok = g_action == ACT_INSTALL ? DoInstall() : DoRemove();
    if (!g_ok) Say(T(S_SU_LOG_FAIL));
    CoUninitialize();
    if (!g_silent) PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

// ---- window ----------------------------------------------------------------------------------------------------
bool Installed(wchar_t* ver, wchar_t* loc)
{
    ver[0] = loc[0] = 0;
    return GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", ver, 64) &&
           (GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", loc, MAX_PATH) || true);
}

void Refresh()
{
    wchar_t ver[64], loc[MAX_PATH], text[MAX_PATH + 100];
    bool inst = Installed(ver, loc);
    if (inst) _snwprintf(text, MAX_PATH + 100, T(S_SU_STATE_INST), ver, loc); else wcscpy(text, T(S_SU_STATE_NONE));
    SetWindowTextW(Ctl(IDC_STATE), text);
    SetWindowTextW(Ctl(IDC_INSTALL), inst ? T(S_SU_UPDATE) : T(S_SU_INSTALL));
    EnableWindow(Ctl(IDC_REMOVE), inst);
    EnableWindow(Ctl(IDC_LAUNCH), inst);
    SetWindowTextW(Ctl(IDC_TITLE), T(S_APP));
    wchar_t sub[200];
    _snwprintf(sub, 200, L"%s — %s", L"" C2S_VER_STR, T(S_SU_SUB));
    SetWindowTextW(Ctl(IDC_SUB), sub);
    SetWindowTextW(Ctl(IDC_DESKTOP), T(S_SU_DESKTOP));
    SetWindowTextW(Ctl(IDC_START), T(S_SU_STARTMENU));
    SetWindowTextW(Ctl(IDC_AUTO), T(S_SU_AUTOSTART));
    SetWindowTextW(Ctl(IDC_REMOVE), T(S_SU_REMOVE));
    SetWindowTextW(Ctl(IDC_LAUNCH), T(S_SU_LAUNCH));
    SetWindowTextW(Ctl(IDC_CLOSE), T(S_SU_CLOSE));
    SetWindowTextW(Ctl(IDC_LANGLBL), T(S_SU_LANG));
}

void Busy(bool busy)
{
    int ids[] = { IDC_INSTALL, IDC_REMOVE, IDC_LAUNCH, IDC_DESKTOP, IDC_START, IDC_AUTO, IDC_LANG };
    for (int id : ids) EnableWindow(Ctl(id), !busy);
    if (!busy) Refresh();
}

void Start(int action)
{
    if (action == ACT_REMOVE && MessageBoxW(g_wnd, T(S_SU_ASK_REMOVE), T(S_APP), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_wantDesktop = IsDlgButtonChecked(g_wnd, IDC_DESKTOP) == BST_CHECKED;
    g_wantStart = IsDlgButtonChecked(g_wnd, IDC_START) == BST_CHECKED;
    g_wantAuto = IsDlgButtonChecked(g_wnd, IDC_AUTO) == BST_CHECKED;
    g_action = action;
    Busy(true);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr, DWORD ex = 0)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, Sc(x), Sc(y), Sc(w), Sc(h), g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

void Launch()
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!ProgramDir(dir)) return;
    _snwprintf(exe, MAX_PATH, L"%s\\Click2Screen.exe", dir);
    // The installer runs elevated; Explorer starts the program with the user's rights.
    ShellExecuteW(nullptr, L"open", L"explorer.exe", exe, nullptr, SW_SHOWNORMAL);
}

void BuildControls()
{
    HWND hw = g_wnd;
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)g_dpi, Dpi96());
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_SEMIBOLD; lf.lfHeight = lf.lfHeight * 2;
    g_fontTitle = CreateFontIndirectW(&lf);
    Add(L"STATIC", L"", SS_LEFT, 20, 14, 330, 34, IDC_TITLE, g_fontTitle);
    Add(L"STATIC", L"", SS_LEFT, 20, 50, 400, 20, IDC_SUB);
    Add(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 20, 78, 520, 20, IDC_STATE);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 108, 340, 22, IDC_DESKTOP);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 134, 340, 22, IDC_START);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 160, 340, 22, IDC_AUTO);
    CheckDlgButton(hw, IDC_DESKTOP, g_wantDesktop ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_START, g_wantStart ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_AUTO, g_wantAuto ? BST_CHECKED : BST_UNCHECKED);
    Add(L"STATIC", L"", SS_RIGHT, 340, 110, 84, 20, IDC_LANGLBL);
    HWND lg = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 430, 106, 110, 300, IDC_LANG);
    for (int i = 0; i < LangCount(); ++i) SendMessageW(lg, CB_ADDSTRING, 0, (LPARAM)LangName(i));
    SendMessageW(lg, CB_SETCURSEL, g_lang, 0);
    Add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, 192, 520, 150, IDC_LOG);
    int bw = 120, y = 354;
    Add(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, y, bw, 32, IDC_INSTALL);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 20 + bw + 10, y, bw, 32, IDC_REMOVE);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 20 + 2 * (bw + 10), y, bw, 32, IDC_LAUNCH);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 20 + 3 * (bw + 10) - 30, y, 90, 32, IDC_CLOSE);
    Refresh();
}

// The window moved to a monitor with another scale: the controls are made again at the new size (the log text,
// the boxes and the language stay).
void Rescale(UINT dpi, const RECT* suggested)
{
    wchar_t* log = nullptr;
    int n = GetWindowTextLengthW(Ctl(IDC_LOG));
    if (n > 0) { log = (wchar_t*)malloc((n + 1) * sizeof(wchar_t)); if (log) GetWindowTextW(Ctl(IDC_LOG), log, n + 1); }
    g_wantDesktop = IsDlgButtonChecked(g_wnd, IDC_DESKTOP) == BST_CHECKED;
    g_wantStart = IsDlgButtonChecked(g_wnd, IDC_START) == BST_CHECKED;
    g_wantAuto = IsDlgButtonChecked(g_wnd, IDC_AUTO) == BST_CHECKED;
    g_dpi = dpi;
    HWND c;
    while ((c = GetWindow(g_wnd, GW_CHILD)) != nullptr) DestroyWindow(c);
    BuildControls();
    if (log) { SetWindowTextW(Ctl(IDC_LOG), log); free(log); }
    if (suggested)
        SetWindowPos(g_wnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g_wnd = hw;
        BuildControls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INSTALL: Start(ACT_INSTALL); break;
        case IDC_REMOVE: Start(ACT_REMOVE); break;
        case IDC_LAUNCH: Launch(); break;
        case IDC_CLOSE: DestroyWindow(hw); break;
        case IDC_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) { g_lang = (int)SendMessageW(Ctl(IDC_LANG), CB_GETCURSEL, 0, 0); LangSelect(g_lang + 1); Refresh(); }
            break;
        }
        return 0;
    case WM_DPICHANGED: Rescale(HIWORD(wp), (const RECT*)lp); return 0;
    case WM_APP_LOG: {
        wchar_t* line = (wchar_t*)lp;
        HWND e = Ctl(IDC_LOG);
        int len = GetWindowTextLengthW(e);
        SendMessageW(e, EM_SETSEL, len, len);
        if (len) SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
        SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)line);
        free(line);
        return 0; }
    case WM_APP_DONE:
        if (g_worker) { WaitForSingleObject(g_worker, 2000); CloseHandle(g_worker); g_worker = nullptr; }
        Busy(false);
        if (g_ok && g_action == ACT_INSTALL) SetFocus(Ctl(IDC_LAUNCH));
        return 0;
    case WM_CLOSE:
        if (g_worker) return 0;                // not in the middle of copying
        DestroyWindow(hw); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show)
{
    g_inst = inst;
    AppLogOpen(L"setup");
    {
        typedef UINT(WINAPI* GetDpiFn)();
        GetDpiFn sys = (GetDpiFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
        if (sys) g_dpi = sys();
        else { HDC dc = GetDC(nullptr); g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    }
    g_lang = LangAutoIndex();
    LangSelect(g_lang + 1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool doInstall = cmd && wcsstr(cmd, L"/install"), doRemove = cmd && wcsstr(cmd, L"/remove");
    g_silent = cmd && wcsstr(cmd, L"/silent");
    if (cmd && wcsstr(cmd, L"/desktop-")) g_wantDesktop = false;
    if (cmd && wcsstr(cmd, L"/startmenu-")) g_wantStart = false;
    if (cmd && wcsstr(cmd, L"/autostart")) g_wantAuto = true;
    if (g_silent && (doInstall || doRemove)) {
        g_action = doInstall ? ACT_INSTALL : ACT_REMOVE;
        Worker(nullptr);
        return g_ok ? 0 : 1;
    }
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"Click2ScreenSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = g_icon;
    RegisterClassW(&wc);
    RECT r = { 0, 0, Sc(560), Sc(404) };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowExW(0, L"Click2ScreenSetup", L"Click2Screen", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    ShowWindow(w, show);
    UpdateWindow(w);
    if (doInstall) PostMessageW(w, WM_COMMAND, IDC_INSTALL, 0);
    else if (doRemove) PostMessageW(w, WM_COMMAND, IDC_REMOVE, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return g_ok || (!doInstall && !doRemove) ? 0 : 1;
}
