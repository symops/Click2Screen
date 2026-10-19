// Settings (HKCU\Software\Click2Screen), the file name template, the output folders.
#include "c2s.h"
#include <shlobj.h>
#include <wchar.h>
#include <stdio.h>

static wchar_t g_keyBuf[160] = L"Software\\Click2Screen";
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
    KnownFolder(FOLDERID_Videos, L"Click2Screen", s.vidFolder, MAX_PATH);
    KnownFolder(FOLDERID_Pictures, L"Click2Screen", s.imgFolder, MAX_PATH);
    wcscpy(s.fileName, L"File $D.$M.$Y $H-$N-$S");
    s.imageFormat = IMG_PNG;
    s.capMouse = true; s.highlight = true; s.sounds = true; s.notify = true; s.loopback = true; s.mic = true;
    s.openFolder = true; s.runAtStartup = false;
    s.delaySec = 3; s.durH = s.durM = s.durS = 0;
    s.videoCodec = 0; s.audioCodec = 0; s.videoQuality = 1; s.audioQuality = 1;
    s.fps = 25; s.language = 0;
}

void SettingsLoad(Settings& s)
{
    SettingsDefaults(s);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    Settings d = s;
    GetStr(k, L"VidOutputFolder", s.vidFolder, MAX_PATH, d.vidFolder);
    GetStr(k, L"ImgOutputFolder", s.imgFolder, MAX_PATH, d.imgFolder);
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
    s.delaySec     = Clamp((int)GetDw(k, L"Delay", d.delaySec), 0, 10);
    s.durH         = Clamp((int)GetDw(k, L"RecDurationHour", 0), 0, 99);
    s.durM         = Clamp((int)GetDw(k, L"RecDurationMin", 0), 0, 59);
    s.durS         = Clamp((int)GetDw(k, L"RecDurationSec", 0), 0, 59);
    s.videoCodec   = Clamp((int)GetDw(k, L"VideoCodec", d.videoCodec), 0, VC_COUNT - 1);
    s.audioCodec   = Clamp((int)GetDw(k, L"AudioCodec", d.audioCodec), 0, 1);
    s.videoQuality = Clamp((int)GetDw(k, L"VideoQuality", d.videoQuality), 0, 2);
    s.audioQuality = Clamp((int)GetDw(k, L"AudioQuality", d.audioQuality), 0, 2);
    s.fps          = Clamp((int)GetDw(k, L"Fps", d.fps), 5, 60);
    s.language     = Clamp((int)GetDw(k, L"Language", d.language), 0, LangCount());
    RegCloseKey(k);
    if (!TemplateValid(s.fileName)) wcscpy(s.fileName, d.fileName);
}

