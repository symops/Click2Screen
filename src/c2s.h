// Click2Screen — shared declarations.
// A screen recorder and screenshot tool for Windows: desktop / region / window video with microphone, screenshots
// global hotkeys, tray control. See ../README.md and ../analysis/FINDINGS.md.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdint.h>

#include "version.h"
#define C2S_WIDEN2(x)   L##x
#define C2S_WIDEN(x)    C2S_WIDEN2(x)
#define C2S_VERSION_STR C2S_WIDEN(C2S_VER_STR)
#define C2S_AUTHOR      L"Symo <symops@gmail.com>"

// ---- strings (lang.cpp) ----------------------------------------------------------------------------------------
enum Str {
    S_APP, S_REC_DESKTOP, S_REC_REGION, S_REC_WINDOW, S_SHOT_DESKTOP, S_SHOT_REGION, S_SHOT_WINDOW,
    S_OPTIONS, S_GRP_VIDEO, S_GRP_SHOT, S_HOTKEYS_HINT, S_STATUS_READY, S_PANEL_TITLE, S_TIME,
    S_SIZE, S_START, S_PAUSE, S_RESUME, S_STOP, S_HIDE, S_SHOW_PANEL,
    S_EXIT, S_STOP_HINT, S_TIP_START, S_TIP_PAUSE, S_TIP_STOP, S_HIDE_ASK, S_STARTING_IN,
    S_RECORDING, S_PAUSED, S_SAVED, S_TRAY_HIDDEN, S_OPT_TITLE, S_OPT_GENERAL, S_OPT_FILES, S_OPT_LANG, S_BTN_OK, S_BTN_CANCEL, S_RESTORE_DEFAULT, S_OPT_AUTOSTART, S_OPT_MOUSE,
    S_OPT_HIGHLIGHT, S_OPT_OPEN_FOLDER, S_OPT_OPEN_EDITOR, S_OPT_SOUNDS, S_OPT_MIC, S_OPT_DELAY, S_OPT_SECONDS,
    S_OPT_VCODEC, S_OPT_ACODEC, S_OPT_VQUALITY, S_OPT_AQUALITY, S_OPT_FPS, S_OPT_FOLDER, S_OPT_IMGFORMAT, S_OPT_TEMPLATE, S_OPT_TEMPLATE_TIPS, S_BROWSE,
    S_LANG_AUTO, S_VC_BEST, S_VC_FAST, S_AC_AAC, S_AC_MP3, S_Q_HIGH, S_Q_MEDIUM,
    S_Q_LOW, S_ERR_FOLDER_MISSING, S_ERR_FOLDER_CREATE, S_ERR_FOLDER_WRITE, S_ERR_TEMPLATE, S_ERR_CAPTURE, S_ERR_RECORDER,
    S_ERR_AUDIO, S_ERR_SAVE, S_ERR_REGION_SMALL, S_ERR_HOTKEY, S_ERR_ALREADY, S_ED_TITLE, S_ED_RECT,
    S_ED_ZOOM_IN, S_ED_ZOOM_OUT, S_ED_UNDO, S_ED_SAVE, S_ED_SAVE_AS, S_ED_IMAGES, S_PICK_REGION_HINT,
    S_PICK_WINDOW_HINT, S_OPT_EXPORT, S_OPT_IMPORT, S_INI_FILTER, S_ERR_EXPORT, S_ERR_IMPORT, S_SU_SUB,
    S_SU_STATE_NONE, S_SU_STATE_INST, S_SU_DESKTOP, S_SU_STARTMENU, S_SU_AUTOSTART, S_SU_INSTALL, S_SU_UPDATE,
    S_SU_REMOVE, S_SU_LAUNCH, S_SU_CLOSE, S_SU_LANG, S_SU_LOG_INSTALL, S_SU_LOG_REMOVE, S_SU_LOG_CLOSEAPP,
    S_SU_LOG_COPY, S_SU_LOG_COPY_FAIL, S_SU_LOG_REG, S_SU_LOG_LINKS, S_SU_LOG_LINKS_DEL, S_SU_LOG_AUTO_ON, S_SU_LOG_AUTO_OFF,
    S_SU_LOG_DONE, S_SU_LOG_FAIL, S_SU_LOG_KEPT, S_SU_LOG_NOSRC, S_SU_LOG_NODIR, S_SU_ASK_REMOVE,
    S_SU_PATH, S_SU_BROWSE, S_SU_BROWSE_TITLE, S_SU_LOG_BADDIR, S_SU_REINSTALL, S_OPEN_FOLDER_ITEM,
    S_OPT_FORMATS, S_OPT_SAVE, S_OPT_VIDEO_OPTIONS, S_OPT_AUDIO_OPTIONS,
    S_SU_VERSION, S_SU_STATUS, S_SU_RUNAFTER, S_OPT_NOTIFY, S_OPT_LOOPBACK,
    S_OPT_HOTKEYS, S_HK_STOP, S_HK_HINT, S_HK_DUP, S_HK_NONE, S_HK_DRAW, S_DRAW_ON, S_DRAW_OFF, S_OPT_NAMEPREVIEW,
    S__COUNT
};
const wchar_t* T(Str s);
void LangSelect(int setting);   // 0 = the Windows display language, 1..LangCount() = a language of the table
int  LangCount();
int  LangAutoIndex();           // table index chosen for "system language"
const wchar_t* LangName(int index);   // native name of table language `index` (0-based)

