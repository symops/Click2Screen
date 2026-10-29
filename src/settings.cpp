// Settings (HKCU\Software\Click2Screen), the file name template, the output folders.
#include "c2s.h"
#include <shlobj.h>
#include <wchar.h>
#include <stdio.h>

static wchar_t g_keyBuf[160] = L"Software\\Click2Screen";
const wchar_t* SettingsKeyName() { return g_keyBuf; }                   // the registry key of the settings (the autotest has its own)
#define KEY g_keyBuf

void SettingsUseKey(const wchar_t* key)
{
    wcsncpy(g_keyBuf, key && *key ? key : L"Software\\Click2Screen", 159);
    g_keyBuf[159] = 0;
}

static DWORD GetDw(HKEY k, const wchar_t* name, DWORD def)
{
    DWORD v = def, n = sizeof(v), t = 0;
    if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)&v, &n) != ERROR_SUCCESS || t != REG_DWORD) return def;
    return v;
}
static void GetStr(HKEY k, const wchar_t* name, wchar_t* out, int cap, const wchar_t* def)
{
    DWORD n = (DWORD)(cap * sizeof(wchar_t)), t = 0;
    wchar_t buf[MAX_PATH + 8];
    n = sizeof(buf) - 2;
    if (RegQueryValueExW(k, name, nullptr, &t, (BYTE*)buf, &n) == ERROR_SUCCESS && t == REG_SZ && n >= 2) {
        buf[n / sizeof(wchar_t)] = 0;
        wcsncpy(out, buf, cap - 1); out[cap - 1] = 0;
    } else {
        wcsncpy(out, def, cap - 1); out[cap - 1] = 0;
    }
}
static void PutDw(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }
static void PutStr(HKEY k, const wchar_t* name, const wchar_t* v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void KnownFolder(const KNOWNFOLDERID& id, const wchar_t* sub, wchar_t* out, int cap)
{
    PWSTR p = nullptr;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        _snwprintf(out, cap, L"%s\\%s", p, sub);
        out[cap - 1] = 0;
        CoTaskMemFree(p);
    } else {
        wcsncpy(out, L"C:\\Click2Screen", cap - 1); out[cap - 1] = 0;
    }
}

void SettingsDefaults(Settings& s)
{
    memset(&s, 0, sizeof(s));
    KnownFolder(FOLDERID_Videos, L"Click2Screen", s.folder, MAX_PATH);           // videos and pictures go to the same folder
    wcscpy(s.fileName, L"File $D.$M.$Y $H-$N-$S");
    s.imageFormat = IMG_PNG;
    s.capMouse = true; s.highlight = true; s.sounds = true; s.notify = true; s.loopback = true; s.mic = true;
    s.openFolder = true; s.runAtStartup = true; s.topmost = true; s.separateMonitors = true;
    for (int i = 0; i < HKI_COUNT; ++i) s.hotkey[i] = HotkeyDefault(i);
    s.delaySec = 0;
    s.videoCodec = 0; s.audioCodec = 0; s.videoQuality = 1; s.audioQuality = 1;
    s.fps = 25; s.language = 0;
}

int HotkeyDefault(int i)
{
    // Ctrl+PrintScreen for the whole desktop, then Ctrl+F6 ... Ctrl+F12 in the order of the Hotkeys page (a plain function key with
    // Ctrl is not reserved by Windows; Windows refuses a plain F12 only).
    switch (i) {
    case 0: return (MOD_CONTROL << 16) | VK_SNAPSHOT;      // screenshot of the desktop
    case 1: return (MOD_CONTROL << 16) | VK_F6;            // screenshot of a region
    case 2: return (MOD_CONTROL << 16) | VK_F7;            // screenshot of a window
    case 4: return (MOD_CONTROL << 16) | VK_F8;            // recording of the desktop
    case 5: return (MOD_CONTROL << 16) | VK_F9;            // recording of a region
    case 6: return (MOD_CONTROL << 16) | VK_F10;           // recording of a window
    case 3: return (MOD_CONTROL << 16) | VK_F11;           // stop the recording
    default: return (MOD_CONTROL << 16) | VK_F12;          // the drawing on / off
    }
}