void SettingsSave(const Settings& s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    PutStr(k, L"VidOutputFolder", s.vidFolder);
    PutStr(k, L"ImgOutputFolder", s.imgFolder);
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
    PutDw(k, L"Delay", s.delaySec);
    PutDw(k, L"RecDurationHour", s.durH);
    PutDw(k, L"RecDurationMin", s.durM);
    PutDw(k, L"RecDurationSec", s.durS);
    PutDw(k, L"VideoCodec", s.videoCodec);
    PutDw(k, L"AudioCodec", s.audioCodec);
    PutDw(k, L"VideoQuality", s.videoQuality);
    PutDw(k, L"AudioQuality", s.audioQuality);
    PutDw(k, L"Fps", s.fps);
    PutDw(k, L"Language", s.language);
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

bool BuildOutputPath(const wchar_t* folder, const wchar_t* tpl, const wchar_t* ext, const wchar_t* counterKey,
                     wchar_t* path, int pathLen, wchar_t* err, int errLen)
{
    if (!TemplateValid(tpl)) { if (err) { wcsncpy(err, T(S_ERR_TEMPLATE), errLen - 1); err[errLen - 1] = 0; } return false; }
    if (!EnsureFolder(folder, err, errLen)) return false;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t user[128]; DWORD un = 128;
    if (!GetUserNameW(user, &un)) wcscpy(user, L"user");

    // Expand everything except the '#' run, remembering where it is.
    wchar_t base[400]; int bl = 0, hashPos = -1, hashLen = 0;
    auto put = [&](const wchar_t* s) { while (*s && bl < 380) base[bl++] = *s++; };
    for (const wchar_t* p = tpl; *p; ++p) {
        wchar_t tmp[16];
        if (*p == L'#') {
            if (hashPos < 0) hashPos = bl;
            ++hashLen;
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
    base[bl] = 0;

    unsigned n = 1;
    if (hashPos >= 0 && counterKey) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) { n = GetDw(k, counterKey, 0) + 1; RegCloseKey(k); }
    }
    for (int tries = 0; tries < 100000; ++tries, ++n) {
        wchar_t name[460];
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
    IniStr(path, L"VidOutputFolder", s.vidFolder);
    IniStr(path, L"ImgOutputFolder", s.imgFolder);
    IniStr(path, L"FileName", s.fileName);
    IniInt(path, L"ImageFormat", s.imageFormat);
    IniInt(path, L"CapMouse", s.capMouse);
    IniInt(path, L"Highlight", s.highlight);
    IniInt(path, L"EnableSound", s.sounds);
    IniInt(path, L"Notifications", s.notify);
    IniInt(path, L"RecordSpeakers", s.loopback);
    IniInt(path, L"EnableMicRec", s.mic);
    IniInt(path, L"OpenOutFolder", s.openFolder);
    IniInt(path, L"RunAtStartup", s.runAtStartup);
    IniInt(path, L"Delay", s.delaySec);
    IniInt(path, L"RecDurationHour", s.durH);
    IniInt(path, L"RecDurationMin", s.durM);
    IniInt(path, L"RecDurationSec", s.durS);
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
    if (GetPrivateProfileStringW(kSec, L"VidOutputFolder", L"", str, MAX_PATH, path) > 0) { wcscpy(s.vidFolder, str); }
    if (GetPrivateProfileStringW(kSec, L"ImgOutputFolder", L"", str, MAX_PATH, path) > 0) { wcscpy(s.imgFolder, str); }
    if (GetPrivateProfileStringW(kSec, L"FileName", L"", str, 128, path) > 0 && TemplateValid(str)) wcscpy(s.fileName, str);
    int v;
    if (IniGetInt(path, L"ImageFormat", 0, IMG_COUNT - 1, &v)) s.imageFormat = v;
    if (IniGetInt(path, L"CapMouse", 0, 1, &v)) s.capMouse = v != 0;
    if (IniGetInt(path, L"Highlight", 0, 1, &v)) s.highlight = v != 0;
    if (IniGetInt(path, L"EnableSound", 0, 1, &v)) s.sounds = v != 0;
    if (IniGetInt(path, L"Notifications", 0, 1, &v)) s.notify = v != 0;
    if (IniGetInt(path, L"RecordSpeakers", 0, 1, &v)) s.loopback = v != 0;
    if (IniGetInt(path, L"EnableMicRec", 0, 1, &v)) s.mic = v != 0;
    if (IniGetInt(path, L"OpenOutFolder", 0, 1, &v)) s.openFolder = v != 0;
    if (IniGetInt(path, L"RunAtStartup", 0, 1, &v)) s.runAtStartup = v != 0;
    if (IniGetInt(path, L"Delay", 0, 10, &v)) s.delaySec = v;
    if (IniGetInt(path, L"RecDurationHour", 0, 99, &v)) s.durH = v;
    if (IniGetInt(path, L"RecDurationMin", 0, 59, &v)) s.durM = v;
    if (IniGetInt(path, L"RecDurationSec", 0, 59, &v)) s.durS = v;
    if (IniGetInt(path, L"VideoCodec", 0, VC_COUNT - 1, &v)) s.videoCodec = v;
    if (IniGetInt(path, L"AudioCodec", 0, 1, &v)) s.audioCodec = v;
    if (IniGetInt(path, L"VideoQuality", 0, 2, &v)) s.videoQuality = v;
    if (IniGetInt(path, L"AudioQuality", 0, 2, &v)) s.audioQuality = v;
    if (IniGetInt(path, L"Fps", 5, 60, &v)) s.fps = v;
    return true;
}
