# Free Screen Video Recorder 3.1.4.617 — журнал находок (REA)

Цель: `FreeScreenVideoRecorder_3.1.4.617_o.exe` (Inno Setup 6.4.3, Digital Wave Ltd / DVDVideoSoft),
SHA-256 `b152a3205c7b1c439174e0d9ebf63ba1f07d64205f015ada8d3217250c84ec61`.
Рекордер внутри: `FreeScreenVideoRecorder.exe` (PE32 i386, Qt5, 3 493 808 байт),
SHA-256 `b3c35875320087402bd77efeb78b214171d9613e1a2134d078038deec9a0817d`.
Анализ: статический (инсталлятор и exe не запускались). REA 6.5.0 + Ghidra 12.1.4.
Распаковка инсталлятора: innounp 2.71.2 (Wine, запускался только распаковщик).

Обозначения: [O] наблюдение, [I] вывод, [U] неизвестно.

## 1. Состав и архитектура
- [O] Inno Setup 6.4.3: `{app}\Free Screen Video Recorder\*` + `{localappdata}\DVDVideoSoft\{bin,lib}` (общие компоненты Free Studio). Файл `analysis/fsvr-extracted/install_script.iss`.
- [O] Импорты exe: Qt5Core/Gui/Widgets/Multimedia/PrintSupport/WinExtras, `mmcore.dll`, `tier0.dll`, `dlmgr.dll`, `libcurl`, `libcrypto`, GDI32, USER32, MSIMG32 (`evidence/exe.imports.txt`).
- [O] `mmcore.dll` экспортирует `createGraphEngine/createMediaInput/createMediaInputBuffered/createMediaOutput/createFramePool/createTimeline/createCutKit`; импортирует ffmpeg 3.x (`avcodec-57`, `avformat-57`, `avfilter-6`, `swresample-2`) и содержит классы `CAmfEncoder`, `CAmfContextDx9` (AMD AMF). [I] Кодирование — через собственный граф поверх ffmpeg, с аппаратным AMD-энкодером.
- [O] `mdevhelper.dll` экспортирует `testAtiGPU/testNvGPU/testDirectX` — проверка GPU для выбора энкодера.
- [O] Исходники по пути отладки: `src/{main,mainform,mainform_upgrade,capture_dialog,globhotkeys,reccon,wincap}.cpp`, `src/recorder/{recorder,framesrc,framesink,capmic}.cpp`.

## 2. Горячие клавиши (подтверждено декомпиляцией)
- [O] Evidence `ev_a411d260…`, `FUN_00462230` (`globhotkeys.cpp`): отдельный поток вызывает `RegisterHotKey(NULL, atom, mods, vk)`:
  - Alt+Shift+4 → mods `0x5`(ALT|SHIFT), VK `0x34`; запись региона.
  - Alt+Shift+5 → mods `0x5`, VK `0x35`; запись окна.
  - Ctrl+PrintScreen → mods `0x2`(CTRL), VK `0x2C`; запись рабочего стола.
  - На Windows ≥ 8 к модификаторам добавляется `MOD_NOREPEAT` (`0x4000`).
  - Цикл `GetMessageW`, обработка `WM_HOTKEY` (0x312); выход по `WM_APP+2` (0x402). Лог-теги `GlobHk`.
- [O] Строка `F11: Stop`, `Hotkey to stop recording`; `PrintScreen` — снимок рабочего стола.

## 3. Захват экрана (подтверждено)
- [O] Evidence `ev_8c06f7e2…`, `FUN_00472dc0` (`wincap.cpp`, тег `ScrCap`): `GetDC(NULL)` + `BitBlt(memDC, 0,0,w,h, screenDC, x,y, 0x40CC0020)` (SRCCOPY|CAPTUREBLT). Размер буфера пересоздаётся, если w/h изменились. Если x/y не заданы (0x80000000) — `SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN` (виртуальный рабочий стол, несколько мониторов).
- [O] Курсор: `GetCursorInfo` (только если `CURSOR_SHOWING`), `GetIconInfo`, `DrawIcon` с учётом hotspot. Опционально (флаг param_6) перед ним `AlphaBlend` 50×50 из bitmap 100×100 (ctx+0x44) — подсветка курсора.
- [U] Выбор между GDI и другими способами (DXGI) — строк DXGI/DDA в exe нет (поиск по строкам и импортам), значит [I] только GDI.

