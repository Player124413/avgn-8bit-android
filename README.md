# AVGN 8-bit — Android port · порт для Android

This repository holds two builds of *Angry Video Game Nerd 8-bit*: the original **PS Vita** port and an
**Android** port made from it. Both run the game's own `data.win` through the same free GameMaker runtime,
**Butterscotch**, and neither one ships the game.

В репозитории две сборки игры *Angry Video Game Nerd 8-bit*: исходный порт для **PS Vita** и сделанный на его
основе порт для **Android**. Обе запускают ваш собственный `data.win` через один и тот же свободный
рантайм GameMaker — **Butterscotch**, — и ни одна из них не содержит файлов игры.

* Android: `android/` (Kotlin + Jetpack Compose, NDK/CMake для рантайма), готовый APK — в релизе
  [`latest-build`](https://github.com/Player124413/avgn-8bit-vita/releases/tag/latest-build),
  подробности сборки — [`android/README.md`](android/README.md).
* PS Vita: `AVGN8bit-Vita-1.10.vpk` + `AVGN8bit-Vita-Patcher.bat` (инструкция ниже).

Интерфейс приложения-лаунчера переведён на **английский и русский** и следует языку системы
(the launcher follows the device language: English by default, Russian on a Russian system).

## Which runtime each build uses / какой рантайм используется

| | PS Vita | Android |
|---|---|---|
| Runtime | **Butterscotch** — свободный рантайм GameMaker (AGPL-3.0), форк из Vita-порта | тот же **Butterscotch** (тот же форк), собранный NDK/CMake под `arm64-v8a` и `armeabi-v7a` |
| Where the sources are | `source/runtime-src.tar.gz` внутри VPK | каталог `runtime/` в этом репозитории |
| Graphics API | **vitaGL** — реализация OpenGL (1.1/2.0) поверх GXM (LGPL-3.0), шейдеры транслируются через SceShaccCg | нативный **OpenGL ES 3.0** через EGL (`GameGL.kt`), шейдеры блита и CRT в `assets/shaders` |
| Cutscene video | **SceAvPlayer** (аппаратный декодер, `src/video/vita/`) | видео отключено: `video_play` — заглушка (`src/video/null_video.c`) |
| Sound | **OpenAL** (порт из vitasdk), `src/audio/openal/` | **miniaudio** с бэкендом **OpenSL ES**, `src/audio/miniaudio/` |
| Input | кнопки/стики Vita + сенсорный экран | сенсорные кнопки, D-Pad и стик на экране (редактор раскладки), физический геймпад, клавиатура |
| Game data | `ux0:data/AVGN8bit/data_vita.win`, собранный Windows-патчером на ПК | `data_vita.win` собирается **на телефоне** из вашего `data.win` и `data_vita.patch` из APK (см. `GameDataPatcher.kt`) |
| Licenses | runtime AGPL-3.0 + vitaGL LGPL-3.0 | runtime AGPL-3.0, FreeType, Box2D, miniaudio и др. — см. [`android/NOTICE.md`](android/NOTICE.md) |

Что общего: ядро рантайма, `data.win`-парсер, виртуальная машина GML, рендерер (`src/gl`, `src/gl_common`) и
оптимизации, которые появились ради Vita (кэш текстур, непрозрачные батчи, запекание CRT). Android-порт
переиспользует их: файлы `runtime/src` байт в байт совпадают с исходниками из VPK, отличается только
платформенный хост — `runtime/src/android/main.c` (JNI-мост) и `android/native/CMakeLists.txt`. Отдельный
список изменений и лицензий — в [`android/NOTICE.md`](android/NOTICE.md).

## Android port (порт для Android)

Порт для Android: тот же рантайм, та же копия игры, сенсорное управление с полноценным редактором раскладки,
масштаб 4:3 / широкий экран, CRT-фильтр и логи, которые можно отправить одной кнопкой.

* **APK**: релиз [`latest-build`](https://github.com/Player124413/avgn-8bit-vita/releases/tag/latest-build)
  (обновляется каждой удачной сборкой: `AVGN8bit-release.apk`, `AVGN8bit-debug.apk`), а также артефакты
  запуска workflow «Android APK».
* **Игра** импортируется в приложении папкой или zip-архивом. Можно собрать APK сразу с игрой внутри:
  `./gradlew assembleRelease -PbundleGame=game.zip` или запустить workflow с ссылкой на архив
  (`game_zip_url`).
* **Сборка и первый запуск**: [`android/README.md`](android/README.md) (по-русски; шаги одинаковы для любой
  системы — Android Studio или `./gradlew`).

<!-- -->

## Installing AVGN 8-bit on your Vita

You need: A copy of your own game, a Windows PC, and a hacked Vita with VitaShell, and libshacccg.suprx installed.

On your PC (once)

Extract the AVGN8bit-Vita zip file.

Drop AVGN8bit-Vita-Patcher.bat into your own Windows Steam AVGN game folder.
Double-click the .bat file. If Windows gives you a warning, hit → Run anyway.
Windows will open a shell script. The script will create a Vita-friendly game folder. Wait a while...If you don't have ffmpeg installed on your computer, the script will have to download ffmpeg to convert the cut scenes to a Vita friendly format. Don’t click in the window, just wait.. For me, it took about 30 minutes (I know)... When it’s finsihed, there will be a new folder called "AVGN8bit" in your game folder. The rest of your game files stay untouched.

On your Vita

Open VitaShell, press Select for USB mode, and plug it into the PC.
Copy the AVGN8bit folder into ux0:data/.
Copy AVGN8bit-Vita-1.10.vpk over, and install it from VitaShell.
Start the game. The first launch takes about 25 seconds, so just wait.

AND THEN ENJOY!

If something goes wrong

It crashes right away: you’re missing libshacccg.suprx. Run ShaRKBR33D once, with Wi-Fi on.
“Game files are missing”: the folder has to be exactly ux0:data/AVGN8bit, with data_vita.win right inside it.
The patcher complains about your version: in Steam, verify the game files and run it again.
Saves live in ux0:data/AVGN8bit.