// ---- settings (settings.cpp) -----------------------------------------------------------------------------------
enum { IMG_PNG = 0, IMG_JPG, IMG_BMP, IMG_GIF, IMG_TIFF, IMG_WEBP, IMG_JXR, IMG_COUNT };      // picture formats (capture.cpp)
enum { VC_H264_BEST = 0, VC_H264_FAST, VC_H265, VC_WMV, VC_AVI, VC_GIF, VC_COUNT };           // video formats (recorder.cpp)
const wchar_t* ImageFormatName(int fmt);              // "PNG", "JPG", ...
const wchar_t* ImageFormatExt(int fmt);               // "png", "jpg", ...
bool ImageFormatAvailable(int fmt);                   // an encoder for it is installed (WebP / JPEG XR may be missing)
int  ImageFormatFromPath(const wchar_t* path);        // by the extension; -1 when it is none of ours
void FitSizeForVideo(int vc, int srcW, int srcH, int& w, int& h);   // FitSize, and the smaller frame a GIF gets
const wchar_t* VideoFormatName(int vc);               // "H.264 (MP4, slower)" ...
const wchar_t* VideoFormatExt(int vc);                // "mp4", "wmv", "avi", "gif"
bool VideoFormatAvailable(int vc);                    // the encoder exists on this Windows (HEVC needs the Microsoft extension)
struct Settings {
    wchar_t folder[MAX_PATH];       // where the videos and the pictures go (one folder for both)
    wchar_t fileName[128];          // template: $Y $M $D $H $N $S, # (digit of the sequence number), %USERNAME%
    int  imageFormat;               // IMG_*
    bool capMouse;                  // pointer in recordings and screenshots
    bool highlight;                 // translucent halo around the pointer and a wave at every click (recordings)
    bool sounds;                    // countdown / shutter sounds
    bool notify;                    // notifications (balloons) from the tray icon
    bool loopback;                  // record the sound that goes to the speakers (mixed with the microphone)
    bool mic;                       // record audio from the default microphone
    int  hotkey[8];                 // global hotkeys: desktop, region, window screenshot, stop recording, desktop, region, window recording, drawing; (MOD_* << 16) | VK, 0 = off
    bool openFolder;                // show the finished file in Explorer
    bool runAtStartup;
    int  delaySec;                  // delay before capture (0..10)
    int  videoCodec;                // VC_*: H.264 best / fast, H.265, WMV, AVI (MJPEG), GIF
    int  audioCodec;                // 0 = AAC, 1 = MP3
    int  videoQuality;              // 0 high, 1 medium, 2 low
    int  audioQuality;              // 0 high, 1 medium, 2 low
    int  fps;                       // 5..60
    int  language;                  // 0 auto, 1 en, 2 ru
};
void SettingsDefaults(Settings& s);
void SettingsLoad(Settings& s);
void SettingsSave(const Settings& s);
bool SettingsSetAutostart(bool on);
void SettingsUseKey(const wchar_t* key);                       // another registry key under HKCU (the autotest)
bool SettingsExport(const Settings& s, const wchar_t* path);   // .ini, UTF-16; the language is not exported
bool SettingsImport(Settings& s, const wchar_t* path);         // false: no such file / no Click2Screen section
bool TemplateValid(const wchar_t* tpl);
void TemplatePreview(const wchar_t* tpl, wchar_t* out, int cap);   // the name (without the extension) a file would get now; "" for an invalid template
// Hotkeys (Settings::hotkey): index 0 desktop, 1 region, 2 window screenshot, 3 stop the recording, 4 desktop, 5 region, 6 window recording, 7 the red-line drawing on / off.
enum { HKI_COUNT = 8 };
int  HotkeyDefault(int index);
bool HotkeyUsable(int code);                                  // a key that may be registered (0 = off is not usable)
void HotkeyName(int code, wchar_t* out, int cap);             // "Ctrl+Alt+PrintScreen"; "" for 0
// Builds "<folder>\<name>.<ext>" from the template. Numbered templates (#) take the first free number from the stored
// counter on; the counter is saved. Returns false when the folder cannot be used (err = message).
bool BuildOutputPath(const wchar_t* folder, const wchar_t* tpl, const wchar_t* ext, const wchar_t* counterKey,
                     wchar_t* path, int pathLen, wchar_t* err, int errLen);