bool HotkeyUsable(int code)
{
    if (code <= 0) return false;
    unsigned mod = ((unsigned)code >> 16) & 0xFFFF, vk = (unsigned)code & 0xFFFF;
    if (mod & ~(unsigned)(MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) return false;
    bool alone = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_SNAPSHOT || vk == VK_PAUSE || vk == VK_SCROLL;   // keys that need no modifier
    bool known = alone || (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) ||
                 (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || (vk >= VK_PRIOR && vk <= VK_DOWN) ||
                 vk == VK_INSERT || vk == VK_DELETE || vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB || vk == VK_BACK;
    if (!known) return false;
    if (vk == VK_F12 && mod == 0) return false;                           // Windows keeps a plain F12 for debuggers and refuses it as a hotkey
    return alone || mod != 0;                                             // a plain letter would take the key from every program
}

void HotkeyName(int code, wchar_t* out, int cap)
{
    out[0] = 0;
    if (code <= 0 || cap < 2) return;
    unsigned mod = ((unsigned)code >> 16) & 0xFFFF, vk = (unsigned)code & 0xFFFF;
    wchar_t k[40] = L"";
    if (vk >= VK_F1 && vk <= VK_F24) _snwprintf(k, 40, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { k[0] = (wchar_t)vk; k[1] = 0; }
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) _snwprintf(k, 40, L"Num %u", vk - VK_NUMPAD0);
    else {
        static const struct { unsigned vk; const wchar_t* n; } names[] = {
            { VK_SNAPSHOT, L"PrintScreen" }, { VK_PAUSE, L"Pause" }, { VK_SCROLL, L"ScrollLock" }, { VK_SPACE, L"Space" },
            { VK_RETURN, L"Enter" }, { VK_TAB, L"Tab" }, { VK_BACK, L"Backspace" }, { VK_DELETE, L"Delete" }, { VK_INSERT, L"Insert" },
            { VK_HOME, L"Home" }, { VK_END, L"End" }, { VK_PRIOR, L"PageUp" }, { VK_NEXT, L"PageDown" },
            { VK_LEFT, L"Left" }, { VK_RIGHT, L"Right" }, { VK_UP, L"Up" }, { VK_DOWN, L"Down" },
            { VK_MULTIPLY, L"Num *" }, { VK_ADD, L"Num +" }, { VK_SUBTRACT, L"Num -" }, { VK_DECIMAL, L"Num ." }, { VK_DIVIDE, L"Num /" } };
        for (auto& n : names) if (n.vk == vk) { wcscpy(k, n.n); break; }
        if (!k[0]) {
            UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            if (!sc || !GetKeyNameTextW((LONG)(sc << 16), k, 40)) _snwprintf(k, 40, L"0x%02X", vk);
        }
    }
    _snwprintf(out, cap, L"%s%s%s%s%s", (mod & MOD_CONTROL) ? L"Ctrl+" : L"", (mod & MOD_ALT) ? L"Alt+" : L"",
               (mod & MOD_SHIFT) ? L"Shift+" : L"", (mod & MOD_WIN) ? L"Win+" : L"", k);
    out[cap - 1] = 0;
}

static const wchar_t* const kHotkeyNames[HKI_COUNT] = { L"HotkeyDesktop", L"HotkeyRegion", L"HotkeyWindow", L"HotkeyStop",
                                                         L"HotkeyRecDesktop", L"HotkeyRecRegion", L"HotkeyRecWindow", L"HotkeyDraw" };

void SettingsLoad(Settings& s)
{
    SettingsDefaults(s);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    Settings d = s;
    {   // one folder; the settings of an older version kept two, the one of the videos stays
        wchar_t f[MAX_PATH]; GetStr(k, L"OutputFolder", f, MAX_PATH, L"");
        if (!f[0]) GetStr(k, L"VidOutputFolder", f, MAX_PATH, L"");
        if (f[0]) wcscpy(s.folder, f);
    }
    GetStr(k, L"FileName", s.fileName, 128, d.fileName);
    s.imageFormat  = Clamp((int)GetDw(k, L"ImageFormat", d.imageFormat), 0, IMG_COUNT - 1);
    s.capMouse     = GetDw(k, L"CapMouse", d.capMouse) != 0;
    s.highlight    = GetDw(k, L"Highlight", d.highlight) != 0;
    s.sounds       = GetDw(k, L"EnableSound", d.sounds) != 0;
    s.notify       = GetDw(k, L"Notifications", d.notify) != 0;
    s.loopback     = GetDw(k, L"RecordSpeakers", d.loopback) != 0;
    s.mic          = GetDw(k, L"EnableMicRec", d.mic) != 0;
    s.openFolder   = GetDw(k, L"OpenOutFolder", d.openFolder) != 0;
    s.runAtStartup = GetDw(k, L"RunAtStartup", d.runAtStartup) != 0;
    s.topmost      = GetDw(k, L"AlwaysOnTop", d.topmost) != 0;
    s.separateMonitors = GetDw(k, L"SeparateMonitorFiles", d.separateMonitors) != 0;
    s.delaySec     = Clamp((int)GetDw(k, L"Delay", d.delaySec), 0, 10);
    s.videoCodec   = Clamp((int)GetDw(k, L"VideoCodec", d.videoCodec), 0, VC_COUNT - 1);
    if (s.videoCodec == VC_H264_FAST) s.videoCodec = VC_H264_BEST;                    // the second H.264 profile is gone from the options: its recordings are the first one now
    s.audioCodec   = Clamp((int)GetDw(k, L"AudioCodec", d.audioCodec), 0, 1);
    s.videoQuality = Clamp((int)GetDw(k, L"VideoQuality", d.videoQuality), 0, 2);
    s.audioQuality = Clamp((int)GetDw(k, L"AudioQuality", d.audioQuality), 0, 2);
    s.fps          = Clamp((int)GetDw(k, L"Fps", d.fps), 5, 60);
    s.language     = Clamp((int)GetDw(k, L"Language", d.language), 0, LangCount());
    for (int i = 0; i < HKI_COUNT; ++i) {                              // 0 = switched off; anything else must be a key that can be registered
        int c = (int)GetDw(k, kHotkeyNames[i], d.hotkey[i]);
        s.hotkey[i] = c == 0 || HotkeyUsable(c) ? c : d.hotkey[i];
    }
    RegCloseKey(k);
    if (!TemplateValid(s.fileName)) wcscpy(s.fileName, d.fileName);
}

bool SettingsStored()
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool has = RegQueryValueExW(k, L"RunAtStartup", nullptr, &type, nullptr, &size) == ERROR_SUCCESS;       // written by the program or by the installer
    RegCloseKey(k);
    return has;
}

void SettingsSave(const Settings& s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    PutStr(k, L"OutputFolder", s.folder);
    PutStr(k, L"FileName", s.fileName);
    PutDw(k, L"ImageFormat", s.imageFormat);
    PutDw(k, L"CapMouse", s.capMouse);
    PutDw(k, L"Highlight", s.highlight);
    PutDw(k, L"EnableSound", s.sounds);
    PutDw(k, L"Notifications", s.notify);
    PutDw(k, L"RecordSpeakers", s.loopback);
    PutDw(k, L"EnableMicRec", s.mic);
    PutDw(k, L"OpenOutFolder", s.openFolder);
    PutDw(k, L"RunAtStartup", s.runAtStartup);
    PutDw(k, L"AlwaysOnTop", s.topmost);
    PutDw(k, L"SeparateMonitorFiles", s.separateMonitors);
    PutDw(k, L"Delay", s.delaySec);
    PutDw(k, L"VideoCodec", s.videoCodec);
    PutDw(k, L"AudioCodec", s.audioCodec);
    PutDw(k, L"VideoQuality", s.videoQuality);
    PutDw(k, L"AudioQuality", s.audioQuality);
    PutDw(k, L"Fps", s.fps);
    PutDw(k, L"Language", s.language);
    for (int i = 0; i < HKI_COUNT; ++i) PutDw(k, kHotkeyNames[i], s.hotkey[i]);
    RegCloseKey(k);
}

bool SettingsSetAutostart(bool on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok;
    if (on) {
        wchar_t exe[MAX_PATH], val[MAX_PATH + 16];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        _snwprintf(val, MAX_PATH + 16, L"\"%s\" /tray", exe);
        val[MAX_PATH + 15] = 0;
        ok = RegSetValueExW(k, L"Click2Screen", 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG r = RegDeleteValueW(k, L"Click2Screen");
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(k);
    return ok;
}

// ---- file name template ------------------------------------------------------------------------------------------
static bool BadChar(wchar_t c) { return c < 32 || wcschr(L"\\/:*?\"<>|", c) != nullptr; }

bool TemplateValid(const wchar_t* tpl)
{
    if (!tpl) return false;
    int runs = 0; bool inRun = false; int literal = 0;
    for (const wchar_t* p = tpl; *p; ++p) {
        if (*p == L'#') { if (!inRun) { ++runs; inRun = true; } continue; }
        inRun = false;
        if (*p == L'$' && wcschr(L"YMDHNS", p[1]) && p[1]) { ++p; ++literal; continue; }
        if (*p == L'%') {   // %USERNAME% only
            if (_wcsnicmp(p, L"%USERNAME%", 10) == 0) { p += 9; ++literal; continue; }
        }
        if (BadChar(*p)) return false;
        ++literal;
    }
    return runs <= 1 && (literal > 0 || runs == 1);
}

bool EnsureFolder(const wchar_t* folder, wchar_t* err, int errLen)
{
    if (err && errLen) err[0] = 0;
    if (!folder || !*folder) { if (err) wcsncpy(err, T(S_ERR_FOLDER_MISSING), errLen - 1); return false; }
    DWORD a = GetFileAttributesW(folder);
    if (a == INVALID_FILE_ATTRIBUTES) {
        int r = SHCreateDirectoryExW(nullptr, folder, nullptr);
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) {
            if (err) { wcsncpy(err, T(S_ERR_FOLDER_CREATE), errLen - 1); err[errLen - 1] = 0; }
            return false;
        }
        a = GetFileAttributesW(folder);
    }
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        if (err) { wcsncpy(err, T(S_ERR_FOLDER_MISSING), errLen - 1); err[errLen - 1] = 0; }
        return false;
    }
    // writable?
    wchar_t probe[MAX_PATH + 40];
    _snwprintf(probe, MAX_PATH + 40, L"%s\\.c2s-probe-%lu", folder, GetCurrentProcessId());
    probe[MAX_PATH + 39] = 0;
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) { wcsncpy(err, T(S_ERR_FOLDER_WRITE), errLen - 1); err[errLen - 1] = 0; }
        return false;
    }
    CloseHandle(h);
    return true;
}

