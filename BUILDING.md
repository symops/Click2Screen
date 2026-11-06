# Building Click2Screen (screen recorder and screenshot tool for Windows 10/11) from a clean Debian 13

This describes building **Click2Screen** (repo `symops/Click2Screen`) from scratch on a fresh Debian 13 ("trixie") machine. It is a cross-build: the Windows programs are compiled on Linux with clang and MinGW-w64, for 64-bit (x64) and 32-bit (x86) Windows, and packed into one self-extracting installer, `Click2Screen-Setup.exe`. Nothing Windows-specific is needed to build: no Windows SDK, no Visual Studio, no Wine (Wine is only used by the optional tests in section 5).

## 1. Install the toolchain and build dependencies

```sh
sudo apt update
sudo apt install -y \
    git ca-certificates \
    clang \
    gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 \
    binutils-mingw-w64-x86-64 binutils-mingw-w64-i686 \
    mingw-w64-x86-64-dev mingw-w64-i686-dev \
    python3 python3-numpy python3-pil gcab
```

What each part is for: `clang` compiles the C++17 sources (`-fno-exceptions -fno-rtti`, no STL; the few C++ runtime hooks live in `src/cxxrt.cpp`) against the MinGW headers in `/usr/<triple>/include`; the MinGW `gcc` links the objects statically and its `binutils` provide `windres` for the resources (version information, icons, manifest); `python3` runs `tools/gen_lang.py` (the translations in `src/lang/*.txt` become `src/lang_table.inc`) and `tools/make_icon.py` (draws the icons, hence `python3-numpy` and `python3-pil`); `gcab` makes the cabinet the installer carries. `iconv`, `sed` and `find` come with the base system.

## 2. Clone the repository

```sh
git clone https://github.com/symops/Click2Screen.git   # or git@github.com:symops/Click2Screen.git
cd Click2Screen
```

The build output (`build/`, `dist/`, `maps/`, `*.exe`, `*.o`, `*.map`) is not tracked in git.

## 3. Build the programs

```sh
ARCH=x64 ./build.sh
ARCH=x86 ./build.sh
```

`build.sh` regenerates the language table first, then compiles and links for the architecture in `ARCH` (`x64` is the default). The output goes to `build/x64/` and `build/x86/`:

| File | What it is |
|---|---|
| `Click2Screen.exe` | the recorder / screenshot program |
| `Click2Screen-Setup.exe` | the graphical installer of that architecture (installs to Program Files, shortcuts, removal) |
| `c2sautotest.exe` | the randomized autotest (section 5) |
| `c2slauncher.exe` | **x86 only**: the package's root `Click2Screen-Setup.exe`, it starts the x64 or the x86 installer, whichever fits the Windows it runs on |
| `*.map` | link maps: a crash line in the program's log names a module and an offset, the map names the function |

The compiler flags are in `build.sh`: `-Wall -Wextra`, and **the build must print no warning** — fix a warning, do not silence it. `ONLY=autotest ARCH=x64 ./build.sh` relinks only `c2sautotest.exe` and leaves the program binaries byte for byte as they were, which lets the tests grow while a long test runs.

## 4. Package

```sh
./package.sh
```

This runs section 3 for both architectures and assembles the release:

- `dist/Click2Screen-Setup.exe` — **the release, one file for 64- and 32-bit Windows**: the x86 launcher with the whole package appended as a cabinet (MSZIP), followed by the trailer `C2SPAYLD` and the cabinet's size. When it runs, Windows extracts the cabinet into a temporary folder and starts the installer of the running Windows.
- `dist/Click2Screen/` — the same package unpacked (`x64\`, `x86\`, `uninstall.cmd`, the install notes) for development and testing.
- `maps/<version>/` — the link maps of this build, kept per version.

**Every package gets a new version, `1.0.<day of year>.<minute of day>`, written into `src/version.h`** (a tracked file, so `git status` shows it as modified afterwards: commit it, as the project does with its "Build number" commits, or drop it with `git checkout src/version.h`). `C2S_KEEP_VERSION=1 ./package.sh` keeps the version, which is what you want when you rebuild the package of the same program and expect the same bytes.

Check the result:

```sh
ls -l dist/Click2Screen-Setup.exe
tail -c 16 dist/Click2Screen-Setup.exe | head -c 8; echo    # prints C2SPAYLD
```

The size and the SHA-256 of the file depend on the compiler and `gcab` versions, so a package built on another machine is not byte for byte the file of a published release.

## 5. Check the package (optional)

The tests run under Wine, in a virtual X display, and need no Windows. **Wine needs its 32-bit part too** (`wine32`, from the `i386` architecture): the package's root `Click2Screen-Setup.exe` is the 32-bit launcher, and without it Wine stops with `failed to load ...\syswow64\ntdll.dll`.

**The order matters, because of how Debian's Wine picks the kind of a new prefix** (as seen on Debian 13 with Wine 10.0): with `wine32` installed and no prefix yet, the first prefix is a 32-bit one, the launcher then installs the x86 program and the package test, which expects the x64 one, fails. So a 64-bit prefix is made *before* `wine32` is installed; after that, new prefixes are 64-bit too. That first prefix (`~/.wine`) is not used afterwards — it was made without `wine32` and stops working with `could not load kernel32.dll, status c0000135` once `wine32` is there — the tests use prefixes of their own.

```sh
sudo apt install -y wine xvfb xauth
xvfb-run -a wine cmd /c ver
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install -y wine32:i386
tools/test-package-wine.sh
```

`tools/test-package-wine.sh` checks the finished package: the single `Click2Screen-Setup.exe` and the unpacked folder install the right files (byte for byte the x64 build), clean up their temporary folder and remove cleanly. It prints `PASS` / `FAIL` per check and exits with 1 on a failure.

The randomized autotest runs the same way:

```sh
export WINEPREFIX="$HOME/.wine-c2s"
LANG=C.UTF-8 xvfb-run -a wine build/x64/c2sautotest.exe 2 --seed 1
```

`WINEPREFIX` gives the autotest a new prefix of its own (see above). **Run it with a UTF-8 locale** (`LANG=C.UTF-8` is always there on Debian): the tests save files with random non-Latin letters in their names, which Wine cannot list without a UTF-8 locale (a handful of "the picture is not saved" failures then). The run takes about 8 minutes and ends with a line like `59 test(s) run ...: 320 PASS, 0 FAIL, 2 WARN, 14 SKIP`. `2` is the time budget in minutes, `--seed` makes the run repeatable, `--only name,name` selects tests. Under Wine the Media Foundation encoder does not exist, so the encoder checks are skipped (a `SKIP` line each) and the rest is tested; the full set, including the encoders and the multi-monitor tests, needs a real Windows 10 / 11. PASS / FAIL / WARN / SKIP go to the console; the exit code is 1 if anything failed. `tools/wine-soak.sh` repeats such runs (random seeds, x64 and x86 in turn) until the program has passed for 20 minutes in a row.

## 6. Install on Windows for testing

Copy `dist/Click2Screen-Setup.exe` to a Windows 10 / 11 machine (a virtual machine with a snapshot is best) and run it as administrator. Without a window:

```sh
Click2Screen-Setup.exe /install /silent [/dir "D:\Tools\Click2Screen"] [/autostart] [/desktop-] [/startmenu-]
Click2Screen-Setup.exe /remove /silent
```

The exit code is 0 on success. Removal also works from Windows' **Apps** and with `uninstall.cmd`; the settings and the pictures and videos you made are kept. The program's log is `C:\ProgramData\Click2Screen\logs\panel.log`.

## 7. Code checks (optional)

The compiler check is section 3: no warning with `-Wall -Wextra`, for x64 and x86. The Clang Static Analyzer needs nothing more than the `clang` of section 1:

```sh
cd src
for f in *.cpp; do
    clang++ --target=x86_64-w64-windows-gnu -std=c++17 -fno-exceptions -fno-rtti -municode \
        -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -isystem /usr/x86_64-w64-mingw32/include \
        --analyze -Xclang -analyzer-output=text -o /dev/null "$f" 2>&1 | grep 'warning:' || true
done
cd ..
```

It reports one known false positive, `c2ssetup.cpp` "Potential leak of memory pointed to by 'copy'": the string is freed by the window that receives the message, or on the spot when the message could not be posted.

## 8. Publish a release (maintainers)

A release is a git tag `v<version>` on the commit of `main` that was built, with `dist/Click2Screen-Setup.exe` as its one file and the SHA-256 of that file in the notes (needs the `gh` CLI and push rights):

```sh
sha256sum dist/Click2Screen-Setup.exe
gh release create "v1.0.310.1154" dist/Click2Screen-Setup.exe \
    --title "Click2Screen 1.0.310.1154" --notes-file RELEASE-NOTES.md --latest
```

`--target` must be the full 40-character commit hash when the tag does not exist yet.