bool EnsureFolder(const wchar_t* folder, wchar_t* err, int errLen);

// ---- capture (capture.cpp) -------------------------------------------------------------------------------------
RECT VirtualScreen();
// Output size for a source of w x h: at most 1920x1080 (aspect kept), both sides even, at least 2.
void FitSize(int srcW, int srcH, int& outW, int& outH);
// Auto video bitrate for a frame size, bit/s (the proportional table found in the original, extended upwards).
int  AutoBitrate(int w, int h);
class Grabber {
public:
    Grabber();
    ~Grabber();
    bool Init(int outW, int outH);
    // Copies the screen rectangle `src` into the bitmap (scaled to the output size), then the pointer.
    bool Grab(const RECT& src, bool mouse, bool highlight, bool fx = false);   // fx: the red lines of the drawing window (recordings); with `highlight` also a wave at every click
    uint8_t* Bits() const { return m_bits; }
    int  Width() const { return m_w; }
    int  Height() const { return m_h; }
    HBITMAP Release();              // hands the DIB to the caller (for screenshots)
private:
    void Close();
    HDC m_dc; HBITMAP m_bmp, m_old; uint8_t* m_bits; int m_w, m_h;
    HDC m_halo; HBITMAP m_haloBmp, m_haloOld;
};
// ---- recording effects (fx.cpp): a wave at every click (left blue, right red, middle purple), the red lines of the drawing window
void FxStart();                                    // low-level mouse hook: the calling thread must pump messages
void FxStop();
void FxPenPoint(int x, int y, bool newStroke);       // a point of the trail given by the drawing window (the pen of the mouse)
void FxWipe();                                     // wipes all the lines at once
void FxDraw(HDC dc, const RECT& src, int outW, int outH, bool waves);   // the lines always, the click waves when `waves`
int  FxTrailSnapshot(POINT* pts, unsigned char* brk, int cap, unsigned* version, unsigned char* shade = nullptr);   // the visible lines (the on-screen copy); shade: 0..255 round the colour wheel for each point
void FxPaintLines(uint32_t* bits, int bw, int bh, bool topDown, bool alpha, const POINT* pt, const unsigned char* brk, const unsigned char* shade, int n, float width);   // the lines of the drawing: smooth, anti-aliased, a gradient
COLORREF FxLineColor(unsigned char shade);          // the colour of a shade of the gradient (the rainbow)
bool FxTestHook(unsigned* clicksSeen, unsigned long* err);   // (autotest) is the hook installed, how many clicks it saw
void FxTestTrailLifetime(unsigned ms);             // (autotest) how long a trail point lives; 0 = five minutes
void FxTestClick(int x, int y, int button);        // (autotest) a click at a screen point, now
void FxTestTrailShade(int x, int y, bool newStroke, int shade);   // (autotest) a trail point of the given shade 0..255 (drawn now or up to 8 s ago)
void FxTestTrail(int x, int y, bool newStroke, unsigned long long ageMs);   // (autotest) a trail point of that age
// One-shot screenshot of a screen rectangle (full size). The caller owns the bitmap.
HBITMAP CaptureRect(const RECT& rc, bool mouse, bool highlight);
// Window rectangle as the user sees it (without the invisible resize borders on Windows 10/11).
bool WindowVisibleRect(HWND hw, RECT* rc);
bool SaveBitmap(HBITMAP bmp, const wchar_t* path, int format);