// The template expanded for the moment: everything except the '#' run, which stays a hole (its position and width are returned).
static void ExpandTemplate(const wchar_t* tpl, wchar_t* base, int* blen, int* hashPos, int* hashLen)
{
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t user[128]; DWORD un = 128;
    if (!GetUserNameW(user, &un)) wcscpy(user, L"user");
    int bl = 0; *hashPos = -1; *hashLen = 0;
    auto put = [&](const wchar_t* s) { while (*s && bl < 380) base[bl++] = *s++; };
    for (const wchar_t* p = tpl; *p; ++p) {
        wchar_t tmp[16];
        if (*p == L'#') {
            if (*hashPos < 0) *hashPos = bl;
            ++*hashLen;
            continue;
        }
        if (*p == L'$' && p[1] && wcschr(L"YMDHNS", p[1])) {
            int v = 0, w = 2;
            switch (p[1]) {
                case L'Y': v = st.wYear; w = 4; break;
                case L'M': v = st.wMonth; break;
                case L'D': v = st.wDay; break;
                case L'H': v = st.wHour; break;
                case L'N': v = st.wMinute; break;
                default:   v = st.wSecond; break;
            }
            _snwprintf(tmp, 16, L"%0*d", w, v);
            put(tmp); ++p; continue;
        }
        if (_wcsnicmp(p, L"%USERNAME%", 10) == 0) { put(user); p += 9; continue; }
        wchar_t one[2] = { *p, 0 };
        put(one);
    }
    base[bl] = 0; *blen = bl;
}