## 4. Запись: размеры и битрейт (подтверждено)
- [O] Evidence `ev_f64c9f9c…`, `FUN_00471ec0` (`recorder.cpp`, тег `Recorder`):
  - ширина = min(0x780 = 1920, (w+1)&~1), высота = min(0x438 = 1080, (h+1)&~1): кадр не больше 1920×1080, стороны чётные;
  - если битрейт в настройках не задан (0): по умолчанию 2 000 000; затем перебор 5 записей таблицы по адресу `0x4af8ac`: первая запись, у которой ширина кадра < W и высота кадра < H, даёт `битрейт_записи × (w·h)/(W·H)`; лог «Autoselected Video Bitrate: N»;
  - темп кадров передаётся парой (значение, 100) либо (значение, 1) из настроек; граф ffmpeg-обёртки создаётся строкой `"[vin]null[vout]"` (createGraphEngine).
- [O] Таблица (Evidence `ev_7abf6f99…`, `read_bytes` 0x4af8a8, декодировано выше):

| кадр меньше | битрейт, бит/с |
|---|---|
| 426×240 | 384 000 |
| 640×360 | 768 000 |
| 854×480 | 1 500 000 |
| 1280×720 | 2 000 000 |
| 1920×1080 | 4 000 000 |

  [I] Для кадра 1920×1080 ни одна запись не подходит (нужно строго «меньше»), остаётся 2 000 000, а на каждой границе записи битрейт скачет (наблюдаемое поведение оригинала). В Click2Screen вместо шагов — кусочно-линейная интерполяция по числу пикселей через те же пять опорных точек (`AutoBitrate` в `src/capture.cpp`): в узлах значения таблицы, ниже первого узла пропорционально площади, выше последнего 4 Мбит/с; функция монотонна (проверяется автотестом).
- [U] Темп кадров по умолчанию: источник возвращает его виртуальным методом (+0x24 в `FUN_00471ec0`); значение не установлено статически.

## 5. Микрофон (подтверждено)
- [O] Evidence `ev_c88f60cd…`, `FUN_0046f0b0` (`capmic.cpp`, тег `CAMPIC`): `QAudioDeviceInfo::defaultInputDevice()`; формат `audio/pcm`, 1 канал, 16 бит, little-endian, signed; если формат не поддержан — `nearestFormat`, и принимается только моно/16 бит/pcm; буфер `QAudioInput` = 50 мс (`rate*50/1000*2` байт). Сообщения: «Cant Init Audio Capture Device», «Audio Buffer Underflow», «Audio Capture suspend/resume» (пауза).

## 6. Конвейер кадров
- [O] Evidence `ev_138beb8a…`, `FUN_00471920` (`framesrc.cpp`, тег `FrameScr`): взять кадр из пула → заполнить → отправить в приёмник; ошибки: «Cant Push Frame Err», «FrameSource ::fillFrame Error!». Пулы: «Created Video/Audio Frame Pool».