// ---- pickers (picker.cpp) --------------------------------------------------------------------------------------
bool PickRegion(HWND owner, RECT* out);
bool PickWindow(HWND owner, HWND* hw, RECT* out);

// ---- frame sinks (sinks.cpp): the video formats that are not written by Media Foundation -----------------------
class FrameSink {
public:
    virtual ~FrameSink() {}
    virtual bool Video(const uint8_t* dib, LONGLONG ts100ns) = 0;     // one w x h BGRA frame, bottom-up (as the Grabber holds it)
    virtual bool Audio(const void* pcm, uint32_t bytes) = 0;          // 16-bit mono 44.1 kHz
    virtual bool Full() const { return false; }                       // the file cannot grow any more: the recording ends
    virtual bool Close(bool ok) = 0;                                  // finishes the file; false: it is unusable
};
FrameSink* CreateAviSink(const wchar_t* path, int w, int h, int fps, int quality, bool audio, wchar_t* why, int whyLen);   // Motion-JPEG + PCM
FrameSink* CreateGifSink(const wchar_t* path, int w, int h, int fps, wchar_t* why, int whyLen);                            // animated, no sound

// ---- sound (audiomix.cpp) ----------------------------------------------------------------------------------------
// Mono 44.1 kHz 16 bit from interleaved float frames at any rate (`phase` / `prev` carry over between calls).
int  AudioToMono44k(const float* in, int frames, int channels, int rate, double* phase, float* prev, int16_t* out, int cap);
// The sample-rate conversion by the audio resampler that ships with Windows (Media Foundation): mono float in at `rate`, mono
// 44.1 kHz 16 bit out. Init fails where the resampler is missing: the caller then uses AudioToMono44k.
class AudioResampler {
public:
    AudioResampler(); ~AudioResampler();
    bool Init(int rate);
    int  Process(const float* mono, int frames, int16_t* out, int cap);     // the stream goes on between calls; returns the samples written
private:
    AudioResampler(const AudioResampler&); void operator=(const AudioResampler&);
    struct Impl; Impl* m;
};
void AudioMix(const int16_t* a, const int16_t* b, int n, int16_t* out);      // a + b, clipped
class Fifo16 {                                      // a queue of samples; Pop pads with silence
public:
    Fifo16(); ~Fifo16();
    int  Size() const;
    void Clear();
    void Push(const int16_t* s, int n);
    void Pop(int16_t* out, int n);
    void TrimTo(int keep);
private:
    Fifo16(const Fifo16&); void operator=(const Fifo16&);
    int16_t* m_buf; size_t m_cap, m_head, m_tail;
};
class SpeakerCapture {                              // WASAPI loopback of the default playback device
public:
    SpeakerCapture(); ~SpeakerCapture();
    bool Open();
    void Close();
    bool IsOpen() const;
    int  Pull(int16_t* out, int cap);               // what was played since the last call (mono 44.1 kHz)
private:
    SpeakerCapture(const SpeakerCapture&); void operator=(const SpeakerCapture&);
    struct Impl; Impl* m;
};

// ---- recorder (recorder.cpp) -----------------------------------------------------------------------------------
struct RecParams {
    RECT   rc;                      // screen rectangle (when hwnd is set it follows that window)
    HWND   hwnd;
    wchar_t path[MAX_PATH];
    Settings cfg;
};
class Recorder {
public:
    Recorder();
    ~Recorder();
    bool Start(const RecParams& p, wchar_t* err, int errLen);   // err also carries non-fatal warnings (no mic)
    void SetPaused(bool paused);
    bool Paused() const;
    bool Active() const;            // started and not stopped
    bool Failed() const;            // the worker stopped by itself because of an error
    bool LimitReached() const;
    double ElapsedSec() const;      // recorded time without pauses
    uint64_t FileBytes() const;
    uint64_t FramesWritten() const; // video frames handed to the encoder
    bool Stop();                    // finalizes the file; false when it could not be completed
    int  Width() const;
    int  Height() const;
private:
    struct Impl; Impl* m;
};

// ---- misc ------------------------------------------------------------------------------------------------------
void AppLogOpen(const wchar_t* name);   // %ProgramData%\Click2Screen\logs\<name>.log (applog.cpp)
void Log(const wchar_t* fmt, ...);
void ShowInExplorer(const wchar_t* path);
int  CurrentDpi();              // DPI of the main window's monitor
int  RunSelfTest(const wchar_t* outDir);