// The name with the number n put in the hole; a name must not end with a dot or a blank and must not be empty.
static void AssembleName(const wchar_t* base, int bl, int hashPos, int hashLen, unsigned n, wchar_t* name)
{
    int nl = 0;
    if (hashPos >= 0) {
        for (int i = 0; i < hashPos && nl < 380; ++i) name[nl++] = base[i];
        wchar_t num[24]; _snwprintf(num, 24, L"%0*u", hashLen, n);
        for (const wchar_t* q = num; *q && nl < 440; ++q) name[nl++] = *q;
        for (int i = hashPos; i < bl && nl < 455; ++i) name[nl++] = base[i];
    } else {
        for (int i = 0; i < bl; ++i) name[nl++] = base[i];
    }
    name[nl] = 0;
    while (nl > 0 && (name[nl - 1] == L'.' || name[nl - 1] == L' ')) name[--nl] = 0;
    if (nl == 0) wcscpy(name, L"Click2Screen");
}

void TemplatePreview(const wchar_t* tpl, wchar_t* out, int cap)
{
    out[0] = 0;
    if (!TemplateValid(tpl)) return;
    wchar_t base[400]; int bl = 0, hashPos = -1, hashLen = 0;
    ExpandTemplate(tpl, base, &bl, &hashPos, &hashLen);
    unsigned n = 1;
    if (hashPos >= 0) {                                            // the number the next video would get (the videos and the pictures count apart)
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) { n = GetDw(k, L"VidCounter", 0) + 1; RegCloseKey(k); }
    }
    wchar_t name[460];
    AssembleName(base, bl, hashPos, hashLen, n, name);
    wcsncpy(out, name, cap - 1); out[cap - 1] = 0;
}

