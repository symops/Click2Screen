// Window, control and message identifiers of the Click2Screen program, shared with c2sautotest.exe (which drives the
// real program through them).
#pragma once

enum { IDC_REC_DESKTOP = 100, IDC_REC_REGION, IDC_REC_WINDOW, IDC_SHOT_DESKTOP, IDC_SHOT_REGION, IDC_SHOT_WINDOW,
       IDC_OPTIONS, IDC_STATUS, IDC_HINT, IDC_GRP_VIDEO, IDC_GRP_SHOT,
       IDC_P_TIME, IDC_P_SIZE, IDC_P_HINT, IDC_P_START, IDC_P_STOP, IDC_P_HIDE, IDC_MENU, IDC_MIN, IDC_CLOSE };
// A hotkey's id is its index in Settings::hotkey + 1.
enum { HK_DESKTOP = 1, HK_REGION, HK_WINDOW, HK_STOP, HK_REC_DESKTOP, HK_REC_REGION, HK_REC_WINDOW, HK_DRAW };
enum { ID_TRAY_SHOW = 200, ID_TRAY_PANEL, ID_TRAY_PAUSE, ID_TRAY_STOP, ID_TRAY_OPTIONS, ID_TRAY_EXIT, ID_MENU_PICTURES, ID_MENU_VIDEOS, ID_MENU_DRAW };
enum { TIMER_UI = 1, TIMER_COUNTDOWN = 2, TIMER_BLINK = 3, TIMER_TRAIL = 4 };
enum State { ST_IDLE, ST_ARMED, ST_COUNTDOWN, ST_RECORDING, ST_PAUSED };
#define WM_C2S_TRAY   (WM_APP + 1)
#define WM_C2S_SHOW   (WM_APP + 2)
#define WM_C2S_QUIT   (WM_APP + 3)     // sent by the installer: finish a recording, remove the tray icon and exit

enum { O_AUTOSTART = 300, O_MOUSE, O_HILITE, O_OPENFOLDER, O_SOUNDS, O_NOTIFY, O_MIC, O_LOOPBACK, O_DELAY, O_VCODEC, O_ACODEC,
       O_VQ, O_AQ, O_FPS, O_DH, O_DM, O_DS, O_VIDFOLDER, O_IMGFOLDER, O_BRVID, O_BRIMG, O_IMGFMT, O_TPL, O_LANG,
       O_TABS, O_DEFAULTS, O_EXPORT, O_IMPORT, O_HK0, O_HK1, O_HK2, O_HK3, O_HK4, O_HK5, O_HK6, O_HK7 };

// Class names of the program's windows.
#define C2S_CLASS_MAIN    L"Click2ScreenMain"
#define C2S_CLASS_PANEL   L"Click2ScreenPanel"
#define C2S_CLASS_OPTIONS L"Click2ScreenOptions"
#define C2S_CLASS_PICKER  L"Click2ScreenPicker"
#define C2S_CLASS_FRAME   L"Click2ScreenFrame"
#define C2S_CLASS_INK     L"Click2ScreenInk"      // the see-through window that takes the mouse while the drawing is on
