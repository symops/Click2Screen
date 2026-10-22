# Click2Screen — screen recorder and screenshot tool for Windows 10/11

Click2Screen records the desktop, a region or a window to MP4 (H.264 or H.265), WMV, AVI or an animated GIF (sound from the
microphone where the format has sound) and takes screenshots of the desktop, a region or a window, with global hotkeys and a tray icon. It is a plain Win32
program (no frameworks, no codec packs: the encoder is the one Windows ships with). A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) and [Show2Cam](https://github.com/symops/Show2Cam); the installer works the
same way. **It never touches the network**: no update check, no telemetry, no downloads (a test checks that).

The functionality was worked out from a static analysis of a freeware screen recorder (see `analysis/FINDINGS.md`: what was
found, with the evidence); the code is written from scratch and shares nothing with it. Its advertising, licensing,
watermarks and online components are deliberately not reproduced.

## What it does

| | |
|---|---|
| **Record desktop / region / window** | The desktop gives **one file for each monitor** (like the screenshots); the pointed window or the dragged region is one file, with a red frame round it that blinks while the recording runs (steady while the area is only chosen or paused). The region is dragged over a dimmed picture of the desktop, a window is pointed at (the bare desktop counts as a window: its work area, without the taskbar). A small panel (excluded from the recording) has Start / Pause / Stop and shows the time and the file size; `Ctrl+F11` (changeable) stops. By default `Ctrl+F8` / `F9` / `F10` start a desktop / region / window recording; `Ctrl+F12` switches the **drawing** on and off, with or without a recording: a see-through window covers the screen and takes the mouse — the left button draws a line in a smooth line (anti-aliased, the path is rounded) in a gradient of all the colours of the rainbow, a full turn in 8 seconds of drawing (every point keeps the colour it was drawn in), the right button wipes all of them, Esc or the same hotkey leaves; lines live 10 seconds. Without a recording of its own the line is part of the screen, so a screen sharing (a messenger, a video call) and other recorders see it; while Click2Screen records, the line is drawn into its video; Windows does not let this see-through window be kept out of the capture, so the line is on the screen too and the two copies lie in one place. It works over windows of any privilege (programs run as administrator), because the mouse goes to Click2Screen's own window — while it is on, clicks do not reach the program below. The keyboard is not used for drawing (Shift does nothing): only the mouse, so it behaves the same everywhere. The recorded window may move and be resized while recording. |
| **Screenshots** | By default `Ctrl+PrintScreen` — desktop (**each monitor in its own file**, not one merged picture), `Ctrl+F6` — region, `Ctrl+F7` — window. The hotkeys are global and (screenshots, the three recordings and the stop key) can be changed or switched off in Options > Hotkeys (a field takes the next combination pressed; Backspace clears it; two actions cannot share one). PNG, JPEG, BMP, GIF, TIFF, WebP or JPEG XR (the list shows the ones this Windows can write). |
| **Delay and countdown** | 0–10 s (a drop-down list, 0 by default) before a recording or a screenshot: big red digits 3 – 2 – 1 in the middle of the chosen area (of every monitor for the whole desktop) and a short mechanical click each second, made after the original's one (the only sound; it can be switched off). The red frame blinks while a screenshot is being made too. |
| **Microphone** | The default microphone, mono 16-bit, 44.1 kHz → AAC or MP3 in the MP4; muted while paused. If it cannot be opened the video is recorded without sound (a message says so). |
| **Speakers** | "Record sound from the speakers" (on by default): what the default playback device plays (WASAPI loopback), mixed with the microphone into the same track. While nothing plays the track is silence; if the device cannot be captured the recording goes on without it. |
| **Pointer** | Drawn into recordings and screenshots. The pointer is shown with "Show the mouse pointer" (on by default). In a **recording**, "Outline and click waves around the pointer (video only)" (on by default, available only while the pointer is shown) adds a translucent yellow circle that follows the pointer and a wave for every click — left button blue, right red, middle purple. Screenshots never get the circle. Switch the **drawing** on (`Ctrl+F12`, also while recording) and draw lines with the left mouse button: they are in the video and on the screen at once, each point goes 10 seconds after it was drawn (the oldest first), and the right mouse button wipes all the lines immediately. |
| **Video formats** | MP4 with H.264 ("best" or "fast"), MP4 with H.265 / HEVC (when Windows has the encoder), WMV (Windows Media Video + Windows Media Audio), AVI (Motion-JPEG + PCM sound, written by the program itself) and animated GIF (no sound, at most 960 × 540 and 15 frames a second). Every format ships with Windows: no codec packs. |
| **Quality** | Video quality high / medium / low (bit rate follows the frame size and the frame rate), 10–60 fps, audio quality high / medium / low. The frame is at most 1920×1080 (the aspect is kept), both sides even. |
| **Files** | One folder for the videos and the pictures (`Videos\Click2Screen` by default). Name template `File $D.$M.$Y $H-$N-$S`, `Img####` (the `#` run is a sequence number), `%USERNAME%`; the Options window shows under the field how a file will be named now (without the extension); never overwrites; "open the folder and select the file" after capture. |
| **Tray** | Like Telegram or Discord: **Minimize** is an ordinary minimize (the window waits on the taskbar; one click or Alt+Tab brings it back), **Close** (the cross) sends it to the tray — no taskbar button, the program and the hotkeys keep running; click on the tray icon = show the window (start / pause while recording), right click = menu, "Exit" quits. A screenshot or a recording leaves a minimized window minimized. Starts with Windows in the tray if you want (`/tray`, also `/t`, `-t`, `--tray`). |
| **Look** | As small and flat as the original: one 206 × 62 window (a dark caption and a row of six icons — three screenshots, three recordings — and a menu), a dark 230 × 29 strip with the time, the size, "F11: Stop" (the key in use), Start / Pause and Stop while recording, a red frame around the recorded area. No advertising, no watermark, no premium offers, no network. |
| **Settings** | Options window (General — delay, autostart, pointer, highlight, sounds, notifications in the notification area (on by default), microphone, speakers; Formats — video and audio codec and quality, frame rate, picture format; Saving — the folder, the name template; Language; Hotkeys; "Restore default" sets back the open page only); export / import to an `.ini` (UTF-16; the language is not part of it); "Restore default". |
| **Languages** | 17: English, Русский, Українська, Беларуская, Deutsch, Français, Español, Italiano, Português (Brasil), Polski, Nederlands, Türkçe, Bahasa Indonesia, Tiếng Việt, 中文(简体), 日本語, 한국어 — the Windows language by default. |
| **DPI** | Per-monitor high DPI; the windows rescale when they move to another monitor. |

How a recording works: every frame is copied from the screen with GDI (`BitBlt` with `CAPTUREBLT`, so layered windows are
included) into a DIB, the pointer is drawn on top, and the frame goes to a Media Foundation sink writer with a media clock
that stops while paused; the microphone is read in 50 ms buffers through `waveIn` and written to the same file with a
sample-count clock. Windows 8.1 or later is needed for `WDA_EXCLUDEFROMCAPTURE` (Windows 10 2004+ keeps the panel out of the
picture; older systems show it).

## Installation

Run `Click2Screen-Setup.exe` (administrator rights). It picks the 64- or 32-bit installer, which copies the program to
`C:\Program Files\Click2Screen` (or the folder you choose in the window: "Install to" + "Browse..."), registers it in *Apps & features*, creates the shortcuts you tick and optionally starts
the program with Windows (in the tray). "Remove" in the same window (or `uninstall.cmd`) takes it all away; your settings and
the screenshots / videos you made are kept. For scripts:

```
Click2Screen-Setup.exe /install /silent [/dir "D:\Tools\Click2Screen"] [/autostart] [/desktop-] [/startmenu-]
Click2Screen-Setup.exe /remove /silent
```

(the exit code is 0 on success). Installing the same version again offers "Reinstall" (a newer one: "Update"); changing the folder
moves the program there and removes the old copy. Versions are numbered as in Speak2Mic / Show2Cam:
`1.0.<day of the year>.<minute of the day>` of the build. See `docs/INSTALL.en.txt` / `docs/INSTALL.txt`.

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
* **`c2sautotest.exe [minutes] [--seed N] [--only a,b] [--keep] [--installer] [--slow N]`** — a randomized end-to-end test.
  Every test runs with random parameters drawn from one seeded generator (the seed is printed first; the same seed repeats
  the same sequence of tests and parameters). A window of four coloured quarters is shown on the screen and everything is
  checked as a user would see it:
  * *capture and files*: screenshots and recordings of random parts of the window (size, orientation, colours, with / without
    the pointer and the halo, scaled, partly outside the screen), saving in every format and decoding it back, parallel
    captures from several threads, leaks of GDI / USER objects and handles over many captures and recordings;
  * *settings*: registry round trip, `.ini` export / import (bad values, foreign files, UTF-16), garbage in the registry and
    in `.ini` files (random bytes, wrong value types), random file name templates against an independent oracle, numbering,
    very long paths with guard bytes after the buffer, `FitSize` / `AutoBitrate` properties, every language table
    (complete, same line breaks and placeholders);
  * *the real program*, started as a child with its own settings key and driven by messages: the language of every control,
    a second copy handing over to the first, hotkey screenshots of the desktop / a dragged region / a pointed window (size
    and colours of the file), recording through the panel (countdown, pause, resume, hide, tray command, F11, Stop), the options window (OK stores exactly what it showed, an invalid template is refused, Cancel changes nothing,
    language switching, the autostart box writes and removes the `Run` entry), hotkeys held
    by another program, an unusable folder, and a "monkey" that does 25–50 random things in a row while the program must stay
    alive and answering; Exit ends it with code 0;
  * *pickers*: the region and window pickers driven by messages (any drag direction, a stray click first, Esc);
  * *recorder*: start / stop cycles and refusals, the frame schedule with pauses, pause spam from another thread, the time
    limit, a long recording without drift, a window that moves while recorded and the video file itself in a random format: with
    Media Foundation the MP4 / WMV (codec, size, duration, frame count, a decoded frame's colours, the microphone track); the
    AVI is parsed by the test (RIFF sizes, index, frame count, a JPEG frame decoded) and the GIF decoded frame by frame (size,
    delays, colours) — those two also run under Wine; every picture format is saved and read back;
  * *installer*: with `--installer` (administrator) a random install / update / remove cycle, silently and through the
    installer window (boxes, log, autostart / shortcut boxes, Remove asks first); the installer window in every language;
  * *no network*: the binaries import no networking library and contain no address (the program never talks to the network).

  PASS / FAIL / WARN / SKIP go to the console and to `C:\ProgramData\Click2Screen\logs\autotest.log`; when a check about the
  real program fails, the last lines of its own log are printed too; exit code 1 if anything failed. Under Wine the Media
  Foundation encoder does not exist, so the encoder checks are skipped and the schedule is tested without it; waits are
  multiplied (`--slow N` sets the factor by hand).
* **`tools/wine-soak.sh`** — a soak test under Wine at the lowest priority (`nice -n 19`, idle I/O): random seeds, x64 and
  x86 in turn, the self-test and the package test (below) in between, until the program has passed for 20 minutes in a row.
  A failure, or a change of the program binaries, resets the stopwatch. Builds are reproducible
  (`--no-insert-timestamp`), and `ONLY=autotest ./build.sh` relinks only the autotest, so tests can grow during the soak.
* **`tools/test-package-wine.sh`** (after `./package.sh`) — the finished package under Wine: the single `Click2Screen-Setup.exe`
  and the unpacked folder install the right files (byte for byte the x64 / x86 build), clean up their temporary folder and
  remove cleanly.

## Logs

`C:\ProgramData\Click2Screen\logs\` — `panel.log` (the program), `setup.log` (installer), `autotest.log`; UTF-8 with
timestamps, over 1 MB a log becomes `*.old.log`. A crash writes the module and the offset of the failing instruction
(`maps/<version>/` names the function).

## Limitations

* Windows 10 or later (the program also runs on Windows 8.1 without the panel being hidden from the picture).
* GDI capture: no DRM-protected video, and a few games with exclusive full screen show black.
* The H.264 / AAC / MP3 encoders are those of Windows; "N" editions of Windows need the Media Feature Pack.

## Author

Symo — symops@gmail.com