## 7. Интерфейс и настройки (по строкам формы и ресурсам; уровень «наблюдение за строками», без декомпиляции)
Источник: `evidence/exe.strings.ascii.txt`, строки ~860–1300.
- Главное окно: кнопки «Record desktop / region / window», «Capture desktop (PrintScreen) / Region (Alt+Shift+4) / Window (Alt+Shift+5)», «Options».
- Панель записи (`recWidget`): время (`lblRecTime`), размер файла (`lblRecSize`, «0.0 Mb»), подсказка «F11: Stop», кнопки старт/пауза (`btnRecStartPause`: «Click to start record video, click again to pause recording») и стоп (`btnRecStop`); сообщения «Pause video recording», «Start/Stop video recording».
- Трей: «Hide control panel? You will control recording from the System Tray» (`trayClickedRecPause`, `trayClickedStop`).
- Обратный отсчёт: `onRecCountdown`, `delay.wav`; «Delay before capture» (в секундах).
- Мониторы: «DISPLAY1», «DISPLAY2» (выбор области на нужном экране), «Position:».
- Опции, вкладка General: «Start application when my computer starts» (`RunAtStartup`), «Include Mouse pointer in the capture» (`CapMouse`), «Enable sounds» (`EnableSound`), «Record audio from microphone» (`EnableMicRec`), «Open output folder after capture» (`OpenOutFolder`, открывает `explorer.exe /n, /e, /select, <файл>`), «Open Editor window after capture».
- Видео и аудио: Video codec «H.264 (slower)» / «H.263 (faster)» (`VideoCodec`), Audio codec «AAC (slower)» / «MP3 (faster)» (`AudioCodec`), Quality «high / medium / low quality» (`VideoQuality`, `AudioQuality`), «Set record time: hours/minutes/seconds» (`RecDurationHour/Min/Sec`).
- Файлы: «Output folder:» (`VidOutputFolder`, `ImgOutputFolder`), «File name template:» по умолчанию `$Y-$M-$D at $H-$N-$S`, подстановки `#` (цифра порядкового номера), `$Y $M $D $H $N $S`, `%USERNAME%`; «Image Format» (`ImageFormat`); проверки «Output folder does not exist», «Can't create/write to the output folder».
- Редактор снимков (`CEditWindow`, `EditWindow`): прямоугольник (`btnRectangle`), масштаб +/− (`btnZoomIn/Out`), «Save As…» (`*.png`).
- Язык и обновления: список языков (22 `.qm`: cn, de, dk, es, fi, fr, gr, hu, it, ja, nl, pl, pt, ru, si, sk, sv, th, tr, tw …), кнопки «Check for updates», «Restore default» (`onResetDefault`).
- Служебное: `SetProcessDPIAware`/`SetProcessDpiAwareness` (DPI-aware), `videorec_tmp` (временный файл записи).

## 8. Что в оригинале НЕ относится к функционалу записи (в Click2Screen не переносится)
- [O] Бесплатная версия добавляет рекламный водяной знак в видео (`dvs_wm_*.png`, «Promo watermark will be added to the video») и в звук («Audio watermark will be added…»); платная Premium снимает. Лицензирование: `activate_premium_key`, `epremium{0}.dat`, проверка ключа на сайте, «License key was refunded/blocked».
- [O] Реклама/предложения (`IOfferManager`, `PremiumMembershipOffer.exe`, баннеры), загрузчик `dlmgr.dll` + `libcurl` (проверка обновлений, `LastCheckForUpdatesTimestamp`), общий менеджер `FreeStudioManager.exe`, `DVSSysReport.exe`, в установщике — `Assistant.exe`/`dvssyshelper.dll` (оформление предложений установщика). Их назначение выведено из имён и строк; сами модули не разбирались.

## 9. Ограничения
- Анализ статический; запись/интерфейс на живой системе не наблюдались. Тексты, сообщения и имена настроек подтверждены строками; поведение захвата, горячих клавиш, микрофона, размеров и битрейта подтверждено декомпиляцией (Evidence-ID выше). Полный набор: `evidence/rea-evidence-bundle.json` (18 записей).
- Не разбирались: `mmcore.dll` (граф/кодек), `mdevhelper.dll`, выбор AMD AMF-энкодера, устройство окна выбора области (`capture_dialog.cpp`), редактор снимков в деталях, формат MP4/контейнер на выходе, темп кадров по умолчанию.
