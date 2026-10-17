@echo off
rem Removes Click2Screen (program, shortcuts, autostart, "Apps & features" entry). Run as Administrator.
rem Your settings and the screenshots / videos you made are kept.
net session >nul 2>&1 || (echo Run this file as Administrator. / Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"
set ARCH=x86
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set ARCH=x64
if /i "%PROCESSOR_ARCHITEW6432%"=="AMD64" set ARCH=x64
rem The package root has x64\ and x86\; an installed copy has Click2Screen-Setup.exe next to it.
if exist Click2Screen-Setup.exe (start /wait Click2Screen-Setup.exe /remove /silent) else (start /wait %ARCH%\Click2Screen-Setup.exe /remove /silent)
echo Done. / Готово.
pause