bool BuildOutputPath(const wchar_t* folder, const wchar_t* tpl, const wchar_t* ext, const wchar_t* counterKey,
                     wchar_t* path, int pathLen, wchar_t* err, int errLen)
{
    if (!TemplateValid(tpl)) { if (err) { wcsncpy(err, T(S_ERR_TEMPLATE), errLen - 1); err[errLen - 1] = 0; } return false; }
    if (!EnsureFolder(folder, err, errLen)) return false;

    wchar_t base[400]; int bl = 0, hashPos = -1, hashLen = 0;
    ExpandTemplate(tpl, base, &bl, &hashPos, &hashLen);

    unsigned n = 1;
    if (hashPos >= 0 && counterKey) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) { n = GetDw(k, counterKey, 0) + 1; RegCloseKey(k); }
    }
    for (int tries = 0; tries < 100000; ++tries, ++n) {
        wchar_t name[460];
        AssembleName(base, bl, hashPos, hashLen, n, name);
        _snwprintf(path, pathLen, L"%s\\%s.%s", folder, name, ext);
        path[pathLen - 1] = 0;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
        if (hashPos < 0) {   // no sequence number: add one so nothing is overwritten
            for (unsigned k2 = 2; k2 < 10000; ++k2) {
                _snwprintf(path, pathLen, L"%s\\%s (%u).%s", folder, name, k2, ext);
                path[pathLen - 1] = 0;
                if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
            }
            return true;
        }
    }
    if (hashPos >= 0 && counterKey) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            PutDw(k, counterKey, n); RegCloseKey(k);
        }
    }
    return true;
}

// ---- export / import (.ini, UTF-16 like Speak2Mic / Show2Cam; the language is not part of it) ------------------------
static const wchar_t* const kSec = L"Click2Screen";

static void IniStr(const wchar_t* path, const wchar_t* name, const wchar_t* v)
{
    wchar_t q[MAX_PATH + 4];
    _snwprintf(q, MAX_PATH + 4, L"\"%s\"", v);
    q[MAX_PATH + 3] = 0;
    WritePrivateProfileStringW(kSec, name, q, path);
}

static void IniInt(const wchar_t* path, const wchar_t* name, int v)
{
    wchar_t b[24]; _snwprintf(b, 24, L"%d", v);
    WritePrivateProfileStringW(kSec, name, b, path);
}

bool SettingsExport(const Settings& s, const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    static const BYTE bom[2] = { 0xFF, 0xFE };           // a UTF-16 file: the profile functions then keep it Unicode
    DWORD w = 0;
    bool ok = WriteFile(h, bom, 2, &w, nullptr) && w == 2;
    CloseHandle(h);
    if (!ok) return false;
    // Text values are written in quotes (the profile functions take them off when reading): a value that starts or
    // ends with a blank comes back unchanged. Quotes cannot occur in a path or in a valid name template.
    IniStr(path, L"OutputFolder", s.folder);
    IniStr(path, L"FileName", s.fileName);
    IniInt(path, L"ImageFormat", s.imageFormat);
    IniInt(path, L"CapMouse", s.capMouse);
    IniInt(path, L"Highlight", s.highlight);
    IniInt(path, L"EnableSound", s.sounds);
    IniInt(path, L"Notifications", s.notify);
    IniInt(path, L"RecordSpeakers", s.loopback);
    for (int i = 0; i < HKI_COUNT; ++i) IniInt(path, kHotkeyNames[i], s.hotkey[i]);
    IniInt(path, L"EnableMicRec", s.mic);
    IniInt(path, L"OpenOutFolder", s.openFolder);
    IniInt(path, L"RunAtStartup", s.runAtStartup);
    IniInt(path, L"AlwaysOnTop", s.topmost);
    IniInt(path, L"SeparateMonitorFiles", s.separateMonitors);
    IniInt(path, L"Delay", s.delaySec);
    IniInt(path, L"VideoCodec", s.videoCodec);
    IniInt(path, L"AudioCodec", s.audioCodec);
    IniInt(path, L"VideoQuality", s.videoQuality);
    IniInt(path, L"AudioQuality", s.audioQuality);
    IniInt(path, L"Fps", s.fps);
    wchar_t back[MAX_PATH];
    return GetPrivateProfileStringW(kSec, L"Fps", L"", back, MAX_PATH, path) > 0;
}

