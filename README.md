# Click2Screen — screen recorder and screenshot tool for Windows 10/11

Click2Screen records the desktop, a region or a window to MP4 (H.264 video, AAC or MP3 sound from the microphone) and takes
screenshots of the desktop, a region or a window, with global hotkeys, a tray icon and a small editor. It is a plain Win32
program (no frameworks, no codec packs: the encoder is the one Windows ships with). A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) and [Show2Cam](https://github.com/symops/Show2Cam); the installer works the
same way.

The functionality was worked out from a static analysis of a freeware screen recorder (see `analysis/FINDINGS.md`: what was
found, with the evidence); the code is written from scratch and shares nothing with it. Its advertising, licensing,
watermarks and online components are deliberately not reproduced.

## What it does

| | |
|---|---|
| **Record desktop / region / window** | The region is dragged over a dimmed picture of the desktop, a window is pointed at. A small panel (excluded from the recording) has Start / Pause / Stop and shows the time and the file size; **F11** stops. The recorded window may move and be resized while recording. |
| **Screenshots** | `Ctrl+PrintScreen` — desktop (all monitors), `Alt+Shift+4` — region, `Alt+Shift+5` — window. PNG, JPEG or BMP. |
| **Editor** | After a screenshot (optional): red rectangles, undo, zoom, Save / Save as. |
| **Delay and countdown** | 0–60 s before a recording or a screenshot, with a tick each second (sounds can be switched off). |
| **Time limit** | Stop a recording after hours / minutes / seconds. |
| **Microphone** | The default microphone, mono 16-bit, 44.1 kHz → AAC or MP3 in the MP4; muted while paused. If it cannot be opened the video is recorded without sound (a message says so). |
| **Pointer** | Drawn into recordings and screenshots, optionally with a translucent halo. |
| **Quality** | H.264 "best" or "fast"; video quality high / medium / low (bit rate follows the frame size and the frame rate), 10–60 fps, audio quality high / medium / low. The frame is at most 1920×1080 (the aspect is kept), both sides even. |
| **Files** | Name template `$Y-$M-$D at $H-$N-$S`, `Img####` (the `#` run is a sequence number), `%USERNAME%`; never overwrites; "open the folder and select the file" after capture. |
| **Tray** | Closing the window keeps the program (and the hotkeys) in the tray; click = start / pause, right click = menu. Starts with Windows in the tray if you want (`/tray`, also `/t`, `-t`, `--tray`). |
| **Settings** | Options window (General, Video and audio, Files, Language); export / import to an `.ini` (UTF-16; the language is not part of it); "Restore default". |
| **Languages** | 17: English, Русский, Українська, Беларуская, Deutsch, Français, Español, Italiano, Português (Brasil), Polski, Nederlands, Türkçe, Bahasa Indonesia, Tiếng Việt, 中文(简体), 日本語, 한국어 — the Windows language by default. |
| **DPI** | Per-monitor high DPI; the windows rescale when they move to another monitor. |

How a recording works: every frame is copied from the screen with GDI (`BitBlt` with `CAPTUREBLT`, so layered windows are
included) into a DIB, the pointer is drawn on top, and the frame goes to a Media Foundation sink writer with a media clock
that stops while paused; the microphone is read in 50 ms buffers through `waveIn` and written to the same file with a
sample-count clock. Windows 8.1 or later is needed for `WDA_EXCLUDEFROMCAPTURE` (Windows 10 2004+ keeps the panel out of the
picture; older systems show it).

## Installation

Run `Click2Screen-Setup.exe` (administrator rights). It picks the 64- or 32-bit installer, which copies the program to
`C:\Program Files\Click2Screen`, registers it in *Apps & features*, creates the shortcuts you tick and optionally starts
the program with Windows (in the tray). "Remove" in the same window (or `uninstall.cmd`) takes it all away; your settings and
the screenshots / videos you made are kept. For scripts:

```
Click2Screen-Setup.exe /install /silent [/autostart] [/desktop-] [/startmenu-]
Click2Screen-Setup.exe /remove /silent
```

(the exit code is 0 on success). See `docs/INSTALL.en.txt` / `docs/INSTALL.txt`.

## Building (Linux)

```
./package.sh
```

needs clang, MinGW-w64 for x86_64 and i686, gcab and Python 3 (Pillow only for `tools/make_icon.py`). Output:
`dist/Click2Screen-Setup.exe` (the whole package in one file: a launcher with the package appended as a cabinet) and the same
unpacked in `dist/Click2Screen/` (`x64\`, `x86\`, `uninstall.cmd`). `./build.sh` (and `ARCH=x86 ./build.sh`) builds only the
programs into `build/x64` / `build/x86`. Link maps are kept per version in `maps/<version>/`.

The interface strings live in `src/lang/<code>.txt` (one line per string of `enum Str` in `src/c2s.h`, `\n` = line break);
`python3 tools/gen_lang.py` checks every file (line count, printf placeholders) and writes `src/lang_table.inc`.

## Tests

* `Click2Screen.exe /selftest <folder>` — a short check of sizes, bit rates, the name template, screenshots and the frame
  schedule; `selftest.log` in the folder.
* **`c2sautotest.exe [minutes] [--seed N] [--keep] [--installer]`** — a randomized end-to-end test. Every test runs with random
  parameters drawn from one seeded generator (the seed is printed first; the same seed repeats the same sequence of tests and
  parameters). A window of four coloured quarters is shown on the screen and everything is checked as a user would see it:
  screenshots and recordings of random parts of it (size, orientation, colours, with / without the pointer and the halo,
  scaled, partly outside the screen), saving in every format and decoding it back, the settings through the registry and the
  `.ini` export (including bad values and foreign files), random file name templates against an independent oracle and
  numbering, `FitSize` / `AutoBitrate` properties, every language table (complete, same line breaks and placeholders), the
  region and window pickers driven by messages, the editor (rectangles drawn and saved), global hotkeys with injected keys,
  recorder start / stop cycles and refusals, the frame schedule with pauses, pause spam from another thread, the time limit,
  a window that moves while recorded and — with Media Foundation — the MP4 itself: H.264, the size, the duration, the frame
  count, a decoded frame's colours and the microphone track. `--installer` (administrator) adds a random
  install / update / remove cycle of the installer. PASS / FAIL / WARN / SKIP go to the console and to
  `C:\ProgramData\Click2Screen\logs\autotest.log`; exit code 1 if anything failed. Under Wine the Media Foundation encoder
  does not exist, so the encoder checks are skipped and the schedule is tested without it.

## Logs

`C:\ProgramData\Click2Screen\logs\` — `panel.log` (the program), `setup.log` (installer), `autotest.log`; UTF-8 with
timestamps, over 1 MB a log becomes `*.old.log`. A crash writes the module and the offset of the failing instruction
(`maps/<version>/` names the function).

## Limitations

* Windows 10 or later (the program also runs on Windows 8.1 without the panel being hidden from the picture).
* GDI capture: no DRM-protected video, and a few games with exclusive full screen show black.
* The H.264 / AAC / MP3 encoders are those of Windows; "N" editions of Windows need the Media Feature Pack.
* The microphone only (no loopback of what the computer plays).

## Author

Symo — symops@gmail.com
