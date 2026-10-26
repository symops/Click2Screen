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
#include <wctype.h>

namespace {

// ---- strings: the shared 17-language table (lang.cpp)
int g_lang = 0;          // language chosen in the window: index in the table

// ---- state -----------------------------------------------------------------------------------------------------
enum { IDC_TITLE = 100, IDC_SUB, IDC_STATE, IDC_DESKTOP, IDC_START, IDC_AUTO, IDC_LOG, IDC_INSTALL, IDC_REMOVE, IDC_LAUNCH, IDC_CLOSE, IDC_LANGLBL, IDC_LANG, IDC_PATHLBL, IDC_PATH, IDC_BROWSE, IDC_VERSION, IDC_GROUP, IDC_RUNAFTER, IDC_LINK };
enum { ACT_INSTALL = 1, ACT_REMOVE };
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_C2S_QUIT (WM_APP + 3)

const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Click2Screen";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font, g_fontTitle, g_fontLink;
// The page of the project: opened in the browser only when the user clicks it (the installer itself never talks to a network).
static const wchar_t kProjectUrl[] = L"https://github.com/symops/Click2Screen";
static const wchar_t kProjectText[] = L"github.com/symops/Click2Screen";
HICON g_icon, g_big;       // g_big: the 64-unit icon of the white header
const int kDy = 14;        // one more row in the header: the link to the project goes under the version line when it does not fit beside it
const int kHeadH = 94 + kDy; // the header (icon, name, version, language) is white, as in Speak2Mic / Show2Cam
HANDLE g_worker;
int g_action;
bool g_ok, g_silent, g_selfDelete;
bool g_wantDesktop = true, g_wantStart = true, g_wantAuto = false, g_wantRun = true;
COLORREF g_stateColor = RGB(90, 90, 100);   // not installed: grey; this version: green; another version: amber (as Speak2Mic / Show2Cam)
wchar_t g_dir[MAX_PATH];      // the folder to install to (the window's box, /dir, or what is installed already)

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

bool GetRegStr(HKEY root, const wchar_t* key, const wchar_t* name, wchar_t* out, DWORD cap);

// Where Click2Screen is installed now (the registry), else the default.
bool InstalledDir(wchar_t* dir)
{
    if (GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", dir, MAX_PATH) && dir[0]) return true;
    return ProgramDir(dir);
}

// A usable folder: a full path (drive letter or UNC), no wildcard / reserved characters, room for the file names.
bool ValidDir(wchar_t* dir)
{
    size_t n = wcslen(dir);
    while (n > 3 && (dir[n - 1] == L'\\' || dir[n - 1] == L' ')) dir[--n] = 0;
    if (n < 4 || n > 200) return false;
    bool drive = iswalpha(dir[0]) && dir[1] == L':' && dir[2] == L'\\';
    bool unc = dir[0] == L'\\' && dir[1] == L'\\';
    if (!drive && !unc) return false;
    for (size_t i = drive ? 3 : 2; i < n; ++i)
        if (dir[i] < 32 || wcschr(L"<>:\"|?*", dir[i])) return false;
    if (drive && n == 3) return false;                       // a drive root: its files would mix with everything else
    return true;
}

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

// The program's files in a folder (an old location after the path was changed, or the removal); the folder goes if it is empty.
void RemoveFilesIn(const wchar_t* dir, const wchar_t* keepSelf)
{
    static const wchar_t* names[] = { L"Click2Screen.exe", L"uninstall.cmd", L"Click2Screen-Setup.exe" };
    for (const wchar_t* n : names) {
        wchar_t f[MAX_PATH + 32];
        _snwprintf(f, MAX_PATH + 32, L"%s\\%s", dir, n);
        if (keepSelf && _wcsicmp(f, keepSelf) == 0) continue;
        for (int i = 0; i < 20 && !DeleteFileW(f) && GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES; ++i) Sleep(250);
    }
    RemoveDirectoryW(dir);
}

bool DoInstall()
{
    Say(T(S_SU_LOG_INSTALL));
    wchar_t dir[MAX_PATH];
    if (!GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", dir, MAX_PATH) || !dir[0]) wcscpy(dir, g_dir);   // an installed copy stays where it is
    if (!dir[0] && !ProgramDir(dir)) { Say(T(S_SU_LOG_NODIR)); return false; }
    if (!ValidDir(dir)) { Say(T(S_SU_LOG_BADDIR)); return false; }
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
        SetRegStr(k, L"Contact", L"" C2S_EMAIL_STR);
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
    wchar_t desk[MAX_PATH], menu[MAX_PATH], dir[MAX_PATH], g_regDir[MAX_PATH];
    g_regDir[0] = 0;
    GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", g_regDir, MAX_PATH);   // read before the key goes
    LinkPaths(desk, menu);
    Say(T(S_SU_LOG_LINKS_DEL));
    if (desk[0]) DeleteFileW(desk);
    if (menu[0]) DeleteFileW(menu);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"Click2Screen"); RegCloseKey(k); }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, L"Click2Screen"); RegCloseKey(k); }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    if (g_regDir[0] ? (wcscpy(dir, g_regDir), true) : ProgramDir(dir)) {
        wchar_t self[MAX_PATH], want[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        _snwprintf(want, MAX_PATH, L"%s\\Click2Screen-Setup.exe", dir);
        if (_wcsicmp(self, want) == 0) { g_selfDelete = true; RemoveFilesIn(dir, self); ScheduleSelfDelete(dir); }
        else RemoveFilesIn(dir, nullptr);
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
    if (inst) _snwprintf(text, MAX_PATH + 100, T(S_SU_STATE_INST), ver); else wcscpy(text, T(S_SU_STATE_NONE));
    g_stateColor = !inst ? RGB(90, 90, 100) : wcscmp(ver, L"" C2S_VER_STR) == 0 ? RGB(0, 128, 60) : RGB(200, 140, 0);
    SetWindowTextW(Ctl(IDC_STATE), text);
    SetWindowTextW(Ctl(IDC_INSTALL), !inst ? T(S_SU_INSTALL) : wcscmp(ver, L"" C2S_VER_STR) == 0 ? T(S_SU_REINSTALL) : T(S_SU_UPDATE));   // same version: Reinstall
    EnableWindow(Ctl(IDC_REMOVE), inst);
    EnableWindow(Ctl(IDC_LAUNCH), inst);
    SetWindowTextW(Ctl(IDC_TITLE), T(S_APP));
    SetWindowTextW(Ctl(IDC_SUB), T(S_SU_SUB));
    wchar_t vl[200], vn[100];
    _snwprintf(vn, 100, T(S_SU_VERSION), L"" C2S_VER_STR);
    _snwprintf(vl, 200, L"%s   ·   %s, %s   ·", vn, L"" C2S_AUTHOR_STR, L"" C2S_EMAIL_STR);       // the link to the project follows on the same line
    SetWindowTextW(Ctl(IDC_VERSION), vl);
    {   // The version line is as wide as its text (the label must not lie under the link: it would be painted over it at every refresh), the link
        // stands right behind it, and on the next line when it does not fit in the window (a long text of another language, a large font).
        HWND ver = Ctl(IDC_VERSION), lnk = Ctl(IDC_LINK);
        SIZE s1 = { 0, 0 }, s2 = { 0, 0 };
        HDC dc = GetDC(ver);
        if (dc) {
            HGDIOBJ of = SelectObject(dc, g_font); GetTextExtentPoint32W(dc, vl, (int)wcslen(vl), &s1);
            SelectObject(dc, g_fontLink ? g_fontLink : g_font); GetTextExtentPoint32W(dc, kProjectText, (int)wcslen(kProjectText), &s2);
            SelectObject(dc, of); ReleaseDC(ver, dc);
        }
        RECT cr; GetClientRect(g_wnd, &cr);
        const int x0 = Sc(100), y0 = Sc(71), h = Sc(19), gap = Sc(8), pad = Sc(6);
        SetWindowPos(ver, nullptr, x0, y0, s1.cx + pad, h, SWP_NOZORDER | SWP_NOACTIVATE);
        bool sameLine = x0 + s1.cx + gap + s2.cx + pad <= cr.right - Sc(12);
        int lh = s2.cy + Sc(4); if (lh < h) lh = h;                    // the height of the font and a little more: the tail of the "g" must not be cut off
        SetWindowPos(lnk, HWND_TOP, sameLine ? x0 + s1.cx + gap : x0, sameLine ? y0 : y0 + h - Sc(2), s2.cx + pad, lh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(ver, nullptr, x0, y0, s1.cx + pad, s1.cy + Sc(4) > h ? s1.cy + Sc(4) : h, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
    SetWindowTextW(Ctl(IDC_GROUP), T(S_SU_STATUS));
    SetWindowTextW(Ctl(IDC_DESKTOP), T(S_SU_DESKTOP));
    SetWindowTextW(Ctl(IDC_START), T(S_SU_STARTMENU));
    SetWindowTextW(Ctl(IDC_AUTO), T(S_SU_AUTOSTART));
    SetWindowTextW(Ctl(IDC_REMOVE), T(S_SU_REMOVE));
    SetWindowTextW(Ctl(IDC_LAUNCH), T(S_SU_LAUNCH));
    SetWindowTextW(Ctl(IDC_CLOSE), T(S_SU_CLOSE));
    SetWindowTextW(Ctl(IDC_LANGLBL), T(S_SU_LANG));
    SetWindowTextW(Ctl(IDC_PATHLBL), T(S_SU_PATH));
    SetWindowTextW(Ctl(IDC_BROWSE), T(S_SU_BROWSE));
    SetWindowTextW(Ctl(IDC_RUNAFTER), T(S_SU_RUNAFTER));
    // The folder is chosen before the installation; an installed copy stays where it is (as Speak2Mic / Show2Cam).
    if (inst && loc[0]) SetWindowTextW(Ctl(IDC_PATH), loc);
    EnableWindow(Ctl(IDC_BROWSE), !inst && !g_worker);
}

void Busy(bool busy)
{
    int ids[] = { IDC_INSTALL, IDC_REMOVE, IDC_LAUNCH, IDC_DESKTOP, IDC_START, IDC_AUTO, IDC_RUNAFTER, IDC_LANG, IDC_BROWSE };
    for (int id : ids) EnableWindow(Ctl(id), !busy);
    if (!busy) Refresh();
}

void Start(int action)
{
    if (action == ACT_REMOVE && MessageBoxW(g_wnd, T(S_SU_ASK_REMOVE), T(S_APP), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_wantDesktop = IsDlgButtonChecked(g_wnd, IDC_DESKTOP) == BST_CHECKED;
    g_wantStart = IsDlgButtonChecked(g_wnd, IDC_START) == BST_CHECKED;
    g_wantAuto = IsDlgButtonChecked(g_wnd, IDC_AUTO) == BST_CHECKED;
    g_wantRun = IsDlgButtonChecked(g_wnd, IDC_RUNAFTER) == BST_CHECKED;
    if (action == ACT_INSTALL) GetWindowTextW(Ctl(IDC_PATH), g_dir, MAX_PATH);
    g_action = action;
    Busy(true);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr, DWORD ex = 0)
{
    if (y >= 100) y += kDy;                                 // everything under the header lies lower by the extra row of the header
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, Sc(x), Sc(y), Sc(w), Sc(h), g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

int CALLBACK BrowseInit(HWND h, UINT m, LPARAM, LPARAM data)
{
    if (m == BFFM_INITIALIZED && data) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

void Browse()
{
    wchar_t cur[MAX_PATH], pick[MAX_PATH];
    GetWindowTextW(Ctl(IDC_PATH), cur, MAX_PATH);
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd; bi.pszDisplayName = pick; bi.lpszTitle = T(S_SU_BROWSE_TITLE);
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = BrowseInit; bi.lParam = (LPARAM)cur;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, pick)) {
        size_t n = wcslen(pick);
        // a folder that already holds other things gets its own sub-folder (the removal deletes only our files, but still)
        bool isRoot = n <= 3, ours = n >= 12 && _wcsicmp(pick + n - 12, L"Click2Screen") == 0;
        if (!ours && !isRoot) wcsncat(pick, L"\\Click2Screen", MAX_PATH - n - 1);
        else if (isRoot) _snwprintf(pick, MAX_PATH, L"%sClick2Screen", pick);
        SetWindowTextW(Ctl(IDC_PATH), pick);
    }
    CoTaskMemFree(pidl);
}

void Launch()
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!InstalledDir(dir)) return;
    _snwprintf(exe, MAX_PATH, L"%s\\Click2Screen.exe", dir);
    // The installer runs elevated; Explorer starts the program with the user's rights.
    ShellExecuteW(nullptr, L"open", L"explorer.exe", exe, nullptr, SW_SHOWNORMAL);
}

void BuildControls()
{
    HWND hw = g_wnd;
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_fontLink) DeleteObject(g_fontLink);
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)g_dpi, Dpi96());
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_BOLD; lf.lfHeight = lf.lfHeight * 2;
    g_fontTitle = CreateFontIndirectW(&lf);
    { LOGFONTW ll = ncm.lfMessageFont; ll.lfUnderline = TRUE; g_fontLink = CreateFontIndirectW(&ll); }
    // 600 x 452 units, laid out as Speak2Mic / Show2Cam: a white header (icon, name, subtitle, version · author, the language
    // list in its right corner), a status group, the buttons, the log, the install folder, the boxes, Close.
    Add(L"STATIC", L"", SS_LEFT, 100, 12, 340, 36, IDC_TITLE, g_fontTitle);
    HWND lg = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 450, 14, 138, 300, IDC_LANG);
    for (int i = 0; i < LangCount(); ++i) SendMessageW(lg, CB_ADDSTRING, 0, (LPARAM)LangName(i));
    SendMessageW(lg, CB_SETCURSEL, g_lang, 0);
    Add(L"STATIC", L"", SS_LEFT, 100, 50, 480, 20, IDC_SUB);
    Add(L"STATIC", L"", SS_LEFT, 100, 71, 480, 19, IDC_VERSION);
    Add(L"STATIC", kProjectText, SS_LEFT | SS_NOTIFY, 100, 71, 190, 19, IDC_LINK, g_fontLink);          // moved right after the author in Refresh
    Add(L"BUTTON", L"", BS_GROUPBOX, 12, 106, 576, 46, IDC_GROUP);
    Add(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 26, 126, 550, 20, IDC_STATE);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, 322, 162, 144, 30, IDC_INSTALL);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 472, 162, 116, 30, IDC_REMOVE);
    Add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 204, 576, 112, IDC_LOG);
    Add(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 17, 327, 150, 20, IDC_PATHLBL);
    Add(L"EDIT", g_dir, WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 169, 326, 320, 22, IDC_PATH);      // changed only with Browse
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 495, 324, 96, 26, IDC_BROWSE);
    SendMessageW(Ctl(IDC_PATH), EM_SETLIMITTEXT, 200, 0);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 14, 360, 270, 22, IDC_DESKTOP);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 300, 360, 280, 22, IDC_START);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 14, 384, 270, 22, IDC_RUNAFTER);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 300, 384, 280, 22, IDC_AUTO);
    CheckDlgButton(hw, IDC_DESKTOP, g_wantDesktop ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_START, g_wantStart ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_AUTO, g_wantAuto ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_RUNAFTER, g_wantRun ? BST_CHECKED : BST_UNCHECKED);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 12, 414, 144, 30, IDC_LAUNCH);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 472, 414, 116, 30, IDC_CLOSE);
    // The list has no label in the window (the language is told by its own names); the label stays for screen readers and tests.
    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, Sc(10), Sc(10), hw, (HMENU)(INT_PTR)IDC_LANGLBL, g_inst, nullptr);
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
    g_wantRun = IsDlgButtonChecked(g_wnd, IDC_RUNAFTER) == BST_CHECKED;
    GetWindowTextW(Ctl(IDC_PATH), g_dir, MAX_PATH);
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
        case IDC_BROWSE: Browse(); break;
        case IDC_CLOSE: DestroyWindow(hw); break;
        case IDC_LINK: if (HIWORD(wp) == STN_CLICKED) ShellExecuteW(hw, L"open", kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDC_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) { g_lang = (int)SendMessageW(Ctl(IDC_LANG), CB_GETCURSEL, 0, 0); LangSelect(g_lang + 1); Refresh(); }
            break;
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
        RECT cr; GetClientRect(hw, &cr);
        RECT head = { 0, 0, cr.right, Sc(kHeadH) }, body = { 0, Sc(kHeadH), cr.right, cr.bottom };
        FillRect(dc, &head, (HBRUSH)GetStockObject(WHITE_BRUSH));
        FillRect(dc, &body, GetSysColorBrush(COLOR_BTNFACE));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160)); HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, 0, Sc(kHeadH), nullptr); LineTo(dc, cr.right, Sc(kHeadH));
        SelectObject(dc, op); DeleteObject(pen);
        if (g_big) DrawIconEx(dc, Sc(21), Sc(16), g_big, Sc(64), Sc(64), 0, nullptr, DI_NORMAL);
        EndPaint(hw, &ps);
        return 0; }
    case WM_CTLCOLORSTATIC: {
        int cid = GetDlgCtrlID((HWND)lp);
        if (cid == IDC_TITLE || cid == IDC_SUB || cid == IDC_VERSION) {
            SetTextColor((HDC)wp, cid == IDC_TITLE ? RGB(40, 40, 60) : cid == IDC_VERSION ? RGB(130, 130, 140) : RGB(90, 90, 100));
            SetBkColor((HDC)wp, RGB(255, 255, 255));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        }
        if (GetDlgCtrlID((HWND)lp) == IDC_LINK) {
            SetTextColor((HDC)wp, RGB(0, 102, 204));
            SetBkColor((HDC)wp, RGB(255, 255, 255));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        if (GetDlgCtrlID((HWND)lp) == IDC_STATE) {
            SetTextColor((HDC)wp, g_stateColor);
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_SETCURSOR:
        if ((HWND)wp == Ctl(IDC_LINK)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
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
        if (g_ok && g_action == ACT_INSTALL) { SetFocus(Ctl(IDC_LAUNCH)); if (g_wantRun) Launch(); }
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
    InstalledDir(g_dir);                                    // the default: where it is installed now, else Program Files
    if (cmd) {                                               // /dir "C:\\Some\\Folder"  (quotes optional without spaces)
        const wchar_t* d = wcsstr(cmd, L"/dir");
        if (d && (d[4] == L' ' || d[4] == L'=' || d[4] == L':')) {
            d += 5; while (*d == L' ') ++d;
            bool q = *d == L'"'; if (q) ++d;
            size_t n = 0;
            while (d[n] && (q ? d[n] != L'"' : d[n] != L' ') && n < MAX_PATH - 1) { g_dir[n] = d[n]; ++n; }
            g_dir[n] = 0;
        }
    }
    if (g_silent && (doInstall || doRemove)) {
        g_action = doInstall ? ACT_INSTALL : ACT_REMOVE;
        Worker(nullptr);
        return g_ok ? 0 : 1;
    }
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, 0);
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"Click2ScreenSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = g_icon;
    RegisterClassW(&wc);
    RECT r = { 0, 0, Sc(600), Sc(452 + kDy) };
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