static bool IniGetInt(const wchar_t* path, const wchar_t* name, int lo, int hi, int* out)
{
    wchar_t b[32];
    if (GetPrivateProfileStringW(kSec, name, L"\x01", b, 32, path) == 0 || b[0] == 1) return false;
    wchar_t* end = nullptr;
    long v = wcstol(b, &end, 10);
    if (end == b || v < lo || v > hi) return false;       // a value out of range is ignored
    *out = (int)v;
    return true;
}

bool SettingsImport(Settings& s, const wchar_t* path)
{
    wchar_t probe[8];
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    // The file must have the Click2Screen section.
    wchar_t names[512];
    if (GetPrivateProfileSectionNamesW(names, 512, path) == 0) return false;
    bool found = false;
    for (const wchar_t* p = names; *p; p += wcslen(p) + 1) if (!_wcsicmp(p, kSec)) found = true;
    if (!found) return false;
    (void)probe;
    wchar_t str[MAX_PATH];
    if (GetPrivateProfileStringW(kSec, L"OutputFolder", L"", str, MAX_PATH, path) > 0 || GetPrivateProfileStringW(kSec, L"VidOutputFolder", L"", str, MAX_PATH, path) > 0) { wcscpy(s.folder, str); }
    if (GetPrivateProfileStringW(kSec, L"FileName", L"", str, 128, path) > 0 && TemplateValid(str)) wcscpy(s.fileName, str);
    int v;
    if (IniGetInt(path, L"ImageFormat", 0, IMG_COUNT - 1, &v)) s.imageFormat = v;
    if (IniGetInt(path, L"CapMouse", 0, 1, &v)) s.capMouse = v != 0;
    if (IniGetInt(path, L"Highlight", 0, 1, &v)) s.highlight = v != 0;
    if (IniGetInt(path, L"EnableSound", 0, 1, &v)) s.sounds = v != 0;
    if (IniGetInt(path, L"Notifications", 0, 1, &v)) s.notify = v != 0;
    if (IniGetInt(path, L"RecordSpeakers", 0, 1, &v)) s.loopback = v != 0;
    for (int i = 0; i < HKI_COUNT; ++i) if (IniGetInt(path, kHotkeyNames[i], 0, 0x7FFFFFFF, &v) && (v == 0 || HotkeyUsable(v))) s.hotkey[i] = v;
    if (IniGetInt(path, L"EnableMicRec", 0, 1, &v)) s.mic = v != 0;
    if (IniGetInt(path, L"OpenOutFolder", 0, 1, &v)) s.openFolder = v != 0;
    if (IniGetInt(path, L"RunAtStartup", 0, 1, &v)) s.runAtStartup = v != 0;
    if (IniGetInt(path, L"AlwaysOnTop", 0, 1, &v)) s.topmost = v != 0;
    if (IniGetInt(path, L"SeparateMonitorFiles", 0, 1, &v)) s.separateMonitors = v != 0;
    if (IniGetInt(path, L"Delay", 0, 10, &v)) s.delaySec = v;
    if (IniGetInt(path, L"VideoCodec", 0, VC_COUNT - 1, &v)) s.videoCodec = v == VC_H264_FAST ? VC_H264_BEST : v;
    if (IniGetInt(path, L"AudioCodec", 0, 1, &v)) s.audioCodec = v;
    if (IniGetInt(path, L"VideoQuality", 0, 2, &v)) s.videoQuality = v;
    if (IniGetInt(path, L"AudioQuality", 0, 2, &v)) s.audioQuality = v;
    if (IniGetInt(path, L"Fps", 5, 60, &v)) s.fps = v;
    return true;
}
